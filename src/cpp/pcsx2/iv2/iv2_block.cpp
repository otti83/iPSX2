// SPDX-License-Identifier: GPL-3.0+
// P101: iv2 — ブロック構造 + decoder (Phase C: native IR)

#include "PrecompiledHeader.h"
#include "Common.h"
#include "iv2/iv2.h"
#include "iv2/iv2_block.h"
#include "iv2/iv2_tlb_l0.h"   // [P101-14 E1] SMC 検証を L0 inline 読みで行う
#include "R5900OpcodeTables.h"
#include "Memory.h"

namespace iv2 {

alignas(64) Block* g_block_map[BLOCK_HASH_SIZE] = {};
alignas(64) Block g_block_pool[BLOCK_POOL_CAP];
u32 g_block_pool_next = 0;
alignas(64) CachedInst g_inst_pool[INST_POOL_CAP];
u32 g_inst_pool_next = 0;
u16 g_generation = 1;
u32 g_block_pool_cap = BLOCK_POOL_CAP;   // [P101-14 E5] 実効 cap (env で旧 16384 に戻せる)

// [P101-14 E5-probe] 無効化内訳。いずれも cold path でしか触らない。
u64 g_iv2_inval_pool = 0;
u64 g_iv2_inval_clear = 0;
u64 g_iv2_inval_clear_calls = 0;
u64 g_iv2_compiles = 0;
u64 g_iv2_clear_insts = 0;

// [P101-14 E6] コードページ ビットマップ (詳細 = iv2_block.h)
alignas(64) u64 g_code_pages[CODE_PAGE_COUNT / 64] = {};
bool g_ee_clear_range = true;
// [P101-14 R1] compile 時 Nop 消去の **安全 kill-switch** (`iPSX2_EE_NOP_COMPACT=0`)。
// off にすると Nop entry がそのまま残り、run_block は 1 個ずつ dispatch して何もしない
// (= 正しいが遅い)。実行時合体スキャンは復活しないので、厳密な「旧挙動」ではなく
// 「R1 以前 - 合体最適化」に相当する。A/B の対照ではなく退行時の逃げ道として置く。
bool g_ee_nop_compact = true;

// ブロックが跨ぐ全ページを立てる。常に過大近似 (落とすのは InvalidateAll だけ)。
static __attribute__((always_inline)) inline void mark_code_pages(u32 pc_start, u32 pc_end)
{
    const u32 a = pc_start & 0x1FFFFFFFu;
    const u32 b = (pc_end   & 0x1FFFFFFFu);
    // RAM 外 (BIOS ROM / scratchpad / 未マップ) は bitmap の対象外。
    // その領域を消す Clear は下の CodePagesOverlap が保守的に true を返す。
    if (a >= CODE_PAGE_SPAN || b >= CODE_PAGE_SPAN || b < a)
        return;
    for (u32 p = a >> CODE_PAGE_SHIFT; p <= (b >> CODE_PAGE_SHIFT); p++)
        g_code_pages[p >> 6] |= (1ull << (p & 63));
}

bool CodePagesOverlap(u32 addr, u32 bytes)
{
    if (bytes == 0)
        bytes = 4;
    const u32 a = addr & 0x1FFFFFFFu;
    const u64 end = (u64)a + (u64)bytes;
    // RAM 範囲外 / 巨大範囲 / ラップ → 判定不能なので保守的に「コード有り」
    if (end > (u64)CODE_PAGE_SPAN)
        return true;
    const u32 p0 = a >> CODE_PAGE_SHIFT;
    const u32 p1 = (u32)((end - 1) >> CODE_PAGE_SHIFT);
    for (u32 p = p0; p <= p1; p++)
        if (g_code_pages[p >> 6] & (1ull << (p & 63)))
            return true;
    return false;
}

// 0=pc 変更なし、1=ブランチ/jump/syscall/eret 等
static bool is_terminator_flags(u32 flags)
{
    if (flags & IS_BRANCH) return true;
    const u32 brtype = flags & BRANCHTYPE_MASK;
    if (brtype == BRANCHTYPE_SYSCALL) return true;
    if (brtype == BRANCHTYPE_ERET)    return true;
    if (brtype == BRANCHTYPE_REGISTER) return true;
    if (brtype == BRANCHTYPE_BC1)      return true;
    if (brtype == BRANCHTYPE_BC0)      return true;
    return false;
}

// ────────────────────────────────────────────────
// Decoder: raw 32-bit MIPS → iv2::CachedInst
// Native 化できる命令は handler=NULL + op=native。
// それ以外は op=Op_Fallback + handler=opcode.interpret。
// ────────────────────────────────────────────────
void DecodeInst(u32 raw, CachedInst& out)
{
    const R5900::OPCODE& opc = R5900::GetInstruction(raw);
    out.raw = raw;
    out.handler = opc.interpret;
    out.op = Op_Fallback;
    out.dest = 0;
    out.src1 = 0;
    out.src2 = 0;
    out.imm = 0;
    out.cycles = (u16)opc.cycles;   // [iv2 per-inst cycle fix 2026-05-31]
    out.pc_adv = 4;                 // [P101-14 R1] 既定 (compact パスで加算されうる)

    const u32 primary = raw >> 26;
    const u32 rs = (raw >> 21) & 0x1F;
    const u32 rt = (raw >> 16) & 0x1F;
    const u32 rd = (raw >> 11) & 0x1F;
    const u32 sa = (raw >> 6) & 0x1F;
    const u32 funct = raw & 0x3F;
    const s32 imm_se = (s32)(s16)(u16)(raw & 0xFFFF);
    const u32 imm_ze = raw & 0xFFFF;

    // 主要 ALU を native 化
    switch (primary) {
    case 0x00: {  // SPECIAL
        switch (funct) {
        case 0x20: /* ADD */    // 正確には ADD は overflow trap が要るが、BIOS は発火しない想定 → AddU と同じ
        case 0x21: /* ADDU */
            if (rd != 0) {
                out.op = Op_AddU; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr;
            }
            break;
        case 0x22: /* SUB */
        case 0x23: /* SUBU */
            if (rd != 0) {
                out.op = Op_SubU; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr;
            }
            break;
        case 0x24: /* AND */
            if (rd != 0) { out.op = Op_AndR; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x25: /* OR */
            if (rd != 0) { out.op = Op_OrR; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x26: /* XOR */
            if (rd != 0) { out.op = Op_XorR; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x27: /* NOR */
            if (rd != 0) { out.op = Op_NorR; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x2A: /* SLT */
            if (rd != 0) { out.op = Op_SltR; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x2B: /* SLTU */
            if (rd != 0) { out.op = Op_SltRU; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x00: /* SLL */  // SLL $0,$0,0 = nop
            if (rd == 0 && rt == 0 && sa == 0) {
                out.op = Op_Nop; out.handler = nullptr;
            } else if (rd != 0) {
                out.op = Op_Sll; out.dest = rd; out.src1 = 0; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr;
            }
            break;
        case 0x02: /* SRL */
            if (rd != 0) { out.op = Op_Srl; out.dest = rd; out.src1 = 0; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr; }
            break;
        case 0x03: /* SRA */
            if (rd != 0) { out.op = Op_Sra; out.dest = rd; out.src1 = 0; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr; }
            break;
        // Phase L: 64-bit ALU
        case 0x2C: /* DADD  → overflow trap 稀なので DADDU と同じ扱い */
        case 0x2D: /* DADDU */
            if (rd != 0) { out.op = Op_DaddU; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x2E: /* DSUB  */
        case 0x2F: /* DSUBU */
            if (rd != 0) { out.op = Op_DsubU; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x14: /* DSLLV */
            if (rd != 0) { out.op = Op_Dsllv; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x16: /* DSRLV */
            if (rd != 0) { out.op = Op_Dsrlv; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x17: /* DSRAV */
            if (rd != 0) { out.op = Op_Dsrav; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x38: /* DSLL */
            if (rd != 0) { out.op = Op_Dsll; out.dest = rd; out.src1 = 0; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr; }
            break;
        case 0x3A: /* DSRL */
            if (rd != 0) { out.op = Op_Dsrl; out.dest = rd; out.src1 = 0; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr; }
            break;
        case 0x3B: /* DSRA */
            if (rd != 0) { out.op = Op_Dsra; out.dest = rd; out.src1 = 0; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr; }
            break;
        case 0x3C: /* DSLL32 */
            if (rd != 0) { out.op = Op_Dsll32; out.dest = rd; out.src1 = 0; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr; }
            break;
        case 0x3E: /* DSRL32 */
            if (rd != 0) { out.op = Op_Dsrl32; out.dest = rd; out.src1 = 0; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr; }
            break;
        case 0x3F: /* DSRA32 */
            if (rd != 0) { out.op = Op_Dsra32; out.dest = rd; out.src1 = 0; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr; }
            break;
        // Phase N: Mult/Div + HI/LO
        case 0x10: /* MFHI */
            if (rd != 0) { out.op = Op_Mfhi; out.dest = rd; out.handler = nullptr; }
            break;
        case 0x11: /* MTHI */
            out.op = Op_Mthi; out.src1 = rs; out.handler = nullptr;
            break;
        case 0x12: /* MFLO */
            if (rd != 0) { out.op = Op_Mflo; out.dest = rd; out.handler = nullptr; }
            break;
        case 0x13: /* MTLO */
            out.op = Op_Mtlo; out.src1 = rs; out.handler = nullptr;
            break;
        case 0x18: /* MULT */
            out.op = Op_Mult; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr;
            break;
        case 0x19: /* MULTU */
            out.op = Op_MultU; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr;
            break;
        case 0x1A: /* DIV */
            out.op = Op_Div; out.src1 = rs; out.src2 = rt; out.handler = nullptr;
            break;
        case 0x1B: /* DIVU */
            out.op = Op_DivU; out.src1 = rs; out.src2 = rt; out.handler = nullptr;
            break;
        // [Phase W-lite] SYNC — 実装は void R5900::Interpreter::OpcodeImpl::SYNC() {} の empty。
        // native Op_Nop でディスパッチ短絡する。block 終端ではない。
        case 0x0F: /* SYNC */
            out.op = Op_Nop; out.handler = nullptr;
            break;
        // JR, JALR, SYSCALL, ERET 等は Fallback 固定
        }
    } break;

    case 0x08: /* ADDI */
        // [P101-3 Phase N] ADDI を native 化 (Op_Addi)。overflow 時のみ c.handler() 経由で
        // Overflow 例外発火。99.9% は non-overflow なので inline compute + branch で native 高速。
        // handler は DecodeInst 先頭で opc.interpret が既に設定済。
        if (rt != 0) {
            out.op = Op_Addi; out.dest = rt; out.src1 = rs; out.imm = imm_se;
        }
        break;
    case 0x09: /* ADDIU */
        if (rt != 0) {
            out.op = Op_AddIU; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        }
        break;
    case 0x0A: /* SLTI */
        if (rt != 0) { out.op = Op_SltI; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x0B: /* SLTIU */
        if (rt != 0) { out.op = Op_SltIU; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x0C: /* ANDI */
        if (rt != 0) { out.op = Op_AndI; out.dest = rt; out.src1 = rs; out.imm = (s32)imm_ze; out.handler = nullptr; }
        break;
    case 0x0D: /* ORI */
        if (rt != 0) { out.op = Op_OrI; out.dest = rt; out.src1 = rs; out.imm = (s32)imm_ze; out.handler = nullptr; }
        break;
    case 0x0E: /* XORI */
        if (rt != 0) { out.op = Op_XorI; out.dest = rt; out.src1 = rs; out.imm = (s32)imm_ze; out.handler = nullptr; }
        break;
    case 0x0F: /* LUI */
        if (rt != 0) {
            out.op = Op_SetConst; out.dest = rt; out.imm = (s32)((u32)imm_ze << 16); out.handler = nullptr;
        }
        break;
    case 0x18: /* DADDI  */
    case 0x19: /* DADDIU */
        if (rt != 0) {
            out.op = Op_DaddIU; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        }
        break;

    // Load/Store (Phase D). 例外経路 (alignment error 等) が発生したら
    // handler 側の処理を書き換えるが、現状は iv2_tlb_l0 側で vtlb fallback に落とす。
    case 0x20: /* LB */
        if (rt != 0) { out.op = Op_Lb;  out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x24: /* LBU */
        if (rt != 0) { out.op = Op_Lbu; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x21: /* LH */
        if (rt != 0) { out.op = Op_Lh;  out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x25: /* LHU */
        if (rt != 0) { out.op = Op_Lhu; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x23: /* LW */
        if (rt != 0) { out.op = Op_Lw;  out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x27: /* LWU */
        if (rt != 0) { out.op = Op_Lwu; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x37: /* LD */
        if (rt != 0) { out.op = Op_Ld;  out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x28: /* SB */
        out.op = Op_Sb; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        break;
    case 0x29: /* SH */
        out.op = Op_Sh; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        break;
    case 0x2B: /* SW */
        out.op = Op_Sw; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        break;
    case 0x3F: /* SD */
        out.op = Op_Sd; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        break;

    // [P101-3 Phase N] 128-bit load/store native (profile 6.7%)
    case 0x1E: /* LQ  — GPR[rt] = mem128[GPR[rs] + imm] (rt=0 は無効命令扱い) */
        if (rt != 0) { out.op = Op_Lq; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x1F: /* SQ  — mem128[GPR[rs] + imm] = GPR[rt] */
        out.op = Op_Sq; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        break;
    case 0x36: /* LQC2 — VU0.VF[ft] = mem128[GPR[rs] + imm]  (ft=0 許容、VF[0] は固定[0,0,0,1])
                       PCSX2 interp は ft=0 だと何もしない。同挙動に。  */
        if (rt != 0) { out.op = Op_Lqc2; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        else         { out.op = Op_Nop;  out.handler = nullptr; }
        break;
    case 0x3E: /* SQC2 — mem128[GPR[rs] + imm] = VU0.VF[ft] */
        out.op = Op_Sqc2; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        break;

    // [Phase W-lite] PREF — empty function (CACHE 系と同じ扱い)。native nop にする。
    case 0x33: /* PREF */
        out.op = Op_Nop; out.handler = nullptr;
        break;

    // Phase F: FPU (COP1) の一部を native 化
    case 0x31: /* LWC1 */
        out.op = Op_Lwc1; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        break;
    case 0x39: /* SWC1 */
        out.op = Op_Swc1; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        break;

    case 0x1C: /* MMI */
        {
            if (rd == 0) break;
            const u32 mmi_funct = raw & 0x3F;
            const u32 sa_field = (raw >> 6) & 0x1F;
            // Shift immediates (funct 0x34/0x36/0x37/0x3C/0x3E/0x3F) use sa as shift amount
            switch (mmi_funct) {
            case 0x08: /* MMI0 group */
                switch (sa_field) {
                case 0x00: out.op = Op_Paddw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x01: out.op = Op_Psubw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x02: out.op = Op_Pcgtw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x03: out.op = Op_Pmaxw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x04: out.op = Op_Paddh; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x05: out.op = Op_Psubh; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x06: out.op = Op_Pcgth; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x07: out.op = Op_Pmaxh; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x08: out.op = Op_Paddb; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x09: out.op = Op_Psubb; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x0A: out.op = Op_Pcgtb; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x10: out.op = Op_Paddsw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x11: out.op = Op_Psubsw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x12: out.op = Op_Pextlw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x14: out.op = Op_Paddsh; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x15: out.op = Op_Psubsh; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x16: out.op = Op_Pextlh; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x18: out.op = Op_Paddsb; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x19: out.op = Op_Psubsb; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x1A: out.op = Op_Pextlb; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                }
                break;
            case 0x09: /* MMI2 group */
                switch (sa_field) {
                case 0x0E: out.op = Op_Pcpyld; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x12: out.op = Op_Pand; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x13: out.op = Op_Pxor; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                }
                break;
            case 0x28: /* MMI1 group */
                switch (sa_field) {
                case 0x02: out.op = Op_Pceqw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x03: out.op = Op_Pminw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x06: out.op = Op_Pceqh; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x07: out.op = Op_Pminh; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x0A: out.op = Op_Pceqb; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x10: out.op = Op_Padduw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x11: out.op = Op_Psubuw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x12: out.op = Op_Pextuw; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x14: out.op = Op_Padduh; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x15: out.op = Op_Psubuh; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x16: out.op = Op_Pextuh; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x18: out.op = Op_Paddub; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x19: out.op = Op_Psubub; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x1A: out.op = Op_Pextub; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                }
                break;
            case 0x29: /* MMI3 group */
                switch (sa_field) {
                case 0x0E: out.op = Op_Pcpyud; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x12: out.op = Op_Por; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                case 0x13: out.op = Op_Pnor; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; break;
                }
                break;
            // shift immediate group (sa = shift amount, not sub-funct)
            case 0x34: out.op = Op_Psllh; out.dest = rd; out.src2 = rt; out.imm = (s32)sa_field; out.handler = nullptr; break;
            case 0x36: out.op = Op_Psrlh; out.dest = rd; out.src2 = rt; out.imm = (s32)sa_field; out.handler = nullptr; break;
            case 0x37: out.op = Op_Psrah; out.dest = rd; out.src2 = rt; out.imm = (s32)sa_field; out.handler = nullptr; break;
            case 0x3C: out.op = Op_Psllw; out.dest = rd; out.src2 = rt; out.imm = (s32)sa_field; out.handler = nullptr; break;
            case 0x3E: out.op = Op_Psrlw; out.dest = rd; out.src2 = rt; out.imm = (s32)sa_field; out.handler = nullptr; break;
            case 0x3F: out.op = Op_Psraw; out.dest = rd; out.src2 = rt; out.imm = (s32)sa_field; out.handler = nullptr; break;
            }
        }
        break;

    case 0x11: /* COP1 */
        {
            const u32 sub = (raw >> 21) & 0x1F;
            const u32 fs = (raw >> 11) & 0x1F;
            const u32 fd = (raw >> 6) & 0x1F;
            const u32 funct = raw & 0x3F;
            switch (sub) {
            case 0x00: /* MFC1 */
                if (rt != 0) { out.op = Op_Mfc1; out.dest = rt; out.src1 = fs; out.handler = nullptr; }
                break;
            case 0x02: /* CFC1 */
                if (rt != 0) { out.op = Op_Cfc1; out.dest = rt; out.src1 = fs; out.handler = nullptr; }
                break;
            case 0x04: /* MTC1 */
                out.op = Op_Mtc1; out.dest = fs; out.src1 = rt; out.handler = nullptr;
                break;
            case 0x06: /* CTC1 */
                out.op = Op_Ctc1; out.dest = fs; out.src1 = rt; out.handler = nullptr;
                break;
            case 0x10: /* COP1_S */
                {
                    const u32 ft = (raw >> 16) & 0x1F;  // Ft field
                    switch (funct) {
                    case 0x00: /* ADD.S */
                        out.op = Op_AddS; out.dest = fd; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x01: /* SUB.S */
                        out.op = Op_SubS; out.dest = fd; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x02: /* MUL.S */
                        out.op = Op_MulS; out.dest = fd; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x03: /* DIV.S */
                        out.op = Op_DivS; out.dest = fd; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x04: /* SQRT.S */
                        out.op = Op_SqrtS; out.dest = fd; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x05: /* ABS.S */
                        out.op = Op_AbsS; out.dest = fd; out.src1 = fs; out.handler = nullptr;
                        break;
                    case 0x06: /* MOV.S */
                        out.op = Op_MovS; out.dest = fd; out.src1 = fs; out.handler = nullptr;
                        break;
                    case 0x07: /* NEG.S */
                        out.op = Op_NegS; out.dest = fd; out.src1 = fs; out.handler = nullptr;
                        break;
                    case 0x16: /* RSQRT.S */
                        out.op = Op_RsqrtS; out.dest = fd; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x18: /* ADDA.S */
                        out.op = Op_AddaS; out.dest = 0; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x19: /* SUBA.S */
                        out.op = Op_SubaS; out.dest = 0; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x1A: /* MULA.S */
                        out.op = Op_MulaS; out.dest = 0; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x1C: /* MADD.S */
                        out.op = Op_MaddS; out.dest = fd; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x1D: /* MSUB.S */
                        out.op = Op_MsubS; out.dest = fd; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x1E: /* MADDA.S */
                        out.op = Op_MaddaS; out.dest = 0; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x1F: /* MSUBA.S */
                        out.op = Op_MsubaS; out.dest = 0; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x24: /* CVT.W */
                        out.op = Op_CvtW; out.dest = fd; out.src1 = fs; out.handler = nullptr;
                        break;
                    case 0x28: /* MAX.S */
                        out.op = Op_MaxS; out.dest = fd; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x29: /* MIN.S */
                        out.op = Op_MinS; out.dest = fd; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x30: /* C.F */
                        out.op = Op_CfS; out.handler = nullptr;
                        break;
                    case 0x32: /* C.EQ */
                        out.op = Op_CeqS; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x34: /* C.LT */
                        out.op = Op_CltS; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    case 0x36: /* C.LE */
                        out.op = Op_CleS; out.src1 = fs; out.src2 = ft; out.handler = nullptr;
                        break;
                    }
                }
                break;
            case 0x14: /* COP1_W: CVT.S */
                if ((raw & 0x3F) == 0x20) {
                    out.op = Op_CvtS; out.dest = fd; out.src1 = fs; out.handler = nullptr;
                }
                break;
            // case 0x08: BC1 族 は branch 処理ルートで扱う (is_branch detected by flags)
            // CVT.S/CVT.W: Fallback のまま
            }
        }
        break;

    // ─── P101-4: COP2 (VU0 macro mode) — rs>=0x10 SPECIAL1 系 ────
    // funct<0x3C = COP2SPECIAL1 (FMAC、ABS、NOP 等)。handler に直接 funct 専用関数を入れて
    // fallback path のコストを削減。native path は run_block の Op_Vu0Macro handler 内で
    // iv2::vu_ops::UpperDispatch(&VU0) を試行。
    // QMFC2/QMTC2/CFC2/CTC2/MTC2/MFC2/BC2 (rs < 0x10) は従来通り Fallback (複雑な semantics)。
    // COP2_SPECIAL2 (funct>=0x3C) も Fallback (sub-dispatch 必要)。
    case 0x12: {  // COP2
        // funct 分類 (Int_COP2SPECIAL1PrintTable):
        //   0x00-0x2F: FMAC (VADD/VSUB/VMUL/VMADD/VMSUB/VMAX/VMINI + bc/i/q variants, VOPMSUB)
        //     → Upper NEON native 流用可 (UpperDispatch(&VU0))
        //   0x30-0x35: VI 整数 op (VIADD/VISUB/VIADDI/VIAND/VIOR)
        //     → Upper 対象外、Fallback 維持 (interp VIADD は cheap なので利得小)
        //   0x38-0x39: VCALLMS/VCALLMSR (VU0 microcode invoke)
        //     → Upper NEON は絶対に使えない (funct=0x38 は VU1 Upper では sub-table)、
        //       誤 dispatch すると microcode 起動されずゲーム状態崩壊。Fallback 必須。
        //   0x3C-0x3F: SPECIAL2 (別 dispatch)
        //     → Fallback 維持
        if (rs >= 0x10 && funct < 0x30) {
            // UpperDispatchNative が false を返す funct は decode 時に Op_Fallback へ
            // 直行して、Op_Vu0Macro 経路の UpperDispatch 試行 + 二重 c.handler() を回避。
            // 非対応: 0x22 ADDi (TriAce hack), 0x26 SUBi (対称), 0x2E OPMSUB (3-comp cross)
            const bool upper_unhandled = (funct == 0x22 || funct == 0x26 || funct == 0x2E);
            if (!upper_unhandled) {
                out.op = Op_Vu0Macro;
                out.handler = ::Int_COP2SPECIAL1PrintTable[funct];
            }
        }
        break;
    }
    }

    // $0 への書き込みは NOP に置換 (上の条件で dest=0 の場合 handler 維持の Fallback)
    // でも Fallback 命令も $0 書き込みを自分で処理する (handler 内で r0 上書きはしない)
}

void InvalidateAll()
{
    g_generation++;
    if (g_generation == 0) g_generation = 1;
    g_block_pool_next = 0;
    g_inst_pool_next = 0;
    // [P101-14 E6] 全ブロックを捨てたのでコードページも空に戻す (ビットが落ちる唯一の場所)
    std::memset(g_code_pages, 0, sizeof(g_code_pages));
}

// Phase E: branch を 3-op パターンに展開する compile ヘルパー。
// branch命令 に遭遇したら [BCheck/Jump] → [BDS 通常デコード] → [BFinalize] を emit。
// 戻り値: pc 前進量 (バイト、8 = branch + BDS 消費)
//
// 対応していない分岐 (BEQL 系 likely / SYSCALL / ERET / COP1 BC1F / COP0 BC0 等) は
// Fallback のまま通常命令として扱い、呼出側が自動で is_terminator により block 打切。
//
// 返す pc 増分が 0 の場合は native 化不可を意味する。呼出側は通常デコードで扱う。
static u32 TryCompileBranch(u32 raw, u32 branch_pc, CachedInst*& out,
                            u32& inst_count, u32& cycles_sum)
{
    const u32 primary = raw >> 26;
    const u32 rs = (raw >> 21) & 0x1F;
    const u32 rt = (raw >> 16) & 0x1F;
    const s32 off = (s32)(s16)(u16)(raw & 0xFFFF);
    // target_pc 計算: branch 命令の pc は branch_pc。
    // 分岐先 = branch_pc + 4 + off*4
    const u32 target_pc = branch_pc + 4 + (u32)(off * 4);

    // 判定用ヘルパー: BCheck / Jump を emit + BDS + BFinalize
    auto emit_triplet = [&](Op check_op, u8 src1, u8 src2, u32 tpc, u8 link_reg) {
        CachedInst& check = *out++;
        check.raw = raw;
        check.handler = nullptr;
        check.op = check_op;
        check.dest = link_reg;
        check.src1 = src1;
        check.src2 = src2;
        check.imm = (s32)tpc;
        check.cycles = (u16)R5900::GetInstruction(raw).cycles;  // [iv2 per-inst cycle fix] 分岐命令分
        check.pc_adv = 4;                                      // [P101-14 R1]
        ++inst_count;
        // BDS
        u32 bds_raw = memRead32(branch_pc + 4);
        const R5900::OPCODE& bds_opc = R5900::GetInstruction(bds_raw);
        CachedInst& bds = *out++;
        DecodeInst(bds_raw, bds);   // bds.cycles は DecodeInst 内で設定済
        cycles_sum += bds_opc.cycles;
        ++inst_count;
        // BFinalize (合成命令、cycle なし)
        CachedInst& fin = *out++;
        fin.raw = 0;
        fin.handler = nullptr;
        fin.op = Op_BFinalize;
        fin.dest = 0;
        fin.src1 = 0;
        fin.src2 = 0;
        fin.imm = (s32)(branch_pc + 8);   // fall-through PC
        fin.cycles = 0;                    // [iv2 per-inst cycle fix] 合成命令は 0
        fin.pc_adv = 4;                    // [P101-14 R1] (BFinalize は直後に pc を上書きするので値は無関係)
        ++inst_count;
    };

    switch (primary) {
    case 2: /* J */
        emit_triplet(Op_JumpConst, 0, 0, ((raw & 0x03FFFFFFu) << 2) | (branch_pc & 0xF0000000u), /*link=*/0);
        return 8;
    case 3: /* JAL */
        emit_triplet(Op_JumpConst, 0, 0, ((raw & 0x03FFFFFFu) << 2) | (branch_pc & 0xF0000000u), /*link=*/31);
        return 8;
    case 4: /* BEQ */
        emit_triplet(Op_BCheckEq, rs, rt, target_pc, 0);
        return 8;
    case 5: /* BNE */
        emit_triplet(Op_BCheckNe, rs, rt, target_pc, 0);
        return 8;
    case 6: /* BLEZ */
        emit_triplet(Op_BCheckLez, rs, 0, target_pc, 0);
        return 8;
    case 7: /* BGTZ */
        emit_triplet(Op_BCheckGtz, rs, 0, target_pc, 0);
        return 8;
    case 0: /* SPECIAL: JR/JALR */
        {
            const u32 funct = raw & 0x3F;
            const u32 rd = (raw >> 11) & 0x1F;
            if (funct == 0x08) { /* JR */
                emit_triplet(Op_JumpReg, rs, 0, 0, 0);
                return 8;
            }
            if (funct == 0x09) { /* JALR */
                emit_triplet(Op_JumpReg, rs, 0, 0, (u8)rd);
                return 8;
            }
        }
        break;
    case 1: /* REGIMM: BLTZ/BGEZ/BLTZAL/BGEZAL */
        {
            const u32 sub = (raw >> 16) & 0x1F;
            switch (sub) {
            case 0x00: /* BLTZ */
                emit_triplet(Op_BCheckLtz, rs, 0, target_pc, 0);
                return 8;
            case 0x01: /* BGEZ */
                emit_triplet(Op_BCheckGez, rs, 0, target_pc, 0);
                return 8;
            case 0x10: /* BLTZAL */
                emit_triplet(Op_BCheckLtzLink, rs, 0, target_pc, 31);
                return 8;
            case 0x11: /* BGEZAL */
                emit_triplet(Op_BCheckGezLink, rs, 0, target_pc, 31);
                return 8;
            // BLTZL/BGEZL/BLTZALL/BGEZALL: likely, Fallback のまま
            }
        }
        break;
    // Phase F: COP1 BC1F/BC1T (non-likely). likely (BC1FL/BC1TL) は Fallback のまま
    case 0x11: /* COP1 */
        {
            const u32 cop1_sub = (raw >> 21) & 0x1F;
            if (cop1_sub == 0x08) {  // BC1
                const u32 bc_nd_tf = (raw >> 16) & 0x3;
                if (bc_nd_tf == 0) {  // BC1F
                    emit_triplet(Op_BC1F, 0, 0, target_pc, 0);
                    return 8;
                }
                if (bc_nd_tf == 1) {  // BC1T
                    emit_triplet(Op_BC1T, 0, 0, target_pc, 0);
                    return 8;
                }
                // BC1FL (2) / BC1TL (3): likely, Fallback へ
            }
        }
        break;
    }
    // likely 系 (BEQL/BNEL/BLEZL/BGTZL) および他の非対応分岐は Fallback へ
    return 0;
}

Block* CompileBlock(u32 entry_pc)
{
    ++g_iv2_compiles;
    if (g_block_pool_next >= g_block_pool_cap ||
        g_inst_pool_next + MAX_BLOCK_INSTS >= INST_POOL_CAP) {
        ++g_iv2_inval_pool;
        InvalidateAll();
    }

    Block* blk = &g_block_pool[g_block_pool_next++];
    blk->entry_pc = entry_pc;
    blk->inst_offset = g_inst_pool_next;
    blk->generation = g_generation;
    blk->inst_count = 0;
    blk->guest_cycles_sum = 0;
    blk->link_taken = nullptr;
    blk->link_not_taken = nullptr;

    u32 pc = entry_pc;
    CachedInst* out = &g_inst_pool[g_inst_pool_next];
    u32 inst_count = 0;

    for (u32 i = 0; i < MAX_BLOCK_INSTS - 2; i++) {
        u32 raw = memRead32(pc);
        const R5900::OPCODE& opc = R5900::GetInstruction(raw);
        const bool is_branch = is_terminator_flags(opc.flags);

        if (is_branch) {
            // native 分岐化を試みる
            u32 advance = TryCompileBranch(raw, pc, out, inst_count, blk->guest_cycles_sum);
            if (advance > 0) {
                // branch の分 + BDS の分を cycles に加算 (BDS は emit_triplet で既に入れた)
                blk->guest_cycles_sum += opc.cycles;
                pc += advance;
                break;
            }
            // Fallback 分岐 (likely 系 / syscall / eret): 通常 decode で emit
            DecodeInst(raw, *out);
            blk->guest_cycles_sum += opc.cycles;
            ++out;
            ++inst_count;
            pc += 4;
            break;
        }

        // 非ブランチ命令: 通常 decode
        DecodeInst(raw, *out);
        blk->guest_cycles_sum += opc.cycles;
        ++out;
        ++inst_count;
        pc += 4;

        // [iv2 fix 2026-05-30] ★ MTC0 (COP0 write) で block 終端 ★。
        // run_block の cycle_mul = 2 - (CP0.Config>>18 & 1) は block 先頭で 1 回読むが、
        // intCpu は命令毎に読む (Interpreter.cpp:219)。BIOS が boot 中 mtc0 で Config を
        // 変えると、同 block 内で変更後の命令を iv2 は古い mul で過大計上 → EE cycle 乖離 →
        // IOP catchup の EEsCycle がズレ → GTA VC freeze。mtc0 後に block を切れば block 内
        // Config 一定となり block-level mul が intCpu の per-inst と bit-exact 一致。
        // mtc0 = COP0(0x10) + MT(rs=0x04)。mtc0 自身は古い Config で計上 (= intCpu と同じ)。
        if ((raw >> 26) == 0x10 && ((raw >> 21) & 0x1F) == 0x04)
            break;
    }

    // ─── [P101-14 R1 2026-08-06] compile 時 Nop 消去 ───────────────────────
    // 動機 (逆アセン実測): dispatch は 1 entry あたり **26 命令の prologue** を必ず通り、
    // Op_Nop はそこに更に 17 命令の合体スキャンを足して「何もしない」。
    // Nop を compile 時に消し、その pc 前進量と cycle を **直後の非 Nop entry へ前方マージ**
    // すれば、dispatch 回数そのものが減る。
    //
    // 等価性:
    //   ・pc — 旧: Nop が pc+=4 してから後続 entry が pc+=4。新: 後続 entry が pc+=4*(1+消した数)。
    //     後続命令の実行時点で cpuRegs.pc は同一。消した Nop は副作用ゼロなので観測差なし。
    //   ・cycle — cpuBlockCycles への加算は「後続命令の実行前」である点が旧新で同じ。
    //     ただし乗数が block 先頭読み (旧: 合体分) から per-inst 読み (新) に変わるため、
    //     **block 内で CP0.Config が不変**であることに依存する。これは MTC0 が block を
    //     強制終端する (:735) ことで保証されている (同 fix の comment 参照)。
    //   ・SMC 検証 (block_content_valid) は entry 0 の raw を entry_pc の生命令と比較するので
    //     **entry 0 は絶対に消さない**。
    //   ・末尾側の Nop (後続の非 Nop が無い) も消さない。前方マージ先が無く、
    //     直前 entry に足すと pc が「実行前」に進みすぎるため。
    if (g_ee_nop_compact && inst_count >= 2) {
        CachedInst* p = &g_inst_pool[blk->inst_offset];
        u32 w = 1;                       // 書き込み位置 (entry 0 は常に残す)
        for (u32 r = 1; r < inst_count; r++) {
            if (p[r].op == Op_Nop) {
                // 後続に非 Nop があるか (無ければ末尾 Nop = 残す)
                u32 s = r + 1;
                while (s < inst_count && p[s].op == Op_Nop) s++;
                if (s < inst_count) {
                    p[s].pc_adv = (u8)(p[s].pc_adv + p[r].pc_adv);
                    p[s].cycles = (u16)(p[s].cycles + p[r].cycles);
                    continue;            // この Nop を捨てる
                }
            }
            if (w != r) p[w] = p[r];
            w++;
        }
        inst_count = w;
    }

    blk->inst_count = (u16)inst_count;
    blk->end_pc = pc;
    g_inst_pool_next += inst_count;

    // [P101-3R I-lite REMOVED] Block-level const propagation was implemented
    // and measured on macOS — net regression (-1.2 to -3.3%) on VU1-heavy
    // scenes because EE time is only ~28% of wall in such scenes and the
    // per-block scan cost exceeded the dispatch savings. Removed for code
    // clarity. Revisit if EE becomes the dominant hot path on some title.
#if 0  // former const-prop pass kept for reference; compile-excluded
    if (inst_count >= 1) {
        CachedInst* p = &g_inst_pool[blk->inst_offset];
        struct CVal { bool k; u64 v; };
        CVal cst[32]; for (int i=0;i<32;i++){cst[i].k=false; cst[i].v=0;} cst[0] = {true, 0};
        auto inv = [&](u8 r){ if (r) cst[r].k = false; };
        auto inv_all = [&](){ for (int i=1;i<32;i++) cst[i].k=false; };
        auto fits_s32 = [](u64 v) -> bool { return (u64)(s64)(s32)(u32)v == v; };

        for (u32 i = 0; i < inst_count; i++) {
            CachedInst& c = p[i];
            switch (c.op) {
            case Op_Nop:
                break;
            case Op_SetConst:
                if (c.dest) cst[c.dest] = {true, (u64)(s64)(s32)c.imm};
                break;

            case Op_AddIU:
                if (cst[c.src1].k) {
                    u64 v = (u64)(s64)(s32)((u32)cst[c.src1].v + (u32)c.imm);
                    c.op = Op_SetConst; c.imm = (s32)v; c.src1 = 0;
                    if (c.dest) cst[c.dest] = {true, v}; else c.op = Op_Nop;
                } else inv(c.dest);
                break;

            case Op_OrI: case Op_AndI: case Op_XorI: {
                if (cst[c.src1].k) {
                    u64 src = cst[c.src1].v;
                    u64 imm = (u32)c.imm;  // zero-ext u16 in low 16
                    u64 v = (c.op == Op_OrI) ? (src | imm)
                          : (c.op == Op_AndI) ? (src & imm)
                          : (src ^ imm);
                    if (fits_s32(v)) {
                        c.op = Op_SetConst; c.imm = (s32)v; c.src1 = 0;
                        if (c.dest) cst[c.dest] = {true, v}; else c.op = Op_Nop;
                    } else inv(c.dest);
                } else inv(c.dest);
                break;
            }

            case Op_SltI:
                if (cst[c.src1].k) {
                    u64 v = ((s64)cst[c.src1].v < (s64)c.imm) ? 1 : 0;
                    c.op = Op_SetConst; c.imm = (s32)v; c.src1 = 0;
                    if (c.dest) cst[c.dest] = {true, v}; else c.op = Op_Nop;
                } else inv(c.dest);
                break;
            case Op_SltIU:
                if (cst[c.src1].k) {
                    u64 v = (cst[c.src1].v < (u64)(s64)c.imm) ? 1 : 0;
                    c.op = Op_SetConst; c.imm = (s32)v; c.src1 = 0;
                    if (c.dest) cst[c.dest] = {true, v}; else c.op = Op_Nop;
                } else inv(c.dest);
                break;

            case Op_AddU: case Op_SubU: {
                bool k1 = cst[c.src1].k, k2 = cst[c.src2].k;
                if (k1 && k2) {
                    u32 a32 = (u32)cst[c.src1].v, b32 = (u32)cst[c.src2].v;
                    u32 r = (c.op == Op_AddU) ? (a32 + b32) : (a32 - b32);
                    u64 v = (u64)(s64)(s32)r;
                    c.op = Op_SetConst; c.imm = (s32)r; c.src1=0; c.src2=0;
                    if (c.dest) cst[c.dest] = {true, v}; else c.op = Op_Nop;
                } else if (c.op == Op_AddU && k1 && fits_s32(cst[c.src1].v)) {
                    c.op = Op_AddIU; c.imm = (s32)cst[c.src1].v; c.src1 = c.src2; c.src2 = 0;
                    inv(c.dest);
                } else if (k2 && fits_s32(cst[c.src2].v)) {
                    // AddU: commutative so also convert when src2 const
                    // SubU: can convert to AddIU with -imm if -imm fits s32
                    if (c.op == Op_AddU) {
                        c.op = Op_AddIU; c.imm = (s32)cst[c.src2].v; c.src2 = 0;
                        inv(c.dest);
                    } else {
                        s64 neg = -(s64)(s32)cst[c.src2].v;
                        if (neg == (s64)(s32)neg) {
                            c.op = Op_AddIU; c.imm = (s32)neg; c.src2 = 0;
                        }
                        inv(c.dest);
                    }
                } else inv(c.dest);
                break;
            }

            case Op_AndR: case Op_OrR: case Op_XorR: case Op_NorR: {
                bool k1 = cst[c.src1].k, k2 = cst[c.src2].k;
                if (k1 && k2) {
                    u64 a = cst[c.src1].v, b = cst[c.src2].v;
                    u64 v = (c.op == Op_AndR) ? (a & b)
                          : (c.op == Op_OrR)  ? (a | b)
                          : (c.op == Op_XorR) ? (a ^ b)
                          : ~(a | b);
                    if (fits_s32(v)) {
                        c.op = Op_SetConst; c.imm = (s32)v; c.src1=0; c.src2=0;
                        if (c.dest) cst[c.dest] = {true, v}; else c.op = Op_Nop;
                    } else inv(c.dest);
                } else inv(c.dest);
                break;
            }

            case Op_SltR: {
                bool k1 = cst[c.src1].k, k2 = cst[c.src2].k;
                if (k1 && k2) {
                    u64 v = ((s64)cst[c.src1].v < (s64)cst[c.src2].v) ? 1 : 0;
                    c.op = Op_SetConst; c.imm = (s32)v; c.src1=0; c.src2=0;
                    if (c.dest) cst[c.dest] = {true, v}; else c.op = Op_Nop;
                } else inv(c.dest);
                break;
            }
            case Op_SltRU: {
                bool k1 = cst[c.src1].k, k2 = cst[c.src2].k;
                if (k1 && k2) {
                    u64 v = (cst[c.src1].v < cst[c.src2].v) ? 1 : 0;
                    c.op = Op_SetConst; c.imm = (s32)v; c.src1=0; c.src2=0;
                    if (c.dest) cst[c.dest] = {true, v}; else c.op = Op_Nop;
                } else inv(c.dest);
                break;
            }

            case Op_Sll: case Op_Srl: case Op_Sra:
                // reads src2, writes dest, imm=sa
                if (cst[c.src2].k) {
                    u32 src = (u32)cst[c.src2].v;
                    u32 sa = (u32)c.imm & 0x1F;
                    u32 r;
                    switch (c.op) {
                    case Op_Sll: r = src << sa; break;
                    case Op_Srl: r = src >> sa; break;
                    default:     r = (u32)((s32)src >> sa); break;
                    }
                    u64 v = (u64)(s64)(s32)r;
                    c.op = Op_SetConst; c.imm = (s32)r; c.src2 = 0;
                    if (c.dest) cst[c.dest] = {true, v}; else c.op = Op_Nop;
                } else inv(c.dest);
                break;

            // Memory loads: GPR write with runtime-dependent value → invalidate
            case Op_Lb: case Op_Lbu: case Op_Lh: case Op_Lhu:
            case Op_Lw: case Op_Lwu: case Op_Ld:
            case Op_Lq:
                inv(c.dest);
                break;

            // Memory stores / branches / FPU float / VU0 macro: no GPR write
            // (LQC2 は VU0.VF[dest] に書くので GPR は触らない。dest は VU 側で不変)
            case Op_Sb: case Op_Sh: case Op_Sw: case Op_Sd:
            case Op_Sq:
            case Op_Lqc2: case Op_Sqc2:
            case Op_BCheckEq: case Op_BCheckNe:
            case Op_BCheckLez: case Op_BCheckGtz:
            case Op_BCheckLtz: case Op_BCheckGez:
            case Op_BFinalize:
            case Op_Mtc1: case Op_Ctc1:
            case Op_MovS: case Op_AbsS: case Op_NegS:
            case Op_AddS: case Op_SubS: case Op_MulS: case Op_DivS:
            case Op_MaddS: case Op_MsubS:
            case Op_AddaS: case Op_SubaS: case Op_MulaS:
            case Op_MaddaS: case Op_MsubaS:
            case Op_MaxS: case Op_MinS: case Op_SqrtS: case Op_RsqrtS:
            case Op_CvtS: case Op_CvtW:
            case Op_CeqS: case Op_CltS: case Op_CleS: case Op_CfS:
            case Op_Lwc1: case Op_Swc1:
            case Op_BC1F: case Op_BC1T:
            case Op_Vu0Macro:
                break;

            // Link-writing branches
            case Op_BCheckLtzLink: case Op_BCheckGezLink:
                cst[31].k = false; break;
            case Op_JumpConst:
                if (c.dest) inv(c.dest); break;
            case Op_JumpReg:
                if (c.dest) inv(c.dest); break;

            // GPR-writing ops (not yet const-folded) → invalidate
            case Op_Addi:
            case Op_Mfc1: case Op_Cfc1:
            case Op_Mfhi: case Op_Mflo:
            case Op_DaddU: case Op_DsubU: case Op_DaddIU:
            case Op_Dsll: case Op_Dsrl: case Op_Dsra:
            case Op_Dsll32: case Op_Dsrl32: case Op_Dsra32:
            case Op_Dsllv: case Op_Dsrlv: case Op_Dsrav:
            case Op_Mult: case Op_MultU: case Op_Div: case Op_DivU:
            case Op_Pand: case Op_Por: case Op_Pxor: case Op_Pnor:
            case Op_Paddb: case Op_Paddh: case Op_Paddw:
            case Op_Psubb: case Op_Psubh: case Op_Psubw:
            case Op_Pceqb: case Op_Pceqh: case Op_Pceqw:
            case Op_Pcgtb: case Op_Pcgth: case Op_Pcgtw:
            case Op_Pmaxh: case Op_Pmaxw: case Op_Pminh: case Op_Pminw:
            case Op_Psllh: case Op_Psrlh: case Op_Psrah:
            case Op_Psllw: case Op_Psrlw: case Op_Psraw:
            case Op_Paddsb: case Op_Paddsh: case Op_Paddsw:
            case Op_Psubsb: case Op_Psubsh: case Op_Psubsw:
            case Op_Paddub: case Op_Padduh: case Op_Padduw:
            case Op_Psubub: case Op_Psubuh: case Op_Psubuw:
            case Op_Pextlb: case Op_Pextlh: case Op_Pextlw:
            case Op_Pextub: case Op_Pextuh: case Op_Pextuw:
            case Op_Pcpyld: case Op_Pcpyud:
                inv(c.dest);
                break;

            // Mthi/Mtlo: read GPR, write HI/LO (not tracked)
            case Op_Mthi: case Op_Mtlo:
                break;

            case Op_Fallback:
            default:
                // 未知副作用 (syscall/eret/COP0 write 等) → 全 GPR invalidate
                inv_all();
                break;
            }
        }
    }
#endif  // former const-prop

    // [P101-14 E6] このブロックが占めるコードページを立てる (Clear の範囲判定用)
    mark_code_pages(entry_pc, blk->end_pc);

    u32 idx = block_hash(entry_pc);
    g_block_map[idx] = blk;
    return blk;
}

// [iv2 SMC fix 2026-05-31] cached block が現メモリと一致するか (先頭命令で検証)。
// iv2 は compile 時 1 回しか memRead32 せず実行時 再 fetch しないため、store/DMA で
// コード領域が書換わっても古い block を実行し続ける (probe 実証: 0x81fe0 syscall stub が
// NOP=0 でコンパイル → 実コード 0x2402006c 上書き後も stale → cycle drift + 誤動作 → freeze)。
// intCpu は毎命令 memRead32 で常に最新。これに合わせ entry 命令の content を検証する。
// コードは RAM (物理 < 32MB、KUSEG/KSEG0) か BIOS ROM のみで実行され、ROM は不変なので
// RAM 領域のみ検査 (性能影響を最小化)。
// [P101-14 E1 2026-08-06] SMC 検証の読みを vtlb_memRead<u32> (out-of-line, vmap 引き +
// handler 判定) から iv2_read32 (L0 primary hit で inline 数命令) へ。
// **ブロック dispatch ごとに 1 回**走るため leaf profile の vtlb_memRead<u32> 1.0% の主因。
// 値は同一 (どちらも同じ vmap を引く。L0 は vmap の キャッシュで、handler ページは
// install を拒否して vtlb_memRead に落ちる)。ここは RAM 領域限定なので handler は来ない。
static __attribute__((always_inline)) inline bool block_content_valid(const Block* blk, u32 pc)
{
    if ((pc & 0x1FFFFFFFu) >= 0x02000000u)
        return true;   // BIOS ROM 等の不変領域は generation 信頼で十分
    return g_inst_pool[blk->inst_offset].raw == iv2_read32(pc);
}

Block* GetOrCompileBlock(u32 pc)
{
    u32 idx = block_hash(pc);
    Block* blk = g_block_map[idx];
    if (__builtin_expect(blk != nullptr
            && blk->entry_pc == pc
            && blk->generation == g_generation
            && block_content_valid(blk, pc), 1)) {
        return blk;
    }
    return CompileBlock(pc);
}

} // namespace iv2
