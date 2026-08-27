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
* Register arithmetic                                    *
* Format:  OP rd, rs, rt                                 *
*********************************************************/

// TODO: overflow checks

#ifndef ARITHMETIC_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_FUNC_DEL(ADD, _Rd_);
REC_FUNC_DEL(ADDU, _Rd_);
REC_FUNC_DEL(DADD, _Rd_);
REC_FUNC_DEL(DADDU, _Rd_);
REC_FUNC_DEL(SUB, _Rd_);
REC_FUNC_DEL(SUBU, _Rd_);
REC_FUNC_DEL(DSUB, _Rd_);
REC_FUNC_DEL(DSUBU, _Rd_);
REC_FUNC_DEL(AND, _Rd_);
REC_FUNC_DEL(OR, _Rd_);
REC_FUNC_DEL(XOR, _Rd_);
REC_FUNC_DEL(NOR, _Rd_);
REC_FUNC_DEL(SLT, _Rd_);
REC_FUNC_DEL(SLTU, _Rd_);

#else

static void recMoveStoD(int info)
{
	if (info & PROCESS_EE_S) {
//        xMOV(xRegister32(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_S)));
        armAsm->Mov(a64::WRegister(HostGprPhys(EEREC_D)), a64::WRegister(HostGprPhys(EEREC_S)));
    }
	else {
//        xMOV(xRegister32(HostGprPhys(EEREC_D)), ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
        armLoad(a64::WRegister(HostGprPhys(EEREC_D)), PTR_CPU(cpuRegs.GPR.r[_Rs_].UL[0]));
    }
}

static void recMoveStoD64(int info)
{
	if (info & PROCESS_EE_S) {
//        xMOV(xRegister64(HostGprPhys(EEREC_D)), xRegister64(HostGprPhys(EEREC_S)));
        armAsm->Mov(a64::XRegister(HostGprPhys(EEREC_D)), a64::XRegister(HostGprPhys(EEREC_S)));
    }
	else {
//        xMOV(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
        armLoad(a64::XRegister(HostGprPhys(EEREC_D)), PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0]));
    }
}

static void recMoveTtoD(int info)
{
	if (info & PROCESS_EE_T) {
//        xMOV(xRegister32(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_T)));
        armAsm->Mov(a64::WRegister(HostGprPhys(EEREC_D)), a64::WRegister(HostGprPhys(EEREC_T)));
    }
	else {
//        xMOV(xRegister32(HostGprPhys(EEREC_D)), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
        armLoad(a64::WRegister(HostGprPhys(EEREC_D)), PTR_CPU(cpuRegs.GPR.r[_Rt_].UL[0]));
    }
}

static void recMoveTtoD64(int info)
{
	if (info & PROCESS_EE_T) {
//        xMOV(xRegister64(HostGprPhys(EEREC_D)), xRegister64(HostGprPhys(EEREC_T)));
        armAsm->Mov(a64::XRegister(HostGprPhys(EEREC_D)), a64::XRegister(HostGprPhys(EEREC_T)));
    }
	else {
//        xMOV(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
        armLoad(a64::XRegister(HostGprPhys(EEREC_D)), PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0]));
    }
}

//// ADD
static void recADD_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = s64(s32(g_cpuConstRegs[_Rs_].UL[0] + g_cpuConstRegs[_Rt_].UL[0]));
}

// s is constant
static void recADD_consts(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	const s32 cval = g_cpuConstRegs[_Rs_].SL[0];
	recMoveTtoD(info);
	if (cval != 0) {
//        xADD(xRegister32(HostGprPhys(EEREC_D)), cval);
        armAsm->Add(a64::WRegister(HostGprPhys(EEREC_D)), a64::WRegister(HostGprPhys(EEREC_D)), cval);
    }
//	xMOVSX(xRegister64(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_D)));
    armAsm->Sxtw(a64::XRegister(HostGprPhys(EEREC_D)), a64::WRegister(HostGprPhys(EEREC_D)));
}

// t is constant
static void recADD_constt(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	const s32 cval = g_cpuConstRegs[_Rt_].SL[0];
	recMoveStoD(info);
	if (cval != 0) {
//        xADD(xRegister32(HostGprPhys(EEREC_D)), cval);
        armAsm->Add(a64::WRegister(HostGprPhys(EEREC_D)), a64::WRegister(HostGprPhys(EEREC_D)), cval);
    }
//	xMOVSX(xRegister64(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_D)));
    armAsm->Sxtw(a64::XRegister(HostGprPhys(EEREC_D)), a64::WRegister(HostGprPhys(EEREC_D)));
}

// nothing is constant
static void recADD_(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

    auto reg32 = a64::WRegister(HostGprPhys(EEREC_D));

	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		// [R99 FIX] Detect Rs/Rt physical register collision.
		// Root cause: EERECOMPILE_CODEX allocates Rs first, then Rt. With 5 slots,
		// Rt's allocation can evict Rs's slot, making EEREC_S point to the same
		// physical register as EEREC_T. This causes ADD to compute Rt+Rt instead
		// of Rs+Rt. Fix: reload Rs from cpuRegs when collision detected.
		const bool phys_collision = (_Rs_ != _Rt_) &&
			(HostGprPhys(EEREC_S) == HostGprPhys(EEREC_T));
		if (phys_collision)
			armLoad(EEX, PTR_CPU(cpuRegs.GPR.r[_Rs_].UL[0]));
		const a64::WRegister rs_w(phys_collision ? EEX.GetCode() : HostGprPhys(EEREC_S));

		if (HostGprPhys(EEREC_D) == HostGprPhys(EEREC_S) && !phys_collision)
		{
            armAsm->Add(reg32, reg32, a64::WRegister(HostGprPhys(EEREC_T)));
		}
		else if (HostGprPhys(EEREC_D) == HostGprPhys(EEREC_T))
		{
            armAsm->Add(reg32, reg32, rs_w);
		}
		else
		{
            armAsm->Mov(reg32, rs_w);
            armAsm->Add(reg32, reg32, a64::WRegister(HostGprPhys(EEREC_T)));
		}
	}
	else if (info & PROCESS_EE_S)
	{
//		xMOV(xRegister32(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_S)));
        armAsm->Mov(reg32, a64::WRegister(HostGprPhys(EEREC_S)));
//		xADD(xRegister32(HostGprPhys(EEREC_D)), ptr32[&cpuRegs.GPR.r[_Rt_].UD[0]]);
        armAsm->Add(reg32, reg32, armLoad(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])));
	}
	else if (info & PROCESS_EE_T)
	{
//		xMOV(xRegister32(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_T)));
        armAsm->Mov(reg32, a64::WRegister(HostGprPhys(EEREC_T)));
//		xADD(xRegister32(HostGprPhys(EEREC_D)), ptr32[&cpuRegs.GPR.r[_Rs_].UD[0]]);
        armAsm->Add(reg32, reg32, armLoad(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])));
	}
	else
	{
//		xMOV(xRegister32(HostGprPhys(EEREC_D)), ptr32[&cpuRegs.GPR.r[_Rs_].UD[0]]);
        armLoad(reg32, PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0]));
//		xADD(xRegister32(HostGprPhys(EEREC_D)), ptr32[&cpuRegs.GPR.r[_Rt_].UD[0]]);
        armAsm->Add(reg32, reg32, armLoad(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])));
	}

//	xMOVSX(xRegister64(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_D)));
    armAsm->Sxtw(a64::XRegister(HostGprPhys(EEREC_D)), reg32);
}

EERECOMPILE_CODERC0(ADD, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_NORENAME);

//// ADDU
void recADDU(void)
{
	recADD();
}

//// DADD
void recDADD_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] + g_cpuConstRegs[_Rt_].UD[0];
}

// s is constant
static void recDADD_consts(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	const s64 cval = g_cpuConstRegs[_Rs_].SD[0];
	recMoveTtoD64(info);
	if (cval != 0) {
//        xImm64Op(xADD, xRegister64(HostGprPhys(EEREC_D)), rax, cval);
        armAsm->Add(a64::XRegister(HostGprPhys(EEREC_D)), a64::XRegister(HostGprPhys(EEREC_D)), cval);
    }
}

// t is constant
static void recDADD_constt(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	const s64 cval = g_cpuConstRegs[_Rt_].SD[0];
	recMoveStoD64(info);
	if (cval != 0) {
//        xImm64Op(xADD, xRegister64(HostGprPhys(EEREC_D)), rax, cval);
        armAsm->Add(a64::XRegister(HostGprPhys(EEREC_D)), a64::XRegister(HostGprPhys(EEREC_D)), cval);
    }
}

// nothing is constant
static void recDADD_(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

    auto reg64 = a64::XRegister(HostGprPhys(EEREC_D));

	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		// [R100 FIX] Rs/Rt physical register collision detection (64-bit).
		const bool phys_collision = (_Rs_ != _Rt_) &&
			(HostGprPhys(EEREC_S) == HostGprPhys(EEREC_T));
		if (phys_collision)
			armLoad(REX, PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0]));
		const a64::XRegister rs_x(phys_collision ? REX.GetCode() : HostGprPhys(EEREC_S));

		if (HostGprPhys(EEREC_D) == HostGprPhys(EEREC_S) && !phys_collision)
		{
            armAsm->Add(reg64, reg64, a64::XRegister(HostGprPhys(EEREC_T)));
		}
		else if (HostGprPhys(EEREC_D) == HostGprPhys(EEREC_T))
		{
            armAsm->Add(reg64, reg64, rs_x);
		}
		else
		{
            armAsm->Mov(reg64, rs_x);
            armAsm->Add(reg64, reg64, a64::XRegister(HostGprPhys(EEREC_T)));
		}
	}
	else if (info & PROCESS_EE_S)
	{
//		xMOV(xRegister64(HostGprPhys(EEREC_D)), xRegister64(HostGprPhys(EEREC_S)));
        armAsm->Mov(reg64, a64::XRegister(HostGprPhys(EEREC_S)));
//		xADD(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
        armAsm->Add(reg64, reg64, armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])));
	}
	else if (info & PROCESS_EE_T)
	{
//		xMOV(xRegister64(HostGprPhys(EEREC_D)), xRegister64(HostGprPhys(EEREC_T)));
        armAsm->Mov(reg64, a64::XRegister(HostGprPhys(EEREC_T)));
//		xADD(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
        armAsm->Add(reg64, reg64, armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])));
	}
	else
	{
//		xMOV(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
        armLoad(reg64, PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0]));
//		xADD(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
        armAsm->Add(reg64, reg64, armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])));
	}
}

EERECOMPILE_CODERC0(DADD, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_64BITOP | XMMINFO_NORENAME);

//// DADDU
void recDADDU(void)
{
	recDADD();
}

//// SUB

static void recSUB_const()
{
	g_cpuConstRegs[_Rd_].SD[0] = s64(s32(g_cpuConstRegs[_Rs_].UL[0] - g_cpuConstRegs[_Rt_].UL[0]));
}

static void recSUB_consts(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	const s32 sval = g_cpuConstRegs[_Rs_].SL[0];
//	xMOV(eax, sval);
    armAsm->Mov(EAX, sval);

	if (info & PROCESS_EE_T) {
//        xSUB(eax, xRegister32(HostGprPhys(EEREC_T)));
        armAsm->Sub(EAX, EAX, a64::WRegister(HostGprPhys(EEREC_T)));
    }
	else {
//        xSUB(eax, ptr32[&cpuRegs.GPR.r[_Rt_].SL[0]]);
        armAsm->Sub(EAX, EAX, armLoad(PTR_CPU(cpuRegs.GPR.r[_Rt_].SL[0])));
    }

//	xMOVSX(xRegister64(HostGprPhys(EEREC_D)), eax);
    armAsm->Sxtw(a64::XRegister(HostGprPhys(EEREC_D)), EAX);
}

static void recSUB_constt(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	const s32 tval = g_cpuConstRegs[_Rt_].SL[0];
	recMoveStoD(info);
	if (tval != 0) {
//        xSUB(xRegister32(HostGprPhys(EEREC_D)), tval);
        armAsm->Sub(a64::WRegister(HostGprPhys(EEREC_D)), a64::WRegister(HostGprPhys(EEREC_D)), tval);
    }

//	xMOVSX(xRegister64(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_D)));
    armAsm->Sxtw(a64::XRegister(HostGprPhys(EEREC_D)), a64::WRegister(HostGprPhys(EEREC_D)));
}

static void recSUB_(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

    auto reg32 = a64::WRegister(HostGprPhys(EEREC_D));

	if (_Rs_ == _Rt_)
	{
//		xXOR(xRegister32(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_D)));
        armAsm->Eor(reg32, reg32, reg32);
		return;
	}

	// a bit messier here because it's not commutative..
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		// [R100 FIX] Detect Rs/Rt physical register collision.
		// Same bug class as recADD_ (R100): Rt allocation can evict Rs's slot.
		const bool phys_collision = (_Rs_ != _Rt_) &&
			(HostGprPhys(EEREC_S) == HostGprPhys(EEREC_T));
		if (phys_collision)
			armLoad(EEX, PTR_CPU(cpuRegs.GPR.r[_Rs_].UL[0]));
		const a64::WRegister rs_w(phys_collision ? EEX.GetCode() : HostGprPhys(EEREC_S));

		if (HostGprPhys(EEREC_D) == HostGprPhys(EEREC_S) && !phys_collision)
		{
            armAsm->Sub(reg32, reg32, a64::WRegister(HostGprPhys(EEREC_T)));
            armAsm->Sxtw(a64::XRegister(HostGprPhys(EEREC_D)), reg32);
		}
		else if (HostGprPhys(EEREC_D) == HostGprPhys(EEREC_T))
		{
            armAsm->Mov(EAX, rs_w);
            armAsm->Sub(EAX, EAX, a64::WRegister(HostGprPhys(EEREC_T)));
            armAsm->Sxtw(a64::XRegister(HostGprPhys(EEREC_D)), EAX);
		}
		else
		{
            armAsm->Mov(reg32, rs_w);
            armAsm->Sub(reg32, reg32, a64::WRegister(HostGprPhys(EEREC_T)));
            armAsm->Sxtw(a64::XRegister(HostGprPhys(EEREC_D)), reg32);
		}
	}
	else if (info & PROCESS_EE_S)
	{
//		xMOV(xRegister32(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_S)));
        armAsm->Mov(reg32, a64::WRegister(HostGprPhys(EEREC_S)));
//		xSUB(xRegister32(HostGprPhys(EEREC_D)), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
        armAsm->Sub(reg32, reg32, armLoad(PTR_CPU(cpuRegs.GPR.r[_Rt_].UL[0])));
//		xMOVSX(xRegister64(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_D)));
        armAsm->Sxtw(a64::XRegister(HostGprPhys(EEREC_D)), reg32);
	}
	else if (info & PROCESS_EE_T)
	{
		// D might equal T
//		xMOV(eax, ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
        armLoad(EAX, PTR_CPU(cpuRegs.GPR.r[_Rs_].UL[0]));
//		xSUB(eax, xRegister32(HostGprPhys(EEREC_T)));
        armAsm->Sub(EAX, EAX, a64::WRegister(HostGprPhys(EEREC_T)));
//		xMOVSX(xRegister64(HostGprPhys(EEREC_D)), eax);
        armAsm->Sxtw(a64::XRegister(HostGprPhys(EEREC_D)), EAX);
	}
	else
	{
//		xMOV(xRegister32(HostGprPhys(EEREC_D)), ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
        armLoad(reg32, PTR_CPU(cpuRegs.GPR.r[_Rs_].UL[0]));
//		xSUB(xRegister32(HostGprPhys(EEREC_D)), ptr32[&cpuRegs.GPR.r[_Rt_].UL[0]]);
        armAsm->Sub(reg32, reg32, armLoad(PTR_CPU(cpuRegs.GPR.r[_Rt_].UL[0])));
//		xMOVSX(xRegister64(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_D)));
        armAsm->Sxtw(a64::XRegister(HostGprPhys(EEREC_D)), reg32);
	}
}

EERECOMPILE_CODERC0(SUB, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_NORENAME);

//// SUBU
void recSUBU(void)
{
	recSUB();
}

//// DSUB
static void recDSUB_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] - g_cpuConstRegs[_Rt_].UD[0];
}

static void recDSUB_consts(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	// gross, because if d == t, we can't destroy t
	const s64 sval = g_cpuConstRegs[_Rs_].SD[0];
	const a64::XRegister regd((info & PROCESS_EE_T && HostGprPhys(EEREC_D) == HostGprPhys(EEREC_T)) ? RAX.GetCode() : HostGprPhys(EEREC_D));
//	xMOV64(regd, sval);
    armAsm->Mov(regd, sval);

	if (info & PROCESS_EE_T) {
//        xSUB(regd, xRegister64(HostGprPhys(EEREC_T)));
        armAsm->Sub(regd, regd, a64::XRegister(HostGprPhys(EEREC_T)));
    }
	else {
//        xSUB(regd, ptr64[&cpuRegs.GPR.r[_Rt_].SD[0]]);
        armAsm->Sub(regd, regd, armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_].SD[0])));
    }

	// emitter will eliminate redundant moves.
//	xMOV(xRegister64(HostGprPhys(EEREC_D)), regd);
    armAsm->Mov(a64::XRegister(HostGprPhys(EEREC_D)), regd);
}

static void recDSUB_constt(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	const s64 tval = g_cpuConstRegs[_Rt_].SD[0];
	recMoveStoD64(info);
	if (tval != 0) {
//        xImm64Op(xSUB, xRegister64(HostGprPhys(EEREC_D)), rax, tval);
        armAsm->Sub(a64::XRegister(HostGprPhys(EEREC_D)), a64::XRegister(HostGprPhys(EEREC_D)), tval);
    }
}

static void recDSUB_(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	if (_Rs_ == _Rt_)
	{
//		xXOR(xRegister32(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_D)));
        auto reg32 = a64::WRegister(HostGprPhys(EEREC_D));
        armAsm->Eor(reg32, reg32, reg32);
		return;
	}

	// a bit messier here because it's not commutative..
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		// [R100 FIX] Rs/Rt physical register collision detection (64-bit SUB).
		const bool phys_collision = (_Rs_ != _Rt_) &&
			(HostGprPhys(EEREC_S) == HostGprPhys(EEREC_T));
		if (phys_collision)
			armLoad(REX, PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0]));
		const a64::XRegister rs_x(phys_collision ? REX.GetCode() : HostGprPhys(EEREC_S));

		// D might equal T — use RAX as intermediate
		const a64::XRegister regd(HostGprPhys(EEREC_D) == HostGprPhys(EEREC_T) ? RAX.GetCode() : HostGprPhys(EEREC_D));
        armAsm->Mov(regd, rs_x);
        armAsm->Sub(regd, regd, a64::XRegister(HostGprPhys(EEREC_T)));
        armAsm->Mov(a64::XRegister(HostGprPhys(EEREC_D)), regd);
	}
	else if (info & PROCESS_EE_S)
	{
//		xMOV(xRegister64(HostGprPhys(EEREC_D)), xRegister64(HostGprPhys(EEREC_S)));
        armAsm->Mov(a64::XRegister(HostGprPhys(EEREC_D)), a64::XRegister(HostGprPhys(EEREC_S)));
//		xSUB(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
        armAsm->Sub(a64::XRegister(HostGprPhys(EEREC_D)), a64::XRegister(HostGprPhys(EEREC_D)), armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])));
	}
	else if (info & PROCESS_EE_T)
	{
		// D might equal T
		const a64::XRegister regd(HostGprPhys(EEREC_D) == HostGprPhys(EEREC_T) ? RAX.GetCode() : HostGprPhys(EEREC_D));
//		xMOV(regd, ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
        armLoad(regd, PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0]));
//		xSUB(regd, xRegister64(HostGprPhys(EEREC_T)));
        armAsm->Sub(regd, regd, a64::XRegister(HostGprPhys(EEREC_T)));
//		xMOV(xRegister64(HostGprPhys(EEREC_D)), regd);
        armAsm->Mov(a64::XRegister(HostGprPhys(EEREC_D)), regd);
	}
	else
	{
//		xMOV(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
        armLoad(a64::XRegister(HostGprPhys(EEREC_D)), PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0]));
//		xSUB(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[_Rt_].UD[0]]);
        armAsm->Sub(a64::XRegister(HostGprPhys(EEREC_D)), a64::XRegister(HostGprPhys(EEREC_D)), armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])));
	}
}

EERECOMPILE_CODERC0(DSUB, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP | XMMINFO_NORENAME);

//// DSUBU
void recDSUBU(void)
{
	recDSUB();
}

namespace
{
enum class LogicalOp
{
	AND,
	OR,
	XOR,
	NOR,
    BAD
};
} // namespace

static void recLogicalOp_constv(LogicalOp op, int info, int creg, u32 vreg, int regv)
{
	pxAssert(!(info & PROCESS_EE_XMM));

//	xImpl_G1Logic bad{};
//	const xImpl_G1Logic& xOP = op == LogicalOp::AND ? xAND : op == LogicalOp::OR ? xOR :
//														 op == LogicalOp::XOR    ? xXOR :
//														 op == LogicalOp::NOR    ? xOR :
//                                                                                   bad;
    const LogicalOp xOP = op == LogicalOp::AND ? LogicalOp::AND : op == LogicalOp::OR ? LogicalOp::OR :
                                                                  op == LogicalOp::XOR    ? LogicalOp::XOR :
                                                                  op == LogicalOp::NOR    ? LogicalOp::OR :
                                                                  LogicalOp::BAD;
	s64 fixedInput, fixedOutput, identityInput;
	bool hasFixed = true;
	switch (op)
	{
		case LogicalOp::AND:
			fixedInput = 0;
			fixedOutput = 0;
			identityInput = -1;
			break;
		case LogicalOp::OR:
			fixedInput = -1;
			fixedOutput = -1;
			identityInput = 0;
			break;
		case LogicalOp::XOR:
			hasFixed = false;
			identityInput = 0;
			break;
		case LogicalOp::NOR:
			fixedInput = -1;
			fixedOutput = 0;
			identityInput = 0;
			break;
		default:
			pxAssert(0);
	}

	GPR_reg64 cval = g_cpuConstRegs[creg];

    auto reg64 = a64::XRegister(HostGprPhys(EEREC_D));

	if (hasFixed && cval.SD[0] == fixedInput)
	{
//		xMOV64(xRegister64(HostGprPhys(EEREC_D)), fixedOutput);
        armAsm->Mov(reg64, fixedOutput);
	}
	else
	{
		if (regv >= 0) {
//            xMOV(xRegister64(HostGprPhys(EEREC_D)), xRegister64(regv));
            armAsm->Mov(reg64, a64::XRegister(regv));
        }
		else {
//            xMOV(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[vreg].UD[0]]);
            armLoad(reg64, PTR_CPU(cpuRegs.GPR.r[vreg].UD[0]));
        }
		if (cval.SD[0] != identityInput) {
//            xImm64Op(xOP, xRegister64(HostGprPhys(EEREC_D)), rax, cval.UD[0]);
            switch (xOP)
            {
                case LogicalOp::AND:
                    armAsm->And(reg64, reg64, cval.UD[0]);
                    break;
                case LogicalOp::NOR: case LogicalOp::OR:
                    armAsm->Orr(reg64, reg64, cval.UD[0]);
                    break;
                case LogicalOp::XOR:
                    armAsm->Eor(reg64, reg64, cval.UD[0]);
                    break;
                case LogicalOp::BAD:
                    break;
            }
        }
		if (op == LogicalOp::NOR) {
//            xNOT(xRegister64(HostGprPhys(EEREC_D)));
            armAsm->Mvn(reg64, reg64);
        }
	}
}

static void recLogicalOp(LogicalOp op, int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

//	xImpl_G1Logic bad{};
//	const xImpl_G1Logic& xOP = op == LogicalOp::AND ? xAND : op == LogicalOp::OR ? xOR :
//														 op == LogicalOp::XOR    ? xXOR :
//														 op == LogicalOp::NOR    ? xOR :
//                                                                                   bad;
//	pxAssert(&xOP != &bad);
    const LogicalOp xOP = op == LogicalOp::AND ? LogicalOp::AND : op == LogicalOp::OR ? LogicalOp::OR :
                                                                  op == LogicalOp::XOR    ? LogicalOp::XOR :
                                                                  op == LogicalOp::NOR    ? LogicalOp::OR :
                                                                  LogicalOp::BAD;

	// swap because it's commutative and Rd might be Rt
	u32 rs = _Rs_, rt = _Rt_;
	int regs = (info & PROCESS_EE_S) ? HostGprPhys(EEREC_S) : -1, regt = (info & PROCESS_EE_T) ? HostGprPhys(EEREC_T) : -1;
	if (_Rd_ == _Rt_)
	{
		std::swap(rs, rt);
		std::swap(regs, regt);
	}

	if (op == LogicalOp::XOR && rs == rt)
	{
//		xXOR(xRegister32(HostGprPhys(EEREC_D)), xRegister32(HostGprPhys(EEREC_D)));
        auto reg32 = a64::WRegister(HostGprPhys(EEREC_D));
        armAsm->Eor(reg32, reg32, reg32);
	}
	else
	{
        auto reg64 = a64::XRegister(HostGprPhys(EEREC_D));

		if (regs >= 0) {
//            xMOV(xRegister64(HostGprPhys(EEREC_D)), xRegister64(regs));
            armAsm->Mov(reg64, a64::XRegister(regs));
        }
		else {
//            xMOV(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[rs].UD[0]]);
            armLoad(reg64, PTR_CPU(cpuRegs.GPR.r[rs].UD[0]));
        }

		if (regt >= 0) {
//            xOP(xRegister64(HostGprPhys(EEREC_D)), xRegister64(regt));
            switch (xOP)
            {
                case LogicalOp::AND:
                    armAsm->And(reg64, reg64, a64::XRegister(regt));
                    break;
                case LogicalOp::NOR: case LogicalOp::OR:
                    armAsm->Orr(reg64, reg64, a64::XRegister(regt));
                    break;
                case LogicalOp::XOR:
                    armAsm->Eor(reg64, reg64, a64::XRegister(regt));
                    break;
                case LogicalOp::BAD:
                    break;
            }
        }
		else {
//            xOP(xRegister64(HostGprPhys(EEREC_D)), ptr64[&cpuRegs.GPR.r[rt].UD[0]]);
            switch (xOP)
            {
                case LogicalOp::AND:
                    armAsm->And(reg64, reg64, armLoad64(PTR_CPU(cpuRegs.GPR.r[rt].UD[0])));
                    break;
                case LogicalOp::NOR: case LogicalOp::OR:
                    armAsm->Orr(reg64, reg64, armLoad64(PTR_CPU(cpuRegs.GPR.r[rt].UD[0])));
                    break;
                case LogicalOp::XOR:
                    armAsm->Eor(reg64, reg64, armLoad64(PTR_CPU(cpuRegs.GPR.r[rt].UD[0])));
                    break;
                case LogicalOp::BAD:
                    break;
            }
        }

		if (op == LogicalOp::NOR) {
//            xNOT(xRegister64(HostGprPhys(EEREC_D)));
            armAsm->Mvn(reg64, reg64);
        }
	}
}

//// AND
static void recAND_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] & g_cpuConstRegs[_Rt_].UD[0];
}

static void recAND_consts(int info)
{
	recLogicalOp_constv(LogicalOp::AND, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? HostGprPhys(EEREC_T) : -1);
}

static void recAND_constt(int info)
{
	recLogicalOp_constv(LogicalOp::AND, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? HostGprPhys(EEREC_S) : -1);
}

static void recAND_(int info)
{
	recLogicalOp(LogicalOp::AND, info);
}

EERECOMPILE_CODERC0(AND, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP | XMMINFO_NORENAME);

//// OR
static void recOR_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] | g_cpuConstRegs[_Rt_].UD[0];
}

static void recOR_consts(int info)
{
	recLogicalOp_constv(LogicalOp::OR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? HostGprPhys(EEREC_T) : -1);
}

static void recOR_constt(int info)
{
	recLogicalOp_constv(LogicalOp::OR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? HostGprPhys(EEREC_S) : -1);
}

static void recOR_(int info)
{
	recLogicalOp(LogicalOp::OR, info);
}

EERECOMPILE_CODERC0(OR, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP | XMMINFO_NORENAME);

//// XOR
static void recXOR_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] ^ g_cpuConstRegs[_Rt_].UD[0];
}

static void recXOR_consts(int info)
{
	recLogicalOp_constv(LogicalOp::XOR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? HostGprPhys(EEREC_T) : -1);
}

static void recXOR_constt(int info)
{
	recLogicalOp_constv(LogicalOp::XOR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? HostGprPhys(EEREC_S) : -1);
}

static void recXOR_(int info)
{
	recLogicalOp(LogicalOp::XOR, info);
}

EERECOMPILE_CODERC0(XOR, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP | XMMINFO_NORENAME);

//// NOR
static void recNOR_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = ~(g_cpuConstRegs[_Rs_].UD[0] | g_cpuConstRegs[_Rt_].UD[0]);
}

static void recNOR_consts(int info)
{
	recLogicalOp_constv(LogicalOp::NOR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? HostGprPhys(EEREC_T) : -1);
}

static void recNOR_constt(int info)
{
	recLogicalOp_constv(LogicalOp::NOR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? HostGprPhys(EEREC_S) : -1);
}

static void recNOR_(int info)
{
	recLogicalOp(LogicalOp::NOR, info);
}

EERECOMPILE_CODERC0(NOR, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP | XMMINFO_NORENAME);

//// SLT - test with silent hill, lemans
static void recSLT_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].SD[0] < g_cpuConstRegs[_Rt_].SD[0];
}

static void recSLTs_const(int info, int sign, int st)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	const s64 cval = g_cpuConstRegs[st ? _Rt_ : _Rs_].SD[0];
	const a64::Condition& SET = st ? (sign ? a64::Condition::lt : a64::Condition::cc) : (sign ? a64::Condition::gt : a64::Condition::hi);

	const int non_const_gpr = st ? _Rs_ : _Rt_;
	const bool overlap = (_Rd_ == non_const_gpr);

	// [iter676e] FIX: overlap case — use scratch register instead of swap+free
	// which corrupts subsequent register resolution (same bug as SLTIU/SLTI).
	if (overlap)
	{
		const int regs = st ? ((info & PROCESS_EE_S) ? HostGprPhys(EEREC_S) : -1) : ((info & PROCESS_EE_T) ? HostGprPhys(EEREC_T) : -1);
		if (regs >= 0) {
			armAsm->Mov(RXVIXLSCRATCH, a64::XRegister(regs));
			armAsm->Cmp(RXVIXLSCRATCH, cval);
		} else {
			armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[non_const_gpr].UD[0])), cval);
		}
		armAsm->Cset(HostW(EEREC_D), SET);
	}
	else
	{
		// [R96 FIX] Removed EOR that destroyed source register when EEREC_D
		// shared the same host register as EEREC_S/EEREC_T due to register
		// allocator pressure. CSET produces 0/1 directly; no pre-clear needed.
		// Same bug pattern as SLTIU (fixed in R68/P16).
		const int regs = st ? ((info & PROCESS_EE_S) ? HostGprPhys(EEREC_S) : -1) : ((info & PROCESS_EE_T) ? HostGprPhys(EEREC_T) : -1);
		if (regs >= 0)
			armAsm->Cmp(a64::XRegister(regs), cval);
		else
			armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[non_const_gpr].UD[0])), cval);
		armAsm->Cset(HostW(EEREC_D), SET);
	}

	// [iter660_fix] FIX: Force-store CSET result — same eviction bug as recSLTs_ (see comment there).
	if (_Rd_ != 0) {
		armStore(PTR_CPU(cpuRegs.GPR.r[_Rd_].UL[0]), a64::XRegister(HostGprPhys(EEREC_D)));
	}
}

static void recSLTs_(int info, int sign)
{
	pxAssert(!(info & PROCESS_EE_XMM));
	const a64::Condition& SET = sign ? a64::Condition::lt : a64::Condition::cc;

	const bool overlap = (_Rd_ == _Rt_ || _Rd_ == _Rs_);

	// [iter682] FIX: _allocX86reg for Rs can evict EEREC_T's slot, making the
	// PROCESS_EE_T flag stale. Save Rt's physical register BEFORE the allocation.
	// If EEREC_T is evicted, fall back to reading Rt from cpuRegs memory.
	const int pre_alloc_t_slot = (info & PROCESS_EE_T) ? EEREC_T : -1;
	const int pre_alloc_t_phys = (info & PROCESS_EE_T) ? HostGprPhys(EEREC_T) : -1;

	// force Rs into a register, may as well cache it since we're loading anyway.
	// [iter242] slot→phys fix: _allocX86reg returns slot, must convert to phys
	const int regs_slot = (info & PROCESS_EE_S) ? -1 : _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	const int regs_phys = (info & PROCESS_EE_S) ? HostGprPhys(EEREC_S) : HostGprPhys(regs_slot);

	// [iter682] After allocation, check if EEREC_T was evicted.
	// If the slot was reused for Rs, HostGprPhys(EEREC_T) now points to Rs's value!
	const bool t_still_valid = (info & PROCESS_EE_T) &&
		(_checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) >= 0) &&
		(HostGprPhys(_checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ)) == pre_alloc_t_phys);

	// [iter676e] FIX: overlap case — use scratch register instead of swap+free
	if (overlap)
	{
		armAsm->Mov(RXVIXLSCRATCH, a64::XRegister(regs_phys));
		if (t_still_valid)
			armAsm->Cmp(RXVIXLSCRATCH, a64::XRegister(pre_alloc_t_phys));
		else
			armAsm->Cmp(RXVIXLSCRATCH, armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])));
		armAsm->Cset(HostW(EEREC_D), SET);
	}
	else
	{
		// [R96 FIX] Removed EOR that destroyed source register when EEREC_D
		// shared the same host register as EEREC_S/EEREC_T due to register
		// allocator pressure. CSET produces 0/1 directly; no pre-clear needed.
		// Same bug pattern as SLTIU (fixed in R68/P16).
		if (t_still_valid)
			armAsm->Cmp(a64::XRegister(regs_phys), a64::XRegister(pre_alloc_t_phys));
		else
			armAsm->Cmp(a64::XRegister(regs_phys), armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_].UD[0])));
		armAsm->Cset(HostW(EEREC_D), SET);
	}

	// [iter660_fix] FIX: Force-store CSET result to GPR[Rd] immediately after CSET.
	// Root cause: With only 5 allocator slots (ARM64 port), the CSET destination slot
	// is frequently evicted by subsequent instructions before the branch reads it.
	// The eviction writeback emits STR at compile time, but by runtime execution order,
	// the physical register (e.g. x19) may have been clobbered by an intervening
	// instruction's address computation or load before the writeback STR executes.
	// This forced store ensures the correct 0/1 value reaches GPR backing memory
	// while the physical register still holds the CSET result.
	if (_Rd_ != 0) {
		armStore(PTR_CPU(cpuRegs.GPR.r[_Rd_].UL[0]), a64::XRegister(HostGprPhys(EEREC_D)));
	}
}

static void recSLT_consts(int info)
{
	recSLTs_const(info, 1, 0);
}

static void recSLT_constt(int info)
{
	recSLTs_const(info, 1, 1);
}

static void recSLT_(int info)
{
	recSLTs_(info, 1);
}

EERECOMPILE_CODERC0(SLT, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_NORENAME);

// SLTU - test with silent hill, lemans
static void recSLTU_const()
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] < g_cpuConstRegs[_Rt_].UD[0];
}

static void recSLTU_consts(int info)
{
	recSLTs_const(info, 0, 0);
}

static void recSLTU_constt(int info)
{
	recSLTs_const(info, 0, 1);
}

static void recSLTU_(int info)
{
	recSLTs_(info, 0);
}

EERECOMPILE_CODERC0(SLTU, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_NORENAME);

#endif

} // namespace R5900::Dynarec::OpcodeImpl
