// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "R3000A.h"
#include "Common.h"

#include "SIO/Sio0.h"
#include "Sif.h"
#include "DebugTools/Breakpoints.h"
#include "R5900OpcodeTables.h"
#include "IopCounters.h"
#include "IopBios.h"
#include "IopHw.h"
#include "IopDma.h"
#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"

using namespace R3000A;

// [iv2 accel Phase 0 2026-06-07] IOP event-test / reset 診断 probe の compile-gate。
// default 0 = release で完全除去。中身は read-only 診断 (Console 出力 + 静的 counter) のみで
// emulation 状態を変えない (logic-preserving)。iopEventTest は IOP の全 branch 境界で
// 呼ばれるため per-call の probe 比較群を release で消す。退行調査時は -DIPSX2_IOP_EVT_DIAG=1。
#ifndef IPSX2_IOP_EVT_DIAG
#define IPSX2_IOP_EVT_DIAG 0
#endif

R3000Acpu *psxCpu;

// used for constant propagation
u32 g_psxConstRegs[32];
u32 g_psxHasConstReg, g_psxFlushedConstReg;

// Used to signal to the EE when important actions that need IOP-attention have
// happened (hsyncs, vsyncs, IOP exceptions, etc).  IOP runs code whenever this
// is true, even if it's already running ahead a bit.
bool iopEventAction = false;

static constexpr uint iopWaitCycles = 384; // Keep inline with EE wait cycle max.

bool iopEventTestIsActive = false;

//alignas(16) psxRegisters psxRegs;

void psxReset()
{
	// [TEMP_DIAG] IOP reboot detection — Removal condition: IOP reboot issue解決後
	static int s_psxreset_n = 0;
	Console.WriteLn("@@PSX_RESET@@ n=%d caller_cycle=%u", s_psxreset_n++, psxRegs.cycle);
	std::memset(&psxRegs, 0, sizeof(psxRegs));

	psxRegs.pc = 0xbfc00000; // Start in bootstrap
	psxRegs.CP0.n.Status = 0x00400000; // BEV = 1
	psxRegs.CP0.n.PRid   = 0x0000001f; // PRevID = Revision ID, same as the IOP R3000A

	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = -1;
	psxRegs.iopCycleEECarry = 0;
	psxRegs.iopNextEventCycle = psxRegs.cycle + 4;

	psxHwReset();
	PSXCLK = 36864000;
	ioman::reset();
	psxBiosReset();
}

void psxShutdown() {
	//psxCpu->Shutdown();
}

void psxException(u32 code, u32 bd)
{
//	PSXCPU_LOG("psxException %x: %x, %x", code, psxHu32(0x1070), psxHu32(0x1074));
	//Console.WriteLn("!! psxException %x: %x, %x", code, psxHu32(0x1070), psxHu32(0x1074));

	// [P9 TEMP_DIAG] @@PSX_EXCEPTION@@ — capture first 30 + late-stage exceptions
	{
		static int s_exc_count = 0;
		static int s_exc_late = 0;
		if (s_exc_count < 30) {
			s_exc_count++;
			Console.WriteLn("@@PSX_EXCEPTION@@ #%d code=%08x bd=%d epc=%08x sp=%08x ra=%08x sr=%08x",
				s_exc_count, code, bd, psxRegs.pc,
				psxRegs.GPR.n.sp, psxRegs.GPR.n.ra, psxRegs.CP0.n.Status);
		}
		// [P12] @@PSX_EXCEPTION_LATE@@ — cyc=35M-37M の例外を捕捉 (IEc永続=0 直前)
		if (psxRegs.cycle > 35000000u && psxRegs.cycle < 37000000u && s_exc_late < 30) {
			s_exc_late++;
			Console.WriteLn("@@PSX_EXCEPTION_LATE@@ #%d code=%08x bd=%d epc=%08x pc=%08x sp=%08x ra=%08x sr=%08x istat=%04x imask=%04x cyc=%u",
				s_exc_late, code, bd, psxRegs.pc,
				psxRegs.pc, psxRegs.GPR.n.sp, psxRegs.GPR.n.ra, psxRegs.CP0.n.Status,
				psxHu32(0x1070), psxHu32(0x1074), psxRegs.cycle);
		}
	}
	// Set the Cause
	psxRegs.CP0.n.Cause &= ~0x7f;
	psxRegs.CP0.n.Cause |= code;

	// Set the EPC & PC
	if (bd)
	{
		PSXCPU_LOG("bd set");
		psxRegs.CP0.n.Cause|= 0x80000000;
		psxRegs.CP0.n.EPC = (psxRegs.pc - 4);
	}
	else
		psxRegs.CP0.n.EPC = (psxRegs.pc);

	if (psxRegs.CP0.n.Status & 0x400000)
		psxRegs.pc = 0xbfc00180;
	else
		psxRegs.pc = 0x80000080;

	// Set the Status
	psxRegs.CP0.n.Status = (psxRegs.CP0.n.Status &~0x3f) |
						  ((psxRegs.CP0.n.Status & 0xf) << 2);

	/*if ((((PSXMu32(psxRegs.CP0.n.EPC) >> 24) & 0xfe) == 0x4a)) {
		// "hokuto no ken" / "Crash Bandicot 2" ... fix
		PSXMu32(psxRegs.CP0.n.EPC)&= ~0x02000000;
	}*/

	/*if (psxRegs.CP0.n.Cause == 0x400 && (!(psxHu32(0x1450) & 0x8))) {
		hwIntcIrq(INTC_SBUS);
	}*/
}

__fi void psxSetNextBranch( u32 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't blow up
	// if startCycle is greater than our next branch cycle.

	if( (int)(psxRegs.iopNextEventCycle - startCycle) > delta )
		psxRegs.iopNextEventCycle = startCycle + delta;
}

__fi void psxSetNextBranchDelta( s32 delta )
{
	psxSetNextBranch( psxRegs.cycle, delta );
}

__fi int psxTestCycle( u32 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't explode
	// if the startCycle is ahead of our current cpu cycle.

	return (int)(psxRegs.cycle - startCycle) >= delta;
}

__fi int psxRemainingCycles(IopEventId n)
{
	if (psxRegs.interrupt & (1 << n))
		return ((psxRegs.cycle - psxRegs.sCycle[n]) + psxRegs.eCycle[n]);
	else
		return 0;
}

__fi void PSX_INT( IopEventId n, s32 ecycle )
{
	// 19 is CDVD read int, it's supposed to be high.
	//if (ecycle > 8192 && n != 19)
	//	DevCon.Warning( "IOP cycles high: %d, n %d", ecycle, n );

	psxRegs.interrupt |= 1 << n;

	psxRegs.sCycle[n] = psxRegs.cycle;
	psxRegs.eCycle[n] = ecycle;

	psxSetNextBranchDelta(ecycle);

	// [P33] Android と同じ固定 8x 比を使用。float 計算は PSXCLK change時に不正確になる。
	const s32 iopDelta = (psxRegs.iopNextEventCycle - psxRegs.cycle) << 3; // cycle * 8

	if (psxRegs.iopCycleEE < iopDelta)
	{
		// The EE called this int, so inform it to branch as needed:
		
		cpuSetNextEventDelta(iopDelta - psxRegs.iopCycleEE);
	}
}

static __fi void IopTestEvent( IopEventId n, void (*callback)() )
{
	if( !(psxRegs.interrupt & (1 << n)) ) return;

	if( psxTestCycle( psxRegs.sCycle[n], psxRegs.eCycle[n] ) )
	{
		psxRegs.interrupt &= ~(1 << n);
		callback();
	}
	else
		psxSetNextBranch( psxRegs.sCycle[n], psxRegs.eCycle[n] );
}

static __fi void Sio0TestEvent(IopEventId n)
{
	if (!(psxRegs.interrupt & (1 << n)))
	{
		return;
	}

	if (psxTestCycle(psxRegs.sCycle[n], psxRegs.eCycle[n]))
	{
		psxRegs.interrupt &= ~(1 << n);
		g_Sio0.Interrupt(Sio0Interrupt::TEST_EVENT);
	}
	else
	{
		psxSetNextBranch(psxRegs.sCycle[n], psxRegs.eCycle[n]);
	}
}

static __fi void _psxTestInterrupts()
{
	IopTestEvent(IopEvt_SIF0,		sif0Interrupt);	// SIF0
	IopTestEvent(IopEvt_SIF1,		sif1Interrupt);	// SIF1
	IopTestEvent(IopEvt_SIF2,		sif2Interrupt);	// SIF2
	Sio0TestEvent(IopEvt_SIO);
	IopTestEvent(IopEvt_CdvdSectorReady, cdvdSectorReady);
	IopTestEvent(IopEvt_CdvdRead,	cdvdReadInterrupt);

	// Profile-guided Optimization (sorta)
	// The following ints are rarely called.  Encasing them in a conditional
	// as follows helps speed up most games.

	if( psxRegs.interrupt & ((1 << IopEvt_Cdvd) | (1 << IopEvt_Dma11) | (1 << IopEvt_Dma12)
		| (1 << IopEvt_Cdrom) | (1 << IopEvt_CdromRead) | (1 << IopEvt_DEV9) | (1 << IopEvt_USB)))
	{
		IopTestEvent(IopEvt_Cdvd,		cdvdActionInterrupt);
		IopTestEvent(IopEvt_Dma11,		psxDMA11Interrupt);	// SIO2
		IopTestEvent(IopEvt_Dma12,		psxDMA12Interrupt);	// SIO2
		IopTestEvent(IopEvt_Cdrom,		cdrInterrupt);
		IopTestEvent(IopEvt_CdromRead,	cdrReadInterrupt);
		IopTestEvent(IopEvt_DEV9,		dev9Interrupt);
		IopTestEvent(IopEvt_USB,		usbInterrupt);
	}
}

__ri void iopEventTest()
{
#if IPSX2_IOP_EVT_DIAG
	// [TEMP_DIAG] @@IOP_STUCK@@ — detect IOP stuck at same PC for extended period
	// Purpose: diagnose why IOP stops loading modules after eesync
	// Removal condition: IOP モジュール読み込み停止のcauseafter identified
	{
		static u32 s_stuck_pc = 0;
		static u32 s_stuck_count = 0;
		static int s_stuck_n = 0;
		if (psxRegs.pc == s_stuck_pc) {
			s_stuck_count++;
			if (s_stuck_count == 50000 && s_stuck_n < 5 && psxRegs.cycle > 550000000u) {
				s_stuck_n++;
				// Dump IOP code at stuck PC
				u32 code[8] = {};
				for (int i = 0; i < 8; i++)
					code[i] = iopMemRead32((psxRegs.pc & 0x1fffff) + i * 4);
				Console.WriteLn("@@IOP_STUCK@@ n=%d pc=%08x cyc=%u sr=%08x cause=%08x intr=%08x "
					"code=[%08x %08x %08x %08x %08x %08x %08x %08x] "
					"v0=%08x v1=%08x a0=%08x a1=%08x ra=%08x sp=%08x",
					s_stuck_n, psxRegs.pc, psxRegs.cycle,
					psxRegs.CP0.n.Status, psxRegs.CP0.n.Cause, psxRegs.interrupt,
					code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7],
					psxRegs.GPR.n.v0, psxRegs.GPR.n.v1, psxRegs.GPR.n.a0, psxRegs.GPR.n.a1,
					psxRegs.GPR.n.ra, psxRegs.GPR.n.sp);
				// Also dump INTC state
				Console.WriteLn("@@IOP_STUCK_INTC@@ ISTAT=%08x IMASK=%08x ICTRL=%08x "
					"DPCR=%08x DPCR2=%08x DICR=%08x DICR2=%08x",
					psxHu32(0x1070), psxHu32(0x1074), psxHu32(0x1078),
					psxHu32(0x10f0), psxHu32(0x1570), psxHu32(0x10f4), psxHu32(0x1574));
			}
		} else {
			s_stuck_pc = psxRegs.pc;
			s_stuck_count = 0;
		}
	}

	static bool s_probe_cfg_printed = false;
	static int s_probe_loop9c_count = 0;
	if (!s_probe_cfg_printed)
	{
		Console.WriteLn("@@IOP_LOOP9C_CFG@@ enabled=1 max=50");
		s_probe_cfg_printed = true;
	}
	if (psxRegs.pc == 0x9fc4109c && s_probe_loop9c_count < 50)
	{
		++s_probe_loop9c_count;
		Console.WriteLn("@@IOP_LOOP9C@@ n=%d pc=%08x code=%08x a0=%08x v0=%08x v1=%08x intr=%08x status=%08x",
			s_probe_loop9c_count, psxRegs.pc, psxRegs.code, psxRegs.GPR.n.a0, psxRegs.GPR.n.v0, psxRegs.GPR.n.v1,
			psxRegs.interrupt, psxRegs.CP0.n.Status);
	}

	// [TEMP_DIAG] iter661: @@IOP_EXCPT_DISPATCH@@ — IOP 例外handler入口 0x2f5c で
	// INTC_STAT/INTC_MASK をキャプチャし、どの割り込みが dispatch されているかverify。
	// 根拠: iter660 確定: IEC=1→0 はalways 0xa0002f5c でoccur (例外handler)。
	//        PC=0x11f30 (VBlank ISR) は cap=20 の間に一度も発火しない。
	//        → 例外が VBlank ISR に到達しないcauseを例外handlerの dispatch 先でverifyする。
	//        INTC_STAT=0x1F801070, INTC_MASK=0x1F801074 (psxHu32)
	// Removal condition: 例外 dispatch 先が判明し VBlank ISR 未実行のcauseafter determined
	{
		static int s_excpt_n = 0;
		u32 _imask = psxHu32(0x1074u);
		// cyc>400000 (IEC 変化開始後) かつ INTC_MASK≠0 (実際の割り込みコンテキスト) のみキャプチャ
		if (psxRegs.pc == 0xa0002f5cu && s_excpt_n < 20
			&& psxRegs.cycle > 400000u && _imask != 0u)
		{
			++s_excpt_n;
			u32 intc_stat = psxHu32(0x1070u);
			Console.WriteLn("@@IOP_EXCPT_DISPATCH@@ n=%d pc=0x%08x sr=0x%08x cause=0x%08x"
				" INTC_STAT=0x%04x INTC_MASK=0x%04x cyc=%u",
				s_excpt_n, psxRegs.pc, psxRegs.CP0.n.Status, psxRegs.CP0.n.Cause,
				intc_stat, _imask, psxRegs.cycle);
		}
	}

	// [TEMP_DIAG] iter638+664: @@IOP_IEC_CHANGE@@ / @@IEC_LATE@@
	// iter638: IEc bit(0) 変化のみ追跡 (cap=50, 初期フェーズ)
	// iter664: @@IEC_LATE@@ — cyc>368M (vsync≥600相当) での IEc 変化をadd補足 (cap=20)
	//          目的: vsync=600 以降に VBlank ISR が発火しているか (IEc 振動) verify
	// Removal condition: vsync≥600 での IEc 振動有無が確定した後
	{
		static u32 s_prev_iec = 0xFFFFFFFFu;
		static int s_iec_chg_n = 0;
		static int s_iec_late_n = 0;
		u32 cur_iec = psxRegs.CP0.n.Status & 1u;
		if (cur_iec != s_prev_iec)
		{
			// [P12] cap 50→200 for JIT IEC permanent=0 transition capture
			if (s_iec_chg_n < 200)
			{
				++s_iec_chg_n;
				Console.WriteLn("@@IOP_IEC_CHANGE@@ n=%d IEc=%d->%d sr=0x%08x pc=0x%08x cyc=%u ra=0x%08x",
					s_iec_chg_n, s_prev_iec, cur_iec,
					psxRegs.CP0.n.Status, psxRegs.pc, psxRegs.cycle, psxRegs.GPR.n.ra);
			}
			if (psxRegs.cycle > 368000000u && s_iec_late_n < 20)
			{
				++s_iec_late_n;
				Console.WriteLn("@@IEC_LATE@@ n=%d IEc=%d->%d sr=0x%08x pc=0x%08x cyc=%u",
					s_iec_late_n, s_prev_iec, cur_iec,
					psxRegs.CP0.n.Status, psxRegs.pc, psxRegs.cycle);
			}
			// [P12] @@IEC_MID@@ — cyc=29M-38M の IEc 変化を捕捉 (永続=0 転換点特定)
			static int s_iec_mid_n = 0;
			if (psxRegs.cycle > 29000000u && psxRegs.cycle < 38000000u && s_iec_mid_n < 100)
			{
				++s_iec_mid_n;
				Console.WriteLn("@@IEC_MID@@ n=%d IEc=%d->%d sr=0x%08x pc=0x%08x cyc=%u ra=0x%08x cause=0x%08x",
					s_iec_mid_n, s_prev_iec, cur_iec,
					psxRegs.CP0.n.Status, psxRegs.pc, psxRegs.cycle, psxRegs.GPR.n.ra,
					psxRegs.CP0.n.Cause);
			}
			s_prev_iec = cur_iec;
		}
	}
#endif // IPSX2_IOP_EVT_DIAG

	psxRegs.iopNextEventCycle = psxRegs.cycle + iopWaitCycles;

	if (psxTestCycle(psxNextStartCounter, psxNextDeltaCounter))
	{
		psxRcntUpdate();
		iopEventAction = true;
	}
	else
	{
		// start the next branch at the next counter event by default
		// the interrupt code below will assign nearer branches if needed.
		if (psxNextDeltaCounter < static_cast<s32>(psxRegs.iopNextEventCycle - psxNextStartCounter))
			psxRegs.iopNextEventCycle = psxNextStartCounter + psxNextDeltaCounter;
	}

	if (psxRegs.interrupt)
	{
		iopEventTestIsActive = true;
		_psxTestInterrupts();
		iopEventTestIsActive = false;
	}

#if IPSX2_IOP_EVT_DIAG
	// [TEMP_DIAG] iter629: @@3EE70_ENTRY@@ — TestEvent stub 進入時の a0 (event_id) を one-shot 捕捉
	// 根拠: iter628 解析で 0x3ee70 = B(0x0b)=TestEvent スタブ確定。IOP が polling する event_id 不明。
	// a0 = TestEvent 引数 (event_id)、ra = call元リターンaddress。
	// Removal condition: 対象 event_id after identified
	{
		static int s_3ee70_capture_n = 0;
		if (psxRegs.pc == 0x3ee70u && s_3ee70_capture_n < 30)
		{
			++s_3ee70_capture_n;
			Console.WriteLn("@@3EE70_ENTRY@@ n=%d a0=%08x ra=%08x v0=%08x cyc=%u",
				s_3ee70_capture_n,
				psxRegs.GPR.n.a0, psxRegs.GPR.n.ra,
				psxRegs.GPR.n.v0, psxRegs.cycle);
		}
	}

	// [TEMP_DIAG] @@IOP_INTGATE@@ — periodic check of interrupt gate conditions
	{
		static u32 s_gate_last_cycle = 0;
		if (psxRegs.cycle - s_gate_last_cycle > 36864000u) // ~1 sec
		{
			s_gate_last_cycle = psxRegs.cycle;
			static int s_gate_cnt = 0;
			if (++s_gate_cnt <= 30)
			{
				Console.WriteLn("@@IOP_INTGATE@@ n=%d ictrl=0x%08x istat=0x%08x imask=0x%08x sr=0x%08x pc=0x%08x cyc=%u",
					s_gate_cnt, psxHu32(0x1078), psxHu32(0x1070), psxHu32(0x1074),
					psxRegs.CP0.n.Status, psxRegs.pc, psxRegs.cycle);
			}
		}
	}
#endif // IPSX2_IOP_EVT_DIAG

	{
		// [FIX] DMA interrupt ICTRL bypass (second check point)
		// 第一チェック (iopTestIntc) と同じ理由で DMA 割り込みは ICTRL=0 でもdispatch
		u32 ictrl = psxHu32(0x1078);
		u32 pending = psxHu32(0x1070) & psxHu32(0x1074);
		if (!((ictrl != 0) && (pending != 0))) goto skip_intc_dispatch;
	}
	{
		// [P12] IEC デッドロック回復ハック — 撤去 (iter652)
		// 根拠: P12 Fix A/B で MTC0→Status および RFE 後に iopNextEventCycle=cycle を
		// configするようになったため、IEc=1 config直後に iopEventTest() が走る。
		// force RFE ハックはnot needed。自然な割り込み配送で進行するか観測する。

		if ((psxRegs.CP0.n.Status & 0xFE01) >= 0x401)
		{
#if IPSX2_IOP_EVT_DIAG
			// [TEMP_DIAG] @@IOP_HWINT@@ — count IOP hardware interrupts
			{
				static int s_iop_hwint_cnt = 0;
				++s_iop_hwint_cnt;
				if (s_iop_hwint_cnt <= 10 || (s_iop_hwint_cnt % 5000 == 0 && s_iop_hwint_cnt <= 100000))
				{
					Console.WriteLn("@@IOP_HWINT@@ n=%d istat=0x%08x imask=0x%08x pc=0x%08x",
						s_iop_hwint_cnt, psxHu32(0x1070), psxHu32(0x1074), psxRegs.pc);
				}
			}
#endif // IPSX2_IOP_EVT_DIAG
			PSXCPU_LOG("Interrupt: %x  %x", psxHu32(0x1070), psxHu32(0x1074));
			psxException(0, 0);
			iopEventAction = true;
		}
	}
	skip_intc_dispatch:;
}

void iopTestIntc()
{
	// [FIX] DMA interrupt bypass for ICTRL=0
	// IOP インタプリタの低速実行により、ICTRL=0 中に複数の SIF DMA 完了が蓄積。
	// 受信リングbufferがoverwriteされ sceSifInit コマンドが消失する。
	// DMA 割り込み (ISTAT bit 3) は ICTRL=0 でもdispatchを遅延させない。
	// PC (IOP recompiler) では ICTRL=0 window が短くissueにならない。
	// Removal condition: IOP recompilerimpl後
	if( psxHu32(0x1078) == 0 ) return;
	if( (psxHu32(0x1070) & psxHu32(0x1074)) == 0 ) return;

	if( !eeEventTestIsActive )
	{
		// An iop exception has occurred while the EE is running code.
		// Inform the EE to branch so the IOP can handle it promptly:
		// [FIX] DMA interrupt (bit 3) needs fast dispatch to maintain SIF DMA chain.
		// IOP interpreter is slower than recompiler; delta=16 causes DMA9 chain break
		// during fake reset because handler runs too late (send buffer already empty).
		// Removal condition: IOP recompilerimpl後
		cpuSetNextEventDelta( 16 );
		iopEventAction = true;
		//Console.Error( "** IOP Needs an EE EventText, kthx **  %d", iopCycleEE );

		// Note: No need to set the iop's branch delta here, since the EE
		// will run an IOP branch test regardless.
	}
	else if ( !iopEventTestIsActive )
		psxSetNextBranchDelta( 2 );
}

inline bool psxIsBranchOrJump(u32 addr)
{
	u32 op = iopMemRead32(addr);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	return (opcode.flags & IS_BRANCH) != 0;
}

// The next two functions return 0 if no breakpoint is needed,
// 1 if it's needed on the current pc, 2 if it's needed in the delay slot
// 3 if needed in both

int psxIsBreakpointNeeded(u32 addr)
{
	int bpFlags = 0;
	if (CBreakPoints::IsAddressBreakPoint(BREAKPOINT_IOP, addr))
		bpFlags += 1;

	// there may be a breakpoint in the delay slot
	if (psxIsBranchOrJump(addr) && CBreakPoints::IsAddressBreakPoint(BREAKPOINT_IOP, addr + 4))
		bpFlags += 2;

	return bpFlags;
}

int psxIsMemcheckNeeded(u32 pc)
{
	if (CBreakPoints::GetNumMemchecks() == 0)
		return 0;

	u32 addr = pc;
	if (psxIsBranchOrJump(addr))
		addr += 4;

	u32 op = iopMemRead32(addr);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	if (opcode.flags & IS_MEMORY)
		return addr == pc ? 1 : 2;

	return 0;
}
