// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "x86/iR5900.h"

#if !defined(__ANDROID__)
using namespace x86Emitter;
#endif

// [iter676c] file-scope extern for JIT pc (defined in iR5900.cpp)
extern u32 pc;

// [TEMP_DIAG] @@SLTIU_RUNTIME@@ probe for FFX loop at 0x10b354
// Removal condition: SLTIU loopissue解決後
extern "C" void armsx2_sltiu_probe_10b354()
{
	static u32 s_n = 0;
	const u32 t2_val = cpuRegs.GPR.r[10].UL[0]; // t2 = $10
	const u32 v1_val = cpuRegs.GPR.r[3].UL[0];  // v1 = $3
	const u64 v1_64  = cpuRegs.GPR.r[3].UD[0];
	// Log first 20 calls, then every 1 millionth
	if (s_n < 20 || (s_n % 1000000 == 0)) {
		Console.WriteLn("@@SLTIU_RUNTIME@@ n=%u v1_32=%08x v1_64=%016llx t2=%08x expected_t2=%d",
			s_n, v1_val, v1_64, t2_val, (v1_64 < 6) ? 1 : 0);
	}
	s_n++;
}

namespace R5900::Dynarec::OpcodeImpl
{
/*********************************************************
* Arithmetic with immediate operand                      *
* Format:  OP rt, rs, immediate                          *
*********************************************************/

#ifndef ARITHMETICIMM_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_FUNC_DEL(ADDI, _Rt_);
REC_FUNC_DEL(ADDIU, _Rt_);
REC_FUNC_DEL(DADDI, _Rt_);
REC_FUNC_DEL(DADDIU, _Rt_);
REC_FUNC_DEL(ANDI, _Rt_);
REC_FUNC_DEL(ORI, _Rt_);
REC_FUNC_DEL(XORI, _Rt_);

REC_FUNC_DEL(SLTI, _Rt_);
REC_FUNC_DEL(SLTIU, _Rt_);

#else

static void recMoveStoT(int info)
{
	if (info & PROCESS_EE_S) {
//        xMOV(xRegister32(HostGprPhys(EEREC_T)), xRegister32(HostGprPhys(EEREC_S)));
        armAsm->Mov(HostW(EEREC_T), HostW(EEREC_S));
    }
	else {
//        xMOV(xRegister32(HostGprPhys(EEREC_T)), ptr32[&cpuRegs.GPR.r[_Rs_].UL[0]]);
        armLoad(HostW(EEREC_T), PTR_CPU(cpuRegs.GPR.r[_Rs_].UL[0]));
    }
}

static void recMoveStoT64(int info)
{
	if (info & PROCESS_EE_S) {
//        xMOV(xRegister64(HostGprPhys(EEREC_T)), xRegister64(HostGprPhys(EEREC_S)));
        armAsm->Mov(HostX(EEREC_T), HostX(EEREC_S));
    }
	else {
//        xMOV(xRegister64(HostGprPhys(EEREC_T)), ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]]);
        armLoad(HostX(EEREC_T), PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0]));
    }
}

//// ADDI
static void recADDI_const(void)
{
	g_cpuConstRegs[_Rt_].SD[0] = s64(s32(g_cpuConstRegs[_Rs_].UL[0] + u32(s32(_Imm_))));

    // FIX: If we are in a delay slot, we MUST materialize this constant into the register
    // because the branch will be taken immediately after, and the target block might
    // rely on the physical register value (or the constant state might be lost/mismatched).
    if (g_recompilingDelaySlot) {
        _flushConstReg(_Rt_);
    }
}

extern "C" void recRuntimeForceStoreT0(u32 pc, uintptr_t addr);

static void recADDI_(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));
	recMoveStoT(info);
//	xADD(xRegister32(HostGprPhys(EEREC_T)), _Imm_);
    armAsm->Add(HostW(EEREC_T), HostW(EEREC_T), _Imm_);
//	xMOVSX(xRegister64(HostGprPhys(EEREC_T)), xRegister32(HostGprPhys(EEREC_T)));
	armAsm->Sxtw(HostX(EEREC_T), HostW(EEREC_T));
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ADDI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_NORENAME);

////////////////////////////////////////////////////
void recADDIU()
{
	recADDI();
}

////////////////////////////////////////////////////
static void recDADDI_const()
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] + u64(s64(_Imm_));
}

static void recDADDI_(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));
	recMoveStoT64(info);
//	xADD(xRegister64(HostGprPhys(EEREC_T)), _Imm_);
    armAsm->Add(HostX(EEREC_T), HostX(EEREC_T), _Imm_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, DADDI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP | XMMINFO_NORENAME);

//// DADDIU
void recDADDIU()
{
	recDADDI();
}

//// SLTIU
static void recSLTIU_const()
{
	// [TEMP_DIAG] log ALL const SLTIU compilations
	{
		static int s_c = 0;
		if (s_c < 200 || pc == 0x10b354) {
			Console.WriteLn("@@SLTIU_CONST@@ n=%d pc=%08x Rs=$%d=%016llx Imm=%d result=%llu",
				s_c, pc, _Rs_, g_cpuConstRegs[_Rs_].UD[0], (s16)_Imm_,
				(u64)(g_cpuConstRegs[_Rs_].UD[0] < (u64)(_Imm_)));
		}
		s_c++;
	}
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] < (u64)(_Imm_);
}

static void recSLTIU_(int info)
{
	pxAssert(!(info & PROCESS_EE_XMM));

	// [TEMP_DIAG] confirm recSLTIU_ is called — log ALL calls, throttled
	{
		static int s_sltiu_all = 0;
		if (s_sltiu_all < 200 || pc == 0x10b354) {
			Console.WriteLn("@@SLTIU_COMPILE@@ n=%d pc=%08x Rs=%d Rt=%d Imm=%d info=%x",
				s_sltiu_all, pc, _Rs_, _Rt_, (s16)_Imm_, info);
		}
		s_sltiu_all++;
	}

	// [iter676e] FIX: _Rt_ == _Rs_ ケースで swap+free パターンが後続命令のregister解決を壊す。
	// scratch register (x16) 経由で CMP し、resultを直接 EEREC_T に書き込む。
	if (_Rt_ == _Rs_)
	{
		// Source is in EEREC_S. Save to scratch for CMP, write result to same slot.
		if (info & PROCESS_EE_S) {
			armAsm->Mov(RXVIXLSCRATCH, HostX(EEREC_S));
			armAsm->Cmp(RXVIXLSCRATCH, _Imm_);
		} else {
			armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])), _Imm_);
		}
		armAsm->Cset(HostW(EEREC_T), a64::Condition::cc);
		// Zero-extend W to X (Cset only sets 32-bit, top 32 auto-zeroed on ARM64)
		// No extra instruction needed: ARM64 Cset Wd zeroes top 32 bits of Xd.
	}
	else
	{
		// [P16 FIX] Removed redundant EOR that destroyed source register when
		// EEREC_T and EEREC_S shared the same host register due to register
		// allocator pressure. CSET produces 0/1 directly; no pre-clear needed.
		// EOR(EEREC_T) before CMP(EEREC_S) was zeroing the source operand,
		// causing SLTIU to always return 1 (0 < imm = true).
		if (info & PROCESS_EE_S)
			armAsm->Cmp(HostX(EEREC_S), _Imm_);
		else
			armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])), _Imm_);
		armAsm->Cset(HostW(EEREC_T), a64::Condition::cc);
	}

	// [iter660_fix] FIX: Force-store CSET result — same eviction bug as SLT (see iR5900Arit.cpp).
	if (_Rt_ != 0) {
		armStore(PTR_CPU(cpuRegs.GPR.r[_Rt_].UL[0]), a64::XRegister(HostGprPhys(EEREC_T)));
	}

	// [TEMP_DIAG] @@SLTIU_RUNTIME@@ — runtime probe for FFX loop exit diagnosis
	// Removal condition: SLTIU loopissue解決後
	// [REMOVED] SLTIU runtime probe — EOR bug confirmed fixed, probe removed for performance
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, SLTIU, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP | XMMINFO_NORENAME);

//// SLTI
static void recSLTI_const()
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].SD[0] < (s64)(_Imm_);
}

static void recSLTI_(int info)
{
	// [iter676e] FIX: same swap+free bug as SLTIU — apply same scratch-based fix
	if (_Rt_ == _Rs_)
	{
		if (info & PROCESS_EE_S) {
			armAsm->Mov(RXVIXLSCRATCH, HostX(EEREC_S));
			armAsm->Cmp(RXVIXLSCRATCH, _Imm_);
		} else {
			armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])), _Imm_);
		}
		armAsm->Cset(HostW(EEREC_T), a64::Condition::lt);
	}
	else
	{
		// [P16 FIX] Same EOR removal as recSLTIU — prevents source register destruction
		if (info & PROCESS_EE_S)
			armAsm->Cmp(HostX(EEREC_S), _Imm_);
		else
			armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])), _Imm_);
		armAsm->Cset(HostW(EEREC_T), a64::Condition::lt);
	}

	// [iter660_fix] FIX: Force-store CSET result — same eviction bug as SLT (see iR5900Arit.cpp).
	if (_Rt_ != 0) {
		armStore(PTR_CPU(cpuRegs.GPR.r[_Rt_].UL[0]), a64::XRegister(HostGprPhys(EEREC_T)));
	}
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, SLTI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP | XMMINFO_NORENAME);

//// ANDI
static void recANDI_const()
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] & (u64)_ImmU_; // Zero-extended Immediate
}

namespace
{
enum class LogicalOp
{
	AND,
	OR,
	XOR
};
} // namespace

static void recLogicalOpI(int info, LogicalOp op)
{
//	xImpl_G1Logic bad{};
//	const xImpl_G1Logic& xOP = op == LogicalOp::AND ? xAND : op == LogicalOp::OR ? xOR :
//														 op == LogicalOp::XOR    ? xXOR :
//                                                                                   bad;
//	pxAssert(&xOP != &bad);

	if (_ImmU_ != 0)
	{
		recMoveStoT64(info);
//		xOP(xRegister64(HostGprPhys(EEREC_T)), _ImmU_);

        auto reg64 = HostX(EEREC_T);

        switch (op)
        {
            case LogicalOp::AND:
                armAsm->And(reg64, reg64, _ImmU_);
                break;
            case LogicalOp::OR:
                armAsm->Orr(reg64, reg64, _ImmU_);
                break;
            case LogicalOp::XOR:
                armAsm->Eor(reg64, reg64, _ImmU_);
                break;
        }
	}
	else
	{
		if (op == LogicalOp::AND)
		{
//			xXOR(xRegister32(HostGprPhys(EEREC_T)), xRegister32(HostGprPhys(EEREC_T)));
            auto reg32 = HostW(EEREC_T);
            armAsm->Eor(reg32, reg32, reg32);
		}
		else
		{
			recMoveStoT64(info);
		}
	}
}

static void recANDI_(int info)
{
	recLogicalOpI(info, LogicalOp::AND);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ANDI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP | XMMINFO_NORENAME);

////////////////////////////////////////////////////
static void recORI_const()
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] | (u64)_ImmU_; // Zero-extended Immediate
}

static void recORI_(int info)
{
	recLogicalOpI(info, LogicalOp::OR);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, ORI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP | XMMINFO_NORENAME);

////////////////////////////////////////////////////
static void recXORI_const()
{
	g_cpuConstRegs[_Rt_].UD[0] = g_cpuConstRegs[_Rs_].UD[0] ^ (u64)_ImmU_; // Zero-extended Immediate
}

static void recXORI_(int info)
{
	recLogicalOpI(info, LogicalOp::XOR);
}

EERECOMPILE_CODEX(eeRecompileCodeRC1, XORI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_64BITOP | XMMINFO_NORENAME);

#endif

} // namespace R5900::Dynarec::OpcodeImpl
