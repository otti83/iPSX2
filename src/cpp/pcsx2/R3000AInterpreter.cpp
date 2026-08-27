// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "R3000A.h"
#include "Common.h"
#include "Config.h"
#include "VMManager.h"

#include "R5900OpcodeTables.h"
#include "DebugTools/Breakpoints.h"
#include "IopBios.h"
#include "IopDma.h"
#include "IopHw.h"
#include "Sif.h"
#include "Dmac.h"

using namespace R3000A;

// Used to flag delay slot instructions when throwig exceptions.
bool iopIsDelaySlot = false;

// [TEMP_DIAG] IOP reboot counter — incremented at 0x890 branch, used by IopBios.cpp @@REG_LIB_ENT@@
u32 g_iop_reboot_count = 0;

static bool branch2 = 0;
static u32 branchPC;

static void doBranch(s32 tar);	// forward declared prototype

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, offset                                 *
*********************************************************/

void psxBGEZ()         // Branch if Rs >= 0
{
	if (_i32(_rRs_) >= 0) doBranch(_BranchTarget_);
}

void psxBGEZAL()   // Branch if Rs >= 0 and link
{
	_SetLink(31);
	if (_i32(_rRs_) >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void psxBGTZ()          // Branch if Rs >  0
{
	if (_i32(_rRs_) > 0) doBranch(_BranchTarget_);
}

void psxBLEZ()         // Branch if Rs <= 0
{
	if (_i32(_rRs_) <= 0) doBranch(_BranchTarget_);
}
void psxBLTZ()          // Branch if Rs <  0
{
	if (_i32(_rRs_) < 0) doBranch(_BranchTarget_);
}

void psxBLTZAL()    // Branch if Rs <  0 and link
{
	_SetLink(31);
	if (_i32(_rRs_) < 0)
		{
			doBranch(_BranchTarget_);
		}
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, rt, offset                             *
*********************************************************/

void psxBEQ()   // Branch if Rs == Rt
{
	if (_i32(_rRs_) == _i32(_rRt_)) doBranch(_BranchTarget_);
}

void psxBNE()   // Branch if Rs != Rt
{
	if (_i32(_rRs_) != _i32(_rRt_)) doBranch(_BranchTarget_);
}

/*********************************************************
* Jump to target                                         *
* Format:  OP target                                     *
*********************************************************/
void psxJ()
{
	// check for iop module import table magic
	u32 delayslot = iopMemRead32(psxRegs.pc);
	if (delayslot >> 16 == 0x2400 && irxImportExec(irxImportTableAddr(psxRegs.pc), delayslot & 0xffff))
		return;

	doBranch(_JumpTarget_);
}

void psxJAL()
{
	_SetLink(31);
	doBranch(_JumpTarget_);
}

/*********************************************************
* Register jump                                          *
* Format:  OP rs, rd                                     *
*********************************************************/
void psxJR()
{
	doBranch(_u32(_rRs_));
}

void psxJALR()
{
	if (_Rd_)
	{
		_SetLink(_Rd_);
	}
	doBranch(_u32(_rRs_));
}

void psxBreakpoint(bool memcheck)
{
	u32 pc = psxRegs.pc;
	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_IOP, pc) != 0)
		return;

	if (!memcheck)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_IOP, pc);
		if (cond && !cond->Evaluate())
			return;
	}

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_IOP);
	VMManager::SetPaused(true);
	Cpu->ExitExecution();
}

void psxMemcheck(u32 op, u32 bits, bool store)
{
	// compute accessed address
	u32 start = psxRegs.GPR.r[(op >> 21) & 0x1F];
	if ((s16)op != 0)
		start += (s16)op;

	u32 end = start + bits / 8;

	auto checks = CBreakPoints::GetMemChecks(BREAKPOINT_IOP);
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
			psxBreakpoint(true);
	}
}

void psxCheckMemcheck()
{
	u32 pc = psxRegs.pc;
	int needed = psxIsMemcheckNeeded(pc);
	if (needed == 0)
		return;

	u32 op = iopMemRead32(needed == 2 ? pc + 4 : pc);
	// Yeah, we use the R5900 opcode table for the R3000
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	bool store = (opcode.flags & IS_STORE) != 0;
	switch (opcode.flags & MEMTYPE_MASK)
	{
	case MEMTYPE_BYTE:
		psxMemcheck(op, 8, store);
		break;
	case MEMTYPE_HALF:
		psxMemcheck(op, 16, store);
		break;
	case MEMTYPE_WORD:
		psxMemcheck(op, 32, store);
		break;
	case MEMTYPE_DWORD:
		psxMemcheck(op, 64, store);
		break;
	}
}

///////////////////////////////////////////
// These macros are used to assemble the repassembler functions

static __fi void execI()
{
	// This function is called for every instruction.
	// Enabling the define below will probably, no, will cause the interpretor to be slower.
//#define EXTRA_DEBUG
#if defined(EXTRA_DEBUG) || defined(PCSX2_DEVBUILD)
	if (psxIsBreakpointNeeded(psxRegs.pc))
		psxBreakpoint(false);

	psxCheckMemcheck();
#endif

	// Inject IRX hack
	if (psxRegs.pc == 0x1630 && EmuConfig.CurrentIRX.length() > 3) {
		if (iopMemRead32(0x20018) == 0x1F) {
			// FIXME do I need to increase the module count (0x1F -> 0x20)
			iopMemWrite32(0x20094, 0xbffc0000);
		}
	}

	psxRegs.code = iopMemRead32(psxRegs.pc);

		PSXCPU_LOG("%s", disR3000AF(psxRegs.code, psxRegs.pc));

	psxRegs.pc+= 4;
	psxRegs.cycle++;

	psxBSC[psxRegs.code >> 26]();
}

static void doBranch(s32 tar) {
	if (tar == 0x0) {
		// [P9 TEMP_DIAG] @@IOP_HANDLER_SKIP@@ fake RFE: exception handler branched to 0x0 (uninitialized dispatch table)
		if (psxRegs.pc >= 0x80000080 && psxRegs.pc < 0x80001000) {
			u32 status = psxRegs.CP0.n.Status;
			// Only restore IE stack when IEc=0 (genuine exception context).
			// If IEc=1 (delay slot of a previously-patched branch), do NOT shift again.
			if ((status & 1) == 0)
				psxRegs.CP0.n.Status = (status & ~0x0fu) | ((status >> 2) & 0x0fu);
			static int s_skip_count = 0;
			if (s_skip_count < 20) {
				int n = ++s_skip_count;
				Console.WriteLn("@@IOP_HANDLER_SKIP@@ #%d pc=%08x EPC=%08x sr_old=%08x sr_new=%08x cause=%08x",
					n, psxRegs.pc, psxRegs.CP0.n.EPC, status, psxRegs.CP0.n.Status, psxRegs.CP0.n.Cause);
				// One-shot: dump exception handler code + dispatch table on first SKIP
				if (n == 1) {
					u32 exc[12], tbl[20];
					for (int i = 0; i < 12; i++) exc[i] = iopMemRead32(0x80 + i * 4);
					for (int i = 0; i < 20; i++) tbl[i] = iopMemRead32(0x400 + i * 4);
					Console.WriteLn("@@IOP_EXCVEC@@ [080]=%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
						exc[0],exc[1],exc[2],exc[3],exc[4],exc[5],exc[6],exc[7],exc[8],exc[9],exc[10],exc[11]);
					Console.WriteLn("@@IOP_DTBL@@ [400]=%08x %08x %08x %08x %08x | [414]=%08x %08x %08x %08x %08x",
						tbl[0],tbl[1],tbl[2],tbl[3],tbl[4],tbl[5],tbl[6],tbl[7],tbl[8],tbl[9]);
					Console.WriteLn("@@IOP_DTBL@@ [428]=%08x %08x %08x %08x %08x | [43c]=%08x %08x %08x %08x %08x",
						tbl[10],tbl[11],tbl[12],tbl[13],tbl[14],tbl[15],tbl[16],tbl[17],tbl[18],tbl[19]);
					// Also dump I_STAT and I_MASK
					u32 istat = iopMemRead32(0x1F801070);
					u32 imask = iopMemRead32(0x1F801074);
					Console.WriteLn("@@IOP_INTREGS@@ I_STAT=%08x I_MASK=%08x CAUSE=%08x",
						istat, imask, psxRegs.CP0.n.Cause);
					// Dump actual dispatch code near the JR ($rs=0) at 0x80000848
					u32 jrarea[8];
					for (int i = 0; i < 8; i++) jrarea[i] = iopMemRead32(0x830 + i * 4);
					Console.WriteLn("@@IOP_JRAREA@@ [830]=%08x %08x %08x %08x | [840]=%08x %08x %08x %08x",
						jrarea[0],jrarea[1],jrarea[2],jrarea[3],jrarea[4],jrarea[5],jrarea[6],jrarea[7]);
					// Dump IOP thread functions at 0xAEA0 and 0xBCE0, plus callsites
					u32 aea0[6], bce0[6], bcb8[12], e163e0[8];
					for (int i = 0; i < 6; i++) aea0[i] = iopMemRead32(0xAEA0 + i * 4);
					for (int i = 0; i < 6; i++) bce0[i] = iopMemRead32(0xBCE0 + i * 4);
					for (int i = 0; i < 12; i++) bcb8[i] = iopMemRead32(0xBCB8 + i * 4);
					for (int i = 0; i < 8; i++) e163e0[i] = iopMemRead32(0x163E0 + i * 4);
					Console.WriteLn("@@IOP_AEA0@@ [aea0]=%08x %08x %08x %08x %08x %08x",
						aea0[0],aea0[1],aea0[2],aea0[3],aea0[4],aea0[5]);
					Console.WriteLn("@@IOP_BCE0@@ [bce0]=%08x %08x %08x %08x %08x %08x",
						bce0[0],bce0[1],bce0[2],bce0[3],bce0[4],bce0[5]);
					Console.WriteLn("@@IOP_BCB8@@ [bcb8]=%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
						bcb8[0],bcb8[1],bcb8[2],bcb8[3],bcb8[4],bcb8[5],bcb8[6],bcb8[7],bcb8[8],bcb8[9],bcb8[10],bcb8[11]);
					Console.WriteLn("@@IOP_163E0@@ [163e0]=%08x %08x %08x %08x | %08x %08x %08x %08x",
						e163e0[0],e163e0[1],e163e0[2],e163e0[3],e163e0[4],e163e0[5],e163e0[6],e163e0[7]);
					// Dump word at 0xAE9C (entry before SYSCALL) + regs at SKIP#1 time
					u32 ae9c = iopMemRead32(0xAE9C);
					Console.WriteLn("@@IOP_AE9C@@ [ae9c]=%08x v0=%08x a0=%08x a1=%08x ra=%08x sp=%08x",
						ae9c, psxRegs.GPR.n.v0, psxRegs.GPR.n.a0, psxRegs.GPR.n.a1, psxRegs.GPR.n.ra, psxRegs.GPR.n.sp);
					// Dump BCB8 second call target at 0x10CC4 (BCD8 = JAL 0x10CC4, forwards to 0x13C0)
					u32 fn10cc4[12];
					for (int i = 0; i < 12; i++) fn10cc4[i] = iopMemRead32(0x10CC4 + i * 4);
					Console.WriteLn("@@IOP_10CC4@@ [10cc4]=%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
						fn10cc4[0],fn10cc4[1],fn10cc4[2],fn10cc4[3],fn10cc4[4],fn10cc4[5],
						fn10cc4[6],fn10cc4[7],fn10cc4[8],fn10cc4[9],fn10cc4[10],fn10cc4[11]);
					// Dump 0x13C0 (LOADCORE func#14 real body) and 0x11080 (boot-up list data)
					u32 fn13c0[12], dat11080[12];
					for (int i = 0; i < 12; i++) fn13c0[i] = iopMemRead32(0x13C0 + i * 4);
					for (int i = 0; i < 12; i++) dat11080[i] = iopMemRead32(0x11080 + i * 4);
					Console.WriteLn("@@IOP_13C0@@ [13c0]=%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
						fn13c0[0],fn13c0[1],fn13c0[2],fn13c0[3],fn13c0[4],fn13c0[5],
						fn13c0[6],fn13c0[7],fn13c0[8],fn13c0[9],fn13c0[10],fn13c0[11]);
					Console.WriteLn("@@IOP_11080@@ [11080]=%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
						dat11080[0],dat11080[1],dat11080[2],dat11080[3],dat11080[4],dat11080[5],
						dat11080[6],dat11080[7],dat11080[8],dat11080[9],dat11080[10],dat11080[11]);
					// Dump MEM[0x14A0] (LOADCORE boot-up list head) and surrounding context
					u32 dat14a0[8];
					for (int i = 0; i < 8; i++) dat14a0[i] = iopMemRead32(0x14A0 + i * 4);
					Console.WriteLn("@@IOP_14A0@@ [14a0]=%08x %08x %08x %08x | %08x %08x %08x %08x",
						dat14a0[0],dat14a0[1],dat14a0[2],dat14a0[3],dat14a0[4],dat14a0[5],dat14a0[6],dat14a0[7]);
					// Dump IOP 0x200000 area (possible loaded module) + BIOS ROM call-site at ra=0x8768
					u32 dat200000[16], site8768[8];
					for (int i = 0; i < 16; i++) dat200000[i] = iopMemRead32(0x200000 + i * 4);
					for (int i = 0; i < 8; i++) site8768[i] = iopMemRead32(0x8764 + i * 4);
					Console.WriteLn("@@IOP_200000@@ [200000]=%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
						dat200000[0],dat200000[1],dat200000[2],dat200000[3],
						dat200000[4],dat200000[5],dat200000[6],dat200000[7],
						dat200000[8],dat200000[9],dat200000[10],dat200000[11],
						dat200000[12],dat200000[13],dat200000[14],dat200000[15]);
					Console.WriteLn("@@IOP_8768@@ [8764]=%08x %08x %08x %08x | %08x %08x %08x %08x",
						site8768[0],site8768[1],site8768[2],site8768[3],
						site8768[4],site8768[5],site8768[6],site8768[7]);
					// Dump DMA handler else-branch at 0x16408 ($a1=0x20 path) and context at 0x8720 (loop re-entry at 0x873C)
					u32 fn16408[12], ctx8720[16];
					for (int i = 0; i < 12; i++) fn16408[i] = iopMemRead32(0x16408 + i * 4);
					for (int i = 0; i < 16; i++) ctx8720[i] = iopMemRead32(0x8720 + i * 4);
					Console.WriteLn("@@IOP_16408@@ [16408]=%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
						fn16408[0],fn16408[1],fn16408[2],fn16408[3],fn16408[4],fn16408[5],
						fn16408[6],fn16408[7],fn16408[8],fn16408[9],fn16408[10],fn16408[11]);
					Console.WriteLn("@@IOP_8720@@ [8720]=%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
						ctx8720[0],ctx8720[1],ctx8720[2],ctx8720[3],ctx8720[4],ctx8720[5],
						ctx8720[6],ctx8720[7],ctx8720[8],ctx8720[9],ctx8720[10],ctx8720[11],
						ctx8720[12],ctx8720[13],ctx8720[14],ctx8720[15]);
					// Dump VBlank handler (dispatch[400]=0x00020000) and EVBlank handler (0x4B3C)
					u32 vbl[8], evbl[8];
					for (int i = 0; i < 8; i++) vbl[i] = iopMemRead32(0x20000 + i * 4);
					for (int i = 0; i < 8; i++) evbl[i] = iopMemRead32(0x4B3C + i * 4);
					Console.WriteLn("@@IOP_VBLHANDLER@@ [20000]=%08x %08x %08x %08x | %08x %08x %08x %08x",
						vbl[0],vbl[1],vbl[2],vbl[3],vbl[4],vbl[5],vbl[6],vbl[7]);
					Console.WriteLn("@@IOP_EVBLHANDLER@@ [4b3c]=%08x %08x %08x %08x | %08x %08x %08x %08x",
						evbl[0],evbl[1],evbl[2],evbl[3],evbl[4],evbl[5],evbl[6],evbl[7]);
				}
			}
			// For synchronous exceptions (SYSCALL/BP/etc), EPC points to the
			// faulting instruction. The real handler would service it and return
			// to EPC+4. For interrupts (ExcCode=0), EPC points to the interrupted
			// instruction — return there directly.
			{
				u32 excCode = (psxRegs.CP0.n.Cause >> 2) & 0x1fu;
				tar = (s32)(psxRegs.CP0.n.EPC + (excCode != 0 ? 4u : 0u));
			}
			// ACK pending IOP interrupts so the interrupt storm stops.
			// Without running the real ISR the interrupt source is never cleared;
			// this prevents the IOP from re-entering the broken exception handler
			// immediately on returning to EPC.
			{
				u32 pending = psxHu32(0x1070) & psxHu32(0x1074);
				if (pending) {
					psxHu32(0x1070) &= ~pending;
					static int s_ack_count = 0;
					if (s_ack_count < 10)
						Console.WriteLn("@@IOP_INTACK@@ #%d ack=0x%08x", ++s_ack_count, pending);
				}
			}
		} else {
			// [P9 TEMP_DIAG] cap=20
			static int s_b0_cap = 0;
			if (s_b0_cap < 20) Console.Warning("[R3000 Interpreter] Warning: Branch to 0x0! [%d/20] pc=%08x", ++s_b0_cap, psxRegs.pc);
		}
	}

	// When upgrading the IOP, there are two resets, the second of which is a 'fake' reset
	// This second 'reset' involves UDNL calling SYSMEM and LOADCORE directly, resetting LOADCORE's modules
	// This detects when SYSMEM is called and clears the modules then
	if(tar == 0x890)
	{
		// IOP memory dump at reboot — check SIF DMA dest for IOPRP command data
		u32 iop_19600 = (iopMem && iopMem->Main) ? *(u32*)(iopMem->Main + 0x19600) : 0xDEADu;
		u32 iop_19618 = (iopMem && iopMem->Main) ? *(u32*)(iopMem->Main + 0x19618) : 0;
		{
			u32 v532c = (iopMem && iopMem->Main) ? *(u32*)(iopMem->Main + 0x532C) : 0xDEADu;
			Console.WriteLn("@@IOP_REBOOT_890@@ iopcyc=%u [19600]=%08x [19618]=%08x D9_CHCR=%08x DPCR2=%08x DICR2=%08x [532C]=%08x",
				psxRegs.cycle, iop_19600, iop_19618, psxHu32(0x1528), psxHu32(0x1570), psxHu32(0x1574), v532c);
		}
		// [P37 TEMP_DIAG] Preserved locations dump BEFORE injection for PC comparison
		if (iopMem && iopMem->Main) {
			auto rd32 = [&](u32 a) -> u32 { return *(u32*)(iopMem->Main + a); };
			Console.WriteLn("@@PRESERVED_PRE@@ iopcyc=%u [0480]=%08x%08x%08x%08x [5a70]=%08x%08x%08x%08x "
				"[1ad28]=%08x%08x%08x%08x [20020]=%08x%08x%08x%08x",
				psxRegs.cycle,
				rd32(0x480), rd32(0x484), rd32(0x488), rd32(0x48C),
				rd32(0x5a70), rd32(0x5a74), rd32(0x5a78), rd32(0x5a7C),
				rd32(0x1ad28), rd32(0x1ad2C), rd32(0x1ad30), rd32(0x1ad34),
				rd32(0x20020), rd32(0x20024), rd32(0x20028), rd32(0x2002C));
			char s0[64]={}, s1[64]={}, s2[64]={}, s3[64]={};
			std::memcpy(s0, iopMem->Main+0x480, 63);
			std::memcpy(s1, iopMem->Main+0x5a70, 63);
			std::memcpy(s2, iopMem->Main+0x1ad28, 63);
			std::memcpy(s3, iopMem->Main+0x20020, 63);
			Console.WriteLn("@@PRESERVED_PRE_STR@@ [0480]='%.60s' [5a70]='%.60s' [1ad28]='%.60s' [20020]='%.60s'",
				s0, s1, s2, s3);
		}
		// [P37] H3-H4 REMOVED: sifcmd naturally writes IOPRP path to preserved locations
		// Evidence: @@PRESERVED_PRE@@ at iopcyc=543105720 shows all 4 locations correct BEFORE injection
		// PC PCSX2 IOP Int comparison confirms identical data

		// IOP sifcmd data area check at reboot
		if (iopMem && iopMem->Main) {
			u32 v179b4 = *(u32*)(iopMem->Main + 0x179B4);
			u32 v179b8 = *(u32*)(iopMem->Main + 0x179B8);
			u32 v179bc = *(u32*)(iopMem->Main + 0x179BC);
			u32 v179c0 = *(u32*)(iopMem->Main + 0x179C0);
			Console.WriteLn("@@IOP_890_SIFDATA@@ [179B4]=%08x [179B8]=%08x [179BC]=%08x [179C0]=%08x",
				v179b4, v179b8, v179bc, v179c0);
		}
		g_iop_reboot_count++;
		DevCon.WriteLn(Color_Gray, "R3000 Debugger: Branch to 0x890 (SYSMEM). Clearing modules. reboot#%u", g_iop_reboot_count);
		R3000SymbolGuardian.ClearIrxModules();
	}

	branch2 = iopIsDelaySlot = true;
	branchPC = tar;
	execI();
	PSXCPU_LOG( "\n" );
	iopIsDelaySlot = false;
	psxRegs.pc = branchPC;

	iopEventTest();
}

static void intReserve() {
}

static void intAlloc() {
}

static void intReset() {
	intAlloc();
}

static s32 intExecuteBlock( s32 eeCycles )
{
    // [DIAGNOSTIC] IOP Heartbeat (1Hz approx 36.8MHz)
    static u32 s_last_log_cycle = 0;
    if ((u32)(psxRegs.cycle - s_last_log_cycle) > 36864000) {
        Console.WriteLn("@@IOP_ALIVE@@ pc=%08x cycle=%d", psxRegs.pc, psxRegs.cycle);
        s_last_log_cycle = psxRegs.cycle;
    }
	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = eeCycles;
	u32 lastIOPCycle = 0;

	while (psxRegs.iopCycleEE > 0)
	{
		lastIOPCycle = psxRegs.cycle;
		if ((psxHu32(HW_ICFG) & 8) && ((psxRegs.pc & 0x1fffffffU) == 0xa0 || (psxRegs.pc & 0x1fffffffU) == 0xb0 || (psxRegs.pc & 0x1fffffffU) == 0xc0))
			psxBiosCall();

		branch2 = 0;
		while (!branch2)
			execI();

		
		if ((psxHu32(HW_ICFG) & (1 << 3)))
		{
			// F = gcd(PS2CLK, PSXCLK) = 230400
			const u32 cnum = 1280; // PS2CLK / F
			const u32 cdenom = 147; // PSXCLK / F

			//One of the Iop to EE delta clocks to be set in PS1 mode.
			const u32 t = ((cnum * (psxRegs.cycle - lastIOPCycle)) + psxRegs.iopCycleEECarry);
			psxRegs.iopCycleEE -= t / cdenom;
			psxRegs.iopCycleEECarry = t % cdenom;
		}
		else
		{ 
			//default ps2 mode value
			psxRegs.iopCycleEE -= (psxRegs.cycle - lastIOPCycle) * 8;
		}
	}

	return psxRegs.iopBreak + psxRegs.iopCycleEE;
}

static void intClear(u32 Addr, u32 Size) {
}

static void intShutdown() {
}

R3000Acpu psxInt = {
	intReserve,
	intReset,
	intExecuteBlock,
	intClear,
	intShutdown
};
