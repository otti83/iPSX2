// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "x86/iR5900.h"
#include "common/Darwin/DarwinMisc.h"

// [iter220] TEMP_DIAG: BNE 9FC43404 runtime v0 probe
extern "C" void armsx2_probe_bne_9fc43404(u64 v0_val, u32 cycle);

#if !defined(__ANDROID__)
using namespace x86Emitter;
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
alignas(64) volatile LfCmpStoreEntry g_lf_cmp_store_ring[LF_CMP_STORE_RING_SIZE] = {};
alignas(64) volatile u32 g_lf_cmp_store_idx = 0;

static bool IsDiagFlagsEnabled();

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

static bool IsDiagFlagsEnabled()
{
	static int s_enabled = -1;
	if (s_enabled < 0)
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_ENABLE_DIAG_FLAGS", false) ? 1 : 0;
	return (s_enabled == 1);
}

static bool IsFix9JitNopEnabled()
{
	if (iPSX2_IsSafeOnlyEnabled() || !IsDiagFlagsEnabled())
		return false;
	static int s_enabled = -1;
	if (s_enabled < 0)
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_FIX9_JIT_NOP", false) ? 1 : 0;
	return (s_enabled == 1);
}

static bool IsBeq023d8NoConstEnabled()
{
	if (iPSX2_IsSafeOnlyEnabled() || !IsDiagFlagsEnabled())
		return false;
	static int s_enabled = -1;
	if (s_enabled < 0)
		s_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_BEQ023D8_NO_CONST", false) ? 1 : 0;
	return (s_enabled == 1);
}

// [TEMP_DIAG][REMOVE_AFTER=EE_9FC410XX_BRANCH_ROOTCAUSE_V1]
// Runtime-gated branch compare probe for the EE BIOS wait loop (9FC4105C / 9FC4109C).
// Purpose: capture host-side compare operands vs cpuRegs values without enabling broad JIT probes.
static bool IsEeLoopBranchProbeEnabled()
{
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		const char* env = iPSX2_GetRuntimeEnv("iPSX2_EE_LOOP_BRANCH_PROBE");
		s_enabled = (env && env[0] == '1') ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_EE_LOOP_BRANCH_PROBE=%d", s_enabled);
	}
	return (s_enabled == 1);
}

// [TEMP_DIAG][REMOVE_AFTER=EE_9FC410XX_CONSTFOLD_CHECK_V1]
// Runtime-gated compile-time const-fold bypass for the EE BIOS wait-loop BEQ/BNE pair.
static bool IsEeLoopNoConstBranchEnabled()
{
	static int s_enabled = -1;
	if (s_enabled < 0)
	{
		const char* env = iPSX2_GetRuntimeEnv("iPSX2_EE_LOOP_NO_CONST_BRANCH");
		s_enabled = (env && env[0] == '1') ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_EE_LOOP_NO_CONST_BRANCH=%d", s_enabled);
	}
	return (s_enabled == 1);
}

extern "C" void LogEeLoopBranchCmp(u32 branch_pc, u32 used_rs, u32 used_rt, u32 meta)
{
	static u32 s_count = 0;
	if (s_count >= 64)
		return;
	++s_count;

	const u32 rs = meta & 0xffu;
	const u32 rt = (meta >> 8) & 0xffu;
	const u32 bne = (meta >> 16) & 0x1u;
	const u32 process = (meta >> 17) & 0x3u;

	Console.WriteLn("@@EE_LOOP_BR@@ n=%u pc=%08x bne=%u process=%u rs=%u rt=%u used_rs=%08x used_rt=%08x cpu_rs=%08x cpu_rt=%08x code=%08x ee_pc=%08x",
		s_count, branch_pc, bne, process, rs, rt, used_rs, used_rt,
		static_cast<u32>(cpuRegs.GPR.r[rs].UL[0]), static_cast<u32>(cpuRegs.GPR.r[rt].UL[0]),
		cpuRegs.code, cpuRegs.pc);
}

extern "C" void iPSX2_DumpLfCmpStoreRing()
{
	static bool s_dumped = false;
	if (s_dumped)
		return;
	s_dumped = true;

	const u32 idx = g_lf_cmp_store_idx;
	const u32 count = (idx < LF_CMP_STORE_RING_SIZE) ? idx : LF_CMP_STORE_RING_SIZE;
	Console.WriteLn("@@LF_CMP_STORE_DUMP@@ idx=%u count=%u", idx, count);
	u32 printed = 0;
	u32 low_value_266c = 0;
	for (u32 pass = 0; pass < 2; pass++)
	{
		for (u32 i = 0; i < count; i++)
		{
			const u32 slot = (idx - count + i) & LF_CMP_STORE_RING_MASK;
			const u32 branch_pc = g_lf_cmp_store_ring[slot].branch_pc;
			if (pass == 0 && branch_pc == 0xBFC0266C)
				continue;
			if (pass == 1)
			{
				if (branch_pc != 0xBFC0266C || low_value_266c >= 8)
					continue;
				low_value_266c++;
			}
			const u32 sample_kind = g_lf_cmp_store_ring[slot].sample_kind;
			const u32 lw_pc = g_lf_cmp_store_ring[slot].lw_pc;
			const u32 lhs_cpu = g_lf_cmp_store_ring[slot].lhs_cpu;
			const u32 lhs_swap = g_lf_cmp_store_ring[slot].lhs_swap;
			const u32 rhs_cpu = g_lf_cmp_store_ring[slot].rhs_cpu;
			const u32 rhs_expected = g_lf_cmp_store_ring[slot].rhs_expected;
			const u32 lw_raw = g_lf_cmp_store_ring[slot].lw_raw;
			const u32 lw_swapped = g_lf_cmp_store_ring[slot].lw_swapped;
			const u32 t0_host = g_lf_cmp_store_ring[slot].t0_host;
			const u32 t0_cpu = g_lf_cmp_store_ring[slot].t0_cpu;
			const u32 eff_addr = g_lf_cmp_store_ring[slot].eff_addr;
			const u32 v0_cpu = g_lf_cmp_store_ring[slot].v0_cpu;
			const u32 taken = g_lf_cmp_store_ring[slot].taken;
			Console.WriteLn("@@LF_CMP_STORE@@ n=%u kind=%u branch_pc=%08x lw_pc=%08x t0_host=%08x t0_cpu=%08x eff=%08x lw_raw=%08x lw_swap=%08x v0_cpu=%08x lhs=%08x lhs_swap=%08x rhs=%08x rhs_used=%08x rhs_expected=%08x taken=%u",
				printed, sample_kind, branch_pc, lw_pc, t0_host, t0_cpu, eff_addr, lw_raw, lw_swapped, v0_cpu, lhs_cpu, lhs_swap, rhs_cpu, rhs_cpu, rhs_expected, taken);
			printed++;
		}
	}
}

// [TEMP_DIAG][REMOVE_AFTER=BASELINE_CLEAN_V1] Branch runtime probe gate
// Set to 1 to enable Push/Pop+armEmitCall probes in branch emission
// Default OFF to eliminate instruction corruption and crash risks
#ifndef iPSX2_ENABLE_BRANCH_RUNTIME_PROBES
#define iPSX2_ENABLE_BRANCH_RUNTIME_PROBES 0
#endif


#if iPSX2_ENABLE_BRANCH_RUNTIME_PROBES
extern "C" void LogBEQ_V0_Runtime(u32 pc, u64 v0_val, int src_type, int branch_taken) {
    if (pc == 0xbfc023d8) { 
        static bool done = false; 
        if (done) return;
        done = true;
        fprintf(stderr, "@@BEQ_V0_RUNTIME@@ pc=%x src=%s v0=%llx branch_taken=%d\n", 
            pc, (src_type == 1) ? "HOST" : "MEM", v0_val, branch_taken);
    }
}

extern "C" void LogBNECHECK(u32 pc, u64 rs_val, u64 rt_val, int is_bne) {
    static bool done = false;
    if (done) return;
    if (pc == 0xbfc0268c && is_bne) {
        done = true;
        int taken = (rs_val != rt_val);
        u32 target = 0xbfc0268c + 4 + (0x21 * 4);
        fprintf(stderr, "@@BNE_PROOF@@ pc=%08x v0=%016llx taken=%d target=%08x\n", pc, rs_val, taken, target);
    }
}
#endif // iPSX2_ENABLE_BRANCH_RUNTIME_PROBES

extern "C" void LogRomdirCmpOperands(u32 pc, u32 rs_used, u32 rt_used, u32 rs_cpu, u32 rt_cpu)
{
	static u32 s_count = 0;
	if (s_count >= 50)
		return;
	++s_count;
	Console.WriteLn("@@ROMDIR_CMP@@ #%u pc=%08x used_rs=%08x used_rt=%08x cpu_rs=%08x cpu_rt=%08x",
		s_count, pc, rs_used, rt_used, rs_cpu, rt_cpu);
}

extern "C" void LogLfCmpOperandsByPc(u32 pc, u32 used_rs, u32 used_rt, u32 rs_cpu, u32 rt_cpu, u32 t0_cpu)
{
	static u32 s_2674_count = 0;
	static u32 s_2684_count = 0;
	u32* count = nullptr;
	if (pc == 0xBFC02674)
		count = &s_2674_count;
	else if (pc == 0xBFC02684)
		count = &s_2684_count;
	else
		return;

	if (*count >= 16)
		return;
	++(*count);
	Console.WriteLn("@@LF_CMP_PC@@ pc=%08x n=%u used_rs=%08x used_rt=%08x cpu_rs=%08x cpu_rt=%08x t0=%08x",
		pc, *count, used_rs, used_rt, rs_cpu, rt_cpu, t0_cpu);
}

extern "C" u32 ReadGuestMem32ForLfCmp(u32 addr)
{
	static int s_probe_lf_memread = -1;
	static u32 s_probe_count = 0;
	if (s_probe_lf_memread < 0)
	{
		const char* env = iPSX2_GetRuntimeEnv("iPSX2_PROBE_LF_MEMREAD");
		s_probe_lf_memread = (env && env[0] == '1') ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_PROBE_LF_MEMREAD=%d", s_probe_lf_memread);
	}

	const u32 value = memRead32(addr);
	if (s_probe_lf_memread == 1 && s_probe_count < 32)
	{
		++s_probe_count;
		Console.WriteLn("@@LF_MEMREAD32@@ n=%u addr=%08x value=%08x", s_probe_count, addr, value);
	}

	return value;
}

extern "C" u32 ReadGuestMem16ForLfCmp(u32 addr)
{
	return static_cast<u32>(memRead16(addr));
}

extern "C" void LogBeq023d8Cmp(u32 pc, u32 used_rs, u32 used_rt, u32 cpu_rs, u32 cpu_rt, u32 taken)
{
	static bool s_done = false;
	if (s_done)
		return;
	s_done = true;
	Console.WriteLn("@@BEQ023D8_CMP@@ pc=%08x used_rs=%08x used_rt=%08x cpu_rs=%08x cpu_rt=%08x taken=%u",
		pc, used_rs, used_rt, cpu_rs, cpu_rt, taken);
}

extern "C" void LogBeqToPanicCmp(u32 pc, u32 target, u32 used_rs, u32 used_rt, u32 cpu_rs, u32 cpu_rt, u32 taken)
{
	static bool s_done = false;
	if (s_done)
		return;
	s_done = true;
	Console.WriteLn("@@BEQ_TO_PANIC_CMP@@ pc=%08x target=%08x used_rs=%08x used_rt=%08x cpu_rs=%08x cpu_rt=%08x taken=%u",
		pc, target, used_rs, used_rt, cpu_rs, cpu_rt, taken);
}

#if iPSX2_ENABLE_BRANCH_RUNTIME_PROBES
// [TEMP_DIAG][REMOVE_AFTER=LF_BNE_RING_V1] LoadFile BNE Store-Only Ring Buffer
struct LfBneRingEntry {
    u32 branch_pc;
    u32 rs_val;
    u32 rt_val;
    u32 taken;
};
static constexpr int LF_BNE_RING_SIZE = 32;
alignas(64) LfBneRingEntry g_lf_bne_ring[LF_BNE_RING_SIZE];
alignas(64) u32 g_lf_bne_ring_idx = 0;

void LfBneRingDump() {
    static bool s_dumped = false;
    if (s_dumped) return;
    s_dumped = true;
    Console.WriteLn("@@LF_BNE_RING_DUMP@@ idx=%u (ring_size=%d)", g_lf_bne_ring_idx, LF_BNE_RING_SIZE);
    int count = (g_lf_bne_ring_idx < LF_BNE_RING_SIZE) ? g_lf_bne_ring_idx : LF_BNE_RING_SIZE;
    for (int i = 0; i < count; i++) {
        int slot = (g_lf_bne_ring_idx - count + i) & (LF_BNE_RING_SIZE - 1);
        const auto& e = g_lf_bne_ring[slot];
        Console.WriteLn("@@LF_BNE_RING@@ [%02d] pc=%08x rs=%08x rt=%08x taken=%d", 
            i, e.branch_pc, e.rs_val, e.rt_val, e.taken);
    }
}
#endif // iPSX2_ENABLE_BRANCH_RUNTIME_PROBES

// [TEMP_DIAG] Forward declarations for LF_CMP_USED globals (defined in iR5900.cpp)
extern volatile u32 g_lf_cmp_seen;
extern volatile u32 g_lf_cmp_pc;
extern volatile u32 g_lf_cmp_t0;
extern volatile u32 g_lf_cmp_rs;
extern volatile u32 g_lf_cmp_rt;
extern volatile u32 g_lf_cmp_flags;

// [TEMP_DIAG][REMOVE_AFTER=LOADFILE_CMP_PACK_V1] LoadFile BNE compare pack
// Capture actual compare operands from emitter sources (not cpuRegs)
#ifndef iPSX2_ENABLE_LOADFILE_CMP_PACK
#define iPSX2_ENABLE_LOADFILE_CMP_PACK 0  // default OFF: avoid probe-side host register clobber
#endif

#if iPSX2_ENABLE_LOADFILE_CMP_PACK
struct LfCmpEntry {
    u32 tag;         // 1 = RESE_BNE (Rs=v0, Rt=t5), 2 = T_BNE (Rs=v0, Rt=t6)
    u32 branch_pc;   // actual branch PC (not delay slot)
    u32 t0_val;      // scan pointer (GPR 8)
    u32 rs_used;     // actual value used for compare (from emitter source)
    u32 rt_used;     // actual value used for compare (from emitter source)
    u32 taken;       // branch taken (0/1)
};
constexpr int LF_CMP_RING_SIZE = 32;
alignas(64) volatile LfCmpEntry g_lf_cmp_ring[LF_CMP_RING_SIZE] = {};
alignas(64) volatile u32 g_lf_cmp_ring_idx = 0;
alignas(64) volatile u32 g_lf_cmp_freeze = 0;  // stop after 8 entries
#endif

#ifndef iPSX2_ENABLE_LF_CMP_USED_PROBE
#define iPSX2_ENABLE_LF_CMP_USED_PROBE 0
#endif

// [TEMP_DIAG][REMOVE_AFTER=LF_PROBE_PACK_V3] LoadFile multi-stage probe pack
// Captures RESE, TCHK, PCHK, SKIP stages to identify where LoadFile scan fails
#ifndef iPSX2_ENABLE_LF_PROBE_PACK_V3
#define iPSX2_ENABLE_LF_PROBE_PACK_V3 0  // DISABLED for baseline
#endif

#if iPSX2_ENABLE_LF_PROBE_PACK_V3
struct LfProbeV3Entry {
    u32 tag;       // 'RESE', 'TCHK', 'PCHK', 'SKIP' as u32 codes
    u32 guest_pc;  // branch PC or instrumented PC
    u32 t0_val;    // scan pointer (GPR8)
    u32 rs_val;    // compare operand Rs
    u32 rt_val;    // compare operand Rt
    u32 taken;     // branch taken (0/1) from NZCV
    u32 t2_val;    // accumulated offset (GPR10) when relevant
};
constexpr int LF_V3_RING_SIZE = 8;
alignas(64) volatile LfProbeV3Entry g_lf_v3_ring[LF_V3_RING_SIZE] = {};
alignas(64) volatile u32 g_lf_v3_idx = 0;
alignas(64) volatile u32 g_lf_v3_armed = 0;   // set when RESE matches at correct t0
alignas(64) volatile u32 g_lf_v3_done = 0;    // set after PCHK captured, stops further capture
// Tag codes
constexpr u32 TAG_RESE = 0x45534552; // "RESE"
constexpr u32 TAG_TCHK = 0x4B484354; // "TCHK"
constexpr u32 TAG_PCHK = 0x4B434850; // "PCHK"
constexpr u32 TAG_SKIP = 0x50494B53; // "SKIP"
#endif

// Runtime gate (default OFF) for LoadFile-specific delay-slot swap disable.
// Keep OFF by default to preserve normal branch semantics.
#ifndef iPSX2_DISABLE_LF_DELAY_SWAP
#define iPSX2_DISABLE_LF_DELAY_SWAP 0
#endif

namespace R5900::Dynarec::OpcodeImpl
{
/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, rt, offset                             *
*********************************************************/
#ifndef BRANCH_RECOMPILE

namespace Interp = R5900::Interpreter::OpcodeImpl;

REC_SYS(BEQ);
REC_SYS(BEQL);
REC_SYS(BNE);
REC_SYS(BNEL);
REC_SYS(BLTZ);
REC_SYS(BGTZ);
REC_SYS(BLEZ);
REC_SYS(BGEZ);
REC_SYS(BGTZL);
REC_SYS(BLTZL);
REC_SYS_DEL(BLTZAL, 31);
REC_SYS_DEL(BLTZALL, 31);
REC_SYS(BLEZL);
REC_SYS(BGEZL);
REC_SYS_DEL(BGEZAL, 31);
REC_SYS_DEL(BGEZALL, 31);

#else

static void recSetBranchEQ(int bne, int process, a64::Label *pj32Ptr)
{
	// [iter661] Unconditional entry probe (note: branch_pc after delay-slot swap
	// is delay_slot_addr, not BEQ_addr. BEQ 0x800065a4 → delay_slot 0x800065a8)
	{
		const u32 bp = pc - 4;
		if (bp == 0x800065A8u || bp == 0x800065A4u) {
			Console.WriteLn("@@BRANCHEQ_ENTRY@@ bpc=%08x bne=%d process=%d rs=%d rt=%d",
				bp, bne, process, _Rs_, _Rt_);
		}
	}
	// TODO(Stenzek): This is suboptimal if the registers are in XMMs.
	// If the constant register is already in a host register, we don't need the immediate...
	const u32 branch_pc = pc - 4;
	const bool ee_loop_probe = ((branch_pc == 0x9FC4105Cu) || (branch_pc == 0x9FC4109Cu)) && IsEeLoopBranchProbeEnabled();
	const u32 ee_loop_probe_meta = (static_cast<u32>(_Rs_) & 0xffu) |
		((static_cast<u32>(_Rt_) & 0xffu) << 8) |
		((static_cast<u32>(bne) & 0x1u) << 16) |
		((static_cast<u32>(process) & 0x3u) << 17);

	if (process & PROCESS_CONSTS)
	{
		// [iter661] Use _allocX86reg instead of _checkX86reg to ensure the runtime
		// register is properly loaded from memory after delay slot compilation.
		// _checkX86reg could return a stale slot if TrySwapDelaySlot clobbered
		// the host register. _allocX86reg guarantees a valid load.
		_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH_AND_FREE);
		const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
		_eeFlushAllDirty();
		if (ee_loop_probe)
		{
			armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
			armAsm->Push(a64::lr, a64::xzr);
			armAsm->Mov(a64::w0, branch_pc);
			armAsm->Mov(a64::w1, static_cast<u32>(g_cpuConstRegs[_Rs_].UL[0]));
			armAsm->Mov(a64::w2, a64::WRegister(HostGprPhys(regt)));
			armAsm->Mov(a64::w3, ee_loop_probe_meta);
			armEmitCall((void*)LogEeLoopBranchCmp);
			armAsm->Pop(a64::lr, a64::xzr);
			armAsm->Pop(a64::x0, a64::x1, a64::x2, a64::x3);
		}
		armAsm->Cmp(a64::XRegister(HostGprPhys(regt)), g_cpuConstRegs[_Rs_].UD[0]);
	}
	else if (process & PROCESS_CONSTT)
	{
		// [iter661] Same fix as CONSTS: _allocX86reg ensures valid register load
		_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH_AND_FREE);
		// [iter663] @@PRE_ALLOC_RS@@ probe: dump register state BEFORE allocating Rs
		if (branch_pc == 0x800065A8u || branch_pc == 0x800065A4u) {
			static int s_pa_n = 0;
			if (s_pa_n < 5) {
				int a0_slot = -1;
				for (int s = 0; s < (int)iREGCNT_GPR; s++)
					if (x86regs[s].inuse && x86regs[s].type == X86TYPE_GPR && x86regs[s].reg == 4) a0_slot = s;
				u32 inuse_mask = 0;
				for (u32 r = 0; r < iREGCNT_GPR; r++)
					if (x86regs[r].inuse) inuse_mask |= (1u << r);
				Console.WriteLn("@@PRE_ALLOC_RS@@ n=%d bpc=%08x a0_slot=%d inuse=%x s0=%d/%d/%x s1=%d/%d/%x s2=%d/%d/%x s3=%d/%d/%x s4=%d/%d/%x",
					s_pa_n++, branch_pc, a0_slot, inuse_mask,
					x86regs[0].inuse ? x86regs[0].reg : -1, x86regs[0].type, x86regs[0].mode,
					x86regs[1].inuse ? x86regs[1].reg : -1, x86regs[1].type, x86regs[1].mode,
					x86regs[2].inuse ? x86regs[2].reg : -1, x86regs[2].type, x86regs[2].mode,
					x86regs[3].inuse ? x86regs[3].reg : -1, x86regs[3].type, x86regs[3].mode,
					x86regs[4].inuse ? x86regs[4].reg : -1, x86regs[4].type, x86regs[4].mode);
			}
		}
		const int regs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
		_eeFlushAllDirty();
		if (ee_loop_probe)
		{
			armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
			armAsm->Push(a64::lr, a64::xzr);
			armAsm->Mov(a64::w0, branch_pc);
			if (regs >= 0)
				armAsm->Mov(a64::w1, a64::WRegister(HostGprPhys(regs)));
			else
				armAsm->Ldr(a64::w1, PTR_CPU(cpuRegs.GPR.r[_Rs_].UL[0]));
			armAsm->Mov(a64::w2, static_cast<u32>(g_cpuConstRegs[_Rt_].UL[0]));
			armAsm->Mov(a64::w3, ee_loop_probe_meta);
			armEmitCall((void*)LogEeLoopBranchCmp);
			armAsm->Pop(a64::lr, a64::xzr);
			armAsm->Pop(a64::x0, a64::x1, a64::x2, a64::x3);
		}
		// [iter220] TEMP_DIAG: runtime v0 value probe for BNE at 9FC43404
		if (branch_pc == 0x9FC43404u)
		{
			Console.WriteLn("@@BNE_COMPILE_9FC43404@@ regs=%d process=%d rs=%d rt=%d",
				regs, process, _Rs_, _Rt_);
			armAsm->Push(a64::x0, a64::x1);
			armAsm->Push(a64::x29, a64::lr);
			armAsm->Mov(a64::x0, a64::XRegister(HostGprPhys(regs)));
			armLoad(a64::w1, PTR_CPU(cpuRegs.cycle));
			armEmitCall((void*)armsx2_probe_bne_9fc43404);
			armAsm->Pop(a64::x29, a64::lr);  // [iter220] match Push order
			armAsm->Pop(a64::x0, a64::x1);  // [iter220] match Push order
		}
		// [iter661] regs is always >= 0 now (allocated by _allocX86reg)
		armAsm->Cmp(a64::XRegister(HostGprPhys(regs)), g_cpuConstRegs[_Rt_].UD[0]);
	}
	else
	{
		// force S into register, since we need to load it, may as well cache.
		_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH_AND_FREE);
		const int regs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
		const int regt = _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
		_eeFlushAllDirty();
		if (ee_loop_probe)
		{
			armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
			armAsm->Push(a64::lr, a64::xzr);
			armAsm->Mov(a64::w0, branch_pc);
			armAsm->Mov(a64::w1, a64::WRegister(HostGprPhys(regs)));
			if (regt >= 0)
				armAsm->Mov(a64::w2, a64::WRegister(HostGprPhys(regt)));
			else
				armAsm->Ldr(a64::w2, PTR_CPU(cpuRegs.GPR.r[_Rt_].UL[0]));
			armAsm->Mov(a64::w3, ee_loop_probe_meta);
			armEmitCall((void*)LogEeLoopBranchCmp);
			armAsm->Pop(a64::lr, a64::xzr);
			armAsm->Pop(a64::x0, a64::x1, a64::x2, a64::x3);
		}

		if (regt >= 0)
			armAsm->Cmp(a64::XRegister(HostGprPhys(regs)), a64::XRegister(HostGprPhys(regt)));
		else
			armAsm->Cmp(a64::XRegister(HostGprPhys(regs)), armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rt_])));
	}

	if (bne)
		armAsm->B(pj32Ptr, a64::Condition::eq);
	else
		armAsm->B(pj32Ptr, a64::Condition::ne);
}

static void recSetBranchL(int ltz, a64::Label *pj32Ptr)
{
	const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	const int regsxmm = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regsxmm >= 0)
	{
//		xMOVMSKPS(eax, xRegisterSSE(regsxmm));
        armMOVMSKPS(EAX, a64::QRegister(regsxmm));
//		xTEST(al, 2);
        armAsm->Tst(EAX, 2);

		if (ltz) {
//            j32Ptr[0] = JZ32(0);
            armAsm->B(pj32Ptr, a64::Condition::eq);
        }
		else {
//            j32Ptr[0] = JNZ32(0);
            armAsm->B(pj32Ptr, a64::Condition::ne);
        }

		return;
	}

	if (regs >= 0) {
//        xCMP(xRegister64(regs), 0);
        armAsm->Cmp(a64::XRegister(HostGprPhys(regs)), 0);
    }
	else {
//        xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], 0);
        armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])), 0);
    }

	if (ltz) {
//        j32Ptr[0] = JGE32(0);
        armAsm->B(pj32Ptr, a64::Condition::ge);
    }
	else {
//        j32Ptr[0] = JL32(0);
        armAsm->B(pj32Ptr, a64::Condition::lt);
    }
}

//// BEQ
static void recBEQ_const()
{
	u32 branchTo;

	// [iter661] @@BEQ_CONST_ENTRY@@ diagnostic
	{
		const u32 bpc = pc - 4;
		if (bpc == 0x800065A4u || bpc == 0x80006490u || bpc == 0x80006574u) {
			static int s_const_n = 0;
			if (s_const_n < 10) {
				Console.WriteLn("@@BEQ_CONST_ENTRY@@ n=%d bpc=%08x rs=%d rt=%d "
					"g_const_rs=%016llx g_const_rt=%016llx taken=%d",
					s_const_n++, bpc, _Rs_, _Rt_,
					(unsigned long long)g_cpuConstRegs[_Rs_].UD[0],
					(unsigned long long)g_cpuConstRegs[_Rt_].UD[0],
					(g_cpuConstRegs[_Rs_].SD[0] == g_cpuConstRegs[_Rt_].SD[0]) ? 1 : 0);
			}
		}
	}

	if (g_cpuConstRegs[_Rs_].SD[0] == g_cpuConstRegs[_Rt_].SD[0])
		branchTo = ((s32)_Imm_ * 4) + pc;
	else
		branchTo = pc + 4;

	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);
}

static void recBEQ_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	// [iter661] @@BEQ_PROCESS_ENTRY@@ diagnostic
	{
		const u32 bpc = pc - 4;
		if (bpc == 0x800065A4u || bpc == 0x80006490u || bpc == 0x80006574u) {
			static int s_entry_n = 0;
			if (s_entry_n < 10) {
				Console.WriteLn("@@BEQ_PROCESS_ENTRY@@ n=%d bpc=%08x process=%d rs=%d rt=%d rs_eq_rt=%d",
					s_entry_n++, bpc, process, _Rs_, _Rt_, _Rs_ == _Rt_);
			}
		}
	}

	if (_Rs_ == _Rt_)
	{
		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
	}
	else
	{
		// [iter662] Dump delay slot opcode for SIF BEQ debug
		{
			const u32 bpc = pc - 4;
			if (bpc == 0x800065A4u || bpc == 0x80006490u || bpc == 0x80006574u) {
				const u32 ds_opcode = *(u32*)PSM(pc);
				const u32 ds_rs = (ds_opcode >> 21) & 0x1F;
				const u32 ds_rt = (ds_opcode >> 16) & 0x1F;
				const u32 ds_rd = (ds_opcode >> 11) & 0x1F;
				static int s_ds_n = 0;
				if (s_ds_n < 10) {
					Console.WriteLn("@@DELAY_SLOT_DUMP@@ n=%d bpc=%08x ds_pc=%08x ds_opcode=%08x ds_rs=%d ds_rt=%d ds_rd=%d",
						s_ds_n++, bpc, pc, ds_opcode, ds_rs, ds_rt, ds_rd);
				}
			}
		}
		const bool swap = TrySwapDelaySlot(_Rs_, _Rt_, 0, true);


        a64::Label j32Ptr;
		recSetBranchEQ(0, process, &j32Ptr);

		if (!swap)
		{
			SaveBranchState();
			recompileNextInstruction(true, false);
		}

		SetBranchImm(branchTo);

//		x86SetJ32(j32Ptr[0]);
        armBind(&j32Ptr);

		if (!swap)
		{
			// recopy the next inst
			pc -= 4;
			LoadBranchState();
			recompileNextInstruction(true, false);
		}

		SetBranchImm(pc);
	}
}

namespace Interp = R5900::Interpreter::OpcodeImpl;

void recBEQ()
{
	if ((pc - 4) == 0x9FC4105Cu)
	{
		static u32 s_ee4105c_log_count = 0;
		if (s_ee4105c_log_count < 4)
		{
			++s_ee4105c_log_count;
			Console.WriteLn("@@EE_LOOP_PATH@@ pc=%08x op=BEQ path=%s constRs=%d constRt=%d rs=%d rt=%d code=%08x",
				pc - 4, GPR_IS_CONST2(_Rs_, _Rt_) ? "CONST" : "PROCESS",
				GPR_IS_CONST1(_Rs_) ? 1 : 0, GPR_IS_CONST1(_Rt_) ? 1 : 0, _Rs_, _Rt_, cpuRegs.code);
		}
		if (IsEeLoopNoConstBranchEnabled())
		{
			recBEQ_process(0);
			return;
		}
	}

	if (DarwinMisc::iPSX2_FORCE_JIT_VERIFY && (pc - 4) == 0xBFC023D8)
	{
		static bool s_log_beq_023d8_path = false;
		if (!s_log_beq_023d8_path)
		{
			s_log_beq_023d8_path = true;
			Console.WriteLn("@@BEQ023D8_PATH@@ branch_pc=%08x path=%s rs=%d rt=%d const_rs=%d const_rt=%d",
				pc - 4, GPR_IS_CONST2(_Rs_, _Rt_) ? "CONST" : "PROCESS", _Rs_, _Rt_,
				GPR_IS_CONST1(_Rs_) ? 1 : 0, GPR_IS_CONST1(_Rt_) ? 1 : 0);
		}
	}


	// prefer using the host register over an immediate, it'll be smaller code.
	    if (pc == 0xbfc02094) {
	        static bool s_log_path = false;
	        if (!s_log_path) {
	            s_log_path = true;
	            Console.WriteLn("@@BR_PATH@@ pc=%08x op=1000fffa path=%s file=" __FILE__, 
	                pc-4, GPR_IS_CONST2(_Rs_, _Rt_) ? "CONST" : "PROCESS");
	        }
	    }

	// Avoid compile-time folding for LoadFile return check; force runtime compare.
	if ((pc - 4) == 0xBFC023D8)
	{
		if (IsBeq023d8NoConstEnabled())
		{
			static bool s_beq_023d8_no_const_logged = false;
			if (!s_beq_023d8_no_const_logged)
			{
				s_beq_023d8_no_const_logged = true;
				Console.WriteLn("@@JIT_FIX@@ BEQ023D8_NO_CONST=1");
			}
			recBEQ_process(0);
			return;
		}
	}

	// [iter661] Path selection diagnostic for kernel SIF BEQs
	{
		const u32 bpc = pc - 4;
		if (bpc == 0x800065A4u || bpc == 0x80006490u || bpc == 0x80006574u) {
			static int s_path_n = 0;
			if (s_path_n < 10) {
				const char* path = GPR_IS_CONST2(_Rs_, _Rt_) ? "CONST" :
					(GPR_IS_CONST1(_Rs_) ? "rs_CONST" :
					(GPR_IS_CONST1(_Rt_) ? "rt_CONST" : "NEITHER"));
				Console.WriteLn("@@BEQ_PATH_SEL@@ n=%d bpc=%08x rs=%d rt=%d "
					"const_rs=%d const_rt=%d path=%s "
					"g_const_rs=%016llx g_const_rt=%016llx",
					s_path_n++, bpc, _Rs_, _Rt_,
					GPR_IS_CONST1(_Rs_) ? 1 : 0,
					GPR_IS_CONST1(_Rt_) ? 1 : 0,
					path,
					(unsigned long long)g_cpuConstRegs[_Rs_].UD[0],
					(unsigned long long)g_cpuConstRegs[_Rt_].UD[0]);
			}
		}
	}

	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBEQ_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBEQ_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBEQ_process(PROCESS_CONSTT);
	else
		recBEQ_process(0);
}

//// BNE
static void recBNE_const()
{
	// [FIX #9 JIT] bfc026a4: BNE v0, t2, +108 -> NOP at JIT compile time (const path).
	if (IsFix9JitNopEnabled() && cpuRegs.code == 0x144a001bu) {
		static bool s_fix9_const_logged = false;
		if (!s_fix9_const_logged) {
			s_fix9_const_logged = true;
			Console.WriteLn("@@FIX9_JIT_NOP@@ pc=%08x code=%08x -> forced NOP (recBNE_const)",
				cpuRegs.pc, cpuRegs.code);
		}
		recompileNextInstruction(true, false);
		SetBranchImm(pc); // fall through
		return;
	}

	u32 branchTo;

	if (g_cpuConstRegs[_Rs_].SD[0] != g_cpuConstRegs[_Rt_].SD[0])
		branchTo = ((s32)_Imm_ * 4) + pc;
	else
		branchTo = pc + 4;

	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);
}

static void recBNE_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (_Rs_ == _Rt_)
	{
		recompileNextInstruction(true, false);
		SetBranchImm(pc);
		return;
	}

	bool swap = TrySwapDelaySlot(_Rs_, _Rt_, 0, true);
    
	{
		static int s_disable_lf_delay_swap = -1;
		if (s_disable_lf_delay_swap < 0)
		{
			const char* env = iPSX2_GetRuntimeEnv("iPSX2_DISABLE_LF_DELAY_SWAP");
			s_disable_lf_delay_swap = (iPSX2_IsSafeOnlyEnabled() || !IsDiagFlagsEnabled()) ? 0 : ((env && env[0] == '1') ? 1 : 0);
			Console.WriteLn("@@CFG@@ iPSX2_DISABLE_LF_DELAY_SWAP=%d", s_disable_lf_delay_swap);
		}
		if (s_disable_lf_delay_swap == 1 && pc >= 0xBFC02660 && pc <= 0xBFC026D0)
		{
			Console.WriteLn("@@LF_SWAP_FORCE@@ pc=%08x swap_was=%d forcing=false", pc, swap);
			swap = false;
		}
	}

    a64::Label j32Ptr;
	recSetBranchEQ(1, process, &j32Ptr);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

    armBind(&j32Ptr);

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

void recBNE()
{
	if ((pc - 4) == 0x9FC4109Cu)
	{
		static u32 s_ee4109c_log_count = 0;
		if (s_ee4109c_log_count < 4)
		{
			++s_ee4109c_log_count;
			Console.WriteLn("@@EE_LOOP_PATH@@ pc=%08x op=BNE path=%s constRs=%d constRt=%d rs=%d rt=%d code=%08x",
				pc - 4, GPR_IS_CONST2(_Rs_, _Rt_) ? "CONST" : "PROCESS",
				GPR_IS_CONST1(_Rs_) ? 1 : 0, GPR_IS_CONST1(_Rt_) ? 1 : 0, _Rs_, _Rt_, cpuRegs.code);
		}
		if (IsEeLoopNoConstBranchEnabled())
		{
			recBNE_process(0);
			return;
		}
	}

	const bool allow_lf_branch_compile_log = (!iPSX2_IsSafeOnlyEnabled() && IsDiagFlagsEnabled());
    // [TEMP_DIAG][REMOVE_AFTER=LF_BRANCH_COMPILE_V1] LoadFile Branch Compile-Time Trace (BNE entry)
	if (allow_lf_branch_compile_log && (pc & 0xFFFFFF00) == 0xBFC02600) {
	    static u32 s_lf_bne_logged = 0;
	    u32 bit = (pc >> 2) & 0x3F;  // Use lower bits as index
	    if (!(s_lf_bne_logged & (1u << bit))) {
	        s_lf_bne_logged |= (1u << bit);
		        const bool const_rs = GPR_IS_CONST1(_Rs_);
		        const bool const_rt = GPR_IS_CONST1(_Rt_);
		        const u32 rt_const_val = const_rt ? static_cast<u32>(g_cpuConstRegs[_Rt_].UL[0]) : 0xFFFFFFFFu;
		        const char* cmp_form = const_rt ? "cmp_reg_imm" : "cmp_reg_reg";
		        const char* cmp_width = ((pc == 0xBFC02670) || (pc == 0xBFC02680) || (pc == 0xBFC02690) || (pc == 0xBFC026A8)) ? "W32" : "X64";
		        Console.WriteLn("@@LF_BNE_COMPILE@@ pc=%08x code=%08x Rs=%d Rt=%d imm=%d target=%08x constRs=%d constRt=%d rt_const_val=%08x cmp_form=%s cmp_width=%s",
		            pc, cpuRegs.code, _Rs_, _Rt_, (s16)(_Imm_), pc + 4 + ((s16)(_Imm_) * 4),
		            const_rs ? 1 : 0, const_rt ? 1 : 0, rt_const_val, cmp_form, cmp_width);
	        fprintf(stderr, "@@LF_BNE_CFG@@ pc=%08x code=%08x\n", pc, cpuRegs.code);
	    }
	}


	// [FIX9 PROBE] Log cpuRegs.code for all BNE in bfc026xx range (one-shot per pc)
	if (allow_lf_branch_compile_log && (pc & 0xFFFFFF00) == 0xBFC02600) {
		static u32 s_fix9_probe_logged = 0;
		u32 bit = (pc >> 2) & 0x3F;
		if (!(s_fix9_probe_logged & (1u << bit))) {
			s_fix9_probe_logged |= (1u << bit);
			u32 rom_23c0 = (eeMem && eeMem->ROM) ? *(u32*)&eeMem->ROM[0x23C0] : 0xDEADBEEF;
			u32 rom_23c4 = (eeMem && eeMem->ROM) ? *(u32*)&eeMem->ROM[0x23C4] : 0xDEADBEEF;
			u32 rom_23c8 = (eeMem && eeMem->ROM) ? *(u32*)&eeMem->ROM[0x23C8] : 0xDEADBEEF;
			Console.WriteLn("@@FIX9_CODE_PROBE@@ pc=%08x cpuRegs_pc=%08x cpuRegs_code=%08x rom23c0=%08x rom23c4=%08x rom23c8=%08x",
				pc, cpuRegs.pc, cpuRegs.code, rom_23c0, rom_23c4, rom_23c8);
		}
	}

	// [FIX #9 JIT] bfc026a4: BNE v0, t2, +108 -> NOP at JIT compile time.
	// 0x144a001b = BNE v0(2), t2(10), +108. cpuRegs.code holds the raw instruction.
	if ((IsFix9JitNopEnabled()) && (cpuRegs.code == 0x144a001bu)) {
		static bool s_fix9_bne_logged = false;
		if (!s_fix9_bne_logged) {
			s_fix9_bne_logged = true;
			Console.WriteLn("@@FIX9_JIT_NOP@@ pc=%08x code=%08x -> forced NOP (recBNE)",
				cpuRegs.pc, cpuRegs.code);
		}
		// NOP: compile delay slot only, then fall through (no branch)
		recompileNextInstruction(true, false);
		SetBranchImm(pc); // fall through to pc+4 (after delay slot)
		return;
	}

	// [iter660] @@BNE_COMPILE_OSDSYS@@ OSDSYS rendering loop BNE diagnostic
	// Removal condition: 0x218030 loopcauseafter identified
	if ((pc - 4) >= 0x002181D0u && (pc - 4) <= 0x002181F0u) {
		static int s_bne_osdsys_n = 0;
		if (s_bne_osdsys_n < 5) {
			Console.WriteLn("@@BNE_COMPILE_OSDSYS@@ n=%d bpc=%08x rs=%d rt=%d "
				"constRs=%d constRt=%d path=%s code=%08x",
				s_bne_osdsys_n++, pc - 4, _Rs_, _Rt_,
				GPR_IS_CONST1(_Rs_) ? 1 : 0, GPR_IS_CONST1(_Rt_) ? 1 : 0,
				GPR_IS_CONST2(_Rs_, _Rt_) ? "CONST" :
				(GPR_IS_CONST1(_Rs_) ? "CONSTS" :
				(GPR_IS_CONST1(_Rt_) ? "CONSTT" : "NEITHER")),
				cpuRegs.code);
		}
	}

	// [FIX] Removed iPSX2_FORCE_JIT_VERIFY override that forced all BNE to process=0.
	// This was preventing PROCESS_CONSTT path (imm compare) from being used when t5/t6 are const.
	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBNE_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBNE_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBNE_process(PROCESS_CONSTT);
	else
		recBNE_process(0);
}

//// BEQL
static void recBEQL_const()
{
	if (g_cpuConstRegs[_Rs_].SD[0] == g_cpuConstRegs[_Rt_].SD[0])
	{
		u32 branchTo = ((s32)_Imm_ * 4) + pc;
		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
	}
	else
	{
		SetBranchImm(pc + 4);
	}
}

static void recBEQL_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

    a64::Label j32Ptr;
	recSetBranchEQ(0, process, &j32Ptr);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}

void recBEQL()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBEQL_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBEQL_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBEQL_process(PROCESS_CONSTT);
	else
		recBEQL_process(0);
}

//// BNEL
static void recBNEL_const()
{
	if (g_cpuConstRegs[_Rs_].SD[0] != g_cpuConstRegs[_Rt_].SD[0])
	{
		u32 branchTo = ((s32)_Imm_ * 4) + pc;
		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
	}
	else
	{
		SetBranchImm(pc + 4);
	}
}

static void recBNEL_process(int process)
{
	u32 branchTo = ((s32)_Imm_ * 4) + pc;

    a64::Label j32Ptr;
	// [iter217] FIX: BNEL branch paths were inverted.
	// B.eq j32Ptr → if Rs == Rt → NOT TAKEN → skip delay slot
	recSetBranchEQ(1, process, &j32Ptr);

	// Fall-through: Rs != Rt → BRANCH TAKEN → execute delay slot + go to branchTo
	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	// Rs == Rt → NOT TAKEN → skip delay slot
	LoadBranchState();
	SetBranchImm(pc);
}

void recBNEL()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
		recBNEL_const();
	else if (GPR_IS_CONST1(_Rs_) && _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ) < 0)
		recBNEL_process(PROCESS_CONSTS);
	else if (GPR_IS_CONST1(_Rt_) && _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ) < 0)
		recBNEL_process(PROCESS_CONSTT);
	else
		recBNEL_process(0);
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, offset                                 *
*********************************************************/

////////////////////////////////////////////////////
//void recBLTZAL()
//{
//	Console.WriteLn("BLTZAL");
//	_eeFlushAllUnused();
//	xMOV(ptr32[(u32*)((int)&cpuRegs.code)], cpuRegs.code );
//	xMOV(ptr32[(u32*)((int)&cpuRegs.pc)], pc );
//	iFlushCall(FLUSH_EVERYTHING);
//	xFastCall((void*)(int)BLTZAL );
//	branch = 2;
//}

////////////////////////////////////////////////////
void recBLTZAL()
{
	EE::Profiler.EmitOp(eeOpcode::BLTZAL);

	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
//	xMOV64(rax, pc + 4);
    armAsm->Mov(RAX, pc + 4);
//	xMOV(ptr64[&cpuRegs.GPR.n.ra.UD[0]], rax);
    armStore(PTR_CPU(cpuRegs.GPR.n.ra.UD[0]), RAX);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);

    a64::Label j32Ptr;
	recSetBranchL(1, &j32Ptr);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBGEZAL()
{
	EE::Profiler.EmitOp(eeOpcode::BGEZAL);

	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
//	xMOV64(rax, pc + 4);
    armAsm->Mov(RAX, pc + 4);
//	xMOV(ptr64[&cpuRegs.GPR.n.ra.UD[0]], rax);
    armStore(PTR_CPU(cpuRegs.GPR.n.ra.UD[0]), RAX);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);

    a64::Label j32Ptr;
	recSetBranchL(0, &j32Ptr);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBLTZALL()
{
	EE::Profiler.EmitOp(eeOpcode::BLTZALL);

	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
//	xMOV64(rax, pc + 4);
    armAsm->Mov(RAX, pc + 4);
//	xMOV(ptr64[&cpuRegs.GPR.n.ra.UD[0]], rax);
    armStore(PTR_CPU(cpuRegs.GPR.n.ra.UD[0]), RAX);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

    a64::Label j32Ptr;
	recSetBranchL(1, &j32Ptr);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBGEZALL()
{
	EE::Profiler.EmitOp(eeOpcode::BGEZALL);

	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	_eeOnWriteReg(31, 0);
	_eeFlushAllDirty();

	_deleteEEreg(31, 0);
//	xMOV64(rax, pc + 4);
    armAsm->Mov(RAX, pc + 4);
//	xMOV(ptr64[&cpuRegs.GPR.n.ra.UD[0]], rax);
    armStore(PTR_CPU(cpuRegs.GPR.n.ra.UD[0]), RAX);

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

    a64::Label j32Ptr;
	recSetBranchL(0, &j32Ptr);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}


//// BLEZ
void recBLEZ()
{
	EE::Profiler.EmitOp(eeOpcode::BLEZ);

	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] <= 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);
	const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0)
//		xCMP(xRegister64(regs), 0);
        armAsm->Cmp(a64::XRegister(HostGprPhys(regs)), 0);
	else
//		xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], 0);
        armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])), 0);

//	j32Ptr[0] = JG32(0);
    a64::Label j32Ptr;
    armAsm->B(&j32Ptr, a64::gt);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

//// BGTZ
void recBGTZ()
{
	EE::Profiler.EmitOp(eeOpcode::BGTZ);

	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] > 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);
	const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0) {
//		xCMP(xRegister64(regs), 0);
        armAsm->Cmp(a64::XRegister(HostGprPhys(regs)), 0);
    }
	else {
//		xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], 0);
        armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])), 0);
    }

//	j32Ptr[0] = JLE32(0);
    a64::Label j32Ptr;
    armAsm->B(&j32Ptr, a64::le);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBLTZ()
{
	EE::Profiler.EmitOp(eeOpcode::BLTZ);

	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);
	_eeFlushAllDirty();

    a64::Label j32Ptr;
	recSetBranchL(1, &j32Ptr);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBGEZ()
{
	EE::Profiler.EmitOp(eeOpcode::BGEZ);

	u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			branchTo = pc + 4;

		recompileNextInstruction(true, false);
		SetBranchImm(branchTo);
		return;
	}

	const bool swap = TrySwapDelaySlot(_Rs_, 0, 0, true);
	_eeFlushAllDirty();

    a64::Label j32Ptr;
	recSetBranchL(0, &j32Ptr);

	if (!swap)
	{
		SaveBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	if (!swap)
	{
		// recopy the next inst
		pc -= 4;
		LoadBranchState();
		recompileNextInstruction(true, false);
	}

	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBLTZL()
{
	EE::Profiler.EmitOp(eeOpcode::BLTZL);

	const u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] < 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

	_eeFlushAllDirty();

    a64::Label j32Ptr;
	recSetBranchL(1, &j32Ptr);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}


////////////////////////////////////////////////////
void recBGEZL()
{
	EE::Profiler.EmitOp(eeOpcode::BGEZL);

	const u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] >= 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

	_eeFlushAllDirty();

    a64::Label j32Ptr;
	recSetBranchL(0, &j32Ptr);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}



/*********************************************************
* Register branch logic  Likely                          *
* Format:  OP rs, offset                                 *
*********************************************************/

////////////////////////////////////////////////////
void recBLEZL()
{
	EE::Profiler.EmitOp(eeOpcode::BLEZL);

	const u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] <= 0))
			SetBranchImm(pc + 4);
		else
		{
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

	const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0) {
//        xCMP(xRegister64(regs), 0);
        armAsm->Cmp(a64::XRegister(HostGprPhys(regs)), 0);
    }
	else {
//        xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], 0);
        armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])), 0);
    }

//	j32Ptr[0] = JG32(0);
    a64::Label j32Ptr;
    armAsm->B(&j32Ptr, a64::Condition::gt);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}

////////////////////////////////////////////////////
void recBGTZL()
{
	EE::Profiler.EmitOp(eeOpcode::BGTZL);

	const u32 branchTo = ((s32)_Imm_ * 4) + pc;

	if (GPR_IS_CONST1(_Rs_))
	{
		if (!(g_cpuConstRegs[_Rs_].SD[0] > 0))
			SetBranchImm(pc + 4);
		else
		{
			_clearNeededXMMregs();
			recompileNextInstruction(true, false);
			SetBranchImm(branchTo);
		}
		return;
	}

	const int regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	_eeFlushAllDirty();

	if (regs >= 0) {
//        xCMP(xRegister64(regs), 0);
        armAsm->Cmp(a64::XRegister(HostGprPhys(regs)), 0);
    }
	else {
//        xCMP(ptr64[&cpuRegs.GPR.r[_Rs_].UD[0]], 0);
        armAsm->Cmp(armLoad64(PTR_CPU(cpuRegs.GPR.r[_Rs_].UD[0])), 0);
    }

//	j32Ptr[0] = JLE32(0);
    a64::Label j32Ptr;
    armAsm->B(&j32Ptr, a64::Condition::le);

	SaveBranchState();
	recompileNextInstruction(true, false);
	SetBranchImm(branchTo);

//	x86SetJ32(j32Ptr[0]);
    armBind(&j32Ptr);

	LoadBranchState();
	SetBranchImm(pc);
}

#endif

} // namespace R5900::Dynarec::OpcodeImpl
