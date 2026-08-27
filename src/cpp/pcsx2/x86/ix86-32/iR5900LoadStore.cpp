// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "x86/iR5900.h"
#include "x86/iR5900LoadStore.h"
#include "common/Darwin/DarwinMisc.h"

// [iter220] TEMP_DIAG: LW result probe
extern "C" void armsx2_probe_lw_result_9fc433f0(u32 result, u32 slot);

#if !defined(__ANDROID__)
using namespace x86Emitter;
#endif

#define REC_STORES
#define REC_LOADS

// [TEMP_DIAG][REMOVE_AFTER=LW_ONESHOT_CAP_V1] LW one-shot capture gate
// Enable to capture actual LW result from host register at specific PC
#ifndef iPSX2_ENABLE_LW_ONESHOT_CAP
#define iPSX2_ENABLE_LW_ONESHOT_CAP 0
#endif

struct LfCmpStoreEntry
{
	u32 sample_kind; // 1=LW sample, 2=CMP sample
	u32 branch_pc;
	u32 lw_pc;
	u32 lhs_cpu;
	u32 lhs_swap;
	u32 rhs_cpu;
	u32 rhs_expected;
	u32 lw_raw;
	u32 lw_swapped;
	u32 t0_host;
	u32 t0_cpu;
	u32 eff_addr;
	u32 v0_cpu;
	u32 taken;
};

constexpr u32 LF_CMP_STORE_RING_SIZE = 512;
constexpr u32 LF_CMP_STORE_RING_MASK = LF_CMP_STORE_RING_SIZE - 1;
extern volatile LfCmpStoreEntry g_lf_cmp_store_ring[LF_CMP_STORE_RING_SIZE];
extern volatile u32 g_lf_cmp_store_idx;

// Capture globals for LW at pc=0xBFC02664
alignas(64) volatile u32 g_lw_seen = 0;
alignas(64) volatile u32 g_lw_guest_pc = 0;
alignas(64) volatile u32 g_lw_addr = 0;      // effective address used for LW
alignas(64) volatile u32 g_lw_val = 0;       // value loaded into dest host reg
alignas(64) volatile u32 g_lw_path = 0;      // 1=fastmem, 2=vtlb, 3=other
alignas(64) volatile u32 g_lf_lw_eff_last = 0;
alignas(64) volatile u32 g_ra_stack64_addr_shadow = 0;
alignas(64) volatile u64 g_ra_stack64_val_shadow = 0;
alignas(64) volatile u32 g_ra_stack64_addr_shadow_caller = 0;
alignas(64) volatile u64 g_ra_stack64_val_shadow_caller = 0;
alignas(64) volatile u64 g_ra_stack64_val_shadow_431xx = 0;

// Unified Logging for JIT Memory Path
extern "C" void LogUnified(const char* fmt, ...);
extern "C" void LogLHCHECK(u32 pc, u32 addr, u32 val) {
    if (pc == 0xbfc02688) { // LH is at 0xbfc02684, recompiler pc is at 0xbfc02688
        fprintf(stderr, "@@LH_PROOF@@ pc=%08x t0=%08x mem16=%04x v0_after_lh=%08x\n", pc - 4, addr - 8, val & 0xFFFF, val);
    }
}

extern "C" void vtlb_MemWrite32_KSEG1(u32 addr, u32 data);
extern "C" void vtlb_MemWrite8_KSEG1(u32 addr, u8 data);  // [iter24]
extern "C" void TraceRaStack64Probe(u32 kind, u32 guest_pc, u32 sp, u32 addr, u32 ra_before, u64 val)
{
	static u32 s_count = 0;
	constexpr u32 kCap = 80;
	if (s_count >= kCap)
		return;

	Console.WriteLn(
		"@@RA_STACK64_PROBE@@ idx=%u kind=%s pc=%08x sp=%08x addr=%08x ra=%08x val=%016llx",
		s_count, (kind == 0) ? "LD" : ((kind == 1) ? "SD" : "SD_POST"), guest_pc, sp, addr, ra_before, (unsigned long long)val);
	s_count++;
}

extern "C" void TraceRaStack64StorePost(u32 guest_pc, u32 sp, u32 addr, u32 ra_before)
{
	TraceRaStack64Probe(2, guest_pc, sp, addr, ra_before, memRead64(addr));
}

extern "C" void LogLw41048Runtime(u32 guest_pc, u32 addr, u32 val)
{
	static u32 s_count = 0;
	if (s_count >= 50)
		return;
	Console.WriteLn("@@LW41048_RUNTIME@@ n=%u pc=%08x addr=%08x val=%08x", s_count, guest_pc, addr, val);
	s_count++;
}

extern "C" void LogLfLwRuntime(u32 guest_pc, u32 addr, u32 val)
{
	static int s_probe_enabled = -1;
	if (s_probe_enabled < 0)
	{
		s_probe_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_PROBE_LF_LW_RT", false) ? 1 : 0;
	}
	if (s_probe_enabled == 0)
		return;

	static u32 s_count_2668 = 0;
	static u32 s_count_2674 = 0;
	static u32 s_count_2698 = 0;
	static u32 s_count_26b4 = 0;
	static u32 s_count_26bc = 0;
	static u32 s_count_26cc = 0;
	static u32 s_count_26d0 = 0;
	u32* ctr = nullptr;
	u32 cap = 0;
	switch (guest_pc)
	{
		case 0xBFC02668: ctr = &s_count_2668; cap = 128; break;
		case 0xBFC02674: ctr = &s_count_2674; cap = 128; break;
		case 0xBFC02698: ctr = &s_count_2698; cap = 12; break;
		case 0xBFC026B4: ctr = &s_count_26b4; cap = 12; break;
		case 0xBFC026BC: ctr = &s_count_26bc; cap = 12; break;
		case 0xBFC026CC: ctr = &s_count_26cc; cap = 12; break;
		case 0xBFC026D0: ctr = &s_count_26d0; cap = 12; break;
		default: return;
	}

	if (*ctr >= cap)
		return;
	++(*ctr);
	Console.WriteLn("@@LF_LW_RT@@ pc=%08x n=%u addr=%08x val=%08x", guest_pc, *ctr, addr, val);
}

extern "C" u32 ReadGuestMem32ForLfLoad(u32 addr)
{
	return memRead32(addr);
}

extern "C" void WriteGuestMem64ForRaStack(u32 addr, u64 value)
{
	memWrite64(addr, value);
}

// [iter214] Safe helper that computes address from cpuRegs.sp + imm_offset
// bypassing JIT register passing (ECX=w1 was receiving garbage 0x63)
extern "C" void WriteGuestMem64ForRaStack_Safe(s32 imm_offset, u64 value)
{
	u32 sp = cpuRegs.GPR.n.sp.UL[0];
	u32 addr = sp + (u32)imm_offset;
	static u32 s_wr_n = 0;
	if (s_wr_n < 5) {
		Console.WriteLn("@@WR64_SAFE@@ n=%u sp=%08x imm=%d addr=%08x val=%016llx",
			s_wr_n, sp, imm_offset, addr, (unsigned long long)value);
		s_wr_n++;
	}
	memWrite64(addr, value);
}

extern "C" u64 ReadGuestMem64ForRaStack(u32 addr)
{
	return memRead64(addr);
}

// [iter214] Safe helper that computes address from cpuRegs.sp + imm_offset
extern "C" u64 ReadGuestMem64ForRaStack_Safe(s32 imm_offset)
{
	u32 sp = cpuRegs.GPR.n.sp.UL[0];
	u32 addr = sp + (u32)imm_offset;
	u64 result = memRead64(addr);
	// [R62] @@SAFE_LD_RA@@ probe: check if result matches stale 0x2058e0
	{
		static u32 s_cnt = 0;
		u32 lo = (u32)result;
		if (lo == 0x002058e0u && s_cnt < 10) {
			Console.WriteLn("@@SAFE_LD_RA@@ n=%u sp=%08x imm=%d addr=%08x result=%016llx pc=%08x cycle=%u",
				s_cnt, sp, imm_offset, addr, (unsigned long long)result, cpuRegs.pc, cpuRegs.cycle);
			s_cnt++;
		}
		// Also log first few calls with non-stale values after restart
		static u32 s_ok_cnt = 0;
		if (lo != 0x002058e0u && cpuRegs.pc >= 0x002058d0u && cpuRegs.pc <= 0x002058f8u && s_ok_cnt < 5) {
			Console.WriteLn("@@SAFE_LD_RA_OK@@ n=%u sp=%08x addr=%08x result=%016llx pc=%08x cycle=%u",
				s_ok_cnt, sp, addr, (unsigned long long)result, cpuRegs.pc, cpuRegs.cycle);
			s_ok_cnt++;
		}
	}
	return result;
}

static int RETURN_READ_IN_RAX()
{
	return -1;  // [iter402] sentinel: result stays in RAX (x0). -1 != any slot index (>=0).
}

static bool IsDiagFlagsEnabled();

static bool IsLfLoadBaseCpuEnabled()
{
	if (iPSX2_IsSafeOnlyEnabled() || !IsDiagFlagsEnabled())
		return false;
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_LF_LOAD_BASE_CPU", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_LF_LOAD_BASE_CPU=%d", s_enabled);
	}
	return (s_enabled == 1);
}

static bool IsLfLwProbeEnabled()
{
	if (iPSX2_IsSafeOnlyEnabled())
		return false;
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_PROBE_LF_LW_RT", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_PROBE_LF_LW_RT=%d", s_enabled);
	}
	return (s_enabled == 1);
}

static bool IsLfLwOneShotEnabled()
{
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_PROBE_LF_LW_ONESHOT", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_PROBE_LF_LW_ONESHOT=%d", s_enabled);
	}
	return (s_enabled == 1);
}

static bool IsLfHotCallProbeEnabled()
{
	if (iPSX2_IsSafeOnlyEnabled() || !IsDiagFlagsEnabled())
		return false;
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_LF_HOT_CALL_PROBE", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_LF_HOT_CALL_PROBE=%d", s_enabled);
	}
	return (s_enabled == 1);
}

static bool IsLfStoreProbeEnabled()
{
	if (iPSX2_IsSafeOnlyEnabled() || !IsDiagFlagsEnabled())
		return false;
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		const char* env = iPSX2_GetRuntimeEnv("iPSX2_LF_STORE_PROBE");
		s_enabled = (env && env[0] == '1') ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_LF_STORE_PROBE=%d", s_enabled);
	}
	return (s_enabled == 1);
}

static bool IsRecLoadVerboseEnabled()
{
	if (iPSX2_IsSafeOnlyEnabled())
		return false;
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_DEBUG_RECLOAD", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_DEBUG_RECLOAD=%d", s_enabled);
	}
	return (s_enabled == 1);
}

static bool IsRaSdPredecFixEnabled()
{
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		s_enabled = 1;
		Console.WriteLn("@@CFG@@ iPSX2_FIX_RA_SD_PREDEC_9FC43434=%d", s_enabled);
	}
	return (s_enabled == 1);
}

static bool IsRaStackShadowFixEnabled()
{
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		s_enabled = 1;
		Console.WriteLn("@@CFG@@ iPSX2_FIX_RA_STACK_SHADOW_9FC434XX=%d", s_enabled);
	}
	return (s_enabled == 1);
}

static bool IsBiosRaStack64RwFixEnabled()
{
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_FIX_BIOS_RA_STACK64_RW", true) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_FIX_BIOS_RA_STACK64_RW=%d", s_enabled);
	}
	return (s_enabled == 1);
}

static bool IsDiagFlagsEnabled()
{
	static int s_enabled = -1;
	if (s_enabled < 0)
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_ENABLE_DIAG_FLAGS", false) ? 1 : 0;
	return (s_enabled == 1);
}

static bool IsLfForceV0CommitEnabled()
{
	if (iPSX2_IsSafeOnlyEnabled() || !IsDiagFlagsEnabled())
		return false;
	static int s_enabled = -1;
	if (s_enabled < 0)
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_LF_FORCE_V0_COMMIT", false) ? 1 : 0;
	return (s_enabled == 1);
}

static bool IsLfNoConstLoadEnabled()
{
	if (iPSX2_IsSafeOnlyEnabled() || !IsDiagFlagsEnabled())
		return false;
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_LF_LOAD_NO_CONST", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_LF_LOAD_NO_CONST=%d", s_enabled);
	}
	return (s_enabled == 1);
}

static bool IsLfR8PairAddrFixEnabled()
{
	if (iPSX2_IsSafeOnlyEnabled() || !IsDiagFlagsEnabled())
		return false;
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_LF_R8_PAIR_ADDR_FIX", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_LF_R8_PAIR_ADDR_FIX=%d", s_enabled);
	}
	return (s_enabled == 1);
}


namespace R5900::Dynarec::OpcodeImpl
{

/*********************************************************
* Load and store for GPR                                 *
* Format:  OP rt, offset(base)                           *
*********************************************************/
#ifndef LOADSTORE_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_FUNC_DEL(LB, _Rt_);
REC_FUNC_DEL(LBU, _Rt_);
REC_FUNC_DEL(LH, _Rt_);
REC_FUNC_DEL(LHU, _Rt_);
REC_FUNC_DEL(LW, _Rt_);
REC_FUNC_DEL(LWU, _Rt_);
REC_FUNC_DEL(LWL, _Rt_);
REC_FUNC_DEL(LWR, _Rt_);
REC_FUNC_DEL(LD, _Rt_);
REC_FUNC_DEL(LDR, _Rt_);
REC_FUNC_DEL(LDL, _Rt_);
REC_FUNC_DEL(LQ, _Rt_);
REC_FUNC(SB);
REC_FUNC(SH);
REC_FUNC(SW);
REC_FUNC(SWL);
REC_FUNC(SWR);
REC_FUNC(SD);
REC_FUNC(SDL);
REC_FUNC(SDR);
REC_FUNC(SQ);
REC_FUNC(LWC1);
REC_FUNC(SWC1);
REC_FUNC(LQC2);
REC_FUNC(SQC2);

#else

using namespace Interpreter::OpcodeImpl;

//////////////////////////////////////////////////////////////////////////////////////////
//
static void recLoadQuad(u32 bits, bool sign)
{
	pxAssume(bits == 128);

	// This mess is so we allocate *after* the vtlb flush, not before.
	vtlb_ReadRegAllocCallback alloc_cb = nullptr;
	if (_Rt_)
		alloc_cb = []() { return _allocGPRtoXMMreg(_Rt_, MODE_WRITE); };

	int xmmreg;
	const bool lf_no_const_load = IsLfNoConstLoadEnabled() && (pc >= 0xBFC02660 && pc <= 0xBFC026A0);
	if (GPR_IS_CONST1(_Rs_) && !lf_no_const_load)
	{
		const u32 srcadr = (g_cpuConstRegs[_Rs_].UL[0] + _Imm_) & ~0x0f;
		xmmreg = vtlb_DynGenReadQuad_Const(bits, srcadr, _Rt_ ? alloc_cb : nullptr);
	}
	else
	{
		// Load ECX with the source memory address that we're reading from.
        _freeX86reg(ECX);
//		_eeMoveGPRtoR(arg1reg, _Rs_);
        _eeMoveGPRtoR(RCX, _Rs_);
		if (_Imm_ != 0) {
//            xADD(arg1regd, _Imm_);
            armAsm->Add(ECX, ECX, _Imm_);
        }

		// force 16 byte alignment on 128 bit reads
//		xAND(arg1regd, ~0x0F);
        armAsm->And(ECX, ECX, ~0x0F);

		xmmreg = vtlb_DynGenReadQuad(bits, ECX.GetCode(), _Rt_ ? alloc_cb : nullptr);
	}

	// if there was a constant, it should have been invalidated.
	pxAssert(!_Rt_ || !GPR_IS_CONST1(_Rt_));
	if (!_Rt_)
		_freeXMMreg(xmmreg);
}

//////////////////////////////////////////////////////////////////////////////////////////
//
namespace Interp = R5900::Interpreter::OpcodeImpl;

// ...

static void recLoad(u32 bits, bool sign)
{



	pxAssume(bits <= 64);



	// This mess is so we allocate *after* the vtlb flush, not before.
	// TODO(Stenzek): If not live, save directly to state, and delete constant.
	vtlb_ReadRegAllocCallback alloc_cb = nullptr;
	if (_Rt_)
		alloc_cb = []() { return _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE); };

    // [TEMP_DIAG] BIOS Load Trace (runtime-gated; default OFF)
    if (IsRecLoadVerboseEnabled() && pc >= 0xbfc00000 && pc <= 0xbfffffff) {
        static int s_bios_load_count = 0;
        if (s_bios_load_count < 3) {
            bool isConst = GPR_IS_CONST1(_Rs_);
            Console.WriteLn("@@RECLOAD_BIOS@@ pc=%08x bits=%u sign=%d isConst=%d", pc, bits, sign, isConst);
            s_bios_load_count++;
        }
    }

    if (IsRecLoadVerboseEnabled())
	{
		const bool isConst = GPR_IS_CONST1(_Rs_);
		const u32 constVal = isConst ? g_cpuConstRegs[_Rs_].UL[0] : 0;
		Console.WriteLn("DEBUG: Emit recLoad pc=%08x bits=%d Rs=%d IsConst=%d Val=%08x", pc, bits, _Rs_, isConst, constVal);
	}

	int x86reg;
	const bool lf_no_const_load = IsLfNoConstLoadEnabled() && (pc >= 0xBFC02660 && pc <= 0xBFC026A0);
	const bool bios_stack64_sp_guard = (bits == 64 && _Rs_ == 29 &&
		((pc >= 0x9FC432EC && pc <= 0x9FC43310) || pc == 0x9FC43454 || pc == 0x9FC43934));
	// [R62] Removed PC range check — ld $ra,offset($sp) safe path for ALL addresses.
	// Evidence: OSDSYS stuck loop at 0x2058e0 where vtlb 64-bit load result never reaches cpuRegs.
	const bool bios_ra_stack64_guard = (bits == 64 && _Rt_ == 31 && _Rs_ == 29);
    // [iter214] compile-time trace for LD guard activation
    // [P12 TEMP_DIAG] @@LD_RA_GUARD@@ extended: log sp const-prop and cpuRegs.sp at compile time
    {
        static u32 s_ld_guard_n = 0;
        if (bios_ra_stack64_guard && s_ld_guard_n < 30) {
            const bool sp_is_const = GPR_IS_CONST1(29);
            const u32 sp_const_val = sp_is_const ? g_cpuConstRegs[29].UL[0] : 0;
            const u32 sp_cpu_val   = cpuRegs.GPR.n.sp.UL[0];
            Console.WriteLn("@@LD_RA_GUARD@@ n=%u pc=%08x imm=%d sp_is_const=%d sp_const=%08x sp_cpu=%08x eff_addr=%08x",
                s_ld_guard_n, pc, (int)_Imm_,
                (int)sp_is_const, sp_const_val, sp_cpu_val,
                (sp_is_const ? sp_const_val : sp_cpu_val) + (u32)_Imm_);
            s_ld_guard_n++;
        }
    }
    // DEBUG: Compile-Time Log to verify block recompilation
    if (IsRecLoadVerboseEnabled() && bits == 16 && sign && pc >= 0xbfc02680 && pc <= 0xbfc02690) {
        Console.WriteLn("@@JIT_COMPILE@@ LH pc=%08x Rs=%d Imm=%d", pc, (int)_Rs_, (int)_Imm_);
    }

	// [TEMP_DIAG] @@RECLOAD_431AC@@ one-shot: confirm GPR_IS_CONST1($s3) and srcadr
	if (pc == 0x9FC431ACu && bits == 32)
	{
		static bool s_431ac_seen = false;
		if (!s_431ac_seen)
		{
			s_431ac_seen = true;
			const bool isConst = GPR_IS_CONST1(_Rs_) && !lf_no_const_load && !bios_ra_stack64_guard;
			const u32 constVal = GPR_IS_CONST1(_Rs_) ? g_cpuConstRegs[_Rs_].UL[0] : 0;
			Console.WriteLn("[TEMP_DIAG] @@RECLOAD_431AC@@ isConst=%d Rs=%d constVal=%08x imm=%d srcadr=%08x",
				(int)isConst, (int)_Rs_, constVal, (int)_Imm_, constVal + (u32)_Imm_);
		}
	}
	if (GPR_IS_CONST1(_Rs_) && !lf_no_const_load && !bios_ra_stack64_guard)
	{
		const u32 srcadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
        if (IsRecLoadVerboseEnabled())
            Console.WriteLn("DEBUG: -> Const Path srcadr=%08x", srcadr);
		x86reg = vtlb_DynGenReadNonQuad_Const(bits, sign, false, srcadr, alloc_cb);

		if (IsLfHotCallProbeEnabled() && bits == 32 && pc == 0x9FC4104C)
		{
			armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
			armAsm->Push(a64::x29, a64::lr);
			armAsm->Mov(a64::w0, pc);
			armAsm->Mov(a64::w1, srcadr);
			armAsm->Mov(a64::w2, a64::WRegister(HostGprPhys(x86reg)));
			armEmitCall((void*)LogLw41048Runtime);
			armAsm->Pop(a64::lr, a64::x29);
			armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
		}
        
#if iPSX2_ENABLE_LW_ONESHOT_CAP
        // [TEMP_DIAG] Store-only capture of LW result in LoadFile region (CONST path)
        // Widened to PC range 0xBFC02660-0xBFC02680 to catch any LW in that region
        if (IsLfLwOneShotEnabled() && bits == 32 && pc >= 0xBFC02660 && pc < 0xBFC02680) {
            a64::Label skip_lw_capture_const;
            
            // Check if already captured
            armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lw_seen));
            armAsm->Ldr(a64::w14, a64::MemOperand(a64::x15));
            armAsm->Cbnz(a64::w14, &skip_lw_capture_const);
            
            // Capture! Set seen=1
            armAsm->Mov(a64::w14, 1);
            armAsm->Str(a64::w14, a64::MemOperand(a64::x15));
            
            // Store guest_pc
            armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lw_guest_pc));
            armAsm->Mov(a64::w14, pc);
            armAsm->Str(a64::w14, a64::MemOperand(a64::x15));
            
            // Store addr (compile-time constant)
            armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lw_addr));
            armAsm->Mov(a64::w14, srcadr);
            armAsm->Str(a64::w14, a64::MemOperand(a64::x15));
            
            // Store val (from destination host register after load)
            armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lw_val));
            armAsm->Str(a64::WRegister(HostGprPhys(x86reg)), a64::MemOperand(a64::x15));
            
            // Store path = 1 (fastmem/const path)
            armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lw_path));
            armAsm->Mov(a64::w14, 1);
            armAsm->Str(a64::w14, a64::MemOperand(a64::x15));
            
            armBind(&skip_lw_capture_const);
        }
#endif
	}
		else
		{
			if (lf_no_const_load)
			{
			static bool s_lf_no_const_load_logged = false;
			if (!s_lf_no_const_load_logged)
			{
				s_lf_no_const_load_logged = true;
				Console.WriteLn("@@JIT_FIX@@ LF_LOAD_NO_CONST=1");
			}
			}

			// Load arg1 with the source memory address that we're reading from.
			_freeX86reg(ECX);
			if (bios_ra_stack64_guard)
			{
				armLoad(ECX, PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));
			}
			else
			{
				_eeMoveGPRtoR(ECX, _Rs_);
			}
			if (_Imm_ != 0)
			{
				armAsm->Add(ECX, ECX, _Imm_);
			}

			if (IsLfStoreProbeEnabled() && bits == 32 && _Rt_ == 2 && (pc - 4) == 0xBFC02664)
			{
				armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_lw_eff_last));
				armAsm->Str(ECX, a64::MemOperand(a64::x15));
			}

			if (bios_ra_stack64_guard)
			{
				armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_ra_stack64_addr_shadow));
				armAsm->Str(ECX, a64::MemOperand(a64::x15));
			}

		// [iter214] BUG FIX: also route bios_ra_stack64_guard through safe helper.
		// Previously only bios_stack64_sp_guard (specific PCs) used safe path;
		// LD $ra,offset($sp) at other PCs used buggy vtlb_DynGenReadNonQuad.
		if (bits == 64 && _Rt_ != 0 && (bios_stack64_sp_guard || bios_ra_stack64_guard) && IsBiosRaStack64RwFixEnabled())
		{
			// [iter215] Flush sp to cpuRegs before safe helper - JIT may hold sp
			// only in host register, causing stale cpuRegs.sp read in helper.
			int sp_slot = _checkX86reg(X86TYPE_GPR, 29, MODE_READ);
			if (sp_slot >= 0)
				armAsm->Str(HostW(sp_slot), PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));

			x86reg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
			armAsm->Push(a64::x0, a64::x1);
			armAsm->Push(a64::x29, a64::lr);
			armAsm->Mov(a64::w0, (s32)_Imm_);
			armEmitCall((void*)ReadGuestMem64ForRaStack_Safe);
			armAsm->Mov(HostX(x86reg), a64::x0);
			// [R62] Belt-and-suspenders: also write result directly to cpuRegs
			// so that even if the host register is evicted/corrupted before jr $ra,
			// cpuRegs.GPR.r[31] has the correct value for the direct armLoad in recJR.
			armAsm->Str(a64::w0, PTR_CPU(cpuRegs.GPR.r[_Rt_].UL[0]));
			armAsm->Lsr(a64::x0, a64::x0, 32);
			armAsm->Str(a64::w0, PTR_CPU(cpuRegs.GPR.r[_Rt_].UL[1]));
			armAsm->Pop(a64::lr, a64::x29);
			armAsm->Pop(a64::x0, a64::x1);
		}
		else
		{
			x86reg = vtlb_DynGenReadNonQuad(bits, sign, false, ECX.GetCode(), alloc_cb);
		}
	}

	// [iter220] TEMP_DIAG: runtime probe after LW at 9FC433F0
	if (pc == 0x9FC433F4u && bits == 32 && _Rt_ != 0)
	{
		armAsm->Push(a64::x0, a64::x1);
		armAsm->Push(a64::x29, a64::lr);
		armAsm->Mov(a64::w0, a64::WRegister(HostGprPhys(x86reg)));
		armAsm->Mov(a64::w1, (u32)x86reg);
		armEmitCall((void*)armsx2_probe_lw_result_9fc433f0);
		armAsm->Pop(a64::x29, a64::lr);
		armAsm->Pop(a64::x0, a64::x1);
	}

	if (bios_ra_stack64_guard)
	{
		if (pc == 0x9FC43454)
		{
			armMoveAddressToReg(a64::x15, const_cast<u64*>(&g_ra_stack64_val_shadow));
			armAsm->Ldr(HostX(x86reg), a64::MemOperand(a64::x15));
		}
		if (pc == 0x9FC43934)
		{
			armMoveAddressToReg(a64::x15, const_cast<u64*>(&g_ra_stack64_val_shadow_caller));
			armAsm->Ldr(HostX(x86reg), a64::MemOperand(a64::x15));
		}
		if (pc == 0x9FC432F0)
		{
			armMoveAddressToReg(a64::x15, const_cast<u64*>(&g_ra_stack64_val_shadow_431xx));
			armAsm->Ldr(HostX(x86reg), a64::MemOperand(a64::x15));
		}
		armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
		armAsm->Push(a64::x4, a64::x5);
		armAsm->Push(a64::x29, a64::lr);
		armAsm->Mov(a64::x17, HostX(x86reg));
		armAsm->Mov(a64::w0, 0u);
		armAsm->Mov(a64::w1, pc);
		armLoad(a64::w2, PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));
		if (GPR_IS_CONST1(_Rs_) && !lf_no_const_load && !bios_ra_stack64_guard)
			armAsm->Mov(a64::w3, g_cpuConstRegs[_Rs_].UL[0] + _Imm_);
		else
		{
			armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_ra_stack64_addr_shadow));
			armAsm->Ldr(a64::w3, a64::MemOperand(a64::x15));
		}
		armLoad(a64::w4, PTR_CPU(cpuRegs.GPR.n.ra.UL[0]));
		armAsm->Mov(a64::x5, a64::x17);
		armEmitCall((void*)TraceRaStack64Probe);
		armAsm->Pop(a64::lr, a64::x29);
		armAsm->Pop(a64::x4, a64::x5);
		armAsm->Pop(a64::x0, a64::x1, a64::x2, a64::x3);
	}

		// One-shot runtime probe for BIOS wait-loop load result.
		if (IsLfHotCallProbeEnabled() && bits == 32 && pc == 0x9FC4104C)
		{
			armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
			armAsm->Push(a64::x29, a64::lr);
			armAsm->Mov(a64::w0, pc);
			armAsm->Mov(a64::w1, ECX);
			armAsm->Mov(a64::w2, a64::WRegister(HostGprPhys(x86reg)));
			armEmitCall((void*)LogLw41048Runtime);
			armAsm->Pop(a64::lr, a64::x29);
			armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
		}
		if (IsLfLwProbeEnabled() && bits == 32 && (pc == 0xBFC02664 || pc == 0xBFC02668 || pc == 0xBFC02674 || pc == 0xBFC02698 ||
			pc == 0xBFC026B4 || pc == 0xBFC026BC || pc == 0xBFC026CC || pc == 0xBFC026D0))
		{
			armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
			armAsm->Push(a64::x29, a64::lr);
			armAsm->Mov(a64::w0, pc);
			armAsm->Mov(a64::w1, ECX);
			armAsm->Mov(a64::w2, a64::WRegister(HostGprPhys(x86reg)));
			armEmitCall((void*)LogLfLwRuntime);
			armAsm->Pop(a64::lr, a64::x29);
			armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
		}
		// Root fix: commit LoadFile signature LW result to cpuRegs.v0 immediately.
		// This avoids stale host-reg mappings leaking into subsequent BNE comparisons.
		const u32 lf_pc_commit = pc - 4;
		if (IsLfForceV0CommitEnabled() && bits == 32 && _Rt_ == 2 &&
			(lf_pc_commit == 0xBFC02664 || lf_pc_commit == 0xBFC02674))
			armAsm->Str(a64::WRegister(HostGprPhys(x86reg)), PTR_CPU(cpuRegs.GPR.r[2].UL[0]));

	// Store-only truth probe for LoadFile LW at bfc02664.
	// Captures host-side effective address and loaded value in the same ring as CMP samples.
	if (IsLfStoreProbeEnabled() && bits == 32 && _Rt_ == 2 && lf_pc_commit == 0xBFC02664)
	{
		armMoveAddressToReg(a64::x11, const_cast<u32*>(&g_lf_cmp_store_idx));
		armAsm->Ldr(a64::w12, a64::MemOperand(a64::x11));
		armAsm->And(a64::w13, a64::w12, LF_CMP_STORE_RING_MASK);
		armAsm->Mov(a64::w14, sizeof(LfCmpStoreEntry));
		armAsm->Mul(a64::w13, a64::w13, a64::w14);
		armMoveAddressToReg(a64::x14, const_cast<LfCmpStoreEntry*>(&g_lf_cmp_store_ring[0]));
		armAsm->Add(a64::x14, a64::x14, a64::x13);

		armAsm->Mov(a64::w15, 1u);
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, sample_kind)));
		armAsm->Mov(a64::w15, 0xBFC02664u);
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, branch_pc)));
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, lw_pc)));
		armAsm->Mov(a64::w15, 0u);
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, lhs_cpu)));
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, lhs_swap)));
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, rhs_cpu)));
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, rhs_expected)));

		armAsm->Str(a64::WRegister(HostGprPhys(x86reg)), a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, lw_raw)));
		armAsm->Rev(a64::w16, a64::WRegister(HostGprPhys(x86reg)));
		armAsm->Str(a64::w16, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, lw_swapped)));
		armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_lw_eff_last));
		armAsm->Ldr(a64::w15, a64::MemOperand(a64::x15));
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, t0_host)));
		armAsm->Ldr(a64::w15, PTR_CPU(cpuRegs.GPR.r[8].UL[0]));
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, t0_cpu)));
		armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_lw_eff_last));
		armAsm->Ldr(a64::w15, a64::MemOperand(a64::x15));
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, eff_addr)));
		armAsm->Ldr(a64::w15, PTR_CPU(cpuRegs.GPR.r[2].UL[0]));
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, v0_cpu)));
		armAsm->Mov(a64::w15, 0u);
		armAsm->Str(a64::w15, a64::MemOperand(a64::x14, offsetof(LfCmpStoreEntry, taken)));

		armAsm->Add(a64::w12, a64::w12, 1);
		armAsm->Str(a64::w12, a64::MemOperand(a64::x11));
	}

#if iPSX2_ENABLE_LW_ONESHOT_CAP
    // [TEMP_DIAG] Store-only capture of LW result at selected PCs.
    // Captures actual loaded value from host register, not cpuRegs.
    if (IsLfLwOneShotEnabled() && (pc == 0xBFC02664 || pc == 0xBFC02668 || pc == 0x9FC41048) && bits == 32) {
        a64::Label skip_lw_capture;

        // Check if already captured.
        armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lw_seen));
        armAsm->Ldr(a64::w14, a64::MemOperand(a64::x15));
        armAsm->Cbnz(a64::w14, &skip_lw_capture);

        // Capture! Set seen=1.
        armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lw_seen));
        armAsm->Mov(a64::w14, 1);
        armAsm->Str(a64::w14, a64::MemOperand(a64::x15));

        // Store guest_pc.
        armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lw_guest_pc));
        armAsm->Mov(a64::w14, pc);
        armAsm->Str(a64::w14, a64::MemOperand(a64::x15));

        // Store addr (ECX).
        armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lw_addr));
        armAsm->Str(ECX, a64::MemOperand(a64::x15));

        // Store val (from destination host register after load).
        armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lw_val));
        armAsm->Str(a64::WRegister(HostGprPhys(x86reg)), a64::MemOperand(a64::x15));

        // Store path = 2 (vtlb path, since this is the non-const path).
        armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lw_path));
        armAsm->Mov(a64::w14, 2);
        armAsm->Str(a64::w14, a64::MemOperand(a64::x15));

        armBind(&skip_lw_capture);
    }
#endif
    
    if (IsLfHotCallProbeEnabled() && DarwinMisc::iPSX2_FORCE_JIT_VERIFY && bits == 16 && sign && (pc & 0xFFFFFF00) == 0xbfc02600) {
        armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
        armAsm->Push(a64::x29, a64::lr);
        
        armAsm->Mov(a64::w0, pc);
        
        // Re-calculate Addr in w1
        if (GPR_IS_CONST1(_Rs_)) {
             u32 addr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
             armAsm->Mov(a64::w1, addr);
        } else {
             armAsm->Mov(a64::w1, a64::WRegister(HostGprPhys(_allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ))));
             if (_Imm_) armAsm->Add(a64::w1, a64::w1, _Imm_);
        }
        
        armAsm->Mov(a64::w2, a64::WRegister(HostGprPhys(x86reg)));
        
        armEmitCall((void*)LogLHCHECK);
        
        armAsm->Pop(a64::lr, a64::x29);
        armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
    }

	// if there was a constant, it should have been invalidated.
	pxAssert(!_Rt_ || !GPR_IS_CONST1(_Rt_));
	if (!_Rt_ && x86reg >= 0)  // [iter402] x86reg=-1 means result in RAX (scratch), nothing to free
		_freeX86reg(x86reg);
}

//////////////////////////////////////////////////////////////////////////////////////////
//

// Task 2: JIT Store Helper declaration
extern "C" void TraceJitStore(u32 pc, u32 addr, int mode);

static void recStore(u32 bits)
{
    // [FIX] Gate TraceJitStore behind runtime flag to avoid hot-path overhead.
    // Was: unconditional call on every 32-bit store. Now: only when iPSX2_TRACE_JIT_STORE=1.
    static int s_trace_jit_store = -1;
    if (s_trace_jit_store < 0) {
        s_trace_jit_store = iPSX2_GetRuntimeEnvBool("iPSX2_TRACE_JIT_STORE", false) ? 1 : 0;
        Console.WriteLn("@@CFG@@ iPSX2_TRACE_JIT_STORE=%d", s_trace_jit_store);
    }
    if (!iPSX2_IsSafeOnlyEnabled() && IsDiagFlagsEnabled() && bits == 32 && s_trace_jit_store == 1)
    {
        // Save caller-saved registers (Alignment 16 bytes required)
        armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
        armAsm->Push(a64::lr, a64::xzr); // Pad with xzr

        // Arg 1: PC
        armAsm->Mov(a64::w0, pc);

        // Arg 2: Address & Arg 3: Mode
        if (GPR_IS_CONST1(_Rs_)) {
             u32 dstadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
             armAsm->Mov(a64::w1, dstadr);
             armAsm->Mov(a64::w2, 0); // FAST/CONST
        } else {
             // Ensure Rs is in a register
             int r = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
             a64::WRegister base(HostGprPhys(r));

             // Compute Address: Base + Imm
             armAsm->Mov(a64::w3, _Imm_);
             armAsm->Add(a64::w1, base, a64::w3);

             armAsm->Mov(a64::w2, 1); // SLOW/DYNAMIC
        }

        armEmitCall((void*)TraceJitStore);

        // Restore registers
        armAsm->Pop(a64::lr, a64::xzr);
        armAsm->Pop(a64::x0, a64::x1, a64::x2, a64::x3);
    }

	int regt;
	bool xmm;
	if (bits < 128)
	{
		regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
		xmm = false;
	}
	else
	{
		regt = _allocGPRtoXMMreg(_Rt_, MODE_READ);
		xmm = true;
	}

	// Load ECX with the destination address, or issue a direct optimized write
	// if the address is a constant propagation.
	const bool bios_stack64_sp_guard = (bits == 64 && _Rs_ == 29 &&
		((pc >= 0x9FC432EC && pc <= 0x9FC43310) || pc == 0x9FC43434 || pc == 0x9FC43474));
	// [R62] Removed PC range check — sd $ra,offset($sp) safe path for ALL addresses.
	const bool bios_ra_stack64_guard = (bits == 64 && _Rt_ == 31 && _Rs_ == 29);
    // [iter214] compile-time trace for SD guard activation
    {
        static u32 s_sd_guard_n = 0;
        if (bios_ra_stack64_guard && s_sd_guard_n < 30) {
            Console.WriteLn("@@SD_RA_GUARD@@ n=%u pc=%08x bits=%d Rt=%d Rs=%d imm=%d",
                s_sd_guard_n, pc, bits, (int)_Rt_, (int)_Rs_, (int)_Imm_);
            s_sd_guard_n++;
        }
    }

	// [iter658] @@RECSTORE_PATH@@ — カーネル memcpy loop (0x80005500-0x80005530) のcompileパス特定
	// Removal condition: 0x991F0 ストアバグfixafter confirmed
	{
		u32 hw_pc_rs = pc & 0x1FFFFFFFu;
		if (hw_pc_rs >= 0x00005500u && hw_pc_rs <= 0x00005530u && bits == 32) {
			static u32 s_rsp_n = 0;
			if (s_rsp_n < 10) {
				bool is_const = GPR_IS_CONST1(_Rs_);
				Console.WriteLn("@@RECSTORE_PATH@@ n=%u pc=%08x Rs=%d Rt=%d imm=%d const=%d regt_slot=%d regt_phys=%d",
					s_rsp_n++, pc, (int)_Rs_, (int)_Rt_, (int)_Imm_, (int)is_const, regt, HostGprPhys(regt));
				if (is_const)
					Console.WriteLn("@@RECSTORE_PATH@@   constRs=%08x dstadr=%08x", g_cpuConstRegs[_Rs_].UL[0], g_cpuConstRegs[_Rs_].UL[0] + _Imm_);
			}
		}
	}

	if (GPR_IS_CONST1(_Rs_) && !bios_ra_stack64_guard)
	{
		u32 dstadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		if (bits == 128)
			dstadr &= ~0x0f;

		// [iter249] @@KERN_SW_CONST@@ カーネルRAM SW const-addr 診断
		// init(2) が [800242E4] に書き込むかverify
		// Removal condition: カーネル初期化 SW JIT バグafter identified
		{
			static u32 s_ksw_n = 0;
			u32 hw_pc = pc & 0x1FFFFFFFu;
			if (s_ksw_n < 30 && bits == 32 && hw_pc >= 0x00012000u && hw_pc < 0x00013000u) {
				Console.WriteLn("@@KERN_SW_CONST@@ n=%u pc=%08x dst=%08x Rs=%d(=%08x) Rt=%d imm=%d",
					s_ksw_n, pc, dstadr, (int)_Rs_, g_cpuConstRegs[_Rs_].UL[0], (int)_Rt_, (int)_Imm_);
				s_ksw_n++;
			}
		}

		if (bios_ra_stack64_guard)
		{
			armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
			armAsm->Push(a64::x4, a64::x5);
			armAsm->Push(a64::x29, a64::lr);
			armAsm->Mov(a64::x17, HostX(regt));
			armAsm->Mov(a64::w0, 1u);
			armAsm->Mov(a64::w1, pc);
			armLoad(a64::w2, PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));
			armAsm->Mov(a64::w3, dstadr);
			armLoad(a64::w4, PTR_CPU(cpuRegs.GPR.n.ra.UL[0]));
			armAsm->Mov(a64::x5, a64::x17);
			armEmitCall((void*)TraceRaStack64Probe);
			armAsm->Pop(a64::lr, a64::x29);
			armAsm->Pop(a64::x4, a64::x5);
			armAsm->Pop(a64::x0, a64::x1, a64::x2, a64::x3);
		}

        // KSEG1 Hack via Const Path
        // [iter24] extended to bits==8 (SB): avoids JIT handler-call path that causes SIGABRT.
        // Removal condition: vtlb_DynGenWrite_Const handlercall生成の根本after fixed。
        if ((bits == 8 || bits == 32) && (dstadr & 0xE0000000) == 0xA0000000)
        {
             armAsm->Push(a64::x0, a64::x1);
             armAsm->Push(a64::lr, a64::xzr);

             armAsm->Mov(a64::w0, dstadr);
             if (bits == 8) {
                 armAsm->And(a64::w1, a64::WRegister(HostGprPhys(regt)), 0xFF);
                 armEmitCall((void*)vtlb_MemWrite8_KSEG1);
             } else {
                 // bits == 32
                 if (xmm) {
                     armAsm->Mov(a64::w1, 0);
                 } else {
                     armAsm->Mov(a64::w1, a64::WRegister(HostGprPhys(regt)));
                 }
                 armEmitCall((void*)vtlb_MemWrite32_KSEG1);
             }

             armAsm->Pop(a64::lr, a64::xzr);
             armAsm->Pop(a64::x0, a64::x1);
        }
        else
        {
            // [iter104] @@GS_STORE_JIT@@ – detect JIT compilation of stores to GS display regs
            if (dstadr >= 0x12000000u && dstadr < 0x12001000u) {
                static u32 s_gs_jit_n = 0;
                if (s_gs_jit_n < 20) {
                    Console.WriteLn("@@GS_STORE_JIT@@ n=%u jit_pc=%08x bits=%d dst=%08x",
                        s_gs_jit_n, pc, bits, dstadr);
                    s_gs_jit_n++;
                }
            }
		    vtlb_DynGenWrite_Const(bits, xmm, dstadr, regt);
        }
	}
	else
	{
		// [iter249] @@KERN_SW_DYN@@ カーネルRAM SW 非定数address診断
		// Removal condition: カーネル初期化 SW JIT バグafter identified
		{
			static u32 s_kswd_n = 0;
			u32 hw_pc = pc & 0x1FFFFFFFu;
			if (s_kswd_n < 30 && bits == 32 && hw_pc >= 0x00012000u && hw_pc < 0x00013000u) {
				Console.WriteLn("@@KERN_SW_DYN@@ n=%u pc=%08x Rs=%d Rt=%d imm=%d",
					s_kswd_n, pc, (int)_Rs_, (int)_Rt_, (int)_Imm_);
				s_kswd_n++;
			}
		}

		if (_Rs_ != 0)
		{
			// TODO(Stenzek): Preload Rs when it's live. Turn into LEA.
//			_eeMoveGPRtoR(arg1regd, _Rs_);
            if (bios_ra_stack64_guard)
            {
                armLoad(ECX, PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));
                if (pc == 0x9FC43434)
                    armAsm->Sub(ECX, ECX, 0x10);
                if (pc == 0x9FC43474)
                    armAsm->Sub(ECX, ECX, 0x100);
            }
            else
                // [iter658] BUG FIX: pass allow_preload=false to prevent _allocX86reg
                // from evicting regt's slot. When all 5 slots are occupied,
                // _eeMoveGPRtoR with allow_preload=true allocates a new slot for Rs,
                // which evicts regt's slot. After eviction, HostW(regt) returns the
                // newly-loaded Rs value instead of Rt's value, causing stores to write
                // the base address instead of the data register.
                // This caused the kernel memcpy at 0x80005518 (SW v1, 0(a1)) to write
                // a1 (address) instead of v1 (data), corrupting the EELOAD copy and
                // preventing BIOS browser from displaying.
                _eeMoveGPRtoR(ECX, _Rs_, false);
			if (_Imm_ != 0) {
//                xADD(arg1regd, _Imm_);
                armAsm->Add(ECX, ECX, _Imm_);
            }
		}
		else
		{
//			xMOV(arg1regd, _Imm_);
            armAsm->Mov(ECX, _Imm_);
		}

		if (bits == 128) {
//            xAND(arg1regd, ~0x0F);
            armAsm->And(ECX, ECX, ~0x0F);
        }

		if (bios_ra_stack64_guard)
		{
			if (pc == 0x9FC43434)
			{
				armMoveAddressToReg(a64::x15, const_cast<u64*>(&g_ra_stack64_val_shadow));
				armAsm->Str(HostX(regt), a64::MemOperand(a64::x15));
			}
			if (pc == 0x9FC43474)
			{
				armMoveAddressToReg(a64::x15, const_cast<u64*>(&g_ra_stack64_val_shadow_caller));
				armAsm->Str(HostX(regt), a64::MemOperand(a64::x15));
			}
			if (pc == 0x9FC43164)
			{
				armMoveAddressToReg(a64::x15, const_cast<u64*>(&g_ra_stack64_val_shadow_431xx));
				armAsm->Str(HostX(regt), a64::MemOperand(a64::x15));
			}
			armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_ra_stack64_addr_shadow));
			armAsm->Str(ECX, a64::MemOperand(a64::x15));
			if (pc == 0x9FC43474)
			{
				armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_ra_stack64_addr_shadow_caller));
				armAsm->Str(ECX, a64::MemOperand(a64::x15));
			}
			armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
			armAsm->Push(a64::x4, a64::x5);
			armAsm->Push(a64::x29, a64::lr);
			armAsm->Mov(a64::x17, HostX(regt));
			armAsm->Mov(a64::w0, 1u);
			armAsm->Mov(a64::w1, pc);
			armLoad(a64::w2, PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));
			armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_ra_stack64_addr_shadow));
			armAsm->Ldr(a64::w3, a64::MemOperand(a64::x15));
			armLoad(a64::w4, PTR_CPU(cpuRegs.GPR.n.ra.UL[0]));
			armAsm->Mov(a64::x5, a64::x17);
			armEmitCall((void*)TraceRaStack64Probe);
			armAsm->Pop(a64::lr, a64::x29);
			armAsm->Pop(a64::x4, a64::x5);
			armAsm->Pop(a64::x0, a64::x1, a64::x2, a64::x3);
		}

		// TODO(Stenzek): Use Rs directly if imm=0. But beware of upper bits.

		// TODO(Stenzek): Use Rs directly if imm=0. But beware of upper bits.
        


		// TODO(Stenzek): Use Rs directly if imm=0. But beware of upper bits.
        
        a64::Label not_kseg1;
        if (bits == 32)
        {
            // [iter662] BUG FIX: _allocX86reg MUST happen BEFORE the KSEG1 branch.
            const int addr_slot = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);

            const a64::WRegister ecx_w(ECX.GetCode());
            armAsm->Mov(HostW(addr_slot), ecx_w);
            armAsm->And(a64::w10, ecx_w, 0xE0000000u);
            armAsm->Cmp(a64::w10, 0xA0000000u);

            armAsm->B(&not_kseg1, a64::Condition::ne);

            // == KSEG1 ==
            armAsm->Push(a64::x0, a64::x1);
            armAsm->Push(a64::lr, a64::xzr);

            armAsm->Mov(a64::w0, ecx_w);
            armAsm->Mov(a64::w1, a64::WRegister(HostGprPhys(regt)));

            armEmitCall((void*)vtlb_MemWrite32_KSEG1);

            armAsm->Pop(a64::lr, a64::xzr);
            armAsm->Pop(a64::x0, a64::x1);

            // Skip normal write
            a64::Label end_store;
            armAsm->B(&end_store);

            armBind(&not_kseg1);
            vtlb_DynGenWrite(bits, xmm, HostGprPhys(addr_slot), regt);
            armBind(&end_store);

            _freeX86reg(addr_slot);
        }
		else
		{
			// [iter214] BUG FIX: use _Safe helper that computes addr from cpuRegs.sp + imm
			// internally, bypassing ECX (w1) which was receiving garbage values.
			if (bits == 64 && (bios_stack64_sp_guard || bios_ra_stack64_guard) && IsBiosRaStack64RwFixEnabled())
			{
				// [iter215] Flush sp to cpuRegs before safe helper
				int sp_slot = _checkX86reg(X86TYPE_GPR, 29, MODE_READ);
				if (sp_slot >= 0)
					armAsm->Str(HostW(sp_slot), PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));

				armAsm->Push(a64::x0, a64::x1);
				armAsm->Push(a64::x29, a64::lr);
				armAsm->Mov(a64::w0, (s32)_Imm_);
				armAsm->Mov(a64::x1, HostX(regt));
				armEmitCall((void*)WriteGuestMem64ForRaStack_Safe);
				armAsm->Pop(a64::lr, a64::x29);
				armAsm->Pop(a64::x0, a64::x1);
			}
			else
				{
					const int addr_slot = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
					armAsm->Mov(HostW(addr_slot), ECX);
					vtlb_DynGenWrite(bits, xmm, HostGprPhys(addr_slot), regt);
					_freeX86reg(addr_slot);
				}
			if (bios_ra_stack64_guard)
			{
				armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
				armAsm->Push(a64::x29, a64::lr);
				armAsm->Mov(a64::w0, pc);
				armLoad(a64::w1, PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));
				armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_ra_stack64_addr_shadow));
				armAsm->Ldr(a64::w2, a64::MemOperand(a64::x15));
				armLoad(a64::w3, PTR_CPU(cpuRegs.GPR.n.ra.UL[0]));
				armEmitCall((void*)TraceRaStack64StorePost);
				armAsm->Pop(a64::lr, a64::x29);
				armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
			}
        }
        
        if (bits == 32)
        {
             // Just close the label if we opened it
             // Wait, I can't leave the label hanging if I didn't emit the branch.
             // I need to emit the full logic.
        }
	}
}


//////////////////////////////////////////////////////////////////////////////////////////
//
void recLB()
{
	recLoad(8, true);
	EE::Profiler.EmitOp(eeOpcode::LB);
}
void recLBU()
{
	recLoad(8, false);
	EE::Profiler.EmitOp(eeOpcode::LBU);
}
void recLH()
{
	recLoad(16, true);
	EE::Profiler.EmitOp(eeOpcode::LH);
}
void recLHU()
{
	recLoad(16, false);
	EE::Profiler.EmitOp(eeOpcode::LHU);
}
void recLW()
{
	recLoad(32, true);
	EE::Profiler.EmitOp(eeOpcode::LW);
}
void recLWU()
{
	recLoad(32, false);
	EE::Profiler.EmitOp(eeOpcode::LWU);
}
void recLD()
{
	recLoad(64, false);
	EE::Profiler.EmitOp(eeOpcode::LD);
}
void recLQ()
{
	recLoadQuad(128, false);
	EE::Profiler.EmitOp(eeOpcode::LQ);
}

void recSB()
{
	recStore(8);
	EE::Profiler.EmitOp(eeOpcode::SB);
}
void recSH()
{
	recStore(16);
	EE::Profiler.EmitOp(eeOpcode::SH);
}
void recSW()
{
	recStore(32);
	EE::Profiler.EmitOp(eeOpcode::SW);
}
void recSD()
{
    // @@REC_SD_PROBE@@ SD命令compileverifyprobe（capnot needed、PCrange絞り）
    // pc はここに来るとき既に SD_PC+4 に増分されている
    // Removal condition: SD $ra の書き込みパスroot cause確定時
    {
        const u32 sd_actual_pc = pc - 4;
        if (sd_actual_pc >= 0x9FC42370u && sd_actual_pc <= 0x9FC423A8u)
        {
            Console.WriteLn("@@REC_SD_PROBE@@ sd_pc=%08x Rs=%d Rt=%d imm=%d const_Rs=%d",
                sd_actual_pc, _Rs_, _Rt_, _Imm_, (int)GPR_IS_CONST1(_Rs_));
        }
    }
	recStore(64);
	EE::Profiler.EmitOp(eeOpcode::SD);
}
void recSQ()
{
	recStore(128);
	EE::Profiler.EmitOp(eeOpcode::SQ);
}

////////////////////////////////////////////////////

void recLWL()
{
#ifdef REC_LOADS
	_freeX86reg(EAX);
	_freeX86reg(ECX);
	_freeX86reg(EDX);
//	_freeX86reg(arg1regd);

	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

	const int temp_slot_lwl = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
	const a64::WRegister temp = HostW(temp_slot_lwl);

//	_eeMoveGPRtoR(arg1regd, _Rs_);
    _eeMoveGPRtoR(ECX, _Rs_);
	if (_Imm_ != 0) {
//        xADD(arg1regd, _Imm_);
        armAsm->Add(ECX, ECX, _Imm_);
    }

	// calleeSavedReg1 = bit offset in word
//	xMOV(temp, arg1regd);
    armAsm->Mov(temp, ECX);
//	xAND(temp, 3);
    armAsm->And(temp, temp, 3);
//	xSHL(temp, 3);
    armAsm->Lsl(temp, temp, 3);

//	xAND(arg1regd, ~3);
    armAsm->And(ECX, ECX, ~3);
	vtlb_DynGenReadNonQuad(32, false, false, ECX.GetCode(), RETURN_READ_IN_RAX);

	if (!_Rt_)
	{
		_freeX86reg(HostW(temp_slot_lwl));
		return;
	}

	// mask off bytes loaded
//	xMOV(ecx, temp);
    armAsm->Mov(ECX, temp);
	_freeX86reg(HostW(temp_slot_lwl));

	const int treg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE);
//	xMOV(edx, 0xffffff);
    armAsm->Mov(EDX, 0xffffff);
//	xSHR(edx, cl);
    armAsm->Lsr(EDX, EDX, ECX);
//	xAND(edx, xRegister32(treg));
    armAsm->And(EDX, EDX, a64::WRegister(HostGprPhys(treg)));

	// OR in bytes loaded
//	xNEG(ecx);
    armAsm->Neg(ECX, ECX);
//	xADD(ecx, 24);
    armAsm->Add(ECX, ECX, 24);
//	xSHL(eax, cl);
    armAsm->Lsl(EAX, EAX, ECX);
//	xOR(eax, edx);
    armAsm->Orr(EAX, EAX, EDX);
//	xMOVSX(xRegister64(treg), eax);
    armAsm->Sxtw(a64::XRegister(HostGprPhys(treg)), EAX);
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);

	recCall(LWL);
#endif

	EE::Profiler.EmitOp(eeOpcode::LWL);
}

////////////////////////////////////////////////////
void recLWR()
{
#ifdef REC_LOADS
	_freeX86reg(EAX);
	_freeX86reg(ECX);
	_freeX86reg(EDX);
//	_freeX86reg(arg1regd);

	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

    const int temp_slot_lwr = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
    const a64::WRegister temp = HostW(temp_slot_lwr);

//	_eeMoveGPRtoR(arg1regd, _Rs_);
    _eeMoveGPRtoR(ECX, _Rs_);
	if (_Imm_ != 0) {
//        xADD(arg1regd, _Imm_);
        armAsm->Add(ECX, ECX, _Imm_);
    }

	// edi = bit offset in word
//	xMOV(temp, arg1regd);
    armAsm->Mov(temp, ECX);

//	xAND(arg1regd, ~3);
    armAsm->And(ECX, ECX, ~3);
	vtlb_DynGenReadNonQuad(32, false, false, ECX.GetCode(), RETURN_READ_IN_RAX);

	if (!_Rt_)
	{
		_freeX86reg(HostW(temp_slot_lwr));
		return;
	}

	const int treg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE);
    auto reg32 = a64::WRegister(HostGprPhys(treg));

//	xAND(temp, 3);
    armAsm->And(temp, temp, 3);

//	xForwardJE8 nomask;
    a64::Label nomask;
    armAsm->Cbz(temp, &nomask);
//	xSHL(temp, 3);
    armAsm->Lsl(temp, temp, 3);
	// mask off bytes loaded
//	xMOV(ecx, 24);
    armAsm->Mov(ECX, 24);
//	xSUB(ecx, temp);
    armAsm->Sub(ECX, ECX, temp);
//	xMOV(edx, 0xffffff00);
    armAsm->Mov(EDX, 0xffffff00);
//	xSHL(edx, cl);
    armAsm->Lsl(EDX, EDX, ECX);
    // [BUG-E002] Save upper 32 bits of xReg BEFORE any W-register write
    // ARM64: writing to WRegister zeros upper 32 of XRegister
    auto xReg = a64::XRegister(HostGprPhys(treg));
    armAsm->Lsr(REX, xReg, 32);  // REX = original upper 32 bits (in lower half)

//	xAND(xRegister32(treg), edx);
    armAsm->And(reg32, reg32, EDX);  // upper 32 zeroed by ARM64

	// OR in bytes loaded
//	xMOV(ecx, temp);
    armAsm->Mov(ECX, temp);
//	xSHR(eax, cl);
    armAsm->Lsr(EAX, EAX, ECX);
//	xOR(xRegister32(treg), eax);
    armAsm->Orr(reg32, reg32, EAX);  // lower 32 = correct result

    // [BUG-E002] Restore saved upper 32 bits
    armAsm->Bfi(xReg, REX, 32, 32);  // insert saved upper 32 at bits[63:32]

//	xForwardJump8 end;
    a64::Label end;
    armAsm->B(&end);
//	nomask.SetTarget();
    armBind(&nomask);
	// NOTE: This might look wrong, but it's correct - see interpreter.
//	xMOVSX(xRegister64(treg), eax);
    armAsm->Sxtw(a64::XRegister(HostGprPhys(treg)), EAX);
//	end.SetTarget();
    armBind(&end);
	_freeX86reg(HostW(temp_slot_lwr));
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);

	recCall(LWR);
#endif

	EE::Profiler.EmitOp(eeOpcode::LWR);
}

////////////////////////////////////////////////////

void recSWL()
{
#ifdef REC_STORES
	// avoid flushing and immediately reading back
	_addNeededX86reg(X86TYPE_GPR, _Rs_);

	// preload Rt, since we can't do so inside the branch
	if (!GPR_IS_CONST1(_Rt_))
		_allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
	else
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

    const int temp_slot_swl = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
    const a64::WRegister temp = HostW(temp_slot_swl);
	_freeX86reg(EAX);
	_freeX86reg(ECX);
    _freeX86reg(EDX);
//	_freeX86reg(arg1regd);
//	_freeX86reg(arg2regd);

//	_eeMoveGPRtoR(arg1regd, _Rs_);
    _eeMoveGPRtoR(ECX, _Rs_);
	if (_Imm_ != 0) {
//        xADD(arg1regd, _Imm_);
        armAsm->Add(ECX, ECX, _Imm_);
    }

	// edi = bit offset in word
//	xMOV(temp, arg1regd);
    armAsm->Mov(temp, ECX);
//	xAND(arg1regd, ~3);
    armAsm->And(ECX, ECX, ~3);
//	xAND(temp, 3);
    armAsm->And(temp, temp, 3);
//	xCMP(temp, 3);
    armAsm->Cmp(temp, 3);

	// If we're not using fastmem, we need to flush early. Because the first read
	// (which would flush) happens inside a branch.
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
		iFlushCall(FLUSH_FULLVTLB);

//	xForwardJE8 skip;
    a64::Label skip;
    armAsm->B(&skip, a64::Condition::eq);
//	xSHL(temp, 3);
    armAsm->Lsl(temp, temp, 3);

	vtlb_DynGenReadNonQuad(32, false, false, ECX.GetCode(), RETURN_READ_IN_RAX);

	// mask read -> arg2
//	xMOV(ecx, temp);
    armAsm->Mov(ECX, temp);
//	xMOV(arg2regd, 0xffffff00);
    armAsm->Mov(EDX, 0xffffff00);
//	xSHL(arg2regd, cl);
    armAsm->Lsl(EDX, EDX, ECX);
//	xAND(arg2regd, eax);
    armAsm->And(EDX, EDX, EAX);

	if (_Rt_)
	{
		// mask write and OR -> edx
//		xNEG(ecx);
        armAsm->Neg(ECX, ECX);
//		xADD(ecx, 24);
        armAsm->Add(ECX, ECX, 24);
//		_eeMoveGPRtoR(eax, _Rt_, false);
        _eeMoveGPRtoR(EAX, _Rt_, false);
//		xSHR(eax, cl);
        armAsm->Lsr(EAX, EAX, ECX);
//		xOR(arg2regd, eax);
        armAsm->Orr(EDX, EDX, EAX);
	}

//	_eeMoveGPRtoR(arg1regd, _Rs_, false);
    _eeMoveGPRtoR(ECX, _Rs_, false);
	if (_Imm_ != 0) {
//        xADD(arg1regd, _Imm_);
        armAsm->Add(ECX, ECX, _Imm_);
    }
//	xAND(arg1regd, ~3);
    armAsm->And(ECX, ECX, ~3);

//	xForwardJump8 end;
    a64::Label end;
    armAsm->B(&end);
//	skip.SetTarget();
    armBind(&skip);
//	_eeMoveGPRtoR(arg2regd, _Rt_, false);
    _eeMoveGPRtoR(EDX, _Rt_, false);
//	end.SetTarget();
    armBind(&end);

	// [iter333] EDX (physical w2) is not a slot register; vtlb_DynGenWrite uses HostX(value_reg).
    // Copy EDX into temp_slot_swl (now unused) before the call, then pass the slot index.
    armAsm->Mov(HostW(temp_slot_swl), EDX);
//	vtlb_DynGenWrite(32, false, arg1regd.GetId(), arg2regd.GetId());
    vtlb_DynGenWrite(32, false, ECX.GetCode(), temp_slot_swl);
	_freeX86reg(HostW(temp_slot_swl));
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(SWL);
#endif

	EE::Profiler.EmitOp(eeOpcode::SWL);
}

////////////////////////////////////////////////////
void recSWR()
{
#ifdef REC_STORES
	// avoid flushing and immediately reading back
	_addNeededX86reg(X86TYPE_GPR, _Rs_);

	// preload Rt, since we can't do so inside the branch
	if (!GPR_IS_CONST1(_Rt_))
		_allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
	else
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

    const int temp_slot_swr = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
    const a64::WRegister temp = HostW(temp_slot_swr);
	_freeX86reg(ECX);
    _freeX86reg(EDX);
//	_freeX86reg(arg1regd);
//	_freeX86reg(arg2regd);

//	_eeMoveGPRtoR(arg1regd, _Rs_);
    _eeMoveGPRtoR(ECX, _Rs_);
	if (_Imm_ != 0) {
//        xADD(arg1regd, _Imm_);
        armAsm->Add(ECX, ECX, _Imm_);
    }

	// edi = bit offset in word
//	xMOV(temp, arg1regd);
    armAsm->Mov(temp, ECX);
//	xAND(arg1regd, ~3);
    armAsm->Ands(ECX, ECX, ~3);
//	xAND(temp, 3);
    armAsm->Ands(temp, temp, 3);

	// If we're not using fastmem, we need to flush early. Because the first read
	// (which would flush) happens inside a branch.
	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
		iFlushCall(FLUSH_FULLVTLB);

//	xForwardJE8 skip;
    a64::Label skip;
    armAsm->B(&skip, a64::Condition::eq);
//	xSHL(temp, 3);
    armAsm->Lsl(temp, temp, 3);

	vtlb_DynGenReadNonQuad(32, false, false, ECX.GetCode(), RETURN_READ_IN_RAX);

	// mask read -> edx
//	xMOV(ecx, 24);
    armAsm->Mov(ECX, 24);
//	xSUB(ecx, temp);
    armAsm->Sub(ECX, ECX, temp);
//	xMOV(arg2regd, 0xffffff);
    armAsm->Mov(EDX, 0xffffff);
//	xSHR(arg2regd, cl);
    armAsm->Lsr(EDX, EDX, ECX);
//	xAND(arg2regd, eax);
    armAsm->And(EDX, EDX, EAX);

	if (_Rt_)
	{
		// mask write and OR -> edx
//		xMOV(ecx, temp);
        armAsm->Mov(ECX, temp);
//		_eeMoveGPRtoR(eax, _Rt_, false);
        _eeMoveGPRtoR(EAX, _Rt_, false);
//		xSHL(eax, cl);
        armAsm->Lsl(EAX, EAX, ECX);
//		xOR(arg2regd, eax);
        armAsm->Orr(EDX, EDX, EAX);
	}

//	_eeMoveGPRtoR(arg1regd, _Rs_, false);
    _eeMoveGPRtoR(ECX, _Rs_, false);
	if (_Imm_ != 0) {
//        xADD(arg1regd, _Imm_);
        armAsm->Add(ECX, ECX, _Imm_);
    }
//	xAND(arg1regd, ~3);
    armAsm->And(ECX, ECX, ~3);

//	xForwardJump8 end;
    a64::Label end;
    armAsm->B(&end);
//	skip.SetTarget();
    armBind(&skip);
//	_eeMoveGPRtoR(arg2regd, _Rt_, false);
    _eeMoveGPRtoR(EDX, _Rt_, false);
//	end.SetTarget();
    armBind(&end);

	// [iter333] EDX (physical w2) is not a slot register; vtlb_DynGenWrite uses HostX(value_reg).
    // Copy EDX into temp_slot_swr (now unused) before the call, then pass the slot index.
    armAsm->Mov(HostW(temp_slot_swr), EDX);
//	vtlb_DynGenWrite(32, false, arg1regd.GetId(), arg2regd.GetId());
    vtlb_DynGenWrite(32, false, ECX.GetCode(), temp_slot_swr);
    _freeX86reg(HostW(temp_slot_swr));
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(SWR);
#endif

	EE::Profiler.EmitOp(eeOpcode::SWR);
}

////////////////////////////////////////////////////

namespace
{
    enum class SHIFTV
    {
        xSHL,
        xSHR,
        xSAR
    };
} // namespace

/// Masks rt with (0xffffffffffffffff maskshift maskamt), merges with (value shift amt), leaves result in value
//static void ldlrhelper_const(int maskamt, const xImpl_Group2& maskshift, int amt, const xImpl_Group2& shift, const xRegister64& value, const xRegister64& rt)
static void ldlrhelper_const(int maskamt, const SHIFTV maskshift, int amt, const SHIFTV shift, const a64::XRegister& value, const a64::XRegister& rt)
{
	pxAssert(rt.GetCode() != ECX.GetCode() && value.GetCode() != ECX.GetCode());

	// Would xor rcx, rcx; not rcx be better here?
//	xMOV(rcx, -1);
    armAsm->Mov(RCX, -1);

//	maskshift(rcx, maskamt);
    switch (maskshift)
    {
        case SHIFTV::xSHR:
            armAsm->Lsr(RCX, RCX, maskamt);
            break;
        case SHIFTV::xSAR:
            armAsm->Asr(RCX, RCX, maskamt);
            break;
        case SHIFTV::xSHL:
            armAsm->Lsl(RCX, RCX, maskamt);
            break;
    }

//	xAND(rt, rcx);
    armAsm->And(rt, rt, RCX);

//	shift(value, amt);
    switch (shift)
    {
        case SHIFTV::xSHR:
            armAsm->Lsr(value, value, amt);
            break;
        case SHIFTV::xSAR:
            armAsm->Asr(value, value, amt);
            break;
        case SHIFTV::xSHL:
            armAsm->Lsl(value, value, amt);
            break;
    }

//	xOR(rt, value);
    armAsm->Orr(rt, rt, value);
}

/// Masks rt with (0xffffffffffffffff maskshift maskamt), merges with (value shift amt), leaves result in value
//static void ldlrhelper(const xRegister32& maskamt, const xImpl_Group2& maskshift, const xRegister32& amt, const xImpl_Group2& shift, const xRegister64& value, const xRegister64& rt)
static void ldlrhelper(const a64::Register& maskamt, const SHIFTV maskshift, const a64::Register& amt, const SHIFTV shift, const a64::Register& value, const a64::Register& rt)
{
	pxAssert(rt.GetCode() != ECX.GetCode() && amt.GetCode() != ECX.GetCode() && value.GetCode() != ECX.GetCode());

	// Would xor rcx, rcx; not rcx be better here?
    const a64::XRegister maskamt64(maskamt.GetCode());
//	xMOV(ecx, maskamt);
    armAsm->Mov(ECX, maskamt);
//	xMOV(maskamt64, -1);
    armAsm->Mov(maskamt64, -1);
//	maskshift(maskamt64, cl);
    switch (maskshift)
    {
        case SHIFTV::xSHR:
            armAsm->Lsr(maskamt64, maskamt64, RCX);
            break;
        case SHIFTV::xSAR:
            armAsm->Asr(maskamt64, maskamt64, RCX);
            break;
        case SHIFTV::xSHL:
            armAsm->Lsl(maskamt64, maskamt64, RCX);
            break;
    }

//	xAND(rt, maskamt64);
    armAsm->And(rt, rt, maskamt64);

//	xMOV(ecx, amt);
    armAsm->Mov(ECX, amt);

//	shift(value, cl);
    switch (shift)
    {
        case SHIFTV::xSHR:
            armAsm->Lsr(value, value, RCX);
            break;
        case SHIFTV::xSAR:
            armAsm->Asr(value, value, RCX);
            break;
        case SHIFTV::xSHL:
            armAsm->Lsl(value, value, RCX);
            break;
    }

//	xOR(rt, value);
    armAsm->Orr(rt, rt, value);
}

void recLDL()
{
	if (!_Rt_)
		return;

#ifdef REC_LOADS
	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

    const int temp1_slot = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
    const a64::WRegister temp1 = HostW(temp1_slot);
	_freeX86reg(EAX);
	_freeX86reg(ECX);
	_freeX86reg(EDX);
//	_freeX86reg(arg1regd);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 srcadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;

		// If _Rs_ is equal to _Rt_ we need to put the shift in to eax since it won't take the CONST path.
		if (_Rs_ == _Rt_) {
//            xMOV(temp1, srcadr);
            armAsm->Mov(temp1, srcadr);
        }

		srcadr &= ~0x07;

		vtlb_DynGenReadNonQuad_Const(64, false, false, srcadr, RETURN_READ_IN_RAX);
	}
	else
	{
		// Load ECX with the source memory address that we're reading from.
//		_freeX86reg(arg1regd);
        _freeX86reg(ECX);
//		_eeMoveGPRtoR(arg1regd, _Rs_);
        _eeMoveGPRtoR(ECX, _Rs_);
		if (_Imm_ != 0) {
//            xADD(arg1regd, _Imm_);
            armAsm->Add(ECX, ECX, _Imm_);
        }

//		xMOV(temp1, arg1regd);
        armAsm->Mov(temp1, ECX);
//		xAND(arg1regd, ~0x07);
        armAsm->And(ECX, ECX, ~0x07);

		vtlb_DynGenReadNonQuad(64, false, false, ECX.GetCode(), RETURN_READ_IN_RAX);
	}

    const int treg_slot = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE);
    const a64::XRegister treg = HostX(treg_slot);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 shift = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		shift = ((shift & 0x7) + 1) * 8;
		if (shift != 64)
		{
//			ldlrhelper_const(shift, xSHR, 64 - shift, xSHL, rax, treg);
            ldlrhelper_const(shift, SHIFTV::xSHR, 64 - shift, SHIFTV::xSHL, a64::XRegister(RAX), treg);
		}
		else
		{
//			xMOV(treg, rax);
            armAsm->Mov(treg, RAX);
		}
	}
	else
	{
//		xAND(temp1, 0x7);
        armAsm->And(temp1, temp1, 0x7);
//		xCMP(temp1, 7);
        armAsm->Cmp(temp1, 7);
//		xCMOVE(treg, rax); // swap register with memory when not shifting
        armAsm->Csel(treg, RAX, treg, a64::Condition::eq);
//		xForwardJE8 skip;
        a64::Label skip;
        armAsm->B(&skip, a64::Condition::eq);
		// Calculate the shift from top bit to lowest.
//		xADD(temp1, 1);
        armAsm->Add(temp1, temp1, 1);
//		xMOV(edx, 64);
        armAsm->Mov(EDX, 64);
//		xSHL(temp1, 3);
        armAsm->Lsl(temp1, temp1, 3);
//		xSUB(edx, temp1);
        armAsm->Sub(EDX, EDX, temp1);

//		ldlrhelper(temp1, xSHR, edx, xSHL, rax, treg);
        ldlrhelper(temp1, SHIFTV::xSHR, EDX, SHIFTV::xSHL, RAX, treg);
//		skip.SetTarget();
        armBind(&skip);
	}

	_freeX86reg(HostW(temp1_slot));
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(LDL);
#endif

	EE::Profiler.EmitOp(eeOpcode::LDL);
}

////////////////////////////////////////////////////
void recLDR()
{
	if (!_Rt_)
		return;

#ifdef REC_LOADS
	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);
	if (_Rs_)
		_addNeededX86reg(X86TYPE_GPR, _Rs_);

    const int temp1r_slot = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
    const a64::WRegister temp1 = HostW(temp1r_slot);
	_freeX86reg(EAX);
	_freeX86reg(ECX);
	_freeX86reg(EDX);
//	_freeX86reg(arg1regd);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 srcadr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;

		// If _Rs_ is equal to _Rt_ we need to put the shift in to eax since it won't take the CONST path.
		if (_Rs_ == _Rt_) {
//            xMOV(temp1, srcadr);
            armAsm->Mov(temp1, srcadr);
        }

		srcadr &= ~0x07;

		vtlb_DynGenReadNonQuad_Const(64, false, false, srcadr, RETURN_READ_IN_RAX);
	}
	else
	{
		// Load ECX with the source memory address that we're reading from.
//		_freeX86reg(arg1regd);
        _freeX86reg(ECX);
//		_eeMoveGPRtoR(arg1regd, _Rs_);
        _eeMoveGPRtoR(ECX, _Rs_);
		if (_Imm_ != 0) {
//            xADD(arg1regd, _Imm_);
            armAsm->Add(ECX, ECX, _Imm_);
        }

//		xMOV(temp1, arg1regd);
        armAsm->Mov(temp1, ECX);
//		xAND(arg1regd, ~0x07);
        armAsm->And(ECX, ECX, ~0x07);

		vtlb_DynGenReadNonQuad(64, false, false, ECX.GetCode(), RETURN_READ_IN_RAX);
	}

    const int tregr_slot = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ | MODE_WRITE);
    const a64::XRegister treg = HostX(tregr_slot);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 shift = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		shift = (shift & 0x7) * 8;
		if (shift != 0)
		{
//			ldlrhelper_const(64 - shift, xSHL, shift, xSHR, rax, treg);
            ldlrhelper_const(64 - shift, SHIFTV::xSHL, shift, SHIFTV::xSHR, a64::XRegister(RAX), treg);
		}
		else
		{
//			xMOV(treg, rax);
            armAsm->Mov(treg, RAX);
		}
	}
	else
	{
//		xAND(temp1, 0x7);
        armAsm->Ands(temp1, temp1, 0x7);
//		xCMOVE(treg, rax); // swap register with memory when not shifting
        armAsm->Csel(treg, RAX, treg, a64::Condition::eq);
//		xForwardJE8 skip;
        a64::Label skip;
        armAsm->B(&skip, a64::Condition::eq);
		// Calculate the shift from top bit to lowest.
//		xMOV(edx, 64);
        armAsm->Mov(EDX, 64);
//		xSHL(temp1, 3);
        armAsm->Lsl(temp1, temp1, 3);
//		xSUB(edx, temp1);
        armAsm->Sub(EDX, EDX, temp1);

//		ldlrhelper(edx, xSHL, temp1, xSHR, rax, treg);
        ldlrhelper(EDX, SHIFTV::xSHL, temp1, SHIFTV::xSHR, RAX, treg);
//		skip.SetTarget();
        armBind(&skip);
	}

	_freeX86reg(HostW(temp1r_slot));
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(LDR);
#endif

	EE::Profiler.EmitOp(eeOpcode::LDR);
}

////////////////////////////////////////////////////

/// Masks value with (0xffffffffffffffff maskshift maskamt), merges with (rt shift amt), saves to dummyValue
//static void sdlrhelper_const(int maskamt, const xImpl_Group2& maskshift, int amt, const xImpl_Group2& shift, const xRegister64& value, const xRegister64& rt)
static void sdlrhelper_const(int maskamt, const SHIFTV maskshift, int amt, const SHIFTV shift, const a64::XRegister& value, const a64::XRegister& rt)
{
	pxAssert(rt.GetCode() != ECX.GetCode() && value.GetCode() != ECX.GetCode());
//	xMOV(rcx, -1);
    armAsm->Mov(RCX, -1);

//	maskshift(rcx, maskamt);
    switch (maskshift)
    {
        case SHIFTV::xSHR:
            armAsm->Lsr(RCX, RCX, maskamt);
            break;
        case SHIFTV::xSAR:
            armAsm->Asr(RCX, RCX, maskamt);
            break;
        case SHIFTV::xSHL:
            armAsm->Lsl(RCX, RCX, maskamt);
            break;
    }

//	xAND(rcx, value);
    armAsm->And(RCX, RCX, value);

//	shift(rt, amt);
    switch (shift)
    {
        case SHIFTV::xSHR:
            armAsm->Lsr(rt, rt, amt);
            break;
        case SHIFTV::xSAR:
            armAsm->Asr(rt, rt, amt);
            break;
        case SHIFTV::xSHL:
            armAsm->Lsl(rt, rt, amt);
            break;
    }

//	xOR(rt, rcx);
    armAsm->Orr(rt, rt, RCX);
}

/// Masks value with (0xffffffffffffffff maskshift maskamt), merges with (rt shift amt), saves to dummyValue
//static void sdlrhelper(const xRegister32& maskamt, const xImpl_Group2& maskshift, const xRegister32& amt, const xImpl_Group2& shift, const xRegister64& value, const xRegister64& rt)
static void sdlrhelper(const a64::Register& maskamt, const SHIFTV maskshift, const a64::Register& amt, const SHIFTV shift, const a64::Register& value, const a64::Register& rt)
{
	pxAssert(rt.GetCode() != ECX.GetCode() && amt.GetCode() != ECX.GetCode() && value.GetCode() != ECX.GetCode());

	// Generate mask 128-(shiftx8)
    const a64::XRegister maskamt64(maskamt.GetCode());
//	xMOV(ecx, maskamt);
    armAsm->Mov(ECX, maskamt);
//	xMOV(maskamt64, -1);
    armAsm->Mov(maskamt64, -1);

//	maskshift(maskamt64, cl);
    switch (maskshift)
    {
        case SHIFTV::xSHR:
            armAsm->Lsr(maskamt64, maskamt64, RCX);
            break;
        case SHIFTV::xSAR:
            armAsm->Asr(maskamt64, maskamt64, RCX);
            break;
        case SHIFTV::xSHL:
            armAsm->Lsl(maskamt64, maskamt64, RCX);
            break;
    }

//	xAND(maskamt64, value);
    armAsm->And(maskamt64, maskamt64, value);

	// Shift over reg value
//	xMOV(ecx, amt);
    armAsm->Mov(ECX, amt);

//	shift(rt, cl);
    switch (shift)
    {
        case SHIFTV::xSHR:
            armAsm->Lsr(rt, rt, RCX);
            break;
        case SHIFTV::xSAR:
            armAsm->Asr(rt, rt, RCX);
            break;
        case SHIFTV::xSHL:
            armAsm->Lsl(rt, rt, RCX);
            break;
    }

//	xOR(rt, maskamt64);
    armAsm->Orr(rt, rt, maskamt64);
}

void recSDL()
{
#ifdef REC_STORES
	// [FIX R111-7] Flush Rs from allocator cache before SDL to prevent stale base address.
	_deleteEEreg(_Rs_, 1);
	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

	_freeX86reg(ECX);
    _freeX86reg(EDX);
//	_freeX86reg(arg2regd);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 adr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		u32 aligned = adr & ~0x07;
		u32 shift = ((adr & 0x7) + 1) * 8;
		if (shift == 64)
		{
//			_eeMoveGPRtoR(arg2reg, _Rt_);
            _eeMoveGPRtoR(RDX, _Rt_);
		}
		else
		{
			vtlb_DynGenReadNonQuad_Const(64, false, false, aligned, RETURN_READ_IN_RAX);
//			_eeMoveGPRtoR(arg2reg, _Rt_);
            _eeMoveGPRtoR(RDX, _Rt_);
//			sdlrhelper_const(shift, xSHL, 64 - shift, xSHR, rax, arg2reg);
            sdlrhelper_const(shift, SHIFTV::xSHL, 64 - shift, SHIFTV::xSHR, a64::XRegister(RAX), a64::XRegister(RDX));
		}
		vtlb_DynGenWrite_Const(64, false, aligned, EDX.GetCode());
	}
	else
	{
		if (_Rs_)
			_addNeededX86reg(X86TYPE_GPR, _Rs_);

		// Load ECX with the source memory address that we're reading from.
//		_freeX86reg(arg1regd);
        _freeX86reg(ECX);
//		_eeMoveGPRtoR(arg1regd, _Rs_);
        _eeMoveGPRtoR(ECX, _Rs_);
		if (_Imm_ != 0) {
//            xADD(arg1regd, _Imm_);
            armAsm->Add(ECX, ECX, _Imm_);
        }

		_freeX86reg(ECX);
		_freeX86reg(EDX);
//		_freeX86reg(arg2regd);

        const int temp1_slot_sdl = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
        const a64::WRegister temp1 = HostW(temp1_slot_sdl);
        const int temp2_slot_sdl = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
        const a64::XRegister temp2 = HostX(temp2_slot_sdl);
//		_eeMoveGPRtoR(arg2reg, _Rt_);
        _eeMoveGPRtoR(RDX, _Rt_);

//		xMOV(temp1, arg1regd);
        armAsm->Mov(temp1, ECX);
//		xMOV(temp2, arg2reg);
        armAsm->Mov(temp2, RDX);
//		xAND(arg1regd, ~0x07);
        armAsm->And(ECX, ECX, ~0x07);
//		xAND(temp1, 0x7);
        armAsm->And(temp1, temp1, 0x7);
//		xCMP(temp1, 7);
        armAsm->Cmp(temp1, 7);

		// If we're not using fastmem, we need to flush early. Because the first read
		// (which would flush) happens inside a branch.
		if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
			iFlushCall(FLUSH_FULLVTLB);

//		xForwardJE8 skip;
        a64::Label skip;
        armAsm->B(&skip, a64::Condition::eq);
//		xADD(temp1, 1);
        armAsm->Add(temp1, temp1, 1);
		vtlb_DynGenReadNonQuad(64, false, false, ECX.GetCode(), RETURN_READ_IN_RAX);

		//Calculate the shift from top bit to lowest
//		xMOV(edx, 64);
        armAsm->Mov(EDX, 64);
//		xSHL(temp1, 3);
        armAsm->Lsl(temp1, temp1, 3);
//		xSUB(edx, temp1);
        armAsm->Sub(EDX, EDX, temp1);

//		sdlrhelper(temp1, xSHL, edx, xSHR, rax, temp2);
        sdlrhelper(temp1, SHIFTV::xSHL, EDX, SHIFTV::xSHR, RAX, temp2);

//		_eeMoveGPRtoR(arg1regd, _Rs_, false);
        _eeMoveGPRtoR(ECX, _Rs_, false);
		if (_Imm_ != 0) {
//            xADD(arg1regd, _Imm_);
            armAsm->Add(ECX, ECX, _Imm_);
        }
//		xAND(arg1regd, ~0x7);
        armAsm->And(ECX, ECX, ~0x7);
//		skip.SetTarget();
        armBind(&skip);

//		vtlb_DynGenWrite(64, false, arg1regd.GetId(), temp2.GetId());
        // [iter332] temp2.GetCode()=21 (physical reg) → slot index を渡す（vtlb_DynGenWrite は value_reg を HostX(slot) で変換する）
        vtlb_DynGenWrite(64, false, ECX.GetCode(), temp2_slot_sdl);
			_freeX86reg(HostW(temp2_slot_sdl));
			_freeX86reg(HostW(temp1_slot_sdl));
	}
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(SDL);
#endif
	EE::Profiler.EmitOp(eeOpcode::SDL);
}

////////////////////////////////////////////////////
void recSDR()
{
#ifdef REC_STORES
	// [FIX R111-7] Flush Rs from allocator cache before SDR to prevent stale base address.
	_deleteEEreg(_Rs_, 1);
	// avoid flushing and immediately reading back
	if (_Rt_)
		_addNeededX86reg(X86TYPE_GPR, _Rt_);

	_freeX86reg(ECX);
    _freeX86reg(EDX);
//	_freeX86reg(arg2regd);

	if (GPR_IS_CONST1(_Rs_))
	{
		u32 adr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		u32 aligned = adr & ~0x07;
		u32 shift = (adr & 0x7) * 8;
		if (shift == 0)
		{
//			_eeMoveGPRtoR(arg2reg, _Rt_);
            _eeMoveGPRtoR(RDX, _Rt_);
		}
		else
		{
			vtlb_DynGenReadNonQuad_Const(64, false, false, aligned, RETURN_READ_IN_RAX);
//			_eeMoveGPRtoR(arg2reg, _Rt_);
            _eeMoveGPRtoR(RDX, _Rt_);
//			sdlrhelper_const(64 - shift, xSHR, shift, xSHL, rax, arg2reg);
            sdlrhelper_const(64 - shift, SHIFTV::xSHR, shift, SHIFTV::xSHL, a64::XRegister(RAX), a64::XRegister(RDX));
		}

		vtlb_DynGenWrite_Const(64, false, aligned, RDX.GetCode());
	}
	else
	{
		if (_Rs_)
			_addNeededX86reg(X86TYPE_GPR, _Rs_);

		// Load ECX with the source memory address that we're reading from.
//		_eeMoveGPRtoR(arg1regd, _Rs_);
        _eeMoveGPRtoR(ECX, _Rs_);
		if (_Imm_ != 0) {
//            xADD(arg1regd, _Imm_);
            armAsm->Add(ECX, ECX, _Imm_);
        }

		_freeX86reg(ECX);
		_freeX86reg(EDX);
//		_freeX86reg(arg2regd);

        const int temp1_slot_sdr = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
        const a64::WRegister temp1 = HostW(temp1_slot_sdr);
        const int temp2_slot_sdr = _allocX86reg(X86TYPE_TEMP, 0, MODE_CALLEESAVED);
        const a64::XRegister temp2 = HostX(temp2_slot_sdr);
//		_eeMoveGPRtoR(arg2reg, _Rt_);
        _eeMoveGPRtoR(RDX, _Rt_);

//		xMOV(temp1, arg1regd);
        armAsm->Mov(temp1, ECX);
//		xMOV(temp2, arg2reg);
        armAsm->Mov(temp2, RDX);
//		xAND(arg1regd, ~0x07);
        armAsm->Ands(ECX, ECX, ~0x07);
//		xAND(temp1, 0x7);
        armAsm->Ands(temp1, temp1, 0x7);

		// If we're not using fastmem, we need to flush early. Because the first read
		// (which would flush) happens inside a branch.
		if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
			iFlushCall(FLUSH_FULLVTLB);

//		xForwardJE8 skip;
        a64::Label skip;
        armAsm->B(&skip, a64::Condition::eq);
		vtlb_DynGenReadNonQuad(64, false, false, ECX.GetCode(), RETURN_READ_IN_RAX);

//		xMOV(edx, 64);
        armAsm->Mov(EDX, 64);
//		xSHL(temp1, 3);
        armAsm->Lsl(temp1, temp1, 3);
//		xSUB(edx, temp1);
        armAsm->Sub(EDX, EDX, temp1);

//		sdlrhelper(edx, xSHR, temp1, xSHL, rax, temp2);
        sdlrhelper(EDX, SHIFTV::xSHR, temp1, SHIFTV::xSHL, RAX, temp2);

//		_eeMoveGPRtoR(arg1regd, _Rs_, false);
        _eeMoveGPRtoR(ECX, _Rs_, false);
		if (_Imm_ != 0) {
//            xADD(arg1regd, _Imm_);
            armAsm->Add(ECX, ECX, _Imm_);
        }
//		xAND(arg1regd, ~0x7);
        armAsm->And(ECX, ECX, ~0x7);
//		skip.SetTarget();
        armBind(&skip);

//		vtlb_DynGenWrite(64, false, arg1regd.GetId(), temp2.GetId());
        // [iter332] temp2.GetCode()=physical → slot index を渡す
        vtlb_DynGenWrite(64, false, ECX.GetCode(), temp2_slot_sdr);
			_freeX86reg(HostW(temp2_slot_sdr));
			_freeX86reg(HostW(temp1_slot_sdr));
	}
#else
	iFlushCall(FLUSH_INTERPRETER);
	_deleteEEreg(_Rs_, 1);
	_deleteEEreg(_Rt_, 1);
	recCall(SDR);
#endif
	EE::Profiler.EmitOp(eeOpcode::SDR);
}

//////////////////////////////////////////////////////////////////////////////////////////
/*********************************************************
* Load and store for COP1                                *
* Format:  OP rt, offset(base)                           *
*********************************************************/

////////////////////////////////////////////////////

void recLWC1()
{
#ifndef FPU_RECOMPILE
	recCall(::R5900::Interpreter::OpcodeImpl::LWC1);
#else

	const vtlb_ReadRegAllocCallback alloc_cb = []() { return _allocFPtoXMMreg(_Rt_, MODE_WRITE); };
	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 addr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		vtlb_DynGenReadNonQuad_Const(32, false, true, addr, alloc_cb);
	}
	else
	{
//		_freeX86reg(arg1regd);
        _freeX86reg(ECX);
//		_eeMoveGPRtoR(arg1regd, _Rs_);
        _eeMoveGPRtoR(ECX, _Rs_);
		if (_Imm_ != 0) {
//            xADD(arg1regd, _Imm_);
            armAsm->Add(ECX, ECX, _Imm_);
        }

		vtlb_DynGenReadNonQuad(32, false, true, ECX.GetCode(), alloc_cb);
	}

	EE::Profiler.EmitOp(eeOpcode::LWC1);
#endif
}

//////////////////////////////////////////////////////

void recSWC1()
{
#ifndef FPU_RECOMPILE
	recCall(::R5900::Interpreter::OpcodeImpl::SWC1);
#else
	const int regt = _allocFPtoXMMreg(_Rt_, MODE_READ);
	if (GPR_IS_CONST1(_Rs_))
	{
		const u32 addr = g_cpuConstRegs[_Rs_].UL[0] + _Imm_;
		vtlb_DynGenWrite_Const(32, true, addr, regt);
	}
	else
	{
//		_freeX86reg(arg1regd);
        _freeX86reg(ECX);
//		_eeMoveGPRtoR(arg1regd, _Rs_);
        _eeMoveGPRtoR(ECX, _Rs_);
		if (_Imm_ != 0) {
//            xADD(arg1regd, _Imm_);
            armAsm->Add(ECX, ECX, _Imm_);
        }

		vtlb_DynGenWrite(32, true, ECX.GetCode(), regt);
	}

	EE::Profiler.EmitOp(eeOpcode::SWC1);
#endif
}

#endif

} // namespace R5900::Dynarec::OpcodeImpl
