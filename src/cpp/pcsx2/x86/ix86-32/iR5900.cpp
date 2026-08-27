// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// [iPSX2] Force Rebuild to clear stale symbols (Step 870)

#include "Common.h"
#include "MemoryTypes.h"
#include "CDVD/CDVD.h"
#include "DebugTools/Breakpoints.h"
#include "Elfheader.h"
#include "GS.h"
#include "Memory.h"
#include "GS/GSRegs.h"
#include "Hw.h"
#include "Patch.h"
#include "R3000A.h"
#include <cstdlib>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h> // [TEMP_DIAG] @@EVENTTEST_STUCK@@
#include <mach/mach.h> // [TEMP_DIAG] @@THREAD_PC@@ for thread_get_state
#include <dlfcn.h>     // [TEMP_DIAG] @@THREAD_PC@@ for dladdr
#include "R5900OpcodeTables.h"
#include "VMManager.h"
#include "vtlb.h"
#include "x86/BaseblockEx.h"
#include "x86/iR5900.h"
#include "common/HostSys.h"
#include "x86/iR5900Analysis.h"
#include "IopMem.h"

#include "common/AlignedMalloc.h"
#include "common/FastJmp.h"
#include "common/HeapArray.h"
#include "common/Perf.h"
#include "x86/microVU_Misc.h"
#include "Memory.h"
#include "ps2/BiosTools.h" // [iter654] BiosRetriggerEeloadCopy

#ifdef __APPLE__
#include "common/Darwin/DarwinMisc.h"
#endif

namespace IopMemory
{
	void DumpIop3204Ring();
}

// Only for MOVQ workaround.
#if !defined(__ANDROID__)
#include "common/emitter/internal.h"
#endif

//#define DUMP_BLOCKS 1
//#define TRACE_BLOCKS 1
#define PCSX2_VTLB_DISPATCH_ENABLE 1


// Forward declaration for vtlb dispatcher base (defined in recVTLB.cpp)
extern std::atomic<u8*> g_vtlb_dispatcher_base;
extern u8 iopHw[]; // Global IOP Memory Array
extern int g_iop3204_test_val; // Defined in IopHwRead.cpp
#define ENABLE_TRACE 1

// Direct Logging Helper
extern "C" void LogUnified(const char* fmt, ...);

#ifdef DUMP_BLOCKS
#include "Zydis/Zydis.h"
#include "Zycore/Format.h"
#include "Zycore/Status.h"
#endif

#ifdef TRACE_BLOCKS
#include <zlib.h>
#endif
#include <stdlib.h> // for getenv

// [LOADFILE_FORCE_SUCCESS] Force LoadFile to return success
// Strategy: When LoadFile returns, patch the BEQ at 0xBFC023D8 to skip error path
// This makes the caller think LoadFile succeeded even if it returned 0
#ifndef iPSX2_ENABLE_LOADFILE_FORCE_SUCCESS
#define iPSX2_ENABLE_LOADFILE_FORCE_SUCCESS 0  // Disabled: BIOS should work naturally now
#endif

// [LOADFILE_TRACE] Trace LoadFile function execution to find PC=0 cause
// Patches key locations in LoadFile to log execution flow
#ifndef iPSX2_ENABLE_LOADFILE_TRACE
#define iPSX2_ENABLE_LOADFILE_TRACE 0  // Disabled by default
#endif

static DynamicHeapArray<u8, 4096> recRAMCopy;
static DynamicHeapArray<u8, 4096> recLutReserve_RAM;
static size_t recLutSize;
static bool extraRam;

static BASEBLOCK* recRAM = nullptr; // and the ptr to the blocks here
static BASEBLOCK* recROM = nullptr; // and here
static BASEBLOCK* recROM1 = nullptr; // also here
static BASEBLOCK* recROM2 = nullptr; // also here

static BaseBlocks recBlocks;
static u8* recPtr = nullptr;
static u8* recPtrEnd = nullptr;
EEINST* s_pInstCache = nullptr;
static u32 s_nInstCacheSize = 0;

// [TEMP_DIAG] @@JIT_PERF_COUNTERS@@ — per-vsync compile/recCall counts
// Removal condition: JIT performance issue resolved
std::atomic<uint32_t> s_jit_compile_count{0};  // blocks compiled since last report
std::atomic<uint32_t> s_jit_reccall_count{0};   // recCall invocations since last report
std::atomic<uint32_t> s_jit_cache_clear_count{0}; // recCACHE_ClearBlock calls since last report

// [TEMP_DIAG] @@COMPILE_CONTENT@@ — per-PC compile frequency + block size tracking
// Removal condition: compile 爆増の型after determined
#include <unordered_map>
#include <unordered_set>
static std::unordered_map<u32, uint16_t> s_compile_pc_freq;  // HWADDR(startpc) → count this window
static std::unordered_map<u32, uint16_t> s_compile_vpc_freq; // virtual startpc → count this window
// [TEMP_DIAG] @@PERSISTENT_SET@@ — track all compiled VPCs across reset cycle
static std::unordered_set<u32> s_ever_compiled_vpcs;
static uint32_t s_recompile_of_known = 0; // # times a known-compiled VPC is compiled again
static uint64_t s_compile_guest_insns = 0;   // total guest instructions compiled
static uint64_t s_compile_native_bytes = 0;  // total ARM64 bytes emitted
static uint32_t s_compile_mtc1_blocks = 0;   // blocks containing MTC1 opcode

// [TEMP_DIAG] @@INVALIDATION_PATH@@ — track block/page invalidation counts
// Removal condition: compile 爆増のroot causeafter fixed
static std::atomic<uint32_t> s_dyna_block_discard_count{0};
static std::atomic<uint32_t> s_dyna_page_reset_count{0};
static std::atomic<uint32_t> s_overlap_clear_count{0}; // recRecompile overlap memcmp→recClear
static std::atomic<uint32_t> s_recClear_total_count{0}; // ALL recClear calls
static std::atomic<uint32_t> s_recClear_total_blocks{0}; // total blocks cleared by recClear
// [TEMP_DIAG] @@RECCLEAR_BUCKET@@ — size category counters
static std::atomic<uint32_t> s_recClear_sz1{0};     // size==1 (single instruction)
static std::atomic<uint32_t> s_recClear_sz1024{0};  // size==0x400 (page clear)
static std::atomic<uint32_t> s_recClear_szlarge{0}; // size>=0x100000 (full RAM clear)
static std::atomic<uint32_t> s_recClear_szother{0}; // everything else
// [TEMP_DIAG] track which pages trigger overlap clears
static uint32_t s_overlap_page_hist[2048] = {}; // page = oldBlock->startpc >> 12, up to 8MB

// [TEMP_DIAG] @@MMAP_CLEAR@@ — extern counters from vtlb.cpp (write-protection fault → recClear)
extern std::atomic<uint32_t> g_mmap_clear_count;
extern std::atomic<uint32_t> g_mmap_clear_page_top;
extern std::atomic<uint32_t> g_mmap_clear_page_top_count;
// [TEMP_DIAG] @@STUB_GUARD@@ — counter from Counters.cpp OSDSYS_FULL_GUARD path
extern std::atomic<uint32_t> g_stub_guard_clear_count;

static BASEBLOCK* s_pCurBlock = nullptr;
static BASEBLOCKEX* s_pCurBlockEx = nullptr;
u32 s_nEndBlock = 0; // what pc the current block ends
u32 s_branchTo;
static bool s_nBlockFF;

// save states for branches
GPR_reg64 s_saveConstRegs[64]; // [iPSX2] Increased size to buffer overflow
volatile u64 s_const_canary = 0xDEADBEEFCAFEBABE; // [iPSX2] Overflow guard
static u32 s_saveHasConstReg = 0, s_saveFlushedConstReg = 0;
static EEINST* s_psaveInstInfo = nullptr;

static u32 s_savenBlockCycles = 0;

static void iBranchTest(u32 newpc = 0xffffffff);
static void ClearRecLUT(BASEBLOCK* base, int count);
static u32 scaleblockcycles();
static void recExitExecution();

// Global lookup tables for recompiler
uptr recLUT[0x10000];
u32 hwLUT[0x10000];

// Global State for Recompiler
bool eeRecExitRequested = false;
bool eeCpuExecuting = false;
bool eeRecNeedsReset = false;
bool g_resetEeScalingStats = false;
bool g_cpuFlushedPC = false;
bool g_cpuFlushedCode = false;

// Global exception flag
bool g_maySignalException = false;

using namespace R5900;

// Missing Standard JIT Globals
u32 pc = 0;
int g_branch = 0;
// [iter39] Track current block's startpc for SetBranchImm diagnostic.
static u32 s_recblock_startpc = 0;
u32 target = 0;
u32 s_nBlockCycles = 0;
bool s_nBlockInterlocked = false;
bool g_recompilingDelaySlot = false;
u32 maxrecmem = 0;
alignas(16) GPR_reg64 g_cpuConstRegs[32];
u32 g_cpuHasConstReg = 0;
u32 g_cpuFlushedConstReg = 0;

// Missing Diagnostic Globals (Dummies to satisfy linker)
u32 g_lf_cmp_flags = 0;
u32 g_lf_cmp_freeze = 0;
u32 g_lf_cmp_pc = 0;
u8 g_lf_cmp_ring[4096];
u32 g_lf_cmp_ring_idx = 0;
u32 g_lf_cmp_rs = 0;
u32 g_lf_cmp_rt = 0;
u32 g_lf_cmp_seen = 0;
u32 g_lf_cmp_t0 = 0;

u32 g_lf_entry_a0 = 0;
u32 g_lf_entry_ra = 0;
u32 g_lf_entry_seen = 0;

u32 g_lf_match_seen = 0;
u32 g_lf_match_t0 = 0;
u32 g_lf_match_v0 = 0;
u32 g_lf_precall_seen = 0;
u32 g_lf_precall_a0 = 0;
u32 g_lf_precall_v0 = 0;
u32 g_lf_precall_t0 = 0;
u32 g_lf_precall_ra = 0;
u32 g_lf_precall_flags = 0;

u32 g_lf_v3_done = 0;
u32 g_lf_v3_idx = 0;
u8 g_lf_v3_ring[4096];

static bool IsFixLfA0CallsiteEnabled()
{
	static int s_cached = -1;
	if (s_cached < 0)
	{
		if (iPSX2_IsSafeOnlyEnabled())
		{
			s_cached = 0;
			return false;
		}
		const bool diag_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_ENABLE_DIAG_FLAGS", false);
		const bool feature_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_FIX_LF_A0_CALLSITE", false);
		s_cached = (diag_enabled && feature_enabled) ? 1 : 0;
	}
	return s_cached == 1;
}

static bool IsDiagFlagsEnabled()
{
	static int s_cached = -1;
	if (s_cached < 0)
	{
		s_cached = iPSX2_GetRuntimeEnvBool("iPSX2_ENABLE_DIAG_FLAGS", false) ? 1 : 0;
	}
	return (s_cached == 1);
}

static bool IsRecompileEscapeEnabled()
{
	if (iPSX2_IsSafeOnlyEnabled() || !IsDiagFlagsEnabled())
		return false;
	static int s_cached = -1;
	if (s_cached < 0)
	{
		s_cached = iPSX2_GetRuntimeEnvBool("iPSX2_ALLOW_RECOMPILE_ESCAPE", false) ? 1 : 0;
	}
	return (s_cached == 1);
}

static bool IsLfAutoDumpOnPanicEnabled()
{
	static int s_cached = -1;
	if (s_cached < 0)
		s_cached = iPSX2_GetRuntimeEnvBool("iPSX2_LF_AUTODUMP_ON_PANIC", false) ? 1 : 0;
	return (s_cached == 1);
}

// [STEP2] Flight Recorder Globals
// Struct defined in iR5900.h
constexpr int STEP2_RING_SIZE = 128;
alignas(64) volatile Step2FlightRecEntry g_step2_ring[STEP2_RING_SIZE] = {};
alignas(64) volatile u32 g_step2_idx = 0;
alignas(64) volatile u32 g_step2_freeze = 0;

    // [DIAG] ROM_READ_DIAG_V1
alignas(64) volatile RomReadDiagEntry g_rom_diag_ring[16] = {};
alignas(64) volatile u32 g_rom_diag_idx = 0;

extern "C" void recLogBlockRun() {}

// Missing Functions/Objects
extern "C" u32 GetRecompilerPC() { 
    return pc; 
}

namespace EE {
    eeProfiler Profiler;
}

// Macros
#define PC_GETBLOCK(x) PC_GETBLOCK_(x, recLUT)
#define HWADDR(x) ((x) & 0x1fffffff)

#ifdef TRACE_BLOCKS
static void pauseAAA()
{
	fprintf(stderr, "\nPaused\n");
	fflush(stdout);
	fflush(stderr);
#ifdef _MSC_VER
	__debugbreak();
#else
	sleep(1);
#endif
}
#endif

#ifdef DUMP_BLOCKS
static ZydisFormatterFunc s_old_print_address;

static ZyanStatus ZydisFormatterPrintAddressAbsolute(const ZydisFormatter* formatter,
	ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
	ZyanU64 address;
	ZYAN_CHECK(ZydisCalcAbsoluteAddress(context->instruction, context->operand,
		context->runtime_address, &address));

	char buf[128];
	u32 len = 0;

#define A(x) ((u64)(x))

	if (address >= A(eeMem->Main) && address < A(eeMem->Scratch))
	{
		len = snprintf(buf, sizeof(buf), "eeMem+0x%08X", static_cast<u32>(address - A(eeMem->Main)));
	}
	else if (address >= A(eeMem->Scratch) && address < A(eeMem->ROM))
	{
		len = snprintf(buf, sizeof(buf), "eeScratchpad+0x%08X", static_cast<u32>(address - A(eeMem->Scratch)));
	}
	else if (address >= A(&cpuRegs.GPR) && address < A(&cpuRegs.HI))
	{
		const u32 offset = static_cast<u32>(address - A(&cpuRegs)) % 16u;
		if (offset != 0)
			len = snprintf(buf, sizeof(buf), "cpuRegs.GPR.%s+%u", GPR_REG[static_cast<u32>(address - A(&cpuRegs)) / 16u], offset);
		else
			len = snprintf(buf, sizeof(buf), "cpuRegs.GPR.%s", GPR_REG[static_cast<u32>(address - A(&cpuRegs)) / 16u]);
	}
	else if (address >= A(&cpuRegs.HI) && address < A(&cpuRegs.CP0))
	{
		const u32 offset = static_cast<u32>(address - A(&cpuRegs.HI)) % 16u;
		if (offset != 0)
			len = snprintf(buf, sizeof(buf), "cpuRegs.%s+%u", (address >= A(&cpuRegs.LO) ? "LO" : "HI"), offset);
		else
			len = snprintf(buf, sizeof(buf), "cpuRegs.%s", (address >= A(&cpuRegs.LO) ? "LO" : "HI"));
	}
	else if (address == A(&cpuRegs.pc))
	{
		len = snprintf(buf, sizeof(buf), "cpuRegs.pc");
	}
	else if (address == A(&cpuRegs.cycle))
	{
		len = snprintf(buf, sizeof(buf), "cpuRegs.cycle");
	}
	else if (address == A(&cpuRegs.nextEventCycle))
	{
		len = snprintf(buf, sizeof(buf), "cpuRegs.nextEventCycle");
	}
	else if (address >= A(fpuRegs.fpr) && address < A(fpuRegs.fprc))
	{
		len = snprintf(buf, sizeof(buf), "fpuRegs.f%02u", static_cast<u32>(address - A(fpuRegs.fpr)) / 4u);
	}
	else if (address >= A(&VU0.VF[0]) && address < A(&VU0.VI[0]))
	{
		const u32 offset = static_cast<u32>(address - A(&VU0.VF[0])) % 16u;
		if (offset != 0)
			len = snprintf(buf, sizeof(buf), "VU0.VF[%02u]+%u", static_cast<u32>(address - A(&VU0.VF[0])) / 16u, offset);
		else
			len = snprintf(buf, sizeof(buf), "VU0.VF[%02u]", static_cast<u32>(address - A(&VU0.VF[0])) / 16u);
	}
	else if (address >= A(&VU0.VI[0]) && address < A(&VU0.ACC))
	{
		const u32 offset = static_cast<u32>(address - A(&VU0.VI[0])) % 16u;
		const u32 vi = static_cast<u32>(address - A(&VU0.VI[0])) / 16u;
		if (offset != 0)
			len = snprintf(buf, sizeof(buf), "VU0.%s+%u", COP2_REG_CTL[vi], offset);
		else
			len = snprintf(buf, sizeof(buf), "VU0.%s", COP2_REG_CTL[vi]);
	}
	else if (address >= A(&VU0.ACC) && address < A(&VU0.q))
	{
		const u32 offset = static_cast<u32>(address - A(&VU0.ACC));
		if (offset != 0)
			len = snprintf(buf, sizeof(buf), "VU0.ACC+%u", offset);
		else
			len = snprintf(buf, sizeof(buf), "VU0.ACC");
	}
	else if (address >= A(&VU0.q) && address < A(&VU0.idx))
	{
		const u32 offset = static_cast<u32>(address - A(&VU0.q)) % 16u;
		const char* reg = (address >= A(&VU0.p)) ? "p" : "q";
		if (offset != 0)
			len = snprintf(buf, sizeof(buf), "VU0.%s+%u", reg, offset);
		else
			len = snprintf(buf, sizeof(buf), "VU0.%s", reg);
	}

#undef A

	if (len > 0)
	{
		ZYAN_CHECK(ZydisFormatterBufferAppend(buffer, ZYDIS_TOKEN_SYMBOL));
		ZyanString* string;
		ZYAN_CHECK(ZydisFormatterBufferGetString(buffer, &string));
		return ZyanStringAppendFormat(string, "&%s", buf);
	}

	return s_old_print_address(formatter, buffer, context);
}
#endif

void _eeFlushAllDirty()
{
	_flushXMMregs();
	_flushX86regs();

	// flush constants, do them all at once for slightly better codegen
	_flushConstRegs(false);
}

void _eeMoveGPRtoR(const a64::Register& to, int fromgpr, bool allow_preload)
{
	if (fromgpr == 0) {
        if(to.IsW()) {
//            xXOR(to, to);
            armAsm->Eor(to, to, to);
        } else {
//            xXOR(xRegister32(to), xRegister32(to));
            auto reg32 = a64::WRegister(to);
            armAsm->Eor(reg32, reg32, reg32);
        }
    }
	else if (GPR_IS_CONST1(fromgpr)) {
        if(to.IsW()) {
//        xMOV(to, g_cpuConstRegs[fromgpr].UL[0]);
            armAsm->Mov(to, g_cpuConstRegs[fromgpr].UL[0]);
        } else {
//        xMOV64(to, g_cpuConstRegs[fromgpr].UD[0]);
            armAsm->Mov(to, g_cpuConstRegs[fromgpr].UD[0]);
        }
    }
	else
	{
		int x86reg = _checkX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, fromgpr, MODE_READ);

		if (allow_preload && x86reg < 0 && xmmreg < 0)
		{
			if (EEINST_XMMUSEDTEST(fromgpr))
				xmmreg = _allocGPRtoXMMreg(fromgpr, MODE_READ);
			else if (EEINST_USEDTEST(fromgpr))
				x86reg = _allocX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		}

		if (x86reg >= 0) {
            if(to.IsW()) {
//            xMOV(to, xRegister32(x86reg));
                armAsm->Mov(to, HostW(x86reg));
            } else {
//            xMOV(to, xRegister64(x86reg));
                armAsm->Mov(to, HostX(x86reg));
            }
        }
		else if (xmmreg >= 0) {
            if(to.IsW()) {
//            xMOVD(to, xRegisterSSE(xmmreg));
                armAsm->Fmov(to, a64::QRegister(xmmreg).S());
            } else {
//            xMOVD(to, xRegisterSSE(xmmreg));
                armAsm->Fmov(to, a64::QRegister(xmmreg).D());
            }
        }
		else {
            if(to.IsW()) {
//            xMOV(to, ptr[&cpuRegs.GPR.r[fromgpr].UL[0]]);
                armLoad(to, PTR_CPU(cpuRegs.GPR.r[fromgpr].UL[0]));
            } else {
//            xMOV(to, ptr32[&cpuRegs.GPR.r[fromgpr].UD[0]]);
                armLoad(to, PTR_CPU(cpuRegs.GPR.r[fromgpr].UD[0]));
            }
        }
	}
}

void _eeMoveGPRtoM(const a64::MemOperand& to, int fromgpr)
{
	if (GPR_IS_CONST1(fromgpr)) {
//        xMOV(ptr32[(u32 *) (to)], g_cpuConstRegs[fromgpr].UL[0]);
        armStorePtr(g_cpuConstRegs[fromgpr].UL[0], to);
    }
	else
	{
		int x86reg = _checkX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, fromgpr, MODE_READ);

		if (x86reg < 0 && xmmreg < 0)
		{
			if (EEINST_XMMUSEDTEST(fromgpr))
				xmmreg = _allocGPRtoXMMreg(fromgpr, MODE_READ);
			else if (EEINST_USEDTEST(fromgpr))
				x86reg = _allocX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		}

		if (x86reg >= 0)
		{
//			xMOV(ptr32[(void*)(to)], xRegister32(x86reg));
            armAsm->Str(HostW(x86reg), to);
		}
		else if (xmmreg >= 0)
		{
//			xMOVSS(ptr32[(void*)(to)], xRegisterSSE(xmmreg));
            armAsm->Str(a64::QRegister(xmmreg).S(), to);
		}
		else
		{
//			xMOV(eax, ptr32[&cpuRegs.GPR.r[fromgpr].UL[0]]);
            armLoad(EAX, PTR_CPU(cpuRegs.GPR.r[fromgpr].UL[0]));
//			xMOV(ptr32[(void*)(to)], eax);
            armAsm->Str(EAX, to);
		}
	}
}

// Use this to call into interpreter functions that require an immediate branchtest
// to be done afterward (anything that throws an exception or enables interrupts, etc).
void recBranchCall(void (*func)())
{
	// In order to make sure a branch test is performed, the nextBranchCycle is set
	// to the current cpu cycle.

//	xMOV(eax, ptr[&cpuRegs.cycle]);
    armLoad(EAX, PTR_CPU(cpuRegs.cycle));
//	xMOV(ptr[&cpuRegs.nextEventCycle], eax);
    armStore(PTR_CPU(cpuRegs.nextEventCycle), EAX);

	recCall(func);
	g_branch = 2;
}

void recCall(void (*func)())
{
	s_jit_reccall_count.fetch_add(1, std::memory_order_relaxed); // [TEMP_DIAG] compile-time count
	iFlushCall(FLUSH_INTERPRETER);
    armEmitCall(reinterpret_cast<void*>(func));
}

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {

// SYNC is a memory barrier, but for emulation purposes, NOP is usually sufficient
// unless strict memory ordering is required between threads/devices.
// Implementing as NOP avoids "Unimplemented Opcode" exceptions.
void recSYNC()
{
    // Do nothing
    armAsm->Nop();
}

} // namespace OpcodeImpl
} // namespace Dynarec
} // namespace R5900

void recBREAK()
{
	if(cpuRegs.code & 0x4000000)
		return;

	//xMOV(eax, cpuRegs.code);
	//xFastCall((void*)Interp::BREAK);
    armAsm->Mov(EAX, cpuRegs.code);
    armEmitCall(reinterpret_cast<void*>(R5900::Interpreter::OpcodeImpl::BREAK));
}

// =====================================================================================================
//  R5900 Dispatchers
// =====================================================================================================

// [TEMP_DIAG] @@DSLL_LOOP_PROBE@@ — log registers at DSLL loop (0x2659f0)
// Removal condition: ギャップcauseafter identified
void dsll_loop_probe()
{
	static u32 s_n = 0;
	if (s_n < 20) {
		// DSLL loop at 0x2659f0: v1=a1<<1, a0 decrement, slt(a2,v1)
		// Registers: v0=$2, v1=$3, a0=$4, a1=$5, a2=$6
		Console.WriteLn("@@DSLL_LOOP@@ n=%u a0=%016llx a1=%016llx a2=%016llx v0=%016llx v1=%016llx pc=%08x cyc=%u",
			s_n,
			cpuRegs.GPR.r[4].UD[0],  // a0
			cpuRegs.GPR.r[5].UD[0],  // a1
			cpuRegs.GPR.r[6].UD[0],  // a2
			cpuRegs.GPR.r[2].UD[0],  // v0
			cpuRegs.GPR.r[3].UD[0],  // v1
			cpuRegs.pc, cpuRegs.cycle);
		s_n++;
	}
}

// [iter90] @@JAL_83860@@ — JAL 0x83860 の引数・戻り値キャプチャ用probefunction
// Removal condition: SYSCALL loop (0x82064/0x82080) root cause確定時
static void probe_83860_entry()
{
	static int s_n = 0;
	if (s_n < 5) {
		s_n++;
		Console.WriteLn("@@JAL_83860_ENTRY@@ n=%d a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x",
			s_n, cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
			cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.a3.UL[0],
			cpuRegs.GPR.n.ra.UL[0]);
	}
}
static void probe_8208c_eret()
{
	static int s_n = 0;
	if (s_n < 5) {
		s_n++;
		Console.WriteLn("@@ERET_8208C@@ n=%d v0=%08x v1=%08x epc=%08x sp=%08x",
			s_n, cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
			cpuRegs.CP0.n.EPC, cpuRegs.GPR.n.sp.UL[0]);
	}
}

// [iter96] @@EELOAD_SLTI_823B8@@ – SLTI v0,s4,2 直前の s2(argc) / s4 実行時値をキャプチャ
// Removal condition: BNEZ 分岐先（0x8256C 経路 vs 0x820E8 経路）確定時
static void probe_eeload_slti()
{
	static int s_n = 0;
	if (s_n++ < 5) {
		const u32 s2 = cpuRegs.GPR.r[18].UL[0]; // $s2 = register 18 = argc
		const u32 s4 = cpuRegs.GPR.r[20].UL[0]; // $s4 = register 20
		const u32 v0 = cpuRegs.GPR.n.v0.UL[0];
		Console.WriteLn("@@EELOAD_SLTI_823B8@@ n=%d s2=%08x s4=%08x v0=%08x",
			s_n, s2, s4, v0);
	}
}

static void recRecompile(const u32 startpc);
static void dyna_block_discard(u32 start, u32 sz);
static void dyna_page_reset(u32 start, u32 sz);

static const void* DispatcherEvent = nullptr;
static const void* DispatcherReg = nullptr;
static const void* JITCompile = nullptr;

extern "C" void recLogJITCompile(u32 pc); // Forward declaration
static const void* EnterRecompiledCode = nullptr;
static const void* DispatchBlockDiscard = nullptr;
static const void* DispatchPageReset = nullptr;

#include "Hw.h"

static bool IsDispatchNegL1SlowpathEnabled()
{
	static int s_cached = -1;
	if (s_cached < 0)
		s_cached = iPSX2_GetRuntimeEnvBool("iPSX2_DISPATCH_NEG_L1_SLOWPATH", false) ? 1 : 0;
	return s_cached == 1;
}

extern uint g_FrameCount; // forward decl for EVENTTEST_STUCK probe
// [TEMP_DIAG] @@EVENTTEST_CNT@@ atomic counter to track recEventTest calls from watchdog
static std::atomic<uint64_t> s_recEventTest_cnt{0};

// [REMOVED] BlockWatchpoint220A8 — removed after SDL/SDR JIT bug confirmed as root cause
// of "bd file open failed" corruption. See recSDL/recSDR interpreter fallback fix.
#if 0
static void BlockWatchpoint220A8(u32 blockpc)
{
	if (!eeMem) return;

	// Source buffer watch (ungated — need to catch early preparation)
	// Use ring buffer to record last 16 transitions, only print when corruption detected
	struct SrcTransition { u32 cycle; u32 blockpc; u32 oldval; u32 newval; u32 ra; u32 a0; u32 a1; };
	static SrcTransition s_src_ring[16];
	static int s_src_ring_idx = 0;
	static u32 s_bw_prev_src = 0;
	static bool s_bw_src_done = false;
	if (!s_bw_src_done) {
		u32 cur_src = *(u32*)(eeMem->Main + 0x5388D8);
		if (cur_src != s_bw_prev_src) {
			int ri = s_src_ring_idx & 15;
			s_src_ring[ri] = {cpuRegs.cycle, blockpc, s_bw_prev_src, cur_src,
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0]};
			s_src_ring_idx++;
			s_bw_prev_src = cur_src;
			// If transitioning to "cdro", dump ring + region
			if (cur_src == 0x6f726463) {
				s_bw_src_done = true;
				int n = std::min(s_src_ring_idx, 16);
				for (int i = 0; i < n; i++) {
					int idx = (s_src_ring_idx - n + i) & 15;
					auto& t = s_src_ring[idx];
					Console.Error("@@BLOCK_WATCH_SRC@@ [%d] cyc=%u bpc=%08x src[5388D8]: %08x->%08x ra=%08x a0=%08x a1=%08x",
						i, t.cycle, t.blockpc, t.oldval, t.newval, t.ra, t.a0, t.a1);
				}
				char hexbuf[256] = {};
				int pos = 0;
				for (u32 i = 0; i < 16 && pos < 240; i++) {
					u32 v = *(u32*)(eeMem->Main + 0x5388C0 + i * 4);
					pos += snprintf(hexbuf + pos, 256 - pos, "%08x ", v);
				}
				Console.Error("@@BLOCK_WATCH_SRC@@ CORRUPT! dump[5388C0]: %s", hexbuf);
				// Dump MIPS at the last block that wrote the corruption
				{
					u32 bpc_phys = blockpc & 0x1FFFFFFF;
					if (bpc_phys < 0x2000000) {
						char ibuf[512] = {};
						int p = 0;
						for (int k = 0; k < 16 && p < 490; k++) {
							u32 insn = *(u32*)(eeMem->Main + bpc_phys + k * 4);
							p += snprintf(ibuf + p, 512 - p, "%08x ", insn);
						}
						Console.Error("@@BLOCK_WATCH_SRC@@ MIPS[%08x]: %s", blockpc, ibuf);
					}
				}
			}
		}
	}

	// Fast exit for 99.9% of calls — only activate near known corruption cycle
	if (cpuRegs.cycle < 1537500000u || cpuRegs.cycle > 1539000000u) return;

	static u32 s_bw_prev_a8 = 0;
	static u32 s_bw_prev_a0 = 0;
	static bool s_bw_done = false;
	if (s_bw_done) return;

	u32 cur_a0 = *(u32*)(eeMem->Main + 0x220A0);
	u32 cur_a8 = *(u32*)(eeMem->Main + 0x220A8);

	if (cur_a0 != s_bw_prev_a0 || cur_a8 != s_bw_prev_a8) {
		Console.Error("@@BLOCK_WATCH@@ blockpc=%08x cyc=%u a0: %08x->%08x a8: %08x->%08x pc=%08x",
			blockpc, cpuRegs.cycle, s_bw_prev_a0, cur_a0, s_bw_prev_a8, cur_a8, cpuRegs.pc);
		s_bw_prev_a0 = cur_a0;
		s_bw_prev_a8 = cur_a8;

		if (cur_a8 == 0x6f726463) {
			s_bw_done = true;
			// Dump full 0x22080-0x220C0 region
			char hexbuf[320] = {};
			int pos = 0;
			for (u32 i = 0; i < 16 && pos < 300; i++) {
				u32 v = *(u32*)(eeMem->Main + 0x22080 + i * 4);
				pos += snprintf(hexbuf + pos, 320 - pos, "%08x ", v);
			}
			Console.Error("@@BLOCK_WATCH@@ CORRUPT! dump[22080]: %s", hexbuf);
			// Dump GPR state
			Console.Error("@@BLOCK_WATCH@@ a0=%08x a1=%08x a2=%08x a3=%08x v0=%08x v1=%08x t0=%08x t1=%08x s0=%08x s1=%08x ra=%08x",
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
				cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.a3.UL[0],
				cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
				cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0],
				cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0],
				cpuRegs.GPR.n.ra.UL[0]);
			// Dump key MIPS regions
			// 1) Syscall stubs 0x1E96D0-0x1E9700
			{
				char ibuf[512] = {};
				int p = 0;
				for (int k = 0; k < 12 && p < 490; k++) {
					u32 insn = *(u32*)(eeMem->Main + 0x1E96D0 + k * 4);
					p += snprintf(ibuf + p, 512 - p, "%08x ", insn);
				}
				Console.Error("@@MIPS_STUBS@@ [1E96D0]: %s", ibuf);
			}
			// 2) Caller function 0x1EB800-0x1EB8C0 (full function with context before BEQ)
			{
				for (int row = 0; row < 3; row++) {
					char ibuf[512] = {};
					int p = 0;
					u32 base = 0x1EB800 + row * 64;
					for (int k = 0; k < 16 && p < 490; k++) {
						u32 insn = *(u32*)(eeMem->Main + base + k * 4);
						p += snprintf(ibuf + p, 512 - p, "%08x ", insn);
					}
					Console.Error("@@MIPS_CALLER@@ [%06x]: %s", base, ibuf);
				}
			}
			// 3) Even more context: dump 0x1EB780-0x1EB800 for function prologue
			{
				char ibuf[512] = {};
				int p = 0;
				for (int k = 0; k < 16 && p < 490; k++) {
					u32 insn = *(u32*)(eeMem->Main + 0x1EB780 + k * 4);
					p += snprintf(ibuf + p, 512 - p, "%08x ", insn);
				}
				Console.Error("@@MIPS_CALLER@@ [1EB780]: %s", ibuf);
			}
			// 4) Source buffer content at corruption time
			{
				char hexbuf[256] = {};
				int pos = 0;
				for (u32 i = 0; i < 16 && pos < 240; i++) {
					u32 v = *(u32*)(eeMem->Main + 0x5388C0 + i * 4);
					pos += snprintf(hexbuf + pos, 256 - pos, "%08x ", v);
				}
				Console.Error("@@BLOCK_WATCH@@ SRC_BUF[5388C0]: %s", hexbuf);
			}
			// 5) Dump EE syscall table entry for syscall 0x64
			// PS2 EE syscall table is at 0x800003C0 (or nearby). Each entry is 4 bytes (pointer).
			// Try common table bases: 0x400, 0x800
			{
				for (u32 tbase : {0x00000340u, 0x00000400u, 0x00000800u}) {
					u32 entry_addr = tbase + 0x64 * 4;
					if (entry_addr < 0x2000000) {
						u32 handler = *(u32*)(eeMem->Main + entry_addr);
						Console.Error("@@SYSCALL_TABLE@@ base=%05x entry[64]@%05x = %08x",
							tbase, entry_addr, handler);
						// If handler looks like a valid address, dump its MIPS code
						u32 hphys = handler & 0x1FFFFFFF;
						if (hphys > 0 && hphys < 0x2000000 - 128) {
							char ibuf[512] = {};
							int p = 0;
							for (int k = 0; k < 16 && p < 490; k++) {
								u32 insn = *(u32*)(eeMem->Main + hphys + k * 4);
								p += snprintf(ibuf + p, 512 - p, "%08x ", insn);
							}
							Console.Error("@@SYSCALL_HANDLER@@ MIPS[%08x]: %s", handler, ibuf);
						}
					}
				}
			}
			// 6) Dump caller code at 0x1DFD70 (who called syscall 0x64)
			{
				char ibuf[512] = {};
				int p = 0;
				for (int k = -8; k < 16 && p < 490; k++) {
					u32 insn = *(u32*)(eeMem->Main + 0x1DFD70 + k * 4);
					p += snprintf(ibuf + p, 512 - p, "%08x ", insn);
				}
				Console.Error("@@CALLER_1DFD70@@ MIPS[-8..+16]: %s", ibuf);
			}
		}
	}
}
#endif // BlockWatchpoint220A8

static void recEventTest()
{
	s_recEventTest_cnt.fetch_add(1, std::memory_order_relaxed);

	_cpuEventTest_Shared();

	if (eeRecExitRequested)
	{
		eeRecExitRequested = false;
		recExitExecution();
	}
}

// The address for all cleared blocks.  It recompiles the current pc and then
// dispatches to the recompiled block address.
// [iPSX2] Fix 003 Proof Helper

static const void* _DynGen_JITCompile()
{
	pxAssertMsg(DispatcherReg != NULL, "Please compile the DispatcherReg subroutine *before* JITComple.  Thanks.");

//	u8* retval = xGetAlignedCallTarget();
    armAlignAsmPtr();
    u8* retval = armGetCurrentCodePointer();

//	xFastCall((const void*)recRecompile, ptr32[&cpuRegs.pc]);
    armLoad(EAX, PTR_CPU(cpuRegs.pc));
    
    // Protect JIT state registers
    // [iPSX2] Fix 006: Explicit STP Save (Safe Order)
    armAsm->Stp(a64::x19, a64::x20, a64::MemOperand(a64::sp, -16, a64::PreIndex));
    armAsm->Stp(a64::x21, a64::x22, a64::MemOperand(a64::sp, -16, a64::PreIndex));
    armAsm->Stp(a64::x23, a64::x24, a64::MemOperand(a64::sp, -16, a64::PreIndex));
    armAsm->Stp(a64::x25, a64::x26, a64::MemOperand(a64::sp, -16, a64::PreIndex));
    armAsm->Stp(a64::x27, a64::x28, a64::MemOperand(a64::sp, -16, a64::PreIndex));
    armAsm->Stp(a64::x29, a64::lr, a64::MemOperand(a64::sp, -16, a64::PreIndex));

    armEmitCall(reinterpret_cast<const void*>(recRecompile));
    
    // [iPSX2] Fix 003: Restore callee-saved regs (LIFO order)
    // [iPSX2] Fix 006: Explicit LDP Restore (Safe Order)
    // Resolves Swap Hazard: Pop(lr, x29) was loading x29->lr, lr->x29. Ldp(x29, lr) is correct.
    armAsm->Ldp(a64::x29, a64::lr, a64::MemOperand(a64::sp, 16, a64::PostIndex));
    armAsm->Ldp(a64::x27, a64::x28, a64::MemOperand(a64::sp, 16, a64::PostIndex));
    armAsm->Ldp(a64::x25, a64::x26, a64::MemOperand(a64::sp, 16, a64::PostIndex));
    armAsm->Ldp(a64::x23, a64::x24, a64::MemOperand(a64::sp, 16, a64::PostIndex));
    armAsm->Ldp(a64::x21, a64::x22, a64::MemOperand(a64::sp, 16, a64::PostIndex));
    armAsm->Ldp(a64::x19, a64::x20, a64::MemOperand(a64::sp, 16, a64::PostIndex));
    
    // armAsm->Mov(a64::x16, (uintptr_t)recRecompile);
    // armAsm->Blr(a64::x16);
    
    // Add logging to verify we returned from recRecompile

    // Re-ensure RSTATEs are pointing to the right places after possible clobbering
    armMoveAddressToReg(RSTATE_x29, &recLUT);
    armMoveAddressToReg(RSTATE_PSX, &psxRegs);
    armMoveAddressToReg(RSTATE_CPU, &g_cpuRegistersPack);
    if (CHECK_FASTMEM) {
        armAsm->Ldr(RFASTMEMBASE, PTR_CPU(vtlbdata.fastmem_base));
    }

    // Transfer to DispatcherReg to dispatch the compiled block
    armEmitJmp(DispatcherReg);

	return retval;
}

// called when jumping to variable pc address
// Forward declarations for fallback helpers
extern "C" void recLogDispatcherFallback(u32 pc);
extern "C" uint64_t recDispatchResolve(u32 pc);
extern "C" void intExecuteInstruction();
extern "C" void interpExecBiosBlock();  // [iter222] BIOS ROM partial interpreter

// [TEMP_DIAG] @@FPU_DIAG@@ Dump FPU registers at buggy block entry for JIT vs Interpreter comparison
// Removal condition: JIT FPU bug in blocks 0x219760/0x2197c4 identified and fixed
extern "C" void fpuDiagDump()
{
	static int s_cnt = 0;
	if (s_cnt >= 100) return;
	s_cnt++;
	const u32 pc = cpuRegs.pc;
	Console.WriteLn("@@FPU_DIAG@@ pc=%08x n=%d f0=%08x f1=%08x f2=%08x f20=%08x f21=%08x fcr31=%08x",
		pc, s_cnt,
		fpuRegs.fpr[0].UL, fpuRegs.fpr[1].UL, fpuRegs.fpr[2].UL,
		fpuRegs.fpr[20].UL, fpuRegs.fpr[21].UL, fpuRegs.fprc[31]);
}
extern "C" void recLogDispatcherWithTarget(u64 pc, u64 target);
extern "C" void recLogEventHit(u32 pc, s32 cycle);
extern "C" void recLogDispPcDiag(u32 reg_pc, u32 mem_pc, u64 target);
extern "C" void recLogLutRead(u32 reg_pc, u32 mem_pc, u64 target, u32 page_idx);
extern "C" void recLogLutAddr(u32 pc, u32 page_idx, u32 idx, u64 slot);
extern "C" void recLogNextPcProbe(u32 site_bit, u32 cur_pc, u32 pc_key, u32 extra0, u32 extra1);
extern "C" void recLogPostNextPath(u32 pc_key);
extern "C" void recLogRealPcCommit(u32 pc, u32 site);
extern "C" void recLogDispatcherPc(u32 pc_val, u32 ra_val, u32 code_val, u32 sp_val);

extern "C" void recLogDispatcherPc(u32 pc_val, u32 ra_val, u32 code_val, u32 sp_val)
{
	static int s_cfg = -1;
	static int s_count = 0;
	constexpr int kCap = 400;
	if (s_cfg < 0)
	{
		s_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_DISPATCH_PC_PROBE", false) ? 1 : 0;
		if (s_cfg)
			Console.WriteLn("@@CFG@@ iPSX2_DISPATCH_PC_PROBE=1 iPSX2_DISPATCH_PC_PROBE_CAP=%d", kCap);
	}
	if (!s_cfg || s_count >= kCap)
		return;

	u32 opc = 0xffffffffu;
	if (const u32* const p = reinterpret_cast<const u32*>(PSM(pc_val)))
		opc = *p;

	Console.WriteLn("@@DISPATCH_PC_PROBE@@ idx=%d pc=%08x ra=%08x sp=%08x code=%08x opc=%08x",
		s_count, pc_val, ra_val, sp_val, code_val, opc);
	s_count++;
}

// [TEMP_DIAG] @@DISPATCH_CNT@@ track recDispatchResolve calls
static std::atomic<uint64_t> s_dispatchResolve_cnt{0};
// [TEMP_DIAG] @@RECOMPILE_CNT@@ track recRecompile calls
static std::atomic<uint64_t> s_recompile_cnt{0};

// [R59] fast dispatcher からの直近の dispatch PC を追跡
// @@LAST_DISPATCH_PC@@ Removal condition: 0x22000000 ジャンプ元after identified
static u32 g_last_dispatch_pc = 0;
static u32 g_prev_dispatch_pc = 0;

// [R59] Last SYSCALL tracking (defined in R5900OpcodeImpl.cpp)
extern u32 g_last_syscall_pc;
extern u32 g_last_syscall_call;
extern u32 g_last_syscall_a0;
extern u32 g_last_syscall_ra;
extern u32 g_last_syscall_cycle;

struct SyscallEntry { u32 pc, call, a0, ra, v1_raw, cycle; };
extern SyscallEntry g_syscall_ring[];
extern int g_syscall_ring_idx;
static constexpr int SYSCALL_RING_SIZE = 32;

extern "C" uint64_t recDispatchResolve(u32 pc)
{
	s_dispatchResolve_cnt.fetch_add(1, std::memory_order_relaxed);

	// [R59] @@PC_HISTORY@@ ring buffer — dump last 16 dispatches when unmapped PC hit
	// Removal condition: 0x22000000 ジャンプ元after identified
	{
		static constexpr int PC_HIST_SIZE = 16;
		static u32 s_pc_hist[PC_HIST_SIZE] = {};
		static int  s_pc_hist_idx = 0;
		s_pc_hist[s_pc_hist_idx & (PC_HIST_SIZE - 1)] = pc;
		s_pc_hist_idx++;

		// If this pc is unmapped (0x22xxxxxx range specifically), dump the ring buffer
		if ((pc >> 16) == 0x2200) {
			static int s_hist_dump_cnt = 0;
			if (s_hist_dump_cnt++ < 3) {
				Console.WriteLn("@@PC_HISTORY@@ unmapped pc=%08x reached. g_last_dispatch_pc=%08x last_syscall: pc=%08x call=%02x a0=%08x ra=%08x cycle=%u",
					pc, g_last_dispatch_pc,
					g_last_syscall_pc, g_last_syscall_call,
					g_last_syscall_a0, g_last_syscall_ra, g_last_syscall_cycle);
				Console.WriteLn("@@PC_HISTORY@@ Last %d dispatches via recDispatchResolve (newest first):", PC_HIST_SIZE);
				for (int i = 1; i <= PC_HIST_SIZE; i++) {
					int idx = (s_pc_hist_idx - 1 - i) & (PC_HIST_SIZE - 1);
					Console.WriteLn("@@PC_HISTORY@@  [-%d] pc=%08x", i, s_pc_hist[idx]);
				}
				Console.WriteLn("@@PC_HISTORY@@ ra=%08x epc=%08x cause=%08x k0=%08x k1=%08x",
					cpuRegs.GPR.r[31].UL[0], cpuRegs.CP0.r[14],
					cpuRegs.CP0.r[13], cpuRegs.GPR.r[26].UL[0], cpuRegs.GPR.r[27].UL[0]);
				// [R59] register全ダンプ + memoryダンプ
				Console.WriteLn("@@PC_HISTORY@@ v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x a3=%08x t9=%08x sp=%08x",
					cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[3].UL[0],
					cpuRegs.GPR.r[4].UL[0], cpuRegs.GPR.r[5].UL[0],
					cpuRegs.GPR.r[6].UL[0], cpuRegs.GPR.r[7].UL[0],
					cpuRegs.GPR.r[25].UL[0], cpuRegs.GPR.r[29].UL[0]);
				Console.WriteLn("@@PC_HISTORY@@ gp=%08x s8=%08x status=%08x",
					cpuRegs.GPR.r[28].UL[0], cpuRegs.GPR.r[30].UL[0],
					cpuRegs.CP0.r[12]);
				if (eeMem) {
					const u32* eel = reinterpret_cast<const u32*>(eeMem->Main + 0x82000);
					Console.WriteLn("@@PC_HISTORY@@ EELOAD[0x82000]: %08x %08x %08x %08x %08x %08x %08x %08x",
						eel[0], eel[1], eel[2], eel[3], eel[4], eel[5], eel[6], eel[7]);
					const u32* sc = reinterpret_cast<const u32*>(eeMem->Main + 0x000280);
					Console.WriteLn("@@PC_HISTORY@@ SYSCALL_VEC[0x280]: %08x %08x %08x %08x %08x %08x %08x %08x",
						sc[0], sc[1], sc[2], sc[3], sc[4], sc[5], sc[6], sc[7]);
					const u32* ra_area = reinterpret_cast<const u32*>(eeMem->Main + 0x000300);
					Console.WriteLn("@@PC_HISTORY@@ MEM[0x300]: %08x %08x %08x %08x %08x %08x %08x %08x",
						ra_area[0], ra_area[1], ra_area[2], ra_area[3], ra_area[4], ra_area[5], ra_area[6], ra_area[7]);
					// BIOS ROM at bfc008c0-bfc00960 (offset 0x8C0-0x960 in ROM)
					const u32* bios_8c0 = reinterpret_cast<const u32*>(eeMem->ROM + 0x8C0);
					for (int row = 0; row < 5; row++) {
						const u32* p = bios_8c0 + row * 8;
						Console.WriteLn("@@PC_HISTORY@@ BIOS[bfc0%04x]: %08x %08x %08x %08x %08x %08x %08x %08x",
							0x08C0 + row * 0x20, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
					}
					// Stack dump at sp=0x80018e70 → phys 0x18e70
					u32 sp_phys = cpuRegs.GPR.r[29].UL[0] & 0x1FFFFFFF;
					if (sp_phys + 0x20 <= 0x02000000) {
						const u32* stk = reinterpret_cast<const u32*>(eeMem->Main + sp_phys);
						Console.WriteLn("@@PC_HISTORY@@ STACK[sp=%08x phys=%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
							cpuRegs.GPR.r[29].UL[0], sp_phys,
							stk[0], stk[1], stk[2], stk[3], stk[4], stk[5], stk[6], stk[7]);
					}
				}
				// Dump syscall ring buffer (last 32 syscalls)
				Console.WriteLn("@@SYSCALL_RING@@ Last %d syscalls (newest first, idx=%d):", SYSCALL_RING_SIZE, g_syscall_ring_idx);
				for (int i = 1; i <= SYSCALL_RING_SIZE; i++) {
					int idx = (g_syscall_ring_idx - i) & (SYSCALL_RING_SIZE - 1);
					auto& e = g_syscall_ring[idx];
					if (e.cycle == 0) break; // empty entry
					Console.WriteLn("@@SYSCALL_RING@@  [-%d] pc=%08x call=%02x v1=%08x a0=%08x ra=%08x cycle=%u",
						i, e.pc, e.call, e.v1_raw, e.a0, e.ra, e.cycle);
				}
				// Dump OSDSYS function body 0x208500-0x208700 (full function + context)
				if (eeMem) {
					for (u32 base = 0x208500; base < 0x208700; base += 0x20) {
						const u32* p = reinterpret_cast<const u32*>(eeMem->Main + base);
						Console.WriteLn("@@OSDSYS_CODE@@ [0x%06x]: %08x %08x %08x %08x %08x %08x %08x %08x",
							base, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
					}
					// Also dump the syscall stub at 0x081fc0-0x082020
					for (u32 base = 0x081fc0; base < 0x082020; base += 0x20) {
						const u32* p = reinterpret_cast<const u32*>(eeMem->Main + base);
						Console.WriteLn("@@STUB_CODE@@ [0x%06x]: %08x %08x %08x %08x %08x %08x %08x %08x",
							base, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
					}
					// Dump caller's code: ra loaded from stack at sp+0x40
					// At epilogue: ld ra, 0x40(sp) before sp+=0x50
					// Current sp after warm reboot may be different, but try to find
					// the return address from the function at 0x208654
					// The stack pointer at function entry was sp_current - 0x50 (already restored)
					// We can't get the exact caller, but dump likely caller areas
					// Also dump code at 0x081f80-0x082020 for full stub context
				}
			}
		}
	}

	// [iter43] One-shot: capture first dispatch into EE KUSEG RAM (BIOS→OS jump).
	// Removal condition: BIOS から EE RAM への JR/JALR 元 PC と理由after determined。
	{
		static bool s_eeram_first = false;
		if (!s_eeram_first && pc >= 0x00100000u && pc < 0x02000000u) {
			s_eeram_first = true;
			Console.WriteLn("@@EERAM_FIRST_DISPATCH@@ pc=%08x ra=%08x a0=%08x a1=%08x v0=%08x k0=%08x k1=%08x sp=%08x",
				pc, cpuRegs.GPR.r[31].UL[0], cpuRegs.GPR.r[4].UL[0], cpuRegs.GPR.r[5].UL[0],
				cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[26].UL[0], cpuRegs.GPR.r[27].UL[0],
				cpuRegs.GPR.r[29].UL[0]);
		}
	}

	// [R59] @@KERN_ENTRY@@ kernel entry (0x80001000) dispatch tracking
	// Removal condition: warm reboot divergence after identified
	{
		static int s_kern_entry_cnt = 0;
		if (pc == 0x80001000u) {
			s_kern_entry_cnt++;
			Console.WriteLn("@@KERN_ENTRY@@ n=%d pc=%08x ra=%08x sp=%08x status=%08x epc=%08x cause=%08x cycle=%u",
				s_kern_entry_cnt, pc,
				cpuRegs.GPR.r[31].UL[0], cpuRegs.GPR.r[29].UL[0],
				cpuRegs.CP0.r[12], cpuRegs.CP0.r[14], cpuRegs.CP0.r[13],
				cpuRegs.cycle);
			if (eeMem) {
				// Dump kernel code at 0x80001000 → phys 0x1000
				const u32* kern = reinterpret_cast<const u32*>(eeMem->Main + 0x1000);
				Console.WriteLn("@@KERN_ENTRY@@ CODE[0x1000]: %08x %08x %08x %08x %08x %08x %08x %08x",
					kern[0], kern[1], kern[2], kern[3], kern[4], kern[5], kern[6], kern[7]);
				Console.WriteLn("@@KERN_ENTRY@@ CODE[0x1020]: %08x %08x %08x %08x %08x %08x %08x %08x",
					kern[8], kern[9], kern[10], kern[11], kern[12], kern[13], kern[14], kern[15]);
			}
		}
		// Also track dispatches to 0x8000xxxx range after warm reboot (cycle > 196M)
		if ((pc >> 16) == 0x8000 && cpuRegs.cycle > 196000000u) {
			static int s_kern_wr_cnt = 0;
			if (s_kern_wr_cnt++ < 50) {
				Console.WriteLn("@@KERN_WARMREBOOT@@ n=%d pc=%08x ra=%08x sp=%08x status=%08x cycle=%u",
					s_kern_wr_cnt, pc,
					cpuRegs.GPR.r[31].UL[0], cpuRegs.GPR.r[29].UL[0],
					cpuRegs.CP0.r[12], cpuRegs.cycle);
			}
		}
	}
	const auto calc_slot = [](u32 curpc) -> u64 {
		const uptr page = recLUT[curpc >> 16];
		const u64 idx = ((u64)curpc >> 2);
		return (u64)page + (idx << 3);
	};

	auto resolve_target = [&](u32 curpc) -> u64 {
		u64 slot = calc_slot(curpc);
		// Unmapped recLUT sentinel collapses to a small non-host address (often 0x0..0x1fffe).
		if ((slot >> 32) == 0)
			return 0;
		return *(const u64*)slot;
	};

	u64 target = resolve_target(pc);

	// [R58] TLB-mapped page の動的 recLUT セットアップ。
	// recLUT はboot時に直接マップ (0x0000-0x01FF の 32MB) しかconfigしないため、
	// カーネルが TLB で仮想 0x22000000 等を物理 RAM にマップしても recLUT は sentinel のまま。
	// vtlb の vmap がenabledmappingを持っている場合、recLUT を動的にconfigして再試行する。
	if (!target)
	{
		using namespace vtlb_private;
		const u32 vpage4k = pc >> VTLB_PAGE_BITS; // 4KB page index
		auto vmv = vtlbdata.vmap[vpage4k];
		if (!vmv.isHandler(pc)) {
			// vtlb has a valid pointer mapping for this virtual address
			uptr host_ptr = vmv.assumePtr(pc & ~VTLB_PAGE_MASK);
			uptr ee_main = (uptr)eeMem->Main;
			if (host_ptr >= ee_main && host_ptr < ee_main + Ps2MemSize::ExposedRam) {
				u32 phys_offset = (u32)(host_ptr - ee_main);
				u32 phys_page64k = phys_offset >> 16; // 64KB page in recRAM
				u32 virt_page64k = pc >> 16;           // 64KB virtual page
				// Set up recLUT for this TLB-mapped virtual page → physical RAM
				recLUT_SetPage(recLUT, nullptr, recRAM, 0, virt_page64k, phys_page64k);
				static int s_tlb_reclut_cnt = 0;
				if (s_tlb_reclut_cnt++ < 50)
					Console.WriteLn("@@TLB_RECLUT@@ n=%d pc=%08x vpage=0x%04x -> phys_page=0x%04x (phys_offset=0x%08x)",
						s_tlb_reclut_cnt, pc, virt_page64k, phys_page64k, phys_offset);
				// Retry resolve after recLUT setup
				target = resolve_target(pc);
				if (target) {
					if (target == (u64)JITCompile) {
						recRecompile(pc);
						pc = cpuRegs.pc;
						target = resolve_target(pc);
						if (target)
							return target;
					} else {
						return target;
					}
				}
			}
		}
	}

	if (!target)
	{
		// [R57] Unmapped PC handling — vtlb にもmappingなし。
		// TLB Refill 例外シミュレーションまたは EELOAD 回復。
		{
			static int s_unmapped_cnt = 0;
			if (s_unmapped_cnt++ < 20) {
				Console.WriteLn("@@UNMAPPED_PC_TLBMISS@@ n=%d pc=%08x ra=%08x epc=%08x cause=%08x status=%08x cycle=%u",
					s_unmapped_cnt, pc,
					cpuRegs.GPR.r[31].UL[0], cpuRegs.CP0.r[14],
					cpuRegs.CP0.r[13], cpuRegs.CP0.r[12], cpuRegs.cycle);
				// [R58] TLB 全48エントリダンプ（0x22000000 mappingverify用）
				u32 target_vpn2 = (pc >> 13) & 0x7FFFF; // VPN2 for the target address
				Console.WriteLn("@@TLB_DUMP@@ target_vpn2=0x%05x (for pc=%08x):", target_vpn2, pc);
				for (int ti = 0; ti < 48; ti++) {
					if (tlb[ti].EntryLo0.V || tlb[ti].EntryLo1.V) {
						Console.WriteLn("@@TLB_DUMP@@ [%2d] VPN2=%08x Mask=%05x PFN0=%08x(V=%d D=%d) PFN1=%08x(V=%d D=%d) ASID=%02x G=%d",
							ti, tlb[ti].VPN2(), tlb[ti].Mask(),
							tlb[ti].PFN0(), tlb[ti].EntryLo0.V, tlb[ti].EntryLo0.D,
							tlb[ti].PFN1(), tlb[ti].EntryLo1.V, tlb[ti].EntryLo1.D,
							tlb[ti].EntryHi.ASID, tlb[ti].isGlobal() ? 1 : 0);
					}
				}
			}

			// [R57] EELOAD warm reboot recovery
			u32 epc = cpuRegs.CP0.r[14];
			bool eeload_recovered = false;
			if (eeMem) {
				const u32* eel = reinterpret_cast<const u32*>(eeMem->Main + 0x82000);
				bool eel_zeroed = true;
				for (int i = 0; i < 8; i++) { if (eel[i] != 0) { eel_zeroed = false; break; } }
				if (eel_zeroed) {
					BiosResetEeloadCopyFlag();
					if (BiosRetriggerEeloadCopy()) {
						Console.WriteLn("@@EELOAD_WARMREBOOT_RECOPY@@ recovered EELOAD from ROM at unmapped dispatch (pc=%08x epc=%08x)",
							pc, epc);
						cpuRegs.pc = epc;
						eeload_recovered = true;
					}
				}
			}
			if (!eeload_recovered) {
				cpuRegs.pc = pc + 4;
				cpuTlbMissR(pc, 0);
			}
		}
		return (uint64_t)DispatcherEvent;
	}

	if (target != (u64)JITCompile)
		return target;

	recRecompile(pc);
	pc = cpuRegs.pc;
	target = resolve_target(pc);
	if (target)
		return target;

	return (uint64_t)DispatcherEvent;
}

// =====================================================
// Custom Logger for Dispatcher Debugging
// =====================================================

// =====================================================
// Android-style DispatcherReg - SIMPLE PC→lookup→BR
// =====================================================
// called when jumping to variable pc address
static const void* _DynGen_DispatcherReg()
{
    u8* retval = armGetCurrentCodePointer();
	static int s_dispatch_pc_probe_cfg = -1;
	if (s_dispatch_pc_probe_cfg < 0)
	{
		s_dispatch_pc_probe_cfg = (std::getenv("iPSX2_DISPATCH_PC_PROBE") != nullptr) ? 1 : 0;
		if (s_dispatch_pc_probe_cfg)
			Console.WriteLn("@@CFG@@ iPSX2_DISPATCH_PC_PROBE=1");
	}


    armLoad(EAX, PTR_CPU(cpuRegs.pc));
	if (s_dispatch_pc_probe_cfg)
	{
		armAsm->Mov(a64::w0, EAX);
		armLoad(a64::w1, PTR_CPU(cpuRegs.GPR.n.ra.UL[0]));
		armLoad(a64::w2, PTR_CPU(cpuRegs.code));
		armLoad(a64::w3, PTR_CPU(cpuRegs.GPR.n.sp.UL[0]));
		armEmitCall(reinterpret_cast<const void*>(recLogDispatcherPc));
		armLoad(EAX, PTR_CPU(cpuRegs.pc));
	}
    if (IsDispatchNegL1SlowpathEnabled())
    {
        armAsm->Mov(a64::w0, EAX);
        armEmitCall(reinterpret_cast<const void*>(recDispatchResolve));
        armAsm->Br(RAX);
        return retval;
    }
    ////
    armAsm->Lsr(ECX, EAX, 16);
    armAsm->Ldr(RCX, a64::MemOperand(RSTATE_x29, RCX, a64::LSL, 3));
    // [iter38] Sentinel check: recLUT page sentinel has bit-63 set (negative value).
    // Without this, EE PC in unmapped page (e.g. 0x02000000) causes null-ptr SIGSEGV
    // in the level-2 load below. Fall back to recDispatchResolve which handles it safely.
    // Removal condition: not needed。これは DispatcherReg のrequired安全チェック。
    {
        a64::Label armsx2_l1_fast;
        armAsm->Tbz(RCX, 63, &armsx2_l1_fast);
        // EAX (w0) still holds original EE PC (loaded above at armLoad(EAX, PTR_CPU(cpuRegs.pc)))
        armEmitCall(reinterpret_cast<const void*>(recDispatchResolve));
        armAsm->Br(RAX);
        armAsm->Bind(&armsx2_l1_fast);
    }
    // [R59] Store last dispatched PC on fast path only (before block jump)
    // Save previous dispatch PC first, so NOP_SLED can see the CALLER block.
    {
		armAsm->Mov(a64::x16, reinterpret_cast<uintptr_t>(&g_last_dispatch_pc));
		armAsm->Ldr(a64::w17, a64::MemOperand(a64::x16));
		armAsm->Mov(a64::x15, reinterpret_cast<uintptr_t>(&g_prev_dispatch_pc));
		armAsm->Str(a64::w17, a64::MemOperand(a64::x15));
		armAsm->Str(EAX, a64::MemOperand(a64::x16));
    }
    ////
    armAsm->Lsr(EAX, EAX, 2);
    armAsm->Ldr(RAX, a64::MemOperand(RCX, EAX, a64::LSL, 3));
    ////
    armAsm->Br(RAX);

    return retval;
}

// =====================================================
// Android-style DispatcherEvent - call recEventTest, then dispatch
// =====================================================
static const void* _DynGen_DispatcherEvent()
{
    u8* retval = armGetCurrentCodePointer();

    // Event test call can clobber state registers via host call paths.
    // Re-materialize dynarec state bases before falling through to DispatcherReg.
    armEmitCall(reinterpret_cast<const void*>(recEventTest));
    armMoveAddressToReg(RSTATE_x29, &recLUT);
    armMoveAddressToReg(RSTATE_PSX, &psxRegs);
    armMoveAddressToReg(RSTATE_CPU, &g_cpuRegistersPack);
    if (CHECK_FASTMEM)
        armAsm->Ldr(RFASTMEMBASE, PTR_CPU(vtlbdata.fastmem_base));

    return retval;
}


static const void* _DynGen_EnterRecompiledCode()
{
	pxAssertMsg(DispatcherReg, "Dynamically generated dispatchers are required prior to generating EnterRecompiledCode!");

//	u8* retval = xGetAlignedCallTarget();
    armAlignAsmPtr();
    u8* retval = armGetCurrentCodePointer();
    
#ifdef ENABLE_VTUNE
	xScopedStackFrame frame(true, true);
#else
#ifdef _WIN32
	// Shadow space for Win32
	static constexpr u32 stack_size = 32 + 8;
#else
	// Stack still needs to be aligned
	static constexpr u32 stack_size = 16;
#endif

	// We never return through this function, instead we fastjmp() out.
	// So we don't need to worry about preserving callee-saved registers, but we do need to align the stack.
//	xSUB(rsp, stack_size);
    armAsm->Sub(a64::sp, a64::sp, stack_size);
#endif

    // From memory to registry
    armMoveAddressToReg(RSTATE_x29, &recLUT);
    armMoveAddressToReg(RSTATE_PSX, &psxRegs);
    armMoveAddressToReg(RSTATE_CPU, &g_cpuRegistersPack);

	if (CHECK_FASTMEM) {
//        xMOV(RFASTMEMBASE, ptrNative[&vtlb_private::vtlbdata.fastmem_base]);
        armAsm->Ldr(RFASTMEMBASE, PTR_CPU(vtlbdata.fastmem_base));
    }

    // [ARM64] Initialize FPCR for EE FPU operations.
    // PS2 EE FPU requires: FlushToZero=1, DenormalsAreZero=1, RoundMode=ChopZero.
    // Without this, JIT FPU ops run with host defaults (FZ=0, Round=Nearest),
    // causing denormal handling differences vs interpreter (fpuDouble).
    armAsm->Msr(a64::FPCR, armLoad64(PTR_CPU(Cpu.FPUFPCR.bitmask)));

//	xJMP(DispatcherReg);
    armEmitJmp(DispatcherReg);

	return retval;
}

static const void* _DynGen_DispatchBlockDiscard()
{
//	u8* retval = xGetPtr();
    u8* retval = armGetCurrentCodePointer();
//	xFastCall((const void*)dyna_block_discard);
    armEmitCall(reinterpret_cast<const void*>(dyna_block_discard));


//	xJMP(DispatcherReg);
    armEmitJmp(DispatcherReg);
	return retval;
}

static const void* _DynGen_DispatchPageReset()
{
//	u8* retval = xGetPtr();
    u8* retval = armGetCurrentCodePointer();
//	xFastCall((const void*)dyna_page_reset);
    armEmitCall(reinterpret_cast<const void*>(dyna_page_reset));
//	xJMP(DispatcherReg);
    armEmitJmp(DispatcherReg);
	return retval;
}

static void _DynGen_Dispatchers()
{
//	const u8* start = xGetAlignedCallTarget();
    const u8* start = armGetCurrentCodePointer();

	// Place the EventTest and DispatcherReg stuff at the top, because they get called the
	// most and stand to benefit from strong alignment and direct referencing.
	DispatcherEvent = _DynGen_DispatcherEvent();
	DispatcherReg = _DynGen_DispatcherReg();

	JITCompile = _DynGen_JITCompile();

	EnterRecompiledCode = _DynGen_EnterRecompiledCode();
	DispatchBlockDiscard = _DynGen_DispatchBlockDiscard();
	DispatchPageReset = _DynGen_DispatchPageReset();

	recBlocks.SetJITCompile(JITCompile);

	// [TEMP_DIAG] JIT arena先頭クラッシュ(host_pc=jbase+0x50)がどのdispatcher stub内かを
	// pcsx2_log.txt だけで特定するため、dispatcher系エントリポイントを1回だけ出力する。
	// Removal condition: crash host_pc と DispatcherReg/EnterRecompiledCode の相対位置が確定した時点。
	static int s_dispatch_ptrs_cfg = -1;
	static int s_dispatch_ptrs_dumped = 0;
	if (s_dispatch_ptrs_cfg < 0)
	{
		s_dispatch_ptrs_cfg = (std::getenv("iPSX2_DISPATCH_PTRS_DIAG") != nullptr) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_DISPATCH_PTRS_DIAG=%d", s_dispatch_ptrs_cfg);
		Console.WriteLn("@@CFG@@ iPSX2_DISPATCH_NEG_L1_SLOWPATH=%d", IsDispatchNegL1SlowpathEnabled() ? 1 : 0);
	}
	if (s_dispatch_ptrs_cfg && !s_dispatch_ptrs_dumped)
	{
		s_dispatch_ptrs_dumped = 1;
		Console.WriteLn("[TEMP_DIAG] @@DISPATCH_PTRS@@ base=%p DispatcherEvent=%p DispatcherReg=%p JITCompile=%p EnterRecompiledCode=%p",
			SysMemory::GetEERec(), DispatcherEvent, DispatcherReg, JITCompile, EnterRecompiledCode);
	}

	Perf::any.Register(start, static_cast<u32>(armGetCurrentCodePointer() - start), "EE Dispatcher");
}


//////////////////////////////////////////////////////////////////////////////////////////
//

static __ri void ClearRecLUT(BASEBLOCK* base, int memsize)
{
	for (int i = 0; i < memsize / (int)sizeof(uptr); i++)
		base[i].SetFnptr((uptr)JITCompile);
}

static void recReserveRAM()
{
	recLutSize = (Ps2MemSize::ExposedRam + Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2) * wordsize / 4;

	if (recRAMCopy.size() != Ps2MemSize::ExposedRam)
		recRAMCopy.resize(Ps2MemSize::ExposedRam);

	if (recLutReserve_RAM.size() != recLutSize)
		recLutReserve_RAM.resize(recLutSize);

	BASEBLOCK* basepos = reinterpret_cast<BASEBLOCK*>(recLutReserve_RAM.data());
	recRAM = basepos;
	basepos += (Ps2MemSize::ExposedRam / 4);
	recROM = basepos;
	basepos += (Ps2MemSize::Rom / 4);
	recROM1 = basepos;
	basepos += (Ps2MemSize::Rom1 / 4);
	recROM2 = basepos;
	basepos += (Ps2MemSize::Rom2 / 4);

	for (int i = 0; i < 0x10000; i++)
		recLUT_SetPage(recLUT, 0, 0, 0, i, 0);

	for (int i = 0x0000; i < (int)(Ps2MemSize::ExposedRam / 0x10000); i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x0000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x2000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x3000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x8000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xa000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xb000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xc000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xd000, i, i);
	}

	for (int i = 0x1fc0; i < 0x2000; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM, 0x0000, i, i - 0x1fc0);
		recLUT_SetPage(recLUT, hwLUT, recROM, 0x8000, i, i - 0x1fc0);
		recLUT_SetPage(recLUT, hwLUT, recROM, 0xa000, i, i - 0x1fc0);
	}

	for (int i = 0x1e00; i < 0x1e40; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0x0000, i, i - 0x1e00);
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0x8000, i, i - 0x1e00);
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0xa000, i, i - 0x1e00);
	}

	for (int i = 0x1e40; i < 0x1e80; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0x0000, i, i - 0x1e40);
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0x8000, i, i - 0x1e40);
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0xa000, i, i - 0x1e40);
	}
}

static void recReserve()
{
	recPtr = SysMemory::GetEERec();
	recPtrEnd = SysMemory::GetEERecEnd() - _64kb;
	recReserveRAM();

	pxAssertRel(!s_pInstCache, "InstCache not allocated");
	s_nInstCacheSize = 128;
	s_pInstCache = (EEINST*)malloc(sizeof(EEINST) * s_nInstCacheSize);
	if (!s_pInstCache)
		pxFailRel("Failed to allocate R5900 InstCache array");
}

alignas(16) static u16 manual_page[Ps2MemSize::TotalRam >> 12];
alignas(16) static u8 manual_counter[Ps2MemSize::TotalRam >> 12];

////////////////////////////////////////////////////
static void recResetRaw()
{
	Console.WriteLn(Color_StrongBlack, "EE/iR5900 Recompiler Reset");

	if (CHECK_EXTRAMEM != extraRam)
	{
		recReserveRAM();
		extraRam = !extraRam;
	}

	EE::Profiler.Reset();

	Console.WriteLn(Color_StrongBlack, "EE: iR5900: recResetRaw starting...");
    armSetAsmPtr(SysMemory::GetEERec(), _4kb, nullptr);
    armStartBlock();

	Console.WriteLn(Color_StrongBlack, "EE: iR5900: Generating dispatchers...");
	_DynGen_Dispatchers();

    // recVTLB => iR5900LoadStore
	Console.WriteLn(Color_StrongBlack, "EE: iR5900: Generating VTLB dispatchers...");
    
#ifdef PCSX2_VTLB_DISPATCH_ENABLE
    vtlb_DynGenDispatchers();
    Console.WriteLn("@@VTLB_DISPATCH@@ Enabled. g_vtlb_dispatcher_base=%p", 
        g_vtlb_dispatcher_base.load(std::memory_order_acquire));
#else
    // vtlb_DynGenDispatchers(); // DISABLED for Hybrid Compatibility (Force C++ Handlers)
    Console.WriteLn("@@VTLB_DISPATCH@@ Disabled (PCSX2_VTLB_DISPATCH_ENABLE not defined)");
#endif

	Console.WriteLn(Color_StrongBlack, "EE: iR5900: Ending recompiler block...");
//	recPtr = xGetPtr();
    recPtr = armEndBlock();
	Console.WriteLn(Color_StrongBlack, "EE: iR5900: recResetRaw complete.");

	ClearRecLUT(reinterpret_cast<BASEBLOCK*>(recLutReserve_RAM.data()), recLutSize);
	recRAMCopy.fill(0);

	maxrecmem = 0;

	if (s_pInstCache)
		memset(s_pInstCache, 0, sizeof(EEINST) * s_nInstCacheSize);

	recBlocks.Reset();
	vtlb_ClearLoadStoreInfo();

	g_branch = 0;
	g_resetEeScalingStats = true;

	memset(manual_page, 0, sizeof(manual_page));
	memset(manual_counter, 0, sizeof(manual_counter));
	s_ever_compiled_vpcs.clear(); // [TEMP_DIAG] reset persistent set on full cache reset
}

void recShutdown()
{
	recRAMCopy.deallocate();
	recLutReserve_RAM.deallocate();

	recBlocks.Reset();

	recRAM = recROM = recROM1 = recROM2 = nullptr;

	safe_free(s_pInstCache);
	s_nInstCacheSize = 0;

	recPtr = nullptr;
	recPtrEnd = nullptr;
}

void recStep()
{
}

static fastjmp_buf m_SetJmp_StateCheck;

static void recExitExecution()
{
	fastjmp_jmp(&m_SetJmp_StateCheck, 1);
}

static void recSafeExitExecution()
{
	// If we're currently processing events, we can't safely jump out of the recompiler here, because we'll
	// leave things in an inconsistent state. So instead, we flag it for exiting once cpuEventTest() returns.
	// Exiting in the middle of a rec block with the registers unsaved would be a bad idea too..
	eeRecExitRequested = true;

	// Force an event test at the end of this block.
	if (!eeEventTestIsActive)
	{
		// EE is running.
		cpuRegs.nextEventCycle = 0;
	}
	else
	{
		// IOP might be running, so break out if so.
		if (psxRegs.iopCycleEE > 0)
		{
			psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
			psxRegs.iopCycleEE = 0;
		}
	}
}

static void recResetEE()
{
	if (eeCpuExecuting)
	{
		// get outta here as soon as we can
		eeRecNeedsReset = true;
		recSafeExitExecution();
		return;
	}

	recResetRaw();
}

static void recCancelInstruction()
{
	pxFailRel("recCancelInstruction() called, this should never happen!");
}

// [TEMP_DIAG] @@EE_WATCHDOG@@ background thread to sample EE PC every 3 seconds
#include <thread>
#include <atomic>
extern uint g_FrameCount;
static std::atomic<bool> s_watchdog_running{false};
static mach_port_t s_ee_thread_port = MACH_PORT_NULL; // [TEMP_DIAG] @@THREAD_PC@@
static void eeWatchdogThread()
{
	int n = 0;
	u32 prev_cyc = 0;
	bool dumped = false;
	while (s_watchdog_running.load(std::memory_order_relaxed) && n < 30) {
		std::this_thread::sleep_for(std::chrono::seconds(3));
		if (!s_watchdog_running.load(std::memory_order_relaxed)) break;
		u32 cur_pc = cpuRegs.pc;
		u32 cur_cyc = cpuRegs.cycle;
		extern std::atomic<uint64_t> g_hpf_cnt;
		extern std::atomic<uintptr_t> g_hpf_last_fa;
		extern std::atomic<uintptr_t> g_hpf_last_epc;
		extern std::atomic<uint32_t> g_hpf_last_path;
		uint64_t hpf = g_hpf_cnt.load(std::memory_order_relaxed);
		Console.WriteLn("@@EE_WATCHDOG@@ n=%d ee_pc=%08x ee_cyc=%u nextEvt=%u evtCnt=%llu hpfCnt=%llu frame=%u",
			n, cur_pc, cur_cyc, cpuRegs.nextEventCycle,
			s_recEventTest_cnt.load(std::memory_order_relaxed),
			hpf,
			g_FrameCount);
		// [iter685] Log HPF details when count is high
		if (hpf > 20) {
			uintptr_t fa_val = g_hpf_last_fa.load(std::memory_order_relaxed);
			uintptr_t epc_val = g_hpf_last_epc.load(std::memory_order_relaxed);
			Dl_info di_epc = {};
			dladdr((void*)epc_val, &di_epc);
			Console.WriteLn("@@HPF_DIAG@@ fa=%p epc=%p [%s+0x%lx] path=%u",
				(void*)fa_val, (void*)epc_val,
				di_epc.dli_sname ? di_epc.dli_sname : "?",
				di_epc.dli_saddr ? (unsigned long)(epc_val - (uintptr_t)di_epc.dli_saddr) : 0UL,
				g_hpf_last_path.load(std::memory_order_relaxed));
		}
		// [TEMP_DIAG] @@THREAD_PC@@ sample EE thread's actual host PC via Mach APIs
		if (n > 0 && cur_cyc == prev_cyc && s_ee_thread_port != MACH_PORT_NULL) {
			static int s_tpc_n = 0;
			if (s_tpc_n < 5) {
#ifdef __aarch64__
				arm_thread_state64_t ts;
				mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
				kern_return_t kr = thread_get_state(s_ee_thread_port, ARM_THREAD_STATE64,
					(thread_state_t)&ts, &count);
				if (kr == KERN_SUCCESS) {
					uintptr_t host_pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(ts);
					uintptr_t host_lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(ts);
					uintptr_t host_sp = (uintptr_t)__darwin_arm_thread_state64_get_sp(ts);
					// Resolve symbol via dladdr
					Dl_info di_pc = {}, di_lr = {};
					dladdr((void*)host_pc, &di_pc);
					dladdr((void*)host_lr, &di_lr);
					Console.WriteLn("@@THREAD_PC@@ n=%d host_pc=%p [%s+0x%lx] host_lr=%p [%s+0x%lx]",
						s_tpc_n, (void*)host_pc,
						di_pc.dli_sname ? di_pc.dli_sname : "?",
						di_pc.dli_saddr ? (long)(host_pc - (uintptr_t)di_pc.dli_saddr) : 0,
						(void*)host_lr,
						di_lr.dli_sname ? di_lr.dli_sname : "?",
						di_lr.dli_saddr ? (long)(host_lr - (uintptr_t)di_lr.dli_saddr) : 0);
				} else {
					Console.WriteLn("@@THREAD_PC@@ n=%d thread_get_state FAILED kr=%d", s_tpc_n, kr);
				}
#endif
				s_tpc_n++;
			}
		}
		// [TEMP_DIAG] @@ADDR_CHECK@@ verify x27 address vs actual g_cpuRegistersPack
		if (n == 0) {
			uintptr_t pack_addr = (uintptr_t)&g_cpuRegistersPack;
			uintptr_t cycle_addr = (uintptr_t)&g_cpuRegistersPack.cpuRegs.cycle;
			uintptr_t cycle_off = cycle_addr - pack_addr;
			u32 raw_cycle = *(volatile u32*)cycle_addr;
			Console.WriteLn("@@ADDR_CHECK@@ pack=%p cycle_addr=%p offset=0x%lx raw_cycle=%u ref_cycle=%u match=%d",
				(void*)pack_addr, (void*)cycle_addr, (unsigned long)cycle_off,
				raw_cycle, cpuRegs.cycle, (raw_cycle == cpuRegs.cycle) ? 1 : 0);
		}
		// [TEMP_DIAG] @@CYCLE_KICK@@ on 3rd frozen sample, bump cycle to test if blocks execute
		if (n == 3 && cur_cyc == prev_cyc) {
			u32 new_cyc = cur_cyc + 1000000;
			cpuRegs.cycle = new_cyc;
			Console.WriteLn("@@CYCLE_KICK@@ wrote cycle=%u (was %u)", new_cyc, cur_cyc);
		}
		// Detect freeze: same cycle for 2 consecutive samples
		if (n > 0 && cur_cyc == prev_cyc && !dumped) {
			dumped = true;
			// [P15] Dump MIPS code at stuck PC to understand the loop
			// Removal condition: freeze root causeafter fixed
			if (eeMem && cur_pc < 0x02000000u) {
				u32 base = (cur_pc >= 0x40u) ? (cur_pc - 0x40u) : 0;
				Console.WriteLn("@@WATCHDOG_MIPS@@ pc=%08x dumping 0x%08x-0x%08x", cur_pc, base, cur_pc + 0x40u);
				for (u32 a = base; a < cur_pc + 0x40u; a += 0x10u) {
					const u32* p = reinterpret_cast<const u32*>(eeMem->Main + (a & 0x01FFFFFFu));
					Console.WriteLn("@@WATCHDOG_MIPS@@ [%08x] %08x %08x %08x %08x%s",
						a, p[0], p[1], p[2], p[3], (a <= cur_pc && a + 0x10u > cur_pc) ? " <-- PC" : "");
				}
			}
			// Dump native code at the stuck PC's block (runtime, post-link)
			BASEBLOCK* blk = PC_GETBLOCK(cur_pc);
			if (blk && blk->GetFnptr()) {
				const u32* nc = reinterpret_cast<const u32*>(blk->GetFnptr());
				Console.WriteLn("@@WATCHDOG_NATIVE@@ pc=%08x fnptr=%p", cur_pc, (void*)nc);
				for (int wi = 0; wi < 40; wi += 4) {
					Console.WriteLn("  +%03x: %08x %08x %08x %08x",
						wi*4, nc[wi], nc[wi+1], nc[wi+2], nc[wi+3]);
				}
			}
		}
		prev_cyc = cur_cyc;
		n++;
	}
}

static void recExecute()
{
    // Task: Confirm Execution Start
    static bool s_exec_once = false;
    if (!s_exec_once) {
        s_exec_once = true;
        LogUnified("@@EXEC_START@@ R5900 Execution Started\n");
    }

	// [TEMP_DIAG] Save EE thread's mach port for watchdog PC sampling
	s_ee_thread_port = mach_thread_self();
	// [TEMP_DIAG] Launch watchdog thread once
	if (!s_watchdog_running.exchange(true)) {
		std::thread(eeWatchdogThread).detach();
	}

	// Reset before we try to execute any code, if there's one pending.
	// We need to do this here, because if we reset while we're executing, it sets the "needs reset"
	// flag, which triggers a JIT exit (the fastjmp_set below), and eventually loops back here.
	if (eeRecNeedsReset)
	{
		eeRecNeedsReset = false;
		recResetRaw();
	}

	// setjmp will save the register context and will return 0
	// A call to longjmp will restore the context (included the eip/rip)
	// but will return the longjmp 2nd parameter (here 1)
		if (!fastjmp_set(&m_SetJmp_StateCheck))
		{
			eeCpuExecuting = true;
	        
	        // DEBUG: Log entry into JIT execution
        // static bool s_jit_entry_log = false;
        // if (!s_jit_entry_log) {
        //    Console.WriteLn("DEBUG: Entered JIT execution via setjmp. Jumping to EnterRecompiledCode.");
        //    s_jit_entry_log = true;
        // }

		// [P49] Legacy lazy toggle: flip RW→RX before entering JIT code
		DarwinMisc::LegacyEnsureExecutable();

		((void (*)())EnterRecompiledCode)();

		// Generally unreachable code here ...
	}

	eeCpuExecuting = false;

	EE::Profiler.Print();
}

void R5900::Dynarec::OpcodeImpl::recSYSCALL()
{
	EE::Profiler.EmitOp(eeOpcode::SYSCALL);
	// FlushCache/iFlushCache skip: PC PCSX2 equivalent optimization.
	// Skip the kernel SYSCALL exception handler and add estimated cycles instead.
	if (GPR_IS_CONST1(3))
	{
		if (g_cpuConstRegs[3].UC[0] == 0x64 || g_cpuConstRegs[3].UC[0] == 0x68)
		{
			// Emulate the amount of cycles it takes for the exception handlers to run
			// This number was found by using github.com/F0bes/flushcache-cycles
			s_nBlockCycles += 5650;
			return;
		}
	}
	recCall(R5900::Interpreter::OpcodeImpl::SYSCALL);
	g_branch = 2; // Indirect branch with event check.
}

////////////////////////////////////////////////////
void R5900::Dynarec::OpcodeImpl::recBREAK()
{
	EE::Profiler.EmitOp(eeOpcode::BREAK);

	recCall(R5900::Interpreter::OpcodeImpl::BREAK);
	g_branch = 2; // Indirect branch with event check.
}

// Size is in dwords (4 bytes)
// [iter681] @@SIF_6668_RUNTIME@@ — capture state at function 0x6668 entry + memory values
void sif6668_runtime_log()
{
	static int s_n = 0;
	if (s_n < 5) {
		// Read the critical memory values the function will use
		u32 smflg = 0, kern_byte = 0;
		if (eeMem) {
			kern_byte = eeMem->Main[0x21344]; // *(u8*)(0x80021344) — kernel variable
		}
		// Read SIF SMFLG via HW handler
		smflg = memRead32(0xB000C430u);
		u32 sif_ctrl = memRead32(0xB000C400u);

		Console.WriteLn("@@SIF_6668_RUNTIME@@ n=%d v0=%08x a0=%08x a1=%08x a2=%08x ra=%08x SMFLG=%08x kern1344=%02x SIF_CTRL=%08x",
			s_n++,
			cpuRegs.GPR.r[2].UL[0],
			cpuRegs.GPR.r[4].UL[0],
			cpuRegs.GPR.r[5].UL[0],
			cpuRegs.GPR.r[6].UL[0],
			cpuRegs.GPR.r[31].UL[0],
			smflg, kern_byte, sif_ctrl);
	}
}

// [iter681] @@SIF_66D0_RUNTIME@@ — BNE-taken path entry
void sif66D0_runtime_log()
{
	static int s_n = 0;
	if (s_n < 5)
		Console.WriteLn("@@SIF_66D0_RUNTIME@@ n=%d v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x",
			s_n++,
			cpuRegs.GPR.r[2].UL[0],
			cpuRegs.GPR.r[3].UL[0],
			cpuRegs.GPR.r[4].UL[0],
			cpuRegs.GPR.r[5].UL[0],
			cpuRegs.GPR.r[6].UL[0]);
}

// [iter681] @@SIF_66E8_RUNTIME@@ — capture state at function 0x6668 return (JR ra)
void sif66E8_runtime_log()
{
	static int s_n = 0;
	if (s_n < 5)
		Console.WriteLn("@@SIF_66E8_RUNTIME@@ n=%d v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x ra=%08x",
			s_n++,
			cpuRegs.GPR.r[2].UL[0],
			cpuRegs.GPR.r[3].UL[0],
			cpuRegs.GPR.r[4].UL[0],
			cpuRegs.GPR.r[5].UL[0],
			cpuRegs.GPR.r[6].UL[0],
			cpuRegs.GPR.r[31].UL[0]);
}

// [iter681] @@SIF_6810_RUNTIME@@ — capture v0 at 0x6810 (return from function 0x6668)
void sif6810_runtime_log()
{
	static int s_n = 0;
	if (s_n < 10)
		Console.WriteLn("@@SIF_6810_RUNTIME@@ n=%d v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x s4=%08x t0=%08x t1=%08x ra=%08x",
			s_n++,
			cpuRegs.GPR.r[2].UL[0],  // v0
			cpuRegs.GPR.r[3].UL[0],  // v1
			cpuRegs.GPR.r[4].UL[0],  // a0
			cpuRegs.GPR.r[5].UL[0],  // a1
			cpuRegs.GPR.r[6].UL[0],  // a2
			cpuRegs.GPR.r[20].UL[0], // s4
			cpuRegs.GPR.r[8].UL[0],  // t0
			cpuRegs.GPR.r[9].UL[0],  // t1
			cpuRegs.GPR.r[31].UL[0]); // ra
}

// [iter681] @@SIF_690C_RUNTIME@@ — runtime register dump when block 0x690C executes
void sif690c_runtime_log()
{
	static int s_n = 0;
	if (s_n < 5)
		Console.WriteLn("@@SIF_690C_RUNTIME@@ n=%d v0=%08x a0=%08x a1=%08x s0=%08x s2=%08x s4=%08x s5=%08x ra=%08x pc=%08x",
			s_n++,
			cpuRegs.GPR.r[2].UL[0],  // v0
			cpuRegs.GPR.r[4].UL[0],  // a0
			cpuRegs.GPR.r[5].UL[0],  // a1
			cpuRegs.GPR.r[16].UL[0], // s0
			cpuRegs.GPR.r[18].UL[0], // s2
			cpuRegs.GPR.r[20].UL[0], // s4
			cpuRegs.GPR.r[21].UL[0], // s5
			cpuRegs.GPR.r[31].UL[0], // ra
			cpuRegs.pc);
}

// [iter682] TEMP_DIAG: capture register state at soft-float normalization loop entry
// Removal condition: OSDSYS stuck causeafter identified
void softfloat_loop_probe()
{
	static int s_n = 0;
	if (s_n < 10)
		Console.WriteLn("@@SOFTFLOAT_LOOP@@ n=%d pc=%08x s1=%016llx a0=%016llx a1=%016llx a2=%016llx a3=%016llx "
			"t0=%016llx v0=%016llx v1=%016llx s0=%016llx ra=%08x sp=%08x",
			s_n++, cpuRegs.pc,
			cpuRegs.GPR.r[17].UD[0], // s1
			cpuRegs.GPR.r[4].UD[0],  // a0
			cpuRegs.GPR.r[5].UD[0],  // a1
			cpuRegs.GPR.r[6].UD[0],  // a2
			cpuRegs.GPR.r[7].UD[0],  // a3
			cpuRegs.GPR.r[8].UD[0],  // t0
			cpuRegs.GPR.r[2].UD[0],  // v0
			cpuRegs.GPR.r[3].UD[0],  // v1
			cpuRegs.GPR.r[16].UD[0], // s0
			cpuRegs.GPR.r[31].UL[0], // ra
			cpuRegs.GPR.r[29].UL[0]  // sp
		);
}

// [TEMP_DIAG] @@JIT_PERF_COUNTERS@@ — report and reset counters (called from Counters.cpp)
namespace R5900 { namespace Dynarec { namespace OpcodeImpl { namespace COP1 {
	uint32_t recMTC1_GetAndResetCount();
} } } }
using R5900::Dynarec::OpcodeImpl::COP1::recMTC1_GetAndResetCount;

void recReportPerfCounters(int vsync)
{
	uint32_t compiles = s_jit_compile_count.exchange(0, std::memory_order_relaxed);
	uint32_t reccalls = s_jit_reccall_count.exchange(0, std::memory_order_relaxed);
	uint32_t clears   = s_jit_cache_clear_count.exchange(0, std::memory_order_relaxed);
	ptrdiff_t cache_used = recPtr ? (recPtr - SysMemory::GetEERec()) : 0;
	uint32_t sg_clears_perf = g_stub_guard_clear_count.exchange(0, std::memory_order_relaxed);
	uint32_t rc_total_perf = s_recClear_total_count.exchange(0, std::memory_order_relaxed);
	uint32_t rc_blocks_perf = s_recClear_total_blocks.exchange(0, std::memory_order_relaxed);
	uint32_t rc_sz1 = s_recClear_sz1.exchange(0, std::memory_order_relaxed);
	uint32_t rc_sz1024 = s_recClear_sz1024.exchange(0, std::memory_order_relaxed);
	uint32_t rc_szlarge = s_recClear_szlarge.exchange(0, std::memory_order_relaxed);
	uint32_t rc_szother = s_recClear_szother.exchange(0, std::memory_order_relaxed);
	Console.WriteLn("@@JIT_PERF@@ vsync=%d compiles=%u reccalls=%u clears=%u cache=%tdKB/%dKB sg=%u rc=%u/%u sz1=%u pg=%u lg=%u ot=%u",
		vsync, compiles, reccalls, clears,
		cache_used / 1024, (int)(HostMemoryMap::EErecSize / 1024),
		sg_clears_perf, rc_total_perf, rc_blocks_perf,
		rc_sz1, rc_sz1024, rc_szlarge, rc_szother);

	// [TEMP_DIAG] @@COMPILE_CONTENT@@ — compile content analysis (DISABLED: suspected crash in map iteration)
#if 0
	{
		const uint32_t unique_pcs = static_cast<uint32_t>(s_compile_pc_freq.size());
		uint32_t recompile_hits = 0;  // PCs compiled more than once
		uint32_t max_freq = 0;
		u32 max_freq_pc = 0;
		// Top-5 by frequency
		struct TopEntry { u32 pc; uint16_t count; };
		TopEntry top5[5] = {};
		for (auto& [pc, cnt] : s_compile_pc_freq) {
			if (cnt > 1) recompile_hits++;
			// Insert into top5
			for (int i = 0; i < 5; i++) {
				if (cnt > top5[i].count) {
					// Shift down
					for (int j = 4; j > i; j--) top5[j] = top5[j-1];
					top5[i] = {pc, cnt};
					break;
				}
			}
		}
		uint32_t mtc1_calls = recMTC1_GetAndResetCount();
		uint32_t avg_guest = compiles ? (uint32_t)(s_compile_guest_insns / compiles) : 0;
		uint32_t avg_native = compiles ? (uint32_t)(s_compile_native_bytes / compiles) : 0;

		uint32_t block_discards = s_dyna_block_discard_count.exchange(0, std::memory_order_relaxed);
		uint32_t page_resets = s_dyna_page_reset_count.exchange(0, std::memory_order_relaxed);
		uint32_t mmap_clears = g_mmap_clear_count.exchange(0, std::memory_order_relaxed);
		uint32_t mmap_top_page = g_mmap_clear_page_top.load(std::memory_order_relaxed);
		uint32_t mmap_top_cnt = g_mmap_clear_page_top_count.load(std::memory_order_relaxed);

		// Virtual PC stats
		const uint32_t unique_vpcs = static_cast<uint32_t>(s_compile_vpc_freq.size());
		uint32_t vpc_recompile_hits = 0;
		TopEntry vtop5[5] = {};
		for (auto& [vpc, cnt] : s_compile_vpc_freq) {
			if (cnt > 1) vpc_recompile_hits++;
			for (int i = 0; i < 5; i++) {
				if (cnt > vtop5[i].count) {
					for (int j = 4; j > i; j--) vtop5[j] = vtop5[j-1];
					vtop5[i] = {vpc, cnt};
					break;
				}
			}
		}

		uint32_t recomp_known = s_recompile_of_known;
		s_recompile_of_known = 0;
		uint32_t ever_total = static_cast<uint32_t>(s_ever_compiled_vpcs.size());
		Console.WriteLn("@@COMPILE_CONTENT@@ vsync=%d unique_pcs=%u recompile_hits=%u unique_vpcs=%u vpc_rehits=%u mtc1=%u avg_guest=%u avg_native=%u recomp_known=%u ever_vpcs=%u",
			vsync, unique_pcs, recompile_hits, unique_vpcs, vpc_recompile_hits, mtc1_calls, avg_guest, avg_native, recomp_known, ever_total);
		uint32_t overlap_clears = s_overlap_clear_count.exchange(0, std::memory_order_relaxed);
		// Find top-3 overlap pages
		struct OvlTop { uint32_t page; uint32_t count; };
		OvlTop ovl_top[3] = {};
		for (uint32_t p = 0; p < 2048; p++) {
			if (s_overlap_page_hist[p] > 0) {
				for (int i = 0; i < 3; i++) {
					if (s_overlap_page_hist[p] > ovl_top[i].count) {
						for (int j = 2; j > i; j--) ovl_top[j] = ovl_top[j-1];
						ovl_top[i] = {p, s_overlap_page_hist[p]};
						break;
					}
				}
			}
		}
		uint32_t rc_total = s_recClear_total_count.exchange(0, std::memory_order_relaxed);
		uint32_t rc_blocks = s_recClear_total_blocks.exchange(0, std::memory_order_relaxed);
		uint32_t sg_clears = g_stub_guard_clear_count.exchange(0, std::memory_order_relaxed);
		Console.WriteLn("@@INVALIDATION_PATH@@ vsync=%d block_discards=%u page_resets=%u mmap_clears=%u overlap_clears=%u recClear_calls=%u recClear_blocks=%u stub_guard=%u mmap_top=%u(x%u)",
			vsync, block_discards, page_resets, mmap_clears, overlap_clears, rc_total, rc_blocks, sg_clears, mmap_top_page, mmap_top_cnt);
		Console.WriteLn("@@OVERLAP_PAGES@@ vsync=%d #1=page%u(x%u) #2=page%u(x%u) #3=page%u(x%u)",
			vsync, ovl_top[0].page, ovl_top[0].count, ovl_top[1].page, ovl_top[1].count, ovl_top[2].page, ovl_top[2].count);
		Console.WriteLn("@@COMPILE_TOP5_HW@@ vsync=%d #1=%08x(%u) #2=%08x(%u) #3=%08x(%u) #4=%08x(%u) #5=%08x(%u)",
			vsync,
			top5[0].pc, top5[0].count, top5[1].pc, top5[1].count,
			top5[2].pc, top5[2].count, top5[3].pc, top5[3].count,
			top5[4].pc, top5[4].count);
		Console.WriteLn("@@COMPILE_TOP5_VP@@ vsync=%d #1=%08x(%u) #2=%08x(%u) #3=%08x(%u) #4=%08x(%u) #5=%08x(%u)",
			vsync,
			vtop5[0].pc, vtop5[0].count, vtop5[1].pc, vtop5[1].count,
			vtop5[2].pc, vtop5[2].count, vtop5[3].pc, vtop5[3].count,
			vtop5[4].pc, vtop5[4].count);

		// Reset for next window
		s_compile_pc_freq.clear();
		s_compile_vpc_freq.clear();
		s_compile_guest_insns = 0;
		s_compile_native_bytes = 0;
		s_compile_mtc1_blocks = 0;
	}
#endif
}

void recClear(u32 addr, u32 size)
{
	s_recClear_total_count.fetch_add(1, std::memory_order_relaxed); // [TEMP_DIAG]
	// [TEMP_DIAG] @@RECCLEAR_BUCKET@@ — categorize recClear calls by size for JIT_PERF reporting
	{
		// Buckets: sz1 (size==1), sz1024 (size==0x400, page clear), sz_large (size>=0x100000), sz_other
		if (size == 1)
			s_recClear_sz1.fetch_add(1, std::memory_order_relaxed);
		else if (size == 0x400)
			s_recClear_sz1024.fetch_add(1, std::memory_order_relaxed);
		else if (size >= 0x100000u) {
			s_recClear_szlarge.fetch_add(1, std::memory_order_relaxed);
			// [TEMP_DIAG] Log first 50 large clears after boot to identify caller
			static uint32_t s_lg_n = 0;
			static uint32_t s_lg_skip = 0;
			s_lg_skip++;
			if (s_lg_skip > 100 && s_lg_n < 50) { // skip initial boot clears
				Console.WriteLn("@@RECCLEAR_LARGE@@ n=%u addr=%08x size=%u(0x%x)", s_lg_n, addr, size, size);
				s_lg_n++;
			}
		}
		else
			s_recClear_szother.fetch_add(1, std::memory_order_relaxed);
	}

	// [iter681] @@RECCLEAR_DIAG@@ — log large clears to verify FlushCache effectiveness
	const bool is_large_clear = (size >= 0x100000u);
	if (is_large_clear) {
		static int s_rc_n = 0;
		if (s_rc_n < 5)
			Console.WriteLn("@@RECCLEAR_DIAG@@ n=%d addr=%08x size=%u maxrecmem=%08x recLUT[0]=%lx",
				s_rc_n++, addr, size, maxrecmem, (unsigned long)recLUT[addr >> 16]);
	}

	if ((addr) >= maxrecmem || !(recLUT[(addr) >> 16] + (addr & ~0xFFFFUL)))
		return;

	addr = HWADDR(addr);

    u32 addr_size = addr + (size << 2); // // size * 4
	int blockidx = recBlocks.LastIndex(addr_size - 4);

	if (blockidx == -1) {
		// (blockidx=-1 log suppressed)
		return;
	}

	u32 lowerextent = 0xFFFFFFFF, upperextent = 0, ceiling = 0xFFFFFFFF; // 0xFFFFFFFF == -1

	BASEBLOCKEX* pexblock = recBlocks[blockidx + 1];
	if (pexblock)
		ceiling = pexblock->startpc;

	int toRemoveLast = blockidx;
	int cleared_count = 0;

    u32 blockstart, blockend;
	while ((pexblock = recBlocks[blockidx]))
	{
		blockstart = pexblock->startpc;
		blockend = pexblock->startpc + (pexblock->size << 2); // pexblock->size * 4
        BASEBLOCK* pblock = PC_GETBLOCK(blockstart);

		if (pblock == s_pCurBlock)
		{
			if (toRemoveLast != blockidx)
			{
				recBlocks.Remove((blockidx + 1), toRemoveLast);
			}
			toRemoveLast = --blockidx;
			continue;
		}

		if (blockend <= addr)
		{
			lowerextent = std::max(lowerextent, blockend);
			break;
		}

		lowerextent = std::min(lowerextent, blockstart);
		upperextent = std::max(upperextent, blockend);
		pblock->SetFnptr((uptr)JITCompile);
		cleared_count++;

		blockidx--;
	}

	if (toRemoveLast != blockidx)
	{
		recBlocks.Remove((blockidx + 1), toRemoveLast);
	}

	if (is_large_clear) {
		static int s_clear_log_n = 0;
		if (s_clear_log_n < 10)
			Console.WriteLn("@@RECCLEAR_DIAG@@ cleared %d blocks, range=[%08x,%08x)", cleared_count, lowerextent, upperextent);
		s_clear_log_n++;
	}

	upperextent = std::min(upperextent, ceiling);

	for (int i = 0; (pexblock = recBlocks[i]); ++i)
	{
		if (s_pCurBlock == PC_GETBLOCK(pexblock->startpc))
			continue;
		blockend = pexblock->startpc + (pexblock->size << 2); // pexblock->size * 4
		if ((pexblock->startpc >= addr && pexblock->startpc < addr_size) || (pexblock->startpc < addr && blockend > addr)) [[unlikely]]
		{
			Console.Warning("[EE] Block clearing edge case: block[%08x-%08x] overlaps clear range[%08x-%08x] — skipping",
				pexblock->startpc, blockend, addr, addr_size);
			// [R103] Changed from pxFail to warning — this edge case occurs during
			// warm reboots when blocks span the clear boundary. Non-fatal.
		}
	}

	if (upperextent > lowerextent)
		ClearRecLUT(PC_GETBLOCK(lowerextent), upperextent - lowerextent);

	if (cleared_count > 0)
		s_recClear_total_blocks.fetch_add(cleared_count, std::memory_order_relaxed); // [TEMP_DIAG]
}


static int* s_pCode;

static bool IsBranchImm0CProbeEnabled()
{
	static int s_cached = -1;
	if (s_cached < 0)
		s_cached = iPSX2_GetRuntimeEnvBool("iPSX2_BRANCHIMM_0C_PROBE", false) ? 1 : 0;
	return s_cached == 1;
}

static bool IsJrRaProbeEnabled()
{
	static int s_cached = -1;
	if (s_cached < 0)
		s_cached = iPSX2_GetRuntimeEnvBool("iPSX2_JR_RA_PROBE", false) ? 1 : 0;
	return s_cached == 1;
}

static void LogJrRaProbe(u32 compile_pc, u32 reg, bool swap)
{
	if (!IsJrRaProbeEnabled() || reg != 31)
		return;

	static bool s_cfg_logged = false;
	static int s_count = 0;
	constexpr int kCap = 50;
	if (!s_cfg_logged)
	{
		s_cfg_logged = true;
		Console.WriteLn("@@CFG@@ iPSX2_JR_RA_PROBE=1 iPSX2_JR_RA_PROBE_CAP=%d", kCap);
	}
	if (s_count >= kCap)
		return;

	const int x86reg = _checkX86reg(X86TYPE_GPR, reg, MODE_READ);
	const int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, reg, MODE_READ);
	const bool is_const = GPR_IS_CONST1(reg);
	const u32 const_lo = is_const ? g_cpuConstRegs[reg].UL[0] : 0;
	const u32 mem_lo = cpuRegs.GPR.r[reg].UL[0];
	Console.WriteLn(
		"@@JR_RA_PROBE@@ idx=%d runtime_pc=%08x compile_pc=%08x reg=%u is_const=%d const_lo=%08x mem_lo=%08x x86reg=%d xmmreg=%d swap=%d",
		s_count, cpuRegs.pc, compile_pc, reg, is_const ? 1 : 0, const_lo, mem_lo, x86reg, xmmreg, swap ? 1 : 0);
	s_count++;
}

static void LogBranchImm0CProbe(u32 compile_pc, u32 imm)
{
	if (!IsBranchImm0CProbeEnabled())
		return;

	static bool s_cfg_logged = false;
	static int s_count = 0;
	constexpr int kCap = 50;
	if (!s_cfg_logged)
	{
		s_cfg_logged = true;
		Console.WriteLn("@@CFG@@ iPSX2_BRANCHIMM_0C_PROBE=1 iPSX2_BRANCHIMM_0C_PROBE_CAP=%d", kCap);
	}

	if (s_count >= kCap)
		return;

	const u32 branch_pc = compile_pc - 8;
	const u32 delay_pc = compile_pc - 4;
	const u32 branch_opcode = (u32)memRead32(branch_pc);
	const u32 delay_opcode = (u32)memRead32(delay_pc);
	const u32 runtime_pc = cpuRegs.pc;
	Console.WriteLn(
		"@@BRIMM_0C_PROBE@@ idx=%d runtime_pc=%08x compile_pc=%08x branch_pc=%08x branch_op=%08x delay_op=%08x imm=%08x",
		s_count, runtime_pc, compile_pc, branch_pc, branch_opcode, delay_opcode, imm);
	s_count++;
}

void SetBranchReg(u32 reg)
{
	g_branch = 1;

	if (reg != 0xffffffff)
	{
		//		if (GPR_IS_CONST1(reg))
		//			xMOV(ptr32[&cpuRegs.pc], g_cpuConstRegs[reg].UL[0]);
		//		else
		//		{
		//			int mmreg;
		//
		//			if ((mmreg = _checkXMMreg(XMMTYPE_GPRREG, reg, MODE_READ)) >= 0)
		//			{
		//				xMOVSS(ptr[&cpuRegs.pc], xRegisterSSE(mmreg));
		//			}
		//			else
		//			{
		//				xMOV(eax, ptr[(void*)((int)&cpuRegs.GPR.r[reg].UL[0])]);
		//				xMOV(ptr[&cpuRegs.pc], eax);
		//			}
		//		}
		const bool swap = EmuConfig.Gamefixes.GoemonTlbHack ? false : TrySwapDelaySlot(reg, 0, 0, true);
		LogJrRaProbe(pc, reg, swap);
		if (!swap)
		{
			const int wbreg = _allocX86reg(X86TYPE_PCWRITEBACK, 0, MODE_WRITE | MODE_CALLEESAVED);
            // Fix: Use HostW to map slot to physical register
            auto reg32 = HostW(wbreg);

			_eeMoveGPRtoR(reg32, reg);
			// Snapshot the pre-delay-slot jump target so fallback path (pcWriteback) cannot read stale/zero.
			armStore(PTR_CPU(cpuRegs.pcWriteback), reg32);

			if (EmuConfig.Gamefixes.GoemonTlbHack)
			{
//				xMOV(ecx, xRegister32(wbreg));
                armAsm->Mov(ECX, reg32);
				vtlb_DynV2P();
//				xMOV(xRegister32(wbreg), eax);
                armAsm->Mov(reg32, EAX);
			}

			recompileNextInstruction(true, false);

			// the next instruction may have flushed the register.. so reload it if so.
			if (x86regs[wbreg].inuse && x86regs[wbreg].type == X86TYPE_PCWRITEBACK)
			{
//				xMOV(ptr[&cpuRegs.pc], xRegister32(wbreg));
                armStore(PTR_CPU(cpuRegs.pc), reg32);
				x86regs[wbreg].inuse = 0;
			}
			else
			{
//				xMOV(eax, ptr[&cpuRegs.pcWriteback]);
                armLoad(EAX, PTR_CPU(cpuRegs.pcWriteback));
//				xMOV(ptr[&cpuRegs.pc], eax);
                armStore(PTR_CPU(cpuRegs.pc), EAX);
			}
		}
		else
		{
			if (GPR_IS_DIRTY_CONST(reg) || _hasX86reg(X86TYPE_GPR, reg, 0))
			{
				const int x86reg = _allocX86reg(X86TYPE_GPR, reg, MODE_READ);
//				xMOV(ptr32[&cpuRegs.pc], xRegister32(x86reg));
                // Fix: Use HostW
                armStore(PTR_CPU(cpuRegs.pc), HostW(x86reg));
			}
			else
			{
				_eeMoveGPRtoM(PTR_CPU(cpuRegs.pc), reg);
			}
		}
	}

	//	xCMP(ptr32[&cpuRegs.pc], 0);
	//	j8Ptr[5] = JNE8(0);
	//	xFastCall((void*)(uptr)tempfn);
	//	x86SetJ8(j8Ptr[5]);

	iFlushCall(FLUSH_EVERYTHING);

	iBranchTest();
}

void SetBranchImm(u32 imm)
{
	g_branch = 1;

	pxAssert(imm);
	if (imm == 0x0c000000)
		LogBranchImm0CProbe(pc, imm);
	// @@SETBRANCH_02000000@@ compile-time one-shot: direct branch to unmapped page
	if (imm == 0x02000000)
	{
		static bool s_seen = false;
		if (!s_seen) {
			s_seen = true;
			// [iter39] Add startpc and COP0 to identify which block leads to PC=0x02000000.
			// [iter40] Add memory content at startpc to check if EE RAM is initialized.
			// Removal condition: EE PC=0x02000000 到達経路after determined。
			Console.WriteLn("@@SETBRANCH_02000000@@ compile_pc=%08x imm=%08x startpc=%08x ra=%08x epc=%08x cause=%08x badvaddr=%08x",
				pc, imm, s_recblock_startpc, cpuRegs.GPR.r[31].UL[0],
				cpuRegs.CP0.r[14], cpuRegs.CP0.r[13], cpuRegs.CP0.r[8]);
			// Dump first 4 and last 4 instructions of the block to check initialization state.
			Console.WriteLn("@@BLOCK_CONTENT@@ [0]=%08x [1]=%08x [2]=%08x [3]=%08x ... [-4]=%08x [-3]=%08x [-2]=%08x [-1]=%08x",
				memRead32(s_recblock_startpc + 0x00), memRead32(s_recblock_startpc + 0x04),
				memRead32(s_recblock_startpc + 0x08), memRead32(s_recblock_startpc + 0x0C),
				memRead32(imm - 0x10), memRead32(imm - 0x0C),
				memRead32(imm - 0x08), memRead32(imm - 0x04));
		}
	}
	// @@SETBRANCH_EERAM_TAIL@@ one-shot: catch jump into uninitialized EE RAM tail
	if (imm >= 0x01E00000u && imm < 0x02000000u)
	{
		static bool s_tail_seen = false;
		if (!s_tail_seen) { s_tail_seen = true; Console.WriteLn("@@SETBRANCH_EERAM_TAIL@@ compile_pc=%08x imm=%08x", pc, imm); }
	}

	// end the current block
	iFlushCall(FLUSH_EVERYTHING);
//	xMOV(ptr32[&cpuRegs.pc], imm);
    armStore(PTR_CPU(cpuRegs.pc), (uint64_t)imm);
	
	// [ROOT_FIX] P3 - Ensure proper link/dispatch for unconditional branches
	// When target block doesn't exist, we MUST go through dispatcher
	// recBlocks.Link will generate a jump to JITCompile if block missing
	// This is correct behavior - JITCompile will compile the target on-demand
	
	iBranchTest(imm);
}

u8* recBeginThunk()
{
	// if recPtr reached the mem limit reset whole mem
	if (recPtr >= recPtrEnd)
		eeRecNeedsReset = true;

	if (eeRecNeedsReset)
	{
		eeRecNeedsReset = false;
		recResetRaw();
	}

    armSetAsmPtr(recPtr, _4kb, nullptr);
    recPtr = armStartBlock();

#ifdef __APPLE__
    DarwinMisc::SetLastGuestPC(pc);
    DarwinMisc::SetLastRecPtr(recPtr);
#endif

    // Log block run
    // armLoad(a64::w0, PTR_CPU(cpuRegs.pc));
    // armEmitCall((void*)recLogBlockRun);

	return recPtr;
}

u8* recEndThunk()
{
//	u8* block_end = x86Ptr;
    u8* block_end = armEndBlock();

	pxAssert(block_end < SysMemory::GetEERecEnd());
	recPtr = block_end;
	return block_end;
}

bool TrySwapDelaySlot(u32 rs, u32 rt, u32 rd, bool allow_loadstore)
{
#if 1
	static int s_disable_swap_9fc434xx = -1;
	if (s_disable_swap_9fc434xx < 0)
	{
		s_disable_swap_9fc434xx = iPSX2_GetRuntimeEnvBool("iPSX2_DISABLE_DELAY_SWAP_9FC434XX", false) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_DISABLE_DELAY_SWAP_9FC434XX=%d", s_disable_swap_9fc434xx);
	}
	if (s_disable_swap_9fc434xx == 1 && pc >= 0x9FC43420 && pc <= 0x9FC43458)
		return false;

	if (g_recompilingDelaySlot)
		return false;

	const u32 opcode_encoded = *(u32*)PSM(pc);
	if (opcode_encoded == 0)
	{
		recompileNextInstruction(true, true);
		return true;
	}

	//std::string disasm;
	//disR5900Fasm(disasm, opcode_encoded, pc, false);

	const u32 opcode_rs = ((opcode_encoded >> 21) & 0x1F);
	const u32 opcode_rt = ((opcode_encoded >> 16) & 0x1F);
	const u32 opcode_rd = ((opcode_encoded >> 11) & 0x1F);

	switch (opcode_encoded >> 26)
	{
		case 8: // ADDI
		case 9: // ADDIU
		case 10: // SLTI
		case 11: // SLTIU
		case 12: // ANDIU
		case 13: // ORI
		case 14: // XORI
		case 24: // DADDI
		case 25: // DADDIU
		{
			if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
				goto is_unsafe;
		}
		break;

		case 26: // LDL
		case 27: // LDR
		case 30: // LQ
		case 31: // SQ
		case 32: // LB
		case 33: // LH
		case 34: // LWL
		case 35: // LW
		case 36: // LBU
		case 37: // LHU
		case 38: // LWR
		case 39: // LWU
		case 40: // SB
		case 41: // SH
		case 42: // SWL
		case 43: // SW
		case 44: // SDL
		case 45: // SDR
		case 46: // SWR
		case 55: // LD
		case 63: // SD
		{
			// We can't allow loadstore swaps for BC0x/BC2x, since they could affect the condition.
			if (!allow_loadstore || (rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
				goto is_unsafe;
		}
		break;

		case 15: // LUI
		{
			if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
				goto is_unsafe;
		}
		break;

		case 49: // LWC1
		case 57: // SWC1
		case 54: // LQC2
		case 62: // SQC2
			// fprintf(stderr, "SWAPPING coprocessor load delay slot (block %08X) %08X %s\n", s_pCurBlockEx->startpc, pc, disasm.c_str());
			break;

		case 0: // SPECIAL
		{
			switch (opcode_encoded & 0x3F)
			{
				case 0: // SLL
				case 2: // SRL
				case 3: // SRA
				case 4: // SLLV
				case 6: // SRLV
				case 7: // SRAV
				case 10: // MOVZ
				case 11: // MOVN
				case 20: // DSLLV
				case 22: // DSRLV
				case 23: // DSRAV
				case 24: // MULT
				case 25: // MULTU
				case 32: // ADD
				case 33: // ADDU
				case 34: // SUB
				case 35: // SUBU
				case 36: // AND
				case 37: // OR
				case 38: // XOR
				case 39: // NOR
				case 42: // SLT
				case 43: // SLTU
				case 44: // DADD
				case 45: // DADDU
				case 46: // DSUB
				case 47: // DSUBU
				case 56: // DSLL
				case 58: // DSRL
				case 59: // DSRA
				case 60: // DSLL32
				case 62: // DSRL31
				case 64: // DSRA32
				{
					if ((rs != 0 && rs == opcode_rd) || (rt != 0 && rt == opcode_rd) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
						goto is_unsafe;
				}
				break;

				case 15: // SYNC
				case 26: // DIV
				case 27: // DIVU
					break;

				default:
					goto is_unsafe;
			}
		}
		break;

		case 16: // COP0
		{
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 0: // MFC0
				case 2: // CFC0
				{
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						goto is_unsafe;
				}
				break;

				case 4: // MTC0
				case 6: // CTC0
					break;

				case 16: // TLB (technically would be safe, but we don't use it anyway)
				default:
					goto is_unsafe;
			}
			break;
		}
		break;

		case 17: // COP1
		{
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 0: // MFC1
				case 2: // CFC1
				{
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						goto is_unsafe;
				}
				break;

				case 4: // MTC1
				case 6: // CTC1
				case 16: // S
				{
					const u32 funct = (opcode_encoded & 0x3F);
					if (funct == 50 || funct == 52 || funct == 54) // C.EQ, C.LT, C.LE
					{
						// affects flags that we're comparing
						goto is_unsafe;
					}
				}
					[[fallthrough]];

				case 20: // W
				{
					// fprintf(stderr, "Swapping FPU delay slot (block %08X) %08X %s\n", s_pCurBlockEx->startpc, pc, disasm.c_str());
				}
				break;

				default:
					goto is_unsafe;
			}
		}
		break;

		case 18: // COP2
		{
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 8: // BC2XX
					goto is_unsafe;

				case 1: // QMFC2
				case 2: // CFC2
				{
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						goto is_unsafe;
				}
				break;

				default:
					break;
			}

			// fprintf(stderr, "Swapping COP2 delay slot (block %08X) %08X %s\n", s_pCurBlockEx->startpc, pc, disasm.c_str());
		}
		break;

		case 28: // MMI
		{
			switch (opcode_encoded & 0x3F)
			{
				case 8: // MMI0
				case 9: // MMI1
				case 10: // MMI2
				case 40: // MMI3
				case 41: // MMI3
				case 52: // PSLLH
				case 54: // PSRLH
				case 55: // LSRAH
				case 60: // PSLLW
				case 62: // PSRLW
				case 63: // PSRAW
				{
					if ((rs != 0 && rs == opcode_rd) || (rt != 0 && rt == opcode_rd) || (rd != 0 && rd == opcode_rd))
						goto is_unsafe;
				}
				break;

				default:
					goto is_unsafe;
			}
		}
		break;

		default:
			goto is_unsafe;
	}

	// fprintf(stderr, "Swapping delay slot %08X %s\n", pc, disasm.c_str());
	recompileNextInstruction(true, true);
	return true;

is_unsafe:
	// fprintf(stderr, "NOT SWAPPING delay slot %08X %s\n", pc, disasm.c_str());
	return false;
#else
	return false;
#endif
}

void SaveBranchState()
{
	s_savenBlockCycles = s_nBlockCycles;
	memcpy(s_saveConstRegs, g_cpuConstRegs, sizeof(g_cpuConstRegs));
	s_saveHasConstReg = g_cpuHasConstReg;
	s_saveFlushedConstReg = g_cpuFlushedConstReg;
	s_psaveInstInfo = g_pCurInstInfo;

	memcpy(s_saveXMMregs, xmmregs, sizeof(xmmregs));
}

void LoadBranchState()
{
	s_nBlockCycles = s_savenBlockCycles;

	memcpy(g_cpuConstRegs, s_saveConstRegs, sizeof(g_cpuConstRegs));
	g_cpuHasConstReg = s_saveHasConstReg;
	g_cpuFlushedConstReg = s_saveFlushedConstReg;
	g_pCurInstInfo = s_psaveInstInfo;

	memcpy(xmmregs, s_saveXMMregs, sizeof(xmmregs));
}

void iFlushCall(int flushtype)
{
	// Free registers that are not saved across function calls (x86-32 ABI):
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (!x86regs[i].inuse)
			continue;

		// [iter663] BUG FIX: armIsCallerSaved must receive the PHYSICAL register number
		// (HostGprPhys(i)), not the slot number (i). Slot numbers 0-4 all pass the
		// (id <= 17) check, causing ALL callee-saved registers (x19-x24) to be wrongly
		// freed by iFlushCall(0). This was the root cause of a0 (ANDI result) being lost
		// before BEQ comparison in the SIF DMA function.
		if (armIsCallerSaved(HostGprPhys(i)) ||
			((flushtype & FLUSH_FREE_VU0) && x86regs[i].type == X86TYPE_VIREG) ||
			((flushtype & FLUSH_FREE_NONTEMP_X86) && x86regs[i].type != X86TYPE_TEMP) ||
			((flushtype & FLUSH_FREE_TEMP_X86) && x86regs[i].type == X86TYPE_TEMP))
		{
			_freeX86reg(i);
		}
	}

	for (u32 i = 0; i < iREGCNT_XMM; i++)
	{
		if (!xmmregs[i].inuse)
			continue;

		if (armIsCallerSavedXmm(i) ||
			(flushtype & FLUSH_FREE_XMM) ||
			((flushtype & FLUSH_FREE_VU0) && xmmregs[i].type == XMMTYPE_VFREG))
		{
			_freeXMMreg(i);
		}
	}

	if (flushtype & FLUSH_ALL_X86)
		_flushX86regs();

	if (flushtype & FLUSH_FLUSH_XMM)
		_flushXMMregs();

	if (flushtype & FLUSH_CONSTANT_REGS)
		_flushConstRegs(true);

	if ((flushtype & FLUSH_PC) && !g_cpuFlushedPC)
	{
//		xMOV(ptr32[&cpuRegs.pc], pc);
        armStore(PTR_CPU(cpuRegs.pc), pc);
		g_cpuFlushedPC = true;
	}

	if ((flushtype & FLUSH_CODE) && !g_cpuFlushedCode)
	{
//		xMOV(ptr32[&cpuRegs.code], cpuRegs.code);
        armStore(PTR_CPU(cpuRegs.code), cpuRegs.code);
		g_cpuFlushedCode = true;
	}

#if 0
	if ((flushtype == FLUSH_CAUSE) && !g_maySignalException)
	{
		if (g_recompilingDelaySlot)
			xOR(ptr32[&cpuRegs.CP0.n.Cause], 1 << 31); // BD
		g_maySignalException = true;
	}
#endif
}

static inline void _freeXMMregs()
{
	for (u32 i = 0; i < iREGCNT_XMM; i++)
		_freeXMMreg(i);
}

// Note: scaleblockcycles() scales s_nBlockCycles respective to the EECycleRate value for manipulating the cycles of current block recompiling.
// s_nBlockCycles is 3 bit fixed point.  Divide by 8 when done!
// Scaling blocks under 40 cycles seems to produce countless problem, so let's try to avoid them.

#define DEFAULT_SCALED_BLOCKS() (s_nBlockCycles >> 3)

static u32 scaleblockcycles_calculation()
{
	const bool lowcycles = (s_nBlockCycles <= 40);
	const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
	u32 scale_cycles = 0;

	if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
		scale_cycles = DEFAULT_SCALED_BLOCKS();

	else if (cyclerate > 1)
		scale_cycles = s_nBlockCycles >> (2 + cyclerate);

	else if (cyclerate == 1)
		scale_cycles = DEFAULT_SCALED_BLOCKS() / 1.3f; // Adds a mild 30% increase in clockspeed for value 1.

	else if (cyclerate == -1) // the mildest value.
		// These values were manually tuned to yield mild speedup with high compatibility
		scale_cycles = (s_nBlockCycles <= 80 || s_nBlockCycles > 168 ? 5 : 7) * s_nBlockCycles / 32;

	else
		scale_cycles = ((5 + (-2 * (cyclerate + 1))) * s_nBlockCycles) >> 5;

	// Ensure block cycle count is never less than 1.
	return (scale_cycles < 1) ? 1 : scale_cycles;
}

static u32 scaleblockcycles()
{
	const u32 scaled = scaleblockcycles_calculation();

#if 0 // Enable this to get some runtime statistics about the scaling result in practice
	static u32 scaled_overall = 0, unscaled_overall = 0;
	if (g_resetEeScalingStats)
	{
		scaled_overall = unscaled_overall = 0;
		g_resetEeScalingStats = false;
	}
	u32 unscaled = DEFAULT_SCALED_BLOCKS();
	if (!unscaled) unscaled = 1;

	scaled_overall += scaled;
	unscaled_overall += unscaled;
	float ratio = static_cast<float>(unscaled_overall) / scaled_overall;

	DevCon.WriteLn(L"Unscaled overall: %d,  scaled overall: %d,  relative EE clock speed: %d %%",
	               unscaled_overall, scaled_overall, static_cast<int>(100 * ratio));
#endif

	return scaled;
}
u32 scaleblockcycles_clear()
{
	u32 scaled = scaleblockcycles_calculation();

#if 0 // Enable this to get some runtime statistics about the scaling result in practice
	static u32 scaled_overall = 0, unscaled_overall = 0;
	if (g_resetEeScalingStats)
	{
		scaled_overall = unscaled_overall = 0;
		g_resetEeScalingStats = false;
	}
	u32 unscaled = DEFAULT_SCALED_BLOCKS();
	if (!unscaled) unscaled = 1;

	scaled_overall += scaled;
	unscaled_overall += unscaled;
	float ratio = static_cast<float>(unscaled_overall) / scaled_overall;

	DevCon.WriteLn(L"Unscaled overall: %d,  scaled overall: %d,  relative EE clock speed: %d %%",
		unscaled_overall, scaled_overall, static_cast<int>(100 * ratio));
#endif
	const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
	const bool lowcycles = (s_nBlockCycles <= 40);

#ifndef DEBUG_ONLY_DISABLE_BLOCKCYCLE_MASK
#define DEBUG_ONLY_DISABLE_BLOCKCYCLE_MASK 0
#endif
	const u32 before = s_nBlockCycles;
#if !DEBUG_ONLY_DISABLE_BLOCKCYCLE_MASK
	if (!lowcycles && cyclerate > 1)
	{
		s_nBlockCycles &= (0x1 << (cyclerate + 2)) - 1;
	}
	else
	{
		s_nBlockCycles &= 0x7;
	}
#endif
	static bool s_log_once = false;
	if (!s_log_once)
	{
		s_log_once = true;
		Console.WriteLn("@@BLOCKCYCLE_MASK@@ cyclerate=%d low=%d before=%u after=%u disabled=%d",
			cyclerate, lowcycles ? 1 : 0, before, s_nBlockCycles, DEBUG_ONLY_DISABLE_BLOCKCYCLE_MASK);
	}
	if (s_nBlockCycles == 0) s_nBlockCycles = 1; // Ensure progress

	return scaled;
}

// =============================================================================
// [V3-features backport 2026-05-04] GTA VC gameplay 到達のための V3 mode 機能を
// v1 JIT (recCpu) base に opt-in env で integrate。
//
// 統一 env: iPSX2_GTAVC_FIXES=1 で 全機能 ON
// 個別 env: iPSX2_JIT_RUNTIME_BLK / iPSX2_JIT_INTERP_FALLBACK で個別制御
//
// Step 5: MTC0 mid-block cycle_mul 変化を runtime 反映、 iv2 と bit-exact 同等。
// V3.3a/b: EE non-branch opcode を iv2 byte-exact 化。
// =============================================================================

bool jit_gtavc_fixes_enabled() {
    static int s_flag = -1;
    if (s_flag < 0) {
        const char* e = std::getenv("iPSX2_GTAVC_FIXES");
        // [V15] 実機 ipa 用 default ON (= ship 構成と等価)、 env=0 で OFF override 可
        s_flag = (e && e[0] == '0') ? 0 : 1;
        Console.WriteLn("@@CFG@@ iPSX2_GTAVC_FIXES=%d (V3-features unified, default ON)", s_flag);
    }
    return s_flag != 0;
}

bool jit_runtime_blk_enabled() {
    static int s_flag = -1;
    if (s_flag < 0) {
        const char* e = std::getenv("iPSX2_JIT_RUNTIME_BLK");
        if (e && e[0]) s_flag = (e[0] != '0') ? 1 : 0;
        else           s_flag = jit_gtavc_fixes_enabled() ? 1 : 0;
        Console.WriteLn("@@CFG@@ iPSX2_JIT_RUNTIME_BLK=%d (Step 5 runtime cycle accumulator)", s_flag);
    }
    return s_flag != 0;
}

bool jit_interp_fallback_enabled() {
    static int s_flag = -1;
    if (s_flag < 0) {
        const char* e = std::getenv("iPSX2_JIT_INTERP_FALLBACK");
        // [V15] V14.7 ship 構成: default OFF (= V3.3a/b OFF、 全 native)、 env=1 で ON override 可
        s_flag = (e && e[0] && e[0] != '0') ? 1 : 0;
        Console.WriteLn("@@CFG@@ iPSX2_JIT_INTERP_FALLBACK=%d (V3.3a/b non-branch iv2 fallback)", s_flag);
    }
    return s_flag != 0;
}

// is_branch_opcode_for_v3: branch family は v1 emit 維持 (interp fallback しない)。
//   J/JAL/JR/JALR/B*/BC1*/SYSCALL/BREAK/ERET 等を識別。
static inline bool is_branch_opcode_for_v3(u32 op) {
    const u32 mainop = op >> 26;
    if (mainop == 0x02 || mainop == 0x03) return true;  // J / JAL
    if (mainop >= 0x04 && mainop <= 0x07) return true;  // BEQ / BNE / BLEZ / BGTZ
    if (mainop == 0x14 || mainop == 0x15) return true;  // BEQL / BNEL
    if (mainop == 0x16 || mainop == 0x17) return true;  // BLEZL / BGTZL
    if (mainop == 0x01) return true;                    // REGIMM (BLTZ / BGEZ etc)
    if (mainop == 0x11) {
        const u32 fmt = (op >> 21) & 0x1F;
        if (fmt == 0x08) return true;                   // BC1
    }
    if (mainop == 0x10) {
        const u32 fmt = (op >> 21) & 0x1F;
        if (fmt == 0x08) return true;                   // BC0
        const u32 funct = op & 0x3F;
        if ((op & 0xFFFFFFE0) == 0x42000000) {
            if (funct == 0x18) return true;             // ERET
        }
    }
    if (mainop == 0x00) {
        const u32 funct = op & 0x3F;
        if (funct == 0x08 || funct == 0x09) return true; // JR / JALR
        if (funct == 0x0C || funct == 0x0D) return true; // SYSCALL / BREAK
    }
    return false;
}

extern u32 cpuBlockCycles;  // Interpreter.cpp で定義 (V3 Step 5 backport で extern 化)

// armEmitFlushBlockCycles_runtime: block-end での runtime cycle flush (iv2 intUpdateCPUCycles 同等)。
//   PS2 R5900: cycle += max(1, cpuBlockCycles >> 3)、 残 3 bit。
void armEmitFlushBlockCycles_runtime(bool clear) {
    using namespace a64;
    armMoveAddressToReg(x16, &cpuBlockCycles);
    armAsm->Ldr(w14, MemOperand(x16));
    armAsm->Lsr(w15, w14, 3);
    armAsm->Mov(w17, 1);
    armAsm->Cmp(w15, 0);
    armAsm->Csel(w15, w17, w15, eq);
    armMoveAddressToReg(x16, &cpuRegs.cycle);
    armAsm->Ldr(w17, MemOperand(x16));
    armAsm->Add(w17, w17, w15);
    armAsm->Str(w17, MemOperand(x16));
    armMoveAddressToReg(x16, &cpuBlockCycles);
    armAsm->And(w14, w14, 7);
    armAsm->Str(w14, MemOperand(x16));
    if (clear) {
        s_nBlockCycles = 0;
    }
}

// armEmitPerInstCycleAdd_runtime: per-inst で cpuBlockCycles += scaled を runtime に追加。
static void armEmitPerInstCycleAdd_runtime(u32 raw_cycles) {
    using namespace a64;
    armMoveAddressToReg(x16, &cpuBlockCycles);
    armAsm->Ldr(w14, MemOperand(x16));
    armMoveAddressToReg(x17, &cpuRegs.CP0.r[16]);
    armAsm->Ldr(w15, MemOperand(x17));
    armAsm->Lsr(w15, w15, 18);
    armAsm->And(w15, w15, 1);
    armAsm->Mov(w17, 2);
    armAsm->Sub(w15, w17, w15);
    armAsm->Mov(w17, raw_cycles);
    armAsm->Mul(w15, w17, w15);
    armAsm->Add(w14, w14, w15);
    armMoveAddressToReg(x16, &cpuBlockCycles);
    armAsm->Str(w14, MemOperand(x16));
}

// armEmitPerInstCycleAdd: public entry。 jit_runtime_blk_enabled で gate。
void armEmitPerInstCycleAdd(u32 raw_cycles) {
    if (!jit_runtime_blk_enabled()) return;
    armEmitPerInstCycleAdd_runtime(raw_cycles);
}

// armEmitCycleAdd: block-end での cycle flush wrapper。
void armEmitCycleAdd(bool clear) {
    if (jit_runtime_blk_enabled()) {
        armEmitFlushBlockCycles_runtime(clear);
    } else {
        armAdd(PTR_CPU(cpuRegs.cycle), clear ? scaleblockcycles_clear() : scaleblockcycles());
    }
}

// Generates dynarec code for Event tests followed by a block dispatch (branch).
// Parameters:
//   newpc - address to jump to at the end of the block.  If newpc == 0xffffffff then
//   the jump is assumed to be to a register (dynamic).  For any other value the
//   jump is assumed to be static, in which case the block will be "hardlinked" after
//   the first time it's dispatched.
//
//   noDispatch - When set true, then jump to Dispatcher.  Used by the recs
//   for blocks which perform exception checks without branching (it's enabled by
//   setting "g_branch = 2";
extern "C" void vtlb_LogSelfLoop(u32 pc);
extern "C" void LogIBranch41048(u32 newpc, u32 cycle_before, u32 cycle_after, u32 next_event)
{
	static u32 s_count = 0;
	if (s_count >= 50)
		return;
	++s_count;
	Console.WriteLn("@@IBRANCH_41048@@ n=%u newpc=%08x cycle_before=%u cycle_after=%u nextEvent=%u",
		s_count, newpc, cycle_before, cycle_after, next_event);
}

// Used by block linking code to ensure block-link validity (checking against current page)
// Parameters:
//   newpc - address to jump to at the end of the block.
static void iBranchTest(u32 newpc)
{
	static bool s_probe_ibranch_41048_enabled = false;
	static bool s_disable_waitloop_41048_enabled = false;
	static bool s_disable_waitloop_all = false;
	static bool s_probe_ibranch_41048_cfg_logged = false;
	if (!s_probe_ibranch_41048_cfg_logged)
	{
		s_probe_ibranch_41048_cfg_logged = true;
		s_probe_ibranch_41048_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_PROBE_IBRANCH_41048", false);
		s_disable_waitloop_41048_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_EE41048_DISABLE_WAITLOOP", false);
		s_disable_waitloop_all = iPSX2_GetRuntimeEnvBool("iPSX2_DISABLE_WAITLOOP", false);
		Console.WriteLn("@@CFG@@ iPSX2_DISABLE_WAITLOOP=%d", s_disable_waitloop_all ? 1 : 0);
		Console.WriteLn("@@CFG@@ iPSX2_PROBE_IBRANCH_41048=%d iPSX2_EE41048_DISABLE_WAITLOOP=%d",
			s_probe_ibranch_41048_enabled ? 1 : 0, s_disable_waitloop_41048_enabled ? 1 : 0);
	}

	// Check the Event scheduler if our "cycle target" has been reached.
	// Equiv code to:
	//    cpuRegs.cycle += blockcycles;
	//    if ( cpuRegs.cycle > g_nextEventCycle ) { DoEvents(); }

	// [iter660_probe] @@IBRANCH_81FC0@@ probe: log WaitLoop path for OSDSYS idle loop
	// Removal condition: vsync>45 停止causeafter identified
	if (newpc == 0x00081FC0u || newpc == 0x00200DD8u) {
		static int s_ibranch_81fc0_n = 0;
		if (s_ibranch_81fc0_n < 5) {
			Console.WriteLn("@@IBRANCH_81FC0@@ n=%d pc=%08x blockFF=%d branchTo=%08x waitloop_cfg=%d nBlockCycles=%d",
				s_ibranch_81fc0_n++, newpc, s_nBlockFF ? 1 : 0, s_branchTo,
				EmuConfig.Speedhacks.WaitLoop ? 1 : 0, scaleblockcycles());
		}
	}

	const bool block_waitloop_41048 = (s_disable_waitloop_41048_enabled && newpc == 0x9FC41048);
	if (EmuConfig.Speedhacks.WaitLoop && s_nBlockFF && newpc == s_branchTo && !block_waitloop_41048 && !s_disable_waitloop_all)
	{
        // WaitLoop optimization: advance cycles to nextEventCycle immediately if we are in a wait loop
        armLoad(EAX, PTR_CPU(cpuRegs.nextEventCycle));
        
        // Calculate potential new cycle count
        // [V3 backport 2026-05-04] Step 5 runtime path or legacy s_nBlockCycles path
        armEmitCycleAdd(false);
        
        // Check if we passed nextEventCycle
        armLoadsw(EEX, PTR_CPU(cpuRegs.cycle));
        armAsm->Cmp(EAX, EEX);
        
        // Use Csel to pick max(cycle, nextEventCycle) - ensure we don't go backwards? 
        // Actually Android logic is: 
        // xCMP(eax, ptr32[&cpuRegs.cycle]); // cmp nextEventCycle, cycle
        // xCMOVS(eax, ptr32[&cpuRegs.cycle]); // if next < cycle (signed), mov cycle to eax. 
        // So eax becomes max(nextEventCycle, cycle). Wait, if next < cycle, taking cycle ensures we don't rewind.
        // If next >= cycle, we take next. So we advance TO nextEventCycle.
        armAsm->Csel(EAX, EEX, EAX, a64::Condition::mi);
        
        armStore(PTR_CPU(cpuRegs.cycle), EAX);

        armEmitJmp(DispatcherEvent);
	}
		else
		{
	        // Normal case
	        if (s_probe_ibranch_41048_enabled && (newpc == 0x9FC41048 || newpc == 0x9FC41060))
	        {
		        armLoad(EDX, PTR_CPU(cpuRegs.cycle));
	        }
	        // [V3 backport 2026-05-04] Step 5 runtime path or legacy s_nBlockCycles path
	        armEmitCycleAdd(false);
	        armLoadsw(EAX, PTR_CPU(cpuRegs.cycle));
	        if (s_probe_ibranch_41048_enabled && (newpc == 0x9FC41048 || newpc == 0x9FC41060))
	        {
		        armAsm->Push(a64::x0, a64::x1, a64::x2, a64::x3);
		        armAsm->Push(a64::x29, a64::lr);
		        armAsm->Mov(a64::w0, newpc);
		        armAsm->Mov(a64::w1, EDX);
		        armAsm->Mov(a64::w2, EAX);
		        armLoad(a64::w3, PTR_CPU(cpuRegs.nextEventCycle));
		        armEmitCall((void*)LogIBranch41048);
		        armAsm->Pop(a64::lr, a64::x29);
		        armAsm->Pop(a64::x3, a64::x2, a64::x1, a64::x0);
	        }
	        armAsm->Subs(EAX, EAX, armLoadsw(PTR_CPU(cpuRegs.nextEventCycle)));

        a64::Label labelSigned;
        armAsm->B(&labelSigned, a64::Condition::pl);

		// If cycle < nextEventCycle, continue execution (link to next block)
		if (newpc == 0xffffffff) {
#if iPSX2_ENABLE_JIT_LOGCALLS
            armAsm->Mov(a64::w0, 0x1);
            armLoad(a64::w1, PTR_CPU(cpuRegs.pc));
            armLoad(a64::w2, PTR_CPU(cpuRegs.pc));
            armAsm->Mov(a64::w3, newpc);
            armAsm->Mov(a64::w4, 0); // Safe fallback
            armEmitCall(reinterpret_cast<void*>(recLogNextPcProbe));
#endif // iPSX2_ENABLE_JIT_LOGCALLS
            armEmitJmp(DispatcherReg);
        }
		else {
            armAsm->Nop();
            recBlocks.Link(HWADDR(newpc), (s32*)armGetCurrentCodePointer()-1);
        }

        armBind(&labelSigned);

        // Events pending
        armEmitJmp(DispatcherEvent);
	}
}

// opcode 'code' modifies:
// 1: status
// 2: MAC
// 4: clip
int cop2flags(u32 code)
{
	if (code >> 26 != 022)
		return 0; // not COP2
	if ((code >> 25 & 1) == 0)
		return 0; // a branch or transfer instruction

	switch (code >> 2 & 15)
	{
		case 15:
			switch (code >> 6 & 0x1f)
			{
				case 4: // ITOF*
				case 5: // FTOI*
				case 12: // MOVE MR32
				case 13: // LQI SQI LQD SQD
				case 15: // MTIR MFIR ILWR ISWR
				case 16: // RNEXT RGET RINIT RXOR
					return 0;
				case 7: // MULAq, ABS, MULAi, CLIP
					if ((code & 3) == 1) // ABS
						return 0;
					if ((code & 3) == 3) // CLIP
						return 4;
					return 3;
				case 11: // SUBA, MSUBA, OPMULA, NOP
					if ((code & 3) == 3) // NOP
						return 0;
					return 3;
				case 14: // DIV, SQRT, RSQRT, WAITQ
					if ((code & 3) == 3) // WAITQ
						return 0;
					return 1; // but different timing, ugh
				default:
					break;
			}
			break;
		case 4: // MAXbc
		case 5: // MINbc
		case 12: // IADD, ISUB, IADDI
		case 13: // IAND, IOR
		case 14: // VCALLMS, VCALLMSR
			return 0;
		case 7:
			if ((code & 1) == 1) // MAXi, MINIi
				return 0;
			return 3;
		case 10:
			if ((code & 3) == 3) // MAX
				return 0;
			return 3;
		case 11:
			if ((code & 3) == 3) // MINI
				return 0;
			return 3;
		default:
			break;
	}
	return 3;
}

int COP2DivUnitTimings(u32 code)
{
	// Note: Cycles are off by 1 since the check ignores the actual op, so they are off by 1
	switch (code & 0x3FF)
	{
		case 0x3BC: // DIV
		case 0x3BD: // SQRT
			return 6;
		case 0x3BE: // RSQRT
			return 12;
		default:
			return 0; // Used mainly for WAITQ
	}
}

bool COP2IsQOP(u32 code)
{
	if (_Opcode_ != 022) // Not COP2 operation
		return false;

	if ((code & 0x3f) == 0x20) // VADDq
		return true;
	if ((code & 0x3f) == 0x21) // VMADDq
		return true;
	if ((code & 0x3f) == 0x24) // VSUBq
		return true;
	if ((code & 0x3f) == 0x25) // VMSUBq
		return true;
	if ((code & 0x3f) == 0x1C) // VMULq
		return true;
	if ((code & 0x7FF) == 0x1FC) // VMULAq
		return true;
	if ((code & 0x7FF) == 0x23C) // VADDAq
		return true;
	if ((code & 0x7FF) == 0x23D) // VMADDAq
		return true;
	if ((code & 0x7FF) == 0x27C) // VSUBAq
		return true;
	if ((code & 0x7FF) == 0x27D) // VMSUBAq
		return true;

	return false;
}


void dynarecCheckBreakpoint()
{
	u32 pc = cpuRegs.pc;
	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_EE, pc) != 0)
		return;

	const int bpFlags = isBreakpointNeeded(pc);
	bool hit = false;
	//check breakpoint at current pc
	if (bpFlags & 1)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_EE, pc);
		if (cond == NULL || cond->Evaluate())
		{
			hit = true;
		}
	}
	//check breakpoint in delay slot
	if (bpFlags & 2)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_EE, pc + 4);
		if (cond == NULL || cond->Evaluate())
			hit = true;
	}

	if (!hit)
		return;

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_EE);
	VMManager::SetPaused(true);
	recExitExecution();
}

void dynarecMemcheck(size_t i)
{
	const u32 op = memRead32(cpuRegs.pc);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);
	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_EE, pc) != 0)
		return;

	auto mc = CBreakPoints::GetMemChecks(BREAKPOINT_EE)[i];

	if (mc.hasCond)
	{
		if (!mc.cond.Evaluate())
			return;
	}

	if (mc.result & MEMCHECK_LOG)
	{
		if (opcode.flags & IS_STORE)
			DevCon.WriteLn("Hit store breakpoint @0x%x", cpuRegs.pc);
		else
			DevCon.WriteLn("Hit load breakpoint @0x%x", cpuRegs.pc);
	}

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_EE);
	VMManager::SetPaused(true);
	recExitExecution();
}

void recMemcheck(u32 op, u32 bits, bool store)
{
	iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);

	// compute accessed address
	_eeMoveGPRtoR(EAX, (op >> 21) & 0x1F);
	if (static_cast<s16>(op) != 0) {
//        xADD(ecx, static_cast<s16>(op));
        armAsm->Add(EAX, EAX, static_cast<s16>(op));
    }
	if (bits == 128) {
//        xAND(ecx, ~0x0F);
        armAsm->And(EAX, EAX, ~0x0F);
    }

//	xFastCall((void*)standardizeBreakpointAddress, ecx);
    armEmitCall(reinterpret_cast<const void*>(standardizeBreakpointAddress));
//	xMOV(ecx, eax);
    armAsm->Mov(ECX, EAX);
//	xMOV(edx, eax);
    armAsm->Mov(EDX, EAX);
//	xADD(edx, bits / 8);
    armAsm->Add(EDX, EDX, bits >> 3); // bits / 8

	// ecx = access address
	// edx = access address+size

	auto checks = CBreakPoints::GetMemChecks(BREAKPOINT_EE);
	for (size_t i = 0; i < checks.size(); i++)
	{
		if (checks[i].result == 0)
			continue;
		if ((checks[i].memCond & MEMCHECK_WRITE) == 0 && store)
			continue;
		if ((checks[i].memCond & MEMCHECK_READ) == 0 && !store)
			continue;

		// logic: memAddress < bpEnd && bpStart < memAddress+memSize

//		xMOV(eax, standardizeBreakpointAddress(checks[i].end));
        armAsm->Mov(EAX, standardizeBreakpointAddress(checks[i].end));
//		xCMP(ecx, eax); // address < end
        armAsm->Cmp(ECX, EAX);
//		xForwardJGE8 next1; // if address >= end then goto next1
        a64::Label next1;
        armAsm->B(&next1, a64::Condition::ge);

//		xMOV(eax, standardizeBreakpointAddress(checks[i].start));
        armAsm->Mov(EAX, standardizeBreakpointAddress(checks[i].start));
//		xCMP(eax, edx); // start < address+size
        armAsm->Cmp(EAX, EDX);
//		xForwardJGE8 next2; // if start >= address+size then goto next2
        a64::Label next2;
        armAsm->B(&next2, a64::Condition::ge);

		// hit the breakpoint
		if (checks[i].result & MEMCHECK_BREAK)
		{
//			xMOV(eax, i);
            armAsm->Mov(EAX, i);
//			xFastCall((void*)dynarecMemcheck, eax);
            armEmitCall(reinterpret_cast<void*>(dynarecMemcheck));
		}

//		next1.SetTarget();
        armBind(&next1);
//		next2.SetTarget();
        armBind(&next2);
	}
}

void encodeBreakpoint()
{
	if (isBreakpointNeeded(pc) != 0)
	{
		iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
//		xFastCall((void*)dynarecCheckBreakpoint);
        armEmitCall(reinterpret_cast<void*>(dynarecCheckBreakpoint));
	}
}

void encodeMemcheck()
{
	const int needed = isMemcheckNeeded(pc);
	if (needed == 0)
		return;

	const u32 op = memRead32(needed == 2 ? pc + 4 : pc);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	const bool store = (opcode.flags & IS_STORE) != 0;
	switch (opcode.flags & MEMTYPE_MASK)
	{
		case MEMTYPE_BYTE:
			recMemcheck(op, 8, store);
			break;
		case MEMTYPE_HALF:
			recMemcheck(op, 16, store);
			break;
		case MEMTYPE_WORD:
			recMemcheck(op, 32, store);
			break;
		case MEMTYPE_DWORD:
			recMemcheck(op, 64, store);
			break;
		case MEMTYPE_QWORD:
			recMemcheck(op, 128, store);
			break;
	}
}

extern u32 g_current_diag_pc;

void recompileNextInstruction(bool delayslot, bool swapped_delay_slot)
{
    g_current_diag_pc = pc;

    // Diagnosis (2): Target Block Entry State
    if (pc == 0xbfc0207c) {
        static bool s_log_target = false;
        if (!s_log_target) {
            s_log_target = true;
            bool is_const = GPR_IS_CONST1(8);
            int hostReg = _checkX86reg(X86TYPE_GPR, 8, 0);
            bool is_dirty = (hostReg >= 0) && (x86regs[hostReg].mode & MODE_WRITE);
            u32 constVal = (u32)g_cpuConstRegs[8].UL[0];
            u32 memVal = (u32)cpuRegs.GPR.r[8].UL[0];
            
            Console.WriteLn("@@BR_TARGET_IN@@ pc=%08x t0={isConst=%d constVal=%08x mapped=%d dirty=%d host=%d} gpr_mem=%08x",
                pc, is_const, constVal, (hostReg>=0), is_dirty, hostReg, memVal);
        }
    }

	if (EmuConfig.EnablePatches)
		Patch::ApplyDynamicPatches(pc);

	// add breakpoint
	if (!delayslot)
	{
		encodeBreakpoint();
		encodeMemcheck();
	}
	else
	{
#ifdef DUMP_BLOCKS
		std::string disasm;
		disR5900Fasm(disasm, *(u32*)PSM(pc), pc, false);
		fprintf(stderr, "Compiling delay slot %08X %s\n", pc, disasm.c_str());
#endif
        // [iter683] JIT_DELAY log removed — was spamming 800K+ lines and killing perf
        
        // Diagnosis (1): Mark that we executed the delay slot
        // Call helper to set g_delayRan = true
#if iPSX2_ENABLE_JIT_LOGCALLS
        armEmitCall(reinterpret_cast<void*>(recLogDelayExecution));
#endif // iPSX2_ENABLE_JIT_LOGCALLS

		_clearNeededX86regs();
		_clearNeededXMMregs();
	}

	s_pCode = (int*)PSM(pc);
    if (!s_pCode) {
        Console.Warning("recRecompile: PSM(pc) failed for %08x", pc);
        return;
    }
	pxAssert(s_pCode);

#if 0
	// acts as a tag for delimiting recompiled instructions when viewing x86 disasm.
	if (IsDevBuild)
		xNOP();
	if (IsDebugBuild)
		xMOV(eax, pc);
#endif

	const int old_code = cpuRegs.code;
	EEINST* old_inst_info = g_pCurInstInfo;

	cpuRegs.code = *(int*)s_pCode;

    
	if (!delayslot)
	{
		pc += 4;
		g_cpuFlushedPC = false;
		g_cpuFlushedCode = false;
	}
	else
	{
		// increment after recompiling so that pc points to the branch during recompilation
		g_recompilingDelaySlot = true;
	}

	g_pCurInstInfo++;

    // pc might be past s_nEndBlock if the last instruction in the block is a DI.
    u32 s_nEndBlock_pc = (s_nEndBlock - pc) / 4 + 1;
    if (pc <= s_nEndBlock && (g_pCurInstInfo + s_nEndBlock_pc) <= s_pInstCache + s_nInstCacheSize)
    {
        int i, count;
        for (i = 0; i < iREGCNT_GPR; ++i)
        {
            if (x86regs[i].inuse)
            {
                count = _recIsRegReadOrWritten(g_pCurInstInfo, s_nEndBlock_pc, x86regs[i].type, x86regs[i].reg);
                if (count > 0)
                    x86regs[i].counter = 1000 - count;
                else
                    x86regs[i].counter = 0;
            }
        }

        for (i = 0; i < iREGCNT_XMM; ++i)
        {
            if (xmmregs[i].inuse)
            {
                count = _recIsRegReadOrWritten(g_pCurInstInfo, s_nEndBlock_pc, xmmregs[i].type, xmmregs[i].reg);
                if (count > 0)
                    xmmregs[i].counter = 1000 - count;
                else
                    xmmregs[i].counter = 0;
            }
        }
    }

	if (g_pCurInstInfo->info & EEINST_COP2_FLUSH_VU0_REGISTERS)
	{
		RALOG("Flushing cop2 registers\n");
		_flushCOP2regs();
	}

	const OPCODE& opcode = GetCurrentInstruction();
    
    // [iter683] SYNC compile log removed — was spamming and killing perf

	//pxAssert( !(g_pCurInstInfo->info & EEINSTINFO_NOREC) );
	//Console.Warning("opcode name = %s, it's cycles = %d\n",opcode.Name,opcode.cycles);
	// if this instruction is a jump or a branch, exit right away
	if (delayslot)
	{
		bool check_branch_delay = false;
		switch (_Opcode_)
		{
			case 0:
				switch (_Funct_)
				{
					case 8: // jr
					case 9: // jalr
						check_branch_delay = true;
						break;
				}
				break;

			case 1:
				switch (_Rt_)
				{
					case 0:
					case 1:
					case 2:
					case 3:
					case 0x10:
					case 0x11:
					case 0x12:
					case 0x13:
						check_branch_delay = true;
						break;
				}
				break;

			case 2:
			case 3:
			case 4:
			case 5:
			case 6:
			case 7:
			case 0x14:
			case 0x15:
			case 0x16:
			case 0x17:
				check_branch_delay = true;
				break;
		}
		// Check for branch in delay slot, new code by FlatOut.
		// Gregory tested this in 2017 using the ps2autotests suite and remarked "So far we return 1 (even with this PR), and the HW 2.
		// Original PR and discussion at https://github.com/PCSX2/pcsx2/pull/1783 so we don't forget this information.
		if (check_branch_delay)
		{
			DevCon.Warning("Branch %x in delay slot!", cpuRegs.code);
            // Incorrect location for valid delay slot logging, removing
			_clearNeededX86regs();
			_clearNeededXMMregs();
			pc += 4;
			g_cpuFlushedPC = false;
			g_cpuFlushedCode = false;
			if (g_maySignalException) {
//                xAND(ptr32[&cpuRegs.CP0.n.Cause], ~(1 << 31)); // BD
                armAnd(PTR_CPU(cpuRegs.CP0.n.Cause), ~(1 << 31));
            }

			g_recompilingDelaySlot = false;
			return;
		}
	}
	// Check for NOP
	if (cpuRegs.code == 0x00000000)
	{
		// Note: Tests on a ps2 suggested more like 5 cycles for a NOP. But there's many factors in this..
		s_nBlockCycles += 9 * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
		// [V3 backport 2026-05-04] Step 5 runtime per-inst cycle add (NOP path)
		armEmitPerInstCycleAdd(9);
	}
	else
	{
		//If the COP0 DIE bit is disabled, cycles should be doubled.
		// [TEMP_DIAG] @@DIE_BIT_CHECK@@ — log DIE bit at compile time for OSDSYS blocks
		// Removal condition: DIE bit 仮説検証後
		{
			static u32 s_die_log_cnt = 0;
			u32 die_bit = (cpuRegs.CP0.n.Config >> 18) & 0x1;
			u32 hw_pc = HWADDR(pc);
			if (hw_pc >= 0x210000 && hw_pc < 0x270000 && s_die_log_cnt < 10) {
				Console.WriteLn("@@DIE_BIT_CHECK@@ pc=%08x die=%u config=%08x mult=%u block_start=%08x",
					pc, die_bit, cpuRegs.CP0.n.Config, 2 - die_bit, s_recblock_startpc);
				s_die_log_cnt++;
			}
		}
		s_nBlockCycles += opcode.cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
		if (opcode.recompile) {
			// [V3.3a/b backport 2026-05-04] non-branch opcode を iv2 byte-exact interp fallback。
			// branch family は v1 emit 維持 (PC writeback / dispatch / delay slot framework 整合性)。
			const u32 op_v12 = cpuRegs.code;
			const bool is_branch_v12 = is_branch_opcode_for_v3(op_v12);
			bool path_v3_v12 = false;
			if (!is_branch_v12 && opcode.interpret) {
				path_v3_v12 = jit_interp_fallback_enabled();
			}
			if (__builtin_expect(path_v3_v12, 0)) {
				iFlushCall(FLUSH_INTERPRETER);
				recCall(opcode.interpret);
			} else {
				opcode.recompile();
			}
			// [V3 backport 2026-05-04] Step 5 runtime per-inst cycle add (non-NOP path)
			armEmitPerInstCycleAdd(opcode.cycles);
		} else {
			// [iPSX2] Diagnostic for missing handlers
			static bool logged = false;
			if (!logged) {
				logged = true;
				Console.WriteLn("@@REC_NULL@@ pc=%08x op=%08x name=%s", cpuRegs.pc, cpuRegs.code, opcode.Name);
			}
			// Fallback to interpreter or just skip to avoid crash? 
			// Safest to just return/abort cleanly in this context or let the loop continue (effectively NOP)
			// But better to stop or we might infinite loop.
			// Ideally we should fallback to interpreter for this block but we are deep in recompiler.
			// For now, let's just not call it.
			// Actually, if we don't compile anything, we might execute garbage or loop. 
			// But we avoided the crash.
		}
	}

	if (!swapped_delay_slot)
	{
		_clearNeededX86regs();
		_clearNeededXMMregs();
	}
	_validateRegs();

	if (delayslot)
	{
		pc += 4;
		g_cpuFlushedPC = false;
		g_cpuFlushedCode = false;
		if (g_maySignalException) {
//            xAND(ptr32[&cpuRegs.CP0.n.Cause], ~(1 << 31)); // BD
            armAnd(PTR_CPU(cpuRegs.CP0.n.Cause), ~(1 << 31));
        }
		g_recompilingDelaySlot = false;
	}

	g_maySignalException = false;

	// Stalls normally occur as necessary on the R5900, but when using COP2 (VU0 macro mode),
	// there are some exceptions to this.  We probably don't even know all of them.
	// We emulate the R5900 as if it was fully interlocked (which is mostly true), and
	// in fact we don't have good enough cycle counting to do otherwise.  So for now,
	// we'll try to identify problematic code in games create patches.
	// Look ahead is probably the most reasonable way to do this given the deficiency
	// of our cycle counting.  Real cycle counting is complicated and will have to wait.
	// Instead of counting the cycles I'm going to count instructions.  There are a lot of
	// classes of instructions which use different resources and specific restrictions on
	// coissuing but this is just for printing a warning so I'll simplify.
	// Even when simplified this is not simple and it is very very wrong.

	// CFC2 flag register after arithmetic operation: 5 cycles
	// CTC2 flag register after arithmetic operation... um.  TODO.
	// CFC2 address register after increment/decrement load/store: 5 cycles TODO
	// CTC2 CMSAR0, VCALLMSR CMSAR0: 3 cycles but I want to do some tests.
	// Don't even want to think about DIV, SQRT, RSQRT now.

	if (_Opcode_ == 022) // COP2
	{
		if ((cpuRegs.code >> 25 & 1) == 1 && (cpuRegs.code >> 2 & 0x1ff) == 0xdf) // [LS]Q[DI]
			; // TODO
		else if (_Rs_ == 6) // CTC2
			; // TODO
		else if ((cpuRegs.code & 0x7FC) == 0x3BC) // DIV/RSQRT/SQRT/WAITQ
		{
			int cycles = COP2DivUnitTimings(cpuRegs.code);
			for (u32 p = pc; cycles > 0 && p < s_nEndBlock; p += 4, cycles--)
			{
				cpuRegs.code = memRead32(p);

				if ((_Opcode_ == 022) && (cpuRegs.code & 0x7FC) == 0x3BC) // WaitQ or another DIV op hit (stalled), we're safe
					break;

				else if (COP2IsQOP(cpuRegs.code))
				{
					Console.Warning("Possible incorrect Q value used in COP2. If the game is broken, please report to https://github.com/pcsx2/pcsx2.");
					for (u32 i = s_pCurBlockEx->startpc; i < s_nEndBlock; i += 4)
					{
						std::string disasm = "";
						disR5900Fasm(disasm, memRead32(i), i, false);
						Console.Warning("%x %s%08X %s", i, i == pc - 4 ? "*" : i == p ? "=" :
																						" ",
							memRead32(i), disasm.c_str());
					}
					break;
				}
			}
		}
		else
		{
			int s = cop2flags(cpuRegs.code);
			int all_count = 0, cop2o_count = 0, cop2m_count = 0;
			for (u32 p = pc; s != 0 && p < s_nEndBlock && all_count < 10 && cop2m_count < 5 && cop2o_count < 4; p += 4)
			{
				// I am so sorry.
				cpuRegs.code = memRead32(p);
				if (_Opcode_ == 022 && _Rs_ == 2) // CFC2
					// rd is fs
					if ((_Rd_ == 16 && s & 1) || (_Rd_ == 17 && s & 2) || (_Rd_ == 18 && s & 4))
					{
						std::string disasm;
						Console.Warning("Possible old value used in COP2 code. If the game is broken, please report to https://github.com/pcsx2/pcsx2.");
						for (u32 i = s_pCurBlockEx->startpc; i < s_nEndBlock; i += 4)
						{
							disasm = "";
							disR5900Fasm(disasm, memRead32(i), i, false);
							Console.Warning("%x %s%08X %s", i, i == pc - 4 ? "*" : i == p ? "=" :
																							" ",
								memRead32(i), disasm.c_str());
						}
						break;
					}
				s &= ~cop2flags(cpuRegs.code);
				all_count++;
				if (_Opcode_ == 022 && _Rs_ == 8) // COP2 branch, handled incorrectly like most things
					;
				else if (_Opcode_ == 022 && (cpuRegs.code >> 25 & 1) == 0)
					cop2m_count++;
				else if (_Opcode_ == 022)
					cop2o_count++;
			}
		}
	}
	cpuRegs.code = *s_pCode;

	if (swapped_delay_slot)
	{
		cpuRegs.code = old_code;
		g_pCurInstInfo = old_inst_info;
	}
}

// (Called from recompiled code)]
// This function is called from the recompiler prior to starting execution of *every* recompiled block.
// Calling of this function can be enabled or disabled through the use of EmuConfig.Recompiler.PreBlockChecks
#ifdef TRACE_BLOCKS
static void PreBlockCheck(u32 blockpc)
{
#if 0
	static FILE* fp = nullptr;
	static bool fp_opened = false;
	if (!fp_opened && cpuRegs.cycle >= 0)
	{
		fp = std::fopen("C:\\Dumps\\comp\\reglog.txt", "wb");
		fp_opened = true;
	}
	if (fp)
	{
		u32 hash = crc32(0, (Bytef*)&cpuRegs, offsetof(cpuRegisters, pc));
		u32 hashf = crc32(0, (Bytef*)&fpuRegs, sizeof(fpuRegisters));
		u32 hashi = crc32(0, (Bytef*)&VU0, offsetof(VURegs, idx));

#if 1
		std::fprintf(fp, "%08X (%u; %08X; %08X; %08X):", cpuRegs.pc, cpuRegs.cycle, hash, hashf, hashi);
		for (int i = 0; i < 34; i++)
		{
			std::fprintf(fp, " %s: %08X%08X%08X%08X", R3000A::disRNameGPR[i], cpuRegs.GPR.r[i].UL[3], cpuRegs.GPR.r[i].UL[2], cpuRegs.GPR.r[i].UL[1], cpuRegs.GPR.r[i].UL[0]);
		}
#if 1
		std::fprintf(fp, "\nFPR: CR: %08X ACC: %08X", fpuRegs.fprc[31], fpuRegs.ACC.UL);
		for (int i = 0; i < 32; i++)
			std::fprintf(fp, " %08X", fpuRegs.fpr[i].UL);
#endif
#if 1
		std::fprintf(fp, "\nVF: ");
		for (int i = 0; i < 32; i++)
			std::fprintf(fp, " %u: %08X %08X %08X %08X", i, VU0.VF[i].UL[0], VU0.VF[i].UL[1], VU0.VF[i].UL[2], VU0.VF[i].UL[3]);
		std::fprintf(fp, "\nVI: ");
		for (int i = 0; i < 32; i++)
			std::fprintf(fp, " %u: %08X", i, VU0.VI[i].UL);
		std::fprintf(fp, "\nACC: %08X %08X %08X %08X Q: %08X P: %08X", VU0.ACC.UL[0], VU0.ACC.UL[1], VU0.ACC.UL[2], VU0.ACC.UL[3], VU0.q.UL, VU0.p.UL);
		std::fprintf(fp, " MAC %08X %08X %08X %08X", VU0.micro_macflags[3], VU0.micro_macflags[2], VU0.micro_macflags[1], VU0.micro_macflags[0]);
		std::fprintf(fp, " CLIP %08X %08X %08X %08X", VU0.micro_clipflags[3], VU0.micro_clipflags[2], VU0.micro_clipflags[1], VU0.micro_clipflags[0]);
		std::fprintf(fp, " STATUS %08X %08X %08X %08X", VU0.micro_statusflags[3], VU0.micro_statusflags[2], VU0.micro_statusflags[1], VU0.micro_statusflags[0]);
#endif
		std::fprintf(fp, "\n");
#else
		std::fprintf(fp, "%08X (%u): %08X %08X %08X\n", cpuRegs.pc, cpuRegs.cycle, hash, hashf, hashi);
#endif
		// std::fflush(fp);
	}
#endif
#if 0
	if (cpuRegs.cycle == 0)
		pauseAAA();
#endif
}
#endif

// Called when a block under manual protection fails it's pre-execution integrity check.
// (meaning the actual code area has been modified -- ie dynamic modules being loaded or,
//  less likely, self-modifying code)
void dyna_block_discard(u32 start, u32 sz)
{
	s_dyna_block_discard_count.fetch_add(1, std::memory_order_relaxed);
#ifdef PCSX2_DEVBUILD
	eeRecPerfLog.Write(Color_StrongGray, "Clearing Manual Block @ 0x%08X  [size=%d]", start, sz * 4);
#endif
	recClear(start, sz);
}

// called when a page under manual protection has been run enough times to be a candidate
// for being reset under the faster vtlb write protection.  All blocks in the page are cleared
// and the block is re-assigned for write protection.
void dyna_page_reset(u32 start, u32 sz)
{
	s_dyna_page_reset_count.fetch_add(1, std::memory_order_relaxed);
	recClear(start & ~0xfffUL, 0x400);
	manual_counter[start >> 12]++;
	mmap_MarkCountedRamPage(start);
}

static void memory_protect_recompiled_code(u32 startpc, u32 size)
{
	u32 inpage_ptr = HWADDR(startpc);
	const u32 inpage_sz = size << 2; // size * 4

	// The kernel context register is stored @ 0x800010C0-0x80001300
	// The EENULL thread context register is stored @ 0x81000-....
    u32 startpc_lsr_12 = (startpc >> 12);
	const bool contains_thread_stack = (startpc_lsr_12 == 0x81) || (startpc_lsr_12 == 0x80001);

	// note: blocks are guaranteed to reside within the confines of a single page.
	const vtlb_ProtectionMode PageType = contains_thread_stack ? ProtMode_Manual : mmap_GetRamPageInfo(inpage_ptr);

	switch (PageType)
	{
		case ProtMode_NotRequired:
			break;

		case ProtMode_None:
		case ProtMode_Write:
			mmap_MarkCountedRamPage(inpage_ptr);
			manual_page[inpage_ptr >> 12] = 0;
			break;

		case ProtMode_Manual:
//			xMOV(arg1regd, inpage_ptr);
            armAsm->Mov(EAX, inpage_ptr);
//			xMOV(arg2regd, inpage_sz / 4);
            armAsm->Mov(ECX, inpage_sz >> 2);
			//xMOV( eax, startpc );		// uncomment this to access startpc (as eax) in dyna_block_discard

            u32 lpc_addr;
			u32 lpc = inpage_ptr;
			u32 stg = inpage_sz;

            armAsm->Ldr(RSCRATCHADDR, PTR_CPU(vtlbdata.pmap));

			while (stg > 0)
			{
//				xCMP(ptr32[PSM(lpc)], *(u32*)PSM(lpc));

                lpc_addr = lpc & 0x1fffffff;
                armAsm->Add(RXVIXLSCRATCH, RSCRATCHADDR, lpc_addr);
                armAsm->Ldr(EDX, a64::MemOperand(RXVIXLSCRATCH));
                armAsm->Cmp(EDX, *(u32*)vtlb_GetPhyPtr(lpc_addr));

//				xJNE(DispatchBlockDiscard);
                armEmitCondBranch(a64::Condition::ne, DispatchBlockDiscard);

				stg -= 4;
				lpc += 4;
			}

			// Tweakpoint!  3 is a 'magic' number representing the number of times a counted block
			// an uncounted (permanent)
			// manual block.  Higher thresholds result in more recompilations for blocks that share code
			// and data on the same page.  Side effects of a lower threshold: over extended gameplay
			// with several map changes, a game's overall performance could degrade.

			// (ideally, perhaps, manual_counter should be reset to 0 every few minutes?)

			if (!contains_thread_stack && manual_counter[inpage_ptr >> 12] <= 3)
			{
				// Counted blocks add a weighted (by block size) value into manual_page each time they're
				// run.  If the block gets run a lot, it resets and re-protects itself in the hope
				// that whatever forced it to be manually-checked before was a 1-time deal.

				// Counted blocks have a secondary threshold check in manual_counter, which forces a block
				// to 'uncounted' mode if it's recompiled several times.  This protects against excessive
				// recompilation of blocks that reside on the same codepage as data.

				// fixme? Currently this algo is kinda dumb and results in the forced recompilation of a
				// lot of blocks before it decides to mark a 'busy' page as uncounted.  There might be
				// be a more clever approach that could streamline this process, by doing a first-pass
				// test using the vtlb memory protection (without recompilation!) to reprotect a counted
				// block.  But unless a new algo is relatively simple in implementation, it's probably
				// not worth the effort (tests show that we have lots of recompiler memory to spare, and
				// that the current amount of recompilation is fairly cheap).

//				xADD(ptr16[&manual_page[inpage_ptr >> 12]], size);
                armAddsh(EEX, &manual_page[inpage_ptr >> 12], size, true);
//				xJC(DispatchPageReset);
                armEmitCondBranch(a64::Condition::cs, DispatchPageReset);

#ifdef PCSX2_DEVBUILD
				// note: clearcnt is measured per-page, not per-block!
				eeRecPerfLog.Write("Manual block @ %08X : size =%3d  page/offs = 0x%05X/0x%03X  inpgsz = %d  clearcnt = %d",
					startpc, size, inpage_ptr >> 12, inpage_ptr & 0xfff, inpage_sz, manual_counter[inpage_ptr >> 12]);
#endif
			}
#ifdef PCSX2_DEVBUILD
			else
			{
				eeRecPerfLog.Write("Uncounted Manual block @ 0x%08X : size =%3d page/offs = 0x%05X/0x%03X  inpgsz = %d",
					startpc, size, inpage_ptr >> 12, inpage_ptr & 0xfff, inpage_sz);
			}
#endif
			break;
	}
}

// Skip MPEG Game-Fix
static bool skipMPEG_By_Pattern(u32 sPC)
{

	if (!CHECK_SKIPMPEGHACK)
		return 0;

	// sceMpegIsEnd: lw reg, 0x40(a0); jr ra; lw v0, 0(reg)
	if ((s_nEndBlock == sPC + 12) && (memRead32(sPC + 4) == 0x03e00008))
	{
		const u32 code = memRead32(sPC);
		const u32 p1 = 0x8c800040;
		const u32 p2 = 0x8c020000 | (code & 0x1f0000) << 5;
		if ((code & 0xffe0ffff) != p1)
			return 0;
		if (memRead32(sPC + 8) != p2)
			return 0;
//		xMOV(ptr32[&cpuRegs.GPR.n.v0.UL[0]], 1);
        armStore(PTR_CPU(cpuRegs.GPR.n.v0.UL[0]), 1);
//		xMOV(ptr32[&cpuRegs.GPR.n.v0.UL[1]], 0);
        armStore(PTR_CPU(cpuRegs.GPR.n.v0.UL[1]), 0);
//		xMOV(eax, ptr32[&cpuRegs.GPR.n.ra.UL[0]]);
        armLoad(EAX, PTR_CPU(cpuRegs.GPR.n.ra.UL[0]));
//		xMOV(ptr32[&cpuRegs.pc], eax);
        armStore(PTR_CPU(cpuRegs.pc), EAX);
		iBranchTest();
		g_branch = 1;
		pc = s_nEndBlock;
		Console.WriteLn(Color_StrongGreen, "sceMpegIsEnd pattern found! Recompiling skip video fix...");
		return 1;
	}
	return 0;
}

extern "C" void Debug_AnalyzeBiosHang(u32 pc) {
    static bool analyzed = false;
    static int loop_count = 0;
    
    loop_count++;
    
    if (!analyzed) {
         Console.WriteLn("--- BIOS HANG ANALYSIS START @ PC=%08x ---", pc);
         // Dump Instructions
         for (u32 addr = pc - 16; addr <= pc + 32; addr += 4) {
             u32 code = memRead32(addr);
             Console.WriteLn("MEM[%08x] = %08x %s", addr, code, addr == pc ? "<-- PC" : "");
         }
         
         // Dump Registers
         Console.WriteLn("CP0.Status = %08x", cpuRegs.CP0.n.Status.val);
         Console.WriteLn("CP0.Cause  = %08x", cpuRegs.CP0.n.Cause);
         Console.WriteLn("CP0.EPC    = %08x", cpuRegs.CP0.n.EPC);
         Console.WriteLn("CP0.ErrorEPC= %08x", cpuRegs.CP0.n.ErrorEPC);
         Console.WriteLn("INTC_STAT  = %08x", psHu32(INTC_STAT));
         Console.WriteLn("INTC_MASK  = %08x", psHu32(INTC_MASK));
         
         analyzed = true;
    }
    
    // Task 4: Force IE experiment
    if (loop_count == 1000) {
         if (!(cpuRegs.CP0.n.Status.val & 1)) { // If IE is 0
             Console.WriteLn("--- EXPERIMENT: Forcing IE=1 at loop 1000 ---");
             cpuRegs.CP0.n.Status.val |= 1;
         }
    }
}

extern "C" void Debug_LogStatusChange(u32 pc, u32 old_val, u32 new_val) {
    Console.WriteLn("DEBUG: Status Change @ PC=%08x: %08x -> %08x", pc, old_val, new_val);
}

static bool recSkipTimeoutLoop(s32 reg, bool is_timeout_loop)
{
	if (!EmuConfig.Speedhacks.WaitLoop || !is_timeout_loop)
		return false;

	DevCon.WriteLn("[EE] Skipping timeout loop at 0x%08X -> 0x%08X", s_pCurBlockEx->startpc, s_nEndBlock);

	// basically, if the time it takes the loop to run is shorter than the
	// time to the next event, then we want to skip ahead to the event, but
	// update v0 to reflect how long the loop would have run for.

	// if (cycle >= nextEventCycle) { jump to dispatcher, we're running late }
	// new_cycles = min(v0 * 8, nextEventCycle)
	// new_v0 = (new_cycles - cycles) / 8
	// if new_v0 > 0 { jump to dispatcher because loop exited early }
	// else new_v0 is 0, so exit loop

//	xMOV(ebx, ptr32[&cpuRegs.cycle]); // ebx = cycle
    armLoad(EBX, PTR_CPU(cpuRegs.cycle));
//	xMOV(ecx, ptr32[&cpuRegs.nextEventCycle]); // ecx = nextEventCycle
    armLoad(ECX, PTR_CPU(cpuRegs.nextEventCycle));
//	xCMP(ebx, ecx);
    armAsm->Cmp(EBX, ECX);
	//xJAE((void*)DispatcherEvent); // jump to dispatcher if event immediately

	// TODO: In the case where nextEventCycle < cycle because it's overflowed, tack 8
	// cycles onto the event count, so hopefully it'll wrap around. This is pretty
	// gross, but until we switch to 64-bit counters, not many better options.
//	xForwardJB8 not_dispatcher;
    a64::Label not_dispatcher;
    armAsm->B(&not_dispatcher, a64::Condition::cc);
//	xADD(ebx, 8);
    armAsm->Add(EBX, EBX, 8);
//	xMOV(ptr32[&cpuRegs.cycle], ebx);
    armStore(PTR_CPU(cpuRegs.cycle), EBX);
//	xJMP((void*)DispatcherEvent);
    armEmitJmp(DispatcherEvent);
//	not_dispatcher.SetTarget();
    armBind(&not_dispatcher);

//	xMOV(edx, ptr32[&cpuRegs.GPR.r[reg].UL[0]]); // eax = v0
    armLoad(EDX, PTR_CPU(cpuRegs.GPR.r[reg].UL[0]));
//	xLEA(rax, ptrNative[rdx * 8 + rbx]); // edx = v0 * 8 + cycle
    armAsm->Add(RAX, RBX, a64::Operand(RDX, a64::LSL, 3));
//	xCMP(rcx, rax);
    armAsm->Cmp(RCX, RAX);
//	xCMOVB(rax, rcx); // eax = new_cycles = min(v8 * 8, nextEventCycle)
    armAsm->Csel(RAX, RCX, RAX, a64::Condition::cc);
//	xMOV(ptr32[&cpuRegs.cycle], eax); // writeback new_cycles
    armStore(PTR_CPU(cpuRegs.cycle), EAX);
//	xSUB(eax, ebx); // new_cycles -= cycle
    armAsm->Sub(EAX, EAX, EBX);
//	xSHR(eax, 3); // compute new v0 value
    armAsm->Lsr(EAX, EAX, 3);
//	xSUB(edx, eax); // v0 -= cycle_diff
    armAsm->Subs(EDX, EDX, EAX);
//	xMOV(ptr32[&cpuRegs.GPR.r[reg].UL[0]], edx); // write back new value of v0
    armStore(PTR_CPU(cpuRegs.GPR.r[reg].UL[0]), EDX);
//	xJNZ((void*)DispatcherEvent); // jump to dispatcher if new v0 is not zero (i.e. an event)
    armEmitCondBranch(a64::ne, DispatcherEvent);
//	xMOV(ptr32[&cpuRegs.pc], s_nEndBlock); // otherwise end of loop
    armStore(PTR_CPU(cpuRegs.pc), s_nEndBlock);
//	recBlocks.Link(HWADDR(s_nEndBlock), xJcc32());
    armAsm->Nop();
    recBlocks.Link(HWADDR(s_nEndBlock), (s32*)armGetCurrentCodePointer()-1);

	g_branch = 1;
	pc = s_nEndBlock;

	return true;
}

// Extern for fallback
extern "C" void intExecute();
extern "C" void vtlb_LogVTLBAccess(u32 addr, u32 pc, bool is_write);
extern "C" void iPSX2_DumpLfCmpStoreRing();

// [iPSX2] TRUTH BLOCK V3

// [iter400] @@SUBFUNC_EPILOG_RT@@ RUNTIME probe: called via JIT-emitted recCall
// reads cpuRegs.sp at the moment block 0x00201434 first EXECUTES (not compiles)
static void probe_subfunc_epilog_rt()
{
	static bool s_rt_logged = false;
	if (!s_rt_logged) {
		s_rt_logged = true;
		Console.WriteLn("@@SUBFUNC_EPILOG_RT@@ sp=%08x ra=%08x (RUNTIME: block 0x00201434 first execution)",
			cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0]);
	}
}

// [iter401] @@SUBFUNC_PROLOG_RT@@ RUNTIME probe: reads cpuRegs.sp at moment block 0x00201404 STARTS
// If sp=0x01effee0 here, the ADDIU r29,r29,-32 JIT code runs but somehow writes 0 back to cpuRegs.r[29].
// If sp=0 here, the corruption happened BEFORE block 0x00201404 executes.
static void probe_subfunc_prolog_rt()
{
	static bool s_rt_logged = false;
	if (!s_rt_logged) {
		s_rt_logged = true;
		Console.WriteLn("@@SUBFUNC_PROLOG_RT@@ sp=%08x ra=%08x r4=%08x (RUNTIME: block 0x00201404 first execution)",
			cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0]);
	}
}

// [R102] Interpreter fallback for specific PC ranges (binary search diagnostic)
static int s_interp_range_flag = -1;
static bool isInterpRangeBlock(u32 pc) {
    if (s_interp_range_flag < 0) {
        const char* env = getenv("iPSX2_INTERP_RANGE");
        s_interp_range_flag = (env && atoi(env)) ? 1 : 0;
    }
    // [R103] Always interpret BSS zeroing block — workaround for JIT bug
    // that causes FF10 warm reboot failure. TODO: find root cause in JIT.
    // [R103] Disabled — kernel interpreter doesn't fix the 0x80001578 issue.
    // The bug is in game code setting IE=0 before entering kernel.
    // fastboot=true works as a workaround.
    (void)pc;
        return true;
    return false;
}

static void recRecompile(const u32 startpc)
{
    // [R103] Compile-time log + code dump for bootstrap blocks
    // (= env iPSX2_DEBUG_VERBOSE=1 で復活、 release default suppress)
    if (iPSX2_IsDebugVerbose() && startpc >= 0x00100000u && startpc < 0x00100100u) {
        static int s_100000_n = 0;
        Console.WriteLn(Color_Red, "[R103_BLOCK] startpc=%08x compile #%d cycle=%u",
            startpc, s_100000_n, cpuRegs.cycle);
        if (s_100000_n < 30) {
            // Dump code at block startpc
            Console.WriteLn("[R103_CODE] %08x:", startpc);
            for (u32 a = startpc; a < startpc + 0x20 && a < 0x100100; a += 4) {
                u32* p = (u32*)PSM(a);
                Console.WriteLn("  %08x: %08x", a, p ? *p : 0);
            }
        }
        s_100000_n++;
    }

    s_recompile_cnt.fetch_add(1, std::memory_order_relaxed);
    static int s_recomp_diag_enabled = -1;
    if (s_recomp_diag_enabled == -1)
    {
        const bool enabled = (!iPSX2_IsSafeOnlyEnabled() && IsDiagFlagsEnabled() &&
            iPSX2_GetRuntimeEnvBool("iPSX2_RECOMP_DIAG", false));
        s_recomp_diag_enabled = enabled ? 1 : 0;
        Console.WriteLn("@@CFG@@ iPSX2_RECOMP_DIAG=%d", s_recomp_diag_enabled);
    }
    const bool recomp_diag = (s_recomp_diag_enabled == 1);

    // [iter39] Capture startpc for SetBranchImm diagnostic.
    s_recblock_startpc = startpc;

    // [iter343] RECOMP_FREEZE_DETECT: 同一 startpc 連続call = cycle freeze の直接証拠
    // どのaddressでフリーズしているかを捕捉する。
    // Removal condition: cycle freeze root causeafter identified
    {
        static u32 s_last_recomp_pc = 0xFFFFFFFFu;
        static int s_freeze_n = 0;
        if (startpc == s_last_recomp_pc && s_freeze_n < 5) {
            ++s_freeze_n;
            const void* psm_val = PSM(startpc);
            const uptr rlu_val = recLUT[startpc >> 16];
            Console.WriteLn("@@RECOMP_FREEZE@@ n=%d startpc=%08x psm=%p recLUT=%016lx sentinel=%d ra=%08x cycle=%u",
                s_freeze_n, startpc, psm_val, (unsigned long)rlu_val,
                !(rlu_val + (uptr)(startpc >> 16) * 0x20000u) ? 1 : 0,
                cpuRegs.GPR.r[31].UL[0], cpuRegs.cycle);
        }
        s_last_recomp_pc = startpc;
    }

	// [iter43] One-shot: first block compiled in EE KUSEG RAM = BIOS OS entry point.
	// [iter45] Dump physical eeMem->Main content at that address.
	// Removal condition: BIOS から EE RAM への JR/JALR 元 PC と OS エントリafter determined。
	{
		static bool s_eeram_recomp_first = false;
		// [iter53] Track the last BIOS block compiled before KUSEG entry.
		static u32 s_last_bios_pc = 0;
		if (HWADDR(startpc) >= 0x1FC00000u && HWADDR(startpc) < 0x20000000u)
			s_last_bios_pc = startpc;
		// [iter51] Lowered to 0x00001000 to catch actual first KUSEG JR entry.
		// [P12] Extended to KSEG0 (0x80000000-0x9FFFFFFF) to catch 0x801a4530 type jumps.
		const u32 hw_start = HWADDR(startpc);
		const bool is_ee_ram = (hw_start >= 0x00001000u && hw_start < 0x02000000u);
		if (!s_eeram_recomp_first && is_ee_ram) {
			s_eeram_recomp_first = true;
			const u32* phys = reinterpret_cast<const u32*>(eeMem->Main + hw_start);
			// [iter49] Dump all 32 GPRs to identify JR source register.
			// Removal condition: 0x00100000 ジャンプ元 BIOS PC after determined。
			// [iter53] last_bios_pc = last BIOS block compiled before KUSEG jump.
			Console.WriteLn("@@EERAM_FIRST_RECOMP@@ startpc=%08x hw=%08x last_bios_pc=%08x", startpc, hw_start, s_last_bios_pc);
			for (int _ri = 0; _ri < 32; _ri += 4)
				Console.WriteLn("  r%02d=%08x r%02d=%08x r%02d=%08x r%02d=%08x",
					_ri,   cpuRegs.GPR.r[_ri].UL[0],
					_ri+1, cpuRegs.GPR.r[_ri+1].UL[0],
					_ri+2, cpuRegs.GPR.r[_ri+2].UL[0],
					_ri+3, cpuRegs.GPR.r[_ri+3].UL[0]);
			// [iter50] Dump COP0 EPC/Status/Cause/EBASE to check ERET hypothesis.
			// Removal condition: 0x00100000 ジャンプ元メカニズム (ERET/J/JR) after determined。
			Console.WriteLn("@@COP0_DUMP@@ epc=%08x status=%08x cause=%08x prid=%08x errpc=%08x",
				cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.Status.val,
				cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.PRid, cpuRegs.CP0.n.ErrorEPC);
			Console.WriteLn("@@EERAM_CONTENT@@ [0]=%08x [1]=%08x [2]=%08x [3]=%08x [4]=%08x [5]=%08x [6]=%08x [7]=%08x",
				phys[0], phys[1], phys[2], phys[3], phys[4], phys[5], phys[6], phys[7]);
			// [iter46] Scan EE RAM alternate OS entry candidates.
			// Removal condition: OS ロード先addressが特定された後。
			const u32* p0 = reinterpret_cast<const u32*>(eeMem->Main + 0x00000000);
			const u32* p8 = reinterpret_cast<const u32*>(eeMem->Main + 0x00080000);
			const u32* p2 = reinterpret_cast<const u32*>(eeMem->Main + 0x00200000);
			Console.WriteLn("@@EERAM_SCAN@@ base[0]=%08x base[1]=%08x 80000[0]=%08x 80000[1]=%08x 200000[0]=%08x 200000[1]=%08x",
				p0[0], p0[1], p8[0], p8[1], p2[0], p2[1]);
		}
	}

	// [iter212] @@BIOS_BLOCK_TRACE@@ – BIOS ROM ブロックcompile順を記録
	// Removal condition: BIOS HW 初期化skipcauseafter determined
	{
		static u32 s_bios_blk_n = 0;
		const u32 hw = HWADDR(startpc);
		if (hw >= 0x1FC00000u && hw < 0x20000000u && s_bios_blk_n < 300) {
			Console.WriteLn("@@BIOS_BLOCK_TRACE@@ n=%u startpc=%08x cycle=%u ra=%08x a0=%08x v0=%08x",
				s_bios_blk_n, startpc, cpuRegs.cycle,
				cpuRegs.GPR.r[31].UL[0],
				cpuRegs.GPR.r[4].UL[0],
				cpuRegs.GPR.r[2].UL[0]);
			s_bios_blk_n++;
		}
	}

	// [P12 TEMP_DIAG] @@EE_EXCVEC_CHECK@@ – EE 例外ベクター (0x80000080/180/200) compile verify
	// Removal condition: EE RAM 到達経路 (例外 vs 直接ジャンプ) after determined
	{
		static u32 s_excvec_n = 0;
		// EE exception vectors: 0x80000080 (TLB miss), 0x80000180 (general), 0x80000200 (debug)
		if (s_excvec_n < 5 && startpc >= 0x80000080u && startpc <= 0x80000280u) {
			Console.WriteLn("@@EE_EXCVEC_CHECK@@ n=%u startpc=%08x epc=%08x status=%08x ra=%08x",
				s_excvec_n, startpc,
				cpuRegs.CP0.n.EPC,
				cpuRegs.CP0.n.Status.val,
				cpuRegs.GPR.r[31].UL[0]);
			s_excvec_n++;
		}
	}

	// [iter681] @@SIF_FUNC_COMPILE@@ — sceSifSetDma implementation blocks (0x80006400-0x80006A00)
	{
		static u32 s_sif_func_n = 0;
		const u32 hw = HWADDR(startpc);
		if (hw >= 0x00006000u && hw < 0x00006A00u && s_sif_func_n < 40) {
			Console.WriteLn("@@SIF_FUNC_COMPILE@@ n=%u startpc=%08x hw=%08x cycle=%u ra=%08x v1=%08x a0=%08x t1=%08x",
				s_sif_func_n++, startpc, hw, cpuRegs.cycle,
				cpuRegs.GPR.r[31].UL[0], cpuRegs.GPR.r[3].UL[0],
				cpuRegs.GPR.r[4].UL[0], cpuRegs.GPR.r[9].UL[0]);
		}
		// [iter681] @@SIF_690C_STATE@@ — log regs when 0x690C (failure path) is reached
		if (hw == 0x0000690Cu) {
			static int s_690c_n = 0;
			if (s_690c_n < 5)
				Console.WriteLn("@@SIF_690C_STATE@@ n=%d v0=%08x%08x a1=%08x%08x s0=%08x s2=%08x s4=%08x ra=%08x",
					s_690c_n++,
					cpuRegs.GPR.r[2].UL[1], cpuRegs.GPR.r[2].UL[0], // v0
					cpuRegs.GPR.r[5].UL[1], cpuRegs.GPR.r[5].UL[0], // a1
					cpuRegs.GPR.r[16].UL[0], // s0
					cpuRegs.GPR.r[18].UL[0], // s2
					cpuRegs.GPR.r[20].UL[0], // s4
					cpuRegs.GPR.r[31].UL[0]); // ra
		}
	}

	// [P12 TEMP_DIAG] @@BIOS_JR_T0_CHECK@@ – 0x9FC008E4 LUI/ORI/JR t0 直前の t0 値verify
	// Removal condition: JIT で 0x801A4530 が最初の EE RAM になるcauseafter determined
	{
		static bool s_jr_t0_fired = false;
		const u32 hw = HWADDR(startpc);
		// 0x9FC0089C-0x9FC0095F: BLTZ check → JALR cache flush → JR t0 領域全体 (BFC/9FC 両エイリアス hw=0x1FC0089C-)
		if (!s_jr_t0_fired && hw >= 0x1FC00880u && hw < 0x1FC009A0u) {
			s_jr_t0_fired = true;
			Console.WriteLn("@@BIOS_JR_T0_CHECK@@ startpc=%08x t0=%08x v0=%08x ra=%08x sp=%08x",
				startpc,
				cpuRegs.GPR.r[8].UL[0],   // t0
				cpuRegs.GPR.r[2].UL[0],   // v0
				cpuRegs.GPR.r[31].UL[0],  // ra
				cpuRegs.GPR.r[29].UL[0]); // sp
		}
	}

	// [P12 TEMP_DIAG] @@BIOS_EPILOG_CHECK@@ – 9FC42170 (9FC41268 epilog) compile verify
	// Removal condition: JIT が EE RAM に飛ぶ経路の root cause after determined
	{
		static u32 s_epilog_n = 0;
		// 9FC42160-9FC42180: 9FC41268 エピローグ (LD ra,0x120(sp) at 9FC42170, JR ra at 9FC42174)
		// 9FC41240-9FC41268: 9FC41000 エピローグ (LD ra,0x50(sp) at 9FC4124C, JR ra at 9FC4125C)
		const bool in_41268_epilog = (startpc >= 0x9FC42160u && startpc < 0x9FC42180u);
		const bool in_41000_epilog = (startpc >= 0x9FC41240u && startpc < 0x9FC41268u);
		if (s_epilog_n < 10 && (in_41268_epilog || in_41000_epilog)) {
			Console.WriteLn("@@BIOS_EPILOG_CHECK@@ n=%u startpc=%08x v0=%08x ra=%08x sp=%08x %s",
				s_epilog_n, startpc,
				cpuRegs.GPR.r[2].UL[0],
				cpuRegs.GPR.r[31].UL[0],
				cpuRegs.GPR.r[29].UL[0],
				in_41268_epilog ? "9FC41268_EPILOG" : "9FC41000_EPILOG");
			s_epilog_n++;
		}
	}

	// [iter654] @@EELOAD_COMPILE@@ — JIT が 0x82000-0x91190 をcompileしようとした時、
	// address pattern をdetectしたら EELOAD を BIOS ROM から再コピーして正しいコードでcompileさせる。
	// Removal condition: カーネルの native EELOAD コピーが JIT で正常behaviorするようになった後
	{
		const u32 hw2 = HWADDR(startpc);
		if (hw2 >= 0x00082000u && hw2 < 0x00092000u && eeMem) {
			static int s_eeload_compile_n = 0;
			const u32 w0 = *(u32*)(eeMem->Main + 0x82000);
			// [R57] EELOAD fixup: address pattern (0x00082000) OR fully zeroed (warm reboot 後)
			// warm reboot で EE RAM が消去されると EELOAD 全域がゼロクリアされる。
			// 先頭 NOP (w0=0) だけでは正常なバイナリと区別できないため、
			// 0x82000-0x8201F (8ワード) が全ゼロかでdetermineする。
			// Removal condition: warm reboot でカーネルが自力で EELOAD を再ロードするようになった後
			const bool eeload_zeroed = [&]() {
				const u32* p = reinterpret_cast<const u32*>(eeMem->Main + 0x82000);
				for (int i = 0; i < 8; i++) { if (p[i] != 0) return false; }
				return true;
			}();
			if (w0 == 0x00082000u || eeload_zeroed) {
				static bool s_eeload_fixup_done = false;
				// warm reboot で全ゼロ: フラグをresetして再コピーを許可
				if (eeload_zeroed) {
					s_eeload_fixup_done = false;
					BiosResetEeloadCopyFlag();
				}
				if (!s_eeload_fixup_done) {
					s_eeload_fixup_done = true;
					if (BiosRetriggerEeloadCopy()) {
						const u32 new_w0 = *(u32*)(eeMem->Main + 0x82000);
						Console.WriteLn("@@EELOAD_COMPILE@@ FIXUP: re-copied EELOAD at compile time (zeroed=%d). startpc=%08x cycle=%u old_w0=%08x new_w0=%08x",
							eeload_zeroed ? 1 : 0, startpc, cpuRegs.cycle, w0, new_w0);
					}
				}
			}
			if (s_eeload_compile_n < 5) {
				u32 w[8] = {};
				for (int i = 0; i < 8; i++) w[i] = *(u32*)(eeMem->Main + hw2 + i*4);
				Console.WriteLn("@@EELOAD_COMPILE@@ n=%d startpc=%08x hw=%08x cycle=%u insn: %08x %08x %08x %08x %08x %08x %08x %08x",
					s_eeload_compile_n, startpc, hw2, cpuRegs.cycle,
					w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7]);
				s_eeload_compile_n++;
			}
		}
	}

	// [iter247b] @@KERN_JIT@@ カーネルfunctioncompile追跡 – [42E4] add
	// Removal condition: カーネル初期化skipcauseafter determined
	{
		static u32 s_kern_jit_n = 0;
		const u32 hw = HWADDR(startpc);
		if (s_kern_jit_n < 50 && hw >= 0x00010000u && hw < 0x00020000u) {
			u32 val_42e0 = eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x000242E0) : 0xDEADu;
			u32 val_42e4 = eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x000242E4) : 0xDEADu;
			Console.WriteLn("@@KERN_JIT@@ n=%u pc=%08x cycle=%u ra=%08x a0=%08x v0=%08x s0=%08x [42E0]=%08x [42E4]=%08x",
				s_kern_jit_n, startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.v0.UL[0],
				cpuRegs.GPR.r[16].UL[0],  // s0
				val_42e0, val_42e4);
			s_kern_jit_n++;
		}

		// [iter248c] @@KERN_HLE_MODTABLE@@ RegisterModule HLE: [800242E4] force初期化
		// カーネル初期化にはカスケード JIT バグあり（80005b88/80005ba8 SIGBUS 等）
		// 自然初期化は非現実的 → HLE でforce
		// Removal condition: カーネル JIT バグ群の根本after fixed
		if (hw == 0x00012030u && eeMem) {
			u32* p_42e4 = reinterpret_cast<u32*>(eeMem->Main + 0x000242E4);
			if (*p_42e4 == 0) {
				*p_42e4 = 32;
				Console.WriteLn("@@KERN_HLE_MODTABLE@@ forced [800242E4]=32 at RegisterModule entry (SW JIT workaround)");
			}
		}
	}

	// [iter179] NOP スレッドdetect: startpc < 0x10000 は jump-to-null の疑いあり
	// [iter180] 複数回発火にchange（上限10）：BIOS初期化訪問 vs OSDSYS誘発クラッシュを両方捕捉
	// Removal condition: NOP スレッド突入元 PC とcauseafter determineddelete
	{
		static int s_nop_sled_count = 0;
		if (s_nop_sled_count < 10 && startpc < 0x00010000u) {
			s_nop_sled_count++;
			Console.WriteLn("@@NOP_SLED_START@@ n=%d startpc=%08x last_dispatch=%08x prev_dispatch=%08x ra=%08x v0=%08x v1=%08x t9=%08x a0=%08x a1=%08x EPC=%08x ErrorEPC=%08x Status=%08x Cause=%08x sp=%08x cycle=%u",
				s_nop_sled_count, startpc, g_last_dispatch_pc, g_prev_dispatch_pc,
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
				cpuRegs.GPR.n.t9.UL[0], cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
				cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.ErrorEPC,
				cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause, cpuRegs.GPR.n.sp.UL[0], cpuRegs.cycle);
			// Dump kernel stack at 0x80018E70 to find saved EPC+4
			if (eeMem) {
				u32* stk = (u32*)(eeMem->Main + 0x00018E70u);
				Console.WriteLn("@@NOP_SLED_KERNSTK@@ [18E70]: ra=%08x old_sp=%08x epc4=%08x [+C]=%08x [+10]=%08x [+14]=%08x",
					stk[0], stk[1], stk[2], stk[3], stk[4], stk[5]);
				// Also dump all GPRs
				Console.WriteLn("@@NOP_SLED_REGS@@ at=%08x k0=%08x k1=%08x gp=%08x fp=%08x s0=%08x s1=%08x s2=%08x",
					cpuRegs.GPR.n.at.UL[0], cpuRegs.GPR.n.k0.UL[0], cpuRegs.GPR.n.k1.UL[0],
					cpuRegs.GPR.n.gp.UL[0], cpuRegs.GPR.n.s8.UL[0],
					cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0], cpuRegs.GPR.n.s2.UL[0]);
				// Dump user stack content (sp was already incremented by delay slot, so original sp = sp - 0x10)
				{
					u32 orig_sp_phys = (cpuRegs.GPR.n.sp.UL[0] - 0x10) & 0x01FFFFFFu;
					if (orig_sp_phys + 0x20 < 0x02000000u) {
						const u32* us = reinterpret_cast<const u32*>(eeMem->Main + orig_sp_phys);
						Console.WriteLn("@@NOP_SLED_USRSTK@@ [sp-10=%08x phys=%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
							cpuRegs.GPR.n.sp.UL[0] - 0x10, orig_sp_phys,
							us[0], us[1], us[2], us[3], us[4], us[5], us[6], us[7]);
					}
				}
				// Dump MIPS code around prev_dispatch_pc to see what jumped to PC=0
				if (g_prev_dispatch_pc >= 0x80 && g_prev_dispatch_pc < 0x02000000u) {
					u32 base = (g_prev_dispatch_pc & 0x01FFFFFFu) & ~3u;
					u32 dump_start = (base >= 0x40) ? base - 0x40 : 0;
					u32 dump_end = (base + 0x80 < 0x02000000u) ? base + 0x80 : base + 0x40;
					for (u32 off = dump_start; off < dump_end; off += 0x20) {
						const u32* p = reinterpret_cast<const u32*>(eeMem->Main + off);
						Console.WriteLn("@@NOP_SLED_CALLER@@ [%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
							off, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
					}
				}
			}
			// [iter292] startpc==0: EE RAM [0x00..0xB0] をcompile時点でダンプ
			if (startpc == 0) {
				const u32* ram0 = reinterpret_cast<const u32*>(PSM(0x00000000));
				// [R56] Comprehensive dump of kernel entry 0x00-0x1FF
				if (ram0) {
					for (u32 off = 0; off < 0x400; off += 0x20) {
						Console.WriteLn("@@KERN_ENTRY_DUMP@@ [%03x]: %08x %08x %08x %08x %08x %08x %08x %08x",
							off,
							ram0[(off+0x00)/4], ram0[(off+0x04)/4], ram0[(off+0x08)/4], ram0[(off+0x0C)/4],
							ram0[(off+0x10)/4], ram0[(off+0x14)/4], ram0[(off+0x18)/4], ram0[(off+0x1C)/4]);
					}
				}
				// [R57] warm reboot 後の EELOAD 再コピー。
				// startpc=0 はカーネル例外ベクタ再エントリ → warm reboot の証拠。
				// EELOAD (0x82000) がゼロクリアされていたら ROM から再コピーする。
				// これにより kernel が EELOAD を見つけて OSDSYS をロードできる。
				// Removal condition: warm reboot でカーネルが自力で EELOAD を再ロードするようになった後
				if (eeMem) {
					const u32* eel = reinterpret_cast<const u32*>(eeMem->Main + 0x82000);
					bool eel_zeroed = true;
					for (int i = 0; i < 8; i++) { if (eel[i] != 0) { eel_zeroed = false; break; } }
					if (eel_zeroed) {
						BiosResetEeloadCopyFlag();
						if (BiosRetriggerEeloadCopy()) {
							Console.WriteLn("@@EELOAD_WARMREBOOT_RECOPY@@ re-copied EELOAD from ROM (detected at startpc=0 warm reboot)");
						}
					}
				}
				// [R57] handlerテーブル 0x80015340 のダンプ (ExcCode 0-31, 各4byte)
				// カーネル例外ベクタ 0x00000000 の JR k0 が参照するテーブル。
				// Removal condition: 0x22000000 hangroot causeafter identified
				{
					const u32* tbl = reinterpret_cast<const u32*>(PSM(0x80015340));
					if (tbl) {
						Console.WriteLn("@@HANDLER_TABLE@@ [0x80015340]: %08x %08x %08x %08x %08x %08x %08x %08x",
							tbl[0],tbl[1],tbl[2],tbl[3],tbl[4],tbl[5],tbl[6],tbl[7]);
						Console.WriteLn("@@HANDLER_TABLE@@ [0x80015360]: %08x %08x %08x %08x %08x %08x %08x %08x",
							tbl[8],tbl[9],tbl[10],tbl[11],tbl[12],tbl[13],tbl[14],tbl[15]);
					}
					Console.WriteLn("@@HANDLER_TABLE_COP0@@ cause=%08x exccode=%u epc=%08x status=%08x",
						cpuRegs.CP0.n.Cause, (cpuRegs.CP0.n.Cause >> 2) & 0x1F,
						cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.Status.val);
					// [R57] Syscall handler (0x80000280) コードダンプ — eeMem直読み
					// Removal condition: 0x22000000 到達経路after identified
					if (tbl && eeMem) {
						const u32 syscall_handler = tbl[8]; // ExcCode=8
						const u32 phys = syscall_handler & 0x1FFFFFFF;
						if (phys < 0x02000000 - 0x80) { // within 32MB EE RAM
							const u32* sc = reinterpret_cast<const u32*>(eeMem->Main + phys);
							Console.WriteLn("@@SYSCALL_HANDLER@@ [%08x] phys=%08x:", syscall_handler, phys);
							Console.WriteLn("@@SYSCALL_HANDLER@@ [+00]: %08x %08x %08x %08x %08x %08x %08x %08x",
								sc[0],sc[1],sc[2],sc[3],sc[4],sc[5],sc[6],sc[7]);
							Console.WriteLn("@@SYSCALL_HANDLER@@ [+20]: %08x %08x %08x %08x %08x %08x %08x %08x",
								sc[8],sc[9],sc[10],sc[11],sc[12],sc[13],sc[14],sc[15]);
							Console.WriteLn("@@SYSCALL_HANDLER@@ [+40]: %08x %08x %08x %08x %08x %08x %08x %08x",
								sc[16],sc[17],sc[18],sc[19],sc[20],sc[21],sc[22],sc[23]);
							Console.WriteLn("@@SYSCALL_HANDLER@@ [+60]: %08x %08x %08x %08x %08x %08x %08x %08x",
								sc[24],sc[25],sc[26],sc[27],sc[28],sc[29],sc[30],sc[31]);
						} else {
							Console.WriteLn("@@SYSCALL_HANDLER@@ handler=%08x phys=%08x OUT_OF_RANGE", syscall_handler, phys);
						}
					}
					// [R58] TLB Refill handler (ExcCode=2: TLBL) のダンプ
					// handler_table[2] = 0x800140c0 → 物理 0x140c0
					{
						const u32 tlb_handler = tbl[2]; // ExcCode=2 (TLBL)
						const u32 tlb_phys = tlb_handler & 0x1FFFFFFF;
						if (tlb_phys < 0x02000000 - 0x80) {
							const u32* th = reinterpret_cast<const u32*>(eeMem->Main + tlb_phys);
							Console.WriteLn("@@TLB_HANDLER@@ [%08x] phys=%08x:", tlb_handler, tlb_phys);
							for (int row = 0; row < 8; row++) {
								Console.WriteLn("@@TLB_HANDLER@@ [+%02x]: %08x %08x %08x %08x %08x %08x %08x %08x",
									row*0x20,
									th[row*8+0],th[row*8+1],th[row*8+2],th[row*8+3],
									th[row*8+4],th[row*8+5],th[row*8+6],th[row*8+7]);
							}
						}
					}
				}
				// [R56] KERNSTUB_PATCH: v0=0 JALR の即リターン。
				// warm reboot 後のカーネル未初期化stateでのみneeded。
				u32* ram0_rw = reinterpret_cast<u32*>(PSM(0x00000000));
				if (ram0_rw && ram0_rw[0xA4/4] == 0x00000000u) {
					ram0_rw[0xA4/4] = 0x03E00008u; // JR $ra
					Console.WriteLn("@@KERNSTUB_PATCH@@ patched [0xA4] JR $ra (kernel uninit)");
				}
			}
		}
	}

	// [iter238d] @@HLE_VEC_A0@@ BIOS function table 0xA0 HLE
	// LOGO が JALR 0xA0 (t1=func#) で BIOS functionを呼ぶ。カーネルが 0xA0 未設置のため
	// C++ で t1 ベースのdispatchを行い、適切な返り値で $ra へ戻す。
	// [R56] TEMP_DIAG: 0x80000360 付近(KERNSTUB return先)のコードダンプ
	// Removal condition: 0x22000000 hangcauseafter identified
	if (startpc == 0x80000360u || startpc == 0x80000340u || startpc == 0x80000380u) {
		static int s_ra_path_n = 0;
		if (s_ra_path_n++ < 5) {
			const u32* p = reinterpret_cast<const u32*>(PSM(startpc));
			if (p)
				Console.WriteLn("@@RA_PATH_BLOCK@@ n=%d startpc=%08x: %08x %08x %08x %08x %08x %08x %08x %08x",
					s_ra_path_n, startpc, p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
		}
	}
	// [R56] TEMP_DIAG: 0x22000000 compiledetect
	// Removal condition: 0x22000000 hangcauseafter identified
	if ((startpc & 0xFFFF0000u) == 0x22000000u) {
		static int s_22m_n = 0;
		if (s_22m_n++ < 3) {
			const u32* p = reinterpret_cast<const u32*>(PSM(startpc));
			Console.WriteLn("@@BLOCK_22M@@ n=%d startpc=%08x psm=%p ra=%08x v0=%08x",
					s_22m_n, startpc, (void*)p,
					cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v0.UL[0]);
			if (p)
				Console.WriteLn("@@BLOCK_22M_CODE@@ %08x %08x %08x %08x %08x %08x %08x %08x",
					p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
		}
	}

	// Removal condition: カーネルが自然に 0xA0 を設置するようになった後
	if (startpc == 0x000000A0u || startpc == 0x000000B0u) {
		const u32 t1 = cpuRegs.GPR.n.t1.UL[0] & 0xFFu;
		static int s_hle_a0_n = 0;
		if (s_hle_a0_n++ < 20)
			Console.WriteLn("@@HLE_VEC_A0@@ n=%d vec=%02x t1=%02x(%s) ra=%08x a0=%08x a1=%08x",
				s_hle_a0_n, startpc & 0xFF, t1,
				(t1==0x02)?"SetGsCrt":(t1==0x04)?"Exit":(t1==0x3C)?"InitHeap":
				(t1==0x44)?"FlushCache":(t1==0x61)?"Alloc":(t1==0x64)?"FlushCache64":
				(t1==0x70)?"GsGetIMR":(t1==0x71)?"GsPutIMR":(t1==0x73)?"SetVSyncFlag":
				(t1==0x76)?"SifDmaStat":(t1==0x78)?"SifSetDma":"???",
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0]);

		switch (t1) {
		case 0x02: // SetGsCrt(interlace, mode, field)
			// GS CRT config: successで v0=0
			cpuRegs.GPR.n.v0.UL[0] = 0;
			break;
		case 0x04: // Exit
			// 何もしない
			break;
		case 0x3C: // InitHeap(base, size)
			// ヒープ初期化: successで v0=base
			cpuRegs.GPR.n.v0.UL[0] = cpuRegs.GPR.n.a0.UL[0];
			break;
		case 0x44: // FlushCache(mode)
		case 0x64: // FlushCache (alternate)
			// [iter244b] FlushCache: LOGO 展開rangeのみ JIT クリア。
			// 全クリアは速度 10% まで低下するため、展開先 (0x30000-0x50000) に限定。
			// Removal condition: カーネル自然boot後 (HLE not needed時)
			Cpu->Clear(0x30000, 0x20000 / 4);
			cpuRegs.GPR.n.v0.UL[0] = 0;
			break;
		case 0x61: { // Alloc(size) — memoryalloc
			// 簡易 bump allocator: 0x01800000 から上方に割り当て
			static u32 s_heap_ptr = 0x01800000u;
			u32 sz = cpuRegs.GPR.n.a0.UL[0];
			if (sz == 0) sz = 16;
			sz = (sz + 15u) & ~15u; // 16-byte align
			u32 ptr = s_heap_ptr;
			s_heap_ptr += sz;
			if (s_heap_ptr > 0x01F00000u) s_heap_ptr = 0x01800000u; // wrap
			cpuRegs.GPR.n.v0.UL[0] = ptr;
			break;
		}
		case 0x70: // GsGetIMR
			cpuRegs.GPR.n.v0.UL[0] = 0xFF00; // default IMR
			break;
		case 0x71: // GsPutIMR(imr)
			cpuRegs.GPR.n.v0.UL[0] = 0;
			break;
		case 0x73: // SetVSyncFlag(addr1, addr2)
			cpuRegs.GPR.n.v0.UL[0] = 0;
			break;
		case 0x76: // SifDmaStat
			cpuRegs.GPR.n.v0.UL[0] = 0xFFFFFFFFu; // -1 = transfer complete
			break;
		case 0x78: // SifSetDma
			cpuRegs.GPR.n.v0.UL[0] = 1; // DMA ID
			break;
		default:
			cpuRegs.GPR.n.v0.UL[0] = 0;
			break;
		}
		cpuRegs.GPR.n.v1.UL[0] = cpuRegs.GPR.n.v0.UL[0]; // v1 = v0 for compat
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0]; // JR $ra
		return;
	}

	// [iter387] 旧 PS1DRV startpc==0x200008 早期リターン HLE をdelete。
	// iter385 で実 PS1DRV コードを BIOS ROM からロードするため、ここで横取りすると
	// PS1DRV が一切実行されずに $ra に即リターンしてしまい OSDSYS → Exit loopになる。
	// Removal condition: PS1DRV が正常bootafter confirmedに全probeとともに整理

	// [iter392] PS1DRV busy-wait delay function (0x00207488) を即時skip
	// このfunctionは a0 のカウント (~0x01000000 = 16.7M iterations) を純粋にカウントダウンするだけ。
	// 副作用なし。skipしても SIF HW への書き込みはcall元で行われている。
	// Removal condition: BIOS browser画面after confirmedに整理
	// [iter395] 外部functionプロローグ直前の $sp をverify (1回のみ)
	if (startpc == 0x00209084u) {
		static bool s_prolog_logged = false;
		if (!s_prolog_logged) {
			s_prolog_logged = true;
			Console.WriteLn("@@PROLOG_SP@@ sp=%08x ra=%08x (at outer func entry compile)",
				cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0]);
		}
	}

	// [iter397] @@POST_SUBFUNC1_SP@@ — 最初の JAL サブfunction (0x002090EC→0x0020906C) 戻り後の $sp verify
	// 0x0020906C は $sp をchangeしない 6命令function。ここで sp が既に壊れていれば prolog ブロック自体がissue。
	// Removal condition: $sp 破損箇所after identified
	if (startpc == 0x002090F4u) {
		static bool s_subfunc1_logged = false;
		if (!s_subfunc1_logged) {
			s_subfunc1_logged = true;
			Console.WriteLn("@@POST_SUBFUNC1_SP@@ sp=%08x ra=%08x",
				cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0]);
		}
	}

	// [iter398] @@MID_JAL_SP@@ — JAL 列中間点の $sp 二分探索
	// JAL#6 ret=0x0020929C (range: JAL#0–#13 が犯人range)
	// Removal condition: $sp 破損 JAL after identified
	if (startpc == 0x00209138u) {
		static bool s_jal1_logged = false;
		if (!s_jal1_logged) {
			s_jal1_logged = true;
			Console.WriteLn("@@MID_JAL_SP@@ sp=%08x ra=%08x (after JAL#1 ret=0x00209138 tgt=0x0020ad60)",
				cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0]);
		}
	}

	// [iter399] @@SUBFUNC_EPILOG_SP@@ — サブfunction 0x00201404 内の epilog ブロック (0x00201438) の $sp verify
	// このブロックは JR r31 + delay slot ADDIU r29,r29,+32 (epilog) をinclude。
	// ここで sp が 0x01effec0 (prolog 後) ならブロック内の JIT コードがcause。0 以外なら上流バグ。
	// Removal condition: $sp 破損causeafter identified
	// 0x00201438 は r4≠0 の場合のみブロック先頭。r4==0 の場合は 0x00201434 から継続。
	if (startpc == 0x00201438u || startpc == 0x00201434u) {
		static bool s_epilog_logged = false;
		if (!s_epilog_logged) {
			s_epilog_logged = true;
			Console.WriteLn("@@SUBFUNC_EPILOG_SP@@ sp=%08x ra=%08x (compile of epilog block startpc=%08x)",
				cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0], startpc);
		}
	}

	// [iter399] @@SUBFUNC_PROLOG_SP@@ — サブfunction 0x00201404 エントリの $sp verify
	if (startpc == 0x00201404u) {
		static bool s_prolog2_logged = false;
		if (!s_prolog2_logged) {
			s_prolog2_logged = true;
			Console.WriteLn("@@SUBFUNC_PROLOG_SP@@ sp=%08x ra=%08x (compile of subf 0x00201404)",
				cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0]);
		}
	}

	if (startpc == 0x00207488u) {
		static u32 s_delay_skip_n = 0;
		if (s_delay_skip_n < 5)
			Console.WriteLn("@@PS1DRV_DELAY_SKIP@@ n=%u ra=%08x a0=%08x",
				s_delay_skip_n, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0]);
		// [iter394/395] n==0,1 でスタック値をダンプ: LQ $ra,0x90($sp) が読む値をverify
		if (s_delay_skip_n <= 1) {
			const u32 sp = cpuRegs.GPR.n.sp.UL[0];
			Console.WriteLn("@@DELAY_SKIP_STACK@@ n=%u sp=%08x [sp+0x90]=%08x [sp+0x94]=%08x ra=%08x",
				s_delay_skip_n, sp, memRead32(sp + 0x90u), memRead32(sp + 0x94u),
				cpuRegs.GPR.n.ra.UL[0]);
		}
		s_delay_skip_n++;
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
		cpuSetEvent();
		return;
	}

	// [iter347] @@OSDSYS_FLOW@@ OSDSYS success/failパス分岐観測
	// ExecPS2(PS1DRV) 直後 (0x001001bc) とリカバリloop入口 (0x001000a4) の命令列/registerを観測
	// Removal condition: OSDSYS の PS1DRV successdetermineconditionと次 PC after determined
	{
		const bool is_ps1drv_ret = (startpc == 0x001001BCu);
		const bool is_recovery   = (startpc == 0x001000A4u);
		if (is_ps1drv_ret || is_recovery) {
			static bool s_ps1drv_ret_done = false;
			static int  s_recovery_n = 0;
			const bool should_log = is_ps1drv_ret ? !s_ps1drv_ret_done : (s_recovery_n < 3);
			if (should_log) {
				if (is_ps1drv_ret) s_ps1drv_ret_done = true;
				else s_recovery_n++;
				const char* tag = is_ps1drv_ret ? "@@OSDSYS_FLOW_RET@@" : "@@OSDSYS_FLOW_LOOP@@";
				Console.WriteLn("%s startpc=%08x v0=%08x v1=%08x a0=%08x ra=%08x sp=%08x",
					tag, startpc,
					cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
					cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.ra.UL[0],
					cpuRegs.GPR.n.sp.UL[0]);
				const u32* insn = reinterpret_cast<const u32*>(PSM(startpc));
				if (insn) {
					for (int _i = 0; _i < 12; _i++)
						Console.WriteLn("%s  [+%02x]=%08x", tag, _i * 4, insn[_i]);
				}
			}
		}
	}

	// [iter237d] LOGO 展開後コードverify: デコンプレッサが 0x80030000 に展開したコードをダンプ
	// Removal condition: LOGO 正常実行after confirmed
	if (startpc == 0x80030000u) {
		static int s_logo_exec_n = 0;
		if (s_logo_exec_n < 3) {
			Console.WriteLn("@@LOGO_EXEC@@ n=%d ra=%08x v0=%08x [30000]=%08x [30004]=%08x [30008]=%08x [3000c]=%08x [30010]=%08x [30014]=%08x [30018]=%08x [3001c]=%08x",
				s_logo_exec_n++, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v0.UL[0],
				memRead32(0x80030000u), memRead32(0x80030004u), memRead32(0x80030008u), memRead32(0x8003000Cu),
				memRead32(0x80030010u), memRead32(0x80030014u), memRead32(0x80030018u), memRead32(0x8003001Cu));
		}
	}

	// [iter237e] LOGO 例外cause特定: 例外handler (0x80000180) 突入時の EPC/Cause ダンプ
	// LOGO (0x80030000+) からの例外を 0x80000180 入口でdetectする
	// Removal condition: LOGO 例外causeafter determined
	if (startpc == 0x80000180u) {
		static int s_excvec_n = 0;
		const u32 epc = cpuRegs.CP0.n.EPC;
		const u32 cause = cpuRegs.CP0.r[13]; // Cause
		const u32 status = cpuRegs.CP0.r[12]; // Status
		const u32 hw_epc = epc & 0x1FFFFFFFu;
		// Only log exceptions from LOGO range (0x30000-0x50000)
		if (hw_epc >= 0x30000u && hw_epc < 0x50000u && s_excvec_n < 10) {
			Console.WriteLn("@@LOGO_EXCEPTION@@ n=%d epc=%08x cause=%08x (ExcCode=%d) status=%08x ra=%08x v1(call)=%08x a0=%08x sp=%08x",
				s_excvec_n++, epc, cause, (cause >> 2) & 0x1F, status,
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v1.UL[0],
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.sp.UL[0]);
		}
	}

	// [iter183] OSDSYS ブロック 0x100a9c: PC=0 ジャンプcauseのブランチ命令特定
	// JIT_DELAY で 0x100ab8=ADDIU s0,s0,-4 (delay slot) verify済み → 0x100ab4 のブランチを読む
	// Removal condition: PC=0 へのジャンプ元命令と ra/v0/t9 の値がverifyされroot causeafter determineddelete
	if (startpc == 0x00100a9cu) {
		Console.WriteLn("@@OSDSYS_BRANCH@@ 0x100ab0=0x%08x 0x100ab4=0x%08x 0x100ab8=0x%08x ra=%08x v0=%08x t9=%08x k1=%08x",
			memRead32(0x100ab0u), memRead32(0x100ab4u), memRead32(0x100ab8u),
			cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.t9.UL[0],
			cpuRegs.GPR.n.k1.UL[0]);
		// [iter186] v0 ロード元特定: JALR直前のコード (0x100a70-0x100a8c) と s0 値をダンプ
		// Removal condition: OSDSYS fn_table のaddressと v0=0 になるcauseがafter determineddelete
		Console.WriteLn("@@OSDSYS_V0_LOAD@@ s0=%08x [100a70]=%08x [100a74]=%08x [100a78]=%08x [100a7c]=%08x [100a80]=%08x [100a84]=%08x [100a88]=%08x [100a8c]=%08x",
			cpuRegs.GPR.n.s0.UL[0],
			memRead32(0x100a70u), memRead32(0x100a74u), memRead32(0x100a78u), memRead32(0x100a7cu),
			memRead32(0x100a80u), memRead32(0x100a84u), memRead32(0x100a88u), memRead32(0x100a8cu));
	}

	// [iter188] JALR リターン先 0x100abc: OSDSYS が fn_table handling後に何をするかをverify
	// Removal condition: OSDSYS の次フェーズ (BIOS browserboot) への遷移がafter determineddelete
	if (startpc == 0x00100abcu) {
		static bool s_100abc_done = false;
		if (!s_100abc_done) {
			s_100abc_done = true;
			Console.WriteLn("@@JALR_RETURN_100ABC@@ s0=%08x v0=%08x a0=%08x a2=%08x [100abc]=%08x [100ac0]=%08x [100ac4]=%08x [100ac8]=%08x [100acc]=%08x [100ad0]=%08x",
				cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.v0.UL[0],
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a2.UL[0],
				memRead32(0x100abcu), memRead32(0x100ac0u), memRead32(0x100ac4u),
				memRead32(0x100ac8u), memRead32(0x100accu), memRead32(0x100ad0u));
		}
	}

	// [iter52] EELOAD_START (0x82000) 到達時点のmemory内容verify。
	// IOP が非同期で 0x82000 に EELOAD を DMA ロードしているかdetermineする。
	// Removal condition: EELOAD ロード元 (IOP DMA or BIOS copy routine) が確定した後。
	{
		static bool s_eeload_check = false;
		if (!s_eeload_check && HWADDR(startpc) == 0x82000u) {
			s_eeload_check = true;
			const u32 word9c = memRead32(EELOAD_START + 0x9c);
			const u32 word00 = memRead32(EELOAD_START + 0x00);
			const u32 word04 = memRead32(EELOAD_START + 0x04);
			const u32 word08 = memRead32(EELOAD_START + 0x08);
			const u32 word0c = memRead32(EELOAD_START + 0x0c);
			Console.WriteLn("@@EELOAD_ARRIVAL@@ startpc=%08x word[0]=%08x [4]=%08x [8]=%08x [c]=%08x [9c]=%08x",
				startpc, word00, word04, word08, word0c, word9c);
			// [iter_ENTRY] EELOAD到達時のregister: どのregisterが 0x82000 を保持していたかverify
			Console.WriteLn("@@EELOAD_ARRIVAL_REGS@@ v0=%08x v1=%08x a0=%08x a1=%08x t0=%08x t1=%08x t9=%08x ra=%08x s0=%08x s1=%08x",
				cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
				cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0],
				cpuRegs.GPR.r[25].UL[0], cpuRegs.GPR.n.ra.UL[0],
				cpuRegs.GPR.r[16].UL[0], cpuRegs.GPR.r[17].UL[0]);
			// [iter54] Direct eeMem read (bypasses vtlb) to detect FastMem-only writes.
			const u32* phys82 = reinterpret_cast<const u32*>(eeMem->Main + 0x82000);
			Console.WriteLn("@@EELOAD_DIRECT@@ eeMem[82000]=%08x [82004]=%08x [8209c]=%08x fastmem=%zx",
				phys82[0], phys82[1], phys82[0x27],
				(size_t)vtlb_private::vtlbdata.fastmem_base);
			// [iter88] @@BIOS_OPCODE_DUMP@@ 82034-82090 – moved from cpupc<0x1000 guard (dead after ECX fix)
			// 目的: 0x82064 の SYSCALL 引数をverifyし、ExecPS2 looproot causeを特定する
			// Removal condition: SYSCALL loop (0x82064/0x82080) のroot cause確定時
			Console.WriteLn("@@BIOS_OPCODE_DUMP@@ 82034: %08x %08x %08x %08x %08x %08x %08x %08x",
				phys82[0x34/4], phys82[0x38/4], phys82[0x3c/4], phys82[0x40/4],
				phys82[0x44/4], phys82[0x48/4], phys82[0x4c/4], phys82[0x50/4]);
			Console.WriteLn("@@BIOS_OPCODE_DUMP@@ 82054: %08x %08x %08x %08x %08x %08x %08x %08x",
				phys82[0x54/4], phys82[0x58/4], phys82[0x5c/4], phys82[0x60/4],
				phys82[0x64/4], phys82[0x68/4], phys82[0x6c/4], phys82[0x70/4]);
			Console.WriteLn("@@BIOS_OPCODE_DUMP@@ 82074: %08x %08x %08x %08x %08x %08x %08x %08x",
				phys82[0x74/4], phys82[0x78/4], phys82[0x7c/4], phys82[0x80/4],
				phys82[0x84/4], phys82[0x88/4], phys82[0x8c/4], phys82[0x90/4]);
		}
	}

	// [iter_82BLK] @@COMPILE_82BLOCK@@ – 0x82000-0x82080 rangeの全ブロックcompileを記録
	// 目的: JIT が 0x82008 を SYSCALL としてcompileした瞬間の物理memoryを特定する
	// Removal condition: JIT 0x82008 SYSCALL のroot cause (compile-time stale / wrong content) after determined
	{
		static int s_82blk_n = 0;
		const u32 hw82 = HWADDR(startpc);
		if (s_82blk_n < 10 && hw82 >= 0x82000u && hw82 <= 0x82080u) {
			const u32* p = eeMem ? reinterpret_cast<const u32*>(eeMem->Main + 0x82000) : nullptr;
			Console.WriteLn("@@COMPILE_82BLOCK@@ n=%d startpc=%08x cycle=%u phys[00]=%08x [04]=%08x [08]=%08x [0c]=%08x [60]=%08x [64]=%08x [68]=%08x",
				s_82blk_n, startpc, cpuRegs.cycle,
				p ? p[0x00/4] : 0u, p ? p[0x04/4] : 0u,
				p ? p[0x08/4] : 0u, p ? p[0x0c/4] : 0u,
				p ? p[0x60/4] : 0u, p ? p[0x64/4] : 0u,
				p ? p[0x68/4] : 0u);
			s_82blk_n++;
		}
	}

	// [iter_DB78] @@WAIT_EXIT_REGION@@ – 0x8000db00-0x8000de00 (wait loop return 付近)
	// 目的: JIT が EELOAD entry を 0x82000 と誤計算するcause特定
	// Removal condition: JIT 誤エントリ計算のroot causeafter determined
	{
		static int s_db78_n = 0;
		const u32 hw_db = HWADDR(startpc);
		if (s_db78_n < 20 && hw_db >= 0x0000db00u && hw_db <= 0x0000de00u) {
			const u32 elf_entry = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82018) : 0u;
			const u32 elf_base  = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82000) : 0u;
			Console.WriteLn("@@WAIT_EXIT_REGION@@ n=%d startpc=%08x cycle=%u a0=%08x v0=%08x v1=%08x t0=%08x t1=%08x ra=%08x s0=%08x elf[82000]=%08x elf[82018]=%08x",
				s_db78_n, startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
				cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.ra.UL[0],
				cpuRegs.GPR.r[16].UL[0], elf_base, elf_entry);
			// [iter_DB70_CODE] 0x8000db70 compile時に物理コードをダンプ (wait loop 前後の命令列)
			if (hw_db == 0x0000db70u && s_db78_n == 12 && eeMem) {
				// dump 0x0000db50-0x0000dbc0 (28 words = 112 bytes = BIOS wait caller context)
				const u32* p = reinterpret_cast<const u32*>(eeMem->Main + 0x0000db50);
				Console.WriteLn("@@DB70_CODE@@ db50: %08x %08x %08x %08x %08x %08x %08x %08x",
					p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
				Console.WriteLn("@@DB70_CODE@@ db70: %08x %08x %08x %08x %08x %08x %08x %08x",
					p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
				Console.WriteLn("@@DB70_CODE@@ db90: %08x %08x %08x %08x %08x %08x %08x %08x",
					p[16],p[17],p[18],p[19],p[20],p[21],p[22],p[23]);
				Console.WriteLn("@@DB70_CODE@@ dbb0: %08x %08x %08x %08x",
					p[24],p[25],p[26],p[27]);
			}
			s_db78_n++;
		}
	}

	// [iter_E588] @@E588_REGION@@ – 0x8000E580-0x8000EA00 (EELOAD entry 決定function)
	// 目的: 0x8000E588 内で JIT が 0x82000 vs Interpreter が 0x82180 にジャンプするcause特定
	// Removal condition: EELOAD entry 計算 divergence root causeafter determined
	{
		static int s_e588_n = 0;
		static bool s_e588_code_done = false;
		const u32 hw_e = HWADDR(startpc);
		if (s_e588_n < 30 && hw_e >= 0x0000E580u && hw_e <= 0x0000EA00u) {
			// 初回のみ物理コードをダンプ
			if (!s_e588_code_done && hw_e == 0x0000E588u && eeMem) {
				s_e588_code_done = true;
				const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x0000E588);
				Console.WriteLn("@@E588_CODE@@ e588: %08x %08x %08x %08x %08x %08x %08x %08x",
					c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7]);
				Console.WriteLn("@@E588_CODE@@ e5a8: %08x %08x %08x %08x %08x %08x %08x %08x",
					c[8],c[9],c[10],c[11],c[12],c[13],c[14],c[15]);
				Console.WriteLn("@@E588_CODE@@ e5c8: %08x %08x %08x %08x %08x %08x %08x %08x",
					c[16],c[17],c[18],c[19],c[20],c[21],c[22],c[23]);
				Console.WriteLn("@@E588_CODE@@ e5e8: %08x %08x %08x %08x %08x %08x %08x %08x",
					c[24],c[25],c[26],c[27],c[28],c[29],c[30],c[31]);
				Console.WriteLn("@@E588_CODE@@ e608: %08x %08x %08x %08x %08x %08x %08x %08x",
					c[32],c[33],c[34],c[35],c[36],c[37],c[38],c[39]);
				Console.WriteLn("@@E588_CODE@@ e628: %08x %08x %08x %08x %08x %08x %08x %08x",
					c[40],c[41],c[42],c[43],c[44],c[45],c[46],c[47]);
			}
			const u32 elf18 = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82018) : 0u;
			Console.WriteLn("@@E588_REGION@@ n=%d startpc=%08x cycle=%u a0=%08x v0=%08x v1=%08x t0=%08x t1=%08x ra=%08x s0=%08x elf18=%08x",
				s_e588_n, startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
				cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.ra.UL[0],
				cpuRegs.GPR.r[16].UL[0], elf18);
			s_e588_n++;
		}
	}

	// [iter_PRE82] @@PRE_EELOAD_EXEC@@ – cycle>27M かつ 0x8000d000-0x8001FFFF の全新ブロック記録
	// 目的: 0x8000db8c 返却後に EELOAD entry (0x82000 vs 0x82180) を計算するブロックを特定
	// Removal condition: EELOAD entry 計算の root cause after determined
	{
		static int s_pre82_n = 0;
		const u32 hw_p = HWADDR(startpc);
		if (s_pre82_n < 50 && cpuRegs.cycle > 27000000u && hw_p >= 0x0000d000u && hw_p <= 0x0001ffffu) {
			const u32 elf00 = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82000) : 0u;
			Console.WriteLn("@@PRE_EELOAD_EXEC@@ n=%d startpc=%08x cycle=%u v0=%08x v1=%08x a0=%08x a1=%08x ra=%08x t9=%08x elf00=%08x",
				s_pre82_n, startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.r[25].UL[0],
				elf00);
			s_pre82_n++;
		}
	}

	// [iter_1460] @@EELOAD_JAL1460@@ – JAL 0x80001460 (EELOAD launcher) 内部probe
	// 目的: 0x80001460 内で JIT が EPC=0x82000 vs Interpreter EPC=0x81fc0 をconfigする場所特定
	// Removal condition: EPC divergence root cause after determined
	{
		static int s_1460_n = 0;
		const u32 hw_1 = HWADDR(startpc);
		if (s_1460_n < 20 && cpuRegs.cycle > 27000000u && hw_1 >= 0x00001400u && hw_1 <= 0x00001600u) {
			const u32 cop0_epc = cpuRegs.CP0.n.EPC;
			const u32 mem1A64C = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x1A64C) : 0u;
			const u32 elf00 = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82000) : 0u;
			Console.WriteLn("@@EELOAD_JAL1460@@ n=%d startpc=%08x cycle=%u v0=%08x a0=%08x a1=%08x ra=%08x COP0_EPC=%08x mem[1A64C]=%08x elf00=%08x",
				s_1460_n, startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.v0.UL[0],
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
				cpuRegs.GPR.n.ra.UL[0], cop0_epc, mem1A64C, elf00);
			s_1460_n++;
		}
	}

	// [iter_517C] @@EELOAD_CALLER@@ – 0x80005160-0x800051d0 + 0x8001A64C 監視
	// 目的: MTC0 EPC 直前の v0 (=EELOAD entry) と mem[0x8001A64C] をverify
	// Removal condition: EELOAD entry 計算 root cause after determined
	{
		static int s_517c_n = 0;
		const u32 hw_c = HWADDR(startpc);
		if (s_517c_n < 10 && hw_c >= 0x00005160u && hw_c <= 0x000051d0u) {
			const u32 elf00c = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82000) : 0u;
			// 0x8001A64C = EELOAD entry pointer格納場所
			const u32 entry_ptr = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x0001A64C) : 0u;
			// COP0 EPC register
			const u32 cop0_epc = cpuRegs.CP0.n.EPC;
			// コード内容も dump (初回のみ)
			const u32* code = eeMem ? reinterpret_cast<const u32*>(eeMem->Main + 0x00005160) : nullptr;
			if (code && s_517c_n == 0) {
				Console.WriteLn("@@EELOAD_CALLER_CODE@@ 5160: %08x %08x %08x %08x %08x %08x %08x %08x",
					code[0],code[1],code[2],code[3],code[4],code[5],code[6],code[7]);
				Console.WriteLn("@@EELOAD_CALLER_CODE@@ 5180: %08x %08x %08x %08x %08x %08x %08x %08x",
					code[8],code[9],code[10],code[11],code[12],code[13],code[14],code[15]);
			}
			// 0x8001A64C 周辺 16 words dump (EPC が指す entry テーブル)
			if (s_517c_n == 0 && eeMem) {
				const u32* tbl = reinterpret_cast<const u32*>(eeMem->Main + 0x0001A640);
				Console.WriteLn("@@EELOAD_ENTRY_TABLE@@ [1A640]: %08x %08x %08x %08x %08x %08x %08x %08x",
					tbl[0],tbl[1],tbl[2],tbl[3],tbl[4],tbl[5],tbl[6],tbl[7]);
				Console.WriteLn("@@EELOAD_ENTRY_TABLE@@ [1A660]: %08x %08x %08x %08x %08x %08x %08x %08x",
					tbl[8],tbl[9],tbl[10],tbl[11],tbl[12],tbl[13],tbl[14],tbl[15]);
				// EELOAD entry field at 0x82000+0x18 and 0x82000+0x24 and 0x82000+0x30
				const u32* elf = reinterpret_cast<const u32*>(eeMem->Main + 0x82000);
				Console.WriteLn("@@EELOAD_HEADER@@ [82000]: %08x [04]=%08x [08]=%08x [0c]=%08x [10]=%08x [14]=%08x [18]=%08x [1c]=%08x",
					elf[0],elf[1],elf[2],elf[3],elf[4],elf[5],elf[6],elf[7]);
				Console.WriteLn("@@EELOAD_HEADER@@ [82020]: %08x [24]=%08x [28]=%08x [2c]=%08x [30]=%08x [34]=%08x [38]=%08x [3c]=%08x",
					elf[8],elf[9],elf[10],elf[11],elf[12],elf[13],elf[14],elf[15]);
				Console.WriteLn("@@EELOAD_HEADER@@ [82180]: %08x [84]=%08x [88]=%08x [8c]=%08x",
					elf[0x180/4],elf[0x184/4],elf[0x188/4],elf[0x18c/4]);
			}
			Console.WriteLn("@@EELOAD_CALLER@@ n=%d startpc=%08x cycle=%u v0=%08x a0=%08x ra=%08x elf00=%08x mem[1A64C]=%08x COP0_EPC=%08x",
				s_517c_n, startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.v0.UL[0],
				cpuRegs.GPR.n.a0.UL[0],
				cpuRegs.GPR.n.ra.UL[0], elf00c, entry_ptr, cop0_epc);
			s_517c_n++;
		}
	}

	// [iter_EF18] @@EELOAD_LOADER_ENTRY@@ – 0x8000ef00-0x8000f000 (EELOAD コピー＋entry 決定function)
	// 目的: mem[0x8001A64C] がいつ・何値にconfigされるかを追跡
	// Removal condition: EELOAD entry 計算 root cause after determined
	{
		static int s_ef18_n = 0;
		static u32 s_last_entry_ptr = 0u;
		const u32 hw_ef = HWADDR(startpc);
		if (s_ef18_n < 30 && cpuRegs.cycle > 27000000u && hw_ef >= 0x0000ef00u && hw_ef <= 0x0001f000u) {
			const u32 entry_ptr2 = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x0001A64C) : 0u;
			const u32 elf00e = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82000) : 0u;
			// mem[0x1A64C] が変化した瞬間のみ詳細ログ
			if (entry_ptr2 != s_last_entry_ptr || s_ef18_n == 0) {
				s_last_entry_ptr = entry_ptr2;
				Console.WriteLn("@@EELOAD_LOADER_ENTRY@@ n=%d startpc=%08x cycle=%u v0=%08x a0=%08x a1=%08x ra=%08x mem[1A64C]=%08x elf00=%08x",
					s_ef18_n, startpc, cpuRegs.cycle,
					cpuRegs.GPR.n.v0.UL[0],
					cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.ra.UL[0], entry_ptr2, elf00e);
				s_ef18_n++;
			}
		}
	}

	// [iter_0C40_JIT] @@JIT_0C40@@ – 0x80000C30-0x80000CA0 JIT compile時引数verify (サイクルconditionなし)
	// 目的: JIT が 0x80000C40 をcompileする際の a0/a1 値をverify
	// Removal condition: 戻り値差分 root cause after determined
	{
		static int s_jit_0c40_n = 0;
		const u32 hw_0c = HWADDR(startpc);
		if (s_jit_0c40_n < 5 && hw_0c >= 0x00000c30u && hw_0c <= 0x00000ca0u) {
			Console.WriteLn("@@JIT_0C40@@ n=%d startpc=%08x cycle=%u v0=%08x a0=%08x a1=%08x ra=%08x",
				s_jit_0c40_n, startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.v0.UL[0],
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
				cpuRegs.GPR.n.ra.UL[0]);
			if (s_jit_0c40_n == 0 && eeMem) {
				// コードダンプ (最初の1回のみ)
				const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x00000c30);
				for (int j = 0; j < 32; j += 8)
					Console.WriteLn("@@JIT_0C40_CODE@@ [%04x]: %08x %08x %08x %08x %08x %08x %08x %08x",
						0xc30 + j*4,
						c[j],c[j+1],c[j+2],c[j+3],c[j+4],c[j+5],c[j+6],c[j+7]);
			}
			s_jit_0c40_n++;
		}
	}

	// [iter_0C40_DUMP_JIT] @@JIT_0C40_DUMP@@ – 0x80000C38-0x80000CA0 コードダンプ (JIT戻り値0x82000のcause)
	// 目的: 0x80000C40 functionの内容をverifyし、なぜ JIT で a1=0x82000 になるか判断
	// Removal condition: 戻り値差分の root cause after determined
	{
		static bool s_0c40_dumped = false;
		const u32 hw_c40 = HWADDR(startpc);
		if (!s_0c40_dumped && cpuRegs.cycle > 27000000u && hw_c40 >= 0x00000c30u && hw_c40 <= 0x00000c50u && eeMem) {
			s_0c40_dumped = true;
			const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x00000c30);
			Console.WriteLn("@@JIT_0C40_DUMP@@ startpc=%08x cycle=%u", startpc, cpuRegs.cycle);
			for (int j = 0; j < 32; j += 8) {
				Console.WriteLn("@@JIT_0C40_CODE@@ [%04x]: %08x %08x %08x %08x %08x %08x %08x %08x",
					0xc30 + j*4,
					c[j],c[j+1],c[j+2],c[j+3],c[j+4],c[j+5],c[j+6],c[j+7]);
			}
		}
	}

	// [iter_EF18_DUMP_JIT] @@JIT_EF18_DUMP@@ – 0x8000ef18-0x8000ef60 コードダンプ (ef24 分岐内容verify)
	// 目的: ef24 の JAL 先と、分岐conditionをverifyする
	// Removal condition: 書き込み元コードafter determined
	{
		static bool s_ef18_dumped = false;
		const u32 hw_e = HWADDR(startpc);
		if (!s_ef18_dumped && cpuRegs.cycle > 27000000u && hw_e >= 0x0000ef00u && hw_e <= 0x0000ef30u && eeMem) {
			s_ef18_dumped = true;
			const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x0000ef10);
			Console.WriteLn("@@JIT_EF18_DUMP@@ startpc=%08x cycle=%u", startpc, cpuRegs.cycle);
			for (int j = 0; j < 24; j += 8) {
				int lim = 24;
				Console.WriteLn("@@JIT_EF18_CODE@@ [ef%02x]: %08x %08x %08x %08x %08x %08x %08x %08x",
					0x10 + j*4,
					c[j],c[j+1],c[j+2],c[j+3],c[j+4],c[j+5],c[j+6],c[j+7]);
			}
		}
	}

	// [iter_10B8_JIT] @@JIT_10B8_DUMP@@ – 0x80001040-0x800010c4 コードダンプ (JIT vs Interpreter 分岐点特定)
	// 目的: MEM82000書き込みloopへ至る BIOS functionのアセンブリを取得する
	// Removal condition: 書き込み元コードafter determined
	{
		static bool s_10b8_dumped = false;
		const u32 hw_10 = HWADDR(startpc);
		if (!s_10b8_dumped && cpuRegs.cycle > 27000000u && hw_10 >= 0x00001040u && hw_10 <= 0x000010c4u && eeMem) {
			s_10b8_dumped = true;
			const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x00001040);
			// 33 words = 0x1040-0x800010c4
			Console.WriteLn("@@JIT_10B8_DUMP@@ startpc=%08x cycle=%u", startpc, cpuRegs.cycle);
			for (int j = 0; j < 34; j += 8) {
				int lim = (j+8 < 34) ? j+8 : 34;
				Console.WriteLn("@@JIT_10B8_CODE@@ [%04x]: %08x %08x %08x %08x %08x %08x %08x %08x",
					0x1040 + j*4,
					j<lim?c[j]:0, j+1<lim?c[j+1]:0, j+2<lim?c[j+2]:0, j+3<lim?c[j+3]:0,
					j+4<lim?c[j+4]:0, j+5<lim?c[j+5]:0, j+6<lim?c[j+6]:0, j+7<lim?c[j+7]:0);
			}
		}
	}

	// [iter_2C40_JIT] @@JIT_2C40_DUMP@@ – 0x80002C30-0x80002CB0 コードダンプ (0x80002C40: a0=0x3c をconfigして 0x80000C40 に渡すfunction)
	// 目的: JIT が 0x80002C40 をcompile時にどの命令コードを持つかverifyし、Interpreter との差分をdetect
	// Removal condition: a0=0xf vs 0x3c 差分の root cause after determined
	{
		static bool s_2c40_dumped = false;
		const u32 hw_2c = HWADDR(startpc);
		if (!s_2c40_dumped && hw_2c >= 0x00002c30u && hw_2c <= 0x00002c60u && eeMem) {
			s_2c40_dumped = true;
			const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x00002c30);
			Console.WriteLn("@@JIT_2C40_DUMP@@ startpc=%08x cycle=%u", startpc, cpuRegs.cycle);
			// 32 words = 0x80 bytes: 0x80002C30-0x80002CAF
			for (int j = 0; j < 32; j += 8)
				Console.WriteLn("@@JIT_2C40_CODE@@ [%04x]: %08x %08x %08x %08x %08x %08x %08x %08x",
					0x2c30 + j*4,
					c[j],c[j+1],c[j+2],c[j+3],c[j+4],c[j+5],c[j+6],c[j+7]);
		}
	}

	// [@@JIT_1578_COMPILE@@] 0x80001578 ブロックcompile時の ra 値verify
	// 目的: JIT が 0x80001578 を初回compileする時点の cpuRegs.r[31] を取得 → ra=0x80001578 自己loopの根本verify
	// Removal condition: ra 汚染root causeafter determined
	{
		static bool s_1578_dumped = false;
		const u32 hw_15 = HWADDR(startpc);
		if (!s_1578_dumped && hw_15 == 0x00001578u && eeMem) {
			s_1578_dumped = true;
			const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x00001570);
			Console.WriteLn("@@JIT_1578_COMPILE@@ startpc=%08x cycle=%u ra=%08x a0=%08x",
				startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0]);
			Console.WriteLn("@@JIT_1578_CODE@@ [1570]: %08x %08x %08x %08x %08x %08x",
				c[0], c[1], c[2], c[3], c[4], c[5]);
		}
	}

	// [@@JIT_DISPATCH_DUMP@@] 例外 dispatch ブロックのコンテキストダンプ
	// 目的: 0x80000280 付近の dispatch コードをverify (JAL 0x80001564 @ ~0x800002FC)
	// Removal condition: ERET 位置と ra 汚染root causeafter determined
	{
		static int s_dispatch_n = 0;
		const u32 hw_d = HWADDR(startpc);
		if (s_dispatch_n < 5 && hw_d >= 0x00000080u && hw_d <= 0x00000310u && eeMem) {
			Console.WriteLn("@@JIT_DISPATCH_DUMP@@ n=%d startpc=%08x cycle=%u ra=%08x epc=%08x",
				s_dispatch_n, startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.CP0.n.EPC);
			// n==0: startpc 付近 [startpc-0x20, startpc+0x80] をダンプ (40 words = 10 rows)
			if (s_dispatch_n == 0) {
				const u32 base = (hw_d >= 0x20u) ? (hw_d - 0x20u) : 0u;
				const u32* c = reinterpret_cast<const u32*>(eeMem->Main + base);
				for (int j = 0; j < 40; j += 4)
					Console.WriteLn("@@JIT_DISPATCH_CODE@@ [%04x]: %08x %08x %08x %08x",
						base + j*4,
						c[j], c[j+1], c[j+2], c[j+3]);
			}
			s_dispatch_n++;
		}
	}

	// [@@JIT_1570_BLOCK@@] 0x80001570 をincludeブロックのcompile時コンテキストダンプ
	// 目的: JAL 0x800073e0 (at 0x80001570) をincludeブロックの startpc 列挙 (0x80001564 compile有無verify)
	// Removal condition: 0x80001570 のcall元コンテキストと ra 汚染root causeafter determined
	{
		static int s_1570_n = 0;
		const u32 hw_15 = HWADDR(startpc);
		if (s_1570_n < 5 && hw_15 >= 0x00001540u && hw_15 <= 0x00001578u && eeMem) {
			const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x00001530);
			const u32 sr = cpuRegs.CP0.n.Status.val;
			Console.WriteLn("@@JIT_1570_BLOCK@@ n=%d startpc=%08x cycle=%u ra=%08x a0=%08x sp=%08x epc=%08x status=%08x exl=%d",
				s_1570_n, startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0],
				cpuRegs.GPR.n.sp.UL[0], cpuRegs.CP0.n.EPC,
				sr, (int)((sr >> 1) & 1));
			// コードダンプは n==0 の初回のみ (startpc 変化のverifyが目的)
			if (s_1570_n == 0) {
				for (int j = 0; j < 32; j += 4)
					Console.WriteLn("@@JIT_1570_CODE@@ [%04x]: %08x %08x %08x %08x",
						0x1530 + j*4,
						c[j], c[j+1], c[j+2], c[j+3]);
			}
			s_1570_n++;
		}
	}

	// [@@JIT_73E0_COMPILE@@] 0x800073e0 ブロックcompile時の命令列ダンプ (ERET 有無verify)
	// 目的: JIT が 0x800073e0 をcompileする時点の EE RAM [73b0-7460] を取得し 0x80006e88 BEQ target [73b4-73d0] をinclude
	// Removal condition: ERET 実行の有無と EPC→PC 経路after determined
	{
		static bool s_73e0_dumped = false;
		const u32 hw_73 = HWADDR(startpc);
		if (!s_73e0_dumped && hw_73 >= 0x000073d0u && hw_73 <= 0x00007410u && eeMem) {
			s_73e0_dumped = true;
			const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x000073b0);
			Console.WriteLn("@@JIT_73E0_COMPILE@@ startpc=%08x cycle=%u ra=%08x epc=%08x",
				startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.CP0.n.EPC);
			// 0x800073b0-0x8000745f (44 words = 11 rows)
			for (int j = 0; j < 44; j += 4)
				Console.WriteLn("@@JIT_73E0_CODE@@ [%04x]: %08x %08x %08x %08x",
					0x73b0 + j*4,
					c[j], c[j+1], c[j+2], c[j+3]);
		}
	}

	// [@@JIT_6E88_COMPILE@@] 0x80006e88 ブロックcompile時の命令列ダンプ (ERET 有無verify)
	// 目的: 0x800073e0 が呼ぶ 0x80006e88 に ERET(0x42000018) が存在するかverify
	// Removal condition: ERET 実行の有無と EPC→PC 経路after determined
	{
		static bool s_6e88_dumped = false;
		const u32 hw_6e = HWADDR(startpc);
		if (!s_6e88_dumped && hw_6e >= 0x00006e70u && hw_6e <= 0x00006eb0u && eeMem) {
			s_6e88_dumped = true;
			const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x00006e70);
			Console.WriteLn("@@JIT_6E88_COMPILE@@ startpc=%08x cycle=%u ra=%08x epc=%08x",
				startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.ra.UL[0], cpuRegs.CP0.n.EPC);
			// 0x80006e70-0x80006f4f (32 words = 8 rows)
			for (int j = 0; j < 32; j += 4)
				Console.WriteLn("@@JIT_6E88_CODE@@ [%04x]: %08x %08x %08x %08x",
					0x6e70 + j*4,
					c[j], c[j+1], c[j+2], c[j+3]);
		}
	}

	// [TEMP_DIAG] @@JIT_13AA8_COMPILE@@ — J 0x80013aa8 の内容verify (正常脱出パス候補)
	// 目的: 0x80006e70/6e7c の J 0x80013aa8 が ERET 系カーネル出口かどうかをverify
	// Removal condition: 0x80001578 loopの脱出メカニズムafter determined
	{
		static bool s_13aa8_dumped = false;
		const u32 hw_aa = HWADDR(startpc);
		if (!s_13aa8_dumped && hw_aa >= 0x00013a90u && hw_aa <= 0x00013ac0u && eeMem) {
			s_13aa8_dumped = true;
			const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x00013a80);
			Console.WriteLn("@@JIT_13AA8_COMPILE@@ startpc=%08x cycle=%u ra=%08x",
				startpc, cpuRegs.cycle, cpuRegs.GPR.n.ra.UL[0]);
			for (int j = 0; j < 20; j += 4)
				Console.WriteLn("@@JIT_13AA8_CODE@@ [%05x]: %08x %08x %08x %08x",
					0x13a80 + j*4, c[j], c[j+1], c[j+2], c[j+3]);
		}
	}

	// [iter_5388_JIT] @@JIT_5388_DUMP@@ – 0x80005380-0x800053f0 コードダンプ (自己ポインタ書き込み先call元の実態verify)
	// 目的: JIT が 0x80005388 をcompileした時点の EE RAM 内容を取得し、BIOS 自己書き換えパッチの有無をverify
	// Removal condition: a1=0x82000→0x81fc0 差分の root cause after determined
	{
		static bool s_5388_dumped = false;
		const u32 hw_53 = HWADDR(startpc);
		if (!s_5388_dumped && hw_53 >= 0x00005380u && hw_53 <= 0x000053b0u && eeMem) {
			s_5388_dumped = true;
			const u32* c = reinterpret_cast<const u32*>(eeMem->Main + 0x00005380);
			Console.WriteLn("@@JIT_5388_DUMP@@ startpc=%08x cycle=%u a0=%08x a1=%08x s0=%08x s4=%08x ra=%08x",
				startpc, cpuRegs.cycle,
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
				cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s4.UL[0],
				cpuRegs.GPR.n.ra.UL[0]);
			// 0x80005380-0x8000557f (128 words = 16 rows) to cover code divergence at [5500-5580]
			for (int j = 0; j < 128; j += 8)
				Console.WriteLn("@@JIT_5388_CODE@@ [%04x]: %08x %08x %08x %08x %08x %08x %08x %08x",
					0x5380 + j*4,
					c[j],c[j+1],c[j+2],c[j+3],c[j+4],c[j+5],c[j+6],c[j+7]);
		}
	}

	// [iter_POST82] @@POST_EELOAD_EXEC@@ – elf[82000]!=0 after confirmedの 0x8000d000-0x8001FFFF 新ブロック記録
	// 目的: EELOAD ロードafter completedに entry address (0x82000 vs 0x82180) を決定するブロックを特定
	// Removal condition: EELOAD entry 計算の root cause after determined
	{
		static int s_post82_n = 0;
		const u32 hw_q = HWADDR(startpc);
		if (s_post82_n < 40 && cpuRegs.cycle > 27000000u && hw_q >= 0x0000d000u && hw_q <= 0x0001ffffu) {
			const u32 elf00q = eeMem ? *reinterpret_cast<const u32*>(eeMem->Main + 0x82000) : 0u;
			if (elf00q != 0u) {
				Console.WriteLn("@@POST_EELOAD_EXEC@@ n=%d startpc=%08x cycle=%u v0=%08x a0=%08x a1=%08x ra=%08x elf00=%08x",
					s_post82_n, startpc, cpuRegs.cycle,
					cpuRegs.GPR.n.v0.UL[0],
					cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.ra.UL[0], elf00q);
				s_post82_n++;
			}
		}
	}

	// [iter_MEM82000] @@MEM82000_CHANGE@@ – eeMem[82000] 変化detect (誰が書いたか特定)
	// 目的: JIT で 0→0x82000 に変化する startpc を記録
	// Removal condition: 書き込み元 PC after determined
	{
		static u32 s_mem82000_prev = 0u;
		static int s_mem82000_n = 0;
		if (eeMem && s_mem82000_n < 10) {
			const u32 cur = *reinterpret_cast<const u32*>(eeMem->Main + 0x82000);
			if (cur != s_mem82000_prev) {
				Console.WriteLn("@@MEM82000_CHANGE@@ n=%d startpc=%08x cycle=%u OLD=%08x NEW=%08x v0=%08x a0=%08x a1=%08x ra=%08x",
					s_mem82000_n, startpc, cpuRegs.cycle,
					s_mem82000_prev, cur,
					cpuRegs.GPR.n.v0.UL[0],
					cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.ra.UL[0]);
				s_mem82000_prev = cur;
				s_mem82000_n++;
			}
		}
	}

    // [iter37] EE PC=0x02000000 診断probe: BIOS がdisabledaddressに到達するcauseを特定する。
    // Removal condition: EE PC=0x02000000 到達経路 (call元 JR/J 命令の PC と ra) after determined。
    if (startpc == 0x02000000) {
        static bool s_02_logged = false;
        if (!s_02_logged) {
            s_02_logged = true;
            Console.WriteLn("@@RECOMP_02000000@@ ra=%08x sp=%08x v0=%08x a0=%08x pc=%08x",
                cpuRegs.GPR.r[31].UL[0], cpuRegs.GPR.r[29].UL[0],
                cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[4].UL[0], cpuRegs.pc);
        }
    }

    if (recomp_diag)
        DevCon.WriteLn("@@RECOMPILE@@ pc=%08x", startpc);
        if (recomp_diag && startpc == 0xBFC02454) {
            static bool s_probe_2454_logged = false;
            if (!s_probe_2454_logged) {
                s_probe_2454_logged = true;
                Console.WriteLn("@@BFC02454_ENTER@@ curr_pc=%08x ra=%08x v0=%08x a0=%08x t0=%08x t1=%08x",
                    startpc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.a0.UL[0],
                    cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0]);
                if (IsLfAutoDumpOnPanicEnabled())
                    iPSX2_DumpLfCmpStoreRing();
                Console.WriteLn("@@DECISION_DISASM_START@@ window=bfc023b0..bfc023f0");
                for (u32 addr = 0xBFC023B0; addr <= 0xBFC023F0; addr += 4) {
                     u32 op = memRead32(addr);
                     u32 opc = (op >> 26) & 0x3F;
                     s16 imm = static_cast<s16>(op & 0xFFFF);
                     if (opc == 0x04 || opc == 0x05 || opc == 0x06 || opc == 0x07 || opc == 0x01) {
                         u32 target = addr + 4 + (static_cast<s32>(imm) << 2);
                         Console.WriteLn("@@DECISION_DISASM@@ %08x: %08x op=%02x imm=%d target=%08x", addr, op, opc, static_cast<s32>(imm), target);
                     } else {
                         u32 funct = op & 0x3F;
                         if (opc == 0 && funct == 8) // JR
                             Console.WriteLn("@@DECISION_DISASM@@ %08x: %08x JR rs=%d", addr, op, (op>>21)&0x1F);
                         else if (opc == 0 && funct == 9) // JALR
                             Console.WriteLn("@@DECISION_DISASM@@ %08x: %08x JALR rs=%d rd=%d", addr, op, (op>>21)&0x1F, (op>>11)&0x1F);
                         else if (opc == 2 || opc == 3) // J or JAL
                             Console.WriteLn("@@DECISION_DISASM@@ %08x: %08x J/JAL target=%08x", addr, op, ((addr+4)&0xF0000000) | ((op&0x3FFFFFF)<<2));
                         else
                             Console.WriteLn("@@DECISION_DISASM@@ %08x: %08x op=%02x funct=%02x", addr, op, opc, funct);
                     }
                }
                Console.WriteLn("@@DECISION_DISASM_END@@");
            }
        }
	    static bool s_fix_lf_cfg_logged = false;
	    if (!s_fix_lf_cfg_logged)
	    {
	        s_fix_lf_cfg_logged = true;
	        Console.WriteLn("@@CFG@@ iPSX2_FIX_LF_A0_CALLSITE=%d", IsFixLfA0CallsiteEnabled() ? 1 : 0);
	        Console.WriteLn("@@CFG@@ iPSX2_ENABLE_DIAG_FLAGS=%d iPSX2_ALLOW_RECOMPILE_ESCAPE=%d",
	            IsDiagFlagsEnabled() ? 1 : 0, IsRecompileEscapeEnabled() ? 1 : 0);
	        Console.WriteLn("@@CFG@@ iPSX2_LF_AUTODUMP_ON_PANIC=%d", IsLfAutoDumpOnPanicEnabled() ? 1 : 0);
	    }
	    if (DarwinMisc::iPSX2_FORCE_JIT_VERIFY)
	    {
	        static u32 s_recomp_entry_stderr_count = 0;
	        if (s_recomp_entry_stderr_count < 5)
	        {
	            fprintf(stderr, "@@RECOMP_ENTRY_STDERR@@ #%u startpc=%08x\n", s_recomp_entry_stderr_count + 1, startpc);
	            s_recomp_entry_stderr_count++;
	        }
	    }
	    if (DarwinMisc::iPSX2_FORCE_JIT_VERIFY && startpc >= 0xBFC023D0 && startpc <= 0xBFC023E0)
	    {
	        static u32 s_recomp_023d8_window_count = 0;
	        if (s_recomp_023d8_window_count < 8)
	        {
	            const u32 opcode = memRead32(startpc);
	            fprintf(stderr, "@@RECOMP_023D8_WINDOW@@ #%u startpc=%08x opcode=%08x op=%02x rs=%u rt=%u imm=%d\n",
	                s_recomp_023d8_window_count + 1, startpc, opcode, (opcode >> 26) & 0x3f,
	                (opcode >> 21) & 0x1f, (opcode >> 16) & 0x1f, static_cast<s16>(opcode & 0xffff));
	            s_recomp_023d8_window_count++;
	        }
	    }
	    if (recomp_diag && startpc == 0xBFC023D8)
    {
        static bool s_logged_023d8_decode = false;
        if (!s_logged_023d8_decode)
        {
            s_logged_023d8_decode = true;
            const u32 opcode = memRead32(startpc);
            const u32 op = opcode >> 26;
            const char* path = "OTHER";
            if (op == 0x04)
                path = "BEQ";
            else if (op == 0x14)
                path = "BEQL";
            else if (op == 0x05)
                path = "BNE";
            else if (op == 0x15)
                path = "BNEL";
	            Console.WriteLn("@@RECOMP_023D8_DECODE@@ pc=%08x opcode=%08x op=%02x path=%s rs=%u rt=%u imm=%d",
	                startpc, opcode, op, path, (opcode >> 21) & 0x1f, (opcode >> 16) & 0x1f, static_cast<s16>(opcode & 0xffff));
	            fprintf(stderr, "@@RECOMP_023D8_DECODE_STDERR@@ pc=%08x opcode=%08x op=%02x path=%s rs=%u rt=%u imm=%d\n",
	                startpc, opcode, op, path, (opcode >> 21) & 0x1f, (opcode >> 16) & 0x1f, static_cast<s16>(opcode & 0xffff));
	        }
		    }
	    DarwinMisc::g_rec_stage = 1; // Entry
    if (DarwinMisc::iPSX2_CRASH_PACK) {
        Console.WriteLn("@@REC_STAGE@@ stage=1 startpc=%08x", startpc);
    }
	    // Fix: Ensure s_nEndBlock is initialized correctly (legacy)
	    s_nEndBlock = (startpc & ~0xfff) + 0x1000;

		u32 i = 0;
		u32 willbranch3 = 0;

		// pxAssert(startpc); // REMOVED

	// if recPtr reached the mem limit reset whole mem
	if (recPtr >= recPtrEnd)
		eeRecNeedsReset = true;

	if (HWADDR(startpc) == VMManager::Internal::GetCurrentELFEntryPoint())
		VMManager::Internal::EntryPointCompilingOnCPUThread();
        
    // DEBUG LOG (disabled by default; opt-in via iPSX2_RECOMP_DIAG gate)
    if (recomp_diag) {
        static int rec_log_count = 0;
        if (rec_log_count < 200) {
            Console.WriteLn("DEBUG: recRecompile ENTRY %08x END %08x", startpc, s_nEndBlock);
            rec_log_count++;
        }
    }

	if (eeRecNeedsReset)
	{
		eeRecNeedsReset = false;
		recResetRaw();
	}

    // [iter187] Guard: unmapped recLUT page → sentinel → PC_GETBLOCK returns NULL → crash.
    // Sentinel formula (from recLUT_SetPage with mapbase=0):
    //   recLUT[page] = -page * 0x20000  (i.e. recLUT[page] + page * 0x20000 == 0)
    // Valid pages (EE RAM, BIOS ROM) satisfy: recLUT[page] + page * 0x20000 != 0.
    // MUST be before armSetAsmPtr/armStartBlock to avoid leaving a partial JIT block.
    {
        const u32 page = (u32)startpc >> 16;
        if (!(recLUT[page] + (uptr)page * 0x20000u)) {
            // [iter336] PS1DRV entry probes: (a) first OOB ever, (b) first OOB in PS1 exec range 0x24000000+
            {
                static bool s_ps1drv_probed = false;
                static bool s_ps1exec_probed = false;
                const bool is_ps1exec = (startpc >= 0x24000000u && startpc < 0x30000000u);
                if (!s_ps1drv_probed || (is_ps1exec && !s_ps1exec_probed)) {
                    if (!s_ps1drv_probed) s_ps1drv_probed = true;
                    if (is_ps1exec && !s_ps1exec_probed) s_ps1exec_probed = true;
                    const u32 ra = cpuRegs.GPR.r[31].UL[0];
                    const u32 sp = cpuRegs.GPR.r[29].UL[0];
                    u32 st0=0,st1=0,st2=0,st3=0,st4=0;
                    const u32* stk = reinterpret_cast<const u32*>(PSM(sp));
                    if (stk) { st0=stk[0]; st1=stk[1]; st2=stk[2]; st3=stk[3]; st4=stk[4]; }
                    const u32* ra_ptr = reinterpret_cast<const u32*>(PSM(ra));
                    u32 ra0=0,ra1=0;
                    if (ra_ptr) { ra0=ra_ptr[0]; ra1=ra_ptr[1]; }
                    const char* tag = is_ps1exec ? "@@PS1EXEC_ENTRY@@" : "@@PS1DRV_ENTRY@@";
                    Console.WriteLn("%s startpc=%08x ra=%08x sp=%08x k0=%08x k1=%08x v0=%08x a0=%08x",
                        tag,
                        startpc, ra, sp,
                        cpuRegs.GPR.r[26].UL[0], cpuRegs.GPR.r[27].UL[0],
                        cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.a0.UL[0]);
                    Console.WriteLn("@@PS1DRV_STK@@ tag=%s sp[0..4]=%08x %08x %08x %08x %08x ra_code[0..1]=%08x %08x",
                        tag,
                        st0,st1,st2,st3,st4, ra0,ra1);
                    Console.WriteLn("@@PS1DRV_EPC@@ epc=%08x cause=%08x status=%08x",
                        cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.Status.val);
                }
            }
            static int s_oob_cnt = 0;
            if (s_oob_cnt++ < 5) {
                // [iter300] physRam: startpc & 0x1FFFFFF → EE RAM wrap address の内容をverify
                Console.WriteLn("@@OOB_PC_GUARD@@ startpc=%08x ra=%08x t9=%08x gp=%08x physRam[0]=%08x physRam[1]=%08x",
                    startpc, cpuRegs.GPR.r[31].UL[0],
                    cpuRegs.GPR.r[25].UL[0], cpuRegs.GPR.r[28].UL[0],
                    memRead32(startpc & 0x01FFFFFFu),
                    memRead32((startpc & 0x01FFFFFFu) + 4u));
                // Dump BIOS ROM at call site (ra-8..ra+4) to identify the indirect call instruction
                const u32 ra_addr = cpuRegs.GPR.r[31].UL[0];
                const u32* cs = reinterpret_cast<const u32*>(PSM(ra_addr - 8));
                if (cs)
                    Console.WriteLn("@@OOB_CALL_SITE@@ [ra-8]=%08x [ra-4]=%08x [ra]=%08x [ra+4]=%08x",
                        cs[0], cs[1], cs[2], cs[3]);
            }
            // [iter291] ra の命令を解析してリダイレクト先を決定。OOB callを no-op (v0=0) として扱う。
            // - J/JAL (opcode=2/3): ra+8 へ (J/JAL + 遅延スロットをskip)
            // - BREAK (opcode=0 funct=0xD): ra+4 へ。続く命令が J/JAL なら ra+12 へ (BREAK+J+DS)
            // - その他: ra に戻る (通常の return)
            // Removal condition: OOB ジャンプのroot cause (カーネルfunctionポインタ未初期化) after fixed
            {
                const u32 ra_addr = cpuRegs.GPR.r[31].UL[0];
                const u32* ra_insn_ptr = reinterpret_cast<const u32*>(PSM(ra_addr));
                u32 redirect_pc = ra_addr;
                if (ra_insn_ptr) {
                    const u32 insn0 = *ra_insn_ptr;
                    const u32 opcode0 = insn0 >> 26;
                    if (opcode0 == 2 || opcode0 == 3) { // J or JAL
                        // [iter307] J/JAL の target が OOB かどうかをverifyしてからskipを決定。
                        // Valid target (BIOS ROM など) の場合はskipせず、そのまま実行させる。
                        // Removal condition: OOB redirect ロジック全般の見直し後
                        const u32 jal_target = ((ra_addr + 4u) & 0xF0000000u) | ((insn0 & 0x03FFFFFFu) << 2u);
                        // [iter310] BIOS ROM 物理range (0x1FC00000-0x1FFFFFFF) は vtlb handler マップのため
                        // PSM()=null かつ recLUT sentinel になるが valid。物理addressrangeで除外する。
                        // Removal condition: OOB redirect ロジック全般の見直し後
                        const u32 jal_phys = jal_target & 0x1FFFFFFFu;
                        const bool jal_in_bios_rom = (jal_phys >= 0x1FC00000u && jal_phys <= 0x1FFFFFFFu);
                        const bool jal_target_oob = !jal_in_bios_rom &&
                            !(recLUT[jal_target >> 16] + (uptr)(jal_target >> 16) * 0x20000u);
                        if (jal_target_oob) {
                            redirect_pc = ra_addr + 8; // skip J/JAL + delay slot
                            if (opcode0 == 3) // JAL: link register を更新
                                cpuRegs.GPR.r[31].UL[0] = ra_addr + 8;
                        }
                        // else: target は valid → redirect_pc = ra_addr (JAL をskipしない)
                    } else if (opcode0 == 0 && (insn0 & 0x3F) == 0x0D) { // BREAK → skip (no DS)
                        redirect_pc = ra_addr + 4;
                        // 次の命令も J/JAL なら一緒にskip
                        const u32* next_ptr = reinterpret_cast<const u32*>(PSM(ra_addr + 4));
                        if (next_ptr) {
                            const u32 opcode1 = (*next_ptr) >> 26;
                            if (opcode1 == 2 || opcode1 == 3) // J or JAL + delay slot
                                redirect_pc = ra_addr + 12;
                        }
                    }
                }
                // [iter338] TEMP_DIAG: redirect_pc も OOB なら cycle-freeze loopが確定する。
                // 最大3回だけログして freeze 地点を特定する。
                // Removal condition: OOB freeze startpc after identified
                {
                    static int s_freeze_cnt = 0;
                    if (s_freeze_cnt < 3) {
                        const u32 rpage = redirect_pc >> 16;
                        const u32 rphys = redirect_pc & 0x1FFFFFFFu;
                        const bool r_bios_rom = (rphys >= 0x1FC00000u && rphys <= 0x1FFFFFFFu);
                        const bool r_oob = !r_bios_rom && !(recLUT[rpage] + (uptr)rpage * 0x20000u);
                        if (r_oob) {
                            ++s_freeze_cnt;
                            Console.WriteLn("@@OOB_FREEZE@@ n=%d startpc=%08x redirect=%08x ra=%08x k0=%08x k1=%08x epc=%08x cycle=%u",
                                s_freeze_cnt, startpc, redirect_pc, cpuRegs.GPR.r[31].UL[0],
                                cpuRegs.GPR.r[26].UL[0], cpuRegs.GPR.r[27].UL[0],
                                cpuRegs.CP0.n.EPC, cpuRegs.cycle);
                        }
                    }
                }
                // [iter311] 0x90000000 range (EE OS サービス) に対して v0=1 (success) を返す。
                // v0=0 の場合 BIOS が同じfunctionを再度呼ぶリトライloopに入るためallsuccess扱い。
                // Removal condition: EE OS サービス (SIF/IOP) が正しくbehaviorするようになった時
                cpuRegs.GPR.n.v0.UD[0] = (startpc >= 0x90000000u) ? 1u : 0u;
                cpuRegs.pc = redirect_pc;
            }
            return;
        }
    }

    // [iter342] Guard: PSM(startpc) == NULL → 仮想addressが物理にマップされていない。
    // recLUT[page] = 0 (初期値) の場合 OOB センチネルdetermineをすり抜けるためaddガード。
    // early return のみでは cpuRegs.pc = startpc のまま → 無限loop → cycle 凍結。
    // OOB ガードと同様 $ra へリダイレクトして脱出する。
    // Removal condition: PS1DRV 仮想address (0x24020000) のmappingissue解決後
    if (!PSM(startpc)) {
        static int s_psm_cnt = 0;
        if (s_psm_cnt++ < 5) {
            Console.WriteLn("@@PSM_NULL_REDIRECT@@ n=%d startpc=%08x ra=%08x cycle=%u",
                s_psm_cnt, startpc, cpuRegs.GPR.r[31].UL[0], cpuRegs.cycle);
        }
        cpuRegs.GPR.n.v0.UD[0] = 0u;
        cpuRegs.pc = cpuRegs.GPR.r[31].UL[0]; // $ra へリダイレクト
        return;
    }

    armSetAsmPtr(recPtr, _256kb, nullptr);
    recPtr = armStartBlock();
    s_jit_compile_count.fetch_add(1, std::memory_order_relaxed);

    // [iPSX2] Flight Recorder: Log Block Entry - REMOVED (Verified OK)
    /*
    {
        struct FlightRec { ... };
        ...
    }
    */

    // Re-ensure RSTATEs are pointing to the right places
    // [TEMP_DIAG] @@PREAMBLE_ADDR@@ verify address before embedding
    {
        static bool s_preamble_addr_logged = false;
        if (!s_preamble_addr_logged) {
            s_preamble_addr_logged = true;
            Console.WriteLn("@@PREAMBLE_ADDR@@ recLUT=%p psxRegs=%p pack=%p pack.cycle_off=0x%lx startpc=%08x",
                (void*)&recLUT, (void*)&psxRegs, (void*)&g_cpuRegistersPack,
                (unsigned long)((uintptr_t)&g_cpuRegistersPack.cpuRegs.cycle - (uintptr_t)&g_cpuRegistersPack),
                startpc);
        }
    }
    armMoveAddressToReg(RSTATE_x29, &recLUT);
    armMoveAddressToReg(RSTATE_PSX, &psxRegs);
    armMoveAddressToReg(RSTATE_CPU, &g_cpuRegistersPack);
    // [TEMP_DIAG] @@PREAMBLE_VERIFY@@ removed — no longer needed
    if (CHECK_FASTMEM) {
        armAsm->Ldr(RFASTMEMBASE, PTR_CPU(vtlbdata.fastmem_base));
    }

    // char buf[64];
    // int len = snprintf(buf, sizeof(buf), "DEBUG: recRecompile(0x%08x) ENTRY\n", startpc);
    // write(1, buf, len);
    // Console.WriteLn("DEBUG: recRecompile(0x%08x) ENTRY", startpc);

	s_pCurBlock = PC_GETBLOCK(startpc);

    // Android/PCSX2 standard: We should only be here if the block is NOT yet compiled.
    // The previous shortcuts (returning if compiled) are removed as they hide bugs.
    // [iPSX2] RECOVERY: Replace fatal assert with escape logic
    if (s_pCurBlock->GetFnptr() != (uptr)JITCompile) {
        Console.WriteLn("@@RECOMPILE_ASSERT_ESCAPED@@ startpc=%08x fnptr=%p expected=%p", 
            startpc, (void*)s_pCurBlock->GetFnptr(), (void*)JITCompile);
            
        if (startpc < 0x1000) {
            Console.WriteLn("@@BAD_STARTPC@@ Preventing abort for invalid PC=%08x. Returning.", startpc);
            // [iter81] @@LOW_PC_REGS@@ – EE GPR context to identify what caused PC<0x1000
            {
                u32 ra  = cpuRegs.GPR.r[31].UL[0];
                u32 v0  = cpuRegs.GPR.r[2].UL[0];
                u32 v1  = cpuRegs.GPR.r[3].UL[0];
                u32 ram0 = (eeMem) ? *(const u32*)eeMem->Main : 0xDEADBEEFu;
                Console.WriteLn("@@LOW_PC_REGS@@ cpupc=%08x ra=%08x v0=%08x v1=%08x ram0=%08x",
                    cpuRegs.pc, ra, v0, v1, ram0);
            }
            // [iter85] @@LOW_PC_EXT@@ – read EE RAM at expected $ra save slot
            // JR $ra delay slot restores $sp: sp_probe = sp AFTER ADDIU +0x30.
            // SD $ra, 0x20($sp) in block 0x82E90 writes to (sp_probe-0x30)+0x20 = sp_probe-0x10.
            // Removal condition: $ra=1 のroot cause（スタック汚染 vs. JITregister不整合）after identified
            if (eeMem) {
                u32 sp_v  = cpuRegs.GPR.r[29].UL[0];         // $sp after ADDIU+0x30 delay slot
                u32 sp_b  = sp_v - 0x30;                      // $sp as seen by SD/LD in 0x82E90/0x82F1C
                u32 ra_slot = sp_b + 0x20;                    // LD $ra, 0x20($sp)
                u32 ram_ra = *(const u32*)(eeMem->Main + ra_slot);  // lower 32b of saved $ra
                u32 ram0_v = *(const u32*)eeMem->Main;        // EE RAM[0]
                Console.WriteLn("@@LOW_PC_EXT@@ sp=%08x sp_b=%08x ra_slot=%08x ram[ra_slot]=%08x ram[0]=%08x",
                    sp_v, sp_b, ra_slot, ram_ra, ram0_v);
            }
            // [iter82] @@BIOS_OPCODE_DUMP@@ – dump MIPS words at suspect blocks to find LW $ra,0($zero)
            if (eeMem) {
                static bool s_dumped = false;
                if (!s_dumped) {
                    s_dumped = true;
                    const u32* m = (const u32*)eeMem->Main;
                    // Blocks compiled after SIGBUS: 0x82e90, 0x82ee8, 0x82f1c (each shows 6 words)
                    Console.WriteLn("@@BIOS_OPCODE_DUMP@@ 82e90: %08x %08x %08x %08x %08x %08x",
                        m[0x82e90/4], m[0x82e94/4], m[0x82e98/4], m[0x82e9c/4], m[0x82ea0/4], m[0x82ea4/4]);
                    Console.WriteLn("@@BIOS_OPCODE_DUMP@@ 82ee8: %08x %08x %08x %08x %08x %08x",
                        m[0x82ee8/4], m[0x82eec/4], m[0x82ef0/4], m[0x82ef4/4], m[0x82ef8/4], m[0x82efc/4]);
                    Console.WriteLn("@@BIOS_OPCODE_DUMP@@ 82f1c: %08x %08x %08x %08x %08x %08x",
                        m[0x82f1c/4], m[0x82f20/4], m[0x82f24/4], m[0x82f28/4], m[0x82f2c/4], m[0x82f30/4]);
                    Console.WriteLn("@@BIOS_OPCODE_DUMP@@ 82f30: %08x %08x %08x %08x %08x %08x",
                        m[0x82f30/4], m[0x82f34/4], m[0x82f38/4], m[0x82f3c/4], m[0x82f40/4], m[0x82f44/4]);
                    // [iter83] BNE target (initialized path) + 0x82388 call site
                    Console.WriteLn("@@BIOS_OPCODE_DUMP@@ 82f4c: %08x %08x %08x %08x %08x %08x",
                        m[0x82f4c/4], m[0x82f50/4], m[0x82f54/4], m[0x82f58/4], m[0x82f5c/4], m[0x82f60/4]);
                    Console.WriteLn("@@BIOS_OPCODE_DUMP@@ 82380: %08x %08x %08x %08x %08x %08x %08x %08x",
                        m[0x82380/4], m[0x82384/4], m[0x82388/4], m[0x8238c/4],
                        m[0x82390/4], m[0x82394/4], m[0x82398/4], m[0x8239c/4]);
                    Console.WriteLn("@@BIOS_OPCODE_DUMP@@ 823a0: %08x %08x %08x %08x %08x %08x %08x %08x",
                        m[0x823a0/4], m[0x823a4/4], m[0x823a8/4], m[0x823ac/4],
                        m[0x823b0/4], m[0x823b4/4], m[0x823b8/4], m[0x823bc/4]);
                    Console.WriteLn("@@BIOS_OPCODE_DUMP@@ mem935a8=%08x", m[0x935a8/4]);
                }
            }
            if (IsRecompileEscapeEnabled())
                return;
        }
        
        // If it appears valid (not 0/1), assume it was already compiled or race condition.
        // Return without aborting.
        if (IsRecompileEscapeEnabled())
            return;
    }

	// @@BIOS_9FC432E8_INSN@@ iter13: one-shot dump of first 8 instructions at crash block
	if (startpc == 0x9FC432E8u)
	{
		static bool s_bios_seen = false;
		if (!s_bios_seen)
		{
			s_bios_seen = true;
			Console.WriteLn("@@BIOS_9FC432E8_INSN@@ pc=%08x i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x",
				startpc,
				memRead32(startpc + 0x00), memRead32(startpc + 0x04),
				memRead32(startpc + 0x08), memRead32(startpc + 0x0C),
				memRead32(startpc + 0x10), memRead32(startpc + 0x14),
				memRead32(startpc + 0x18), memRead32(startpc + 0x1C));
		}
	}

	// @@BIOS_43120_INSN@@ iter08: one-shot dump of 12 instructions at 9FC43120 polling loop
	if (startpc == 0x9FC43120u)
	{
		static bool s_43120_seen = false;
		if (!s_43120_seen)
		{
			s_43120_seen = true;
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43120_INSN@@ pc=%08x i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x i8=%08x i9=%08x iA=%08x iB=%08x",
				startpc,
				memRead32(startpc + 0x00), memRead32(startpc + 0x04),
				memRead32(startpc + 0x08), memRead32(startpc + 0x0C),
				memRead32(startpc + 0x10), memRead32(startpc + 0x14),
				memRead32(startpc + 0x18), memRead32(startpc + 0x1C),
				memRead32(startpc + 0x20), memRead32(startpc + 0x24),
				memRead32(startpc + 0x28), memRead32(startpc + 0x2C));
		}
	}

	// @@BIOS_432C0_INSN@@ iter09: one-shot dump of 16 instructions at 9FC432C0 (calling loop)
	if (startpc == 0x9FC432C0u)
	{
		static bool s_432c0_seen = false;
		if (!s_432c0_seen)
		{
			s_432c0_seen = true;
			Console.WriteLn("[TEMP_DIAG] @@BIOS_432C0_INSN@@ pc=%08x i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x",
				startpc,
				memRead32(startpc + 0x00), memRead32(startpc + 0x04),
				memRead32(startpc + 0x08), memRead32(startpc + 0x0C),
				memRead32(startpc + 0x10), memRead32(startpc + 0x14),
				memRead32(startpc + 0x18), memRead32(startpc + 0x1C),
				memRead32(startpc + 0x20), memRead32(startpc + 0x24),
				memRead32(startpc + 0x28), memRead32(startpc + 0x2C),
				memRead32(startpc + 0x30), memRead32(startpc + 0x34),
				memRead32(startpc + 0x38), memRead32(startpc + 0x3C));
		}
	}

	// @@BIOS_4115C_INSN@@ iter12: one-shot dump of 24 instructions starting at 9FC4115C
	// Covers 9FC4115C (JAL block) AND 9FC41164 onwards (post-return code)
	// Replaces @@BIOS_41164_INSN@@ which never fired (9FC41164 not compiled as separate block)
	if (startpc == 0x9FC4115Cu)
	{
		static bool s_4115c_seen = false;
		if (!s_4115c_seen)
		{
			s_4115c_seen = true;
			// First 8 words: 9FC4115C - 9FC41178 (the JAL block and delay slot)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_4115C_INSN@@ A: pc=%08x i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x",
				startpc,
				memRead32(0x9FC4115C), memRead32(0x9FC41160),
				memRead32(0x9FC41164), memRead32(0x9FC41168),
				memRead32(0x9FC4116C), memRead32(0x9FC41170),
				memRead32(0x9FC41174), memRead32(0x9FC41178));
			// Next 8 words: 9FC4117C - 9FC41198
			Console.WriteLn("[TEMP_DIAG] @@BIOS_4115C_INSN@@ B: i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x",
				memRead32(0x9FC4117C), memRead32(0x9FC41180),
				memRead32(0x9FC41184), memRead32(0x9FC41188),
				memRead32(0x9FC4118C), memRead32(0x9FC41190),
				memRead32(0x9FC41194), memRead32(0x9FC41198));
			// Next 8 words: 9FC4119C - 9FC411B8 (outer loop condition / branch back)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_4115C_INSN@@ C: iG=%08x iH=%08x iI=%08x iJ=%08x iK=%08x iL=%08x iM=%08x iN=%08x",
				memRead32(0x9FC4119C), memRead32(0x9FC411A0),
				memRead32(0x9FC411A4), memRead32(0x9FC411A8),
				memRead32(0x9FC411AC), memRead32(0x9FC411B0),
				memRead32(0x9FC411B4), memRead32(0x9FC411B8));
			// D: 9FC411BC - 9FC411D8 (delay slot of JAL 9FC41268 + post-return loop-back logic)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_4115C_INSN@@ D: iO=%08x iP=%08x iQ=%08x iR=%08x iS=%08x iT=%08x iU=%08x iV=%08x",
				memRead32(0x9FC411BC), memRead32(0x9FC411C0),
				memRead32(0x9FC411C4), memRead32(0x9FC411C8),
				memRead32(0x9FC411CC), memRead32(0x9FC411D0),
				memRead32(0x9FC411D4), memRead32(0x9FC411D8));
			// E: 9FC411DC - 9FC411F4 (delay slot of BEQ +28 + code between D and 4124C)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_4115C_INSN@@ E: iW=%08x iX=%08x iY=%08x iZ=%08x ja=%08x jb=%08x jc=%08x jd=%08x",
				memRead32(0x9FC411DC), memRead32(0x9FC411E0),
				memRead32(0x9FC411E4), memRead32(0x9FC411E8),
				memRead32(0x9FC411EC), memRead32(0x9FC411F0),
				memRead32(0x9FC411F4), memRead32(0x9FC411F8));
			// F: 9FC4124C - 9FC41268 (BEQ target = loop-back / loop-exit candidate)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_4115C_INSN@@ F: je=%08x jf=%08x jg=%08x jh=%08x ji=%08x jj=%08x jk=%08x jl=%08x",
				memRead32(0x9FC4124C), memRead32(0x9FC41250),
				memRead32(0x9FC41254), memRead32(0x9FC41258),
				memRead32(0x9FC4125C), memRead32(0x9FC41260),
				memRead32(0x9FC41264), memRead32(0x9FC41268));
		}
	}

	// @@BIOS_43150_INSN@@ iter13: one-shot dump of 24 instructions at 9FC43150
	// 9FC43150 is called from 9FC4115C (JAL) with $ra=9FC41164 but never returns there.
	// Dumping to find internal loop structure.
	if (startpc == 0x9FC43150u)
	{
		static bool s_43150_seen = false;
		if (!s_43150_seen)
		{
			s_43150_seen = true;
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43150_INSN@@ A: pc=%08x i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x",
				startpc,
				memRead32(0x9FC43150), memRead32(0x9FC43154),
				memRead32(0x9FC43158), memRead32(0x9FC4315C),
				memRead32(0x9FC43160), memRead32(0x9FC43164),
				memRead32(0x9FC43168), memRead32(0x9FC4316C));
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43150_INSN@@ B: i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x",
				memRead32(0x9FC43170), memRead32(0x9FC43174),
				memRead32(0x9FC43178), memRead32(0x9FC4317C),
				memRead32(0x9FC43180), memRead32(0x9FC43184),
				memRead32(0x9FC43188), memRead32(0x9FC4318C));
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43150_INSN@@ C: iG=%08x iH=%08x iI=%08x iJ=%08x iK=%08x iL=%08x iM=%08x iN=%08x",
				memRead32(0x9FC43190), memRead32(0x9FC43194),
				memRead32(0x9FC43198), memRead32(0x9FC4319C),
				memRead32(0x9FC431A0), memRead32(0x9FC431A4),
				memRead32(0x9FC431A8), memRead32(0x9FC431AC));
			// D: 9FC431B0 - 9FC431CF (post-loop: ROM count check and branch to 9FC43220)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43150_INSN@@ D: iO=%08x iP=%08x iQ=%08x iR=%08x iS=%08x iT=%08x iU=%08x iV=%08x",
				memRead32(0x9FC431B0), memRead32(0x9FC431B4),
				memRead32(0x9FC431B8), memRead32(0x9FC431BC),
				memRead32(0x9FC431C0), memRead32(0x9FC431C4),
				memRead32(0x9FC431C8), memRead32(0x9FC431CC));
			// E: 9FC431D0 - 9FC431EC (fallthrough from $s2>=48 path)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43150_INSN@@ E: iW=%08x iX=%08x iY=%08x iZ=%08x ja=%08x jb=%08x jc=%08x jd=%08x",
				memRead32(0x9FC431D0), memRead32(0x9FC431D4),
				memRead32(0x9FC431D8), memRead32(0x9FC431DC),
				memRead32(0x9FC431E0), memRead32(0x9FC431E4),
				memRead32(0x9FC431E8), memRead32(0x9FC431EC));
			// F: 9FC43220 - 9FC4323C (target of unconditional branch when $s2<48)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43150_INSN@@ F: je=%08x jf=%08x jg=%08x jh=%08x ji=%08x jj=%08x jk=%08x jl=%08x",
				memRead32(0x9FC43220), memRead32(0x9FC43224),
				memRead32(0x9FC43228), memRead32(0x9FC4322C),
				memRead32(0x9FC43230), memRead32(0x9FC43234),
				memRead32(0x9FC43238), memRead32(0x9FC4323C));
			// G: 9FC431F0 - 9FC4320F (continuation after E-section JAL 9FC43120, delay slot + loop-back?)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43150_INSN@@ G: jm=%08x jn=%08x jo=%08x jp=%08x jq=%08x jr=%08x js=%08x jt=%08x",
				memRead32(0x9FC431F0), memRead32(0x9FC431F4),
				memRead32(0x9FC431F8), memRead32(0x9FC431FC),
				memRead32(0x9FC43200), memRead32(0x9FC43204),
				memRead32(0x9FC43208), memRead32(0x9FC4320C));
			// H: ROM data values that control branching in 9FC43150
			// $s2=ROM[0x9FC43C10] controls which TLB-setup path is taken
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43150_INSN@@ H: rom_3C10=%08x rom_3C14=%08x rom_3C18=%08x rom_3C1C=%08x rom_3EF0=%08x rom_3EF4=%08x",
				memRead32(0x9FC43C10), memRead32(0x9FC43C14),
				memRead32(0x9FC43C18), memRead32(0x9FC43C1C),
				memRead32(0x9FC43EF0), memRead32(0x9FC43EF4));
		}
	}

	// @@BIOS_43460_INSN@@ iter20: one-shot dump of 24 instructions at 9FC43460
	// Called from 9FC43150 F-path (JAL 9FC43460 at 9FC43220) with $a0=0x9FC43EF0
	// Also called from 9FC411D0 (failure path of 9FC41268) with $a0=0x9FC43E70
	// Suspected to contain TLB flush / 9FC432C0-related loop
	if (startpc == 0x9FC43460u)
	{
		static bool s_43460_seen = false;
		if (!s_43460_seen)
		{
			s_43460_seen = true;
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43460_INSN@@ A: pc=%08x i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x",
				startpc,
				memRead32(0x9FC43460), memRead32(0x9FC43464),
				memRead32(0x9FC43468), memRead32(0x9FC4346C),
				memRead32(0x9FC43470), memRead32(0x9FC43474),
				memRead32(0x9FC43478), memRead32(0x9FC4347C));
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43460_INSN@@ B: i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x",
				memRead32(0x9FC43480), memRead32(0x9FC43484),
				memRead32(0x9FC43488), memRead32(0x9FC4348C),
				memRead32(0x9FC43490), memRead32(0x9FC43494),
				memRead32(0x9FC43498), memRead32(0x9FC4349C));
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43460_INSN@@ C: iG=%08x iH=%08x iI=%08x iJ=%08x iK=%08x iL=%08x iM=%08x iN=%08x",
				memRead32(0x9FC434A0), memRead32(0x9FC434A4),
				memRead32(0x9FC434A8), memRead32(0x9FC434AC),
				memRead32(0x9FC434B0), memRead32(0x9FC434B4),
				memRead32(0x9FC434B8), memRead32(0x9FC434BC));
		}
	}

	// @@BIOS_43120_INSN@@ iter21: one-shot dump of 48 instructions at 9FC43120
	// 目的: JAL 9FC43120 の内部で EE PC が 9FC432C0/9FC432DC で循環するcauseを解析
	if (startpc == 0x9FC43120u)
	{
		static bool s_43120_seen = false;
		if (!s_43120_seen)
		{
			s_43120_seen = true;
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43120_INSN@@ A: pc=%08x i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x",
				startpc, memRead32(0x9FC43120), memRead32(0x9FC43124),
				memRead32(0x9FC43128), memRead32(0x9FC4312C),
				memRead32(0x9FC43130), memRead32(0x9FC43134),
				memRead32(0x9FC43138), memRead32(0x9FC4313C));
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43120_INSN@@ B: i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x",
				memRead32(0x9FC43140), memRead32(0x9FC43144),
				memRead32(0x9FC43148), memRead32(0x9FC4314C),
				memRead32(0x9FC43150), memRead32(0x9FC43154),
				memRead32(0x9FC43158), memRead32(0x9FC4315C));
			// 9FC432A0-9FC432BF (EE PC 9FC432C0 付近の直前)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43120_INSN@@ C: 432a0=%08x 432a4=%08x 432a8=%08x 432ac=%08x 432b0=%08x 432b4=%08x 432b8=%08x 432bc=%08x",
				memRead32(0x9FC432A0), memRead32(0x9FC432A4),
				memRead32(0x9FC432A8), memRead32(0x9FC432AC),
				memRead32(0x9FC432B0), memRead32(0x9FC432B4),
				memRead32(0x9FC432B8), memRead32(0x9FC432BC));
			// 9FC432C0-9FC432DF (EE PC 9FC432C0/9FC432DC 周辺)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43120_INSN@@ D: 432c0=%08x 432c4=%08x 432c8=%08x 432cc=%08x 432d0=%08x 432d4=%08x 432d8=%08x 432dc=%08x",
				memRead32(0x9FC432C0), memRead32(0x9FC432C4),
				memRead32(0x9FC432C8), memRead32(0x9FC432CC),
				memRead32(0x9FC432D0), memRead32(0x9FC432D4),
				memRead32(0x9FC432D8), memRead32(0x9FC432DC));
			// 9FC432E0-9FC432FF (エピローグ候補)
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43120_INSN@@ E: 432e0=%08x 432e4=%08x 432e8=%08x 432ec=%08x 432f0=%08x 432f4=%08x 432f8=%08x 432fc=%08x",
				memRead32(0x9FC432E0), memRead32(0x9FC432E4),
				memRead32(0x9FC432E8), memRead32(0x9FC432EC),
				memRead32(0x9FC432F0), memRead32(0x9FC432F4),
				memRead32(0x9FC432F8), memRead32(0x9FC432FC));
			// 9FC43300-9FC4331F
			Console.WriteLn("[TEMP_DIAG] @@BIOS_43120_INSN@@ F: 43300=%08x 43304=%08x 43308=%08x 4330c=%08x 43310=%08x 43314=%08x 43318=%08x 4331c=%08x",
				memRead32(0x9FC43300), memRead32(0x9FC43304),
				memRead32(0x9FC43308), memRead32(0x9FC4330C),
				memRead32(0x9FC43310), memRead32(0x9FC43314),
				memRead32(0x9FC43318), memRead32(0x9FC4331C));
		}
	}

	// @@BIOS_433F0_INSN@@ iter22: one-shot dump of 32 instructions at 9FC433F0
	// 目的: EE PC が 9FC433F0 で停滞するcauseを解析
	if (startpc == 0x9FC433F0u)
	{
		static bool s_433f0_seen = false;
		if (!s_433f0_seen)
		{
			s_433f0_seen = true;
			Console.WriteLn("[TEMP_DIAG] @@BIOS_433F0_INSN@@ A: pc=%08x i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x",
				startpc, memRead32(0x9FC433F0), memRead32(0x9FC433F4),
				memRead32(0x9FC433F8), memRead32(0x9FC433FC),
				memRead32(0x9FC43400), memRead32(0x9FC43404),
				memRead32(0x9FC43408), memRead32(0x9FC4340C));
			Console.WriteLn("[TEMP_DIAG] @@BIOS_433F0_INSN@@ B: i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x",
				memRead32(0x9FC43410), memRead32(0x9FC43414),
				memRead32(0x9FC43418), memRead32(0x9FC4341C),
				memRead32(0x9FC43420), memRead32(0x9FC43424),
				memRead32(0x9FC43428), memRead32(0x9FC4342C));
			Console.WriteLn("[TEMP_DIAG] @@BIOS_433F0_INSN@@ C: iG=%08x iH=%08x iI=%08x iJ=%08x iK=%08x iL=%08x iM=%08x iN=%08x",
				memRead32(0x9FC43430), memRead32(0x9FC43434),
				memRead32(0x9FC43438), memRead32(0x9FC4343C),
				memRead32(0x9FC43440), memRead32(0x9FC43444),
				memRead32(0x9FC43448), memRead32(0x9FC4344C));
			Console.WriteLn("[TEMP_DIAG] @@BIOS_433F0_INSN@@ D: iO=%08x iP=%08x iQ=%08x iR=%08x iS=%08x iT=%08x iU=%08x iV=%08x",
				memRead32(0x9FC43450), memRead32(0x9FC43454),
				memRead32(0x9FC43458), memRead32(0x9FC4345C),
				memRead32(0x9FC43460), memRead32(0x9FC43464),
				memRead32(0x9FC43468), memRead32(0x9FC4346C));
		}
	}

	// @@COMPILE_BAD_PC@@ one-shot: dump memory at suspect PC
	if (HWADDR(startpc) >= 0x01F00000u && HWADDR(startpc) <= 0x02100000u)
	{
		static bool s_bad_seen = false;
		if (!s_bad_seen) {
			s_bad_seen = true;
			const u32 w0 = memRead32(startpc);
			const u32 w1 = memRead32(startpc + 4);
			const u32 w2 = memRead32(startpc + 8);
			const u32 w3 = memRead32(startpc + 12);
			const u32 caller_pc = cpuRegs.pc;
			Console.WriteLn("@@COMPILE_BAD_PC@@ startpc=%08x caller_pc=%08x w0=%08x w1=%08x w2=%08x w3=%08x",
				startpc, caller_pc, w0, w1, w2, w3);
		}
	}
	s_pCurBlockEx = recBlocks.Get(HWADDR(startpc));
    // Verify we don't already have an extended block for this PC
	pxAssert(!s_pCurBlockEx || s_pCurBlockEx->startpc != HWADDR(startpc));

    // Create new extended block
	s_pCurBlockEx = recBlocks.New(HWADDR(startpc), (uptr)recPtr);
	pxAssert(s_pCurBlockEx);
    if (!s_pCurBlockEx) return;

    DarwinMisc::g_rec_stage = 2; // Before AutoCodeWrite
    if (DarwinMisc::iPSX2_CRASH_PACK) {
        Console.WriteLn("@@REC_STAGE@@ stage=2 (AutoCodeWrite init) tid=%p wx_write_enabled=%d", 
            pthread_self(), DarwinMisc::g_jit_write_state);
        // Explicitly flush trace if enabled
        if (DarwinMisc::iPSX2_WX_TRACE) {
             Console.WriteLn("@@WX_TOGGLE@@ (rec explicit) tid=%p write=%d", pthread_self(), DarwinMisc::g_jit_write_state);
        }
    }
    HostSys::AutoCodeWrite writer(nullptr, 0); // Flush manually at end
    const u8* recPtrStart = armGetCurrentCodePointer();

    // =========================================================================
    // [iter223] @@BIOS_INTERP_STUB@@ BIOS ROM 部分interpreter化
    // [iter224] disabled化: HLE_LOADEXECPS2 が ExecPS2 を直接インターセプトするためnot needed。
    // BIOS_INTERP_STUB は ~11x 速度低下をcauseため、JIT に戻す。
    // Removal condition: 完全撤去可能（HLE で十分）
    // =========================================================================
#if 0 // [iter224] disabled - HLE handles ExecPS2, BIOS_INTERP_STUB causes 11x slowdown
    {
        const u32 hw = HWADDR(startpc);
        if (hw >= 0x1FC00000u && hw < 0x20000000u)
        {
            static int s_bios_interp_log = 0;
            if (s_bios_interp_log < 20) {
                s_bios_interp_log++;
                Console.WriteLn("@@BIOS_INTERP_STUB@@ n=%d startpc=%08x", s_bios_interp_log, startpc);
            }

            // Emit minimal ARM64 stub:
            //   MOV w0, #startpc_lo
            //   MOVK w0, #startpc_hi, LSL #16
            //   STR w0, [RSTATE_CPU, #offsetof(cpuRegs.pc)]
            //   BL interpExecBiosBlock
            //   ; reload RSTATE registers (callee-saved but interpExecBiosBlock may clobber)
            //   MOV RSTATE_CPU, &g_cpuRegistersPack
            //   MOV RSTATE_x29, &recLUT
            //   MOV RSTATE_PSX, &psxRegs
            //   LDR RFASTMEMBASE, [RSTATE_CPU, #offsetof(vtlbdata.fastmem_base)]
            //   B DispatcherEvent

            // Store startpc → cpuRegs.pc
            armStore(PTR_CPU(cpuRegs.pc), (uint64_t)startpc);

            // Call interpExecBiosBlock
            armEmitCall(reinterpret_cast<const void*>(interpExecBiosBlock));

            // Reload RSTATE registers (may have been clobbered by C call)
            armMoveAddressToReg(RSTATE_x29, &recLUT);
            armMoveAddressToReg(RSTATE_PSX, &psxRegs);
            armMoveAddressToReg(RSTATE_CPU, &g_cpuRegistersPack);
            if (CHECK_FASTMEM) {
                armAsm->Ldr(RFASTMEMBASE, PTR_CPU(vtlbdata.fastmem_base));
            }

            // Jump to DispatcherEvent (event check + next block dispatch)
            armEmitJmp(DispatcherEvent);

            // Finalize block
            s_pCurBlock->SetFnptr((uptr)recPtr);
            s_pCurBlockEx->x86size = static_cast<u32>(armGetCurrentCodePointer() - recPtr);
            Perf::ee.RegisterPC((void*)s_pCurBlockEx->fnptr, s_pCurBlockEx->x86size, s_pCurBlockEx->startpc);
            recPtr = armEndBlock();
            s_pCurBlock = nullptr;
            s_pCurBlockEx = nullptr;
            return;
        }
    }
#endif // [iter224] BIOS_INTERP_STUB disabled

	// [TEMP_DIAG] @@FPU_DIAG@@ JIT FPU flow trace for blocks 0x219760-0x2197c4
	// All blocks in range get FPU dump at entry, all run via JIT (no interpreter fallback)
	// Removal condition: JIT FPU bug in blocks 0x219760/0x2197c4 identified and fixed
	{
		const u32 hw_comp = HWADDR(startpc);
		if (hw_comp >= 0x219760u && hw_comp <= 0x2197c4u)
		{
			// Emit FPU dump call at block entry, then let JIT compile normally
			armStore(PTR_CPU(cpuRegs.pc), (uint64_t)startpc);
			armEmitCall(reinterpret_cast<const void*>(fpuDiagDump));
			// RSTATE registers are callee-saved (x25,x27,x28,x29), safe across call
			// Fall through to normal JIT compilation for ALL blocks
		}
	}

		if (HWADDR(startpc) == EELOAD_START)
	{
		// The EELOAD _start function is the same across all BIOS versions
		const u32 mainjump = memRead32(EELOAD_START + 0x9c);
		if (mainjump >> 26 == 3) // JAL
			g_eeloadMain = ((EELOAD_START + 0xa0) & 0xf0000000U) | (mainjump << 2 & 0x0fffffffU);

		// [iter97] @@EELOAD_820E8_STATIC@@ – EELOAD_START compile時に 0x820E8 を静的ダンプ
		// (0x820E8 は startup 時に事前compileされるため EELOAD_START フックで取得)
		// Removal condition: 0x820E8 functionのbehavior（ExecPS2 callパス）確定時
		if (eeMem) {
			static bool s_820e8_static_dumped = false;
			if (!s_820e8_static_dumped) {
				s_820e8_static_dumped = true;
				const u32* p = reinterpret_cast<const u32*>(eeMem->Main + 0x820E8u);
				Console.WriteLn("@@EELOAD_820E8_STATIC@@ +00: %08x %08x %08x %08x %08x %08x %08x %08x",
					p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
				Console.WriteLn("@@EELOAD_820E8_STATIC@@ +20: %08x %08x %08x %08x %08x %08x %08x %08x",
					p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
				// Also dump 0x82E90 (target of 0x82F30 first-call jump)
				const u32* q = reinterpret_cast<const u32*>(eeMem->Main + 0x82E90u);
				Console.WriteLn("@@EELOAD_82E90_STATIC@@ +00: %08x %08x %08x %08x %08x %08x %08x %08x",
					q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
			}
		}
	}

	if (g_eeloadMain && HWADDR(startpc) == HWADDR(g_eeloadMain))
	{
//		xFastCall((void*)eeloadHook);
        armEmitCall(reinterpret_cast<void*>(eeloadHook));
		if (VMManager::Internal::IsFastBootInProgress())
		{
			// There are four known versions of EELOAD, identifiable by the location of the 'jal' to the EELOAD function which
			// calls ExecPS2(). The function itself is at the same address in all BIOSs after v1.00-v1.10.
			const u32 typeAexecjump = memRead32(EELOAD_START + 0x470); // v1.00, v1.01?, v1.10?
			const u32 typeBexecjump = memRead32(EELOAD_START + 0x5B0); // v1.20, v1.50, v1.60 (3000x models)
			const u32 typeCexecjump = memRead32(EELOAD_START + 0x618); // v1.60 (3900x models)
			const u32 typeDexecjump = memRead32(EELOAD_START + 0x600); // v1.70, v1.90, v2.00, v2.20, v2.30
			if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) || (typeDexecjump >> 26 == 3)) // JAL to 0x822B8
				g_eeloadExec = EELOAD_START + 0x2B8;
			else if (typeAexecjump >> 26 == 3) // JAL to 0x82170
				g_eeloadExec = EELOAD_START + 0x170;
			else // There might be other types of EELOAD, because these models' BIOSs have not been examined: 18000, 3500x, 3700x, 5500x, and 7900x. However, all BIOS versions have been examined except for v1.01 and v1.10.
				Console.WriteLn("recRecompile: Could not enable launch arguments for fast boot mode; unidentified BIOS version! Please report this to the PCSX2 developers.");
		}
	}

	if (g_eeloadExec && HWADDR(startpc) == HWADDR(g_eeloadExec)) {
//        xFastCall((void *) eeloadHook2);
        armEmitCall(reinterpret_cast<void*>(eeloadHook2));
    }

	// [iter90] @@JAL_83860@@ – 0x83860 compile時: 静的コードダンプ + 実行時引数probe emit
	if (HWADDR(startpc) == 0x83860u && eeMem) {
		static bool s_code_dumped = false;
		if (!s_code_dumped) {
			s_code_dumped = true;
			const u32* p = reinterpret_cast<const u32*>(eeMem->Main + 0x83860);
			Console.WriteLn("@@JAL_83860_CODE@@ %08x %08x %08x %08x %08x %08x %08x %08x",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
		}
		armEmitCall(reinterpret_cast<void*>(probe_83860_entry));
	}
	// [iter90] @@ERET_8208C@@ – ERET 直前の v0/v1/EPC をキャプチャ (JAL 0x83860 戻り値)
	if (HWADDR(startpc) == 0x8208cu) {
		armEmitCall(reinterpret_cast<void*>(probe_8208c_eret));
	}
	// [iter96] @@EELOAD_82F30@@ / [iter97] @@EELOAD_820E8@@ – EELOAD 主要分岐functionのコードダンプ
	// Removal condition: EELOAD main から ExecPS2 への到達パス確定時
	if (HWADDR(startpc) == 0x82F30u && eeMem) {
		static bool s_82f30_dumped = false;
		if (!s_82f30_dumped) {
			s_82f30_dumped = true;
			const u32* p = reinterpret_cast<const u32*>(eeMem->Main + 0x82F30u);
			Console.WriteLn("@@EELOAD_82F30_CODE@@ +00: %08x %08x %08x %08x %08x %08x %08x %08x",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
		}
		armEmitCall(reinterpret_cast<void*>(probe_eeload_slti));
	}
	// [iter97] @@EELOAD_820E8_CODE@@ – 0x820E8 (EELOAD main の核心ロジック) 先頭 16 命令ダンプ
	// Removal condition: ExecPS2 callパス (rom0:OSDSYS) 確定またはfailcause確定時
	if (HWADDR(startpc) == 0x820E8u && eeMem) {
		static bool s_820e8_dumped = false;
		if (!s_820e8_dumped) {
			s_820e8_dumped = true;
			const u32* p = reinterpret_cast<const u32*>(eeMem->Main + 0x820E8u);
			Console.WriteLn("@@EELOAD_820E8_CODE@@ +00: %08x %08x %08x %08x %08x %08x %08x %08x",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
			Console.WriteLn("@@EELOAD_820E8_CODE@@ +20: %08x %08x %08x %08x %08x %08x %08x %08x",
				p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
		}
	}
	// [iter95] @@EELOAD_MAIN_CODE@@ – 0x82388 (EELOAD main) 先頭 16 命令を静的ダンプ
	// Removal condition: ExecPS2 未到達cause（condition分岐パス）確定時
	if (HWADDR(startpc) == HWADDR(g_eeloadMain) && g_eeloadMain && eeMem) {
		static bool s_main_dumped = false;
		if (!s_main_dumped) {
			s_main_dumped = true;
			const u32* p = reinterpret_cast<const u32*>(eeMem->Main + HWADDR(g_eeloadMain));
			Console.WriteLn("@@EELOAD_MAIN_CODE@@ +00: %08x %08x %08x %08x %08x %08x %08x %08x",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
			Console.WriteLn("@@EELOAD_MAIN_CODE@@ +20: %08x %08x %08x %08x %08x %08x %08x %08x",
				p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
		}
	}

	g_branch = 0;

	// reset recomp state variables
	s_nBlockCycles = 0;
	s_nBlockInterlocked = false;
	pc = startpc;
	g_cpuHasConstReg = g_cpuFlushedConstReg = 1;
	pxAssert(g_cpuConstRegs[0].UD[0] == 0);

	_initX86regs();
    
    // VERIFICATION: Check allocatable registers on first run
    static bool s_verified_jit_regs = false;
    if (!s_verified_jit_regs) {
        s_verified_jit_regs = true;
        Console.WriteLn("@@JIT_VERIFY@@ Checking Allocatable Registers (Platform: iOS/ARM64)");
        for (int r = 0; r < 32; r++) {
            bool allocatable = _isAllocatableX86reg(r);
            // Log status of lower registers (x0-x18) specifically as they are the target of exclusion
            if (r <= 32) {
                Console.WriteLn("@@JIT_VERIFY@@ Reg x%d: %s", r, allocatable ? "ALLOCATABLE" : "EXCLUDED");
            }
        }
    }
	_initXMMregs();

#ifdef TRACE_BLOCKS
	xFastCall((void*)PreBlockCheck, pc);
#endif

	if (EmuConfig.Gamefixes.GoemonTlbHack)
	{
		if (pc == 0x33ad48 || pc == 0x35060c)
		{
			// 0x33ad48 and 0x35060c are the return address of the function (0x356250) that populate the TLB cache
//			xFastCall((void*)GoemonPreloadTlb);
            armEmitCall(reinterpret_cast<void*>(GoemonPreloadTlb));
		}
		else if (pc == 0x3563b8)
		{
			// Game will unmap some virtual addresses. If a constant address were hardcoded in the block, we would be in a bad situation.
			eeRecNeedsReset = true;
			// 0x3563b8 is the start address of the function that invalidate entry in TLB cache
//			xFastCall((void*)GoemonUnloadTlb, ptr32[&cpuRegs.GPR.n.a0.UL[0]]);
            armLoad(EAX, PTR_CPU(cpuRegs.GPR.n.a0.UL[0]));
            armEmitCall(reinterpret_cast<void*>(GoemonUnloadTlb));
		}
	}

	// go until the next branch
	i = startpc;
    
    // [DEBUG-VERIFY] Log BIOS Boot Block Disassembly
    if (startpc == 0xbfc00000) {
        Console.WriteLn("@@JIT_DISASM@@ Block 0xbfc00000 Start");
        u32 temp_pc = startpc;
        while (true) {
            u32 op = memRead32(temp_pc);
            Console.WriteLn("@@JIT_DISASM@@ %08x: %08x", temp_pc, op);
            
            // Basic branch detection to break loop (approximation)
            u32 opcode = op >> 26;
            bool isBranch = (opcode == 2 || opcode == 3 || opcode == 4 || opcode == 5 || opcode == 1 || opcode == 20); // J, JAL, BEQ, BNE, Bxx, BEQL
            
            // Check for JR (Special 0 / 8) or JALR (Special 0 / 9)
            if (opcode == 0) {
                 u32 funct = op & 0x3f;
                 if (funct == 8 || funct == 9) isBranch = true;
            }
            // Check for Branch Likely
            if (opcode >= 20 && opcode <= 23) isBranch = true; // BEQL..BGTZL
            
            temp_pc += 4;
            if (isBranch) {
                // One delay slot
                u32 delay_op = memRead32(temp_pc);
                Console.WriteLn("@@JIT_DISASM@@ %08x: %08x (DelaySlot)", temp_pc, delay_op);
                break;
            }
            if ((temp_pc - startpc) > 1024) break; // Safety break
        }
        Console.WriteLn("@@JIT_DISASM@@ Block 0xbfc00000 End");
    }
	s_nEndBlock = 0xffffffff;
	s_branchTo = -1;

	// Timeout loop speedhack.
	// God of War 2 and other games (e.g. NFS series) have these timeout loops which just spin for a few thousand
	// iterations, usually after kicking something which results in an IRQ, but instead of cancelling the loop,
	// they just let it finish anyway. Such loops look like:
	//
	//   00186D6C addiu  v0,v0, -0x1
	//   00186D70 nop
	//   00186D74 nop
	//   00186D78 nop
	//   00186D7C nop
	//   00186D80 bne    v0, zero, ->$0x00186D6C
	//   00186D84 nop
	//
	// Skipping them entirely seems to have no negative effects, but we skip cycles based on the incoming value
	// if the register being decremented, which appears to vary. So far I haven't seen any which increment instead
	// of decrementing, so we'll limit the test to that to be safe.
	//
	s32 timeout_reg = -1;
	bool is_timeout_loop = true;

	// compile breakpoints as individual blocks
	const int n1 = isBreakpointNeeded(i);
	const int n2 = isMemcheckNeeded(i);
	const int n = std::max<int>(n1, n2);
	if (n != 0)
	{
		s_nEndBlock = i + (n << 2); // n * 4
		goto StartRecomp;
	}

	while (1)
	{
		BASEBLOCK* pblock = PC_GETBLOCK(i);

		// stop before breakpoints
		if (isBreakpointNeeded(i) != 0 || isMemcheckNeeded(i) != 0)
		{
			s_nEndBlock = i;
			break;
		}

		if (i != startpc) // Block size truncation checks.
		{
			if ((i & 0xffc) == 0x0) // breaks blocks at 4k page boundaries
			{
				willbranch3 = 1;
				s_nEndBlock = i;

#ifdef PCSX2_DEVBUILD
				eeRecPerfLog.Write("Pagesplit @ %08X : size=%d insts", startpc, (i - startpc) / 4);
#endif
				break;
			}

			if (pblock->GetFnptr() != (uptr)JITCompile)
			{
				willbranch3 = 1;
				s_nEndBlock = i;
				break;
			}
		}

		//HUH ? PSM ? whut ? THIS IS VIRTUAL ACCESS GOD DAMMIT
		cpuRegs.code = *(int*)PSM(i);

		if (is_timeout_loop)
		{
			if ((cpuRegs.code >> 26) == 8 || (cpuRegs.code >> 26) == 9)
			{
				// addi/addiu
				if (timeout_reg >= 0 || _Rs_ != _Rt_ || _Imm_ >= 0)
					is_timeout_loop = false;
				else
					timeout_reg = _Rs_;
			}
			else if ((cpuRegs.code >> 26) == 5)
			{
				// bne
				if (timeout_reg != static_cast<s32>(_Rs_) || _Rt_ != 0 || memRead32(i + 4) != 0)
					is_timeout_loop = false;
			}
			else if (cpuRegs.code != 0)
			{
				is_timeout_loop = false;
			}
		}

		switch (cpuRegs.code >> 26)
		{
			case 0: // special
				if (_Funct_ == 8 || _Funct_ == 9) // JR, JALR
				{
					s_nEndBlock = i + 8;
					goto StartRecomp;
				}
				else if (_Funct_ == 12 || _Funct_ == 13) // SYSCALL, BREAK
				{
					s_nEndBlock = i + 4; // No delay slot.
					goto StartRecomp;
				}
				break;

			case 1: // regimm

				if (_Rt_ < 4 || (_Rt_ >= 16 && _Rt_ < 20))
				{
					// branches
					s_branchTo = (_Imm_ << 2) + i + 4; // _Imm_ * 4
					if (s_branchTo > startpc && s_branchTo < i)
						s_nEndBlock = s_branchTo;
					else
						s_nEndBlock = i + 8;

					goto StartRecomp;
				}
				break;

			case 2: // J
			case 3: // JAL
				s_branchTo = (_InstrucTarget_ << 2) | ((i + 4) & 0xf0000000);
				s_nEndBlock = i + 8;
				goto StartRecomp;

			// branches
			case 4:
			case 5:
			case 6:
			case 7:
			case 20:
			case 21:
			case 22:
			case 23:
				s_branchTo = (_Imm_ << 2) + i + 4; // _Imm_ * 4
				if (s_branchTo > startpc && s_branchTo < i)
					s_nEndBlock = s_branchTo;
				else
					s_nEndBlock = i + 8;

				goto StartRecomp;

			case 16: // cp0
				if (_Rs_ == 16)
				{
					if (_Funct_ == 24) // eret
					{
						s_nEndBlock = i + 4;
						goto StartRecomp;
					}
				}
				// Fall through!
				// COP0's branch opcodes line up with COP1 and COP2's

			case 17: // cp1
			case 18: // cp2
				if (_Rs_ == 8)
				{
					// BC1F, BC1T, BC1FL, BC1TL
					// BC2F, BC2T, BC2FL, BC2TL
					s_branchTo = (_Imm_ << 2) + i + 4; // _Imm_ * 4
					if (s_branchTo > startpc && s_branchTo < i)
						s_nEndBlock = s_branchTo;
					else
						s_nEndBlock = i + 8;

					goto StartRecomp;
				}
				break;
		}

		i += 4;
	}

StartRecomp:
    // DEBUG: Final Block Limits (= env iPSX2_DEBUG_VERBOSE=1 で復活、 release default suppress)
    if (iPSX2_IsDebugVerbose())
    {
        static int log_lim = 0;
        if (log_lim < 500) { // Log first 500 compilations
             Console.WriteLn("DEBUG: StartRecomp PC=%08x END=%08x", startpc, s_nEndBlock);
             log_lim++;
        }
    }
    // [TEMP_DIAG] Dump MIPS opcodes for crash block 9fc42550 to identify PC=0/RA=0 cause.
    // Removal condition: @@MIPS42550@@ ログから命令列が確定し、root causeが特定された時点。
    if (startpc == 0x9fc42550) {
        for (u32 _dp = startpc; _dp < s_nEndBlock; _dp += 4) {
            const u32* _pm = (const u32*)PSM(_dp);
            if (_pm) Console.WriteLn("[TEMP_DIAG] @@MIPS42550@@ pc=%08x op=%08x", _dp, *_pm);
        }
    }

	// The idea here is that as long as a loop doesn't write to a register it's already read
	// (excepting registers initialised with constants or memory loads) or use any instructions
	// which alter the machine state apart from registers, it will do the same thing on every
	// iteration.
	s_nBlockFF = false;
	if (s_branchTo == startpc)
	{
		s_nBlockFF = true;

		u32 reads = 0, loads = 1;

		for (i = startpc; i < s_nEndBlock; i += 4)
		{
			if (i == s_nEndBlock - 8)
				continue;
			cpuRegs.code = *(u32*)PSM(i);
            
            // [TEMP_DIAG][REMOVE_AFTER=BIOS_CORRUPTION_AUDIT_V1] BIOS Analysis Phase patches
            // Hardcoded BIOS PC-based opcode rewrites - gated OFF by default
#if iPSX2_ENABLE_BIOS_OPCODE_PATCHES
	            if (!iPSX2_IsSafeOnlyEnabled())
	            {
	            // WARNING: These patches mutate cpuRegs.code based on hardcoded BIOS PCs
	            if (i == 0xbfc02678) cpuRegs.code = 0x2509000c;
	            if (i == 0xbfc02680) cpuRegs.code = 0x8fa20000;
            if (i == 0xbfc02684) cpuRegs.code = 0x8522fffc;
            if (i == 0xbfc0268c) cpuRegs.code = 0x1440fff5;
            
            // Break infinite loop at bfc02454
            if (i == 0xbfc02434) {
	                 DevCon.WriteLn("@@BIOS_PATCH@@ Skipping instruction at %08x (Compile)", i);
	                 cpuRegs.code = 0; // NOP
	            }
	            }
#endif // iPSX2_ENABLE_BIOS_OPCODE_PATCHES
			// nop
			if (cpuRegs.code == 0)
				continue;
			// cache, sync
			else if (_Opcode_ == 057 || (_Opcode_ == 0 && _Funct_ == 017))
				continue;
			// imm arithmetic
			else if ((_Opcode_ & 070) == 010 || (_Opcode_ & 076) == 030)
			{
				if (loads & 1 << _Rs_)
				{
					loads |= 1 << _Rt_;
					continue;
				}
				else
					reads |= 1 << _Rs_;
				if (reads & 1 << _Rt_)
				{
					s_nBlockFF = false;
					break;
				}
			}
			// common register arithmetic instructions
			else if (_Opcode_ == 0 && (_Funct_ & 060) == 040 && (_Funct_ & 076) != 050)
			{
				if (loads & 1 << _Rs_ && loads & 1 << _Rt_)
				{
					loads |= 1 << _Rd_;
					continue;
				}
				else
					reads |= 1 << _Rs_ | 1 << _Rt_;
				if (reads & 1 << _Rd_)
				{
					s_nBlockFF = false;
					break;
				}
			}
			// loads
			else if ((_Opcode_ & 070) == 040 || (_Opcode_ & 076) == 032 || _Opcode_ == 067)
			{
				if (loads & 1 << _Rs_)
				{
					loads |= 1 << _Rt_;
					continue;
				}
				else
					reads |= 1 << _Rs_;
				if (reads & 1 << _Rt_)
				{
					s_nBlockFF = false;
					break;
				}
			}
			// mfc*, cfc*
			else if ((_Opcode_ & 074) == 020 && _Rs_ < 4)
			{
				loads |= 1 << _Rt_;
			}
			else
			{
				s_nBlockFF = false;
				break;
			}
		}
	}
	else
	{
		is_timeout_loop = false;
	}

	// rec info //
	bool has_cop2_instructions = false;
	{
        u32 block_offset = (s_nEndBlock - startpc) >> 2; // (s_nEndBlock - startpc) / 4
		if (s_nInstCacheSize < block_offset + 1)
		{
			const u32 required_size = block_offset + 10;
			const u32 new_size = std::max(required_size, s_nInstCacheSize << 1); // s_nInstCacheSize * 2
			
			EEINST* new_cache = (EEINST*)malloc(sizeof(EEINST) * new_size);
			if (!new_cache)
				pxFailRel("Failed to allocate R5900 InstCache array");
			
			if (s_pInstCache && s_nInstCacheSize > 0)
			{
				memcpy(new_cache, s_pInstCache, sizeof(EEINST) * s_nInstCacheSize);
			}
			
			free(s_pInstCache);
			s_pInstCache = new_cache;
			s_nInstCacheSize = new_size;
		}

		EEINST* pcur = s_pInstCache + block_offset;
		_recClearInst(pcur);
		pcur->info = 0;

		for (i = s_nEndBlock; i > startpc; i -= 4)
		{
			cpuRegs.code = *(int*)PSM(i - 4);
			pcur[-1] = pcur[0];
			recBackpropBSC(cpuRegs.code, pcur - 1, pcur);
			pcur--;

			has_cop2_instructions |= (_Opcode_ == 022 || _Opcode_ == 066 || _Opcode_ == 076);
		}
	}

	// eventually we'll want to have a vector of passes or something.
	if (has_cop2_instructions)
	{
		COP2MicroFinishPass().Run(startpc, s_nEndBlock, s_pInstCache + 1);

		if (EmuConfig.Speedhacks.vuFlagHack)
			COP2FlagHackPass().Run(startpc, s_nEndBlock, s_pInstCache + 1);
	}

#ifdef DUMP_BLOCKS
	ZydisDecoder disas_decoder;
	ZydisDecoderInit(&disas_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_ADDRESS_WIDTH_64);

	ZydisFormatter disas_formatter;
	ZydisFormatterInit(&disas_formatter, ZYDIS_FORMATTER_STYLE_INTEL);

	s_old_print_address = (ZydisFormatterFunc)&ZydisFormatterPrintAddressAbsolute;
	ZydisFormatterSetHook(&disas_formatter, ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_ABS, (const void**)&s_old_print_address);

	ZydisDecodedInstruction disas_instruction;
#if 0
	const bool dump_block = (startpc == 0x00000000);
#elif 1
	const bool dump_block = true;
#else
	const bool dump_block = false;
#endif
#endif

	// Detect and handle self-modified code
	memory_protect_recompiled_code(startpc, (s_nEndBlock - startpc) >> 2);

	// Skip Recompilation if sceMpegIsEnd Pattern detected
	const bool doRecompilation = !skipMPEG_By_Pattern(startpc) && !recSkipTimeoutLoop(timeout_reg, is_timeout_loop);

	if (doRecompilation)
	{
		// Finally: Generate x86 recompiled code!
		g_pCurInstInfo = s_pInstCache;

		// [TEMP_DIAG][REMOVE_AFTER=LF_ENTRY_CAPTURE_V2] One-shot runtime capture at LoadFile prologue block entry.
		// Capture the actual a0/ra seen by JIT when block 0xBFC02640 executes.
			if (startpc == 0xBFC02640)
			{
				a64::Label skip_lf_entry_cap;
				armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_entry_seen));
			armAsm->Ldr(a64::w14, a64::MemOperand(a64::x15));
			armAsm->Cbnz(a64::w14, &skip_lf_entry_cap);

			armAsm->Mov(a64::w14, 1);
			armAsm->Str(a64::w14, a64::MemOperand(a64::x15)); // g_lf_entry_seen = 1

			armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_entry_a0));
			armAsm->Ldr(a64::w14, PTR_CPU(cpuRegs.GPR.r[4].UL[0])); // a0
			armAsm->Str(a64::w14, a64::MemOperand(a64::x15));

				armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_entry_ra));
				armAsm->Ldr(a64::w14, PTR_CPU(cpuRegs.GPR.r[31].UL[0])); // ra
				armAsm->Str(a64::w14, a64::MemOperand(a64::x15));

				armBind(&skip_lf_entry_cap);
			}

	// [TEMP_DIAG] @@DSLL_LOOP_PROBE@@ — log registers at DSLL loop hotspot
	// Removal condition: ギャップcauseafter identified
	if (HWADDR(startpc) == 0x002659f0u) {
		iFlushCall(FLUSH_EVERYTHING);
		extern void dsll_loop_probe();
		armEmitCall((void*)dsll_loop_probe);
	}
	// [iter681] @@SIF_690C_RUNTIME@@ — inject runtime register dump at key blocks
	if (HWADDR(startpc) == 0x0000690Cu) {
		iFlushCall(FLUSH_EVERYTHING);
		extern void sif690c_runtime_log();
		armEmitCall((void*)sif690c_runtime_log);
	}
	// [iter681] @@SIF_6668_RUNTIME@@ — capture state at function 0x6668 entry
	if (HWADDR(startpc) == 0x00006668u) {
		iFlushCall(FLUSH_EVERYTHING);
		extern void sif6668_runtime_log();
		armEmitCall((void*)sif6668_runtime_log);
	}
	// [iter681] @@SIF_66D0_RUNTIME@@ — BNE taken path (a1 != a0)
	if (HWADDR(startpc) == 0x000066D0u) {
		iFlushCall(FLUSH_EVERYTHING);
		extern void sif66D0_runtime_log();
		armEmitCall((void*)sif66D0_runtime_log);
	}
	// [iter681] @@SIF_66E8_RUNTIME@@ — capture state at function 0x6668 return
	if (HWADDR(startpc) == 0x000066E8u) {
		iFlushCall(FLUSH_EVERYTHING);
		extern void sif66E8_runtime_log();
		armEmitCall((void*)sif66E8_runtime_log);
	}
	// [iter681] @@SIF_6810_RUNTIME@@ — capture v0 (function 0x6668 return value) at JAL return point
	if (HWADDR(startpc) == 0x00006810u) {
		iFlushCall(FLUSH_EVERYTHING);
		extern void sif6810_runtime_log();
		armEmitCall((void*)sif6810_runtime_log);
	}

	while (!g_branch && pc < s_nEndBlock)
	{
        // [TEMP_DIAG][REMOVE_AFTER=LF_PRECALL_CAPTURE_V1] One-shot capture at LoadFile callsite JAL (0xBFC023D0).
        if (pc == 0xBFC023D0)
        {
            a64::Label skip_lf_precall_cap;
            armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_precall_seen));
            armAsm->Ldr(a64::w14, a64::MemOperand(a64::x15));
            armAsm->Cbnz(a64::w14, &skip_lf_precall_cap);

            armAsm->Mov(a64::w14, 1);
            armAsm->Str(a64::w14, a64::MemOperand(a64::x15)); // g_lf_precall_seen = 1

            u32 precall_flags = 0;

            armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_precall_a0));
            const int reg_a0 = _checkX86reg(X86TYPE_GPR, 4, MODE_READ);
            if (GPR_IS_CONST1(4))
            {
                armAsm->Mov(a64::w14, (u32)g_cpuConstRegs[4].UL[0]);
                precall_flags |= (1u << 1); // a0 const
            }
            else if (reg_a0 >= 0)
            {
                armAsm->Mov(a64::w14, a64::WRegister(HostGprPhys(reg_a0)));
                precall_flags |= (1u << 0); // a0 hostreg
            }
            else
            {
                armAsm->Ldr(a64::w14, PTR_CPU(cpuRegs.GPR.r[4].UL[0])); // a0
            }
            armAsm->Str(a64::w14, a64::MemOperand(a64::x15));

            armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_precall_v0));
            const int reg_v0 = _checkX86reg(X86TYPE_GPR, 2, MODE_READ);
            if (GPR_IS_CONST1(2))
            {
                armAsm->Mov(a64::w14, (u32)g_cpuConstRegs[2].UL[0]);
                precall_flags |= (1u << 3); // v0 const
            }
            else if (reg_v0 >= 0)
            {
                armAsm->Mov(a64::w14, a64::WRegister(HostGprPhys(reg_v0)));
                precall_flags |= (1u << 2); // v0 hostreg
            }
            else
            {
                armAsm->Ldr(a64::w14, PTR_CPU(cpuRegs.GPR.r[2].UL[0])); // v0
            }
            armAsm->Str(a64::w14, a64::MemOperand(a64::x15));

            armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_precall_t0));
            armAsm->Ldr(a64::w14, PTR_CPU(cpuRegs.GPR.r[8].UL[0])); // t0
            armAsm->Str(a64::w14, a64::MemOperand(a64::x15));

            armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_precall_ra));
            armAsm->Ldr(a64::w14, PTR_CPU(cpuRegs.GPR.r[31].UL[0])); // ra
            armAsm->Str(a64::w14, a64::MemOperand(a64::x15));

            armMoveAddressToReg(a64::x15, const_cast<u32*>(&g_lf_precall_flags));
            armAsm->Mov(a64::w14, precall_flags);
            armAsm->Str(a64::w14, a64::MemOperand(a64::x15));

            armBind(&skip_lf_precall_cap);
        }

        // [iPSX2] Old probe removed
        g_current_diag_pc = pc;
		// The recompiler needs to know if a branch instruction is in a delay sloth
            // DEBUG: Hang analysis removed - was causing crash
            // DEBUG: Status tracing also removed for stability
            // (MTC0/ERET hooks commented out)

		    // Console.WriteLn("DEBUG: JIT Compiling PC=%08x Code=%08x", pc, *(u32*)PSM(pc));
#ifdef DUMP_BLOCKS
			if (dump_block)
			{
				std::string disasm;
				disR5900Fasm(disasm, *(u32*)PSM(pc), pc, false);
				fprintf(stderr, "Compiling %08X %s\n", pc, disasm.c_str());

				const u8* instStart = armGetCurrentCodePointer();
				recompileNextInstruction(false, false);

				const u8* instPtr = instStart;
				// (ARM64 disassembly would go here)
			}
			else
			{
                // Force full flush in narrow BIOS setup windows where stale host-reg state
                // was observed to desync LoadFile arguments (a0/t0) across JAL boundaries.
                if ((pc >= 0xbfc02070 && pc <= 0xbfc020a0) ||
                    (pc >= 0xbfc023b8 && pc <= 0xbfc02670)) {
                    iFlushCall(FLUSH_EVERYTHING);
                }
                
                // Phase 8.5: JIT Entry Log (File)
                // [DELETED] Phase 8.5: JIT Entry Log converted/removed.

				recompileNextInstruction(false, false);

				// [TEMP_DIAG] @@FPU_STEP@@ Per-instruction FPU dump for block 0x219760
				// pc-4 = just-compiled instruction. Dump after each key FPU op:
				// 0x219770: MUL f0,f2,f0  0x219774: SUB f0,f20,f0
				// 0x219778: MUL f0,f0,f1  0x219784: DIV f0,f0,f20
				// 0x219788: ADD f2,f0,f21  0x21978c: C.LT f2,f21
				// Removal condition: JIT FPU bug identified
				{
					const u32 hw_pc = HWADDR(pc - 4);
					if (hw_pc == 0x219770u || hw_pc == 0x219774u ||
						hw_pc == 0x219778u || hw_pc == 0x219784u ||
						hw_pc == 0x219788u || hw_pc == 0x21978cu)
					{
						iFlushCall(FLUSH_EVERYTHING);
						armStore(PTR_CPU(cpuRegs.pc), (uint64_t)(pc - 4));
						armEmitCall(reinterpret_cast<const void*>(fpuDiagDump));
						armMoveAddressToReg(RSTATE_x29, &recLUT);
						armMoveAddressToReg(RSTATE_PSX, &psxRegs);
						armMoveAddressToReg(RSTATE_CPU, &g_cpuRegistersPack);
						if (CHECK_FASTMEM) {
							armAsm->Ldr(RFASTMEMBASE, PTR_CPU(vtlbdata.fastmem_base));
						}
					}
				}
			}
#else
			recompileNextInstruction(false, false); // For the love of recursion, batman!

			// [TEMP_DIAG] @@FPU_STEP@@ Per-instruction FPU dump for block 0x219760
			// pc-4 = just-compiled instruction. Dump after each key FPU op.
			// Removal condition: JIT FPU bug identified
			{
				const u32 hw_pc = HWADDR(pc - 4);
				if (hw_pc == 0x219764u || hw_pc == 0x21976cu ||
					hw_pc == 0x219770u || hw_pc == 0x219774u)
				{
					iFlushCall(FLUSH_EVERYTHING);
					armStore(PTR_CPU(cpuRegs.pc), (uint64_t)(pc - 4));
					armEmitCall(reinterpret_cast<const void*>(fpuDiagDump));
					armMoveAddressToReg(RSTATE_x29, &recLUT);
					armMoveAddressToReg(RSTATE_PSX, &psxRegs);
					armMoveAddressToReg(RSTATE_CPU, &g_cpuRegistersPack);
					if (CHECK_FASTMEM) {
						armAsm->Ldr(RFASTMEMBASE, PTR_CPU(vtlbdata.fastmem_base));
					}
				}
			}
#endif
		}
	}

	pxAssert((pc - startpc) >> 2 <= 0xffff);
	s_pCurBlockEx->size = (pc - startpc) >> 2;

	if (HWADDR(pc) <= Ps2MemSize::ExposedRam)
	{
		BASEBLOCKEX* oldBlock;
		int ii = recBlocks.LastIndex(HWADDR(pc) - 4);
		while ((oldBlock = recBlocks[ii--]))
		{
			if (oldBlock == s_pCurBlockEx)
				continue;
			if (oldBlock->startpc >= HWADDR(pc))
				continue;
			if ((oldBlock->startpc + (oldBlock->size << 2)) <= HWADDR(startpc)) // oldBlock->size * 4
				break;

			if (memcmp(&recRAMCopy[oldBlock->startpc], PSM(oldBlock->startpc), oldBlock->size << 2)) // [FIX] removed >>2 index compression to prevent overlap corruption
			{
				s_overlap_clear_count.fetch_add(1, std::memory_order_relaxed); // [TEMP_DIAG]
				{
					uint32_t pg = oldBlock->startpc >> 12;
					if (pg < 2048) s_overlap_page_hist[pg]++;
				}
				recClear(startpc, (pc - startpc) >> 2); // (pc - startpc) / 4
				s_pCurBlockEx = recBlocks.Get(HWADDR(startpc));
				pxAssert(s_pCurBlockEx->startpc == HWADDR(startpc));
				break;
			}
		}

		memcpy(&recRAMCopy[HWADDR(startpc)], PSM(startpc), pc - startpc); // [FIX] removed >>2 index compression
	}

	s_pCurBlock->SetFnptr((uptr)recPtr);

#ifdef __APPLE__
    // [iPSX2] Record JIT Block for Crash Analysis
    DarwinMisc::RecordJitBlock(startpc, (void*)recPtr, 0);
#endif

#ifdef ENABLE_TRACE
    // @@LUT_WRITE@@ diagnostic: show the slot address where we're writing
    {
        static int s_lut_write_count = 0;
        s_lut_write_count++;
        uptr wrote_value = s_pCurBlock->GetFnptr();
        uptr slot_addr = (uptr)&s_pCurBlock->m_pFnptr;  // The actual memory address we wrote to
        bool is_host_ptr = (wrote_value >= 0x100000000ULL); // Host pointers on 64-bit are > 4GB
        bool is_guest_pc = (wrote_value >= 0xbfc00000 && wrote_value <= 0xc0000000); // BIOS range
        if (s_lut_write_count <= 100 || is_guest_pc) {
            Console.WriteLn("@@LUT_WRITE@@ pc=%08x slot=%p wrote=%p recPtr=%p is_host=%d%s",
                startpc, (void*)slot_addr, (void*)wrote_value, recPtr, is_host_ptr,
                is_guest_pc ? " BAD!" : "");
        }

        // [TEMP_DIAG] guest 0x00036000生成時に block先頭とJIT arena先頭を1回だけ記録し、
        // host_pc=jbase+0x50 の SIGSEGV がどの命令列由来かを log だけで特定する。
        // Removal condition: @@JIT_BLOCK36000@@ / @@JIT_ARENA_HEAD@@ でcause命令列が確定した時点。
        static int s_jit36000_cfg = -1;
        static int s_jit36000_dumped = 0;
        if (s_jit36000_cfg < 0)
        {
            s_jit36000_cfg = (std::getenv("iPSX2_JIT_BLOCK36000_DUMP") != nullptr) ? 1 : 0;
            Console.WriteLn("@@CFG@@ iPSX2_JIT_BLOCK36000_DUMP=%d", s_jit36000_cfg);
        }
        if (s_jit36000_cfg && !s_jit36000_dumped && startpc == 0x00036000)
        {
            s_jit36000_dumped = 1;
            const u32* const block_words = reinterpret_cast<const u32*>(recPtr);
            const u32* const arena_words = reinterpret_cast<const u32*>(SysMemory::GetEERec());
            const u32* const arena_words40 = arena_words + 16; // +0x40
            Console.WriteLn("[TEMP_DIAG] @@JIT_BLOCK36000@@ recPtr=%p w0=%08x w1=%08x w2=%08x w3=%08x w4=%08x w5=%08x w6=%08x w7=%08x",
                recPtr, block_words[0], block_words[1], block_words[2], block_words[3], block_words[4], block_words[5],
                block_words[6], block_words[7]);
            Console.WriteLn("[TEMP_DIAG] @@JIT_ARENA_HEAD@@ base=%p w0=%08x w1=%08x w2=%08x w3=%08x w4=%08x w5=%08x w6=%08x w7=%08x",
                SysMemory::GetEERec(), arena_words[0], arena_words[1], arena_words[2], arena_words[3], arena_words[4],
                arena_words[5], arena_words[6], arena_words[7]);
            Console.WriteLn("[TEMP_DIAG] @@JIT_ARENA_HEAD2@@ base=%p off40=%08x off44=%08x off48=%08x off4c=%08x off50=%08x off54=%08x off58=%08x off5c=%08x off60=%08x off64=%08x off68=%08x off6c=%08x off70=%08x off74=%08x off78=%08x off7c=%08x",
                SysMemory::GetEERec(), arena_words40[0], arena_words40[1], arena_words40[2], arena_words40[3], arena_words40[4],
                arena_words40[5], arena_words40[6], arena_words40[7], arena_words40[8], arena_words40[9], arena_words40[10],
                arena_words40[11], arena_words40[12], arena_words40[13], arena_words40[14], arena_words40[15]);
        }
    }
#endif

	if (!(pc & 0x10000000))
		maxrecmem = std::max((pc & ~0xa0000000), maxrecmem);

	if (g_branch == 2)
	{
		// Branch type 2 - This is how I "think" this works (air):
		// Performs a branch/event test but does not actually "break" the block.
		// This allows exceptions to be raised, and is thus sufficient for
		// certain types of things like SYSCALL, EI, etc.  but it is not sufficient
		// for actual branching instructions.

        // [iter162] Post-Store をdelete: recCall内のiFlushCall(FLUSH_INTERPRETER|FLUSH_PC)がBL SYSCALL前に
        // cpuRegs.pc=pc(0x82068)を書き込む。SYSCALL()がcpuRegs.pcをchangeした場合(HLE等)に
        // この後書きがoverwriteしてしまうためdelete。cpuRegs.pcはiBranchTest→DispatcherRegで読まれる。
        // [iter683] Pre-Store log removed — spam

		iFlushCall(FLUSH_EVERYTHING);
		iBranchTest();

        // Fix: Link to the next block (fallthrough case).
        // Since g_branch==2 terminates the recRecompile loop, we must emit a jump to 'pc'.
        // Without this, execution falls through into the literal pool/garbage.
        armAsm->Nop();
        recBlocks.Link(HWADDR(pc), (s32*)armGetCurrentCodePointer()-1);
	}
	else
	{
		if (g_branch)
			pxAssert(!willbranch3);

		if (willbranch3 || !g_branch)
		{

			iFlushCall(FLUSH_EVERYTHING);

	// Split Block concatenation mode.
			// This code is run when blocks are split either to keep block sizes manageable
			// or because we're crossing a 4k page protection boundary in ps2 mem.  The latter
			// case can result in very short blocks which should not issue branch tests for
			// performance reasons.

			const int numinsts = (pc - startpc) >> 2; // (pc - startpc) / 4
			if (numinsts > 6)
				SetBranchImm(pc);
			else
			{
//				xMOV(ptr32[&cpuRegs.pc], pc);
                armStore(PTR_CPU(cpuRegs.pc), (uint64_t)pc);
//				xADD(ptr32[&cpuRegs.cycle], scaleblockcycles());
                // [V3 backport 2026-05-04] Step 5 runtime path or legacy s_nBlockCycles path
                armEmitCycleAdd(false);
//				recBlocks.Link(HWADDR(pc), xJcc32());
                armAsm->Nop();
                recBlocks.Link(HWADDR(pc), (s32*)armGetCurrentCodePointer()-1);
			}
		}
	}

	pxAssert(armGetCurrentCodePointer() < SysMemory::GetEERecEnd());

	s_pCurBlockEx->x86size = static_cast<u32>(armGetCurrentCodePointer() - recPtr);

	// [TEMP_DIAG] @@NATIVE_272C@@ dump complete ARM64 native code for stuck blocks
	if (HWADDR(startpc) == 0x272c08u || HWADDR(startpc) == 0x272c50u) {
		const u32* ncode = reinterpret_cast<const u32*>(s_pCurBlockEx->fnptr);
		u32 nwords = s_pCurBlockEx->x86size / 4;
		if (nwords > 120) nwords = 120;
		Console.WriteLn("@@NATIVE_272C@@ startpc=%08x fnptr=%p x86size=%u words=%u",
			startpc, (void*)ncode, s_pCurBlockEx->x86size, nwords);
		for (u32 ni = 0; ni < nwords; ni += 4) {
			if (ni + 3 < nwords)
				Console.WriteLn("  +%03x: %08x %08x %08x %08x", ni*4, ncode[ni], ncode[ni+1], ncode[ni+2], ncode[ni+3]);
			else {
				for (u32 nj = ni; nj < nwords; nj++)
					Console.WriteLn("  +%03x: %08x", nj*4, ncode[nj]);
			}
		}
	}

#if 0
	// Example: Dump both x86/EE code
	if (startpc == 0x456630) {
		iDumpBlock(s_pCurBlockEx->startpc, s_pCurBlockEx->size*4, s_pCurBlockEx->fnptr, s_pCurBlockEx->x86size);
	}
#endif
	Perf::ee.RegisterPC((void*)s_pCurBlockEx->fnptr, s_pCurBlockEx->x86size, s_pCurBlockEx->startpc);

	// [TEMP_DIAG] @@COMPILE_CONTENT@@ — record per-block stats
	{
		const u32 hw_pc = HWADDR(startpc);
		s_compile_pc_freq[hw_pc]++;
		s_compile_vpc_freq[startpc]++;
		s_compile_guest_insns += s_pCurBlockEx->size;        // guest instructions
		s_compile_native_bytes += s_pCurBlockEx->x86size;    // ARM64 bytes
		// Track recompilation of already-known PCs within one reset cycle
		if (!s_ever_compiled_vpcs.insert(startpc).second)
			s_recompile_of_known++;
	}

//	recPtr = xGetPtr();
    recPtr = armEndBlock();

	pxAssert((g_cpuHasConstReg & g_cpuFlushedConstReg) == g_cpuHasConstReg);

	s_pCurBlock = nullptr;
	s_pCurBlockEx = nullptr;
}

R5900cpu recCpu = {
	recReserve,
	recShutdown,

	recResetEE,
	recStep,
	recExecute,

	recSafeExitExecution,
	recCancelInstruction,
	recClear};

extern "C" void recReset()
{
    fprintf(stderr, "@@EEREC_INIT@@\n");
	recResetRaw();
}

extern "C" void recSwitchForReset()
{
    Console.WriteLn("DEBUG: recSwitchForReset calling longjmp to force JIT exit.");
    eeRecNeedsReset = true;
    fastjmp_jmp(&m_SetJmp_StateCheck, 1);
}
