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

/*********************************************************
* Shift arithmetic with constant shift                   *
* Format:  OP rd, rt, sa                                 *
*********************************************************/
#ifndef MOVE_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_FUNC_DEL(LUI, _Rt_);
REC_FUNC_DEL(MFLO, _Rd_);
REC_FUNC_DEL(MFHI, _Rd_);
REC_FUNC(MTLO);
REC_FUNC(MTHI);

REC_FUNC_DEL(MFLO1, _Rd_);
REC_FUNC_DEL(MFHI1, _Rd_);
REC_FUNC(MTHI1);
REC_FUNC(MTLO1);

REC_FUNC_DEL(MOVZ, _Rd_);
REC_FUNC_DEL(MOVN, _Rd_);

#else

/*********************************************************
* Load higher 16 bits of the first word in GPR with imm  *
* Format:  OP rt, immediate                              *
*********************************************************/

//// LUI
void recLUI()
{
	if (!_Rt_)
		return;

	// need to flush the upper 64 bits for xmm
	GPR_DEL_CONST(_Rt_);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH_AND_FREE);

	if (EE_CONST_PROP)
	{
		GPR_SET_CONST(_Rt_);
		g_cpuConstRegs[_Rt_].UD[0] = (s32)(cpuRegs.code << 16);
	}

	else
	{
		const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
//		xMOV64(xRegister64(regt), (s64)(s32)(cpuRegs.code << 16));
        armAsm->Mov(HostX(regt), (s64)(s32)(cpuRegs.code << 16));
	}

	EE::Profiler.EmitOp(eeOpcode::LUI);
}

////////////////////////////////////////////////////
static void recMFHILO(bool hi, bool upper)
{
	if (!_Rd_)
		return;

	// kill any constants on rd, lower 64 bits get written regardless of upper
	_eeOnWriteReg(_Rd_, 0);

	const int reg = hi ? XMMGPR_HI : XMMGPR_LO;
	const int xmmd = EEINST_XMMUSEDTEST(_Rd_) ? _allocGPRtoXMMreg(_Rd_, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, _Rd_, MODE_READ | MODE_WRITE);
	const int xmmhilo = EEINST_XMMUSEDTEST(reg) ? _allocGPRtoXMMreg(reg, MODE_READ) : _checkXMMreg(XMMTYPE_GPRREG, reg, MODE_READ);
	if (xmmd >= 0)
	{
		if (xmmhilo >= 0)
		{
			if (upper) {
//                xMOVHL.PS(xRegisterSSE(xmmd), xRegisterSSE(xmmhilo));
                armAsm->Mov(a64::QRegister(xmmd).V2D(), 1, a64::QRegister(xmmhilo).V2D(), 0);
            }
			else {
//                xMOVSD(xRegisterSSE(xmmd), xRegisterSSE(xmmhilo));
                armAsm->Mov(a64::QRegister(xmmd).V2D(), 0, a64::QRegister(xmmhilo).V2D(), 0);
            }
		}
		else
		{
			const int gprhilo = upper ? -1 : _allocIfUsedGPRtoX86(reg, MODE_READ);
			if (gprhilo >= 0) {
//                xPINSR.Q(xRegisterSSE(xmmd), xRegister64(gprhilo), 0);
                armAsm->Ins(a64::QRegister(xmmd).V2D(), 0, HostX(gprhilo));
            }
			else {
//                xPINSR.Q(xRegisterSSE(xmmd), ptr64[hi ? &cpuRegs.HI.UD[static_cast<u8>(upper)]
//                                                      : &cpuRegs.LO.UD[static_cast<u8>(upper)]], 0);
                armLoad(REX, hi ? PTR_CPU(cpuRegs.HI.UD[static_cast<u8>(upper)]) : PTR_CPU(cpuRegs.LO.UD[static_cast<u8>(upper)]));
                armAsm->Ins(a64::QRegister(xmmd).V2D(), 0, REX);
            }
		}
	}
	else
	{
		// try rename {hi,lo} -> rd
		const int gprreg = upper ? -1 : _checkX86reg(X86TYPE_GPR, reg, MODE_READ);
		if (gprreg >= 0 && _eeTryRenameReg(_Rd_, reg, gprreg, -1, 0) >= 0)
			return;

		const int gprd = _allocIfUsedGPRtoX86(_Rd_, MODE_WRITE);
		if (gprd >= 0 && xmmhilo >= 0)
		{
			pxAssert(gprreg < 0);
			if (upper) {
//                xPEXTR.Q(xRegister64(gprd), xRegisterSSE(xmmhilo), 1);
                armAsm->Fmov(HostX(gprd), a64::QRegister(xmmhilo).V1D(), 1);
            }
			else {
//                xMOVD(xRegister64(gprd), xRegisterSSE(xmmhilo));
                armAsm->Fmov(HostX(gprd), a64::QRegister(xmmhilo).V1D());
            }
		}
		else if (gprd < 0 && xmmhilo >= 0)
		{
			pxAssert(gprreg < 0);
			if (upper) {
//                xPEXTR.Q(ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], xRegisterSSE(xmmhilo), 1);
                armAsm->Fmov(REX, a64::QRegister(xmmhilo).V1D(), 1);
                armStore(PTR_CPU(cpuRegs.GPR.r[_Rd_].UD[0]), REX);
            }
			else {
//                xMOVQ(ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], xRegisterSSE(xmmhilo));
                armStore(PTR_CPU(cpuRegs.GPR.r[_Rd_].UD[0]), a64::QRegister(xmmhilo).V1D());
            }
		}
		else if (gprd >= 0)
		{
			if (gprreg >= 0) {
//                xMOV(xRegister64(gprd), xRegister64(gprreg));
                armAsm->Mov(HostX(gprd), HostX(gprreg));
            }
			else {
//                xMOV(xRegister64(gprd), ptr64[hi ? &cpuRegs.HI.UD[static_cast<u8>(upper)]
//                                                 : &cpuRegs.LO.UD[static_cast<u8>(upper)]]);
                armLoad(HostX(gprd), hi ? PTR_CPU(cpuRegs.HI.UD[static_cast<u8>(upper)]) : PTR_CPU(cpuRegs.LO.UD[static_cast<u8>(upper)]));
            }
		}
		else if (gprreg >= 0)
		{
//			xMOV(ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], xRegister64(gprreg));
            armStore(PTR_CPU(cpuRegs.GPR.r[_Rd_].UD[0]), HostX(gprreg));
		}
		else
		{
//			xMOV(rax, ptr64[hi ? &cpuRegs.HI.UD[static_cast<u8>(upper)] : &cpuRegs.LO.UD[static_cast<u8>(upper)]]);
            armLoad(RAX, hi ? PTR_CPU(cpuRegs.HI.UD[static_cast<u8>(upper)]) : PTR_CPU(cpuRegs.LO.UD[static_cast<u8>(upper)]));
//			xMOV(ptr64[&cpuRegs.GPR.r[_Rd_].UD[0]], rax);
            armStore(PTR_CPU(cpuRegs.GPR.r[_Rd_].UD[0]), RAX);
		}
	}
}

static void recMTHILO(bool hi, bool upper)
{
	const int reg = hi ? XMMGPR_HI : XMMGPR_LO;
	_eeOnWriteReg(reg, 0);

	const int xmms = EEINST_XMMUSEDTEST(_Rs_) ? _allocGPRtoXMMreg(_Rs_, MODE_READ) : _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ);
	const int xmmhilo = EEINST_XMMUSEDTEST(reg) ? _allocGPRtoXMMreg(reg, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, reg, MODE_READ | MODE_WRITE);
	if (xmms >= 0)
	{
		if (xmmhilo >= 0)
		{
			if (upper) {
//                xMOVLH.PS(xRegisterSSE(xmmhilo), xRegisterSSE(xmms));
                armAsm->Mov(a64::QRegister(xmmhilo).V2D(), 1, a64::QRegister(xmms).V2D(), 0);
            }
			else {
//                xMOVSD(xRegisterSSE(xmmhilo), xRegisterSSE(xmms));
                armAsm->Mov(a64::QRegister(xmmhilo).V2D(), 0, a64::QRegister(xmms).V2D(), 0);
            }
		}
		else
		{
			const int gprhilo = upper ? -1 : _allocIfUsedGPRtoX86(reg, MODE_WRITE);
			if (gprhilo >= 0) {
//                xMOVD(xRegister64(gprhilo), xRegisterSSE(xmms)); // actually movq
                armAsm->Fmov(HostX(gprhilo), a64::QRegister(xmms).V1D());
            }
			else {
//                xMOVQ(ptr64[hi ? &cpuRegs.HI.UD[static_cast<u8>(upper)]
//                               : &cpuRegs.LO.UD[static_cast<u8>(upper)]], xRegisterSSE(xmms));
                armStore(hi ? PTR_CPU(cpuRegs.HI.UD[static_cast<u8>(upper)]) : PTR_CPU(cpuRegs.LO.UD[static_cast<u8>(upper)]), a64::QRegister(xmms).V1D());
            }
		}
	}
	else
	{
		int gprs = _allocIfUsedGPRtoX86(_Rs_, MODE_READ);

		if (xmmhilo >= 0)
		{
			if (gprs >= 0)
			{
//				xPINSR.Q(xRegisterSSE(xmmhilo), xRegister64(gprs), static_cast<u8>(upper));
                armAsm->Ins(a64::QRegister(xmmhilo).V2D(), static_cast<u8>(upper), HostX(gprs));
			}
			else if (GPR_IS_CONST1(_Rs_))
			{
				// force it into a register, since we need to load the constant anyway
				gprs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
//				xPINSR.Q(xRegisterSSE(xmmhilo), xRegister64(gprs), static_cast<u8>(upper));
                armAsm->Ins(a64::QRegister(xmmhilo).V2D(), static_cast<u8>(upper), HostX(gprs));
			}
			else
			{
//				xPINSR.Q(xRegisterSSE(xmmhilo), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], static_cast<u8>(upper));
                armAsm->Ins(a64::QRegister(xmmhilo).V2D(), static_cast<u8>(upper), armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])));
			}
		}
		else
		{
			// try rename rs -> {hi,lo}
			if (gprs >= 0 && !upper && _eeTryRenameReg(reg, _Rs_, gprs, -1, 0) >= 0)
				return;

			const int gprreg = upper ? -1 : _allocIfUsedGPRtoX86(reg, MODE_WRITE);
			if (gprreg >= 0)
			{
				_eeMoveGPRtoR(HostX(gprreg), _Rs_);
			}
			else
			{
				// force into a register, since we need to load it to write anyway
				gprs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
//				xMOV(ptr64[hi ? &cpuRegs.HI.UD[static_cast<u8>(upper)] : &cpuRegs.LO.UD[static_cast<u8>(upper)]], xRegister64(gprs));
                armStore(hi ? PTR_CPU(cpuRegs.HI.UD[static_cast<u8>(upper)]) : PTR_CPU(cpuRegs.LO.UD[static_cast<u8>(upper)]), HostX(gprs));
			}
		}
	}
}


void recMFHI()
{
	recMFHILO(true, false);
	EE::Profiler.EmitOp(eeOpcode::MFHI);
}

void recMFLO()
{
	recMFHILO(false, false);
	EE::Profiler.EmitOp(eeOpcode::MFLO);
}

void recMTHI()
{
	recMTHILO(true, false);
	EE::Profiler.EmitOp(eeOpcode::MTHI);
}

void recMTLO()
{
	recMTHILO(false, false);
	EE::Profiler.EmitOp(eeOpcode::MTLO);
}

void recMFHI1()
{
	recMFHILO(true, true);
	EE::Profiler.EmitOp(eeOpcode::MFHI1);
}

void recMFLO1()
{
	recMFHILO(false, true);
	EE::Profiler.EmitOp(eeOpcode::MFLO1);
}

void recMTHI1()
{
	recMTHILO(true, true);
	EE::Profiler.EmitOp(eeOpcode::MTHI1);
}

void recMTLO1()
{
	recMTHILO(false, true);
	EE::Profiler.EmitOp(eeOpcode::MTLO1);
}

//// MOVZ
// if (rt == 0) then rd <- rs
static void recMOVZtemp_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0];
}

static void recMOVZtemp_consts(int info)
{
	// [iter676h FIX] consts: rs is constant. Avoid _allocX86reg which may evict
	// registers needed by other operands. Load rs from memory instead.
	if (info & PROCESS_EE_T) {
		armAsm->Tst(HostX(EEREC_T), HostX(EEREC_T));
	}
	else {
		armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])), 0);
	}

	auto regX = HostX(EEREC_D);
	if (info & PROCESS_EE_S) {
		armAsm->Csel(regX, HostX(EEREC_S), regX, a64::Condition::eq);
	}
	else {
		// [iter681] FIX: Rs is const-folded — use g_cpuConstRegs, NOT cpuRegs memory.
		// cpuRegs.GPR.r[Rs] may contain a stale value from before the const-fold.
		armAsm->Mov(RXVIXLSCRATCH, (u64)g_cpuConstRegs[_Rs_].UD[0]);
		armAsm->Csel(regX, RXVIXLSCRATCH, regX, a64::Condition::eq);
	}
}

static void recMOVZtemp_constt(int info)
{
	// [iter676h FIX] constt: rt is constant zero → condition always true → unconditional move.
	// Previous code used Csel with stale flags and potentially invalid EEREC_S when PROCESS_EE_S=0.
	if (info & PROCESS_EE_S) {
		armAsm->Mov(HostX(EEREC_D), HostX(EEREC_S));
	}
	else {
		auto rsVal = armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0]));
		armAsm->Mov(HostX(EEREC_D), rsVal);
	}
}

static void recMOVZtemp_(int info)
{
	if (info & PROCESS_EE_T) {
//        xTEST(xRegister64(HostGprPhys(EEREC_T)), xRegister64(HostGprPhys(EEREC_T)));
        armAsm->Tst(HostX(EEREC_T), HostX(EEREC_T));
    }
	else {
//        xCMP(ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]], 0);
        armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])), 0);
    }

    auto reg64 = HostX(EEREC_D);
	if (info & PROCESS_EE_S) {
//        xCMOVE(xRegister64(HostGprPhys(EEREC_D)), xRegister64(HostGprPhys(EEREC_S)));
        armAsm->Csel(reg64, HostX(EEREC_S), reg64, a64::Condition::eq);
    }
	else {
//        xCMOVE(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
        armAsm->Csel(reg64, armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])), reg64, a64::Condition::eq);
    }
}

// Specify READD here, because we might not write to it, and want to preserve the value.
static EERECOMPILE_CODERC0(MOVZtemp, XMMINFO_READS | XMMINFO_READT | XMMINFO_READD | XMMINFO_WRITED | XMMINFO_NORENAME);

void recMOVZ()
{
	if (_Rs_ == _Rd_)
		return;

	if (GPR_IS_CONST1(_Rt_) && g_cpuConstRegs[_Rt_].UD[0] != 0)
		return;

	recMOVZtemp();
}

//// MOVN
static void recMOVNtemp_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0];
}

static void recMOVNtemp_consts(int info)
{
	// [iter676h FIX] consts: rs is constant. Avoid _allocX86reg which may evict
	// registers needed by other operands. Load rs from memory instead.
	if (info & PROCESS_EE_T) {
		armAsm->Tst(HostX(EEREC_T), HostX(EEREC_T));
	}
	else {
		armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])), 0);
	}

	auto reg64 = HostX(EEREC_D);
	if (info & PROCESS_EE_S) {
		armAsm->Csel(reg64, HostX(EEREC_S), reg64, a64::Condition::ne);
	}
	else {
		// [iter681] FIX: Rs is const-folded — use g_cpuConstRegs, NOT cpuRegs memory.
		armAsm->Mov(RXVIXLSCRATCH, (u64)g_cpuConstRegs[_Rs_].UD[0]);
		armAsm->Csel(reg64, RXVIXLSCRATCH, reg64, a64::Condition::ne);
	}
}

static void recMOVNtemp_constt(int info)
{
	// [iter676h FIX] constt: rt is constant non-zero → condition always true → unconditional move.
	// Previous code used Csel with stale flags and potentially invalid EEREC_S when PROCESS_EE_S=0.
	if (info & PROCESS_EE_S) {
		armAsm->Mov(HostX(EEREC_D), HostX(EEREC_S));
	}
	else {
		auto rsVal = armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0]));
		armAsm->Mov(HostX(EEREC_D), rsVal);
	}
}

static void recMOVNtemp_(int info)
{
	if (info & PROCESS_EE_T) {
		armAsm->Tst(HostX(EEREC_T), HostX(EEREC_T));
	}
	else {
		armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])), 0);
	}

	auto reg64 = HostX(EEREC_D);
	if (info & PROCESS_EE_S) {
		armAsm->Csel(reg64, HostX(EEREC_S), reg64, a64::Condition::ne);
	}
	else {
		armAsm->Csel(reg64, armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])), reg64, a64::Condition::ne);
	}
}

static EERECOMPILE_CODERC0(MOVNtemp, XMMINFO_READS | XMMINFO_READT | XMMINFO_READD | XMMINFO_WRITED | XMMINFO_NORENAME);

void recMOVN()
{
	if (_Rs_ == _Rd_)
		return;

	if (GPR_IS_CONST1(_Rt_) && g_cpuConstRegs[_Rt_].UD[0] == 0)
		return;

	recMOVNtemp();
}

#endif

} // namespace R5900::Dynarec::OpcodeImpl
