// SPDX-License-Identifier: GPL-3.0+
// P101: iv2 — ブロック構造とキャッシュ (Phase C: native IR opcode 拡張)

#pragma once

#include "R5900OpcodeTables.h"

namespace iv2 {

using FallbackFn = void(*)();

// 内部 IR opcode
// Fallback 以外は native 実装で、cpuRegs.code 設定を省ける。
enum Op : u8 {
    Op_Fallback = 0,   // handler 経由 (互換性保証)
    Op_Nop,
    Op_SetConst,       // GPR[dest] = (s64)(s32)imm32            (LUI: imm32 = imm16<<16)
    Op_AddIU,          // GPR[dest] = (s64)(s32)((u32)GPR[src1].UL[0] + imm)     (imm = sign-ext s16)
    Op_OrI,            // GPR[dest] = GPR[src1].UD[0] | (u32)imm  (imm = zero-ext u16)
    Op_AndI,           // GPR[dest] = GPR[src1].UD[0] & (u32)imm
    Op_XorI,           // GPR[dest] = GPR[src1].UD[0] ^ (u32)imm
    Op_SltI,           // GPR[dest] = (s64)GPR[src1].SD[0] < (s64)imm ? 1 : 0
    Op_SltIU,          // GPR[dest] = (u64)GPR[src1].UD[0] < (u64)(s64)imm ? 1 : 0
    Op_AddU,           // GPR[dest] = (s64)(s32)((u32)GPR[src1].UL[0] + (u32)GPR[src2].UL[0])
    Op_SubU,           // 同 sub
    Op_AndR, Op_OrR, Op_XorR, Op_NorR,   // 64-bit
    Op_SltR,           // (s64)GPR[src1].SD[0] < (s64)GPR[src2].SD[0]
    Op_SltRU,
    Op_Sll,            // GPR[dest] = (s64)(s32)((u32)GPR[src2].UL[0] << imm)
    Op_Srl,
    Op_Sra,

    // ─── Phase D: Load/Store ──────────────────────
    // 共通: addr = (u32)GPR[src1].UL[0] + (s32)imm
    Op_Lb,             // GPR[dest] = (s64)(s8) *addr
    Op_Lbu,            // GPR[dest] = (u64)(u8) *addr
    Op_Lh,             // GPR[dest] = (s64)(s16)*addr  (addr & 1 → Fallback)
    Op_Lhu,            // GPR[dest] = (u64)(u16)*addr
    Op_Lw,             // GPR[dest] = (s64)(s32)*addr  (addr & 3 → Fallback)
    Op_Lwu,            // GPR[dest] = (u64)(u32)*addr
    Op_Ld,             // GPR[dest].UD[0] = *addr      (addr & 7 → Fallback)
    Op_Sb, Op_Sh, Op_Sw, Op_Sd,

    // ─── Phase E: Branch/Jump ──────────────────────
    // ブロック末尾のみに配置される。
    // 3-op パターン: [BCheck/Jump] → [BDS] → [BFinalize]
    // BCheck は s_branch_taken, s_branch_target を設定
    // BDS は通常の IR 命令
    // BFinalize は taken なら iv2BranchFinalize_Taken、not-taken なら event test
    Op_BCheckEq,        // src1=rs src2=rt imm=target_pc
    Op_BCheckNe,
    Op_BCheckLez,       // src1=rs imm=target_pc
    Op_BCheckGtz,
    Op_BCheckLtz,
    Op_BCheckGez,
    Op_BCheckLtzLink,   // BLTZAL: link $31 = pc+8 then check Ltz
    Op_BCheckGezLink,   // BGEZAL
    Op_JumpConst,       // dest=link_reg (0 or 31), imm=target_pc. Unconditional.
    Op_JumpReg,         // dest=link_reg, src1=rs (target register). JR (dest=0) / JALR (dest=rd).
    Op_BFinalize,       // imm=fall_through_pc

    // ─── Phase F: FPU (COP1) native ─────────────────
    // dest = rt (GPR) or fd/fs/ft (FPR) 番号 — 文脈で判断
    Op_Mfc1,            // GPR[dest] = sign_ext(fpuRegs.fpr[src1].SL)  dest=rt, src1=fs
    Op_Cfc1,            // GPR[dest] = fpuRegs.fprc[src1] (特殊処理)   dest=rt, src1=fs
    Op_Mtc1,            // fpuRegs.fpr[dest].UL = GPR[src1].UL[0]       dest=fs, src1=rt
    Op_Ctc1,            // fpuRegs.fprc[dest] = GPR[src1].UL[0]         dest=fs, src1=rt
    Op_MovS,            // fpuRegs.fpr[dest].UL = fpuRegs.fpr[src1].UL  dest=fd, src1=fs
    Op_AbsS,            // fpuRegs.fpr[dest].UL = fpuRegs.fpr[src1].UL & 0x7FFFFFFF
    Op_NegS,            // fpuRegs.fpr[dest].UL = fpuRegs.fpr[src1].UL ^ 0x80000000
    Op_Lwc1,            // fpuRegs.fpr[dest].UL = mem[GPR[src1]+imm]    dest=ft, src1=base
    Op_Swc1,            // mem[GPR[src1]+imm] = fpuRegs.fpr[dest].UL    dest=ft, src1=base
    Op_BC1F,            // s_branch_taken = (fprc[31] & 0x00800000) == 0 ? 1 : 0; 3-op pattern 用
    Op_BC1T,            // s_branch_taken = (fprc[31] & 0x00800000) != 0 ? 1 : 0

    // ─── Phase G: MMI native (NEON 128-bit) ──────────
    // GPR[dest/src1/src2] の UQ (u128) を full 128-bit NEON 操作。
    Op_Pand,            // rd = rs AND rt (64bit x 2)
    Op_Por,             // rd = rs OR  rt
    Op_Pxor,            // rd = rs XOR rt
    Op_Pnor,            // rd = ~(rs OR rt)
    Op_Paddb,           // rd[i] = rs[i] + rt[i]  (byte wrap)     dest/src1/src2 = rd/rs/rt
    Op_Paddh,           // halfword wrap
    Op_Paddw,           // word wrap
    Op_Psubb,
    Op_Psubh,
    Op_Psubw,
    Op_Pceqb,           // rd[i] = rs[i] == rt[i] ? 0xFF : 0
    Op_Pceqh,
    Op_Pceqw,
    Op_Pcgtb,           // signed compare gt
    Op_Pcgth,
    Op_Pcgtw,

    // ─── Phase F+: FPU 算術 (クランプ付き) ─────────
    Op_AddS,            // fpr[dest] = fpuDouble(fpr[src1]) + fpuDouble(fpr[src2])
    Op_SubS,
    Op_MulS,
    Op_DivS,
    Op_MaddS,           // fpr[dest] = ACC + fpuDouble(src1)*fpuDouble(src2)
    Op_MsubS,
    Op_AddaS,           // ACC = fpr[src1] + fpr[src2]  (dest unused)
    Op_SubaS,
    Op_MulaS,
    Op_MaddaS,          // ACC += fpuDouble(src1) * fpuDouble(src2)
    Op_MsubaS,
    Op_MaxS,            // fpr[dest] = max(fpr[src1], fpr[src2])
    Op_MinS,
    Op_SqrtS,           // fpr[dest] = sqrt(|fpr[src2]|)  (ft=src2 semantics)
    Op_RsqrtS,          // fpr[dest] = fpr[src1] / sqrt(|fpr[src2]|)
    Op_CvtS,            // fpr[dest] = (float)(s32)fpr[src1].UL
    Op_CvtW,            // fpr[dest].SL = (s32)fpr[src1].f (with clamp)
    Op_CeqS,            // if (fpr[src1] == fpr[src2]) set fprc[31]|=FPUflagC else clear
    Op_CltS,            // if (fpr[src1] <  fpr[src2])
    Op_CleS,            // if (fpr[src1] <= fpr[src2])
    Op_CfS,             // clearFPUFlagC (unconditional clear)

    // ─── Phase G+: MMI 追加 (NEON 継続) ──────────────
    Op_Pmaxh,           // MMI0 sa=7: signed halfword max
    Op_Pmaxw,           // MMI0 sa=3: signed word max
    Op_Pminh,           // MMI1 sa=7: signed halfword min
    Op_Pminw,           // MMI1 sa=3: signed word min
    Op_Psllh,           // MMI funct=0x34: halfword shift left imm sa
    Op_Psrlh,           // MMI funct=0x36: halfword shift right logical
    Op_Psrah,           // MMI funct=0x37: halfword shift right arith
    Op_Psllw,           // MMI funct=0x3C: word shift left
    Op_Psrlw,           // MMI funct=0x3E
    Op_Psraw,           // MMI funct=0x3F
    Op_Paddsb,          // MMI0 sa=24: signed saturating byte add
    Op_Paddsh,          // MMI0 sa=20: signed saturating half add
    Op_Paddsw,          // MMI0 sa=16: signed saturating word add
    Op_Psubsb,          // MMI0 sa=25: signed saturating byte sub
    Op_Psubsh,          // MMI0 sa=21
    Op_Psubsw,          // MMI0 sa=17
    Op_Paddub,          // MMI1 sa=24: unsigned saturating byte add
    Op_Padduh,          // MMI1 sa=20
    Op_Padduw,          // MMI1 sa=16
    Op_Psubub,          // MMI1 sa=25
    Op_Psubuh,          // MMI1 sa=21
    Op_Psubuw,          // MMI1 sa=17
    Op_Pextlb,          // MMI0 sa=26: interleave lower bytes
    Op_Pextlh,          // MMI0 sa=22
    Op_Pextlw,          // MMI0 sa=18
    Op_Pextub,          // MMI1 sa=26
    Op_Pextuh,          // MMI1 sa=22
    Op_Pextuw,          // MMI1 sa=18
    Op_Pcpyld,          // MMI2 sa=14: rd.lo = rt.lo, rd.hi = rs.lo (copy low doubles)
    Op_Pcpyud,          // MMI3 sa=14: rd.lo = rs.hi, rd.hi = rt.hi

    // ─── Phase L: 64-bit ALU native ──────────────
    Op_DaddU,           // GPR[dest].UD[0] = GPR[src1].UD[0] + GPR[src2].UD[0]
    Op_DsubU,           // 64bit sub
    Op_DaddIU,          // GPR[dest].UD[0] = GPR[src1].UD[0] + (s64)(s16)imm
    Op_Dsll,            // GPR[dest].UD[0] = GPR[src2].UD[0] << (imm & 0x3F) ; imm=sa
    Op_Dsrl,
    Op_Dsra,
    Op_Dsll32,          // GPR[dest] = GPR[src2] << (imm+32)
    Op_Dsrl32,
    Op_Dsra32,
    Op_Dsllv,           // GPR[dest] = GPR[src2] << (GPR[src1] & 0x3F)
    Op_Dsrlv,
    Op_Dsrav,

    // ─── Phase N: Mult/Div + HI/LO native ────────
    Op_Mult,            // (s64) = (s32)rs * (s32)rt; LO=(s32)result, HI=(s32)(result>>32); rd!=0→rd=LO
    Op_MultU,
    Op_Div,             // rs/rt → LO, rs%rt → HI
    Op_DivU,
    Op_Mfhi,            // GPR[dest] = HI
    Op_Mflo,            // GPR[dest] = LO
    Op_Mthi,            // HI = GPR[src1]
    Op_Mtlo,            // LO = GPR[src1]

    // ─── P101-4 Phase V: COP2 (VU0 macro mode) native ─────
    // Upper FMAC macro (VADD/VSUB/VMUL/VMADD/VMSUB/VMAX/VMINI + bc/i/q/A variants)
    // and sub-ops (ADDA/SUBA/MULA/MADDA/MSUBA + ABS/NOP/CLIP/OPMULA/FTOI/ITOF).
    // handler = Int_COP2SPECIAL1PrintTable[funct] (resolved at decode time for fallback).
    // 処理: _vu0FinishMicro() → VU0.code=raw → iv2::vu_ops::UpperDispatch(&VU0) →
    //   native 成功なら FMAC 系は SYNCMSFLAGS、CLIP は SYNCCLIPFLAG、MAX/MINI/FTOI/ITOF/ABS は no-sync。
    //   native 未対応 (OPMSUB/OPMULA/CLIP/FTOI/ITOF 等) は handler() fallback。
    Op_Vu0Macro,

    // ─── P101-3 Phase N 残り: 128-bit load/store native (PS2 LQ/SQ/LQC2/SQC2) ───
    // これまで Fallback。profile で total の 6.7% (合計 ~380M/5.66B ops) を占める最大 bottleneck。
    // 16-byte align 前提、misalign は Fallback に退避。
    Op_Lq,              // GPR[dest].UQ = mem128[GPR[src1] + imm]
    Op_Sq,              // mem128[GPR[src1] + imm] = GPR[dest].UQ
    Op_Lqc2,            // VU0.VF[dest].UQ = mem128[GPR[src1] + imm]
    Op_Sqc2,            // mem128[GPR[src1] + imm] = VU0.VF[dest].UQ

    // ADDI (primary 0x08): ADDIU と同じ s32 加算だが signed overflow で Overflow 例外発火。
    // 実機ゲームは 99%+ overflow を起こさない (programmer guard)。native で inline compute +
    // branch で overflow 検出、稀な overflow のみ handler fallback (cpuRegs.code = raw 経由)。
    Op_Addi,            // GPR[dest] = (s64)(s32)(GPR[src1].SL[0] + (s32)imm) w/ overflow trap

    Op_COUNT
};

// 1 命令分のプリデコード結果。
// 16 bytes (align 8) — L1 に 4 命令詰めるサイズ。
struct CachedInst {
    u32 raw;                       // 生命令語 (Fallback で cpuRegs.code に設定)
    s32 imm;                       // native imm (sign-ext or zero-ext 済)
    FallbackFn handler;            // Fallback 時: opcode.interpret。native 時: NULL
    u8  op;                        // iv2::Op enum
    u8  dest;                      // 0..31
    u8  src1;                      // 0..31
    u8  src2;                      // 0..31
    u16 cycles;                    // [iv2 per-inst cycle fix 2026-05-31] opcode.cycles (mul 未適用)
    // [P101-14 R1 2026-08-06] この entry を dispatch したときの pc 前進量 (バイト)。
    // 既定 4。compile 時に消した Nop の分がここに前方マージされる (下記 compact パス)。
    // 既存の padding (offset 22) を使うので sizeof は 24 のまま。
    u8  pc_adv;
};
static_assert(sizeof(CachedInst) == 24, "CachedInst must be 24 bytes");

static constexpr u32 MAX_BLOCK_INSTS    = 32;
// [P101-14 R1] pc_adv(u8) が 1 block 分の pc 前進を保持できること
static_assert(MAX_BLOCK_INSTS * 4 <= 255, "pc_adv(u8) overflow");
static constexpr u32 BLOCK_HASH_BITS    = 17;
static constexpr u32 BLOCK_HASH_SIZE    = 1u << BLOCK_HASH_BITS;
static constexpr u32 BLOCK_HASH_MASK    = BLOCK_HASH_SIZE - 1;
// [P101-14 E5 2026-08-06] block arena は bump allocator + 世代番号で個別回収が無く、
// 枯渇すると InvalidateAll() が **キャッシュ全体を捨てる**。旧 16384 では
// 「平均ブロック長 < INST_POOL_CAP/BLOCK_POOL_CAP = 32 命令」が常に成立する
// (ブロックは最初の分岐で必ず切れるので平均が 32 に達するのは原理的に不可能) ため、
// **必ずブロックプール側が先に枯れる** = 命令プール 512K は一度も使い切られない。
// 65536 にすると 65536 × 平均 8 命令 ≈ 512K で両者がほぼ同時に枯れる = 釣り合う。
// メモリ増 = 49152 × 40B ≈ 2.0MB。
// 実効 cap は g_block_pool_cap (env `iPSX2_EE_BLOCK_POOL` で旧値 16384 に戻せる)。
static constexpr u32 BLOCK_POOL_CAP     = 65536;
static constexpr u32 INST_POOL_CAP      = 512 * 1024;    // 512K 命令 = 12MB

struct Block {
    u32 entry_pc;
    u32 end_pc;
    u32 inst_offset;
    u16 inst_count;
    u16 generation;
    u32 guest_cycles_sum;          // ブロック合計 cycle (cycle_mul 未適用)
    // [P101 Phase H] ブロック間リンク: first miss 時に install、以降スキャン不要
    Block* link_taken;             // 分岐 taken 先 (J/JAL/BEQ taken 等)
    Block* link_not_taken;         // 分岐 fall-through 先 (BEQ not-taken)
};

extern Block* g_block_map[BLOCK_HASH_SIZE];
extern Block g_block_pool[BLOCK_POOL_CAP];
extern u32 g_block_pool_next;
extern CachedInst g_inst_pool[INST_POOL_CAP];
extern u32 g_inst_pool_next;
extern u16 g_generation;
// [P101-14 E5] 実効 block pool 上限 (kill-switch)。既定 = BLOCK_POOL_CAP、
// `iPSX2_EE_BLOCK_POOL=16384` で旧挙動。同一バイナリ A/B 用。
extern u32 g_block_pool_cap;

// [P101-14 E5-probe] 無効化と再コンパイルの内訳 (cold path のみ、`iPSX2_IV2_PROFILE=1` で出力)
extern u64 g_iv2_inval_pool;        // pool 枯渇による InvalidateAll
extern u64 g_iv2_inval_clear;       // iv2::Clear が実際に行った InvalidateAll
extern u64 g_iv2_inval_clear_calls; // iv2::Clear の呼出回数 (skip 分を含む)
extern u64 g_iv2_compiles;          // CompileBlock 呼出
extern u64 g_iv2_clear_insts;       // Clear が要求した size_insts の総和

static inline u32 block_hash(u32 pc) { return (pc >> 2) & BLOCK_HASH_MASK; }

// ───────────────────────────────────────────────────────────────
// [P101-14 E6 2026-08-06] コードページ ビットマップ
//
// 実測 (probe `iPSX2_IV2_PROFILE=1` の @@IV2_INVAL@@): GTA VC 定常 3D で
//   inval_pool=0 / inval_clear=62,532 / compiles=8,744,823 (94 秒)
// = **プール枯渇は一度も起きておらず、EE 再コンパイル (leaf profile 1.94%) は
//   すべて `Cpu->Clear` 由来**。1 回の Clear につき約 140 ブロックを再コンパイル
//   しており、ブロックキャッシュは常に 232 個程度までしか育たない。
//
// 対策 = 「消す範囲にコードが無ければ何もしない」。IOP 側 (-4.76% で差し戻し) と
// 違い、EE の Clear は **毎秒約 325 回しか呼ばれない** (IOP は全 RAM ストア) ので
// 判定コストが無視でき、逆に invalidate 側 (全ブロック破棄 + TLB L0 全消去) が高い。
//
// マスクは PCSX2 の JIT と同一の `HWADDR(x) = x & 0x1fffffff` (`iR5900.cpp:298`)。
// 出荷 JIT の recClear も同じ近似で invalidate 範囲を決めており、加えて iv2 には
// ブロック dispatch ごとの content 検証 (block_content_valid) がある。
// ビットは InvalidateAll でしか落とさない = **常に過大近似 = 安全側**。
static constexpr u32 CODE_PAGE_SHIFT = 12;                                  // 4KB ページ
static constexpr u32 CODE_PAGE_SPAN  = 128u << 20;                          // 最大 128MB (extra mem mode)
static constexpr u32 CODE_PAGE_COUNT = CODE_PAGE_SPAN >> CODE_PAGE_SHIFT;   // 32768 ページ
extern u64 g_code_pages[CODE_PAGE_COUNT / 64];                              // 4KB のビットマップ
extern bool g_ee_clear_range;                                               // kill-switch (既定 true)
extern bool g_ee_nop_compact;   // [P101-14 R1] compile 時 Nop 消去 kill-switch (既定 true)

// [addr, addr+bytes) にコンパイル済みコードが有りうるか。true = 保守的に無効化せよ。
bool CodePagesOverlap(u32 addr, u32 bytes);

Block* GetOrCompileBlock(u32 pc);
Block* CompileBlock(u32 pc);
void InvalidateAll();

// Compile helper: raw opcode を iv2 IR にデコード。
// is_terminator, cycles, handler を埋める。
void DecodeInst(u32 raw, CachedInst& out);

} // namespace iv2
