// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <ctime>

#include "iR3000A.h"
#include "IopMem.h"
#include "IopDma.h"
#include "IopGte.h"

#include "common/Console.h"


#if !defined(__ANDROID__)
using namespace x86Emitter;
#endif

extern int g_psxWriteOk;
extern u32 g_psxMaxRecMem;

static void antigravityLogIopBranchCmp(u32 branch_pc, u32 kind, u32 rs, u32 rt)
{
	// [iter122] @@ADV_BNE@@ – advance block BNE (psxpc=0xBFC02728) flush-point diagnostic.
	// Fires AFTER _psxFlushAllDirty() so psxRegs.r[9] reflects the JIT-committed t1 value.
	// Cap at 5 entries (fires every loop iteration so we only want the first few).
	if (branch_pc == 0xBFC02728u)
	{
		static int s_adv_n = 0;
		if (s_adv_n < 5)
		{
			++s_adv_n;
			Console.WriteLn(
				"@@ADV_BNE@@ n=%d t0=%08x t1=%08x t2=%08x v0=%08x",
				s_adv_n, psxRegs.GPR.r[8], psxRegs.GPR.r[9], psxRegs.GPR.r[10], psxRegs.GPR.r[2]);
		}
		return;
	}

	// [iter114/115] @@IOP_SCAN_BR@@ – trace branches inside the BIOS ROM scan function (bfc02640-bfc02730)
	// branch_pc = psxpc AFTER delay slot (not the branch instruction itself)
	// Exclude bfc02674 (BNE bfc0266c RESE mismatch, delay-slot-pc) and bfc02728 (BNE bfc02720 outer loop, delay-slot-pc)
	if (branch_pc >= 0xBFC02640u && branch_pc <= 0xBFC02730u &&
		branch_pc != 0xBFC02674u && branch_pc != 0xBFC02728u)
	{
		static int s_scan_n = 0;
		if (s_scan_n < 100)
		{
			++s_scan_n;
			const u32 lhs = (rs == 0) ? 0 : psxRegs.GPR.r[rs];
			const u32 rhs = (rt == 0) ? 0 : psxRegs.GPR.r[rt];
			// [iter116] also log t1 (r9) to verify LW v0,-8(t1) addr = t1-8 = t0+4
			Console.WriteLn(
				"@@IOP_SCAN_BR@@ n=%d pc=%08x lhs=%08x rhs=%08x eq=%u t0=%08x t1=%08x t2=%08x t3=%08x",
				s_scan_n, branch_pc, lhs, rhs, (lhs == rhs) ? 1u : 0u,
				psxRegs.GPR.r[8], psxRegs.GPR.r[9], psxRegs.GPR.r[10], psxRegs.GPR.r[11]);
		}
		return;
	}

	static int s_count = 0;
	if (s_count >= 80)
		return;
	++s_count;

	const u32 lhs = (rs == 0) ? 0 : psxRegs.GPR.r[rs];
	const u32 rhs = (rt == 0) ? 0 : psxRegs.GPR.r[rt];
	const u32 eq = (lhs == rhs) ? 1u : 0u;
	Console.WriteLn(
		"@@IOP_BR_CMP@@ n=%d pc=%08x kind=%u rs=%u rt=%u lhs=%08x rhs=%08x eq=%u code=%08x curpc=%08x a0=%08x v0=%08x v1=%08x",
		s_count, branch_pc, kind, rs, rt, lhs, rhs, eq, psxRegs.code, psxRegs.pc, psxRegs.GPR.n.a0, psxRegs.GPR.n.v0,
		psxRegs.GPR.n.v1);
}

// R3000A instruction implementation
#define REC_FUNC(f) \
	static void rpsx##f() \
	{ \
		armStore(PTR_CPU(psxRegs.code), (u32)psxRegs.code); \
		_psxFlushCall(FLUSH_EVERYTHING); \
		armEmitCall(reinterpret_cast<void*>((uptr)psx##f)); \
		PSX_DEL_CONST(_Rt_); \
		/*	branch = 2; */ \
	}

// Same as above but with a different naming convension (to avoid various rename)
#define REC_GTE_FUNC(f) \
	static void rgte##f() \
	{ \
		armStore(PTR_CPU(psxRegs.code), (u32)psxRegs.code); \
		_psxFlushCall(FLUSH_EVERYTHING); \
		armEmitCall(reinterpret_cast<void*>((uptr)gte##f)); \
		PSX_DEL_CONST(_Rt_); \
		/*	branch = 2; */ \
	}

extern void psxLWL();
extern void psxLWR();
extern void psxSWL();
extern void psxSWR();

// TODO(Stenzek): Operate directly on mem when destination register is not live.
// Do we want aligned targets? Seems wasteful...
#ifdef PCSX2_DEBUG
#define x86SetJ32A x86SetJ32
#endif

static int rpsxAllocRegIfUsed(int reg, int mode)
{
	if (EEINST_USEDTEST(reg))
		return _allocX86reg(X86TYPE_PSX, reg, mode);
	else
		return _checkX86reg(X86TYPE_PSX, reg, mode);
}

static void rpsxMoveStoT(int info)
{
	// [iter125] Reverted iter123 (_Rs_!=_Rt_ guard removed). In-place ops (_Rs_==_Rt_)
	// reuse the existing JIT slot (from LW WRITE alloc in same block) which already has
	// the correct value. armLoad of stale psxRegs is WRONG when the slot has live data.
	// Cross-block t1 case fixed separately via force-noconst at bfc02714 in iR3000A.cpp.
	if ((info & PROCESS_EE_S) && EEREC_T == EEREC_S)
		return;

    auto reg32 = HostW(EEREC_T);
	if ((info & PROCESS_EE_S) && EEREC_T != EEREC_S) {
//        xMOV(xRegister32(EEREC_T), xRegister32(EEREC_S));
        armAsm->Mov(reg32, HostW(EEREC_S));
    }
	else {
//        xMOV(xRegister32(EEREC_T), ptr32[&psxRegs.GPR.r[_Rs_]]);
        armLoad(reg32, PTR_CPU(psxRegs.GPR.r[_Rs_]));
    }
}

static void rpsxMoveStoD(int info)
{
	// [iter125] Reverted iter124 (_Rs_!=_Rd_ guard removed). Same reasoning as rpsxMoveStoT:
	// in-place ops reuse the existing JIT slot which already holds the correct value.
	if ((info & PROCESS_EE_S) && EEREC_D == EEREC_S)
		return;

    auto reg32 = HostW(EEREC_D);
	if ((info & PROCESS_EE_S) && EEREC_D != EEREC_S) {
//        xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
        armAsm->Mov(reg32, HostW(EEREC_S));
    }
	else {
//        xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rs_]]);
        armLoad(reg32, PTR_CPU(psxRegs.GPR.r[_Rs_]));
    }
}

static void rpsxMoveTtoD(int info)
{
	// [iter119] Same fix as rpsxMoveStoD: only skip when PROCESS_EE_T is genuinely set.
	if ((info & PROCESS_EE_T) && EEREC_D == EEREC_T)
		return;

    auto reg32 = HostW(EEREC_D);
	if (info & PROCESS_EE_T) {
//        xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
        armAsm->Mov(reg32, HostW(EEREC_T));
    }
	else {
//        xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rt_]]);
        armLoad(reg32, PTR_CPU(psxRegs.GPR.r[_Rt_]));
    }
}

static void rpsxMoveSToECX(int info)
{
	if (info & PROCESS_EE_S) {
//        xMOV(ecx, xRegister32(EEREC_S));
        armAsm->Mov(ECX, HostW(EEREC_S));
    }
	else {
//        xMOV(ecx, ptr32[&psxRegs.GPR.r[_Rs_]]);
        armLoad(ECX, PTR_CPU(psxRegs.GPR.r[_Rs_]));
    }
//    armAsm->Uxth(ECX, ECX);
}

static void rpsxCopyReg(int dest, int src)
{
	// try a simple rename first...
	const int roldsrc = _checkX86reg(X86TYPE_PSX, src, MODE_READ);
	if (roldsrc >= 0 && psxTryRenameReg(dest, src, roldsrc, 0, 0) >= 0)
		return;

	const int rdest = rpsxAllocRegIfUsed(dest, MODE_WRITE);
	if (PSX_IS_CONST1(src))
	{
		if (dest < 32)
		{
			g_psxConstRegs[dest] = g_psxConstRegs[src];
			PSX_SET_CONST(dest);
		}
		else
		{
			if (rdest >= 0) {
//                xMOV(xRegister32(rdest), g_psxConstRegs[src]);
                armAsm->Mov(HostW(rdest), g_psxConstRegs[src]);
            }
			else {
//                xMOV(ptr32[&psxRegs.GPR.r[dest]], g_psxConstRegs[src]);
                armStore(PTR_CPU(psxRegs.GPR.r[dest]), g_psxConstRegs[src]);
            }
		}

		return;
	}

	if (dest < 32)
		PSX_DEL_CONST(dest);

	const int rsrc = rpsxAllocRegIfUsed(src, MODE_READ);
	if (rsrc >= 0 && rdest >= 0)
	{
//		xMOV(xRegister32(rdest), xRegister32(rsrc));
        armAsm->Mov(HostW(rdest), HostW(rsrc));
	}
	else if (rdest >= 0)
	{
//		xMOV(xRegister32(rdest), ptr32[&psxRegs.GPR.r[src]]);
        armLoad(HostW(rdest), PTR_CPU(psxRegs.GPR.r[src]));
	}
	else if (rsrc >= 0)
	{
//		xMOV(ptr32[&psxRegs.GPR.r[dest]], xRegister32(rsrc));
        armStore(PTR_CPU(psxRegs.GPR.r[dest]), HostW(rsrc));
	}
	else
	{
//		xMOV(eax, ptr32[&psxRegs.GPR.r[src]]);
        armLoad(EAX, PTR_CPU(psxRegs.GPR.r[src]));
//		xMOV(ptr32[&psxRegs.GPR.r[dest]], eax);
        armStore(PTR_CPU(psxRegs.GPR.r[dest]), EAX);
	}
}

////
static void rpsxADDIU_const()
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] + _Imm_;
}

static void rpsxADDIU_(int info)
{
	// Rt = Rs + Im
	rpsxMoveStoT(info);
	if (_Imm_ != 0) {
//        xADD(xRegister32(EEREC_T), _Imm_);
        armAsm->Add(HostW(EEREC_T), HostW(EEREC_T), _Imm_);
    }
	// [iter121] allocator 枯渇で PROCESS_EE_T 未configの場合、HostW(EEREC_T)=HostW(0)=scratch
	// になり _psxFlushAllDirty() でフラッシュされない。明示的に psxRegs へ書き戻す。
	if (!(info & PROCESS_EE_T) && _Rt_ != 0) {
		armStore(PTR_CPU(psxRegs.GPR.r[_Rt_]), HostW(EEREC_T));
	}
}

PSXRECOMPILE_CONSTCODE1(ADDIU, XMMINFO_WRITET | XMMINFO_READS);

void rpsxADDI() { rpsxADDIU(); }

//// SLTI
static void rpsxSLTI_const()
{
	g_psxConstRegs[_Rt_] = *(int*)&g_psxConstRegs[_Rs_] < _Imm_;
}

static void rpsxSLTI_(int info)
{
	// [R98 FIX] Removed EOR before CSET — ARM64 CSET produces 0/1 directly.
	// EOR destroyed source register when allocator assigned same host register.
	// Same bug class as R5900 SLTIU fix (R68/P16).
	if (info & PROCESS_EE_S)
		armAsm->Cmp(HostW(EEREC_S), _Imm_);
	else
		armAsm->Cmp(armLoad(PTR_CPU(psxRegs.GPR.r[_Rs_])), _Imm_);

	armAsm->Cset(HostW(EEREC_T), a64::Condition::lt);
	// Force-store to psxRegs to survive allocator eviction.
	armStore(PTR_CPU(psxRegs.GPR.r[_Rt_]), HostW(EEREC_T));
}

PSXRECOMPILE_CONSTCODE1(SLTI, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_NORENAME);

//// SLTIU
static void rpsxSLTIU_const()
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] < (u32)_Imm_;
}

static void rpsxSLTIU_(int info)
{
	// [R98 FIX] Removed EOR before CSET + force-store. Same fix as rpsxSLTI_.
	if (info & PROCESS_EE_S)
		armAsm->Cmp(HostW(EEREC_S), _Imm_);
	else
		armAsm->Cmp(armLoad(PTR_CPU(psxRegs.GPR.r[_Rs_])), _Imm_);

	armAsm->Cset(HostW(EEREC_T), a64::Condition::cc);
	armStore(PTR_CPU(psxRegs.GPR.r[_Rt_]), HostW(EEREC_T));
}

PSXRECOMPILE_CONSTCODE1(SLTIU, XMMINFO_WRITET | XMMINFO_READS | XMMINFO_NORENAME);

static void rpsxLogicalOpI(int info, int op)
{
	if (_ImmU_ != 0)
	{
		rpsxMoveStoT(info);

        auto reg32 = HostW(EEREC_T);
		switch (op)
		{
			case 0:
//				xAND(xRegister32(EEREC_T), _ImmU_);
                armAsm->And(reg32, reg32, _ImmU_);
				break;
			case 1:
//				xOR(xRegister32(EEREC_T), _ImmU_);
                armAsm->Orr(reg32, reg32, _ImmU_);
				break;
			case 2:
//				xXOR(xRegister32(EEREC_T), _ImmU_);
                armAsm->Eor(reg32, reg32, _ImmU_);
				break;

				jNO_DEFAULT
		}
	}
	else
	{
		if (op == 0)
		{
//			xXOR(xRegister32(EEREC_T), xRegister32(EEREC_T));
            auto reg32 = HostW(EEREC_T);
            armAsm->Eor(reg32, reg32, reg32);
		}
		else if (EEREC_T != EEREC_S)
		{
			rpsxMoveStoT(info);
		}
	}
}

//// ANDI
static void rpsxANDI_const()
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] & _ImmU_;
}

static void rpsxANDI_(int info)
{
	rpsxLogicalOpI(info, 0);
}

PSXRECOMPILE_CONSTCODE1(ANDI, XMMINFO_WRITET | XMMINFO_READS);

//// ORI
static void rpsxORI_const()
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] | _ImmU_;
}

static void rpsxORI_(int info)
{
	rpsxLogicalOpI(info, 1);
}

PSXRECOMPILE_CONSTCODE1(ORI, XMMINFO_WRITET | XMMINFO_READS);

static void rpsxXORI_const()
{
	g_psxConstRegs[_Rt_] = g_psxConstRegs[_Rs_] ^ _ImmU_;
}

static void rpsxXORI_(int info)
{
	rpsxLogicalOpI(info, 2);
}

PSXRECOMPILE_CONSTCODE1(XORI, XMMINFO_WRITET | XMMINFO_READS);

void rpsxLUI()
{
	if (!_Rt_)
		return;
	_psxOnWriteReg(_Rt_);
	_psxDeleteReg(_Rt_, 0);
	PSX_SET_CONST(_Rt_);
	g_psxConstRegs[_Rt_] = psxRegs.code << 16;
}

static void rpsxADDU_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] + g_psxConstRegs[_Rt_];
}

static void rpsxADDU_consts(int info)
{
	const s32 cval = static_cast<s32>(g_psxConstRegs[_Rs_]);
	rpsxMoveTtoD(info);
	if (cval != 0) {
//        xADD(xRegister32(EEREC_D), cval);
        armAsm->Add(HostW(EEREC_D), HostW(EEREC_D), cval);
    }
}

static void rpsxADDU_constt(int info)
{
	const s32 cval = static_cast<s32>(g_psxConstRegs[_Rt_]);
	rpsxMoveStoD(info);
	if (cval != 0) {
//        xADD(xRegister32(EEREC_D), cval);
        armAsm->Add(HostW(EEREC_D), HostW(EEREC_D), cval);
    }
}

void rpsxADDU_(int info)
{
    auto reg32 = HostW(EEREC_D);
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		if (EEREC_D == EEREC_S)
		{
//			xADD(xRegister32(EEREC_D), xRegister32(EEREC_T));
            armAsm->Add(reg32, reg32, HostW(EEREC_T));
		}
		else if (EEREC_D == EEREC_T)
		{
//			xADD(xRegister32(EEREC_D), xRegister32(EEREC_S));
            armAsm->Add(reg32, reg32, HostW(EEREC_S));
		}
		else
		{
//			xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
            armAsm->Mov(reg32, HostW(EEREC_S));
//			xADD(xRegister32(EEREC_D), xRegister32(EEREC_T));
            armAsm->Add(reg32, reg32, HostW(EEREC_T));
		}
	}
	else if (info & PROCESS_EE_S)
	{
//		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
        armAsm->Mov(reg32, HostW(EEREC_S));
//		xADD(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rt_]]);
        armAsm->Add(reg32, reg32, armLoad(PTR_CPU(psxRegs.GPR.r[_Rt_])));
	}
	else if (info & PROCESS_EE_T)
	{
//		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_T));
        armAsm->Mov(reg32, HostW(EEREC_T));
//		xADD(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rs_]]);
        armAsm->Add(reg32, reg32, armLoad(PTR_CPU(psxRegs.GPR.r[_Rs_])));
	}
	else
	{
//		xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rs_]]);
        armLoad(reg32, PTR_CPU(psxRegs.GPR.r[_Rs_]));
//		xADD(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rt_]]);
        armAsm->Add(reg32, reg32, armLoad(PTR_CPU(psxRegs.GPR.r[_Rt_])));
	}
}

PSXRECOMPILE_CONSTCODE0(ADDU, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void rpsxADD() { rpsxADDU(); }

static void rpsxSUBU_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] - g_psxConstRegs[_Rt_];
}

static void rpsxSUBU_consts(int info)
{
	// more complex because Rt can be Rd, and we're reversing the op
	const s32 sval = g_psxConstRegs[_Rs_];
	const a64::WRegister dreg((_Rt_ == _Rd_) ? EAX : HostW(EEREC_D));
//	xMOV(dreg, sval);
    armAsm->Mov(dreg, sval);

	if (info & PROCESS_EE_T) {
//        xSUB(dreg, xRegister32(EEREC_T));
        armAsm->Sub(dreg, dreg, HostW(EEREC_T));
    }
	else {
//        xSUB(dreg, ptr32[&psxRegs.GPR.r[_Rt_]]);
        armAsm->Sub(dreg, dreg, armLoad(PTR_CPU(psxRegs.GPR.r[_Rt_])));
    }

//	xMOV(xRegister32(EEREC_D), dreg);
    armAsm->Mov(HostW(EEREC_D), dreg);
}

static void rpsxSUBU_constt(int info)
{
	const s32 tval = g_psxConstRegs[_Rt_];
	rpsxMoveStoD(info);
	if (tval != 0) {
//        xSUB(xRegister32(EEREC_D), tval);
        armAsm->Sub(HostW(EEREC_D), HostW(EEREC_D), tval);
    }
}

static void rpsxSUBU_(int info)
{
    auto reg32 = HostW(EEREC_D);

	// Rd = Rs - Rt
	if (_Rs_ == _Rt_)
	{
//		xXOR(xRegister32(EEREC_D), xRegister32(EEREC_D));
        armAsm->Eor(reg32, reg32, reg32);
		return;
	}

	// a bit messier here because it's not commutative..
	if ((info & PROCESS_EE_S) && (info & PROCESS_EE_T))
	{
		if (EEREC_D == EEREC_S)
		{
//			xSUB(xRegister32(EEREC_D), xRegister32(EEREC_T));
            armAsm->Sub(reg32, reg32, HostW(EEREC_T));
		}
		else if (EEREC_D == EEREC_T)
		{
			// D might equal T
			const a64::WRegister dreg((_Rt_ == _Rd_) ? EAX : HostW(EEREC_D));
//			xMOV(dreg, xRegister32(EEREC_S));
            armAsm->Mov(dreg, HostW(EEREC_S));
//			xSUB(dreg, xRegister32(EEREC_T));
            armAsm->Sub(dreg, dreg, HostW(EEREC_T));
//			xMOV(xRegister32(EEREC_D), dreg);
            armAsm->Mov(reg32, dreg);
		}
		else
		{
//			xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
            armAsm->Mov(reg32, HostW(EEREC_S));
//			xSUB(xRegister32(EEREC_D), xRegister32(EEREC_T));
            armAsm->Sub(reg32, reg32, HostW(EEREC_T));
		}
	}
	else if (info & PROCESS_EE_S)
	{
//		xMOV(xRegister32(EEREC_D), xRegister32(EEREC_S));
        armAsm->Mov(reg32, HostW(EEREC_S));
//		xSUB(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rt_]]);
        armAsm->Sub(reg32, reg32, armLoad(PTR_CPU(psxRegs.GPR.r[_Rt_])));
	}
	else if (info & PROCESS_EE_T)
	{
		// D might equal T
		const a64::WRegister dreg((_Rt_ == _Rd_) ? EAX : HostW(EEREC_D));
//		xMOV(dreg, ptr32[&psxRegs.GPR.r[_Rs_]]);
        armLoad(dreg, PTR_CPU(psxRegs.GPR.r[_Rs_]));
//		xSUB(dreg, xRegister32(EEREC_T));
        armAsm->Sub(dreg, dreg, HostW(EEREC_T));
//		xMOV(xRegister32(EEREC_D), dreg);
        armAsm->Mov(reg32, dreg);
	}
	else
	{
//		xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rs_]]);
        armLoad(reg32, PTR_CPU(psxRegs.GPR.r[_Rs_]));
//		xSUB(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[_Rt_]]);
        armAsm->Sub(reg32, reg32, armLoad(PTR_CPU(psxRegs.GPR.r[_Rt_])));
	}
}

PSXRECOMPILE_CONSTCODE0(SUBU, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void rpsxSUB() { rpsxSUBU(); }

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

static void rpsxLogicalOp_constv(LogicalOp op, int info, int creg, u32 vreg, int regv)
{
//	xImpl_G1Logic bad{};
//	const xImpl_G1Logic& xOP = op == LogicalOp::AND ? xAND : op == LogicalOp::OR ? xOR :
//														 op == LogicalOp::XOR    ? xXOR :
//														 op == LogicalOp::NOR    ? xOR :
//                                                                                   bad;
    const LogicalOp xOP = op == LogicalOp::AND ? LogicalOp::AND : op == LogicalOp::OR ? LogicalOp::OR :
                                                                  op == LogicalOp::XOR    ? LogicalOp::XOR :
                                                                  op == LogicalOp::NOR    ? LogicalOp::OR :
                                                                  LogicalOp::BAD;
	s32 fixedInput, fixedOutput, identityInput;
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

	const s32 cval = static_cast<s32>(g_psxConstRegs[creg]);

    auto reg32 = HostW(EEREC_D);
	if (hasFixed && cval == fixedInput)
	{
//		xMOV(xRegister32(EEREC_D), fixedOutput);
        armAsm->Mov(reg32, fixedOutput);
	}
	else
	{
		if (regv >= 0) {
//            xMOV(xRegister32(EEREC_D), xRegister32(regv));
            armAsm->Mov(reg32, HostW(regv));
        }
		else {
//            xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[vreg]]);
            armLoad(reg32, PTR_CPU(psxRegs.GPR.r[vreg]));
        }
		if (cval != identityInput) {
//            xOP(xRegister32(EEREC_D), cval);
            switch (xOP)
            {
                case LogicalOp::AND:
                    armAsm->And(reg32, reg32, cval);
                    break;
                case LogicalOp::NOR: case LogicalOp::OR:
                    armAsm->Orr(reg32, reg32, cval);
                    break;
                case LogicalOp::XOR:
                    armAsm->Eor(reg32, reg32, cval);
                    break;
                case LogicalOp::BAD:
                    break;
            }
        }
		if (op == LogicalOp::NOR) {
//            xNOT(xRegister32(EEREC_D));
            armAsm->Mvn(reg32, reg32);
        }
	}
}

static void rpsxLogicalOp(LogicalOp op, int info)
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
	int regs = (info & PROCESS_EE_S) ? EEREC_S : -1, regt = (info & PROCESS_EE_T) ? EEREC_T : -1;
	if (_Rd_ == _Rt_)
	{
		std::swap(rs, rt);
		std::swap(regs, regt);
	}

    auto reg32 = HostW(EEREC_D);
	if (op == LogicalOp::XOR && rs == rt)
	{
//		xXOR(xRegister32(EEREC_D), xRegister32(EEREC_D));
        armAsm->Eor(reg32, reg32, reg32);
	}
	else
	{
		if (regs >= 0) {
//            xMOV(xRegister32(EEREC_D), xRegister32(regs));
            armAsm->Mov(reg32, HostW(regs));
        }
		else {
//            xMOV(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[rs]]);
            armLoad(reg32, PTR_CPU(psxRegs.GPR.r[rs]));
        }

		if (regt >= 0) {
//            xOP(xRegister32(EEREC_D), xRegister32(regt));
            switch (xOP)
            {
                case LogicalOp::AND:
                    armAsm->And(reg32, reg32, HostW(regt));
                    break;
                case LogicalOp::NOR: case LogicalOp::OR:
                    armAsm->Orr(reg32, reg32, HostW(regt));
                    break;
                case LogicalOp::XOR:
                    armAsm->Eor(reg32, reg32, HostW(regt));
                    break;
                case LogicalOp::BAD:
                    break;
            }
        }
		else {
//            xOP(xRegister32(EEREC_D), ptr32[&psxRegs.GPR.r[rt]]);
            switch (xOP)
            {
                case LogicalOp::AND:
                    armAsm->And(reg32, reg32, armLoad(PTR_CPU(psxRegs.GPR.r[rt])));
                    break;
                case LogicalOp::NOR: case LogicalOp::OR:
                    armAsm->Orr(reg32, reg32, armLoad(PTR_CPU(psxRegs.GPR.r[rt])));
                    break;
                case LogicalOp::XOR:
                    armAsm->Eor(reg32, reg32, armLoad(PTR_CPU(psxRegs.GPR.r[rt])));
                    break;
                case LogicalOp::BAD:
                    break;
            }
        }

		if (op == LogicalOp::NOR) {
//            xNOT(xRegister32(EEREC_D));
            armAsm->Mvn(reg32, reg32);
        }
	}
}

static void rpsxAND_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] & g_psxConstRegs[_Rt_];
}

static void rpsxAND_consts(int info)
{
	rpsxLogicalOp_constv(LogicalOp::AND, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxAND_constt(int info)
{
	rpsxLogicalOp_constv(LogicalOp::AND, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxAND_(int info)
{
	rpsxLogicalOp(LogicalOp::AND, info);
}

PSXRECOMPILE_CONSTCODE0(AND, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

static void rpsxOR_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] | g_psxConstRegs[_Rt_];
}

static void rpsxOR_consts(int info)
{
	rpsxLogicalOp_constv(LogicalOp::OR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxOR_constt(int info)
{
	rpsxLogicalOp_constv(LogicalOp::OR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxOR_(int info)
{
	rpsxLogicalOp(LogicalOp::OR, info);
}

PSXRECOMPILE_CONSTCODE0(OR, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// XOR
static void rpsxXOR_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] ^ g_psxConstRegs[_Rt_];
}

static void rpsxXOR_consts(int info)
{
	rpsxLogicalOp_constv(LogicalOp::XOR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxXOR_constt(int info)
{
	rpsxLogicalOp_constv(LogicalOp::XOR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxXOR_(int info)
{
	rpsxLogicalOp(LogicalOp::XOR, info);
}

PSXRECOMPILE_CONSTCODE0(XOR, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// NOR
static void rpsxNOR_const()
{
	g_psxConstRegs[_Rd_] = ~(g_psxConstRegs[_Rs_] | g_psxConstRegs[_Rt_]);
}

static void rpsxNOR_consts(int info)
{
	rpsxLogicalOp_constv(LogicalOp::NOR, info, _Rs_, _Rt_, (info & PROCESS_EE_T) ? EEREC_T : -1);
}

static void rpsxNOR_constt(int info)
{
	rpsxLogicalOp_constv(LogicalOp::NOR, info, _Rt_, _Rs_, (info & PROCESS_EE_S) ? EEREC_S : -1);
}

static void rpsxNOR_(int info)
{
	rpsxLogicalOp(LogicalOp::NOR, info);
}

PSXRECOMPILE_CONSTCODE0(NOR, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

//// SLT
static void rpsxSLT_const()
{
	g_psxConstRegs[_Rd_] = *(int*)&g_psxConstRegs[_Rs_] < *(int*)&g_psxConstRegs[_Rt_];
}

static void rpsxSLTs_const(int info, int sign, int st)
{
	// [R98 FIX] Removed EOR before CSET + force-store. Same bug class as R5900 SLT (R96).
	const s32 cval = g_psxConstRegs[st ? _Rt_ : _Rs_];
	const a64::Condition& SET = st ? (sign ? a64::Condition::lt : a64::Condition::cc) : (sign ? a64::Condition::gt : a64::Condition::hi);

	const int regs = st ? ((info & PROCESS_EE_S) ? EEREC_S : -1) : ((info & PROCESS_EE_T) ? EEREC_T : -1);

	if (regs >= 0)
		armAsm->Cmp(HostW(regs), cval);
	else
		armAsm->Cmp(armLoad(PTR_CPU(psxRegs.GPR.r[st ? _Rs_ : _Rt_])), cval);

	armAsm->Cset(HostW(EEREC_D), SET);
	if (_Rd_ != 0)
		armStore(PTR_CPU(psxRegs.GPR.r[_Rd_]), HostW(EEREC_D));
}

static void rpsxSLTs_(int info, int sign)
{
	// [R98 FIX] Removed EOR before CSET + overlap handling via scratch + force-store.
	// Same bug class as R5900 SLT (R96).
	const a64::Condition& SET = (sign ? a64::Condition::lt : a64::Condition::cc);
	const bool overlap = (_Rd_ == _Rt_ || _Rd_ == _Rs_);

	const int regs = (info & PROCESS_EE_S) ? EEREC_S : _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);

	if (overlap)
	{
		// Use scratch register for CMP to avoid destroying source via EEREC_D alias.
		// R3000A uses 32-bit registers — must use W variants, not X/64-bit.
		armAsm->Mov(a64::w16, HostW(regs));
		if (info & PROCESS_EE_T)
			armAsm->Cmp(a64::w16, HostW(EEREC_T));
		else
			armAsm->Cmp(a64::w16, armLoad(PTR_CPU(psxRegs.GPR.r[_Rt_])));
		armAsm->Cset(HostW(EEREC_D), SET);
	}
	else
	{
		if (info & PROCESS_EE_T)
			armAsm->Cmp(HostW(regs), HostW(EEREC_T));
		else
			armAsm->Cmp(HostW(regs), armLoad(PTR_CPU(psxRegs.GPR.r[_Rt_])));
		armAsm->Cset(HostW(EEREC_D), SET);
	}

	if (_Rd_ != 0)
		armStore(PTR_CPU(psxRegs.GPR.r[_Rd_]), HostW(EEREC_D));
}

static void rpsxSLT_consts(int info)
{
	rpsxSLTs_const(info, 1, 0);
}

static void rpsxSLT_constt(int info)
{
	rpsxSLTs_const(info, 1, 1);
}

static void rpsxSLT_(int info)
{
	rpsxSLTs_(info, 1);
}

PSXRECOMPILE_CONSTCODE0(SLT, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_NORENAME);

//// SLTU
static void rpsxSLTU_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rs_] < g_psxConstRegs[_Rt_];
}

static void rpsxSLTU_consts(int info)
{
	rpsxSLTs_const(info, 0, 0);
}

static void rpsxSLTU_constt(int info)
{
	rpsxSLTs_const(info, 0, 1);
}

static void rpsxSLTU_(int info)
{
	rpsxSLTs_(info, 0);
}

PSXRECOMPILE_CONSTCODE0(SLTU, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT | XMMINFO_NORENAME);

//// MULT
static void rpsxMULT_const()
{
	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	u64 res = (s64)((s64) * (int*)&g_psxConstRegs[_Rs_] * (s64) * (int*)&g_psxConstRegs[_Rt_]);

//	xMOV(ptr32[&psxRegs.GPR.n.hi], (u32)((res >> 32) & 0xffffffff));
    armStore(PTR_CPU(psxRegs.GPR.n.hi), (u32)((res >> 32) & 0xffffffff));
//	xMOV(ptr32[&psxRegs.GPR.n.lo], (u32)(res & 0xffffffff));
    armStore(PTR_CPU(psxRegs.GPR.n.lo), (u32)(res & 0xffffffff));
}

static void rpsxWritebackHILO(int info)
{
	if (EEINST_LIVETEST(PSX_LO))
	{
		if (info & PROCESS_EE_LO) {
            armAsm->Mov(HostW(EEREC_LO), EAX);
            // [R98 FIX] Force-store LO to psxRegs to survive allocator eviction.
            // Same bug class as R5900 MULT Rd writeback (R98).
            armStore(PTR_CPU(psxRegs.GPR.n.lo), HostW(EEREC_LO));
        }
		else {
            armStore(PTR_CPU(psxRegs.GPR.n.lo), EAX);
        }
	}

	if (EEINST_LIVETEST(PSX_HI))
	{
		if (info & PROCESS_EE_HI) {
            armAsm->Mov(HostW(EEREC_HI), EDX);
            // [R98 FIX] Force-store HI to psxRegs.
            armStore(PTR_CPU(psxRegs.GPR.n.hi), HostW(EEREC_HI));
        }
		else {
            armStore(PTR_CPU(psxRegs.GPR.n.hi), EDX);
        }
	}
}

static void rpsxMULTsuperconst(int info, int sreg, int imm, int sign)
{
	// Lo/Hi = Rs * Rt (signed)
//	xMOV(eax, imm);
    armAsm->Mov(EAX, imm);

	const int regs = rpsxAllocRegIfUsed(sreg, MODE_READ);
	if (sign)
	{
		if (regs >= 0) {
//            xMUL(xRegister32(regs));
            armAsm->Smull(RAX, EAX, HostW(regs));
        }
		else {
//            xMUL(ptr32[&psxRegs.GPR.r[sreg]]);
            armAsm->Smull(RAX, EAX, armLoad(PTR_CPU(psxRegs.GPR.r[sreg])));
        }
	}
	else
	{
		if (regs >= 0) {
//            xUMUL(xRegister32(regs));
            armAsm->Umull(RAX, EAX, HostW(regs));
        }
		else {
//            xUMUL(ptr32[&psxRegs.GPR.r[sreg]]);
            armAsm->Umull(RAX, EAX, armLoad(PTR_CPU(psxRegs.GPR.r[sreg])));
        }
	}
    armAsm->Lsr(RDX, RAX, 32);

	rpsxWritebackHILO(info);
}

static void rpsxMULTsuper(int info, int sign)
{
	// Lo/Hi = Rs * Rt (signed)
	_psxMoveGPRtoR(EAX, _Rs_);

	const int regt = rpsxAllocRegIfUsed(_Rt_, MODE_READ);
	if (sign)
	{
		if (regt >= 0) {
//            xMUL(xRegister32(regt));
            armAsm->Smull(RAX, EAX, HostW(regt));
        }
		else {
//            xMUL(ptr32[&psxRegs.GPR.r[_Rt_]]);
            armAsm->Smull(RAX, EAX, armLoad(PTR_CPU(psxRegs.GPR.r[_Rt_])));
        }
	}
	else
	{
		if (regt >= 0) {
//            xUMUL(xRegister32(regt));
            armAsm->Umull(RAX, EAX, HostW(regt));
        }
		else {
//            xUMUL(ptr32[&psxRegs.GPR.r[_Rt_]]);
            armAsm->Umull(RAX, EAX, armLoad(PTR_CPU(psxRegs.GPR.r[_Rt_])));
        }
	}
    armAsm->Lsr(RDX, RAX, 32);

	rpsxWritebackHILO(info);
}

static void rpsxMULT_consts(int info)
{
	rpsxMULTsuperconst(info, _Rt_, g_psxConstRegs[_Rs_], 1);
}

static void rpsxMULT_constt(int info)
{
	rpsxMULTsuperconst(info, _Rs_, g_psxConstRegs[_Rt_], 1);
}

static void rpsxMULT_(int info)
{
	rpsxMULTsuper(info, 1);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(MULT, 1, psxInstCycles_Mult);

//// MULTU
static void rpsxMULTU_const()
{
	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	u64 res = (u64)((u64)g_psxConstRegs[_Rs_] * (u64)g_psxConstRegs[_Rt_]);

//	xMOV(ptr32[&psxRegs.GPR.n.hi], (u32)((res >> 32) & 0xffffffff));
    armStore(PTR_CPU(psxRegs.GPR.n.hi), (u32)((res >> 32) & 0xffffffff));
//	xMOV(ptr32[&psxRegs.GPR.n.lo], (u32)(res & 0xffffffff));
    armStore(PTR_CPU(psxRegs.GPR.n.lo), (u32)(res & 0xffffffff));
}

static void rpsxMULTU_consts(int info)
{
	rpsxMULTsuperconst(info, _Rt_, g_psxConstRegs[_Rs_], 0);
}

static void rpsxMULTU_constt(int info)
{
	rpsxMULTsuperconst(info, _Rs_, g_psxConstRegs[_Rt_], 0);
}

static void rpsxMULTU_(int info)
{
	rpsxMULTsuper(info, 0);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(MULTU, 1, psxInstCycles_Mult);

//// DIV
static void rpsxDIV_const()
{
	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	u32 lo, hi;

	/*
	 * Normally, when 0x80000000(-2147483648), the signed minimum value, is divided by 0xFFFFFFFF(-1), the
	 * 	operation will result in overflow. However, in this instruction an overflow exception does not occur and the
	 * 	result will be as follows:
	 * 	Quotient: 0x80000000 (-2147483648), and remainder: 0x00000000 (0)
	 */
	// Of course x86 cpu does overflow !
	if (g_psxConstRegs[_Rs_] == 0x80000000u && g_psxConstRegs[_Rt_] == 0xFFFFFFFFu)
	{
//		xMOV(ptr32[&psxRegs.GPR.n.hi], 0);
        armStore(PTR_CPU(psxRegs.GPR.n.hi), 0);
//		xMOV(ptr32[&psxRegs.GPR.n.lo], 0x80000000);
        armStore(PTR_CPU(psxRegs.GPR.n.lo), 0x80000000);
		return;
	}

	if (g_psxConstRegs[_Rt_] != 0)
	{
		lo = *(int*)&g_psxConstRegs[_Rs_] / *(int*)&g_psxConstRegs[_Rt_];
		hi = *(int*)&g_psxConstRegs[_Rs_] % *(int*)&g_psxConstRegs[_Rt_];
//		xMOV(ptr32[&psxRegs.GPR.n.hi], hi);
        armStore(PTR_CPU(psxRegs.GPR.n.hi), hi);
//		xMOV(ptr32[&psxRegs.GPR.n.lo], lo);
        armStore(PTR_CPU(psxRegs.GPR.n.lo), lo);
	}
	else
	{
//		xMOV(ptr32[&psxRegs.GPR.n.hi], g_psxConstRegs[_Rs_]);
        armStore(PTR_CPU(psxRegs.GPR.n.hi), g_psxConstRegs[_Rs_]);
		if (g_psxConstRegs[_Rs_] & 0x80000000u)
		{
//			xMOV(ptr32[&psxRegs.GPR.n.lo], 0x1);
            armStore(PTR_CPU(psxRegs.GPR.n.lo), 0x1);
		}
		else
		{
//			xMOV(ptr32[&psxRegs.GPR.n.lo], 0xFFFFFFFFu);
            armStore(PTR_CPU(psxRegs.GPR.n.lo), 0xFFFFFFFFu);
		}
	}
}

static void rpsxDIVsuper(int info, int sign, int process = 0)
{
	// Lo/Hi = Rs / Rt (signed)
	if (process & PROCESS_CONSTT) {
//        xMOV(ecx, g_psxConstRegs[_Rt_]);
        armAsm->Mov(ECX, g_psxConstRegs[_Rt_]);
    }
	else if (info & PROCESS_EE_T) {
//        xMOV(ecx, xRegister32(EEREC_T));
        armAsm->Mov(ECX, HostW(EEREC_T));
    }
	else {
//        xMOV(ecx, ptr32[&psxRegs.GPR.r[_Rt_]]);
        armLoad(ECX, PTR_CPU(psxRegs.GPR.r[_Rt_]));
    }

	if (process & PROCESS_CONSTS) {
//        xMOV(eax, g_psxConstRegs[_Rs_]);
        armAsm->Mov(EAX, g_psxConstRegs[_Rs_]);
    }
	else if (info & PROCESS_EE_S) {
//        xMOV(eax, xRegister32(EEREC_S));
        armAsm->Mov(EAX, HostW(EEREC_S));
    }
	else {
//        xMOV(eax, ptr32[&psxRegs.GPR.r[_Rs_]]);
        armLoad(EAX, PTR_CPU(psxRegs.GPR.r[_Rs_]));
    }

//	u8* end1;
    a64::Label end1;
	if (sign) //test for overflow (x86 will just throw an exception)
	{
//		xCMP(eax, 0x80000000);
        armAsm->Cmp(EAX, 0x80000000);
//		u8* cont1 = JNE8(0);
        a64::Label cont1;
        armAsm->B(&cont1, a64::Condition::ne);
//		xCMP(ecx, 0xffffffff);
        armAsm->Cmp(ECX, 0xffffffff);
//		u8* cont2 = JNE8(0);
        a64::Label cont2;
        armAsm->B(&cont2, a64::Condition::ne);
		//overflow case:
//		xXOR(edx, edx); //EAX remains 0x80000000
        armAsm->Eor(EDX, EDX, EDX);
//		end1 = JMP8(0);
        armAsm->B(&end1);

//		x86SetJ8(cont1);
        armBind(&cont1);
//		x86SetJ8(cont2);
        armBind(&cont2);
	}

//	xCMP(ecx, 0);
//	u8* cont3 = JNE8(0);
    a64::Label cont3;
    armCbnz(ECX, &cont3);

	//divide by zero
//	xMOV(edx, eax);
    armAsm->Mov(EDX, EAX);
	if (sign) //set EAX to (EAX < 0)?1:-1
	{
//		xSAR(eax, 31); //(EAX < 0)?-1:0
        armAsm->Asr(EAX, EAX, 31);
//		xSHL(eax, 1); //(EAX < 0)?-2:0
        armAsm->Lsl(EAX, EAX, 1);
//		xNOT(eax); //(EAX < 0)?1:-1
        armAsm->Mvn(EAX, EAX);
	}
	else {
//        xMOV(eax, 0xffffffff);
        armAsm->Mov(EAX, 0xffffffff);
    }
//	u8* end2 = JMP8(0);
    a64::Label end2;
    armAsm->B(&end2);

	// Normal division
//	x86SetJ8(cont3);
    armBind(&cont3);

    // temp
    armAsm->Mov(EEX, EAX);

	if (sign)
	{
//		xCDQ();
//		xDIV(ecx);
        armAsm->Sdiv(EAX, EEX, ECX);
	}
	else
	{
//		xXOR(edx, edx);
//		xUDIV(ecx);
        armAsm->Udiv(EAX, EEX, ECX);
	}
    armAsm->Msub(EDX, EAX, ECX, EEX);

	if (sign) {
//        x86SetJ8(end1);
        armBind(&end1);
    }
//	x86SetJ8(end2);
    armBind(&end2);

	rpsxWritebackHILO(info);
}

static void rpsxDIV_consts(int info)
{
	rpsxDIVsuper(info, 1, PROCESS_CONSTS);
}

static void rpsxDIV_constt(int info)
{
	rpsxDIVsuper(info, 1, PROCESS_CONSTT);
}

static void rpsxDIV_(int info)
{
	rpsxDIVsuper(info, 1);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(DIV, 1, psxInstCycles_Div);

//// DIVU
void rpsxDIVU_const()
{
	u32 lo, hi;

	_deletePSXtoX86reg(PSX_HI, DELETE_REG_FREE_NO_WRITEBACK);
	_deletePSXtoX86reg(PSX_LO, DELETE_REG_FREE_NO_WRITEBACK);

	if (g_psxConstRegs[_Rt_] != 0)
	{
		lo = g_psxConstRegs[_Rs_] / g_psxConstRegs[_Rt_];
		hi = g_psxConstRegs[_Rs_] % g_psxConstRegs[_Rt_];
//		xMOV(ptr32[&psxRegs.GPR.n.hi], hi);
        armStore(PTR_CPU(psxRegs.GPR.n.hi), hi);
//		xMOV(ptr32[&psxRegs.GPR.n.lo], lo);
        armStore(PTR_CPU(psxRegs.GPR.n.lo), lo);
	}
	else
	{
//		xMOV(ptr32[&psxRegs.GPR.n.hi], g_psxConstRegs[_Rs_]);
        armStore(PTR_CPU(psxRegs.GPR.n.hi), g_psxConstRegs[_Rs_]);
//		xMOV(ptr32[&psxRegs.GPR.n.lo], 0xFFFFFFFFu);
        armStore(PTR_CPU(psxRegs.GPR.n.lo), 0xFFFFFFFFu);
	}
}

void rpsxDIVU_consts(int info)
{
	rpsxDIVsuper(info, 0, PROCESS_CONSTS);
}

void rpsxDIVU_constt(int info)
{
	rpsxDIVsuper(info, 0, PROCESS_CONSTT);
}

void rpsxDIVU_(int info)
{
	rpsxDIVsuper(info, 0);
}

PSXRECOMPILE_CONSTCODE3_PENALTY(DIVU, 1, psxInstCycles_Div);

// TLB loadstore functions

static u8* rpsxGetConstantAddressOperand(bool store)
{
#if 0
	if (!PSX_IS_CONST1(_Rs_))
		return nullptr;

	const u32 addr = g_psxConstRegs[_Rs_];
	return store ? iopVirtMemW<u8>(addr) : const_cast<u8*>(iopVirtMemR<u8>(addr));
#else
	return nullptr;
#endif
}

static void rpsxCalcAddressOperand()
{
	// if it's a const register, just flush it, since we'll need to do that
	// when we call the load/store function anyway
	int rs;
	if (PSX_IS_CONST1(_Rs_))
		rs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	else
		rs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);

//	_freeX86reg(arg1regd);
    _freeX86reg(EAX);

	if (rs >= 0) {
//        xMOV(arg1regd, xRegister32(rs));
        armAsm->Mov(EAX, HostW(rs));
    }
	else {
//        xMOV(arg1regd, ptr32[&psxRegs.GPR.r[_Rs_]]);
        armLoad(EAX, PTR_CPU(psxRegs.GPR.r[_Rs_]));
    }

	if (_Imm_) {
//        xADD(arg1regd, _Imm_);
        armAsm->Add(EAX, EAX, _Imm_);
    }
}

static void rpsxCalcStoreOperand()
{
	int rt;
	if (PSX_IS_CONST1(_Rt_))
		rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
	else
		rt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);

//	_freeX86reg(arg2regd);
    _freeX86reg(ECX);

	if (rt >= 0) {
//        xMOV(arg2regd, xRegister32(rt));
        armAsm->Mov(ECX, HostW(rt));
    }
	else {
//        xMOV(arg2regd, ptr32[&psxRegs.GPR.r[_Rt_]]);
        armLoad(ECX, PTR_CPU(psxRegs.GPR.r[_Rt_]));
    }
}

static void rpsxLoad(int size, bool sign)
{
	rpsxCalcAddressOperand();

	if (_Rt_ != 0)
	{
		PSX_DEL_CONST(_Rt_);
		_deletePSXtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
	}

	_psxFlushCall(FLUSH_FULLVTLB);
//	xTEST(arg1regd, 0x10000000);
//	xForwardJZ8 is_ram_read;

    a64::Label is_ram_read;
    armAsm->Tbz(EAX, 28, &is_ram_read); // 28 = find_bit_pos(0x10000000)

	switch (size)
	{
		case 8:
//			xFastCall((void*)iopMemRead8);
            armEmitCall(reinterpret_cast<const void*>(iopMemRead8));
			break;
		case 16:
//			xFastCall((void*)iopMemRead16);
            armEmitCall(reinterpret_cast<const void*>(iopMemRead16));
			break;
		case 32:
//			xFastCall((void*)iopMemRead32);
            armEmitCall(reinterpret_cast<const void*>(iopMemRead32));
			break;

			jNO_DEFAULT
	}

	if (_Rt_ == 0)
	{
		// dummy read
//		is_ram_read.SetTarget();
        armBind(&is_ram_read);
		return;
	}

//	xForwardJump8 done;
    a64::Label done;
    armAsm->B(&done);

//	is_ram_read.SetTarget();
    armBind(&is_ram_read);

	// read from psM directly
//	xAND(arg1regd, 0x1fffff);
    armAsm->And(EAX, EAX,  0x1fffff);

//	auto addr = xComplexAddress(rax, iopMem->Main, arg1reg);
    const auto addr = a64::MemOperand(RSTATE_x26, RAX);
	switch (size)
	{
		case 8:
//			xMOVZX(eax, ptr8[addr]);
            armAsm->Ldrb(EAX, addr);
			break;
		case 16:
//			xMOVZX(eax, ptr16[addr]);
            armAsm->Ldrh(EAX, addr);
			break;
		case 32:
//			xMOV(eax, ptr32[addr]);
            armAsm->Ldr(EAX, addr);
			break;

			jNO_DEFAULT
	}

//	done.SetTarget();
    armBind(&done);

	const int rt = rpsxAllocRegIfUsed(_Rt_, MODE_WRITE);
	const a64::WRegister dreg((rt < 0) ? EAX : HostW(rt));

	// sign/zero extend as needed
	switch (size)
	{
		case 8:
//			sign ? xMOVSX(dreg, al) : xMOVZX(dreg, al);
            sign ? armAsm->Sxtb(dreg, EAX) : armAsm->Uxtb(dreg, EAX);
			break;
		case 16:
//			sign ? xMOVSX(dreg, ax) : xMOVZX(dreg, ax);
            sign ? armAsm->Sxth(dreg, EAX) : armAsm->Uxth(dreg, EAX);
			break;
		case 32:
//			xMOV(dreg, eax);
            armAsm->Move(dreg, EAX);
			break;
			jNO_DEFAULT
	}

	// if not caching, write back
	if (rt < 0) {
//        xMOV(ptr32[&psxRegs.GPR.r[_Rt_]], eax);
        armStore(PTR_CPU(psxRegs.GPR.r[_Rt_]), EAX);
    }
}


REC_FUNC(LWL);
REC_FUNC(LWR);
REC_FUNC(SWL);
REC_FUNC(SWR);

static void rpsxLB()
{
	rpsxLoad(8, true);
}

static void rpsxLBU()
{
	rpsxLoad(8, false);
}

static void rpsxLH()
{
	rpsxLoad(16, true);
}

static void rpsxLHU()
{
	rpsxLoad(16, false);
}

static void rpsxLW()
{
	// [iter133] IOP LW at 0xBFC4AB38: LW v0,0(v0) reads from dispatch table.
	// psxpc = instruction_addr+4 in IOP JIT (confirmed by iter127: JR at bfc4ab40 → psxpc==bfc4ab44).
	// So this LW (at bfc4ab38) is compiled when psxpc==0xBFC4AB3C (not 0xBFC4AB38 as in iter128).
	// Fastmem path (Tbz bit28) returns garbage if $v0+$t8 has bit28=0 on 2nd execution.
	// Force vtlb (iopMemRead32) unconditionally so all executions go through iopMemRead32.
	if (psxpc == 0xBFC4AB3Cu)
	{
		// [iter135] confirm compile-time path fires
		Console.WriteLn("@@IOP_LW_FORCECOMPILE@@ psxpc=0x%08x _Rt_=%u _Rs_=%u PSX_IS_CONST1_Rs=%d",
			psxpc, _Rt_, _Rs_, PSX_IS_CONST1(_Rs_) ? 1 : 0);
		rpsxCalcAddressOperand(); // EAX = runtime address
		if (_Rt_ != 0)
		{
			PSX_DEL_CONST(_Rt_);
			_deletePSXtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
		}
		_psxFlushCall(FLUSH_FULLVTLB);
		armEmitCall(reinterpret_cast<const void*>(iopMemRead32)); // result in EAX/w0
		if (_Rt_ != 0)
		{
			const int rt = rpsxAllocRegIfUsed(_Rt_, MODE_WRITE);
			if (rt >= 0)
				armAsm->Move(HostW(rt), EAX);
			else
				armStore(PTR_CPU(psxRegs.GPR.r[_Rt_]), EAX);
		}
		return;
	}
	rpsxLoad(32, false);
}

static void rpsxSB()
{
	rpsxCalcAddressOperand();
	rpsxCalcStoreOperand();
	_psxFlushCall(FLUSH_FULLVTLB);
//	xFastCall((void*)iopMemWrite8);
    armEmitCall(reinterpret_cast<void*>(iopMemWrite8));
}

static void rpsxSH()
{
	rpsxCalcAddressOperand();
	rpsxCalcStoreOperand();
	_psxFlushCall(FLUSH_FULLVTLB);
//	xFastCall((void*)iopMemWrite16);
    armEmitCall(reinterpret_cast<void*>(iopMemWrite16));
}

static void rpsxSW()
{
	u8* ptr = rpsxGetConstantAddressOperand(true);
	if (ptr)
	{
		const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
//		xMOV(ptr32[ptr], xRegister32(rt));
        armAsm->Str(HostW(rt), armMemOperandPtr(ptr));
		return;
	}

	rpsxCalcAddressOperand();
	rpsxCalcStoreOperand();
	_psxFlushCall(FLUSH_FULLVTLB);
//	xFastCall((void*)iopMemWrite32);
    armEmitCall(reinterpret_cast<void*>(iopMemWrite32));
}

//// SLL
static void rpsxSLL_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] << _Sa_;
}

static void rpsxSLLs_(int info, int sa)
{
	rpsxMoveTtoD(info);
	if (sa != 0) {
//        xSHL(xRegister32(EEREC_D), sa);
        armAsm->Lsl(HostW(EEREC_D), HostW(EEREC_D), sa);
    }
}

static void rpsxSLL_(int info)
{
	rpsxSLLs_(info, _Sa_);
}

PSXRECOMPILE_CONSTCODE2(SLL, XMMINFO_WRITED | XMMINFO_READS);

//// SRL
static void rpsxSRL_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] >> _Sa_;
}

static void rpsxSRLs_(int info, int sa)
{
	rpsxMoveTtoD(info);
	if (sa != 0) {
//        xSHR(xRegister32(EEREC_D), sa);
        armAsm->Lsr(HostW(EEREC_D), HostW(EEREC_D), sa);
    }
}

static void rpsxSRL_(int info)
{
	rpsxSRLs_(info, _Sa_);
}

PSXRECOMPILE_CONSTCODE2(SRL, XMMINFO_WRITED | XMMINFO_READS);

//// SRA
static void rpsxSRA_const()
{
	g_psxConstRegs[_Rd_] = *(int*)&g_psxConstRegs[_Rt_] >> _Sa_;
}

static void rpsxSRAs_(int info, int sa)
{
	rpsxMoveTtoD(info);
	if (sa != 0) {
//        xSAR(xRegister32(EEREC_D), sa);
        armAsm->Asr(HostW(EEREC_D), HostW(EEREC_D), sa);
    }
}

static void rpsxSRA_(int info)
{
	rpsxSRAs_(info, _Sa_);
}

PSXRECOMPILE_CONSTCODE2(SRA, XMMINFO_WRITED | XMMINFO_READS);

namespace
{
    enum class SHIFTV
    {
        xSHL,
        xSHR,
        xSAR
    };
} // namespace

//// SLLV
static void rpsxShiftV_constt(int info, const SHIFTV shift)
{
	pxAssert(_Rs_ != 0);
	rpsxMoveSToECX(info);

    auto reg32 = HostW(EEREC_D);

//	xMOV(xRegister32(EEREC_D), g_psxConstRegs[_Rt_]);
    armAsm->Mov(reg32, g_psxConstRegs[_Rt_]);

//	shift(xRegister32(EEREC_D), cl);
    switch (shift) {
        case SHIFTV::xSHL:
            armAsm->Lsl(reg32, reg32, ECX);
            break;
        case SHIFTV::xSHR:
            armAsm->Lsr(reg32, reg32, ECX);
            break;
        case SHIFTV::xSAR:
            armAsm->Asr(reg32, reg32, ECX);
            break;
    }
}

static void rpsxShiftV(int info, const SHIFTV shift)
{
	pxAssert(_Rs_ != 0);

	rpsxMoveSToECX(info);
	rpsxMoveTtoD(info);

    auto reg32 = HostW(EEREC_D);
//	shift(xRegister32(EEREC_D), cl);
    switch (shift) {
        case SHIFTV::xSHL:
            armAsm->Lsl(reg32, reg32, ECX);
            break;
        case SHIFTV::xSHR:
            armAsm->Lsr(reg32, reg32, ECX);
            break;
        case SHIFTV::xSAR:
            armAsm->Asr(reg32, reg32, ECX);
            break;
    }
}

static void rpsxSLLV_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] << (g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSLLV_consts(int info)
{
	rpsxSLLs_(info, g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSLLV_constt(int info)
{
	rpsxShiftV_constt(info, SHIFTV::xSHL);
}

static void rpsxSLLV_(int info)
{
	rpsxShiftV(info, SHIFTV::xSHL);
}

PSXRECOMPILE_CONSTCODE0(SLLV, XMMINFO_WRITED | XMMINFO_READS);

//// SRLV
static void rpsxSRLV_const()
{
	g_psxConstRegs[_Rd_] = g_psxConstRegs[_Rt_] >> (g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRLV_consts(int info)
{
	rpsxSRLs_(info, g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRLV_constt(int info)
{
	rpsxShiftV_constt(info, SHIFTV::xSHR);
}

static void rpsxSRLV_(int info)
{
	rpsxShiftV(info, SHIFTV::xSHR);
}

PSXRECOMPILE_CONSTCODE0(SRLV, XMMINFO_WRITED | XMMINFO_READS);

//// SRAV
static void rpsxSRAV_const()
{
	g_psxConstRegs[_Rd_] = *(int*)&g_psxConstRegs[_Rt_] >> (g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRAV_consts(int info)
{
	rpsxSRAs_(info, g_psxConstRegs[_Rs_] & 0x1f);
}

static void rpsxSRAV_constt(int info)
{
	rpsxShiftV_constt(info, SHIFTV::xSAR);
}

static void rpsxSRAV_(int info)
{
	rpsxShiftV(info, SHIFTV::xSAR);
}

PSXRECOMPILE_CONSTCODE0(SRAV, XMMINFO_WRITED | XMMINFO_READS);

extern void rpsxSYSCALL();
extern void rpsxBREAK();

static void rpsxMFHI()
{
	if (!_Rd_)
		return;

	rpsxCopyReg(_Rd_, PSX_HI);
}

static void rpsxMTHI()
{
	rpsxCopyReg(PSX_HI, _Rs_);
}

static void rpsxMFLO()
{
	if (!_Rd_)
		return;

	rpsxCopyReg(_Rd_, PSX_LO);
}

static void rpsxMTLO()
{
	rpsxCopyReg(PSX_LO, _Rs_);
}

static void rpsxJ()
{
	// j target
	u32 newpc = _InstrucTarget_ * 4 + (psxpc & 0xf0000000);
    Console.WriteLn("@@J_COMPILE@@ psxpc=%x newpc=%x", psxpc, newpc);
	psxRecompileNextInstruction(true, false);
	psxSetBranchImm(newpc);
}

static void rpsxJAL()
{
	u32 newpc = (_InstrucTarget_ << 2) + (psxpc & 0xf0000000);
    Console.WriteLn("@@JAL_COMPILE@@ psxpc=%x newpc=%x", psxpc, newpc);
	_psxDeleteReg(31, DELETE_REG_FREE_NO_WRITEBACK);
	PSX_SET_CONST(31);
	g_psxConstRegs[31] = psxpc + 4;

	psxRecompileNextInstruction(true, false);
	psxSetBranchImm(newpc);
}

static void rpsxJR()
{
    Console.WriteLn("@@JR_COMPILE@@ psxpc=%x rs=%d is_const=%d val=%x", psxpc, _Rs_, PSX_IS_CONST1(_Rs_), g_psxConstRegs[_Rs_]);
    // [iter127] probe: JR $v0 at bfc4ab44 loads function ptr from table at bfc4b138.
    // Hypothesis A: IOP ROM modified at runtime. Hypothesis B: IOP memory mapping error.
    // Print IOP virtual memory value at JIT compile time to distinguish.
    if (psxpc == 0xbfc4ab44u) {
        const u32* ptr = iopVirtMemR<u32>(0xbfc4b138u);
        Console.WriteLn("@@IOP_ROM_bfc4b138@@ ptr=%s val=0x%08x",
            ptr ? "OK" : "NULL", ptr ? *ptr : 0xDEADBEEFu);
    }
	psxSetBranchReg(_Rs_);
}

static void rpsxJALR()
{
	const u32 newpc = psxpc + 4;
    Console.WriteLn("@@JALR_COMPILE@@ psxpc=%x rs=%d rd=%d", psxpc, _Rs_, _Rd_);
	const bool swap = (_Rd_ == _Rs_) ? false : psxTrySwapDelaySlot(_Rs_, 0, _Rd_);

	// jalr Rs
	int wbreg = -1;
	if (!swap)
	{
		wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
		_psxMoveGPRtoR(HostW(wbreg), _Rs_);
	}

	if (_Rd_)
	{
		_psxDeleteReg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);
		PSX_SET_CONST(_Rd_);
		g_psxConstRegs[_Rd_] = newpc;
	}

	if (!swap)
	{
		psxRecompileNextInstruction(true, false);

		if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
		{
//			xMOV(ptr32[&psxRegs.pc], xRegister32(wbreg));
            armStore(PTR_CPU(psxRegs.pc), HostW(wbreg));
			x86regs[wbreg].inuse = 0;
		}
		else
		{
//			xMOV(eax, ptr32[&psxRegs.pcWriteback]);
            armLoad(EAX, PTR_CPU(psxRegs.pcWriteback));
//			xMOV(ptr32[&psxRegs.pc], eax);
            armStore(PTR_CPU(psxRegs.pc), EAX);
		}
	}
	else
	{
		if (PSX_IS_DIRTY_CONST(_Rs_) || _hasX86reg(X86TYPE_PSX, _Rs_, 0))
		{
			const int x86reg = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
//			xMOV(ptr32[&psxRegs.pc], xRegister32(x86reg));
            armStore(PTR_CPU(psxRegs.pc), HostW(x86reg));
		}
		else
		{
			_psxMoveGPRtoM(PTR_CPU(psxRegs.pc), _Rs_);
		}
	}

	psxSetBranchReg(0xffffffff);
}

//// BEQ
//static u32* s_pbranchjmp;

static void rpsxSetBranchEQ(int process, a64::Label* p_pbranchjmp)
{
	if (process & PROCESS_CONSTS)
	{
		const int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
		if (regt >= 0) {
//            xCMP(xRegister32(regt), g_psxConstRegs[_Rs_]);
            armAsm->Cmp(HostW(regt), g_psxConstRegs[_Rs_]);
        }
		else {
//            xCMP(ptr32[&psxRegs.GPR.r[_Rt_]], g_psxConstRegs[_Rs_]);
            armAsm->Cmp(armLoad(PTR_CPU(psxRegs.GPR.r[_Rt_])), g_psxConstRegs[_Rs_]);
        }
	}
	else if (process & PROCESS_CONSTT)
	{
		const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
		if (regs >= 0) {
//            xCMP(xRegister32(regs), g_psxConstRegs[_Rt_]);
            armAsm->Cmp(HostW(regs), g_psxConstRegs[_Rt_]);
        }
		else {
//            xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], g_psxConstRegs[_Rt_]);
            armAsm->Cmp(armLoad(PTR_CPU(psxRegs.GPR.r[_Rs_])), g_psxConstRegs[_Rt_]);
        }
	}
	else
	{
		// force S into register, since we need to load it, may as well cache.
		const int regs = _allocX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
		const int regt = _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ);

		if (regt >= 0) {
//            xCMP(xRegister32(regs), xRegister32(regt));
            armAsm->Cmp(HostW(regs), HostW(regt));
        }
		else {
//            xCMP(xRegister32(regs), ptr32[&psxRegs.GPR.r[_Rt_]]);
            armAsm->Cmp(HostW(regs), armLoad(PTR_CPU(psxRegs.GPR.r[_Rt_])));
        }
	}

//	s_pbranchjmp = JNE32(0);
    armAsm->B(p_pbranchjmp, a64::Condition::ne);
}

static void rpsxBEQ_const()
{
	u32 branchTo;

	if (g_psxConstRegs[_Rs_] == g_psxConstRegs[_Rt_])
		branchTo = ((s32)_Imm_ * 4) + psxpc;
	else
		branchTo = psxpc + 4;

	psxRecompileNextInstruction(true, false);
	psxSetBranchImm(branchTo);
}

static void rpsxBEQ_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + psxpc;

	if (_Rs_ == _Rt_)
	{
		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
	}
	else
	{
		const bool swap = psxTrySwapDelaySlot(_Rs_, _Rt_, 0);
		// [iter117] @@BEQ_CONST_STATE@@ — compile-time log: const state of t0 at bfc02648 BEQ flush point
		if (psxpc == 0xBFC02650u)
			Console.WriteLn("@@BEQ_CONST_STATE@@ hasConst=%08x flushed=%08x t0const=%u t0flushed=%u t0val=%08x swap=%u",
				g_psxHasConstReg, g_psxFlushedConstReg,
				(g_psxHasConstReg >> 8) & 1u, (g_psxFlushedConstReg >> 8) & 1u,
				g_psxConstRegs[8], swap ? 1u : 0u);
		_psxFlushAllDirty();
		armAsm->Mov(EAX, psxpc);
		armAsm->Mov(ECX, 0u);
		armAsm->Mov(EDX, _Rs_);
		armAsm->Mov(a64::WRegister(3), _Rt_);
		armEmitCall(reinterpret_cast<void*>(antigravityLogIopBranchCmp));

        a64::Label s_pbranchjmp;
		rpsxSetBranchEQ(process, &s_pbranchjmp);

		if (!swap)
		{
			psxSaveBranchState();
			psxRecompileNextInstruction(true, false);
		}

		psxSetBranchImm(branchTo);

//		x86SetJ32A(s_pbranchjmp);
        armBind(&s_pbranchjmp);

		if (!swap)
		{
			// recopy the next inst
			psxpc -= 4;
			psxLoadBranchState();
			psxRecompileNextInstruction(true, false);
		}

		psxSetBranchImm(psxpc);
	}
}

static void rpsxBEQ()
{
	// The BIOS poll loop around 0x9FC41094/0x9FC4109C is currently the primary JIT
	// stall site. Force the runtime branch path here to avoid const-folded branch code
	// until the underlying JIT compare/const-prop issue is fully identified.
	if (psxpc == 0x9fc41098u)
	{
		rpsxBEQ_process(0);
		return;
	}

	// prefer using the host register over an immediate, it'll be smaller code.
	if (PSX_IS_CONST2(_Rs_, _Rt_))
		rpsxBEQ_const();
	else if (PSX_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ) < 0)
		rpsxBEQ_process(PROCESS_CONSTS);
	else if (PSX_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ) < 0)
		rpsxBEQ_process(PROCESS_CONSTT);
	else
		rpsxBEQ_process(0);
}

//// BNE
static void rpsxBNE_const()
{
	u32 branchTo;

	if (g_psxConstRegs[_Rs_] != g_psxConstRegs[_Rt_])
		branchTo = ((s32)_Imm_ * 4) + psxpc;
	else
		branchTo = psxpc + 4;

	psxRecompileNextInstruction(true, false);
	psxSetBranchImm(branchTo);
}

static void rpsxBNE_process(int process)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + psxpc;

	if (_Rs_ == _Rt_)
	{
		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(psxpc);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, _Rt_, 0);
	_psxFlushAllDirty();
	armAsm->Mov(EAX, psxpc);
	armAsm->Mov(ECX, 1u);
	armAsm->Mov(EDX, _Rs_);
	armAsm->Mov(a64::WRegister(3), _Rt_);
	armEmitCall(reinterpret_cast<void*>(antigravityLogIopBranchCmp));

    a64::Label s_pbranchjmp;
	rpsxSetBranchEQ(process, &s_pbranchjmp);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

//	x86SetJ32A(s_pbranchjmp);
    armBind(&s_pbranchjmp);

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

static void rpsxBNE()
{
	// Same BIOS poll loop handling as rpsxBEQ() above. psxpc points to the delay-slot+4
	// location when the branch op is compiled, so 0x9FC410A0 corresponds to the BNE at
	// 0x9FC4109C.
	if (psxpc == 0x9fc410a0u)
	{
		rpsxBNE_process(0);
		return;
	}

	if (PSX_IS_CONST2(_Rs_, _Rt_))
		rpsxBNE_const();
	else if (PSX_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ) < 0)
		rpsxBNE_process(PROCESS_CONSTS);
	else if (PSX_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_PSX, _Rt_, MODE_READ) < 0)
		rpsxBNE_process(PROCESS_CONSTT);
	else
		rpsxBNE_process(0);
}

//// BLTZ
static void rpsxBLTZ()
{
	// Branch if Rs < 0
	u32 branchTo = (s32)_Imm_ * 4 + psxpc;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] >= 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0) {
//        xCMP(xRegister32(regs), 0);
        armAsm->Cmp(HostW(regs), 0);
    }
	else {
//        xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);
        armAsm->Cmp(armLoad(PTR_CPU(psxRegs.GPR.r[_Rs_])), 0);
    }

//	u32* pjmp = JL32(0);
    a64::Label pjmp;
    armAsm->B(&pjmp, a64::Condition::lt);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

//	x86SetJ32A(pjmp);
    armBind(&pjmp);

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

//// BGEZ
static void rpsxBGEZ()
{
	u32 branchTo = ((s32)_Imm_ * 4) + psxpc;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] < 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0) {
//        xCMP(xRegister32(regs), 0);
        armAsm->Cmp(HostW(regs), 0);
    }
	else {
//        xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);
        armAsm->Cmp(armLoad(PTR_CPU(psxRegs.GPR.r[_Rs_])), 0);
    }

//	u32* pjmp = JGE32(0);
    a64::Label pjmp;
    armAsm->B(&pjmp, a64::Condition::ge);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

//	x86SetJ32A(pjmp);
    armBind(&pjmp);

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

//// BLTZAL
static void rpsxBLTZAL()
{
	// Branch if Rs < 0
	u32 branchTo = (s32)_Imm_ * 4 + psxpc;

	_psxDeleteReg(31, DELETE_REG_FREE_NO_WRITEBACK);

	PSX_SET_CONST(31);
	g_psxConstRegs[31] = psxpc + 4;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] >= 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0) {
//        xCMP(xRegister32(regs), 0);
        armAsm->Cmp(HostW(regs), 0);
    }
	else {
//        xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);
        armAsm->Cmp(armLoad(PTR_CPU(psxRegs.GPR.r[_Rs_])), 0);
    }

//	u32* pjmp = JL32(0);
    a64::Label pjmp;
    armAsm->B(&pjmp, a64::Condition::lt);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

//	x86SetJ32A(pjmp);
    armBind(&pjmp);

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

//// BGEZAL
static void rpsxBGEZAL()
{
	u32 branchTo = ((s32)_Imm_ * 4) + psxpc;

	_psxDeleteReg(31, DELETE_REG_FREE_NO_WRITEBACK);

	PSX_SET_CONST(31);
	g_psxConstRegs[31] = psxpc + 4;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] < 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0) {
//        xCMP(xRegister32(regs), 0);
        armAsm->Cmp(HostW(regs), 0);
    }
	else {
//        xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);
        armAsm->Cmp(armLoad(PTR_CPU(psxRegs.GPR.r[_Rs_])), 0);
    }

//	u32* pjmp = JGE32(0);
    a64::Label pjmp;
    armAsm->B(&pjmp, a64::Condition::ge);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

//	x86SetJ32A(pjmp);
    armBind(&pjmp);

	if (!swap)
	{
		// recopy the next inst
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

//// BLEZ
static void rpsxBLEZ()
{
	// Branch if Rs <= 0
	u32 branchTo = (s32)_Imm_ * 4 + psxpc;

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] > 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0) {
//        xCMP(xRegister32(regs), 0);
        armAsm->Cmp(HostW(regs), 0);
    }
	else {
//        xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);
        armAsm->Cmp(armLoad(PTR_CPU(psxRegs.GPR.r[_Rs_])), 0);
    }

//	u32* pjmp = JLE32(0);
    a64::Label pjmp;
    armAsm->B(&pjmp, a64::Condition::le);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

//	x86SetJ32A(pjmp);
    armBind(&pjmp);

	if (!swap)
	{
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

//// BGTZ
static void rpsxBGTZ()
{
	// Branch if Rs > 0
	u32 branchTo = (s32)_Imm_ * 4 + psxpc;

	_psxFlushAllDirty();

	if (PSX_IS_CONST1(_Rs_))
	{
		if ((int)g_psxConstRegs[_Rs_] <= 0)
			branchTo = psxpc + 4;

		psxRecompileNextInstruction(true, false);
		psxSetBranchImm(branchTo);
		return;
	}

	const bool swap = psxTrySwapDelaySlot(_Rs_, 0, 0);
	_psxFlushAllDirty();

	const int regs = _checkX86reg(X86TYPE_PSX, _Rs_, MODE_READ);
	if (regs >= 0) {
//        xCMP(xRegister32(regs), 0);
        armAsm->Cmp(HostW(regs), 0);
    }
	else {
//        xCMP(ptr32[&psxRegs.GPR.r[_Rs_]], 0);
        armAsm->Cmp(armLoad(PTR_CPU(psxRegs.GPR.r[_Rs_])), 0);
    }

//	u32* pjmp = JG32(0);
    a64::Label pjmp;
    armAsm->B(&pjmp, a64::Condition::gt);

	if (!swap)
	{
		psxSaveBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(psxpc);

//	x86SetJ32A(pjmp);
    armBind(&pjmp);

	if (!swap)
	{
		psxpc -= 4;
		psxLoadBranchState();
		psxRecompileNextInstruction(true, false);
	}

	psxSetBranchImm(branchTo);
}

static void rpsxMFC0()
{
	// Rt = Cop0->Rd
	if (!_Rt_)
		return;

	const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_WRITE);
//	xMOV(xRegister32(rt), ptr32[&psxRegs.CP0.r[_Rd_]]);
    armLoad(HostW(rt), PTR_CPU(psxRegs.CP0.r[_Rd_]));
}

static void rpsxCFC0()
{
	// Rt = Cop0->Rd
	if (!_Rt_)
		return;

	const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_WRITE);
//	xMOV(xRegister32(rt), ptr32[&psxRegs.CP0.r[_Rd_]]);
    armLoad(HostW(rt), PTR_CPU(psxRegs.CP0.r[_Rd_]));
}

// [P12 Fix A] Helper: after MTC0 writes to Status (Rd=12), immediately check for pending interrupts.
// Interpreter checks pending IRQs at every branch (iopEventTest in doBranch). JIT only checks at
// block boundaries when cycle >= iopNextEventCycle. If MTC0 sets IEc=1 but a subsequent SYSCALL
// sets IEc=0 before the next branch test, the interrupt is permanently missed. Calling iopTestIntc()
// here ensures the pending interrupt is dispatched immediately when IEc becomes 1.
static void iopMTC0_Status_helper()
{
	static int s_mtc0_late = 0;
	if (psxRegs.cycle > 34000000u && psxRegs.cycle < 38000000u && s_mtc0_late < 30)
	{
		++s_mtc0_late;
		Console.WriteLn("@@IOP_MTC0_STATUS@@ n=%d new_sr=0x%08x IEc=%d pc=0x%08x cyc=%u",
			s_mtc0_late, psxRegs.CP0.n.Status, psxRegs.CP0.n.Status & 1u, psxRegs.pc, psxRegs.cycle);
	}
	// [P12 Fix A] If IEc=1 (interrupts enabled), force immediate event check at next
	// IOP branch boundary. Interpreter calls iopEventTest() after every branch unconditionally;
	// JIT only calls it when cycle >= iopNextEventCycle. Setting iopNextEventCycle = cycle
	// ensures iopEventTest() runs at the very next branch, matching interpreter behavior.
	if (psxRegs.CP0.n.Status & 1u)
	{
		psxRegs.iopNextEventCycle = psxRegs.cycle;
		iopTestIntc();
	}
}

static void rpsxMTC0()
{
	// Cop0->Rd = Rt
	if (PSX_IS_CONST1(_Rt_))
	{
//		xMOV(ptr32[&psxRegs.CP0.r[_Rd_]], g_psxConstRegs[_Rt_]);
        armStore(PTR_CPU(psxRegs.CP0.r[_Rd_]), g_psxConstRegs[_Rt_]);
	}
	else
	{
		const int rt = _allocX86reg(X86TYPE_PSX, _Rt_, MODE_READ);
//		xMOV(ptr32[&psxRegs.CP0.r[_Rd_]], xRegister32(rt));
        armStore(PTR_CPU(psxRegs.CP0.r[_Rd_]), HostW(rt));
	}
	// [P12] If writing to Status register (Rd=12), log the new value
	if (_Rd_ == 12)
	{
		_psxFlushCall(0);
		armEmitCall(reinterpret_cast<void*>((uptr)&iopMTC0_Status_helper));
	}
}

static void rpsxCTC0()
{
	// Cop0->Rd = Rt
	rpsxMTC0();
}

// [TEMP_DIAG] @@IOP_RFE@@ — helper to perform RFE and log
static void iopRFE_helper()
{
	static int s_rfe_cnt = 0;
	static int s_rfe_late = 0;
	const u32 old_sr = psxRegs.CP0.n.Status;
	psxRegs.CP0.n.Status = (old_sr & 0xFFFFFFF0u) | ((old_sr & 0x3Cu) >> 2);
	const u32 new_sr = psxRegs.CP0.n.Status;
	if (++s_rfe_cnt <= 20 || (s_rfe_cnt % 5000 == 0 && s_rfe_cnt <= 100000))
	{
		Console.WriteLn("@@IOP_RFE@@ n=%d old_sr=0x%08x new_sr=0x%08x pc=0x%08x",
			s_rfe_cnt, old_sr, new_sr, psxRegs.pc);
	}
	// [P12] @@IOP_RFE_LATE@@ — cyc=34M-38M の RFE を捕捉
	if (psxRegs.cycle > 34000000u && psxRegs.cycle < 38000000u && s_rfe_late < 30)
	{
		++s_rfe_late;
		Console.WriteLn("@@IOP_RFE_LATE@@ n=%d old_sr=0x%08x new_sr=0x%08x pc=0x%08x cyc=%u epc=0x%08x",
			s_rfe_late, old_sr, new_sr, psxRegs.pc, psxRegs.cycle, psxRegs.CP0.n.EPC);
	}
	// [P12 Fix B] If RFE restored IEc=1, force immediate event check (same as MTC0 fix).
	if (new_sr & 1u)
		psxRegs.iopNextEventCycle = psxRegs.cycle;
	iopTestIntc();
}

static void rpsxRFE()
{
	_psxFlushCall(0);
	armEmitCall(reinterpret_cast<void*>((uptr)&iopRFE_helper));
}

//// COP2
REC_GTE_FUNC(RTPS);
REC_GTE_FUNC(NCLIP);
REC_GTE_FUNC(OP);
REC_GTE_FUNC(DPCS);
REC_GTE_FUNC(INTPL);
REC_GTE_FUNC(MVMVA);
REC_GTE_FUNC(NCDS);
REC_GTE_FUNC(CDP);
REC_GTE_FUNC(NCDT);
REC_GTE_FUNC(NCCS);
REC_GTE_FUNC(CC);
REC_GTE_FUNC(NCS);
REC_GTE_FUNC(NCT);
REC_GTE_FUNC(SQR);
REC_GTE_FUNC(DCPL);
REC_GTE_FUNC(DPCT);
REC_GTE_FUNC(AVSZ3);
REC_GTE_FUNC(AVSZ4);
REC_GTE_FUNC(RTPT);
REC_GTE_FUNC(GPF);
REC_GTE_FUNC(GPL);
REC_GTE_FUNC(NCCT);

REC_GTE_FUNC(MFC2);
REC_GTE_FUNC(CFC2);
REC_GTE_FUNC(MTC2);
REC_GTE_FUNC(CTC2);

REC_GTE_FUNC(LWC2);
REC_GTE_FUNC(SWC2);


// R3000A tables
extern void (*rpsxBSC[64])();
extern void (*rpsxSPC[64])();
extern void (*rpsxREG[32])();
extern void (*rpsxCP0[32])();
extern void (*rpsxCP2[64])();
extern void (*rpsxCP2BSC[32])();

static void rpsxSPECIAL() { rpsxSPC[_Funct_](); }
static void rpsxREGIMM() { rpsxREG[_Rt_](); }
static void rpsxCOP0() { rpsxCP0[_Rs_](); }
static void rpsxCOP2() { rpsxCP2[_Funct_](); }
static void rpsxBASIC() { rpsxCP2BSC[_Rs_](); }

static void rpsxNULL()
{
	Console.WriteLn("psxUNK: %8.8x", psxRegs.code);
}

// clang-format off
void (*rpsxBSC[64])() = {
	rpsxSPECIAL, rpsxREGIMM, rpsxJ   , rpsxJAL  , rpsxBEQ , rpsxBNE , rpsxBLEZ, rpsxBGTZ,
	rpsxADDI   , rpsxADDIU , rpsxSLTI, rpsxSLTIU, rpsxANDI, rpsxORI , rpsxXORI, rpsxLUI ,
	rpsxCOP0   , rpsxNULL  , rpsxCOP2, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL   , rpsxNULL  , rpsxNULL, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxLB     , rpsxLH    , rpsxLWL , rpsxLW   , rpsxLBU , rpsxLHU , rpsxLWR , rpsxNULL,
	rpsxSB     , rpsxSH    , rpsxSWL , rpsxSW   , rpsxNULL, rpsxNULL, rpsxSWR , rpsxNULL,
	rpsxNULL   , rpsxNULL  , rgteLWC2, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL   , rpsxNULL  , rgteSWC2, rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};

void (*rpsxSPC[64])() = {
	rpsxSLL , rpsxNULL, rpsxSRL , rpsxSRA , rpsxSLLV   , rpsxNULL , rpsxSRLV, rpsxSRAV,
	rpsxJR  , rpsxJALR, rpsxNULL, rpsxNULL, rpsxSYSCALL, rpsxBREAK, rpsxNULL, rpsxNULL,
	rpsxMFHI, rpsxMTHI, rpsxMFLO, rpsxMTLO, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxMULT, rpsxMULTU, rpsxDIV, rpsxDIVU, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxADD , rpsxADDU, rpsxSUB , rpsxSUBU, rpsxAND    , rpsxOR   , rpsxXOR , rpsxNOR ,
	rpsxNULL, rpsxNULL, rpsxSLT , rpsxSLTU, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL   , rpsxNULL , rpsxNULL, rpsxNULL,
};

void (*rpsxREG[32])() = {
	rpsxBLTZ  , rpsxBGEZ  , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL  , rpsxNULL  , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxBLTZAL, rpsxBGEZAL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL  , rpsxNULL  , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};

void (*rpsxCP0[32])() = {
	rpsxMFC0, rpsxNULL, rpsxCFC0, rpsxNULL, rpsxMTC0, rpsxNULL, rpsxCTC0, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxRFE , rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};

void (*rpsxCP2[64])() = {
	rpsxBASIC, rgteRTPS , rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL , rgteNCLIP, rpsxNULL, // 00
	rpsxNULL , rpsxNULL , rpsxNULL , rpsxNULL, rgteOP  , rpsxNULL , rpsxNULL , rpsxNULL, // 08
	rgteDPCS , rgteINTPL, rgteMVMVA, rgteNCDS, rgteCDP , rpsxNULL , rgteNCDT , rpsxNULL, // 10
	rpsxNULL , rpsxNULL , rpsxNULL , rgteNCCS, rgteCC  , rpsxNULL , rgteNCS  , rpsxNULL, // 18
	rgteNCT  , rpsxNULL , rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL , rpsxNULL , rpsxNULL, // 20
	rgteSQR  , rgteDCPL , rgteDPCT , rpsxNULL, rpsxNULL, rgteAVSZ3, rgteAVSZ4, rpsxNULL, // 28
	rgteRTPT , rpsxNULL , rpsxNULL , rpsxNULL, rpsxNULL, rpsxNULL , rpsxNULL , rpsxNULL, // 30
	rpsxNULL , rpsxNULL , rpsxNULL , rpsxNULL, rpsxNULL, rgteGPF  , rgteGPL  , rgteNCCT, // 38
};

void (*rpsxCP2BSC[32])() = {
	rgteMFC2, rpsxNULL, rgteCFC2, rpsxNULL, rgteMTC2, rpsxNULL, rgteCTC2, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
	rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL, rpsxNULL,
};
// clang-format on

////////////////////////////////////////////////
// Back-Prob Function Tables - Gathering Info //
////////////////////////////////////////////////
#define rpsxpropSetRead(reg) \
	{ \
		if (!(pinst->regs[reg] & EEINST_USED)) \
			pinst->regs[reg] |= EEINST_LASTUSE; \
		prev->regs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->regs[reg] |= EEINST_USED; \
		_recFillRegister(*pinst, XMMTYPE_GPRREG, reg, 0); \
	}

#define rpsxpropSetWrite(reg) \
	{ \
		prev->regs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
		if (!(pinst->regs[reg] & EEINST_USED)) \
			pinst->regs[reg] |= EEINST_LASTUSE; \
		pinst->regs[reg] |= EEINST_USED; \
		_recFillRegister(*pinst, XMMTYPE_GPRREG, reg, 1); \
	}

void rpsxpropBSC(EEINST* prev, EEINST* pinst);
void rpsxpropSPECIAL(EEINST* prev, EEINST* pinst);
void rpsxpropREGIMM(EEINST* prev, EEINST* pinst);
void rpsxpropCP0(EEINST* prev, EEINST* pinst);
void rpsxpropCP2(EEINST* prev, EEINST* pinst);

//SPECIAL, REGIMM, J   , JAL  , BEQ , BNE , BLEZ, BGTZ,
//ADDI   , ADDIU , SLTI, SLTIU, ANDI, ORI , XORI, LUI ,
//COP0   , NULL  , COP2, NULL , NULL, NULL, NULL, NULL,
//NULL   , NULL  , NULL, NULL , NULL, NULL, NULL, NULL,
//LB     , LH    , LWL , LW   , LBU , LHU , LWR , NULL,
//SB     , SH    , SWL , SW   , NULL, NULL, SWR , NULL,
//NULL   , NULL  , NULL, NULL , NULL, NULL, NULL, NULL,
//NULL   , NULL  , NULL, NULL , NULL, NULL, NULL, NULL
void rpsxpropBSC(EEINST* prev, EEINST* pinst)
{
	switch (psxRegs.code >> 26)
	{
		case 0:
			rpsxpropSPECIAL(prev, pinst);
			break;
		case 1:
			rpsxpropREGIMM(prev, pinst);
			break;
		case 2: // j
			break;
		case 3: // jal
			rpsxpropSetWrite(31);
			break;
		case 4: // beq
		case 5: // bne
			rpsxpropSetRead(_Rs_);
			rpsxpropSetRead(_Rt_);
			break;

		case 6: // blez
		case 7: // bgtz
			rpsxpropSetRead(_Rs_);
			break;

		case 15: // lui
			rpsxpropSetWrite(_Rt_);
			break;

		case 16:
			rpsxpropCP0(prev, pinst);
			break;
		case 18:
			rpsxpropCP2(prev, pinst);
			break;

		// stores
		case 40:
		case 41:
		case 42:
		case 43:
		case 46:
			rpsxpropSetRead(_Rt_);
			rpsxpropSetRead(_Rs_);
			break;

		case 50: // LWC2
		case 58: // SWC2
			// Operation on COP2 registers/memory. GPRs are left untouched
			break;

		default:
			rpsxpropSetWrite(_Rt_);
			rpsxpropSetRead(_Rs_);
			break;
	}
}

//SLL , NULL, SRL , SRA , SLLV   , NULL , SRLV, SRAV,
//JR  , JALR, NULL, NULL, SYSCALL, BREAK, NULL, NULL,
//MFHI, MTHI, MFLO, MTLO, NULL   , NULL , NULL, NULL,
//MULT, MULTU, DIV, DIVU, NULL   , NULL , NULL, NULL,
//ADD , ADDU, SUB , SUBU, AND    , OR   , XOR , NOR ,
//NULL, NULL, SLT , SLTU, NULL   , NULL , NULL, NULL,
//NULL, NULL, NULL, NULL, NULL   , NULL , NULL, NULL,
//NULL, NULL, NULL, NULL, NULL   , NULL , NULL, NULL,
void rpsxpropSPECIAL(EEINST* prev, EEINST* pinst)
{
	switch (_Funct_)
	{
		case 0: // SLL
		case 2: // SRL
		case 3: // SRA
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(_Rt_);
			break;

		case 8: // JR
			rpsxpropSetRead(_Rs_);
			break;
		case 9: // JALR
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(_Rs_);
			break;

		case 12: // syscall
		case 13: // break
			_recClearInst(prev);
			prev->info = 0;
			break;
		case 15: // sync
			break;

		case 16: // mfhi
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(PSX_HI);
			break;
		case 17: // mthi
			rpsxpropSetWrite(PSX_HI);
			rpsxpropSetRead(_Rs_);
			break;
		case 18: // mflo
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(PSX_LO);
			break;
		case 19: // mtlo
			rpsxpropSetWrite(PSX_LO);
			rpsxpropSetRead(_Rs_);
			break;

		case 24: // mult
		case 25: // multu
		case 26: // div
		case 27: // divu
			rpsxpropSetWrite(PSX_LO);
			rpsxpropSetWrite(PSX_HI);
			rpsxpropSetRead(_Rs_);
			rpsxpropSetRead(_Rt_);
			break;

		case 32: // add
		case 33: // addu
		case 34: // sub
		case 35: // subu
			rpsxpropSetWrite(_Rd_);
			if (_Rs_)
				rpsxpropSetRead(_Rs_);
			if (_Rt_)
				rpsxpropSetRead(_Rt_);
			break;

		default:
			rpsxpropSetWrite(_Rd_);
			rpsxpropSetRead(_Rs_);
			rpsxpropSetRead(_Rt_);
			break;
	}
}

//BLTZ  , BGEZ  , NULL, NULL, NULL, NULL, NULL, NULL,
//NULL  , NULL  , NULL, NULL, NULL, NULL, NULL, NULL,
//BLTZAL, BGEZAL, NULL, NULL, NULL, NULL, NULL, NULL,
//NULL  , NULL  , NULL, NULL, NULL, NULL, NULL, NULL
void rpsxpropREGIMM(EEINST* prev, EEINST* pinst)
{
	switch (_Rt_)
	{
		case 0: // bltz
		case 1: // bgez
			rpsxpropSetRead(_Rs_);
			break;

		case 16: // bltzal
		case 17: // bgezal
			// do not write 31
			rpsxpropSetRead(_Rs_);
			break;

			jNO_DEFAULT
	}
}

//MFC0, NULL, CFC0, NULL, MTC0, NULL, CTC0, NULL,
//NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
//RFE , NULL, NULL, NULL, NULL, NULL, NULL, NULL,
//NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
void rpsxpropCP0(EEINST* prev, EEINST* pinst)
{
	switch (_Rs_)
	{
		case 0: // mfc0
		case 2: // cfc0
			rpsxpropSetWrite(_Rt_);
			break;

		case 4: // mtc0
		case 6: // ctc0
			rpsxpropSetRead(_Rt_);
			break;
		case 16: // rfe
			break;

			jNO_DEFAULT
	}
}


// Basic table:
// gteMFC2, psxNULL, gteCFC2, psxNULL, gteMTC2, psxNULL, gteCTC2, psxNULL,
// psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
// psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
// psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL, psxNULL,
void rpsxpropCP2_basic(EEINST* prev, EEINST* pinst)
{
	switch (_Rs_)
	{
		case 0: // mfc2
		case 2: // cfc2
			rpsxpropSetWrite(_Rt_);
			break;

		case 4: // mtc2
		case 6: // ctc2
			rpsxpropSetRead(_Rt_);
			break;

		default:
			pxFail("iop invalid opcode in const propagation (rpsxpropCP2/BASIC)");
			break;
	}
}


// Main table:
// psxBASIC, gteRTPS , psxNULL , psxNULL, psxNULL, psxNULL , gteNCLIP, psxNULL, // 00
// psxNULL , psxNULL , psxNULL , psxNULL, gteOP  , psxNULL , psxNULL , psxNULL, // 08
// gteDPCS , gteINTPL, gteMVMVA, gteNCDS, gteCDP , psxNULL , gteNCDT , psxNULL, // 10
// psxNULL , psxNULL , psxNULL , gteNCCS, gteCC  , psxNULL , gteNCS  , psxNULL, // 18
// gteNCT  , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL, // 20
// gteSQR  , gteDCPL , gteDPCT , psxNULL, psxNULL, gteAVSZ3, gteAVSZ4, psxNULL, // 28
// gteRTPT , psxNULL , psxNULL , psxNULL, psxNULL, psxNULL , psxNULL , psxNULL, // 30
// psxNULL , psxNULL , psxNULL , psxNULL, psxNULL, gteGPF  , gteGPL  , gteNCCT, // 38
void rpsxpropCP2(EEINST* prev, EEINST* pinst)
{
	switch (_Funct_)
	{
		case 0: // Basic opcode
			rpsxpropCP2_basic(prev, pinst);
			break;

		default:
			// COP2 operation are likely done with internal COP2 registers
			// No impact on GPR
			break;
	}
}
