// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "x86/iR5900.h"

#if !defined(__ANDROID__)
using namespace x86Emitter;
#endif



namespace R5900::Dynarec::OpcodeImpl
{

extern "C" void LogJR_V0_Runtime(u32 pc, u64 v0_val, int src_type) {
    if (pc == 0xbfc023e4) { 
        static bool done = false; 
        if (done) return;
        done = true;
        fprintf(stderr, "@@JR_V0_RUNTIME@@ pc=%x src=%s v0=%llx\n", 
            pc, (src_type == 1) ? "HOST" : "MEM", v0_val);
    }
}

// [R62] @@JR_RA_RUNTIME_TRACE@@ — runtime probe for jr $ra at specific PC
extern "C" void TraceJrRaRuntime(u32 jr_pc, u32 ra_val, u32 sp_val, u32 ra_hi)
{
	static u32 s_cnt = 0;
	// Only log after restart (cycle wraps, so use frame count proxy via small cycle)
	// Also catch the exact stuck case: ra=0x2058e0 or sp > 0x400000
	const bool is_stuck = (ra_val == 0x002058e0 || sp_val > 0x00400000u);
	if (is_stuck && s_cnt < 20)
	{
		Console.WriteLn("@@JR_RA_RUNTIME_TRACE@@ n=%u jr_pc=%08x ra=%08x sp=%08x ra_hi=%08x cycle=%u",
			s_cnt, jr_pc, ra_val, sp_val, ra_hi, cpuRegs.cycle);
		s_cnt++;
	}
}

extern "C" void TraceJrTargetProbe(u32 guest_pc, u32 rs, u32 target, u32 ra, u32 sp)
{
	// @@BAD_TARGET@@ unconditional one-shot: catch bad branch targets regardless of flag
	if (target == 0x02000000 || target < 0x00100000)
	{
		static bool s_bad_seen = false;
		if (!s_bad_seen)
		{
			s_bad_seen = true;
			Console.WriteLn("@@JR_BAD_TARGET@@ pc=%08x rs=%u target=%08x ra=%08x sp=%08x",
				guest_pc, rs, target, ra, sp);
		}
	}

	static int s_enabled = -1;
	static u32 s_count = 0;
	constexpr u32 kCap = 50;
	if (s_enabled < 0)
	{
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_JR_TARGET_PROBE", false) ? 1 : 0;
		if (s_enabled == 1)
			Console.WriteLn("@@CFG@@ iPSX2_JR_TARGET_PROBE=1 iPSX2_JR_TARGET_PROBE_CAP=%u", kCap);
	}
	if (s_enabled != 1 || s_count >= kCap)
		return;

	Console.WriteLn("@@JR_TARGET_PROBE@@ idx=%u pc=%08x rs=%u target=%08x ra=%08x sp=%08x",
		s_count, guest_pc, rs, target, ra, sp);
	s_count++;
}


/*********************************************************
* Jump to target                                         *
* Format:  OP target                                     *
*********************************************************/
#ifndef JUMP_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_SYS(J);
REC_SYS_DEL(JAL, 31);
REC_SYS(JR);
REC_SYS_DEL(JALR, _Rd_);

#else

////////////////////////////////////////////////////
namespace Interp = R5900::Interpreter::OpcodeImpl;

////////////////////////////////////////////////////
void recJ()
{


	EE::Profiler.EmitOp(eeOpcode::J);

	// SET_FPUSTATE;
	u32 newpc = (_InstrucTarget_ << 2) + (pc & 0xf0000000);
	recompileNextInstruction(true, false);
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchImm(vtlb_V2P(newpc));
	else
		SetBranchImm(newpc);
}

////////////////////////////////////////////////////
void recJAL()
{
	EE::Profiler.EmitOp(eeOpcode::JAL);

	u32 newpc = (_InstrucTarget_ << 2) + (pc & 0xf0000000);

	// [@@JAL_1578_PROBE@@] recJAL RA sanity check: fire when target=0x80001578
	// 目的: JIT が 0x80001578 を呼ぶ JAL でどの PC から・何の ra をconfigするかverify
	// Removal condition: ra=0x80001578 自己looproot causeafter determined
	if (newpc == 0x80001578u) {
		static int s_jal1578_n = 0;
		if (s_jal1578_n < 10) {
			Console.WriteLn("@@JAL_1578_PROBE@@ n=%d newpc=%08x pc(ds)=%08x ra_set=%08x jal_addr=%08x",
				s_jal1578_n, newpc, pc, pc + 4, pc - 4);
			s_jal1578_n++;
		}
	}

	_deleteEEreg(31, 0);
	if (EE_CONST_PROP)
	{
		GPR_SET_CONST(31);
		g_cpuConstRegs[31].UL[0] = pc + 4;
		g_cpuConstRegs[31].UL[1] = 0;
	}
	// Keep runtime RA coherent even when constant propagation is active.
//	xMOV(ptr32[&cpuRegs.GPR.r[31].UL[0]], pc + 4);
	armStore(PTR_CPU(cpuRegs.GPR.r[31].UL[0]), pc + 4);
//	xMOV(ptr32[&cpuRegs.GPR.r[31].UL[1]], 0);
	armStore(PTR_CPU(cpuRegs.GPR.r[31].UL[1]), 0);

	recompileNextInstruction(true, false);
	if (EmuConfig.Gamefixes.GoemonTlbHack)
		SetBranchImm(vtlb_V2P(newpc));
	else
		SetBranchImm(newpc);
}

/*********************************************************
* Register jump                                          *
* Format:  OP rs, rd                                     *
*********************************************************/

////////////////////////////////////////////////////
void recJR()
{


	EE::Profiler.EmitOp(eeOpcode::JR);
	if (pc >= 0x9FC42000 && pc <= 0x9FC43FFF)
	{
		const int rsreg = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
		armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
		armAsm->Push(a64::x4, a64::x5);
		armAsm->Push(a64::x29, a64::lr);
		armAsm->Mov(a64::w0, pc);
		armAsm->Mov(a64::w1, _Rs_);
		armAsm->Mov(a64::w2, a64::WRegister(HostGprPhys(rsreg)));
		armLoad(a64::w3, PTR_CPU(cpuRegs.GPR.n.ra.UL[0]));
		armLoad(a64::w4, PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));
		armEmitCall((void*)TraceJrTargetProbe);
		armAsm->Pop(a64::lr, a64::x29);
		armAsm->Pop(a64::x4, a64::x5);
		armAsm->Pop(a64::x0, a64::x1, a64::x2, a64::x3);
	}

	// @@FIX_JR_RA_DIRECT@@ iter12→R62: ALL jr $ra (no PC range limit)
	// Original evidence: @@JR_BAD_TARGET@@ target=00000000 ra=9fc42410 sp=70003d30
	// R62 evidence: OSDSYS stuck loop at pc=0x2058e0 — ld ra then jr ra reads stale
	// recJAL value because SetBranchReg/_eeMoveGPRtoR misses live host-reg for ra.
	// Fix: flush all cached ra state, then armLoad directly from cpuRegs.
	// [R62] Removed PC range check (was 0x9FC42000-0x9FC44000) — same bug class
	// at any address where ld $ra + jr $ra follows jal in previous block.
	if (_Rs_ == 31)
	{
		static int s_fix_count = 0;
		if (s_fix_count < 5)
		{
			Console.WriteLn("@@FIX_JR_RA_DIRECT@@ [%d] pc=%08x const1=%d hasX86=%d const_lo=%08x",
				s_fix_count, pc,
				GPR_IS_CONST1(31) ? 1 : 0,
				_hasX86reg(X86TYPE_GPR, 31, 0) ? 1 : 0,
				GPR_IS_CONST1(31) ? g_cpuConstRegs[31].UL[0] : 0u);
			s_fix_count++;
		}

		// Clear ALL stale state for r31: const-fold and host/XMM regs.
		// [iter39] DELETE_REG_FREE (not NO_WRITEBACK): if r31 has a live host register
		// (e.g. from LD ra, 0x90(sp) via bios_ra_stack64_guard), flush it to cpuRegs.r[31]
		// BEFORE the armLoad below.  Without this, the LD result (9FC41164) is discarded
		// and armLoad reads the stale recJAL value (9FC432DC), causing an infinite loop.
		GPR_DEL_CONST(31);
		_deleteGPRtoX86reg(31, DELETE_REG_FREE);
		_deleteGPRtoXMMreg(31, DELETE_REG_FREE_NO_WRITEBACK);

		// Allocate PCWRITEBACK register and DIRECTLY load from memory (not _eeMoveGPRtoR).
		// This is the only way to guarantee we read the runtime value (not stale 0).
		const int wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
		const auto wbr32 = HostW(wbreg);
		armLoad(wbr32, PTR_CPU(cpuRegs.GPR.r[31].UL[0]));
		armStore(PTR_CPU(cpuRegs.pcWriteback), wbr32);  // backup before delay slot

		// [R62] @@JR_RA_RUNTIME_TRACE@@ — one-shot runtime probe for stuck 0x2058e0
		if (pc == 0x002058f4u) // pc = jr_addr + 4
		{
			armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
			armAsm->Push(a64::x29, a64::lr);
			armAsm->Mov(a64::w0, pc - 4);  // guest PC of jr instruction
			armAsm->Mov(a64::w1, wbr32);   // ra value being used for jump
			armLoad(a64::w2, PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));  // sp at jr time
			armLoad(a64::w3, PTR_CPU(cpuRegs.GPR.r[31].UL[1]));  // ra upper 32
			armEmitCall((void*)TraceJrRaRuntime);
			armAsm->Pop(a64::lr, a64::x29);
			armAsm->Pop(a64::x0, a64::x1, a64::x2, a64::x3);
		}

		recompileNextInstruction(true, false);  // compile delay slot

		// After delay slot: wbreg may have been flushed; use pcWriteback as fallback.
		if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
		{
			armStore(PTR_CPU(cpuRegs.pc), wbr32);
			x86regs[wbreg].inuse = 0;
		}
		else
		{
			armLoad(EAX, PTR_CPU(cpuRegs.pcWriteback));
			armStore(PTR_CPU(cpuRegs.pc), EAX);
		}

		SetBranchReg(0xffffffff);  // dispatch only; cpuRegs.pc already set above
		return;
	}

	SetBranchReg(_Rs_);
}

////////////////////////////////////////////////////
void recJALR()
{


	EE::Profiler.EmitOp(eeOpcode::JALR);
	if (pc == 0x9FC43930)
	{
		const int rsreg = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
		armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
		armAsm->Push(a64::x4, a64::x5);
		armAsm->Push(a64::x29, a64::lr);
		armAsm->Mov(a64::w0, pc);
		armAsm->Mov(a64::w1, _Rs_);
		armAsm->Mov(a64::w2, a64::WRegister(HostGprPhys(rsreg)));
		armLoad(a64::w3, PTR_CPU(cpuRegs.GPR.n.ra.UL[0]));
		armLoad(a64::w4, PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));
		armEmitCall((void*)TraceJrTargetProbe);
		armAsm->Pop(a64::lr, a64::x29);
		armAsm->Pop(a64::x4, a64::x5);
		armAsm->Pop(a64::x0, a64::x1, a64::x2, a64::x3);
	}

	const u32 newpc = pc + 4;
	const bool swap = (EmuConfig.Gamefixes.GoemonTlbHack || _Rd_ == _Rs_) ? false : TrySwapDelaySlot(_Rs_, 0, _Rd_, true);

	// uncomment when there are NO instructions that need to call interpreter
	//	int mmreg;
	//	if (GPR_IS_CONST1(_Rs_))
	//		xMOV(ptr32[&cpuRegs.pc], g_cpuConstRegs[_Rs_].UL[0]);
	//	else
	//	{
	//		int mmreg;
	//
	//		if ((mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ)) >= 0)
	//		{
	//			xMOVSS(ptr[&cpuRegs.pc], xRegisterSSE(mmreg));
	//		}
	//		else {
	//			xMOV(eax, ptr[(void*)((int)&cpuRegs.GPR.r[_Rs_].UL[0])]);
	//			xMOV(ptr[&cpuRegs.pc], eax);
	//		}
	//	}

	int wbreg = -1;
	if (!swap)
	{
		wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
		_eeMoveGPRtoR(a64::WRegister(HostGprPhys(wbreg)), _Rs_);

		if (EmuConfig.Gamefixes.GoemonTlbHack)
		{
//			xMOV(ecx, xRegister32(wbreg));
            armAsm->Mov(ECX, a64::WRegister(HostGprPhys(wbreg)));
			vtlb_DynV2P();
//			xMOV(xRegister32(wbreg), eax);
            armAsm->Mov(a64::WRegister(HostGprPhys(wbreg)), EAX);
		}
	}

	if (_Rd_)
	{
		_deleteEEreg(_Rd_, 0);
		if (EE_CONST_PROP)
		{
			GPR_SET_CONST(_Rd_);
			g_cpuConstRegs[_Rd_].UD[0] = newpc;
		}
		// [iter185] EE_CONST_PROP は g_cpuConstRegs のみ更新し cpuRegs を書かない。
		// 次ブロック (JR $ra の callee 等) は cpuRegs.GPR.r[_Rd_] を直接読むため stale=0 で
		// JR $ra → PC=0 の無限loopがoccurする。always cpuRegs も更新して整合性を保つ。
		// Removal condition: const-flush が全ブロックboundaryで確実にbehaviorすることがverifyされた後delete
		armStore64(PTR_CPU(cpuRegs.GPR.r[_Rd_].UD[0]), newpc);
	}

	if (!swap)
	{
		recompileNextInstruction(true, false);

		// the next instruction may have flushed the register.. so reload it if so.
		if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
		{
//			xMOV(ptr[&cpuRegs.pc], xRegister32(wbreg));
            armStore(PTR_CPU(cpuRegs.pc), a64::WRegister(HostGprPhys(wbreg)));
			x86regs[wbreg].inuse = 0;
		}
		else
		{
//			xMOV(eax, ptr[&cpuRegs.pcWriteback]);
            armLoad(EAX, PTR_CPU(cpuRegs.pcWriteback));
//			xMOV(ptr[&cpuRegs.pc], eax);
            armStore(PTR_CPU(cpuRegs.pc), EAX);
		}
	}
	else
	{
		if (GPR_IS_DIRTY_CONST(_Rs_) || _hasX86reg(X86TYPE_GPR, _Rs_, 0))
		{
			const int x86reg = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
//			xMOV(ptr32[&cpuRegs.pc], xRegister32(x86reg));
            armStore(PTR_CPU(cpuRegs.pc), a64::WRegister(HostGprPhys(x86reg)));
		}
		else
		{
            _eeMoveGPRtoM(PTR_CPU(cpuRegs.pc), _Rs_);
		}
	}

	SetBranchReg(0xffffffff);
}

#endif

} // namespace R5900::Dynarec::OpcodeImpl

// [ROOT_CAUSE_INVESTIGATION] ROMDIR TBIN Search Trace
// This traces what the ROMDIR scan sees at BNE decision point
extern "C" void recLogRomdirBneDecision(u32 pc, u32 t0_ptr, u32 mem_val, u32 expected_v0, u32 taken)
{
    // Log the first 100 iterations to capture TBIN search
    static int s_bne_log_count = 0;
    if (s_bne_log_count < 100) {
        s_bne_log_count++;
        
        // Read 16 bytes from ROM at t0_ptr for context
        u8 bytes[16] = {};
        char ascii[17] = {};
        u32* pMem = (u32*)PSM(t0_ptr);
        if (pMem) {
            for (int i = 0; i < 16; i++) {
                bytes[i] = ((u8*)pMem)[i];
                ascii[i] = (bytes[i] >= 0x20 && bytes[i] < 0x7F) ? bytes[i] : '.';
            }
            ascii[16] = '\0';
        }
        
        Console.WriteLn("@@ROMDIR_BNE@@ #%d pc=%08x t0=%08x mem=%08x want=%08x taken=%d ascii=%s",
            s_bne_log_count, pc, t0_ptr, mem_val, expected_v0, taken, ascii);
        
        // Special check: is this the TBIN entry?
        if (mem_val == expected_v0) {
            Console.WriteLn("@@ROMDIR_TBIN_FOUND@@ t0=%08x mem_val=%08x", t0_ptr, mem_val);
        }
    }
}

// [VTLB_TRACE] Trace VTLB reads during ROMDIR scan
extern "C" void recLogVtlbRead(u32 pc, u32 addr, u32 val)
{
    // Only trace ROMDIR region reads (0xBFC02700-0xBFC03000)
    if (addr >= 0xBFC02700 && addr < 0xBFC03000) {
        static int s_vtlb_read_count = 0;
        if (s_vtlb_read_count < 50) {
            s_vtlb_read_count++;
            
            // Read 16 bytes from ROM at addr for context
            u8 bytes[16] = {};
            char ascii[17] = {};
            u32* pMem = (u32*)PSM(addr);
            if (pMem) {
                for (int i = 0; i < 16; i++) {
                    bytes[i] = ((u8*)pMem)[i];
                    ascii[i] = (bytes[i] >= 0x20 && bytes[i] < 0x7F) ? bytes[i] : '.';
                }
                ascii[16] = '\0';
            }
            
            Console.WriteLn("@@VTLB_READ@@ #%d pc=%08x addr=%08x val=%08x ascii=%s",
                s_vtlb_read_count, pc, addr, val, ascii);
        }
    }
}
