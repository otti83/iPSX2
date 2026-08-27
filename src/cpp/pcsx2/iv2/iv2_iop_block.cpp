// SPDX-License-Identifier: GPL-3.0+
// P101-2 Phase R: iv2 IOP decoder + block cache

#include "PrecompiledHeader.h"
#include "Common.h"
#include "R3000A.h"
#include "IopMem.h"
#include "iv2/iv2_iop_block.h"

namespace iv2_iop {

alignas(64) Block* g_block_map[BLOCK_HASH_SIZE] = {};
alignas(64) Block g_block_pool[BLOCK_POOL_CAP];
u32 g_block_pool_next = 0;
alignas(64) CachedInst g_inst_pool[INST_POOL_CAP];
u32 g_inst_pool_next = 0;
u16 g_generation = 1;

bool IsTerminator(u32 raw)
{
    const u32 primary = raw >> 26;
    switch (primary) {
    case 0x02: /* J     */
    case 0x03: /* JAL   */
    case 0x04: /* BEQ   */
    case 0x05: /* BNE   */
    case 0x06: /* BLEZ  */
    case 0x07: /* BGTZ  */
        return true;
    case 0x01: {  // REGIMM
        const u32 sub = (raw >> 16) & 0x1F;
        switch (sub) {
        case 0x00: /* BLTZ   */
        case 0x01: /* BGEZ   */
        case 0x10: /* BLTZAL */
        case 0x11: /* BGEZAL */
            return true;
        }
        return false;
    }
    case 0x10: {  // COP0: RFE は pc 変更なしだが ev test 影響
        const u32 sub = (raw >> 21) & 0x1F;
        if (sub == 0x10) {
            const u32 funct = raw & 0x3F;
            if (funct == 0x10) /* RFE */ return true;
        }
        return false;
    }
    case 0x00: {  // SPECIAL
        const u32 funct = raw & 0x3F;
        switch (funct) {
        case 0x08: /* JR      */
        case 0x09: /* JALR    */
        case 0x0C: /* SYSCALL */
        case 0x0D: /* BREAK   */
            return true;
        }
        return false;
    }
    }
    return false;
}

// ────────────────────────────────────────────────
// psxBSC[] は extern で R3000A で宣言済み
// raw >> 26 を見て dispatch する primary 関数を返す
// ────────────────────────────────────────────────
static FallbackFn GetFallbackHandler(u32 raw)
{
    return psxBSC[raw >> 26];
}

void DecodeInst(u32 raw, CachedInst& out)
{
    out.raw = raw;
    out.op  = Op_Fallback;
    out.handler = GetFallbackHandler(raw);
    out.dest = 0;
    out.src1 = 0;
    out.src2 = 0;
    out.imm  = 0;

    const u32 primary = raw >> 26;
    const u32 rs = (raw >> 21) & 0x1F;
    const u32 rt = (raw >> 16) & 0x1F;
    const u32 rd = (raw >> 11) & 0x1F;
    const u32 sa = (raw >> 6)  & 0x1F;
    const u32 funct = raw & 0x3F;
    const s32 imm_se = (s32)(s16)(u16)(raw & 0xFFFF);
    const u32 imm_ze = raw & 0xFFFFu;

    switch (primary) {
    case 0x00: { // SPECIAL
        switch (funct) {
        case 0x20: /* ADD  */   // R3000 には overflow trap があるが BIOS/ゲームは発火しない想定
        case 0x21: /* ADDU */
            if (rd != 0) { out.op = Op_AddU; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x22: /* SUB  */
        case 0x23: /* SUBU */
            if (rd != 0) { out.op = Op_SubU; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x24: /* AND  */
            if (rd != 0) { out.op = Op_And; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x25: /* OR   */
            if (rd != 0) { out.op = Op_Or;  out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x26: /* XOR  */
            if (rd != 0) { out.op = Op_Xor; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x27: /* NOR  */
            if (rd != 0) { out.op = Op_Nor; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x2A: /* SLT  */
            if (rd != 0) { out.op = Op_Slt; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x2B: /* SLTU */
            if (rd != 0) { out.op = Op_SltU; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x00: /* SLL  */
            if (rd == 0 && rt == 0 && sa == 0) { out.op = Op_Nop; out.handler = nullptr; }
            else if (rd != 0) { out.op = Op_Sll; out.dest = rd; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr; }
            break;
        case 0x02: /* SRL  */
            if (rd != 0) { out.op = Op_Srl; out.dest = rd; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr; }
            break;
        case 0x03: /* SRA  */
            if (rd != 0) { out.op = Op_Sra; out.dest = rd; out.src2 = rt; out.imm = (s32)sa; out.handler = nullptr; }
            break;
        case 0x04: /* SLLV */
            if (rd != 0) { out.op = Op_Sllv; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x06: /* SRLV */
            if (rd != 0) { out.op = Op_Srlv; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
        case 0x07: /* SRAV */
            if (rd != 0) { out.op = Op_Srav; out.dest = rd; out.src1 = rs; out.src2 = rt; out.handler = nullptr; }
            break;
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
        case 0x18: /* MULT  */
            out.op = Op_Mult;  out.src1 = rs; out.src2 = rt; out.handler = nullptr;
            break;
        case 0x19: /* MULTU */
            out.op = Op_Multu; out.src1 = rs; out.src2 = rt; out.handler = nullptr;
            break;
        case 0x1A: /* DIV   */
            out.op = Op_Div;   out.src1 = rs; out.src2 = rt; out.handler = nullptr;
            break;
        case 0x1B: /* DIVU  */
            out.op = Op_Divu;  out.src1 = rs; out.src2 = rt; out.handler = nullptr;
            break;
        // JR/JALR/SYSCALL/BREAK は Fallback のまま (terminator)
        }
    } break;

    case 0x08: /* ADDI  */   // R3000 ADDI overflow は一応あるが BIOS/ゲームは発火しない想定
    case 0x09: /* ADDIU */
        if (rt != 0) { out.op = Op_AddIU; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x0A: /* SLTI  */
        if (rt != 0) { out.op = Op_SltI;  out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x0B: /* SLTIU */
        if (rt != 0) { out.op = Op_SltIU; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x0C: /* ANDI */
        if (rt != 0) { out.op = Op_AndI;  out.dest = rt; out.src1 = rs; out.imm = (s32)imm_ze; out.handler = nullptr; }
        break;
    case 0x0D: /* ORI  */
        if (rt != 0) { out.op = Op_OrI;   out.dest = rt; out.src1 = rs; out.imm = (s32)imm_ze; out.handler = nullptr; }
        break;
    case 0x0E: /* XORI */
        if (rt != 0) { out.op = Op_XorI;  out.dest = rt; out.src1 = rs; out.imm = (s32)imm_ze; out.handler = nullptr; }
        break;
    case 0x0F: /* LUI  */
        if (rt != 0) { out.op = Op_SetConst; out.dest = rt; out.imm = (s32)((u32)imm_ze << 16); out.handler = nullptr; }
        break;

    // Load/Store — HW アドレス (0x1F800000-0x1FFFFFFF) でも iopMemRead/Write が handler に dispatch するので共通に使える
    case 0x20: /* LB  */
        if (rt != 0) { out.op = Op_Lb;  out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x24: /* LBU */
        if (rt != 0) { out.op = Op_Lbu; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x21: /* LH  */
        if (rt != 0) { out.op = Op_Lh;  out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x25: /* LHU */
        if (rt != 0) { out.op = Op_Lhu; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x23: /* LW  */
        if (rt != 0) { out.op = Op_Lw;  out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr; }
        break;
    case 0x28: /* SB  */
        out.op = Op_Sb; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        break;
    case 0x29: /* SH  */
        out.op = Op_Sh; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        break;
    case 0x2B: /* SW  */
        out.op = Op_Sw; out.dest = rt; out.src1 = rs; out.imm = imm_se; out.handler = nullptr;
        break;
    // LWL/LWR/SWL/SWR は Fallback (境界跨ぎ複雑)

    default:
        // Fallback
        break;
    }

    (void)rd;
}

void InvalidateAll()
{
    g_generation++;
    if (g_generation == 0) g_generation = 1;
    g_block_pool_next = 0;
    g_inst_pool_next = 0;
}

// [P8] Native branch enable. Verified safe on FF10 OP (3min stability, clean
// exit, no exceptions). Emits 3-op triplet [BCheck | delay_slot | BFinalize]
// in place of fallback doBranch chain. Bails to fallback if delay slot is
// SYSCALL/BREAK/another-branch, or J/JAL target is 0 / 0x890.
bool g_iop_native_branch_enabled = []() {
    const char* e = std::getenv("iPSX2_IOP_NATIVE_BRANCH");
    return !e || *e != '0';  // default ON
}();

// [P8] Emit 3-op branch triplet: [BCheck/Jump] → [delay slot] → [BFinalize].
// Returns pc advance in bytes (8 = branch + BDS consumed), or 0 if branch cannot
// be natively translated (caller falls through to Fallback path).
//
// Safety bail-outs:
// - Delay slot is SYSCALL/BREAK/JR/JALR/branch: bail (delay slot itself can
//   change pc or raise exception; semantically messy).
// - J/JAL target is 0 or 0x890: these hit doBranch's IOP_HANDLER_SKIP /
//   SYSMEM reboot special paths; must let doBranch handle.
static u32 TryCompileIopBranch(u32 raw, u32 branch_pc, CachedInst*& out, u32& inst_count)
{
    const u32 primary = raw >> 26;
    const u32 rs = (raw >> 21) & 0x1F;
    const u32 rt = (raw >> 16) & 0x1F;
    const u32 rd = (raw >> 11) & 0x1F;
    const s32 off = (s32)(s16)(u16)(raw & 0xFFFF);
    // Target for PC-relative branches: branch_pc + 4 + off*4
    const u32 target_pc_rel = branch_pc + 4u + (u32)(off * 4);
    // Target for J/JAL: (branch_pc+4 & 0xF0000000) | (imm26 << 2)
    const u32 target_pc_j = ((branch_pc + 4u) & 0xF0000000u) | ((raw & 0x03FFFFFFu) << 2);

    // Pre-check: delay slot must be safe to execute in-block.
    u32 bds_raw = iopMemRead32(branch_pc + 4u);
    u32 bds_primary = bds_raw >> 26;
    // Exclude SYSCALL/BREAK (primary 0 funct 0x0C/0x0D) and any branch in delay slot.
    if (bds_primary == 0x00 /*SPECIAL*/) {
        u32 bds_funct = bds_raw & 0x3F;
        if (bds_funct == 0x0C || bds_funct == 0x0D || bds_funct == 0x08 || bds_funct == 0x09)
            return 0;
    } else if (bds_primary >= 0x01 && bds_primary <= 0x07) {
        // Branches in delay slot (REGIMM, J, JAL, BEQ, BNE, BLEZ, BGTZ): undefined, bail.
        return 0;
    } else if (bds_primary == 0x10) {
        // COP0 (RFE etc): pc side-effects possible, bail.
        return 0;
    }

    auto emit_triplet = [&](Op check_op, u8 src1, u8 src2, u32 tpc, u8 link_reg) {
        CachedInst& check = *out++;
        check.raw = raw;
        check.handler = nullptr;
        check.op = check_op;
        check.dest = link_reg;
        check.src1 = (u8)src1;
        check.src2 = (u8)src2;
        check.imm = (s32)tpc;
        ++inst_count;
        // Delay slot
        CachedInst& bds = *out++;
        DecodeInst(bds_raw, bds);
        ++inst_count;
        // BFinalize with fall-through pc
        CachedInst& fin = *out++;
        fin.raw = 0;
        fin.handler = nullptr;
        fin.op = Op_BFinalize;
        fin.dest = 0;
        fin.src1 = 0;
        fin.src2 = 0;
        fin.imm = (s32)(branch_pc + 8u);
        ++inst_count;
    };

    switch (primary) {
    case 0x02: /* J */
        if (target_pc_j == 0 || target_pc_j == 0x890) return 0;  // doBranch special paths
        emit_triplet(Op_JumpConst, 0, 0, target_pc_j, /*link=*/0);
        return 8;
    case 0x03: /* JAL */
        if (target_pc_j == 0 || target_pc_j == 0x890) return 0;
        emit_triplet(Op_JumpConst, 0, 0, target_pc_j, /*link=*/31);
        return 8;
    case 0x04: /* BEQ */
        emit_triplet(Op_BCheckEq, (u8)rs, (u8)rt, target_pc_rel, 0);
        return 8;
    case 0x05: /* BNE */
        emit_triplet(Op_BCheckNe, (u8)rs, (u8)rt, target_pc_rel, 0);
        return 8;
    case 0x06: /* BLEZ */
        emit_triplet(Op_BCheckLez, (u8)rs, 0, target_pc_rel, 0);
        return 8;
    case 0x07: /* BGTZ */
        emit_triplet(Op_BCheckGtz, (u8)rs, 0, target_pc_rel, 0);
        return 8;
    case 0x01: { /* REGIMM */
        const u32 sub = rt;
        switch (sub) {
        case 0x00: /* BLTZ */
            emit_triplet(Op_BCheckLtz, (u8)rs, 0, target_pc_rel, 0);
            return 8;
        case 0x01: /* BGEZ */
            emit_triplet(Op_BCheckGez, (u8)rs, 0, target_pc_rel, 0);
            return 8;
        case 0x10: /* BLTZAL */
            emit_triplet(Op_BCheckLtzLink, (u8)rs, 0, target_pc_rel, 31);
            return 8;
        case 0x11: /* BGEZAL */
            emit_triplet(Op_BCheckGezLink, (u8)rs, 0, target_pc_rel, 31);
            return 8;
        }
        return 0;
    }
    // JR/JALR and all other terminators: not natively translated (Stage A).
    // SYSCALL/BREAK/RFE: never natively translated.
    default:
        (void)rd;
        return 0;
    }
}

Block* CompileBlock(u32 entry_pc)
{
    if (g_block_pool_next >= BLOCK_POOL_CAP ||
        g_inst_pool_next + MAX_BLOCK_INSTS >= INST_POOL_CAP) {
        InvalidateAll();
    }

    Block* blk = &g_block_pool[g_block_pool_next++];
    blk->entry_pc    = entry_pc;
    blk->inst_offset = g_inst_pool_next;
    blk->generation  = g_generation;
    blk->inst_count  = 0;

    u32 pc = entry_pc;
    CachedInst* out = &g_inst_pool[g_inst_pool_next];
    u32 inst_count = 0;

    // Reserve 3 slots at block end for native branch triplet (BCheck + BDS + BFinalize).
    const u32 cap = g_iop_native_branch_enabled ? (MAX_BLOCK_INSTS - 3) : MAX_BLOCK_INSTS;

    for (u32 i = 0; i < cap; i++) {
        u32 raw = iopMemRead32(pc);
        if (g_iop_native_branch_enabled && IsTerminator(raw)) {
            u32 advance = TryCompileIopBranch(raw, pc, out, inst_count);
            if (advance > 0) {
                pc += advance;
                break;
            }
            // Fallback: emit as-is, block ends.
            DecodeInst(raw, *out);
            ++out;
            ++inst_count;
            pc += 4;
            break;
        }
        DecodeInst(raw, *out);
        ++out;
        ++inst_count;
        pc += 4;
        if (IsTerminator(raw)) break;
    }

    blk->inst_count = (u16)inst_count;
    blk->end_pc     = pc;
    g_inst_pool_next += inst_count;

    g_block_map[block_hash(entry_pc)] = blk;
    return blk;
}

Block* GetOrCompileBlock(u32 pc)
{
    u32 idx = block_hash(pc);
    Block* blk = g_block_map[idx];
    if (__builtin_expect(blk != nullptr
            && blk->entry_pc == pc
            && blk->generation == g_generation, 1)) {
        return blk;
    }
    return CompileBlock(pc);
}

} // namespace iv2_iop
