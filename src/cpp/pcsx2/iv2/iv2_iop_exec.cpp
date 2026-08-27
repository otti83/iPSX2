// SPDX-License-Identifier: GPL-3.0+
// P101-2 Phase R: iv2 IOP 実行ループ + R3000Acpu 差し替え
//
// 方針:
//  - Fallback branch は psxBSC[raw>>26]() がそのまま doBranch() を内部呼出し、
//    doBranch が delay slot + iopEventTest() を処理する。
//    → Block は分岐命令で終端、分岐後は branch2 が set されて while (!branch2) を抜ける
//  - Native non-branch 命令はブロック途中で次々に dispatch
//  - pc+=4, cycle++ は各命令前で行う (execI と同じ挙動)

#include "PrecompiledHeader.h"
#include "Common.h"
#include "R3000A.h"
#include "IopMem.h"
#include "IopHw.h"
#include "IopBios.h"
#include "IopDma.h"
#include "iv2/iv2_iop_block.h"

using namespace R3000A;

// R3000AInterpreter.cpp にある static だが、同じシンボルが必要。
// iv2_iop は独立経路のため、branch2 を内部で持つ。
// 注意: Fallback 分岐は R3000AInterpreter.cpp の static branch2 を set するため、
// iv2_iop 側でも参照できるよう Fallback 経路だけは R3000AInterpreter.cpp の分岐に
// そのまま委ねる (同じ関数ポインタ psxBSC[] を使用)。
//
// ただし iv2_iop では分岐命令後に block 終端するため、branch2 の値は直接参照しない。
// 代わりに「分岐命令を実行したら block 終端」のルールで対応。

namespace iv2_iop {

// [P8] Native branch state (used by BCheck → BFinalize within one block).
// Thread-local unnecessary: iv2_iop is single-threaded on the EE thread.
static u8  s_iop_branch_taken = 0;
static u32 s_iop_branch_target = 0;

// [P8] iopEventTest cycle gating: only fire when cycle >= iopNextEventCycle.
// iopEventTest itself resets iopNextEventCycle = cycle + iopWaitCycles.
// Default OFF — matches legacy doBranch semantics (iopEventTest every branch).
// When ON, reduces iopEventTest invocations ~167x (verified 99.4% skip rate),
// but aggregate psxRcntUpdate work is conserved (catch-up at each call).
static bool s_iop_bfin_gate = []() {
    const char* e = std::getenv("iPSX2_IOP_BFIN_GATE");
    return e && *e == '1';  // default OFF (no gating, match legacy)
}();

// [P8 DIAG] iPSX2_IOP_NATIVE_BRANCH_DIAG=1 enables BFinalize counters.
static bool s_iop_bfin_diag = []() {
    const char* e = std::getenv("iPSX2_IOP_NATIVE_BRANCH_DIAG");
    return e && *e == '1';
}();
static u64 s_bfin_called = 0;
static u64 s_bfin_gated_skip = 0;
static u64 s_bfin_eventtest_fired = 0;

static __forceinline void iop_finalize_eventtest()
{
    if (s_iop_bfin_diag) s_bfin_called++;
    if (s_iop_bfin_gate) {
        if ((s32)(psxRegs.cycle - psxRegs.iopNextEventCycle) < 0) {
            if (s_iop_bfin_diag) s_bfin_gated_skip++;
            return;
        }
    }
    if (s_iop_bfin_diag) s_bfin_eventtest_fired++;
    iopEventTest();
}

static __forceinline void iop_bfin_dump_periodic()
{
    if (!s_iop_bfin_diag) return;
    static u64 s_next = 1000000;
    if (s_bfin_called >= s_next) {
        s_next += 1000000;
        Console.WriteLn("@@IOP_NATIVE_BFIN@@ called=%llu gated_skip=%llu evt_fired=%llu gate_rate=%.1f%%",
            (unsigned long long)s_bfin_called,
            (unsigned long long)s_bfin_gated_skip,
            (unsigned long long)s_bfin_eventtest_fired,
            s_bfin_called ? 100.0 * (double)s_bfin_gated_skip / (double)s_bfin_called : 0.0);
    }
}

// cycle/cycleEE 会計は intExecuteBlock と同じ
static s32 ExecuteBlock(s32 eeCycles)
{
    // Heartbeat
    static u32 s_last_log_cycle = 0;
    if ((u32)(psxRegs.cycle - s_last_log_cycle) > 36864000) {
        Console.WriteLn("@@IOP_IV2_ALIVE@@ pc=%08x cycle=%d", psxRegs.pc, psxRegs.cycle);
        s_last_log_cycle = psxRegs.cycle;
    }

    psxRegs.iopBreak    = 0;
    psxRegs.iopCycleEE  = eeCycles;
    u32 lastIOPCycle    = 0;

    while (psxRegs.iopCycleEE > 0) {
        lastIOPCycle = psxRegs.cycle;

        // BIOS fast-path (intExecuteBlock と完全一致)
        if ((psxHu32(HW_ICFG) & 8) &&
            ((psxRegs.pc & 0x1fffffffU) == 0xa0 ||
             (psxRegs.pc & 0x1fffffffU) == 0xb0 ||
             (psxRegs.pc & 0x1fffffffU) == 0xc0))
        {
            psxBiosCall();
        }

        // 分岐で block 終端、分岐の Fallback handler が branch2 を set
        // ここでは分岐実行後に再度 block を取り直すループを回す
        // → R3000AInterpreter の branch2 をそのまま使うため、一発判定用に static 参照する

        // NOTE: branch2 は R3000AInterpreter.cpp 内の static で、外部参照不可。
        // 代わりに「分岐命令を実行したら1ブロックを抜ける」ルール + 分岐実行後の
        // pc は doBranch が書き換えているため、次回 GetOrCompileBlock(pc) で
        // 新 pc を拾う。これで intExecuteBlock の while (!branch2) と等価になる。
        //
        // ただし「1 ブロック = 1 branch 命令で終わる」ため、分岐 handler 実行後に
        // 外ループ (iopCycleEE > 0) に戻り、cycle 会計を行う必要がある。

        const Block* blk = GetOrCompileBlock(psxRegs.pc);

        const CachedInst* inst = &g_inst_pool[blk->inst_offset];
        const u32 n = blk->inst_count;
        u32* const r = psxRegs.GPR.r;

        for (u32 i = 0; i < n; i++) {
            const CachedInst& c = inst[i];
            psxRegs.pc    += 4;
            psxRegs.cycle += 1;

            switch (c.op) {
            case Op_Fallback:
                psxRegs.code = c.raw;
                c.handler();
                break;

            case Op_Nop:
                break;

            case Op_SetConst:
                r[c.dest] = (u32)c.imm;
                break;

            case Op_AddIU:
                r[c.dest] = r[c.src1] + (u32)c.imm;
                break;

            case Op_AndI:
                r[c.dest] = r[c.src1] & (u32)c.imm;
                break;

            case Op_OrI:
                r[c.dest] = r[c.src1] | (u32)c.imm;
                break;

            case Op_XorI:
                r[c.dest] = r[c.src1] ^ (u32)c.imm;
                break;

            case Op_SltI:
                r[c.dest] = ((s32)r[c.src1] < c.imm) ? 1u : 0u;
                break;

            case Op_SltIU:
                r[c.dest] = (r[c.src1] < (u32)c.imm) ? 1u : 0u;
                break;

            case Op_AddU:
                r[c.dest] = r[c.src1] + r[c.src2];
                break;

            case Op_SubU:
                r[c.dest] = r[c.src1] - r[c.src2];
                break;

            case Op_And:
                r[c.dest] = r[c.src1] & r[c.src2];
                break;

            case Op_Or:
                r[c.dest] = r[c.src1] | r[c.src2];
                break;

            case Op_Xor:
                r[c.dest] = r[c.src1] ^ r[c.src2];
                break;

            case Op_Nor:
                r[c.dest] = ~(r[c.src1] | r[c.src2]);
                break;

            case Op_Slt:
                r[c.dest] = ((s32)r[c.src1] < (s32)r[c.src2]) ? 1u : 0u;
                break;

            case Op_SltU:
                r[c.dest] = (r[c.src1] < r[c.src2]) ? 1u : 0u;
                break;

            case Op_Sll:
                r[c.dest] = r[c.src2] << (c.imm & 0x1F);
                break;

            case Op_Srl:
                r[c.dest] = r[c.src2] >> (c.imm & 0x1F);
                break;

            case Op_Sra:
                r[c.dest] = (u32)((s32)r[c.src2] >> (c.imm & 0x1F));
                break;

            case Op_Sllv:
                r[c.dest] = r[c.src2] << (r[c.src1] & 0x1F);
                break;

            case Op_Srlv:
                r[c.dest] = r[c.src2] >> (r[c.src1] & 0x1F);
                break;

            case Op_Srav:
                r[c.dest] = (u32)((s32)r[c.src2] >> (r[c.src1] & 0x1F));
                break;

            case Op_Mfhi:
                r[c.dest] = psxRegs.GPR.n.hi;
                break;

            case Op_Mflo:
                r[c.dest] = psxRegs.GPR.n.lo;
                break;

            case Op_Mthi:
                psxRegs.GPR.n.hi = r[c.src1];
                break;

            case Op_Mtlo:
                psxRegs.GPR.n.lo = r[c.src1];
                break;

            case Op_Mult: {
                s64 res = (s64)(s32)r[c.src1] * (s64)(s32)r[c.src2];
                psxRegs.GPR.n.lo = (u32)(u64)res;
                psxRegs.GPR.n.hi = (u32)((u64)res >> 32);
                break;
            }

            case Op_Multu: {
                u64 res = (u64)r[c.src1] * (u64)r[c.src2];
                psxRegs.GPR.n.lo = (u32)res;
                psxRegs.GPR.n.hi = (u32)(res >> 32);
                break;
            }

            case Op_Div: {
                const u32 rs_u = r[c.src1];
                const u32 rt_u = r[c.src2];
                if (rt_u == 0) {
                    psxRegs.GPR.n.lo = ((s32)rs_u < 0) ? 1u : 0xFFFFFFFFu;
                    psxRegs.GPR.n.hi = rs_u;
                } else if (rs_u == 0x80000000u && rt_u == 0xFFFFFFFFu) {
                    psxRegs.GPR.n.lo = 0x80000000u;
                    psxRegs.GPR.n.hi = 0;
                } else {
                    psxRegs.GPR.n.lo = (u32)((s32)rs_u / (s32)rt_u);
                    psxRegs.GPR.n.hi = (u32)((s32)rs_u % (s32)rt_u);
                }
                break;
            }

            case Op_Divu: {
                const u32 rs_u = r[c.src1];
                const u32 rt_u = r[c.src2];
                if (rt_u == 0) {
                    psxRegs.GPR.n.lo = 0xFFFFFFFFu;
                    psxRegs.GPR.n.hi = rs_u;
                } else {
                    psxRegs.GPR.n.lo = rs_u / rt_u;
                    psxRegs.GPR.n.hi = rs_u % rt_u;
                }
                break;
            }

            case Op_Lb: {
                u32 addr = r[c.src1] + (u32)c.imm;
                r[c.dest] = (u32)(s32)(s8)iopMemRead8(addr);
                break;
            }
            case Op_Lbu: {
                u32 addr = r[c.src1] + (u32)c.imm;
                r[c.dest] = (u32)(u8)iopMemRead8(addr);
                break;
            }
            case Op_Lh: {
                u32 addr = r[c.src1] + (u32)c.imm;
                r[c.dest] = (u32)(s32)(s16)iopMemRead16(addr);
                break;
            }
            case Op_Lhu: {
                u32 addr = r[c.src1] + (u32)c.imm;
                r[c.dest] = (u32)(u16)iopMemRead16(addr);
                break;
            }
            case Op_Lw: {
                u32 addr = r[c.src1] + (u32)c.imm;
                r[c.dest] = iopMemRead32(addr);
                break;
            }
            case Op_Sb: {
                u32 addr = r[c.src1] + (u32)c.imm;
                iopMemWrite8(addr, (u8)r[c.dest]);
                break;
            }
            case Op_Sh: {
                u32 addr = r[c.src1] + (u32)c.imm;
                iopMemWrite16(addr, (u16)r[c.dest]);
                break;
            }
            case Op_Sw: {
                u32 addr = r[c.src1] + (u32)c.imm;
                iopMemWrite32(addr, r[c.dest]);
                break;
            }

            // ─── P8 Native branch triplet ────────────────────────────
            // BCheck sets s_iop_branch_taken / s_iop_branch_target and sets
            // iopIsDelaySlot=true so the next (delay slot) instruction runs
            // with correct flag for potential exception path.
            case Op_BCheckEq:
                s_iop_branch_taken = (r[c.src1] == r[c.src2]) ? 1 : 0;
                s_iop_branch_target = (u32)c.imm;
                iopIsDelaySlot = true;
                break;
            case Op_BCheckNe:
                s_iop_branch_taken = (r[c.src1] != r[c.src2]) ? 1 : 0;
                s_iop_branch_target = (u32)c.imm;
                iopIsDelaySlot = true;
                break;
            case Op_BCheckLez:
                s_iop_branch_taken = ((s32)r[c.src1] <= 0) ? 1 : 0;
                s_iop_branch_target = (u32)c.imm;
                iopIsDelaySlot = true;
                break;
            case Op_BCheckGtz:
                s_iop_branch_taken = ((s32)r[c.src1] > 0) ? 1 : 0;
                s_iop_branch_target = (u32)c.imm;
                iopIsDelaySlot = true;
                break;
            case Op_BCheckLtz:
                s_iop_branch_taken = ((s32)r[c.src1] < 0) ? 1 : 0;
                s_iop_branch_target = (u32)c.imm;
                iopIsDelaySlot = true;
                break;
            case Op_BCheckGez:
                s_iop_branch_taken = ((s32)r[c.src1] >= 0) ? 1 : 0;
                s_iop_branch_target = (u32)c.imm;
                iopIsDelaySlot = true;
                break;
            case Op_BCheckLtzLink:
                // BLTZAL: ALWAYS set $31 = pc_of_branch+8, then test.
                // At this point psxRegs.pc was bumped to delay_slot+4 by loop,
                // but c is BCheck (first of triplet), so pc was just
                // delay_slot_pc. Wait no — the loop does pc+=4 for each inst
                // including BCheck, so after pc+=4 on BCheck, pc = delay_slot_pc.
                // delay_slot_pc = branch_pc + 4, so branch_pc + 8 = pc + 4.
                r[31] = psxRegs.pc + 4;
                s_iop_branch_taken = ((s32)r[c.src1] < 0) ? 1 : 0;
                s_iop_branch_target = (u32)c.imm;
                iopIsDelaySlot = true;
                break;
            case Op_BCheckGezLink:
                r[31] = psxRegs.pc + 4;
                s_iop_branch_taken = ((s32)r[c.src1] >= 0) ? 1 : 0;
                s_iop_branch_target = (u32)c.imm;
                iopIsDelaySlot = true;
                break;
            case Op_JumpConst:
                // J / JAL always taken. If JAL, link $31 = pc_of_branch+8 = pc+4.
                if (c.dest != 0)
                    r[c.dest] = psxRegs.pc + 4;
                s_iop_branch_taken = 1;
                s_iop_branch_target = (u32)c.imm;
                iopIsDelaySlot = true;
                break;
            case Op_BFinalize:
                // Clear delay-slot flag before pc commit.
                iopIsDelaySlot = false;
                psxRegs.pc = s_iop_branch_taken ? s_iop_branch_target : (u32)c.imm;
                iop_finalize_eventtest();
                iop_bfin_dump_periodic();
                goto after_block;  // Exit inner loop; outer while re-enters.

            default:
                // 安全側: 未知 op は fallback
                psxRegs.code = c.raw;
                if (c.handler) c.handler();
                break;
            }
            // GPR[0] は常に 0: writes to r0 は「if (!_Rt_) return;」で保護されるが
            // Native op は「if (rd != 0)」で compile 時に弾いているので r[0] は触らない。
        }
    after_block:;

        // [P8] Native branch path (BFinalize) already updated psxRegs.pc and
        // ran iopEventTest. Fallback branch path: psxBSC[raw>>26]() → doBranch()
        // already set psxRegs.pc and ran iopEventTest. Non-branch MAX_BLOCK_INSTS
        // exit: psxRegs.pc already advanced naturally. In all 3 cases the next
        // GetOrCompileBlock(psxRegs.pc) picks up where we are.

        // cycle 会計 (intExecuteBlock と一致)
        if ((psxHu32(HW_ICFG) & (1 << 3))) {
            const u32 cnum = 1280;
            const u32 cdenom = 147;
            const u32 t = ((cnum * (psxRegs.cycle - lastIOPCycle)) + psxRegs.iopCycleEECarry);
            psxRegs.iopCycleEE -= t / cdenom;
            psxRegs.iopCycleEECarry = t % cdenom;
        } else {
            psxRegs.iopCycleEE -= (psxRegs.cycle - lastIOPCycle) * 8;
        }
    }

    return psxRegs.iopBreak + psxRegs.iopCycleEE;
}

static void iop_Reserve() {}
static void iop_Reset()
{
    InvalidateAll();
}
static void iop_Clear(u32 /*Addr*/, u32 /*Size*/)
{
    // 保守的に全無効化
    //
    // [2026-08-06 P101-14] A range-aware version was implemented and MEASURED,
    // then reverted. Motivation looked strong: every IOP RAM store calls this
    // with a 1-word range (IopMem.cpp:349/392/507), so discarding the arguments
    // means the IOP block cache is wiped on essentially every store and never
    // stays warm. A code-page bitmap (4KB pages over the 2MB of IOP RAM, set by
    // CompileBlock, consulted here) made the common store skip the invalidate.
    //
    // Result: -4.08% VPS (savestate A/B, GTA VC 3D, alternating order, every
    // fixed run below every control run). The trade is simply the wrong way
    // round here — InvalidateAll() is almost free (a generation bump plus two
    // pool-pointer resets, ~6 instructions), while the bitmap test costs ~10-15
    // inlined instructions on EVERY IOP RAM store, and the recompilation it
    // avoids is only ~0.55% of the CPU thread (leaf profile 2026-08-06; the
    // 2.5% "block recompile" figure is mostly the EE side, ~1.94%).
    //
    // Do not re-propose for IOP without first making the check cheaper than the
    // invalidate. The EE side (iv2::Clear -> InvalidateAll + FlushTlbL0) is a
    // genuinely different case: there the invalidate is expensive because of the
    // TLB-L0 wipe, so separating those two is still worth measuring.
    InvalidateAll();
}
static void iop_Shutdown() {}

} // namespace iv2_iop

// 公開 R3000Acpu
R3000Acpu iv2PsxCpu = {
    iv2_iop::iop_Reserve,
    iv2_iop::iop_Reset,
    iv2_iop::ExecuteBlock,
    iv2_iop::iop_Clear,
    iv2_iop::iop_Shutdown
};
