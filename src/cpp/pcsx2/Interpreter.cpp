// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "VMManager.h"
#include "Elfheader.h"
#include "Cache.h"

#include "DebugTools/Breakpoints.h"

#include "common/FastJmp.h"

#include <float.h>
#include <cstdio>			// [EE event-test gate 2026-06-07] fprintf(stderr) diag 用

using namespace R5900;		// for OPCODE and OpcodeImpl

extern int vu0branch, vu1branch;

static int branch2 = 0;
// [V3 Step 5 backport 2026-05-04] Removed `static` so iR5900.cpp Step 5 runtime
// accumulator (armEmitFlushBlockCycles_runtime) can access via extern.
u32 cpuBlockCycles = 0;		// 3 bit fixed point version of cycle count

static std::string disOut;
static bool intExitExecution = false;
static fastjmp_buf intJmpBuf;
u32 intLastBranchTo = 0xFFFFFFFFu;   // [iv2 2026-05-30] un-static: iv2_exec.cpp extern 参照

void intEventTest();   // [iv2 2026-05-30] non-static: iv2_exec.cpp から extern 参照 (external linkage 化)
void intEventTestGated();   // [EE event-test gate 2026-06-07] iv2 native branch finalize 用 (gate 付き intEventTest)


void intUpdateCPUCycles()
{
	const bool lowcycles = (cpuBlockCycles <= 40);
	const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
	u32 scale_cycles = 0;

	if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
		scale_cycles = cpuBlockCycles >> 3;

	else if (cyclerate > 1)
		scale_cycles = cpuBlockCycles >> (2 + cyclerate);

	else if (cyclerate == 1)
		scale_cycles = (cpuBlockCycles >> 3) / 1.3f; // Adds a mild 30% increase in clockspeed for value 1.

	else if (cyclerate == -1) // the mildest value.
		// These values were manually tuned to yield mild speedup with high compatibility
		scale_cycles = (cpuBlockCycles <= 80 || cpuBlockCycles > 168 ? 5 : 7) * cpuBlockCycles / 32;

	else
		scale_cycles = ((5 + (-2 * (cyclerate + 1))) * cpuBlockCycles) >> 5;

	// Ensure block cycle count is never less than 1.
	cpuRegs.cycle += (scale_cycles < 1) ? 1 : scale_cycles;

	if (cyclerate > 1)
	{
		cpuBlockCycles &= (0x1 << (cyclerate + 2)) - 1;
	}
	else
	{
		cpuBlockCycles &= 0x7;
	}
}

// These macros are used to assemble the repassembler functions

void intBreakpoint(bool memcheck)
{
	const u32 pc = cpuRegs.pc;
 	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_EE, pc) != 0)
		return;

	if (!memcheck)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_EE, pc);
		if (cond && !cond->Evaluate())
			return;
	}

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_EE);
	VMManager::SetPaused(true);
	Cpu->ExitExecution();
}

void intMemcheck(u32 op, u32 bits, bool store)
{
	// compute accessed address
	u32 start = cpuRegs.GPR.r[(op >> 21) & 0x1F].UL[0];
	if (static_cast<s16>(op) != 0)
		start += static_cast<s16>(op);
	if (bits == 128)
		start &= ~0x0F;

	start = standardizeBreakpointAddress(start);
	const u32 end = start + bits/8;

	auto checks = CBreakPoints::GetMemChecks(BREAKPOINT_EE);
	for (size_t i = 0; i < checks.size(); i++)
	{
		auto& check = checks[i];

		if (check.result == 0)
			continue;
		if ((check.memCond & MEMCHECK_WRITE) == 0 && store)
			continue;
		if ((check.memCond & MEMCHECK_READ) == 0 && !store)
			continue;

		if (check.hasCond)
		{
			if (!check.cond.Evaluate())
				continue;
		}

		if (start < check.end && check.start < end)
			intBreakpoint(true);
	}
}

void intCheckMemcheck()
{
	const u32 pc = cpuRegs.pc;
	const int needed = isMemcheckNeeded(pc);
	if (needed == 0)
		return;

	const u32 op = memRead32(needed == 2 ? pc + 4 : pc);
	const OPCODE& opcode = GetInstruction(op);

	const bool store = (opcode.flags & IS_STORE) != 0;
	switch (opcode.flags & MEMTYPE_MASK)
	{
		case MEMTYPE_BYTE:
			intMemcheck(op, 8, store);
			break;
		case MEMTYPE_HALF:
			intMemcheck(op, 16, store);
			break;
		case MEMTYPE_WORD:
			intMemcheck(op, 32, store);
			break;
		case MEMTYPE_DWORD:
			intMemcheck(op, 64, store);
			break;
		case MEMTYPE_QWORD:
			intMemcheck(op, 128, store);
			break;
	}
}

static void execI()
{
	// execI is called for every instruction so it must remains as light as possible.
	// If you enable the next define, Interpreter will be much slower (around
	// ~4fps on 3.9GHz Haswell vs ~8fps (even 10fps on dev build))
	// Extra note: due to some cycle count issue PCSX2's internal debugger is
	// not yet usable with the interpreter
//#define EXTRA_DEBUG
#if defined(EXTRA_DEBUG) || defined(PCSX2_DEVBUILD)
	// check if any breakpoints or memchecks are triggered by this instruction
	if (isBreakpointNeeded(cpuRegs.pc))
		intBreakpoint(false);

	intCheckMemcheck();
#endif

	const u32 pc = cpuRegs.pc;

	// [P12] Per-instruction probes removed: @@INTERP_HB@@, @@INTERP_9FC40050@@,
	// @@INTERP_PC0@@, @@GUEST_PROBE@@, @@PANIC_LOOP@@, @@PS1DRV_ENTRY@@,
	// @@INTERP_81FC0@@  — caused massive interpreter slowdown.


	// We need to increase the pc before executing the memRead32. An exception could appears
	// and it expects the PC counter to be pre-incremented
	cpuRegs.pc += 4;

	// interprete instruction
	cpuRegs.code = memRead32( pc );

	const OPCODE& opcode = GetCurrentInstruction();

#if 0
	static long int runs = 0;
	//use this to find out what opcodes your game uses. very slow! (rama)
	runs++;
	 //leave some time to startup the testgame
	if (runs > 1599999999)
	{
		 //find all opcodes beginning with "L"
		if (opcode.Name[0] == 'L')
		{
			Console.WriteLn ("Load %s", opcode.Name);
		}
	}
#endif

#if 0
	static long int print_me = 0;
	// Based on cycle
	// if ( cpuRegs.cycle > 0x4f24d714 )
	// Or dump from a particular PC (useful to debug handler/syscall)
	if (pc == 0x80000000)
	{
		print_me = 2000;
	}
	if (print_me)
	{
		print_me--;
		disOut.clear();
		disR5900Fasm(disOut, cpuRegs.code, pc);
		CPU_LOG( disOut.c_str() );
	}
#endif


	cpuBlockCycles += opcode.cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));

	opcode.interpret();
}

static __fi void _doBranch_shared(u32 tar)
{
	// [TEMP_DIAG] @@DSLL_LOOP_INTERP@@ — log register state at DSLL loop target
	// Removal condition: ギャップcauseafter identified
	if (tar == 0x002659f0u) {
		static u32 s_il = 0;
		if (s_il < 20) {
			Console.WriteLn("@@DSLL_LOOP_INTERP@@ n=%u a0=%016llx a1=%016llx a2=%016llx v0=%016llx v1=%016llx pc=%08x cyc=%u",
				s_il,
				cpuRegs.GPR.r[4].UD[0], cpuRegs.GPR.r[5].UD[0],
				cpuRegs.GPR.r[6].UD[0], cpuRegs.GPR.r[2].UD[0],
				cpuRegs.GPR.r[3].UD[0], cpuRegs.pc, cpuRegs.cycle);
			s_il++;
		}
	}
	branch2 = cpuRegs.branch = 1;
	execI();

	// branch being 0 means an exception was thrown, since only the exception
	// handler should ever clear it.

	if( cpuRegs.branch != 0 )
	{
		// [iv2 fix 2026-05-30] iv2Cpu も interpreter 系 → WaitLoop double-flush (263 で 1 回目
		// flush) を有効化。Fallback branch (doBranch 経由) が iv2 で WaitLoop を skip し cycle
		// 乖離 → freeze していた (native branch は iv2BranchFinalize_Taken で対応済)。
		if (Cpu == &intCpu || Cpu == &iv2Cpu)
		{
			if (intLastBranchTo == tar && EmuConfig.Speedhacks.WaitLoop)
			{
				intUpdateCPUCycles();
				bool can_skip = true;
				if (tar != 0x81fc0)
				{
					if ((cpuRegs.pc - tar) < (4 * 10))
					{
						for (u32 i = tar; i < cpuRegs.pc; i += 4)
						{
							if (PSM(i) != 0)
							{
								can_skip = false;
								break;
							}
						}
					}
					else
						can_skip = false;
				}

				if (can_skip)
				{
					if (static_cast<s32>(cpuRegs.nextEventCycle - cpuRegs.cycle) > 0)
						cpuRegs.cycle = cpuRegs.nextEventCycle;
					else
						cpuRegs.nextEventCycle = cpuRegs.cycle;
				}
			}
		}
		intLastBranchTo = tar;
		cpuRegs.pc = tar;
		cpuRegs.branch = 0;
	}
}

static void doBranch( u32 target )
{
	_doBranch_shared( target );

	// [iter652] @@INTERP_BIOS_TRACE@@ — Interpreter の BIOS ROM 実行パスを記録。
	// JIT の @@BIOS_BLOCK_TRACE@@ と直接比較し、最初の分岐乖離を特定するため。
	// 各 BIOS PC は初回到達時のみ記録（loop反復をskip）。cap=500。
	// Removal condition: JIT vs Interpreter BIOS ROM 乖離のroot causeafter identified
	{
		static int s_ibt_n = 0;
		static u32 s_ibt_seen[512];
		static int s_ibt_seen_cnt = 0;
		const u32 pc = cpuRegs.pc;
		const u32 hw = pc & 0x1FFFFFFFu;
		if (hw >= 0x1FC00000u && hw < 0x20000000u && s_ibt_n < 500)
		{
			bool already_seen = false;
			for (int i = 0; i < s_ibt_seen_cnt; i++) {
				if (s_ibt_seen[i] == pc) { already_seen = true; break; }
			}
			if (!already_seen) {
				if (s_ibt_seen_cnt < 512) s_ibt_seen[s_ibt_seen_cnt++] = pc;
				Console.WriteLn("@@INTERP_BIOS_TRACE@@ n=%d startpc=%08x cyc=%u ra=%08x a0=%08x v0=%08x",
					s_ibt_n, pc, cpuRegs.cycle,
					cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.v0.UL[0]);
				s_ibt_n++;
			}
		}
	}

	intUpdateCPUCycles();
	intEventTest();
}

void intDoBranch(u32 target)
{
	//Console.WriteLn("Interpreter Branch ");
	_doBranch_shared( target );

	if( Cpu == &intCpu || Cpu == &iv2Cpu )   // [iv2 fix 2026-05-30] iv2 も flush+event
	{
		intUpdateCPUCycles();
		intEventTest();
	}
}

// [P101-14 E3 2026-08-06] WaitLoop スキャン部を cold 関数に outline (純粋なコード移動、挙動同一)。
// 動機 = 逆アセン実測: iv2BranchFinalize_Taken は **taken branch ごと** に呼ばれるのに
//   `sub sp,#0x90` (144B フレーム) + callee-saved 12 本の stp/ldp を無条件に積んでいた。
//   それらのレジスタを要求しているのは以下の稀な WaitLoop 経路 (同一 target への連続分岐時のみ)
//   だけで、ホットパス (pc/cycle 更新 + gated event test) は数本のレジスタしか要らない。
// **挙動は完全に同一** — 呼出順・読み書きの順序・cpuRegs.pc 参照タイミングを保存している
// (スキャンは cpuRegs.pc = target より前に走るので旧 pc を読む)。
static __attribute__((noinline, cold)) void iv2BranchWaitLoopSkip(u32 target)
{
	// [iv2 fix 2026-05-30] ★ intCpu _doBranch_shared:263 と同じ「1回目」flush ★。
	// WaitLoop branch では intCpu は intUpdateCPUCycles を 2 回呼ぶ (263 で flush →
	// can_skip 判定 → doBranch:327 で再 flush)。2 回目は cpuBlockCycles が 0-7 のため
	// min-1 rule で +1 加算。この 1 回目を欠落していたため WaitLoop loop 毎に iv2 が
	// intCpu より 1 cycle 少なく数え、IOP catchup ズレ → SIF poll loop 分岐 → freeze。
	intUpdateCPUCycles();
	bool can_skip = true;
	if (target != 0x81fc0)
	{
		if ((cpuRegs.pc - target) < (4 * 10))
		{
			for (u32 i = target; i < cpuRegs.pc; i += 4)
			{
				if (PSM(i) != 0)
				{
					can_skip = false;
					break;
				}
			}
		}
		else
			can_skip = false;
	}
	if (can_skip)
	{
		if (static_cast<s32>(cpuRegs.nextEventCycle - cpuRegs.cycle) > 0)
			cpuRegs.cycle = cpuRegs.nextEventCycle;
		else
			cpuRegs.nextEventCycle = cpuRegs.cycle;
	}
}

// [iv2 2026-05-30] iv2_exec.cpp の native branch path から呼ばれる taken-branch finalize。
// intCpu _doBranch_shared と同一の WaitLoop 最適化 + pc 更新 + cycle 更新 + event test。
extern "C" void iv2BranchFinalize_Taken(u32 target)
{
	if (intLastBranchTo == target && EmuConfig.Speedhacks.WaitLoop)
		iv2BranchWaitLoopSkip(target);
	intLastBranchTo = target;
	cpuRegs.pc = target;
	intUpdateCPUCycles();
	intEventTestGated();   // [EE event-test gate 2026-06-07] intUpdateCPUCycles は維持・event-test のみ gate
}

void intSetBranch()
{
	branch2 = /*cpuRegs.branch =*/ 1;
}

////////////////////////////////////////////////////////////////////
// R5900 Branching Instructions!
// These are the interpreter versions of the branch instructions.  Unlike other
// types of interpreter instructions which can be called safely from the recompilers,
// these instructions are not "recSafe" because they may not invoke the
// necessary branch test logic that the recs need to maintain sync with the
// cpuRegs.pc and delaySlot instruction and such.

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {

static bool IsInterpJumpProbeEnabled()
{
	static int s_cached = -1;
	if (s_cached < 0)
		s_cached = iPSX2_GetRuntimeEnvBool("iPSX2_INTERP_JUMP_PROBE", false) ? 1 : 0;
	return s_cached == 1;
}

static void LogInterpJumpProbe(const char* tag, u32 pc_now, u32 target, u32 link31, u32 code_now, u32 rs, u32 rs_val)
{
	if (!IsInterpJumpProbeEnabled())
		return;

	static bool s_cfg_logged = false;
	static int s_count = 0;
	constexpr int kCap = 50;
	if (!s_cfg_logged)
	{
		s_cfg_logged = true;
		Console.WriteLn("@@CFG@@ iPSX2_INTERP_JUMP_PROBE=1 iPSX2_INTERP_JUMP_PROBE_CAP=%d", kCap);
	}
	if (s_count >= kCap)
		return;

	Console.WriteLn(
		"@@INTERP_JUMP_PROBE@@ idx=%d tag=%s pc=%08x target=%08x ra=%08x code=%08x rs=%u rs_val=%08x",
		s_count, tag, pc_now, target, link31, code_now, rs, rs_val);
	s_count++;
}

/*********************************************************
* Jump to target                                         *
* Format:  OP target                                     *
*********************************************************/
// fixme: looking at the other branching code, shouldn't those _SetLinks in BGEZAL and such only be set
// if the condition is true? --arcum42

void J()
{
	doBranch(_JumpTarget_);
}

void JAL()
{
	// 0x3563b8 is the start address of the function that invalidate entry in TLB cache
	if (EmuConfig.Gamefixes.GoemonTlbHack) {
		if (_JumpTarget_ == 0x3563b8)
			GoemonUnloadTlb(cpuRegs.GPR.n.a0.UL[0]);
	}
	_SetLink(31);
	LogInterpJumpProbe("JAL", cpuRegs.pc, _JumpTarget_, cpuRegs.GPR.n.ra.UL[0], cpuRegs.code, 31, cpuRegs.GPR.n.ra.UL[0]);
	doBranch(_JumpTarget_);
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, rt, offset                             *
*********************************************************/

void BEQ()  // Branch if Rs == Rt
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] == cpuRegs.GPR.r[_Rt_].SD[0])
		doBranch(_BranchTarget_);
	else
		intEventTest();
}

void BNE()  // Branch if Rs != Rt
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] != cpuRegs.GPR.r[_Rt_].SD[0])
		doBranch(_BranchTarget_);
	else
		intEventTest();
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, offset                                 *
*********************************************************/

void BGEZ()    // Branch if Rs >= 0
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BGEZAL() // Branch if Rs >= 0 and link
{
	_SetLink(31);
	if (cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BGTZ()    // Branch if Rs >  0
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] > 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BLEZ()   // Branch if Rs <= 0
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] <= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BLTZ()    // Branch if Rs <  0
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
}

void BLTZAL()  // Branch if Rs <  0 and link
{
	_SetLink(31);
	if (cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
}

/*********************************************************
* Register branch logic  Likely                          *
* Format:  OP rs, offset                                 *
*********************************************************/


void BEQL()    // Branch if Rs == Rt
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] == cpuRegs.GPR.r[_Rt_].SD[0])
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BNEL()     // Branch if Rs != Rt
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] != cpuRegs.GPR.r[_Rt_].SD[0])
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BLEZL()    // Branch if Rs <= 0
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] <= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BGTZL()     // Branch if Rs >  0
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] > 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BLTZL()     // Branch if Rs <  0
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BGEZL()     // Branch if Rs >= 0
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BLTZALL()   // Branch if Rs <  0 and link
{
	_SetLink(31);
	if(cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void BGEZALL()   // Branch if Rs >= 0 and link
{
	_SetLink(31);
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

/*********************************************************
* Register jump                                          *
* Format:  OP rs, rd                                     *
*********************************************************/
void JR()
{
	// 0x33ad48 and 0x35060c are the return address of the function (0x356250) that populate the TLB cache
	if (EmuConfig.Gamefixes.GoemonTlbHack) {
		const u32 add = cpuRegs.GPR.r[_Rs_].UL[0];
		if (add == 0x33ad48 || add == 0x35060c)
			GoemonPreloadTlb();
	}
	LogInterpJumpProbe("JR", cpuRegs.pc, cpuRegs.GPR.r[_Rs_].UL[0], cpuRegs.GPR.n.ra.UL[0], cpuRegs.code, _Rs_, cpuRegs.GPR.r[_Rs_].UL[0]);
	// [P9 TEMP_DIAG] @@JR_ZERO_PRE/RET@@ - target saved before doBranch to avoid false positives
	const u32 jr_target = cpuRegs.GPR.r[_Rs_].UL[0];
	{
		static int s_jr0_pre = 0;
		if (s_jr0_pre < 10 && jr_target < 0x1000) {
			Console.WriteLn("@@JR_ZERO_PRE@@ #%d pc=%08x tgt=%08x cycle=%u nxt=%u",
				s_jr0_pre++, cpuRegs.pc, jr_target, cpuRegs.cycle, cpuRegs.nextEventCycle);
		}
	}
	doBranch(jr_target);
	// [P9 TEMP_DIAG] @@JR_ZERO_RET@@ - fires if doBranch returned (target was < 0x1000)
	{
		static int s_jr0_ret = 0;
		if (s_jr0_ret < 10 && jr_target < 0x1000) {
			Console.WriteLn("@@JR_ZERO_RET@@ #%d pc_after=%08x cycle=%u",
				s_jr0_ret++, cpuRegs.pc, cpuRegs.cycle);
		}
	}
}

void JALR()
{
	const u32 temp = cpuRegs.GPR.r[_Rs_].UL[0];

	if (_Rd_)  _SetLink(_Rd_);

	doBranch(temp);
}

} } }		// end namespace R5900::Interpreter::OpcodeImpl


// --------------------------------------------------------------------------------------
//  R5900cpu/intCpu interface (implementations)
// --------------------------------------------------------------------------------------

static void intReserve()
{
	// fixme : detect cpu for use the optimize asm code
}

static void intReset()
{
	cpuRegs.branch = 0;
	branch2 = 0;
}

void intEventTest()
{
	// [P9 TEMP_DIAG] @@INTET_ENTER@@ - log when cycle > 12000000 (critical vsync window), cap=20
	{
		static int s_et_hi = 0;
		if (s_et_hi < 20 && cpuRegs.cycle > 12735000u) {
			Console.WriteLn("@@INTET_ENTER@@ #%d pc=%08x cycle=%u nxt=%u exitReq=%d iop_pc=%08x",
				s_et_hi++, cpuRegs.pc, cpuRegs.cycle, cpuRegs.nextEventCycle,
				(int)intExitExecution, psxRegs.pc);
		}
	}
	// Perform counters, ints, and IOP updates:
	_cpuEventTest_Shared();

	// [P9 TEMP_DIAG] @@INTET_AFTER@@ - log after _cpuEventTest_Shared returns (same window)
	{
		static int s_et_after = 0;
		if (s_et_after < 20 && cpuRegs.cycle > 12735000u) {
			Console.WriteLn("@@INTET_AFTER@@ #%d pc=%08x cycle=%u nxt=%u exitReq=%d iop_pc=%08x",
				s_et_after++, cpuRegs.pc, cpuRegs.cycle, cpuRegs.nextEventCycle,
				(int)intExitExecution, psxRegs.pc);
		}
	}

	if (intExitExecution)
	{
		intExitExecution = false;
		if (CHECK_EEREC)
			writebackCache();
		fastjmp_jmp(&intJmpBuf, 1);
	}
}

// ───────────────────────────────────────────────────────────────
// [EE event-test cadence gate 2026-06-07] EEIOP 計画 Phase 4。
// iv2 は全 native branch finalize で intEventTest() を無条件発火する (interpreter 由来の
// over-eager cadence)。JIT/canonical の本来の cadence は cpuRegs.cycle が nextEventCycle に
// 達した時のみ event-test する: _cpuEventTest_Shared 自身が nextEventCycle = cycle + eeWaitCycles
// を毎回設定 (R5900.cpp:945) し、scheduled interrupt が前倒しするため horizon は有界 → 必ず収束。
// gate skip = deferral (loss でない): EE→IOP catch-up は EEsCycle += cycle - EEoCycle を積み
// psxCpu->ExecuteBlock でまとめ処理 (R5900.cpp:1006,1037)、rcntUpdate/_cpuTestTIMR も cycle ベースで
// 次回発火時に追いつく。IOP の実証済 gate (iv2_iop_exec.cpp:57, iPSX2_IOP_BFIN_GATE, 99.4% skip) と同型。
// intUpdateCPUCycles は呼出側で常に維持。
// ★ 2026-06-07 default-ON 昇格: GTA VC real VPS +29.2% / det +27.5% / GIF 2,000,000 packet
//   bit-identical (mismatch=0) / sample EE_eventtest 12.6→3.0% / FF10 +5.5% (heavy-3D)。
//   user 視覚確認 (FF10 + GTA VC、freeze/描画崩れなし) 済 → 既定 ON。
//   kill-switch = env iPSX2_EE_BFIN_GATE=0 で OFF (= legacy bit-exact cadence、退行調査用)。
//   詳細 = docs/report/ee_event_test_gate_2026-06-07.md。
// ───────────────────────────────────────────────────────────────
static const bool s_ee_bfin_gate = []() {
	const char* e = std::getenv("iPSX2_EE_BFIN_GATE");
	return !(e && *e == '0');   // ★ default ON。iPSX2_EE_BFIN_GATE=0 のみ OFF (legacy bit-exact cadence)
}();
static const bool s_ee_bfin_diag = []() {
	const char* e = std::getenv("iPSX2_EE_NATIVE_BRANCH_DIAG");
	return e && *e == '1';
}();
static u64 s_ee_bfin_called = 0;
static u64 s_ee_bfin_skip = 0;
static u64 s_ee_bfin_fired = 0;

void intEventTestGated()
{
	if (__builtin_expect(s_ee_bfin_diag, 0)) {
		s_ee_bfin_called++;
		if ((s_ee_bfin_called & 0xFFFFFu) == 0)
			fprintf(stderr, "@@EE_BFIN_GATE@@ called=%llu fired=%llu skip=%llu skip_pct=%.2f gate=%d\n",
				(unsigned long long)s_ee_bfin_called, (unsigned long long)s_ee_bfin_fired,
				(unsigned long long)s_ee_bfin_skip,
				100.0 * (double)s_ee_bfin_skip / (double)(s_ee_bfin_called ? s_ee_bfin_called : 1),
				(int)s_ee_bfin_gate);
	}
	if (s_ee_bfin_gate && (s32)(cpuRegs.cycle - cpuRegs.nextEventCycle) < 0) {
		if (__builtin_expect(s_ee_bfin_diag, 0)) s_ee_bfin_skip++;
		return;
	}
	if (__builtin_expect(s_ee_bfin_diag, 0)) s_ee_bfin_fired++;
	intEventTest();
}

static void intSafeExitExecution()
{
	// [P9 TEMP_DIAG] @@INTERP_SAFE_EXIT@@ - log why/where intExecute is exiting
	{
		static int s_exit_count = 0;
		if (s_exit_count < 20) {
			Console.WriteLn("@@INTERP_SAFE_EXIT@@ #%d pc=%08x eeEventTestIsActive=%d intExitExecution=%d cycle=%u",
				s_exit_count, cpuRegs.pc, (int)eeEventTestIsActive, (int)intExitExecution, cpuRegs.cycle);
			s_exit_count++;
		}
	}
	// If we're currently processing events, we can't safely jump out of the interpreter here, because we'll
	// leave things in an inconsistent state. So instead, we flag it for exiting once cpuEventTest() returns.
	if (eeEventTestIsActive)
		intExitExecution = true;
	else
	{
		if (CHECK_EEREC)
			writebackCache();
		fastjmp_jmp(&intJmpBuf, 1);
	}
}

static void intCancelInstruction()
{
	// See execute function.
	fastjmp_jmp(&intJmpBuf, 0);
}

static void intExecute()
{
	// [P9 TEMP_DIAG] @@INTERP_EXEC_ENTRY@@ - count how many times intExecute() is called
	{
		static int s_entry_count = 0;
		if (s_entry_count < 20)
			Console.WriteLn("@@INTERP_EXEC_ENTRY@@ #%d booted=%d pc=%08x", s_entry_count++,
				(int)VMManager::Internal::HasBootedELF(), cpuRegs.pc);
	}

	// This will come back as zero the first time it runs, or on instruction cancel.
	// It will come back as nonzero when we exit execution.
	if (fastjmp_set(&intJmpBuf) != 0) {
		// [P9 TEMP_DIAG] @@INTERP_JMP_RETURN@@ - fastjmp_jmp returned nonzero: intExecute exiting
		static int s_jmp_count = 0;
		if (s_jmp_count < 20)
			Console.WriteLn("@@INTERP_JMP_RETURN@@ #%d pc=%08x cycle=%u", s_jmp_count++, cpuRegs.pc, cpuRegs.cycle);
		return;
	}

	for (;;)
	{
		if (!VMManager::Internal::HasBootedELF())
		{
			// Avoid reloading every instruction.
			u32 elf_entry_point = VMManager::Internal::GetCurrentELFEntryPoint();
			u32 eeload_main = g_eeloadMain;
			u32 eeload_exec = g_eeloadExec;

			while (true)
			{
				// [iter_DB78_INTERP] @@INTERP_DB78@@ – Interpreter が 0x8000db78 到達時のregister
				// 目的: JIT a0=0x02000000 vs Interpreter a0=? の差分特定 (cycle>25M以降のみ)
				// Removal condition: EELOAD entry (0x82000 vs 0x82180) divergence root cause after determined
				{
					static int s_db78_n = 0;
					const u32 ipc = cpuRegs.pc;
					// cycle>25M = wait loop exit 後。早期アクセスはskip
					if (s_db78_n < 20 && cpuRegs.cycle > 25000000u && ipc >= 0x8000db00u && ipc <= 0x8000de00u) {
						const u32 elf_base  = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82000) : 0u;
						const u32 elf_entry = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82018) : 0u;
						Console.WriteLn("@@INTERP_DB78@@ n=%d pc=%08x cycle=%u a0=%08x v0=%08x v1=%08x t0=%08x t1=%08x ra=%08x s0=%08x elf[82000]=%08x elf[82018]=%08x",
							s_db78_n, ipc, cpuRegs.cycle,
							cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
							cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.ra.UL[0],
							cpuRegs.GPR.r[16].UL[0], elf_base, elf_entry);
						s_db78_n++;
					}
				}

				// [iter_EF18_INTERP] @@INTERP_EF18@@
				{
					static int s_ef18_n = 0;
					const u32 ipc2 = cpuRegs.pc;
					if (s_ef18_n < 5 && cpuRegs.cycle > 27000000u && ipc2 >= 0x8000ef00u && ipc2 <= 0x8000f100u) {
						const u32 elf81fc0 = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x81fc0) : 0u;
						const u32 mem1A64C = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x1A64C) : 0u;
						Console.WriteLn("@@INTERP_EF18@@ n=%d pc=%08x cycle=%u a0=%08x a1=%08x v0=%08x ra=%08x elf81fc0=%08x mem[1A64C]=%08x",
							s_ef18_n, ipc2, cpuRegs.cycle,
							cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
							cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.ra.UL[0],
							elf81fc0, mem1A64C);
						s_ef18_n++;
					}
				}

				// [iter_5160_INTERP] @@INTERP_5160@@ – 0x80005160-0x80005188 (ExecEELOAD) a0 verify
				// Removal condition: SYSCALL divergence root cause after determined
				{
					static int s_5160_n = 0;
					const u32 ipc5 = cpuRegs.pc;
					if (s_5160_n < 20 && ipc5 >= 0x80005160u && ipc5 <= 0x80005188u) {
						const u32 cop0_epc = cpuRegs.CP0.n.EPC;
						const u32 mem1a698 = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x1a698) : 0u;
						Console.WriteLn("@@INTERP_5160@@ n=%d pc=%08x cycle=%u a0=%08x v0=%08x ra=%08x COP0_EPC=%08x mem[1a698]=%08x",
							s_5160_n, ipc5, cpuRegs.cycle,
							cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.v0.UL[0],
							cpuRegs.GPR.n.ra.UL[0], cop0_epc, mem1a698);
						s_5160_n++;
					}
				}
				// [iter_0C40_INTERP] @@INTERP_0C40@@ – 0x80000C30-0x80000CA0 到達verify (JIT戻り値差分cause調査)
			// 目的: 0x80000C40 の戻り値 v0 が Interpreter では 0x81fc0 → 比較
			// Removal condition: 戻り値差分 root cause after determined
			{
				static int s_0c40_n = 0;
				const u32 ipc_c = cpuRegs.pc;
				if (s_0c40_n < 20 && ipc_c >= 0x80000c30u && ipc_c <= 0x80000ca0u) {
					Console.WriteLn("@@INTERP_0C40@@ n=%d pc=%08x cycle=%u v0=%08x a0=%08x a1=%08x ra=%08x",
						s_0c40_n, ipc_c, cpuRegs.cycle,
						cpuRegs.GPR.n.v0.UL[0],
						cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
						cpuRegs.GPR.n.ra.UL[0]);
					s_0c40_n++;
				}
			}

			// [iter_2C40_INTERP] @@INTERP_2C40_DUMP@@ – Interpreter が 0x80002C40 実行時の eeMem ダンプ
			// 目的: JIT_2C40_DUMP との比較により BIOS 自己書き換えの有無verify
			// Removal condition: a0 差分の root cause after determined
			{
				static bool s_2c40_idumped = false;
				if (!s_2c40_idumped && cpuRegs.pc == 0x80002c40u && eeMem) {
					s_2c40_idumped = true;
					const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x00002c30);
					Console.WriteLn("@@INTERP_2C40_DUMP@@ pc=%08x cycle=%u a0=%08x a1=%08x ra=%08x",
						cpuRegs.pc, cpuRegs.cycle,
						cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0], cpuRegs.GPR.n.ra.UL[0]);
					for (int j = 0; j < 32; j += 8)
						Console.WriteLn("@@INTERP_2C40_CODE@@ [%04x]: %08x %08x %08x %08x %08x %08x %08x %08x",
							0x2c30 + j*4,
							c[j],c[j+1],c[j+2],c[j+3],c[j+4],c[j+5],c[j+6],c[j+7]);
				}
			}

			// [iter_54C4_INTERP] @@INTERP_54C4@@ – 0x800054c0-0x800054d0 実行時 a1/s0/s4 キャプチャ (0x8000d8a8 call直前)
			// 目的: JAL 0x8000d8a8 に渡る a1 の実際値をverifyし、s4 が変化したかdetermine
			// Removal condition: a1 差分の root cause after determined
			{
				static int s_54c4_n = 0;
				const u32 pc54 = cpuRegs.pc;
				if (s_54c4_n < 10 && pc54 >= 0x800054c0u && pc54 <= 0x800054d0u) {
					Console.WriteLn("@@INTERP_54C4@@ n=%d pc=%08x cycle=%u a0=%08x a1=%08x s0=%08x s4=%08x ra=%08x",
						s_54c4_n, pc54, cpuRegs.cycle,
						cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
						cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s4.UL[0],
						cpuRegs.GPR.n.ra.UL[0]);
					s_54c4_n++;
				}
			}

			// [iter_5388_INTERP] @@INTERP_5388_DUMP@@ – Interpreter が 0x80005388 実行時のmemory内容取得
			// 目的: JIT compile時 (cycle=27138998) と Interpreter 実行時の eeMem[5380] 差分verify → 自己書き換え検証
			// Removal condition: a1=0x82000→0x81fc0 差分 root cause after determined
			{
				static bool s_5388_idumped = false;
				if (!s_5388_idumped && cpuRegs.pc == 0x80005388u && eeMem) {
					s_5388_idumped = true;
					const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x00005380);
					Console.WriteLn("@@INTERP_5388_DUMP@@ pc=%08x cycle=%u", cpuRegs.pc, cpuRegs.cycle);
					// 0x80005380-0x8000557f (128 words = 16 rows) to cover code divergence at [5500-5580]
					for (int j = 0; j < 128; j += 8)
						Console.WriteLn("@@INTERP_5388_CODE@@ [%04x]: %08x %08x %08x %08x %08x %08x %08x %08x",
							0x5380 + j*4,
							c[j],c[j+1],c[j+2],c[j+3],c[j+4],c[j+5],c[j+6],c[j+7]);
				}
			}

			// [iter_5508_INTERP] @@INTERP_5508@@ – 0x80005500-0x80005530 到達verify (JIT書き込み元 pc=0x80005508)
			// 目的: Interpreter が 0x80005508 に到達するかverify + a1 値 (コピー先) 比較
			// Removal condition: 書き込み元 PC/path after determined
			{
				static int s_5508_n = 0;
				const u32 ipc_5 = cpuRegs.pc;
				if (s_5508_n < 20 && ipc_5 >= 0x80005500u && ipc_5 <= 0x80005530u) {
					const u32 mem82000v = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82000) : 0u;
					Console.WriteLn("@@INTERP_5508@@ n=%d pc=%08x cycle=%u v0=%08x a0=%08x a1=%08x ra=%08x mem[82000]=%08x",
						s_5508_n, ipc_5, cpuRegs.cycle,
						cpuRegs.GPR.n.v0.UL[0],
						cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
						cpuRegs.GPR.n.ra.UL[0], mem82000v);
					s_5508_n++;
				}
			}

			// [iter_10B8_INTERP] @@INTERP_10B8@@ – 0x80001000-0x800010FF 到達verify (JIT MEM82000書き込み元付近)
			// 目的: Interpreter が JIT の書き込み元 startpc=0x800010b8 付近に到達するかverify
			// Removal condition: 書き込み元 PC after determined
			{
				static int s_10b8_n = 0;
				const u32 ipc_b = cpuRegs.pc;
				if (s_10b8_n < 60 && ipc_b >= 0x80001000u && ipc_b <= 0x800011ffu) {
					Console.WriteLn("@@INTERP_10B8@@ n=%d pc=%08x cycle=%u v0=%08x a0=%08x a1=%08x ra=%08x",
						s_10b8_n, ipc_b, cpuRegs.cycle,
						cpuRegs.GPR.n.v0.UL[0],
						cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
						cpuRegs.GPR.n.ra.UL[0]);
					s_10b8_n++;
				}
			}

			// [iter_MEM82000_INTERP] @@INTERP_MEM82000_CHANGE@@ – eeMem[82000] 変化detect
			// 目的: Interpreter でも同じ書き込みがoccurするかverify (JIT は startpc=800010b8, cycle=27302034 でdetect)
			// Removal condition: 書き込み元 PC after determined
			{
				static u32 s_im82000_prev = 0u;
				static int s_im82000_n = 0;
				if (eeMem && s_im82000_n < 10) {
					const u32 cur = *reinterpret_cast<const u32*>(eeMem->Main + 0x82000);
					if (cur != s_im82000_prev) {
						Console.WriteLn("@@INTERP_MEM82000_CHANGE@@ n=%d pc=%08x cycle=%u OLD=%08x NEW=%08x v0=%08x a0=%08x a1=%08x ra=%08x",
							s_im82000_n, cpuRegs.pc, cpuRegs.cycle,
							s_im82000_prev, cur,
							cpuRegs.GPR.n.v0.UL[0],
							cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
							cpuRegs.GPR.n.ra.UL[0]);
						s_im82000_prev = cur;
						s_im82000_n++;
					}
				}
			}

			// [iter_SYSCALL_INTERP] @@INTERP_SYSCALL_1564@@ – SYSCALL例外後のEPC追跡
				// 目的: (A) 0x82000-0x8200f での EPC/mem verify; (B) 例外ベクタ 0x80000180-0x80000210 での EPC verify
				// JIT: EPC=0x8200c(delay slot) → 0x80001564 fail。Interpreter の EPC 差分を特定
				// Removal condition: SYSCALL EPC divergence after determined
				{
					static int s_sc_n = 0;
					const u32 ipc_sc = cpuRegs.pc;
					// Range A: 0x82000-0x82010 (module header execution in user space)
					const bool in_82000 = (ipc_sc >= 0x00082000u && ipc_sc <= 0x00082010u);
					// Range B: exception vector area
					const bool in_excvec = (ipc_sc >= 0x80000180u && ipc_sc <= 0x80000220u);
					// Range C: 0x80001550-0x80001590 (original range)
					const bool in_1564 = (ipc_sc >= 0x80001550u && ipc_sc <= 0x80001590u);
					if (s_sc_n < 40 && (in_82000 || in_excvec || in_1564)) {
						const u32 cop0_epc_sc = cpuRegs.CP0.n.EPC;
						const u32 cop0_cause = cpuRegs.CP0.n.Cause;
						const u32 mem82000 = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82000) : 0u;
						Console.WriteLn("@@INTERP_SYSCALL@@ n=%d pc=%08x cycle=%u a0=%08x v0=%08x ra=%08x EPC=%08x Cause=%08x mem[82000]=%08x",
							s_sc_n, ipc_sc, cpuRegs.cycle,
							cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.v0.UL[0],
							cpuRegs.GPR.n.ra.UL[0],
							cop0_epc_sc, cop0_cause, mem82000);
						s_sc_n++;
					}
				}

				execI();

				// [P12 TEMP_DIAG] @@INTERP_9FC41268@@ + @@INTERP_9FC411C0@@ one-shot
				// 目的: 0x9FC41268 (フルbootfunction) への到達と 0x9FC411C0 (BGEZ return check) のverify
				// Removal condition: JIT vs Interpreter 分岐点が 0x9FC41268 内に確定した後
				{
					static bool s_9fc41268_fired = false;
					static bool s_9fc411c0_fired = false;
					const u32 ipc = cpuRegs.pc;
					if (!s_9fc41268_fired && (ipc == 0x9FC41268u || ipc == 0xBFC41268u)) {
						s_9fc41268_fired = true;
						Console.WriteLn("@@INTERP_9FC41268@@ pc=%08x v0=%08x a0=%08x ra=%08x sp=%08x",
							ipc,
							cpuRegs.GPR.r[2].UL[0],
							cpuRegs.GPR.r[4].UL[0],
							cpuRegs.GPR.r[31].UL[0],
							cpuRegs.GPR.r[29].UL[0]);
					}
					if (!s_9fc411c0_fired && (ipc == 0x9FC411C0u || ipc == 0xBFC411C0u)) {
						s_9fc411c0_fired = true;
						const s32 v0s = (s32)cpuRegs.GPR.r[2].UL[0];
						Console.WriteLn("@@INTERP_9FC411C0@@ pc=%08x v0=%08x (%s)",
							ipc,
							cpuRegs.GPR.r[2].UL[0],
							v0s >= 0 ? "BGEZ taken->success" : "BGEZ NOT taken->error");
					}
				}

				// [P12 TEMP_DIAG] @@INTERP_EERAM_FIRST@@ one-shot: first entry into EE RAM
				// Counterpart of @@EERAM_FIRST_RECOMP@@ in recRecompile(); used for JIT vs Interp comparison.
				// Remove when: JIT vs Interp entry-point divergence root cause is confirmed.
				{
					static bool s_interp_eeram_fired = false;
					const u32 new_pc = cpuRegs.pc;
					if (!s_interp_eeram_fired && new_pc >= 0x80000000u && new_pc < 0x9FC00000u) {
						s_interp_eeram_fired = true;
						Console.WriteLn("@@INTERP_EERAM_FIRST@@ pc=%08x", new_pc);
						for (int i = 0; i < 32; i++)
							Console.WriteLn("  r%02d=%08x", i, cpuRegs.GPR.r[i].UL[0]);
						Console.WriteLn("@@COP0_DUMP_INTERP@@ status=%08x epc=%08x cause=%08x",
							cpuRegs.CP0.r[12], cpuRegs.CP0.r[14], cpuRegs.CP0.r[13]);
					}
				}

				if (cpuRegs.pc == EELOAD_START)
				{
					// The EELOAD _start function is the same across all BIOS versions afaik
					const u32 mainjump = memRead32(EELOAD_START + 0x9c);
					if (mainjump >> 26 == 3) // JAL
						g_eeloadMain = ((EELOAD_START + 0xa0) & 0xf0000000U) | (mainjump << 2 & 0x0fffffffU);

					eeload_main = g_eeloadMain;
				}
				else if (eeload_main != 0u && cpuRegs.pc == eeload_main)
				{
					eeloadHook();
					if (VMManager::Internal::IsFastBootInProgress())
					{
						// See comments on this code in iR5900.cpp's recRecompile()
						const u32 typeAexecjump = memRead32(EELOAD_START + 0x470);
						const u32 typeBexecjump = memRead32(EELOAD_START + 0x5B0);
						const u32 typeCexecjump = memRead32(EELOAD_START + 0x618);
						const u32 typeDexecjump = memRead32(EELOAD_START + 0x600);
						if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) || (typeDexecjump >> 26 == 3)) // JAL to 0x822B8
							g_eeloadExec = EELOAD_START + 0x2B8;
						else if (typeAexecjump >> 26 == 3) // JAL to 0x82170
							g_eeloadExec = EELOAD_START + 0x170;
						else
							Console.WriteLn("intExecute: Could not enable launch arguments for fast boot mode; unidentified BIOS version! Please report this to the PCSX2 developers.");

						eeload_exec = g_eeloadExec;
					}

					elf_entry_point = VMManager::Internal::GetCurrentELFEntryPoint();
				}
				else if (eeload_exec != 0u && cpuRegs.pc == eeload_exec)
				{
					eeloadHook2();
				}
				else if (cpuRegs.pc == elf_entry_point)
				{
					VMManager::Internal::EntryPointCompilingOnCPUThread();
					break;
				}
			}
		}
		else
		{
			while (true)
				execI();
		}
	}
}

static void intStep()
{
	execI();
}

static void intClear(u32 Addr, u32 Size)
{
}

static void intShutdown() {
}

// [R102] Single-step interpreter for JIT fallback diagnostics
void interpExecOneInstruction()
{
    execI();
}

extern "C" void interpExecBiosBlock()
{
    // Execute instructions until a branch or event check is needed
    // This runs one basic block worth of instructions via interpreter
    const u32 startpc = cpuRegs.pc;
    int count = 0;
    const int max_insns = 128;  // safety limit per block

    while (count < max_insns)
    {
        execI();
        count++;

        // Check if PC has changed significantly (branch taken) or if we need to handle events
        if (cpuRegs.pc < startpc || cpuRegs.pc >= startpc + (u32)(count * 4) + 8)
            break;  // branch was taken

        // Check for pending events
        if ((int)(cpuRegs.nextEventCycle - cpuRegs.cycle) <= 0)
            break;
    }
}

R5900cpu intCpu =
{
	intReserve,
	intShutdown,

	intReset,
	intStep,
	intExecute,

	intSafeExitExecution,
	intCancelInstruction,

	intClear
};
