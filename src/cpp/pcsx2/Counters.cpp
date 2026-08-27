// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <atomic>
#include <time.h>
#include <cmath>
#include <sys/time.h> // [TEMP_DIAG] gettimeofday for @@WALLCLOCK_PC@@

#include "Common.h"
#include "R3000A.h"
#include "Counters.h"
#include "IopCounters.h"

// [port 2026-05-30] iv2 VU1 per-stage profiler emit (defined in iv2/iv2_vu.cpp,
// no-op unless iPSX2_VU_IV2_ORCH_PROF=1). Declared here to call from the GS_TICK path.
extern "C" void iv2_vu1_orch_emit(uint32_t vsync);
#include "QAProbe.h" // [V34]

#include "GS.h"
#include "GS/GS.h"
#include "MTGS.h"
#include "Gif_Unit.h" // [P24] for gifUnit readAmount pacing
#include "PerformanceMetrics.h"
#include "Patch.h"
#include "ps2/HwInternal.h"
#include "SIO/Sio.h"
#include "SPU2/spu2.h"
#include "Recording/InputRecording.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "vtlb.h" // For vtlbdata
#include "IopMem.h" // [iter106] for iopMemRead32
#include "IopDma.h" // [iter504] for iopTestIntc()
#include "COP0.h"   // [P11] for WriteCP0Status()
#include "ps2/BiosTools.h" // [iter230] for BiosRetriggerEeloadCopy
#include "ps2/pgif.h"   // [iter447] psxGPUw - GP0 FIFO injection
#include "SIO/Pad/Pad.h" // [P16] Auto-start injection
#include "CDVD/CDVD.h"  // [P15] cdvd state access
#include "SifRingBuffer.h" // [P34] SIF/RPC ring buffer
#include "iv2/iv2_vu_ops.h" // [P101-18] T1 償却カウンタ (T1_StatsDump)

// [P34] EE cycle shadow for ring buffer (updated in vsync handler)
u32 g_ee_cycle_shadow = 0;
// [P15] g_sif0_ee_xfer_total removed — Sif0.cpp reverted to master
static u32 g_sif0_ee_xfer_total = 0; // stub to prevent link error
#include "IopHw.h"      // [iter447] HW_PS1_GPU_DATA

// [TEMP_DIAG] @@STUB_GUARD@@ clear counter — externed by iR5900.cpp for perf reporting
std::atomic<uint32_t> g_stub_guard_clear_count{0};
// [TEMP_DIAG] SW renderer draw/skip counters — externed by GSRendererSW.cpp
std::atomic<uint32_t> g_sw_draw_count{0};
std::atomic<uint32_t> g_sw_skip_count{0};
// [TEMP_DIAG] HW renderer draw counter — externed by GSRendererHW.cpp
// Removal condition: draws=0 root causeafter identified
std::atomic<uint32_t> g_hw_draw_count{0};
// [TEMP_DIAG] VIF→VU exec counters — defined in Vif_Codes.cpp
extern uint32_t getVU0ExecCount();
extern uint32_t getVU1ExecCount();
// [TEMP_DIAG] VU1 kick counter — externed by VU1micro.cpp
// Removal condition: BIOS browserafter confirmed
std::atomic<uint32_t> g_vu1_kick_count{0};
// [TEMP_DIAG] per-path GS xfer counter — defined in GS.cpp
extern volatile uint32_t g_gs_xfer_count[4];

// [CLIFF_DIAG] Per-frame lightweight metrics
#include <sys/time.h>
namespace CliffDiag {
	std::atomic<uint32_t> copyGSPkt{0};
	std::atomic<uint32_t> realignPkt{0};
	std::atomic<uint32_t> xgkickXfer{0};
	std::atomic<uint32_t> mtgsWaitCalls{0};
	std::atomic<uint32_t> path1Bytes{0};
	std::atomic<uint32_t> path3Bytes{0};
	std::atomic<int32_t>  readAmountMax{0};
	std::atomic<uint32_t> gsXferUs{0};
	std::atomic<uint32_t> gsXferCalls{0};
	std::atomic<uint32_t> vu1Ebit{0};
	std::atomic<uint32_t> vu1ExecUs{0};
	std::atomic<uint32_t> vu1Kicks{0};
	std::atomic<uint32_t> xgkickUs{0};
	// [P101-12 / 2026-06-30] per-kick/per-XGKICK gettimeofday timing gate. Default OFF (skips the
	// profiled ~56-sample gettimeofday tax). ON only when a consuming diag is requested. Reads env once.
	bool timingEnabled = []{
		const char* e;
		return ((e = std::getenv("iPSX2_CLIFF_TIMING")) && e[0] == '1') ||
		       ((e = std::getenv("iPSX2_HLE_PROBE"))    && e[0] == '1') ||
		       (std::getenv("iPSX2_VU1_SURVEY") != nullptr);
	}();
	// [HLE_PROBE] per-microprogram VU1-time distribution (diagnostic-only, env iPSX2_HLE_PROBE=1)
	std::atomic<bool> microDirty{true};
	bool hleProbeOn = false;
	static uint64_t curProgHash = 0;
	struct ProgBucket { uint64_t hash; uint64_t us; uint32_t kicks; uint32_t repPC; };
	static const int kProgBuckets = 512;
	static ProgBucket progTab[kProgBuckets];
	static int progCount = 0;
	uint64_t vu1CurProgHash(const u8* micro)
	{
		if (microDirty.load(std::memory_order_relaxed)) {
			uint64_t h = 1469598103934665603ULL; // FNV-1a 64
			const u8* p = micro; const u8* end = micro + 0x4000;
			for (; p < end; ++p) { h ^= *p; h *= 1099511628211ULL; }
			curProgHash = h;
			microDirty.store(false, std::memory_order_relaxed);
		}
		return curProgHash;
	}
	// [HLE_KERNEL PoC] hot T&L programs measured 2026-06-15 (GTA VC 3D ×3, FF10 OP 3D ×2)
	static const uint64_t kHotProgs[] = {
		0x8ac5daebfa578107ULL, 0x3f150dd0a1f0a9ebULL, 0x92d542c43042231cULL, // GTA VC pc 1454/1365/1435
		0xb5852f65bf2fd657ULL, 0x3837f6578cb57ab8ULL,                         // FF10 pc 84/42
		// J-Phoenix (SLPM-65033) 3D mecha — top VU1 programs measured 2026-06-27 (pc19 dominant 44-47%)
		0x125220b50566d214ULL, 0x1138e093c5b394f4ULL,                         // J-Phoenix pc19 (T&L dominant + variant)
		0x433853b123f76f55ULL, 0x0a284e6a538fe80bULL,                         // J-Phoenix pc18
		0xd01f0015ce2640f3ULL, 0x0942281b77c339d1ULL,                         // J-Phoenix pc5
		0xe557112a2eab4c28ULL, 0x6e40df8cda1e1dd3ULL,                         // J-Phoenix pc63
		// J-Phoenix BATTLE SCENE 2 hot programs (2026-06-28)
		0x129acad21b364844ULL,                                                // pc18 variant (40% scene2)
		0xdce9dc8df74918dfULL, 0x27fc6a33d1b61787ULL,                         // pc87 (new, ~11%)
		0x41b41666cb196537ULL,                                                // pc59 (new, ~9.5%)
		0xb18001eea1afab24ULL, 0xe005a28fb541a570ULL,                         // pc63 variants
	};
	bool g_hle_skip = false;
	bool vu1IsHotProgram(const u8* micro)
	{
		const uint64_t h = vu1CurProgHash(micro);
		for (uint64_t x : kHotProgs) if (x == h) return true;
		return false;
	}
	void vu1ProgSample(const u8* micro, uint32_t startPC, uint32_t dt)
	{
		static bool inited = false;
		if (!inited) { inited = true; const char* e = getenv("iPSX2_HLE_PROBE"); hleProbeOn = (e && e[0] == '1'); }
		if (!hleProbeOn) return;
		curProgHash = vu1CurProgHash(micro);
		int slot = -1;
		for (int i = 0; i < progCount; ++i) { if (progTab[i].hash == curProgHash) { slot = i; break; } }
		if (slot < 0 && progCount < kProgBuckets) { slot = progCount++; progTab[slot] = {curProgHash, 0, 0, startPC}; }
		if (slot >= 0) { progTab[slot].us += dt; progTab[slot].kicks++; }
	}
	void hleProbeDump(uint32_t frame)
	{
		if (!hleProbeOn || progCount == 0) return;
		uint64_t total = 0; for (int i = 0; i < progCount; ++i) total += progTab[i].us;
		if (total == 0) return;
		// selection-sort top buckets by us (progCount small)
		int order[kProgBuckets]; for (int i = 0; i < progCount; ++i) order[i] = i;
		for (int i = 0; i < progCount && i < 12; ++i) {
			int best = i; for (int j = i + 1; j < progCount; ++j) if (progTab[order[j]].us > progTab[order[best]].us) best = j;
			int t = order[i]; order[i] = order[best]; order[best] = t;
		}
		uint64_t cum = 0;
		Console.WriteLn(Color_Cyan, "@@HLE_PROBE@@ frame=%u distinctPrograms=%d totalVu1Us=%llu", frame, progCount, (unsigned long long)total);
		for (int i = 0; i < progCount && i < 12; ++i) {
			ProgBucket& b = progTab[order[i]];
			cum += b.us;
			Console.WriteLn(Color_Cyan, "  #%d hash=%016llx pc=%u us=%llu (%.1f%%) cum=%.1f%% kicks=%u",
				i, (unsigned long long)b.hash, b.repPC,
				(unsigned long long)b.us, 100.0 * (double)b.us / (double)total,
				100.0 * (double)cum / (double)total, b.kicks);
		}
	}
	std::atomic<uint32_t> path2Bytes{0};
	uint64_t frameStartUs{0};
	std::atomic<uint32_t> frameKickNum{0};
	std::atomic<uint32_t> firstWaitKick{0xFFFFFFFF};
	std::atomic<uint32_t> firstRealignKick{0xFFFFFFFF};
	std::atomic<uint32_t> waitUsAccum{0};
	uint32_t f2258_totalKicks{0};
	// [P52] Log output suppressed in Release via Console.cpp @@-tag filter
}
#include "common/Darwin/DarwinMisc.h" // [P11] iPSX2_JIT_HLE

// P0 gate: vtlb_NullHandlerFallback default OFF
#ifndef iPSX2_ENABLE_COUNTERS_NULLFALLBACK
#define iPSX2_ENABLE_COUNTERS_NULLFALLBACK 0
#endif

extern "C" void recSwitchForReset();

static const uint EECNT_FUTURE_TARGET = 0x10000000;

uint g_FrameCount = 0;

// Counter 4 takes care of scanlines - hSync/hBlanks
// Counter 5 takes care of vSync/vBlanks
Counter counters[4];
SyncCounter hsyncCounter;
SyncCounter vsyncCounter;

u32 nextStartCounter;	// records the cpuRegs.cycle value of the last call to rcntUpdate()
s32 nextDeltaCounter;	// delta from nextsCounter, in cycles, until the next rcntUpdate()

// [TEMP_DIAG] iter05: @@T0_READ@@ probe state promoted to file-scope to avoid __fi static-isolation
// Removal condition: @@T0_READ@@ 20件after confirmed、T0 state確定次第delete
static u32 s_t0d_prev_cycle = 0;
static u16 s_t0d_prev_ret   = 0xFFFFu;
static int s_t0d_n          = 0;
// [TEMP_DIAG][REMOVE_AFTER=EE_9FC41048_T0_WAIT_ROOTCAUSE_V1]
// SAFE_ONLYsupportのEE BIOS wait-loop限定 T0 probe/toggle state。
static int s_ee41048_t0_cfg_init = 0;
static int s_ee41048_t0_probe_enabled = 0;
static int s_ee41048_t0_toggle_enabled = 0;
static u32 s_ee41048_t0_probe_n = 0;
static u32 s_ee41048_t0_toggle_n = 0;
static u32 s_ee41048_t0_prev_cycle = 0;
static u16 s_ee41048_t0_prev_out = 0xFFFFu;

// [TEMP_DIAG] iter614: @@IOP_CP0SR_TRACE@@ — cp0sr 変化追跡 (file-scope to avoid __fi isolation)
// Removal condition: IEC=0 のroot causeafter identified
static u32 s_cp0sr_trace_prev = 0xDEADBEEFu;
static int s_cp0sr_trace_n    = 0;

// Forward declarations needed because C/C++ both are wimpy single-pass compilers.

static void rcntStartGate(bool mode, u32 sCycle);
static void rcntEndGate(bool mode, u32 sCycle);
static void rcntWcount(int index, u32 value);
static void rcntWmode(int index, u32 value);
static void rcntWtarget(int index, u32 value);
static void rcntWhold(int index, u32 value);

// For Analog/Double Strike and Interlace modes
static bool IsInterlacedVideoMode()
{
	return (gsVideoMode == GS_VideoMode::PAL || gsVideoMode == GS_VideoMode::NTSC || gsVideoMode == GS_VideoMode::DVD_NTSC || gsVideoMode == GS_VideoMode::DVD_PAL || gsVideoMode == GS_VideoMode::HDTV_1080I);
}

static bool IsProgressiveVideoMode()
{
	// The FIELD register only flips if the CMOD field in SMODE1 is set to anything but 0 and Front Porch bottom bit in SYNCV is set.
	// Also see "isReallyInterlaced()" in GSState.cpp
	return !(*(u32*)PS2GS_BASE(GS_SYNCV) & 0x1) || !(*(u32*)PS2GS_BASE(GS_SMODE1) & 0x6000);
}

void rcntReset(int index)
{
	counters[index].count = 0;
	counters[index].startCycle = cpuRegs.cycle;
}

// Updates the state of the nextCounter value (if needed) to serve
// any pending events for the given counter.
// Call this method after any modifications to the state of a counter.
static __fi void _rcntSet(int cntidx)
{
	s32 c;
	pxAssume(cntidx <= 4); // rcntSet isn't valid for h/vsync counters.

	const Counter& counter = counters[cntidx];

	// Stopped or special hsync gate?
	if (!rcntCanCount(cntidx) || (counter.mode.ClockSource == 0x3))
		return;

	if (!counter.mode.TargetInterrupt && !counter.mode.OverflowInterrupt && !counter.mode.ZeroReturn)
		return;
	// check for special cases where the overflow or target has just passed
	// (we probably missed it because we're doing/checking other things)
	if (counter.count > 0x10000 || counter.count > counter.target)
	{
		nextDeltaCounter = 4;
		return;
	}

	// nextCounter is relative to the cpuRegs.cycle when rcntUpdate() was last called.
	// However, the current _rcntSet could be called at any cycle count, so we need to take
	// that into account.  Adding the difference from that cycle count to the current one
	// will do the trick!

	c = ((0x10000 - counter.count) * counter.rate) - (cpuRegs.cycle - counter.startCycle);
	c += cpuRegs.cycle - nextStartCounter; // adjust for time passed since last rcntUpdate();

	if (c < nextDeltaCounter)
	{
		nextDeltaCounter = c;

		cpuSetNextEvent(nextStartCounter, nextDeltaCounter); // Need to update on counter resets/target changes
	}

	// Ignore target diff if target is currently disabled.
	// (the overflow is all we care about since it goes first, and then the
	// target will be turned on afterward, and handled in the next event test).

	if (counter.target & EECNT_FUTURE_TARGET)
	{
		return;
	}
	else
	{

		c = ((counter.target - counter.count) * counter.rate) - (cpuRegs.cycle - counter.startCycle);
		c += cpuRegs.cycle - nextStartCounter; // adjust for time passed since last rcntUpdate();

		if (c < nextDeltaCounter)
		{
			nextDeltaCounter = c;
			cpuSetNextEvent(nextStartCounter, nextDeltaCounter); // Need to update on counter resets/target changes
		}
	}
}


static __fi void cpuRcntSet()
{
	int i;

	// Default to next VBlank
	nextStartCounter = cpuRegs.cycle;
	nextDeltaCounter = vsyncCounter.deltaCycles - (cpuRegs.cycle - vsyncCounter.startCycle);

	// Also check next HSync
	s32 nextHsync = hsyncCounter.deltaCycles - (cpuRegs.cycle - hsyncCounter.startCycle);
	if (nextHsync < nextDeltaCounter)
		nextDeltaCounter = nextHsync;

	for (i = 0; i < 4; i++)
		_rcntSet(i);

	// sanity check!
	if (nextDeltaCounter < 0)
		nextDeltaCounter = 0;

	cpuSetNextEvent(nextStartCounter, nextDeltaCounter); // Need to update on counter resets/target changes
}


struct vSyncTimingInfo
{
	double Framerate;       // frames per second (8 bit fixed)
	GS_VideoMode VideoMode; // used to detect change (interlaced/progressive)
	u32 Render;             // time from vblank end to vblank start (cycles)
	u32 Blank;              // time from vblank start to vblank end (cycles)

	u32 GSBlank;            // GS CSR is swapped roughly 3.5 hblank's after vblank start

	u32 hSyncError;         // rounding error after the duration of a rendered frame (cycles)
	u32 hRender;            // time from hblank end to hblank start (cycles)
	u32 hBlank;             // time from hblank start to hblank end (cycles)
	u32 hScanlinesPerFrame; // number of scanlines per frame (525/625 for NTSC/PAL)
};

static vSyncTimingInfo vSyncInfo;

void rcntInit()
{
	int i;

	g_FrameCount = 0;

	static bool s_p0_nullfallback_logged = false;
	if (!s_p0_nullfallback_logged)
	{
		s_p0_nullfallback_logged = true;
		const bool env_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_NULL_FALLBACK", false);
		Console.WriteLn("@@P0_COUNTERS_NULLFALLBACK@@ active=%d env=%d", iPSX2_ENABLE_COUNTERS_NULLFALLBACK, env_enabled ? 1 : 0);
	}

	std::memset(counters, 0, sizeof(counters));

	for (i = 0; i < 4; i++)
	{
		counters[i].rate = 2;
		counters[i].target = 0xffff;
	}
	counters[0].interrupt = 9;
	counters[1].interrupt = 10;
	counters[2].interrupt = 11;
	counters[3].interrupt = 12;

	std::memset(&vSyncInfo, 0, sizeof(vSyncInfo));

	gsVideoMode = GS_VideoMode::Uninitialized;
	gsIsInterlaced = VMManager::Internal::IsFastBootInProgress();

	hsyncCounter.Mode = MODE_HRENDER;
	hsyncCounter.startCycle = cpuRegs.cycle;
	hsyncCounter.deltaCycles = vSyncInfo.hRender;
	vsyncCounter.Mode = MODE_VRENDER;
	vsyncCounter.deltaCycles = vSyncInfo.Render;
	vsyncCounter.startCycle = cpuRegs.cycle;

	for (i = 0; i < 4; i++)
		rcntReset(i);
	cpuRcntSet();
}

static void vSyncInfoCalc(vSyncTimingInfo* info, double framesPerSecond, u32 scansPerFrame)
{
	constexpr double clock = static_cast<double>(PS2CLK);

	const u64 Frame = clock * 10000ULL / framesPerSecond;
	const u64 Scanline = Frame / scansPerFrame;

	// There are two renders and blanks per frame. This matches the PS2 test results.
	// The PAL and NTSC VBlank periods respectively lasts for approximately 22 and 26 scanlines.
	// An older test suggests that these periods are actually the periods that VBlank is off, but
	// Legendz Gekitou! Saga Battle runs very slowly if the VBlank period is inverted.
	// Some of the more timing sensitive games and their symptoms when things aren't right:
	// Dynasty Warriors 3 Xtreme Legends - fake save corruption when loading save
	// Jak II - random speedups
	// Shadow of Rome - FMV audio issues
	const bool ntsc_hblank = gsVideoMode != GS_VideoMode::PAL && gsVideoMode != GS_VideoMode::DVD_PAL;
	const u64 HalfFrame = Frame / 2;
	const float extra_scanlines = static_cast<float>(IsProgressiveVideoMode()) * (ntsc_hblank ? 0.5f : 1.5f);
	const u64 Blank = Scanline * ((ntsc_hblank ? 22.5f : 24.5f) + extra_scanlines);
	const u64 Render = HalfFrame - Blank;
	const u64 GSBlank = Scanline * ((ntsc_hblank ? 3.5 : 3) + extra_scanlines); // GS VBlank/CSR Swap happens roughly 3.5(NTSC) and 3(PAL) Scanlines after VBlank Start

	// Important!  The hRender/hBlank timer ratio below is set according to PS2 tests.
	// in EE Cycles taken from PAL system:
	// 18876 cycles for hsync
	// 15796 cycles for hsync are low (render)
	// Ratio: 83.68298368298368
	u64 hRender = Scanline * 0.8368298368298368f;
	u64 hBlank = Scanline - hRender;

	if (!IsInterlacedVideoMode())
	{
		hBlank /= 2;
		hRender /= 2;
 	}

	//TODO: Carry fixed-point math all the way through the entire vsync and hsync counting processes, and continually apply rounding
	//as needed for each scheduled v/hsync related event. Much better to handle than this messed state.
	info->Framerate = framesPerSecond;
	info->GSBlank = (u32)(GSBlank / 10000);
	info->Render = (u32)(Render / 10000);
	info->Blank = (u32)(Blank / 10000);
	const u64 accumilated_vrender = (Render % 10000) + (Blank % 10000);
	info->Render += (u32)(accumilated_vrender / 10000);

	info->hRender = (u32)(hRender / 10000);
	info->hBlank = (u32)(hBlank / 10000);
	info->hScanlinesPerFrame = scansPerFrame;

	const u64 accumilatedHRenderError = (hRender % 10000) + (hBlank % 10000);
	const u64 accumilatedHFractional = accumilatedHRenderError % 10000;
	info->hRender += (u32)(accumilatedHRenderError / 10000);
	info->hSyncError = (u32)((accumilatedHFractional * (scansPerFrame / (IsInterlacedVideoMode() ? 2 : 1))) / 10000);

	// Note: In NTSC modes there is some small rounding error in the vsync too,
	// however it would take thousands of frames for it to amount to anything and
	// is thus not worth the effort at this time.
}

const char* ReportVideoMode()
{
	switch (gsVideoMode)
	{
	case GS_VideoMode::PAL:          return "PAL";
	case GS_VideoMode::NTSC:         return "NTSC";
	case GS_VideoMode::DVD_NTSC:     return "DVD NTSC";
	case GS_VideoMode::DVD_PAL:      return "DVD PAL";
	case GS_VideoMode::VESA:         return "VESA";
	case GS_VideoMode::SDTV_480P:    return "SDTV 480p";
	case GS_VideoMode::SDTV_576P:    return "SDTV 576p";
	case GS_VideoMode::HDTV_720P:    return "HDTV 720p";
	case GS_VideoMode::HDTV_1080I:   return "HDTV 1080i";
	case GS_VideoMode::HDTV_1080P:   return "HDTV 1080p";
	default:                         return "Unknown";
	}
}

const char* ReportInterlaceMode()
{
	const u64& smode2 = *(u64*)PS2GS_BASE(GS_SMODE2);
	return !IsProgressiveVideoMode() ? ((smode2 & 2) ? "Interlaced (Frame)" : "Interlaced (Field)") : "Progressive";
}

double GetVerticalFrequency()
{
	// Note about NTSC/PAL "double strike" modes:
	// NTSC and PAL can be configured in such a way to produce a non-interlaced signal.
	// This involves modifying the signal slightly by either adding or subtracting a line (526/524 instead of 525)
	// which has the function of causing the odd and even fields to strike the same lines.
	// Doing this modifies the vertical refresh rate slightly. Beatmania is sensitive to this and
	// not accounting for it will cause the audio and video to become desynced.
	//
	// In the case of the GS, I believe it adds a halfline to the vertical back porch but more research is needed.
	// For now I'm just going to subtract off the config setting.
	//
	// According to the GS:
	// NTSC (interlaced): 59.94
	// NTSC (non-interlaced): 59.82
	// PAL (interlaced): 50.00
	// PAL (non-interlaced): 49.76
	//
	// More Information:
	// https://web.archive.org/web/20201031235528/https://wiki.nesdev.com/w/index.php/NTSC_video
	// https://web.archive.org/web/20201102100937/http://forums.nesdev.com/viewtopic.php?t=7909
	// https://web.archive.org/web/20120629231826fw_/http://ntsc-tv.com/index.html
	// https://web.archive.org/web/20200831051302/https://www.hdretrovision.com/240p/

	switch (gsVideoMode)
	{
		case GS_VideoMode::Uninitialized: // SetGsCrt hasn't executed yet, give some temporary values.
			return 60.00;
		case GS_VideoMode::PAL:
		case GS_VideoMode::DVD_PAL:
			return (IsProgressiveVideoMode() == false) ? EmuConfig.GS.FrameratePAL : EmuConfig.GS.FrameratePAL - 0.24f;
		case GS_VideoMode::NTSC:
		case GS_VideoMode::DVD_NTSC:
			return (IsProgressiveVideoMode() == false) ? EmuConfig.GS.FramerateNTSC : EmuConfig.GS.FramerateNTSC - 0.11f;
		case GS_VideoMode::SDTV_480P:
			return 59.94;
		case GS_VideoMode::HDTV_1080P:
		case GS_VideoMode::HDTV_1080I:
		case GS_VideoMode::HDTV_720P:
		case GS_VideoMode::SDTV_576P:
		case GS_VideoMode::VESA:
			return 60.00;
		default:
			// Pass NTSC vertical frequency value when unknown video mode is detected.
			return FRAMERATE_NTSC * 2;
	}
}

void UpdateVSyncRate(bool force)
{
	// Notice:  (and I probably repeat this elsewhere, but it's worth repeating)
	//  The PS2's vsync timer is an *independent* crystal that is fixed to either 59.94 (NTSC)
	//  or 50.0 (PAL) Hz.  It has *nothing* to do with real TV timings or the real vsync of
	//  the GS's output circuit.  It is the same regardless if the GS is outputting interlace
	//  or progressive scan content.

	const double vertical_frequency = GetVerticalFrequency();

	const double frames_per_second = vertical_frequency / 2.0;

	if (vSyncInfo.Framerate != frames_per_second || vSyncInfo.VideoMode != gsVideoMode || force)
	{
		u32 total_scanlines = 0;
		bool custom = false;

		switch (gsVideoMode)
		{
			case GS_VideoMode::Uninitialized: // SYSCALL instruction hasn't executed yet, give some temporary values.
				if (gsIsInterlaced)
					total_scanlines = SCANLINES_TOTAL_NTSC_I;
				else
					total_scanlines = SCANLINES_TOTAL_NTSC_NI;
				break;
			case GS_VideoMode::PAL:
			case GS_VideoMode::DVD_PAL:
				custom = (EmuConfig.GS.FrameratePAL != Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_PAL);
				if (gsIsInterlaced)
					total_scanlines = SCANLINES_TOTAL_PAL_I;
				else
					total_scanlines = SCANLINES_TOTAL_PAL_NI;
				break;
			case GS_VideoMode::NTSC:
			case GS_VideoMode::DVD_NTSC:
				custom = (EmuConfig.GS.FramerateNTSC != Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_NTSC);
				if (gsIsInterlaced)
					total_scanlines = SCANLINES_TOTAL_NTSC_I;
				else
					total_scanlines = SCANLINES_TOTAL_NTSC_NI;
				break;
			case GS_VideoMode::SDTV_480P:
			case GS_VideoMode::SDTV_576P:
			case GS_VideoMode::HDTV_720P:
			case GS_VideoMode::VESA:
				total_scanlines = SCANLINES_TOTAL_NTSC_I;
				break;
			case GS_VideoMode::HDTV_1080P:
			case GS_VideoMode::HDTV_1080I:
				total_scanlines = SCANLINES_TOTAL_1080;
				break;
			case GS_VideoMode::Unknown:
			default:
				if (gsIsInterlaced)
					total_scanlines = SCANLINES_TOTAL_NTSC_I;
				else
					total_scanlines = SCANLINES_TOTAL_NTSC_NI;
				Console.Error("PCSX2-Counters: Unknown video mode detected");
				pxAssertMsg(false, "Unknown video mode detected via SetGsCrt");
		}

		const bool video_mode_initialized = gsVideoMode != GS_VideoMode::Uninitialized;

		// NBA Jam 2004 PAL will fail to display 3D on the menu if this value isn't correct on reset.
		if (video_mode_initialized && vSyncInfo.VideoMode != gsVideoMode)
			CSRreg.FIELD = 1;

		vSyncInfo.VideoMode = gsVideoMode;

		vSyncInfoCalc(&vSyncInfo, frames_per_second, total_scanlines);

		if (video_mode_initialized)
			Console.WriteLn(Color_Green, "UpdateVSyncRate: Mode Changed to %s.", ReportVideoMode());

		if (custom && video_mode_initialized)
			Console.WriteLn(Color_StrongGreen, "  ... with user configured refresh rate: %.02f Hz", vertical_frequency);

		s32 hdiff = hsyncCounter.deltaCycles;
		s32 vdiff = vsyncCounter.deltaCycles;
		hsyncCounter.deltaCycles = (hsyncCounter.Mode == MODE_HBLANK) ? vSyncInfo.hBlank : vSyncInfo.hRender;
		vsyncCounter.deltaCycles = (vsyncCounter.Mode == MODE_GSBLANK) ?
								  vSyncInfo.GSBlank :
								  ((vsyncCounter.Mode == MODE_VBLANK) ? vSyncInfo.Blank : vSyncInfo.Render);

		hsyncCounter.startCycle += hdiff - hsyncCounter.deltaCycles;
		vsyncCounter.startCycle += vdiff - vsyncCounter.deltaCycles;

		cpuRcntSet();

		VMManager::Internal::FrameRateChanged();
	}
}

// FMV switch stuff
extern uint eecount_on_last_vdec;
extern bool FMVstarted;
extern bool EnableFMV;

static bool s_last_fmv_state = false;

static __fi void DoFMVSwitch()
{
	bool new_fmv_state = s_last_fmv_state;
	if (EnableFMV)
	{
		DevCon.WriteLn("FMV started");
		new_fmv_state = true;
		EnableFMV = false;
	}
	else if (FMVstarted)
	{
		const int diff = cpuRegs.cycle - eecount_on_last_vdec;
		if (diff > 60000000)
		{
			DevCon.WriteLn("FMV ended");
			new_fmv_state = false;
			FMVstarted = false;
		}
	}

	if (new_fmv_state == s_last_fmv_state)
		return;

	s_last_fmv_state = new_fmv_state;

	switch (EmuConfig.GS.FMVAspectRatioSwitch)
	{
		case FMVAspectRatioSwitchType::Off:
			break;
		case FMVAspectRatioSwitchType::RAuto4_3_3_2:
			EmuConfig.CurrentAspectRatio = new_fmv_state ? AspectRatioType::RAuto4_3_3_2 : EmuConfig.GS.AspectRatio;
			break;
		case FMVAspectRatioSwitchType::R4_3:
			EmuConfig.CurrentAspectRatio = new_fmv_state ? AspectRatioType::R4_3 : EmuConfig.GS.AspectRatio;
			break;
		case FMVAspectRatioSwitchType::R16_9:
			EmuConfig.CurrentAspectRatio = new_fmv_state ? AspectRatioType::R16_9 : EmuConfig.GS.AspectRatio;
			break;
		case FMVAspectRatioSwitchType::R10_7:
			EmuConfig.CurrentAspectRatio = new_fmv_state ? AspectRatioType::R10_7 : EmuConfig.GS.AspectRatio;
			break;
		default:
			break;
	}

	if (EmuConfig.Gamefixes.SoftwareRendererFMVHack && EmuConfig.GS.UseHardwareRenderer())
	{
		DevCon.Warning("FMV Switch");
		// we don't use the sw toggle here, because it'll change back to auto if set to sw
		MTGS::SetSoftwareRendering(new_fmv_state, new_fmv_state ? GSInterlaceMode::AdaptiveTFF : EmuConfig.GS.InterlaceMode, false);
	}
}



extern "C" u32 vtlb_NullHandlerFallback(u32 paddr, u32 data, u32 vaddr, u32 mode)
{
	const bool safe_only = iPSX2_IsSafeOnlyEnabled();
	const bool diag_enabled = (!safe_only && iPSX2_GetRuntimeEnvBool("iPSX2_ENABLE_DIAG_FLAGS", false));
	const bool feature_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_NULL_FALLBACK", iPSX2_ENABLE_COUNTERS_NULLFALLBACK != 0);
	if (!(diag_enabled && feature_enabled))
		return 0;

    // Diagnosis: Why did JIT receive a NULL handler?
    // Re-perform lookup from C++ side.
    
    using namespace vtlb_private;
    u32 page = vaddr >> 12;
    // Access vtlbdata via g_cpuRegistersPack or global reference if available.
    // vtlbDef.h says: extern MapData& vtlbdata; in vtlb_private namespace.
    
    // Check if vmap entry exists (bounds check skipped for speed, but this is fallback)
    if (page >= VTLB_VMAP_ITEMS) {
        Console.WriteLn("CRITICAL: vtlb_NullHandlerFallback vaddr OOB: %08x", vaddr);
        return 0;
    }
    
    const auto& entry = vtlbdata.vmap[page];
    bool is_handler = entry.isHandler(vaddr);
    u8 handler_id = entry.assumeHandlerGetID(); 
    
    // Look up the function pointer in the table
    // mode: 0=Read8, 1=Read16, 2=Read32 ... we need to know the index.
    // Assuming 32-bit read for now (based on context of bug).
    // RWFT[size_idx][is_write]
    // Size Index for 32-bit is 2 (vtlbMemFP<32, false>::Index). Read is 0.
    
    void* handler_ptr = vtlbdata.RWFT[2][0][handler_id];
    
    Console.WriteLn("CRITICAL: JIT NULL HANDLER @ vaddr=%08x paddr=%08x. C++ Lookup: ID=%d Func=%p isHandler=%d", 
        vaddr, paddr, handler_id, handler_ptr, is_handler);

    // Tier 1: If C++ sees a valid handler, call it!
    if (handler_ptr) {
        Console.WriteLn("DEBUG: Recovering using C++ Handler...");
        // Cast to 32-bit read function
        vtlbMemR32FP* fp = (vtlbMemR32FP*)handler_ptr;
        // Arguments for handler: usually (u32 addr) where addr is physical or virtual depending on handler type.
        // HW handlers (paddr based) usually expect physical address.
        // Memory handlers (vaddr based) usually expect virtual.
        // Since this is mapped to 0x1000xxxx (HW), it expects physical.
        // But let's check KSEG1 logic. paddr is passed as physical.
        return fp(paddr); 
    }
    
    // Tier 2: Hardcoded fallback for Timer 0 (0x10000000 physical)
    // 0xb0000000 -> 0x10000000
    if ((paddr & 0x1FFFFFFF) == 0x10000000) {
        Console.WriteLn("DEBUG: Timer 0 Fallback triggered. Reading rcntRcount(0).");
        // rcntRcount returns u32 count.
        return rcntRcount(0);
    }
    
    // Tier 3: General panic fallback
    Console.WriteLn("CRITICAL: No handler found. Returning 0.");
    return 0;
}

extern "C" void vtlb_LogSelfLoop(u32 pc)
{
    static int log_count = 0;
    if (log_count < 1) { // Rate limit: Only 1 time
        Console.WriteLn("DEBUG: SelfLoop @ PC=%08x", pc);
        log_count++;
    }
}

extern "C" void vtlb_LogKseg1Write(u32 addr, u32 data)
{
    // static int log_count = 0;
    // if (log_count < 1) { // Rate limit: Only 1 time
        Console.WriteLn("DEBUG: KSEG1 WRITE Addr=%08x Data=%08x", addr, data);
    //    log_count++;
    // }
}

extern "C" void vtlb_MemWrite32_KSEG1(u32 addr, u32 data)
{
    // Track early timer setup writes in BIOS wait-loop area.
    static u32 s_kseg1_wr_probe = 0;
    if (s_kseg1_wr_probe < 64 && addr >= 0xB0000000 && addr < 0xB0000100)
    {
        Console.WriteLn("@@KSEG1_WR32@@ n=%u pc=%08x addr=%08x data=%08x", s_kseg1_wr_probe, cpuRegs.pc, addr, data);
        s_kseg1_wr_probe++;
    }
    // [iter57] @@KSEG1_WR32_HW@@ – capture HW register writes (0xB0008000-0xB0010000)
    // [iter235] rangeをextend: DMAC (0xB000E000), INTC (0xB000F000) include
    static u32 s_kseg1_hw_wr_probe = 0;
    if (s_kseg1_hw_wr_probe < 64 && addr >= 0xB0008000 && addr < 0xB0010000)
    {
        Console.WriteLn("@@KSEG1_WR32_HW@@ n=%u pc=%08x addr=%08x data=%08x", s_kseg1_hw_wr_probe, cpuRegs.pc, addr, data);
        s_kseg1_hw_wr_probe++;
    }

    // [iter146] @@KSEG1_SBUS_WRITE@@ – SBUS_F220/F240への書き込みを専用キャップで追跡（MCH操作でキャップ消費されない）
    {
        u32 phys = addr & 0x1FFFFFFFu;
        if (phys == 0x1000F220u || phys == 0x1000F240u) {
            static u32 s_sbus_kseg1_n = 0;
            if (s_sbus_kseg1_n < 20) {
                ++s_sbus_kseg1_n;
                Console.WriteLn("@@KSEG1_SBUS_WRITE@@ n=%u pc=0x%08x addr=0x%08x data=0x%08x",
                    s_sbus_kseg1_n, cpuRegs.pc, addr, data);
            }
        }
    }

    // [iter58] Pass original KSEG1 virtual address to memWrite32 so vtlb routes
    // correctly to hwWrite32 (hardware handler).  Stripping bits to physical
    // caused memWrite32 to treat 0x1000F430 as KUSEG EE-RAM, bypassing hardware.
    memWrite32(addr, data);
}

// [iter24] KSEG1 const bypass for SB (8-bit store): bypasses JIT handler-call path.
// Removal condition: vtlb_DynGenWrite_Const のhandlercall生成がfixされた時点。
extern "C" void vtlb_MemWrite8_KSEG1(u32 addr, u8 data)
{
    u32 paddr = addr & 0x1FFFFFFF;
    memWrite8(paddr, data);
}

static __fi void VSyncStart(u32 sCycle)
{
    // Console.WriteLn("DEBUG: INTC_STAT=%x, INTC_MASK=%x, Status=%x, CAUSE=%x", 
	// 	psHu32(INTC_STAT), psHu32(INTC_MASK), cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause);

	// DEBUG HACK REMOVED: Do not force enable interrupts.
	// if (psHu32(INTC_MASK) == 0 || !(cpuRegs.CP0.n.Status.val & 1) || !(cpuRegs.CP0.n.Status.val & 0x400) || (cpuRegs.CP0.n.Status.val & 0x2)) {
	// 	Console.WriteLn("DEBUG: Forcing INTC_MASK |= 0xC, Status: IE=1, IM2=1, EIE=1, EXL=0");
	// 	psHu32(INTC_MASK) |= 0xC;
	// 	cpuRegs.CP0.n.Status.val |= 0x10401; // Enable IE(0), IM2(10), EIE(16)
	// 	cpuRegs.CP0.n.Status.val &= ~0x2;    // Clear EXL(1) to unmask interrupts
	// }

	// End-of-frame tasks.
	DoFMVSwitch();
	VMManager::Internal::VSyncOnCPUThread();

	// Don't bother throttling if we're going to pause.
	if (!VMManager::Internal::IsExecutionInterrupted())
		VMManager::Internal::Throttle();

	// [P24] VSync-level MTGS pacing: drain GIF PATH1 buffer before starting next frame.
	// On iOS, MTGS (430ms/frame) can't keep up with EE (200ms/frame) during heavy VU1 scenes.
	// Without this, PATH1 buffer fills up (~8MB) and CopyGSPacketData deadlocks.
	// Cost: one Gif_MTGS_Wait (~5.9ms) per heavy frame = negligible at 5 FPS.
	{
		const s32 ra = gifUnit.gifPath[GIF_PATH_1].readAmount.load(std::memory_order_relaxed);
		if (ra > 4 * 1024 * 1024) // 4MB threshold
		{
			Gif_MTGS_Wait(false);
		}
	}

	// [CLIFF_DIAG] Per-frame metrics output (frame 1700+ only, ~1 line/frame)
	{
		struct timeval tv;
		gettimeofday(&tv, nullptr);
		uint64_t nowUs = (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
		uint64_t frameUs = (CliffDiag::frameStartUs > 0) ? (nowUs - CliffDiag::frameStartUs) : 0;
		if (g_FrameCount <= 10000)
		{
			s32 raMax = CliffDiag::readAmountMax.load(std::memory_order_relaxed);
			u32 gsUs = CliffDiag::gsXferUs.load(std::memory_order_relaxed);
			u32 gsN  = CliffDiag::gsXferCalls.load(std::memory_order_relaxed);
			u32 ebit = CliffDiag::vu1Ebit.load(std::memory_order_relaxed);
			u32 p2   = CliffDiag::path2Bytes.load(std::memory_order_relaxed);
			u32 vu1Us = CliffDiag::vu1ExecUs.load(std::memory_order_relaxed);
			u32 vu1K  = CliffDiag::vu1Kicks.load(std::memory_order_relaxed);
			u32 xgkUs = CliffDiag::xgkickUs.load(std::memory_order_relaxed);
			u32 eeRes = (frameUs > vu1Us) ? (u32)(frameUs - vu1Us) : 0;
			Console.WriteLn("@@CLIFF@@ f=%d wt=%llu vu1=%u xgkUs=%u gsUs=%u eeRes=%u kicks=%u gsN=%u ebit=%u cpGS=%u p1=%u p2=%u p3=%u iopc=%08x",
				g_FrameCount, frameUs, vu1Us, xgkUs, gsUs, eeRes, vu1K, gsN, ebit,
				CliffDiag::copyGSPkt.load(std::memory_order_relaxed),
				CliffDiag::path1Bytes.load(std::memory_order_relaxed),
				p2,
				CliffDiag::path3Bytes.load(std::memory_order_relaxed),
				psxRegs.pc);

			// [P27] F_CMP_DONE for frame 2258/2260
			if (g_FrameCount == 2258 || g_FrameCount == 2260)
			{
				Console.WriteLn(Color_Yellow,
					"[F_CMP_DONE] f=%d totalKicks=%u totalEbit=%u totalWait=%u firstWaitK=%u firstRealignK=%u waitUsAccum=%u p1=%u p2=%u p3=%u wt=%llu vu1=%u",
					g_FrameCount,
					CliffDiag::frameKickNum.load(std::memory_order_relaxed),
					ebit,
					CliffDiag::mtgsWaitCalls.load(std::memory_order_relaxed),
					CliffDiag::firstWaitKick.load(std::memory_order_relaxed),
					CliffDiag::firstRealignKick.load(std::memory_order_relaxed),
					CliffDiag::waitUsAccum.load(std::memory_order_relaxed),
					CliffDiag::path1Bytes.load(std::memory_order_relaxed),
					p2,
					CliffDiag::path3Bytes.load(std::memory_order_relaxed),
					frameUs, vu1Us);
			}
			if (g_FrameCount == 2258)
				CliffDiag::f2258_totalKicks = CliffDiag::frameKickNum.load(std::memory_order_relaxed);

			// [HLE_PROBE] cumulative per-microprogram VU1-time distribution dump
			if ((g_FrameCount % 300) == 0)
				CliffDiag::hleProbeDump(g_FrameCount);
		}
		CliffDiag::copyGSPkt.store(0, std::memory_order_relaxed);
		CliffDiag::realignPkt.store(0, std::memory_order_relaxed);
		CliffDiag::xgkickXfer.store(0, std::memory_order_relaxed);
		CliffDiag::mtgsWaitCalls.store(0, std::memory_order_relaxed);
		CliffDiag::path1Bytes.store(0, std::memory_order_relaxed);
		CliffDiag::path2Bytes.store(0, std::memory_order_relaxed);
		CliffDiag::path3Bytes.store(0, std::memory_order_relaxed);
		CliffDiag::readAmountMax.store(0, std::memory_order_relaxed);
		CliffDiag::gsXferUs.store(0, std::memory_order_relaxed);
		CliffDiag::gsXferCalls.store(0, std::memory_order_relaxed);
		CliffDiag::vu1Ebit.store(0, std::memory_order_relaxed);
		CliffDiag::vu1ExecUs.store(0, std::memory_order_relaxed);
		CliffDiag::vu1Kicks.store(0, std::memory_order_relaxed);
		CliffDiag::xgkickUs.store(0, std::memory_order_relaxed);
		// Reset P27 per-frame counters
		CliffDiag::frameKickNum.store(0, std::memory_order_relaxed);
		CliffDiag::firstWaitKick.store(0xFFFFFFFF, std::memory_order_relaxed);
		CliffDiag::firstRealignKick.store(0xFFFFFFFF, std::memory_order_relaxed);
		CliffDiag::waitUsAccum.store(0, std::memory_order_relaxed);
		CliffDiag::frameStartUs = nowUs;

	}

	gsPostVsyncStart(); // MUST be after framelimit; doing so before causes funk with frame times!

	// Poll input after MTGS frame push, just in case it has to stall to catch up.
	VMManager::Internal::PollInputOnCPUThread();

	EECNT_LOG("    ================  EE COUNTER VSYNC START (frame: %d)  ================", g_FrameCount);

	// Memcard auto ejection - Uses a tick system timed off of real time, decrementing one tick per frame.
	AutoEject::CountDownTicks();
	// Memcard IO detection - Uses a tick system to determine when memcards are no longer being written.
	MemcardBusy::Decrement();

	if (!GSSMODE1reg.SINT)
	{
		hwIntcIrq(INTC_VBLANK_S);
		rcntStartGate(true, sCycle); // Counters Start Gate code
		psxVBlankStart();
	}

	// INTC - VB Blank Start Hack --
	// Hack fix!  This corrects a freezeup in Granda 2 where it decides to spin
	// on the INTC_STAT register after the exception handler has already cleared
	// it.  But be warned!  Set the value to larger than 4 and it breaks Dark
	// Cloud and other games. -_-

	// How it works: Normally the INTC raises exceptions immediately at the end of the
	// current branch test.  But in the case of Grandia 2, the game's code is spinning
	// on the INTC status, and the exception handler (for some reason?) clears the INTC
	// before returning *and* returns to a location other than EPC.  So the game never
	// gets to the point where it sees the INTC Irq set true.

	// (I haven't investigated why Dark Cloud freezes on larger values)
	// (all testing done using the recompiler -- dunno how the ints respond yet)

	//cpuRegs.eCycle[30] = 2;

	// Update 08/2021: The only game I know to require this kind of thing as of 1.7.0 is Penny Racers/Gadget Racers (which has a patch to avoid the problem and others)
	// These games have a tight loop checking INTC_STAT waiting for the VBLANK Start, however the game also has a VBLANK Hander which clears it.
	// Therefore, there needs to be some delay in order for it to see the interrupt flag before the interrupt is acknowledged, likely helped on real hardware by the pipelines.
	// Without the patch and fixing this, the games have other issues, so I'm not going to rush to fix it.
	// Refraction

	// Bail out before the next frame starts if we're paused, or the CPU has changed.
	// Need to re-check this, because we might've paused during the sleep time.
	if (VMManager::Internal::IsExecutionInterrupted())
		Cpu->ExitExecution();
}

static __fi void GSVSync()
{
	static int gsvsync_count = 0;
	gsvsync_count++;

	// [P34] SIF ring buffer + DMA STR clear: ゲーム退出detect時
	{
		static bool s_was_game = false;
		static int s_dump_count = 0;
		bool is_game = (cpuRegs.pc >= 0x00100000u && cpuRegs.pc < 0x02000000u);
		if (is_game) s_was_game = true;
		if (s_was_game && !is_game && cpuRegs.pc == 0x00081fc0u) {
			s_was_game = false;
			if (s_dump_count < 3) {
				s_dump_count++;
				Console.WriteLn("@@SIF_RING_DUMP_START@@ vsync=%d ee_pc=%08x (game exited to EELOAD)",
					gsvsync_count, cpuRegs.pc);
				SifRing::DumpTo([](const char* fmt, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f) {
					Console.WriteLn(fmt, a, b, c, d, e, f);
				});
				Console.WriteLn("@@SIF_RING_DUMP_END@@ total=%u", SifRing::g_idx);
			}
		}
		extern u32 g_ee_cycle_shadow;
		g_ee_cycle_shadow = cpuRegs.cycle;
	}

    // @@GS_TICK@@ - Track GS path activity (once per second at 60fps = every 60 calls)
    // [TEMP_DIAG] @@GIF_DMA_RATE@@ counter — Removal condition: BIOS browserafter confirmed
    extern u32 g_gif_dma_total;
    static u32 s_last_gif_total = 0;
    static u32 s_last_draw = 0, s_last_skip = 0, s_last_vu1 = 0;
    static u32 s_last_p[4] = {};
    if (gsvsync_count <= 10 || (gsvsync_count % 60) == 0) {
        u32 gif_delta = g_gif_dma_total - s_last_gif_total;
        u32 cur_draw = g_sw_draw_count.load(std::memory_order_relaxed);
        u32 cur_skip = g_sw_skip_count.load(std::memory_order_relaxed);
        u32 cur_vu1  = g_vu1_kick_count.load(std::memory_order_relaxed);
        u32 cur_hw   = g_hw_draw_count.load(std::memory_order_relaxed);
        u32 cp[4];
        for (int i = 0; i < 4; i++) cp[i] = g_gs_xfer_count[i];
        if (gsvsync_count == 1) Console.WriteLn("@@XFER_ADDR@@ g_gs_xfer_count=%p", (void*)g_gs_xfer_count);
        // [TEMP_DIAG] PATH bytes from gifUnit
        extern u64 g_path_bytes[4];
        u64 pb[4];
        for (int i = 0; i < 4; i++) pb[i] = g_path_bytes[i];
        static u64 s_last_pb[4] = {};
        static u32 s_last_hw = 0;
        // [R103] BSS check + register dump after BSS zeroing
        if (gsvsync_count == 500 || gsvsync_count == 600 || gsvsync_count == 900) {
            u32* bss = (u32*)PSM(0x158700);
            if (bss) {
                bool allZero = true;
                for (int i = 0; i < 16; i++) { if (bss[i] != 0) { allZero = false; break; } }
                Console.WriteLn(Color_Red, "[R103_BSS] vsync=%d BSS@0x158700: %08x %08x %08x %08x %s ee_pc=%08x",
                    gsvsync_count, bss[0], bss[1], bss[2], bss[3],
                    allZero ? "ALL_ZERO" : "NOT_ZERO", cpuRegs.pc);
            }
        }
        Console.WriteLn("@@GS_TICK@@ vsync=%d gif_dma=%u(+%u) sw=%u(+%u) hw=%u(+%u) skips=%u(+%u) vu1=%u(+%u) p1=%u(+%u) p2=%u(+%u) p3=%u(+%u) ee_pc=%08x cycle=%u",
            gsvsync_count, g_gif_dma_total, gif_delta,
            cur_draw, cur_draw - s_last_draw,
            cur_hw, cur_hw - s_last_hw,
            cur_skip, cur_skip - s_last_skip,
            cur_vu1, cur_vu1 - s_last_vu1,
            cp[0], cp[0] - s_last_p[0],
            cp[1], cp[1] - s_last_p[1],
            cp[2], cp[2] - s_last_p[2], cpuRegs.pc, cpuRegs.cycle);
        iv2_vu1_orch_emit((uint32_t)gsvsync_count);  // [port 2026-05-30] VU1 stage profiler
        // [P101-18 A] T1 の償却 (入場回数 / 連続長) を vsync 刻みで出す。既定 OFF、
        // env iPSX2_VU1_T1_STATS=1 のときだけ 1 行増える。区間差分で I と E が取れる。
        iv2::vu_ops::T1_StatsDump();
        extern u32 g_vif1_dma_starts;
        static u32 s_last_vif1 = 0;
        Console.WriteLn("@@PATH_BYTES@@ vsync=%d vif1=%u(+%u) xgkick=%llu(+%llu) direct=%llu(+%llu) dma=%llu(+%llu) fifo=%llu(+%llu)",
            gsvsync_count, g_vif1_dma_starts, g_vif1_dma_starts - s_last_vif1,
            pb[0], pb[0] - s_last_pb[0],
            pb[1], pb[1] - s_last_pb[1],
            pb[2], pb[2] - s_last_pb[2],
            pb[3], pb[3] - s_last_pb[3]);
        extern u32 g_intchack_fire_cnt, g_intchack_call_cnt;
        static u32 s_last_fire = 0, s_last_call = 0;
        Console.WriteLn("@@INTCHACK@@ vsync=%d fire=%u(+%u) call=%u(+%u)",
            gsvsync_count, g_intchack_fire_cnt, g_intchack_fire_cnt - s_last_fire,
            g_intchack_call_cnt, g_intchack_call_cnt - s_last_call);
        s_last_fire = g_intchack_fire_cnt;
        s_last_call = g_intchack_call_cnt;
        for (int i = 0; i < 4; i++) s_last_pb[i] = pb[i];
        s_last_vif1 = g_vif1_dma_starts;
        s_last_gif_total = g_gif_dma_total;
        s_last_draw = cur_draw;
        s_last_hw = cur_hw;
        s_last_skip = cur_skip;
        s_last_vu1 = cur_vu1;
        for (int i = 0; i < 4; i++) s_last_p[i] = cp[i];
    }
    // [P16] Auto-start button injection for testing
    // env var iPSX2_AUTO_START_FRAME で指定したフレーム以降、Start ボタンを自動押下
    // Removal condition: iOS 仮想コントローラ UI impl後
    {
        static int s_auto_start_frame = -2; // -2 = not checked yet
        if (s_auto_start_frame == -2) {
            const char* env = getenv("iPSX2_AUTO_START_FRAME");
            s_auto_start_frame = env ? atoi(env) : -1;
            if (s_auto_start_frame >= 0)
                Console.WriteLn("@@CFG@@ iPSX2_AUTO_START_FRAME=%d", s_auto_start_frame);
        }
        if (s_auto_start_frame >= 0 && gsvsync_count >= (u32)s_auto_start_frame) {
            // Start = bind index 9 (PAD_START in PadDualshock2::Inputs)
            // Press for 4 frames, release for 60, repeat
            const u32 phase = (gsvsync_count - s_auto_start_frame) % 64;
            const float val = (phase < 4) ? 1.0f : 0.0f;
            Pad::SetControllerState(0, 9, val); // Port 0, PAD_START
            if (phase == 0 && gsvsync_count < (u32)s_auto_start_frame + 256)
                Console.WriteLn("@@AUTO_START@@ frame=%d pressing Start", gsvsync_count);
        }
    }

    // [TEMP_DIAG] @@SPINLOOP_DUMP@@ — dump MIPS instructions at FFX spinloop PCs
    // Removal condition: スピンloopcauseafter identified
    if (gsvsync_count == 1700 && eeMem) {
        // Focus on the loop control flow: 0x10b280-0x10b400 (384 bytes = 96 insns)
        Console.WriteLn("@@SPINLOOP_DUMP@@ --- PC=0x0010b280 (loop full) ---");
        for (u32 a = 0x10b280; a < 0x10b400; a += 4) {
            u32 insn = *(u32*)(eeMem->Main + a);
            Console.WriteLn("@@SPINLOOP_DUMP@@ %08x: %08x", a, insn);
        }
        // Also dump the VU0 function 0x2d0c00-0x2d0c60
        Console.WriteLn("@@SPINLOOP_DUMP@@ --- PC=0x002d0c00 (VU0 func) ---");
        for (u32 a = 0x2d0c00; a < 0x2d0c60; a += 4) {
            u32 insn = *(u32*)(eeMem->Main + a);
            Console.WriteLn("@@SPINLOOP_DUMP@@ %08x: %08x", a, insn);
        }
        // Dump watchdog register state
        Console.WriteLn("@@SPINLOOP_REGS@@ s0=%08x s1=%08x s2=%08x s3=%08x s4=%08x s5=%08x s6=%08x s7=%08x",
            cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0], cpuRegs.GPR.n.s2.UL[0],
            cpuRegs.GPR.n.s3.UL[0], cpuRegs.GPR.n.s4.UL[0], cpuRegs.GPR.n.s5.UL[0],
            cpuRegs.GPR.n.s6.UL[0], cpuRegs.GPR.n.s7.UL[0]);
        Console.WriteLn("@@SPINLOOP_REGS@@ v0=%08x v1=%08x t2=%08x sp=%08x fp=%08x ra=%08x counter(stk78)=%08x",
            cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0], cpuRegs.GPR.n.t2.UL[0],
            cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.s8.UL[0], cpuRegs.GPR.n.ra.UL[0],
            eeMem ? *(u32*)(eeMem->Main + (cpuRegs.GPR.n.sp.UL[0] & 0x1FFFFFF) + 0x78) : 0xDEAD);
    }

    // [TEMP_DIAG] @@OSDSYS_CODE_DUMP@@ — dump MIPS instructions at key OSDSYS PCs
    // Removal condition: BIOS browserafter confirmed
    if (gsvsync_count == 60 && eeMem) {
        // Scan for JAL/J instructions targeting 0x26e000-0x26f000 in the OSDSYS code range 0x260000-0x270000
        u32 jal_target_base = 0x26e000 >> 2; // 0x9B800
        u32 jal_target_end  = 0x26f000 >> 2;  // 0x9BC00
        Console.WriteLn("@@OSDSYS_CODE_DUMP@@ Scanning 0x260000-0x270000 for JAL/J targeting 0x26e000-0x26f000");
        for (u32 addr = 0x260000; addr < 0x270000; addr += 4) {
            u32 insn = *(u32*)(eeMem->Main + addr);
            u32 opcode = insn >> 26;
            u32 target = insn & 0x03FFFFFF;
            // JAL = opcode 3, J = opcode 2
            if ((opcode == 2 || opcode == 3) && target >= jal_target_base && target < jal_target_end) {
                Console.WriteLn("@@OSDSYS_CODE_DUMP@@ addr=%08x insn=%08x -> %s 0x%08x",
                    addr, insn, (opcode == 3) ? "JAL" : "J", target << 2);
            }
        }
        // Dump animation code: 0x26ee00-0x26f020 (GIF DMA trigger area)
        Console.WriteLn("@@OSDSYS_CODE_DUMP@@ --- animation code 0x26ee00-0x26f020 ---");
        for (u32 a = 0x26ee00; a < 0x26f020; a += 4) {
            u32 insn = *(u32*)(eeMem->Main + (a & 0x01FFFFFF));
            Console.WriteLn("@@OSDSYS_CODE_DUMP@@ %08x: %08x", a, insn);
        }
        // Also dump the function entry area: 0x26e300-0x26e420
        Console.WriteLn("@@OSDSYS_CODE_DUMP@@ --- function entries 0x26e300-0x26e420 ---");
        for (u32 a = 0x26e300; a < 0x26e420; a += 4) {
            u32 insn = *(u32*)(eeMem->Main + (a & 0x01FFFFFF));
            Console.WriteLn("@@OSDSYS_CODE_DUMP@@ %08x: %08x", a, insn);
        }
    }

    // [P37-2] STUB_GUARD and OSDSYS_FULL_GUARD removed.
    // Evidence: both guards never fired RESTORE (0 restores across multiple runs).
    // The 0x200000-0x270000 region is not being corrupted by JIT fastmem.

    // [TEMP_DIAG] @@OSDSYS_LOOP_DUMP@@ Dump EE code at busy-loop PCs
    // Removal condition: OSDSYS busy loop causeafter identified
    if (gsvsync_count == 100 && eeMem && cpuRegs.pc >= 0x200000u && cpuRegs.pc < 0x270000u) {
        u32 base = cpuRegs.pc & ~0xFu;
        Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ vsync=100 ee_pc=%08x ra=%08x v0=%08x a0=%08x a1=%08x sp=%08x t9=%08x",
            cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v0.UL[0],
            cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0], cpuRegs.GPR.n.sp.UL[0],
            cpuRegs.GPR.n.t9.UL[0]);
        // Dump code around ra (call site)
        u32 ra_area = cpuRegs.GPR.n.ra.UL[0];
        if (ra_area >= 0x200000u && ra_area < 0x270000u) {
            u32 ra_base = (ra_area - 0x20u) & 0x1FFFFFFu;
            const u32* rp = reinterpret_cast<const u32*>(eeMem->Main + ra_base);
            Console.WriteLn("@@OSDSYS_CALLSITE@@ ra=%08x code_before:", ra_area);
            Console.WriteLn("@@OSDSYS_CALLSITE@@ [%08x] %08x %08x %08x %08x %08x %08x %08x %08x",
                ra_area - 0x20u, rp[0], rp[1], rp[2], rp[3], rp[4], rp[5], rp[6], rp[7]);
            rp = reinterpret_cast<const u32*>(eeMem->Main + ((ra_area - 0x10u) & 0x1FFFFFFu));
            Console.WriteLn("@@OSDSYS_CALLSITE@@ [%08x] %08x %08x %08x %08x %08x %08x %08x %08x",
                ra_area - 0x10u, rp[0], rp[1], rp[2], rp[3], rp[4], rp[5], rp[6], rp[7]);
            rp = reinterpret_cast<const u32*>(eeMem->Main + (ra_area & 0x1FFFFFFu));
            Console.WriteLn("@@OSDSYS_CALLSITE@@ [%08x] %08x %08x %08x %08x",
                ra_area, rp[0], rp[1], rp[2], rp[3]);
        }
        for (u32 off = 0; off < 0x80u; off += 0x10u) {
            u32 addr = (base - 0x40u + off) & 0x1FFFFFFu;
            const u32* p = reinterpret_cast<const u32*>(eeMem->Main + addr);
            Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ [%08x] %08x %08x %08x %08x",
                base - 0x40u + off, p[0], p[1], p[2], p[3]);
        }
    }
    if (gsvsync_count == 200 && eeMem && cpuRegs.pc >= 0x200000u && cpuRegs.pc < 0x270000u) {
        Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ vsync=200 ee_pc=%08x ra=%08x v0=%08x a0=%08x",
            cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.a0.UL[0]);
    }
    if (gsvsync_count == 500 && eeMem && cpuRegs.pc >= 0x200000u && cpuRegs.pc < 0x270000u) {
        Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ vsync=500 ee_pc=%08x ra=%08x v0=%08x a0=%08x",
            cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.a0.UL[0]);
    }
    // [TEMP_DIAG] @@OSDSYS_EARLY_SNAP@@ Snapshot code at key addresses before corruption
    if (gsvsync_count == 10 && eeMem) {
        // Dump 0x2167a0-0x216800 at vsync=10 (before any corruption)
        const u32* p = reinterpret_cast<const u32*>(eeMem->Main + 0x2167a0u);
        Console.WriteLn("@@OSDSYS_EARLY_SNAP@@ vsync=10 [2167a0] %08x %08x %08x %08x %08x %08x %08x %08x",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        p = reinterpret_cast<const u32*>(eeMem->Main + 0x2167c0u);
        Console.WriteLn("@@OSDSYS_EARLY_SNAP@@ vsync=10 [2167c0] %08x %08x %08x %08x %08x %08x %08x %08x",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        p = reinterpret_cast<const u32*>(eeMem->Main + 0x2167e0u);
        Console.WriteLn("@@OSDSYS_EARLY_SNAP@@ vsync=10 [2167e0] %08x %08x %08x %08x %08x %08x %08x %08x",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    }
    if (gsvsync_count == 50 && eeMem) {
        const u32* p = reinterpret_cast<const u32*>(eeMem->Main + 0x2167a0u);
        Console.WriteLn("@@OSDSYS_EARLY_SNAP@@ vsync=50 [2167a0] %08x %08x %08x %08x %08x %08x %08x %08x",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        p = reinterpret_cast<const u32*>(eeMem->Main + 0x2167c0u);
        Console.WriteLn("@@OSDSYS_EARLY_SNAP@@ vsync=50 [2167c0] %08x %08x %08x %08x %08x %08x %08x %08x",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        // [TEMP_DIAG] @@BUGGY_BLOCK_DUMP@@ Dump MIPS code at 0x219700-0x219800 (buggy JIT region)
        // Removal condition: JIT bug root cause in 0x219700-0x219800 identified and fixed
        for (u32 daddr = 0x219700u; daddr < 0x219800u; daddr += 0x20u) {
            p = reinterpret_cast<const u32*>(eeMem->Main + daddr);
            Console.WriteLn("@@BUGGY_BLOCK_DUMP@@ [%06x] %08x %08x %08x %08x %08x %08x %08x %08x",
                daddr, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
        // Also dump 0x219600-0x219700 for context
        for (u32 daddr = 0x219600u; daddr < 0x219700u; daddr += 0x20u) {
            p = reinterpret_cast<const u32*>(eeMem->Main + daddr);
            Console.WriteLn("@@BUGGY_BLOCK_DUMP@@ [%06x] %08x %08x %08x %08x %08x %08x %08x %08x",
                daddr, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    }

    // [TEMP_DIAG] @@MEM_COMPARE@@ Dump memory at key OSDSYS addresses regardless of EE PC
    if ((gsvsync_count == 60 || gsvsync_count == 100 || gsvsync_count == 200) && eeMem) {
        const u32* p;
        Console.WriteLn("@@MEM_COMPARE@@ vsync=%d ee_pc=%08x", gsvsync_count, cpuRegs.pc);
        // Check 0x265800-0x266000 region (suspected NOP in JIT)
        for (u32 addr = 0x265800u; addr < 0x266000u; addr += 0x40u) {
            p = reinterpret_cast<const u32*>(eeMem->Main + (addr & 0x1FFFFFFu));
            bool allzero = (p[0]==0 && p[1]==0 && p[2]==0 && p[3]==0 &&
                           p[4]==0 && p[5]==0 && p[6]==0 && p[7]==0);
            if (!allzero) {
                Console.WriteLn("@@MEM_COMPARE@@ [%08x] %08x %08x %08x %08x %08x %08x %08x %08x",
                    addr, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            }
        }
        // Also check 0x264f00-0x265100
        for (u32 addr = 0x264f00u; addr < 0x265100u; addr += 0x40u) {
            p = reinterpret_cast<const u32*>(eeMem->Main + (addr & 0x1FFFFFFu));
            bool allzero = (p[0]==0 && p[1]==0 && p[2]==0 && p[3]==0);
            if (!allzero) {
                Console.WriteLn("@@MEM_COMPARE@@ [%08x] %08x %08x %08x %08x",
                    addr, p[0], p[1], p[2], p[3]);
            }
        }
    }

    // [TEMP_DIAG] @@OSDSYS_SYSCALL_SCAN@@ Find all syscall instructions in OSDSYS range
    // Removal condition: JIT OSDSYS loop divergence causeafter identified
    if (gsvsync_count == 56 && eeMem) {
        int found = 0;
        for (u32 addr = 0x200000u; addr < 0x270000u; addr += 4u) {
            u32 instr = *reinterpret_cast<const u32*>(eeMem->Main + addr);
            if ((instr & 0xFC00003Fu) == 0x0000000Cu) { // MIPS syscall opcode
                Console.WriteLn("@@OSDSYS_SYSCALL_SCAN@@ syscall at 0x%08x code=%u", addr, (instr >> 6) & 0xFFFFF);
                found++;
            }
        }
        Console.WriteLn("@@OSDSYS_SYSCALL_SCAN@@ total=%d in 0x200000-0x270000", found);

        // Dump main loop code at 0x216780-0x216880 (OSDSYS main loop)
        Console.WriteLn("@@OSDSYS_MAINLOOP_CODE@@ dumping 0x216780-0x216880:");
        for (u32 a = 0x216780u; a < 0x216880u; a += 0x10u) {
            const u32* p = reinterpret_cast<const u32*>(eeMem->Main + a);
            Console.WriteLn("@@OSDSYS_MAINLOOP_CODE@@ [%08x] %08x %08x %08x %08x", a, p[0], p[1], p[2], p[3]);
        }
        // Dump WaitSema caller at 0x2701c0-0x270240
        Console.WriteLn("@@OSDSYS_WAITSEMA_CALLER@@ dumping 0x2701c0-0x270240:");
        for (u32 a = 0x2701c0u; a < 0x270240u; a += 0x10u) {
            const u32* p = reinterpret_cast<const u32*>(eeMem->Main + a);
            Console.WriteLn("@@OSDSYS_WAITSEMA_CALLER@@ [%08x] %08x %08x %08x %08x", a, p[0], p[1], p[2], p[3]);
        }
        // Dump dispatch table WaitSema entry at 0x2588c0-0x258910
        Console.WriteLn("@@OSDSYS_DISPATCH@@ dumping WaitSema dispatch 0x2588c0-0x258910:");
        for (u32 a = 0x2588c0u; a < 0x258910u; a += 0x10u) {
            const u32* p = reinterpret_cast<const u32*>(eeMem->Main + a);
            Console.WriteLn("@@OSDSYS_DISPATCH@@ [%08x] %08x %08x %08x %08x", a, p[0], p[1], p[2], p[3]);
        }
        // Dump computation outer loop area 0x265400-0x265700
        Console.WriteLn("@@OSDSYS_COMP_CODE@@ dumping 0x265400-0x265700:");
        for (u32 a = 0x265400u; a < 0x265700u; a += 0x10u) {
            const u32* p = reinterpret_cast<const u32*>(eeMem->Main + a);
            Console.WriteLn("@@OSDSYS_COMP_CODE@@ [%08x] %08x %08x %08x %08x", a, p[0], p[1], p[2], p[3]);
        }
    }

    // [TEMP_DIAG] @@OSDSYS_EE_STATE@@ Dense EE register dump at key vsyncs
    // Removal condition: JIT OSDSYS loop divergence causeafter identified
    if (eeMem && (gsvsync_count >= 50 && gsvsync_count <= 55)) {
        const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
        Console.WriteLn("@@OSDSYS_EE_STATE@@ [%s] vsync=%d pc=%08x ra=%08x sp=%08x",
            mode, gsvsync_count, cpuRegs.pc,
            cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.sp.UL[0]);
        // 64-bit register dump (hi:lo format) for key registers
        Console.WriteLn("@@OSDSYS_EE64@@ v0=%08x_%08x v1=%08x_%08x a0=%08x_%08x a1=%08x_%08x",
            cpuRegs.GPR.n.v0.UL[1], cpuRegs.GPR.n.v0.UL[0],
            cpuRegs.GPR.n.v1.UL[1], cpuRegs.GPR.n.v1.UL[0],
            cpuRegs.GPR.n.a0.UL[1], cpuRegs.GPR.n.a0.UL[0],
            cpuRegs.GPR.n.a1.UL[1], cpuRegs.GPR.n.a1.UL[0]);
        Console.WriteLn("@@OSDSYS_EE64@@ a2=%08x_%08x a3=%08x_%08x s0=%08x_%08x s1=%08x_%08x",
            cpuRegs.GPR.n.a2.UL[1], cpuRegs.GPR.n.a2.UL[0],
            cpuRegs.GPR.n.a3.UL[1], cpuRegs.GPR.n.a3.UL[0],
            cpuRegs.GPR.n.s0.UL[1], cpuRegs.GPR.n.s0.UL[0],
            cpuRegs.GPR.n.s1.UL[1], cpuRegs.GPR.n.s1.UL[0]);
        Console.WriteLn("@@OSDSYS_EE64@@ s2=%08x_%08x s3=%08x_%08x s4=%08x_%08x s5=%08x_%08x",
            cpuRegs.GPR.n.s2.UL[1], cpuRegs.GPR.n.s2.UL[0],
            cpuRegs.GPR.n.s3.UL[1], cpuRegs.GPR.n.s3.UL[0],
            cpuRegs.GPR.n.s4.UL[1], cpuRegs.GPR.n.s4.UL[0],
            cpuRegs.GPR.n.s5.UL[1], cpuRegs.GPR.n.s5.UL[0]);
        Console.WriteLn("@@OSDSYS_EE64@@ s6=%08x_%08x s7=%08x_%08x t0=%08x_%08x t9=%08x_%08x",
            cpuRegs.GPR.n.s6.UL[1], cpuRegs.GPR.n.s6.UL[0],
            cpuRegs.GPR.n.s7.UL[1], cpuRegs.GPR.n.s7.UL[0],
            cpuRegs.GPR.n.t0.UL[1], cpuRegs.GPR.n.t0.UL[0],
            cpuRegs.GPR.n.t9.UL[1], cpuRegs.GPR.n.t9.UL[0]);
        Console.WriteLn("@@OSDSYS_EE64@@ hi=%08x_%08x lo=%08x_%08x cp0_sr=%08x cp0_epc=%08x",
            cpuRegs.HI.UL[1], cpuRegs.HI.UL[0],
            cpuRegs.LO.UL[1], cpuRegs.LO.UL[0],
            cpuRegs.CP0.r[12], cpuRegs.CP0.r[14]);
        // Also dump code at current PC (correct 32MB mask)
        if (cpuRegs.pc >= 0x200000u && cpuRegs.pc < 0x270000u) {
            u32 dump_base = (cpuRegs.pc - 0x20u) & 0x1FFFFFFu;
            const u32* dp = reinterpret_cast<const u32*>(eeMem->Main + dump_base);
            Console.WriteLn("@@OSDSYS_EE_STATE@@ code[pc-0x20]: %08x %08x %08x %08x %08x %08x %08x %08x",
                dp[0], dp[1], dp[2], dp[3], dp[4], dp[5], dp[6], dp[7]);
            dp = reinterpret_cast<const u32*>(eeMem->Main + (cpuRegs.pc & 0x1FFFFFFu));
            Console.WriteLn("@@OSDSYS_EE_STATE@@ code[pc+0x00]: %08x %08x %08x %08x %08x %08x %08x %08x",
                dp[0], dp[1], dp[2], dp[3], dp[4], dp[5], dp[6], dp[7]);
        }
    }

    // [R63] @@IOP_STUCK_DUMP@@ One-shot dump of IOP code at stuck PC and exception vector
    // Removal condition: IOP restart 後 stuck 解消後
    if (gsvsync_count == 925 && iopMem) {
        Console.WriteLn("@@IOP_STUCK_DUMP@@ vsync=%d iop_pc=%08x iop_sr=%08x iop_cause=%08x iop_epc=%08x",
            gsvsync_count, psxRegs.pc, psxRegs.CP0.r[12], psxRegs.CP0.r[13], psxRegs.CP0.r[14]);
        Console.WriteLn("@@IOP_STUCK_DUMP@@ iop_ra=%08x iop_sp=%08x iop_v0=%08x iop_a0=%08x iop_at=%08x",
            psxRegs.GPR.n.ra, psxRegs.GPR.n.sp, psxRegs.GPR.n.v0, psxRegs.GPR.n.a0, psxRegs.GPR.n.at);
        // Dump code at stuck PC area using raw memory pointer (no loop)
        {
            const u32* p1 = reinterpret_cast<const u32*>(iopMem->Main + 0x40200u);
            const u32* p2 = reinterpret_cast<const u32*>(iopMem->Main + 0x40210u);
            const u32* p3 = reinterpret_cast<const u32*>(iopMem->Main + 0x40220u);
            Console.WriteLn("@@IOP_STUCK_CODE@@ [40200] %08x %08x %08x %08x [40210] %08x %08x %08x %08x",
                p1[0], p1[1], p1[2], p1[3], p2[0], p2[1], p2[2], p2[3]);
            Console.WriteLn("@@IOP_STUCK_CODE@@ [40220] %08x %08x %08x %08x",
                p3[0], p3[1], p3[2], p3[3]);
            const u32* ev = reinterpret_cast<const u32*>(iopMem->Main + 0x80u);
            Console.WriteLn("@@IOP_STUCK_CODE@@ excvec[00080] %08x %08x %08x %08x %08x %08x %08x %08x",
                ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
        }
    }

    // [R63] @@IOP_IEC_FORCE_POST_RESTART@@ DISABLED — root cause (ROM2/IOP memory
    // overlap) is now fixed. This hack was forcibly enabling IOP interrupts during
    // the restart phase, interfering with normal IOP boot sequence.
    // Removed: ROM2 MapBlock + fastmem boundaryfixによりnot needed
#if 0
    if (gsvsync_count >= 920 && (psxRegs.CP0.r[12] & 0x401u) != 0x401u) {
        static int s_iec_force_cnt = 0;
        psxRegs.CP0.r[12] |= 0x401u;
        iopTestIntc();
        s_iec_force_cnt++;
        if (s_iec_force_cnt <= 30) {
            Console.WriteLn("@@IOP_IEC_FORCE_POST_RESTART@@ vsync=%d forced IEC=1 (n=%d) old_sr=%08x iop_pc=%08x",
                gsvsync_count, s_iec_force_cnt, psxRegs.CP0.r[12] & ~1u, psxRegs.pc);
        }
    }
#endif

    // [R61] @@GS_REG_GUARD@@ save & restore GS privileged registers
    // g_RealGSMem の offset 0x10-0xEF が handler を経由せず直接overwriteされるissueへの対策
    // PMODE (offset 0x00) は影響を受けないが、SMODE1/SMODE2/DISPFB/DISPLAY/BGCOLOR が破損する
    // Removal condition: g_RealGSMem 直接overwriteのroot cause確定・after fixed
    {
        static u8 s_saved_gs_regs[0xF0] = {};  // offset 0x00-0xEF
        static bool s_gs_regs_saved = false;
        static int s_gs_restore_cnt = 0;
        // Save after initial correct setup (PMODE set, SMODE2 set, around vsync 50)
        const u64 pmode_val = *(const u64*)(g_RealGSMem + 0x00);
        const u64 smode2_val = *(const u64*)(g_RealGSMem + 0x20);
        const u64 display2_val = *(const u64*)(g_RealGSMem + 0xA0);
        if (!s_gs_regs_saved && pmode_val != 0 && smode2_val != 0 && display2_val != 0) {
            std::memcpy(s_saved_gs_regs, g_RealGSMem, 0xF0);
            s_gs_regs_saved = true;
            Console.WriteLn("@@GS_REG_GUARD@@ saved at vsync=%d cycle=%u pmode=%016llx smode2=%016llx display2=%016llx",
                gsvsync_count, cpuRegs.cycle,
                (unsigned long long)pmode_val, (unsigned long long)smode2_val, (unsigned long long)display2_val);
        }
        // Check & restore: detect upper32==lower32 corruption pattern
        // [R63] Only restore sync/mode registers, NOT DISPFB/DISPLAY which are legitimately
        // updated by the BIOS for double buffering. Restoring DISPFB causes the display to
        // always point to the initial (empty) framebuffer instead of the drawn one.
        if (s_gs_regs_saved && smode2_val != 0 && (u32)(smode2_val >> 32) == (u32)(smode2_val)) {
            // Selective restore: only sync/mode registers, preserve framebuffer pointers
            // 0x00: PMODE, 0x10: SMODE1, 0x20: SMODE2, 0x30: SRFSH
            // 0x40: SYNCH1, 0x50: SYNCH2, 0x60: SYNCV, 0xE0: BGCOLOR
            static const u32 safe_offsets[] = {0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0xE0};
            for (u32 off : safe_offsets) {
                std::memcpy(g_RealGSMem + off, s_saved_gs_regs + off, 0x10);
            }
            // Do NOT restore: 0x70 (DISPFB1), 0x80 (DISPLAY1), 0x90 (DISPFB2), 0xA0 (DISPLAY2)
            s_gs_restore_cnt++;
            if (s_gs_restore_cnt <= 20) {
                const u64 dispfb2_val = *(const u64*)(g_RealGSMem + 0x90);
                Console.WriteLn("@@GS_REG_GUARD@@ RESTORED(selective) at vsync=%d (restore #%d) cycle=%u "
                    "corrupted_smode2=%016llx dispfb2=%016llx(preserved)",
                    gsvsync_count, s_gs_restore_cnt, cpuRegs.cycle,
                    (unsigned long long)smode2_val, (unsigned long long)dispfb2_val);
            }
        }
    }

    // [R61] @@OSDSYS_LOOP_DUMP@@ — pc=0x002058e0 loop周辺コードダンプ (one-shot)
    // EE が OSDSYS 0x2058e0 で停止しているcause調査
    // Removal condition: BIOS browserdisplayafter confirmed
    if (gsvsync_count == 100 && eeMem) {
        Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ vsync=%d Dumping code at 0x2058a0-0x205940:", gsvsync_count);
        for (u32 base = 0x2058a0u; base < 0x205940u; base += 0x20) {
            const u32* p = reinterpret_cast<const u32*>(eeMem->Main + base);
            Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ [0x%06x]: %08x %08x %08x %08x %08x %08x %08x %08x",
                base, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
        // Also dump wider context 0x205800-0x205a00
        Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ wider context 0x205800-0x205a00:");
        for (u32 base = 0x205800u; base < 0x205a00u; base += 0x20) {
            const u32* p = reinterpret_cast<const u32*>(eeMem->Main + base);
            Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ [0x%06x]: %08x %08x %08x %08x %08x %08x %08x %08x",
                base, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
        // [R62] callee 0x205310 (jal target from 0x2058d8) + 0x2053f0 (jal target from 0x20588c)
        Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ callee 0x205280-0x205400:");
        for (u32 base = 0x205280u; base < 0x205400u; base += 0x20) {
            const u32* p = reinterpret_cast<const u32*>(eeMem->Main + base);
            Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ [0x%06x]: %08x %08x %08x %08x %08x %08x %08x %08x",
                base, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    }

    // [R64 TEMP_DIAG] @@DISPFB2_CODE_DUMP@@ — dump MIPS code around PC=0x26df54 (DISPFB2 write site)
    // Removal condition: DISPFB2 固定issueのroot causeafter determined
    if (gsvsync_count == 55 && eeMem) {
        Console.WriteLn("@@DISPFB2_CODE_DUMP@@ MIPS 0x26dc00-0x26e000:");
        for (u32 base = 0x26dc00u; base < 0x26e000u; base += 0x20) {
            const u32* p = reinterpret_cast<const u32*>(eeMem->Main + base);
            Console.WriteLn("  [%06x]: %08x %08x %08x %08x %08x %08x %08x %08x",
                base, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
        // Also dump the caller region 0x26df04 (RA from DISPFB2 write)
        Console.WriteLn("@@DISPFB2_CODE_DUMP@@ caller 0x26de80-0x26df80:");
        for (u32 base = 0x26de80u; base < 0x26df80u; base += 0x20) {
            const u32* p = reinterpret_cast<const u32*>(eeMem->Main + base);
            Console.WriteLn("  [%06x]: %08x %08x %08x %08x %08x %08x %08x %08x",
                base, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    }

    // [R62] @@OSDSYS_STUCK_TRACE@@ — 0x2058e0 loopのregisterstateキャプチャ
    // restart 後に EE が 0x2058e0 で停止しているcause特定
    // Removal condition: BIOS browserdisplayafter confirmed
    if (gsvsync_count >= 880 && gsvsync_count <= 960 && (gsvsync_count % 5) == 0 && eeMem) {
        const u32 ee_pc = cpuRegs.pc;
        // Key OSDSYS state vars: 0x27b400 (func_205310 check), 0x27b410 (func_2053f0 check)
        Console.WriteLn("@@OSDSYS_STUCK_TRACE@@ vsync=%d pc=%08x v0=%08x v1=%08x "
            "s0=%08x sp=%08x ra=%08x cycle=%u "
            "st400=%08x st410=%08x st3e0=%08x",
            gsvsync_count, ee_pc,
            cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
            cpuRegs.GPR.n.s0.UL[0],
            cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.ra.UL[0],
            cpuRegs.cycle,
            *(const u32*)(eeMem->Main + 0x27b400),
            *(const u32*)(eeMem->Main + 0x27b410),
            *(const u32*)(eeMem->Main + 0x27b3e0));
        Console.WriteLn("@@OSDSYS_STUCK_TRACE@@ COP0_Status=%08x COP0_Cause=%08x COP0_EPC=%08x "
            "INTC_STAT=%08x INTC_MASK=%08x "
            "D5_CHCR=%08x D5_QWC=%08x D6_CHCR=%08x D6_QWC=%08x "
            "DMAC=%08x ee24124=%08x sbus_f220=%08x sbus_f230=%08x",
            cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.EPC,
            psHu32(INTC_STAT), psHu32(INTC_MASK),
            psHu32(0xC000), psHu32(0xC020),  // D5 (SIF0)
            psHu32(0xC400), psHu32(0xC420),  // D6 (SIF1)
            psHu32(0xE000),                    // DMAC_CTRL
            *(const u32*)(eeMem->Main + 0x24124),
            psHu32(0xF220), psHu32(0xF230));
    }

    // [TEMP_DIAG] Multi-phase code dump: early init + stuck phase
    {
        static bool s_codedump_done = false;
        static int s_early_dump_count = 0;
        // Early phase dump + sceSifGetReg code dump
        // One-shot dump of code at 0x1e9730-0x1e9760 at vsync 1000
        // [TEMP_DIAG] IOP memory dump at 0x19600 (SIF DMA dest for IOPRP) at vsync 920
        if (gsvsync_count == 920 && iopMem) {
            const u32* iop_data = (const u32*)(iopMem->Main + 0x19600);
            Console.WriteLn("@@IOP_SIF_DEST@@ phys=19600: %08x %08x %08x %08x %08x %08x %08x %08x",
                iop_data[0], iop_data[1], iop_data[2], iop_data[3],
                iop_data[4], iop_data[5], iop_data[6], iop_data[7]);
            Console.WriteLn("@@IOP_SIF_DEST@@ phys=19620: %08x %08x %08x %08x %08x %08x %08x %08x",
                iop_data[8], iop_data[9], iop_data[10], iop_data[11],
                iop_data[12], iop_data[13], iop_data[14], iop_data[15]);
            // Also as string
            char iop_str[65] = {};
            std::memcpy(iop_str, iopMem->Main + 0x19600 + 24, 64);
            iop_str[64] = 0;
            for (int i = 0; i < 64; i++) {
                if (iop_str[i] == 0) break;
                if (iop_str[i] < 0x20 || iop_str[i] > 0x7e) iop_str[i] = '.';
            }
            Console.WriteLn("@@IOP_SIF_DEST@@ ioprp_path_at_IOP='%s'", iop_str);
        }
        if (gsvsync_count == 1000 && eeMem) {
            const u32* code = (const u32*)(eeMem->Main + 0x1e9730);
            Console.WriteLn("@@SIFGETREG_CODE@@ 1e9730: %08x %08x %08x %08x %08x %08x %08x %08x",
                code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
            // Dump caller code at 0x1ee3c0-0x1ee400 (ra=0x1ee3d8 is return from sceSifIopReset)
            const u32* caller = (const u32*)(eeMem->Main + 0x1ee3c0);
            Console.WriteLn("@@CALLER_CODE@@ 1ee3c0: %08x %08x %08x %08x %08x %08x %08x %08x",
                caller[0], caller[1], caller[2], caller[3], caller[4], caller[5], caller[6], caller[7]);
            Console.WriteLn("@@CALLER_CODE@@ 1ee3e0: %08x %08x %08x %08x %08x %08x %08x %08x",
                caller[8], caller[9], caller[10], caller[11], caller[12], caller[13], caller[14], caller[15]);
            // Also dump sceSifIopReset code around 0x1ee2e0-0x1ee3e0
            const u32* reset_fn = (const u32*)(eeMem->Main + 0x1ee2e0);
            Console.WriteLn("@@RESET_FN@@ 1ee2e0: %08x %08x %08x %08x %08x %08x %08x %08x",
                reset_fn[0], reset_fn[1], reset_fn[2], reset_fn[3], reset_fn[4], reset_fn[5], reset_fn[6], reset_fn[7]);
            Console.WriteLn("@@RESET_FN@@ 1ee300: %08x %08x %08x %08x %08x %08x %08x %08x",
                reset_fn[8], reset_fn[9], reset_fn[10], reset_fn[11], reset_fn[12], reset_fn[13], reset_fn[14], reset_fn[15]);
            const u32* reset_fn2 = (const u32*)(eeMem->Main + 0x1ee320);
            Console.WriteLn("@@RESET_FN@@ 1ee320: %08x %08x %08x %08x %08x %08x %08x %08x",
                reset_fn2[0], reset_fn2[1], reset_fn2[2], reset_fn2[3], reset_fn2[4], reset_fn2[5], reset_fn2[6], reset_fn2[7]);
            Console.WriteLn("@@RESET_FN@@ 1ee340: %08x %08x %08x %08x %08x %08x %08x %08x",
                reset_fn2[8], reset_fn2[9], reset_fn2[10], reset_fn2[11], reset_fn2[12], reset_fn2[13], reset_fn2[14], reset_fn2[15]);
            const u32* reset_fn3 = (const u32*)(eeMem->Main + 0x1ee360);
            Console.WriteLn("@@RESET_FN@@ 1ee360: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                reset_fn3[0], reset_fn3[1], reset_fn3[2], reset_fn3[3], reset_fn3[4], reset_fn3[5],
                reset_fn3[6], reset_fn3[7], reset_fn3[8], reset_fn3[9], reset_fn3[10], reset_fn3[11]);
        }
        if (s_early_dump_count < 30 && eeMem &&
            ((gsvsync_count >= 20 && gsvsync_count <= 200 && (gsvsync_count % 10) == 0) ||
             (gsvsync_count >= 600 && gsvsync_count <= 1200 && (gsvsync_count % 100) == 0))) {
            s_early_dump_count++;
            Console.WriteLn("@@EARLY_STATE@@ vsync=%u pc=%08x "
                "D1_CHCR=%08x D1_MADR=%08x D2_CHCR=%08x "
                "INTC_STAT=%08x INTC_MASK=%08x "
                "DMAC_CTRL=%08x DMAC_STAT=%08x "
                "sbus_f220=%08x sbus_f230=%08x sbus_f240=%08x "
                "iop_pc=%08x cop0_status=%08x",
                gsvsync_count, cpuRegs.pc,
                psHu32(0x9000), psHu32(0x9010), psHu32(0xA000),
                psHu32(INTC_STAT), psHu32(INTC_MASK),
                psHu32(0xE000), psHu32(0xE010),
                psHu32(0xF220), psHu32(0xF230), psHu32(0xF240),
                psxRegs.pc, cpuRegs.CP0.n.Status.val);
            // [TEMP_DIAG] game stuck-PC code dump + caller context
            // Removal condition: ゲーム polling loopのcauseafter identified
            u32 phys = cpuRegs.pc & 0x1FFFFFFFu;
            if (phys >= 0x100000u && phys < 0x300000u && eeMem) {
                const u32* code = (const u32*)(eeMem->Main + phys);
                Console.WriteLn("@@GAME_STUCK_CODE@@ vsync=%u pc=%08x insn[-2..+5]: %08x %08x [%08x] %08x %08x %08x %08x %08x",
                    gsvsync_count, cpuRegs.pc,
                    (phys >= 8) ? code[-2] : 0, (phys >= 4) ? code[-1] : 0,
                    code[0], code[1], code[2], code[3], code[4], code[5]);
                Console.WriteLn("@@GAME_STUCK_REGS@@ vsync=%u v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x a3=%08x t0=%08x t1=%08x s0=%08x s1=%08x s2=%08x ra=%08x sp=%08x",
                    gsvsync_count,
                    cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
                    cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
                    cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.a3.UL[0],
                    cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0],
                    cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0],
                    cpuRegs.GPR.n.s2.UL[0], cpuRegs.GPR.n.ra.UL[0],
                    cpuRegs.GPR.n.sp.UL[0]);
                // Dump caller function context: 16 instructions around ra
                u32 ra_phys = cpuRegs.GPR.n.ra.UL[0] & 0x1FFFFFFFu;
                if (ra_phys >= 0x100020u && ra_phys < 0x300000u) {
                    const u32* ra_code = (const u32*)(eeMem->Main + ra_phys);
                    Console.WriteLn("@@GAME_CALLER_CODE@@ vsync=%u ra=%08x insn[-8..+3]: %08x %08x %08x %08x %08x %08x %08x %08x [%08x] %08x %08x %08x",
                        gsvsync_count, cpuRegs.GPR.n.ra.UL[0],
                        ra_code[-8], ra_code[-7], ra_code[-6], ra_code[-5],
                        ra_code[-4], ra_code[-3], ra_code[-2], ra_code[-1],
                        ra_code[0], ra_code[1], ra_code[2], ra_code[3]);
                }
                // Also dump SBUS_F200 (MSCOM) — sceSifIopReset writes module name addr here
                Console.WriteLn("@@GAME_SIF_STATE@@ vsync=%u F200=%08x F210=%08x F220=%08x F230=%08x F240=%08x F260=%08x D5_CHCR=%08x D6_CHCR=%08x",
                    gsvsync_count,
                    psHu32(0xF200), psHu32(0xF210), psHu32(0xF220), psHu32(0xF230), psHu32(0xF240), psHu32(0xF260),
                    psHu32(0xC000), psHu32(0xC400));
            }
        }
        // VIF1 post-activation probe: vsync 250-400, every 50 vsyncs
        {
            static int s_vif1_post_count = 0;
            if (s_vif1_post_count < 4 && gsvsync_count >= 250 && gsvsync_count <= 400
                && (gsvsync_count % 50) == 0) {
                s_vif1_post_count++;
                // VIF1 regs at eeHw offsets: STAT=0x3C00, ERR=0x3C10, MARK=0x3C30, CODE=0x3C30+0x40?
                // Actually: vif1 base=0x3C00, cycle=0x3C40, mask=0x3C50, code=0x3C30, err=0x3C20
                Console.WriteLn("@@VIF1_POST@@ vsync=%u "
                    "D1_CHCR=%08x D1_MADR=%08x D1_QWC=%04x D1_TADR=%08x "
                    "VIF1_STAT=%08x VIF1_ERR=%08x VIF1_CODE=%08x "
                    "D2_CHCR=%08x GIF_STAT=%08x "
                    "INTC_MASK=%08x pc=%08x iop_pc=%08x",
                    gsvsync_count,
                    psHu32(0x9000), psHu32(0x9010), psHu32(0x9020), psHu32(0x9080),
                    psHu32(0x3C00), psHu32(0x3C20), psHu32(0x3C30),
                    psHu32(0xA000), psHu32(0x3000),
                    psHu32(INTC_MASK), cpuRegs.pc, psxRegs.pc);
            }
        }
        // Stuck phase dump (original)
        if (!s_codedump_done && gsvsync_count >= 700 && eeMem) {
            s_codedump_done = true;
            // [CRITICAL] Compare VTLB read vs direct HW read for 0x1000A000 (D2_CHCR)
            {
                using namespace vtlb_private;
                u32 hw_val = psHu32(0xA000);  // direct HW register
                u32 vtlb_page = 0x1000A000u >> VTLB_PAGE_BITS;
                auto vmv = vtlbdata.vmap[vtlb_page];
                bool is_handler = vmv.isHandler(0x1000A000u);
                u32 vtlb_val = 0;
                if (is_handler) {
                    vtlb_val = vtlb_memRead<mem32_t>(0x1000A000u);
                } else {
                    // Direct pointer - this would be WRONG for HW regs
                    vtlb_val = *reinterpret_cast<u32*>(vmv.assumePtr(0x1000A000u));
                }
                Console.WriteLn("@@VTLB_D2_CHECK@@ hw=%08x vtlb=%08x is_handler=%d page=%08x",
                    hw_val, vtlb_val, (int)is_handler, vtlb_page);
                // Also check 0x10003C00 (VIF1_STAT) and 0x10003000 (GIF_STAT)
                u32 vif1_hw = psHu32(0x3C00);
                bool vif1_handler = vtlbdata.vmap[0x10003C00u >> VTLB_PAGE_BITS].isHandler(0x10003C00u);
                Console.WriteLn("@@VTLB_VIF1_CHECK@@ hw=%08x is_handler=%d",
                    vif1_hw, (int)vif1_handler);
            }
            // Dump instructions at 0x001e7500 (stuck loop)
            const u32* p1 = (const u32*)(eeMem->Main + 0x1e7500);
            Console.WriteLn("@@CODEDUMP_1E7500@@ %08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x",
                p1[0], p1[1], p1[2], p1[3], p1[4], p1[5], p1[6], p1[7],
                p1[8], p1[9], p1[10], p1[11], p1[12], p1[13], p1[14], p1[15]);
            // Dump at 0x001e7040 (other stuck addr)
            const u32* p2 = (const u32*)(eeMem->Main + 0x1e7040);
            Console.WriteLn("@@CODEDUMP_1E7040@@ %08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x",
                p2[0], p2[1], p2[2], p2[3], p2[4], p2[5], p2[6], p2[7],
                p2[8], p2[9], p2[10], p2[11], p2[12], p2[13], p2[14], p2[15]);
            // Dump at 0x00100bc0 (caller ra=00100be0 nearby)
            const u32* p3 = (const u32*)(eeMem->Main + 0x100bc0);
            Console.WriteLn("@@CODEDUMP_100BC0@@ %08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x",
                p3[0], p3[1], p3[2], p3[3], p3[4], p3[5], p3[6], p3[7],
                p3[8], p3[9], p3[10], p3[11], p3[12], p3[13], p3[14], p3[15]);
            // Also dump VIF1/D1 state at this point
            Console.WriteLn("@@CODEDUMP_DMASTATE@@ D1_CHCR=%08x D1_MADR=%08x D1_QWC=%04x "
                "D1_TADR=%08x D2_CHCR=%08x D_STAT=%08x D_CTRL=%08x VIF1=%08x",
                psHu32(0x9000), psHu32(0x9010), psHu32(0x9020),
                psHu32(0x9030), psHu32(0xA000), psHu32(0xE010), psHu32(0xE000),
                psHu32(0x3C00));
        }
    }

    // [TEMP_DIAG] @@IOP_EXCVEC_WATCH@@ — periodic check for IOP exception vector corruption
    // Expected: ac010400 at 0x80 (SW at, 0x0400)
    // Removal condition: IOP corruptcauseafter identified
    {
        static u32 s_excvec_last = 0;
        u32 excvec80 = iopMemRead32(0x80);
        if (excvec80 != s_excvec_last) {
            Console.WriteLn("@@IOP_EXCVEC_WATCH@@ vsync=%d excvec80=%08x (was %08x) excvec84=%08x excvec88=%08x excvec8c=%08x iop_pc=%08x iop_cyc=%u",
                gsvsync_count, excvec80, s_excvec_last,
                iopMemRead32(0x84), iopMemRead32(0x88), iopMemRead32(0x8C),
                psxRegs.pc, psxRegs.cycle);
            s_excvec_last = excvec80;
        }
    }

    // [TEMP_DIAG] @@DMAC_WATCH@@ — detect DMAC_CTRL (D_CTRL) transitions
    // Removal condition: DMAC disabled root cause after determined
    {
        static u32 s_dmac_last = 0xFFFFFFFF;
        const u32 dmac_cur = psHu32(0xE000);
        if (dmac_cur != s_dmac_last) {
            Console.WriteLn("@@DMAC_WATCH@@ vsync=%d DMAC %08x -> %08x ee_pc=%08x ee_cyc=%u iop_pc=%08x",
                gsvsync_count, s_dmac_last, dmac_cur, cpuRegs.pc, cpuRegs.cycle, psxRegs.pc);
            s_dmac_last = dmac_cur;
        }
    }

    // [TEMP_DIAG] @@JIT_PERF@@ — report JIT performance counters every 30 vsyncs
    // Removal condition: JIT performance issue resolved
    if (Cpu != &intCpu && (gsvsync_count % 30) == 0) {
        extern void recReportPerfCounters(int vsync);
        recReportPerfCounters(gsvsync_count);
    }

    // [iter666] @@EELOAD_LOOP_CODE@@ EELOAD ポーリングloop + SIF init code ダンプ
    // Removal condition: EELOAD SIF bind issue解決後
    if (gsvsync_count == 15) {
        const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
        // Dump EELOAD code around 0x82698
        for (u32 base = 0x82660u; base <= 0x82700u; base += 32) {
            Console.WriteLn("@@EELOAD_CODE@@ [%s] [%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode, base,
                memRead32(base+0), memRead32(base+4), memRead32(base+8), memRead32(base+12),
                memRead32(base+16), memRead32(base+20), memRead32(base+24), memRead32(base+28));
        }
        // [iter672] @@EELOAD_SIFINIT_CALLER@@ SIF init loopの caller コード解読
        // 0x82140-0x821E0: sceSifGetReg(0x80000002) ポーリングloop周辺
        // Removal condition: SIF init loopissue解決後
        for (u32 base = 0x82100u; base <= 0x82220u; base += 32) {
            Console.WriteLn("@@EELOAD_SIFINIT_CALLER@@ [%s] [%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode, base,
                memRead32(base+0), memRead32(base+4), memRead32(base+8), memRead32(base+12),
                memRead32(base+16), memRead32(base+20), memRead32(base+24), memRead32(base+28));
        }
        // [iter666] Dump exception vector 0x80000180 (SYSCALL handler)
        Console.WriteLn("@@EXCVEC_180@@ [%s] vsync=%d:", mode, gsvsync_count);
        for (u32 base = 0x80000180u; base < 0x80000200u; base += 32) {
            Console.WriteLn("@@EXCVEC_180@@ [%s] [%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode, base,
                memRead32(base+0), memRead32(base+4), memRead32(base+8), memRead32(base+12),
                memRead32(base+16), memRead32(base+20), memRead32(base+24), memRead32(base+28));
        }
        // [iter666] Dump Level 2 DMAC handler table (kernel internal)
        // 0x80018F54-0x80019040: the real handler registration table
        Console.WriteLn("@@L2_DMAC_TABLE@@ [%s] handler table ptr/count:", mode);
        for (u32 base = 0x80018F50u; base <= 0x80018F70u; base += 16) {
            Console.WriteLn("@@L2_DMAC_TABLE@@ [%s] [%08x]: %08x %08x %08x %08x",
                mode, base,
                memRead32(base+0), memRead32(base+4), memRead32(base+8), memRead32(base+12));
        }
        // Follow the pointer at 0x80018F60 to dump the actual handler entries
        u32 l2_ptr = memRead32(0x80018F60u);
        if (l2_ptr >= 0x80000000u && l2_ptr < 0x82000000u) {
            Console.WriteLn("@@L2_DMAC_ENTRIES@@ [%s] base=%08x:", mode, l2_ptr);
            // Dump 16 entries * ~24 bytes each = ~384 bytes
            for (u32 off = 0; off < 384; off += 32) {
                u32 a = l2_ptr + off;
                Console.WriteLn("@@L2_DMAC_ENTRIES@@ [%s] [%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, a,
                    memRead32(a+0), memRead32(a+4), memRead32(a+8), memRead32(a+12),
                    memRead32(a+16), memRead32(a+20), memRead32(a+24), memRead32(a+28));
            }
        } else {
            Console.WriteLn("@@L2_DMAC_ENTRIES@@ [%s] ptr=%08x (invalid)", mode, l2_ptr);
        }
    }

    // [iter671] @@IOP_PROGRESS@@ — IOP実行進捗比較 (JIT vs Interpreter)
    // 目的: SIF bind 応答で server_ptr=0 のcause調査。IOP側モジュール初期化タイミング比較
    // Removal condition: SIF bind server_ptr=0 issue解決後
    if (gsvsync_count <= 20 && (gsvsync_count % 2 == 0 || gsvsync_count <= 5)) {
        const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
        // IOP PC, cycle, SIF status
        Console.WriteLn("@@IOP_PROGRESS@@ [%s] vsync=%d ioppc=%08x iopcyc=%u sbus_f200=%08x sbus_f220=%08x sbus_f230=%08x sbus_f240=%08x",
            mode, gsvsync_count, psxRegs.pc, psxRegs.cycle,
            psHu32(0xF200), psHu32(0xF220), psHu32(0xF230), psHu32(0xF240));
        // IOP memory at known SIF server data region: check if sifcmd module is loaded
        // Also sample IOP RAM at 0x1CA00-0x1CB00 (near Interp's server_func=0x1CA68)
        u32 iop_1ca60 = iopMemRead32(0x0001CA60u);
        u32 iop_1ca64 = iopMemRead32(0x0001CA64u);
        u32 iop_1ca68 = iopMemRead32(0x0001CA68u);
        u32 iop_1ca6c = iopMemRead32(0x0001CA6Cu);
        u32 iop_1cab0 = iopMemRead32(0x0001CAB0u);
        Console.WriteLn("@@IOP_PROGRESS@@ [%s] vsync=%d IOP_MEM[1CA60]=%08x %08x %08x %08x [1CAB0]=%08x",
            mode, gsvsync_count, iop_1ca60, iop_1ca64, iop_1ca68, iop_1ca6c, iop_1cab0);
    }

    // [P12] @@MEM_26DF64_CHECK@@ — 0x26DF64 にコードが存在するかverify (one-shot)
    // 目的: Interpreter で PMODE=0x66 を書く ee_pc=0x26DF64 のコードが JIT でもロード済みかdetermine
    // Removal condition: ロード差異のcauseafter identified
    if (gsvsync_count == 1 || gsvsync_count == 5 || gsvsync_count == 10 || gsvsync_count == 20) {
        Console.WriteLn("@@MEM_26DF64_CHECK@@ vsync=%d MEM[0x26DF60]:", gsvsync_count);
        for (u32 _a = 0x26DF50u; _a < 0x26DFB0u; _a += 0x10u)
            Console.WriteLn("  [%06x] %08x %08x %08x %08x", _a,
                memRead32(_a), memRead32(_a+4), memRead32(_a+8), memRead32(_a+12));
        // Also check if 0x260000 area has any code loaded
        Console.WriteLn("@@MEM_260000_CHECK@@ MEM[0x260000]=%08x MEM[0x26DF00]=%08x MEM[0x270000]=%08x",
            memRead32(0x260000u), memRead32(0x26DF00u), memRead32(0x270000u));
    }

    // [P12] @@GS_DISP_REGS@@ — GS ディスプレイregisterダンプ (Sony ロゴ描画determine用)
    // 目的: PMODE.EN1/EN2, DISPLAY1/2, DISPFB1/2 をinterpreter/JIT で比較
    // Removal condition: JIT modeで Sony ロゴ描画after confirmed
    {
        static const int s_dump_vs[] = {1, 3, 5, 8, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 90, 120, 300, 600, 1200, 2400, 4800, 9600};
        for (int _i = 0; _i < 23; _i++) {
            if (gsvsync_count == s_dump_vs[_i]) {
                const u64 pmode    = *(const u64*)(g_RealGSMem + 0x00);
                const u64 smode2   = *(const u64*)(g_RealGSMem + 0x20);
                const u64 dispfb1  = *(const u64*)(g_RealGSMem + 0x70);
                const u64 display1 = *(const u64*)(g_RealGSMem + 0x80);
                const u64 dispfb2  = *(const u64*)(g_RealGSMem + 0x90);
                const u64 display2 = *(const u64*)(g_RealGSMem + 0xA0);
                const u64 bgcolor  = *(const u64*)(g_RealGSMem + 0xE0);
                Console.WriteLn("@@GS_DISP_REGS@@ vsync=%d PMODE=%016llx SMODE2=%016llx DISPFB1=%016llx DISPLAY1=%016llx DISPFB2=%016llx DISPLAY2=%016llx BGCOLOR=%016llx",
                    gsvsync_count,
                    (unsigned long long)pmode, (unsigned long long)smode2,
                    (unsigned long long)dispfb1, (unsigned long long)display1,
                    (unsigned long long)dispfb2, (unsigned long long)display2,
                    (unsigned long long)bgcolor);
                Console.WriteLn("@@GS_DISP_REGS@@ vsync=%d ee_pc=%08x iop_pc=%08x ee_cyc=%u gif_stat=%08x",
                    gsvsync_count, cpuRegs.pc, psxRegs.pc, cpuRegs.cycle,
                    psHu32(0x3020));
                // [R64 TEMP_DIAG] @@DISPFB2_COUNTER@@ — report total DISPFB2 write count at vsync
                {
                    extern std::atomic<uint32_t> g_dispfb2_write_count;
                    extern std::atomic<uint64_t> g_dispfb2_last_value;
                    Console.WriteLn("@@DISPFB2_COUNTER@@ vsync=%d total_writes=%u last_val=%016llx g_RealGSMem=%016llx",
                        gsvsync_count,
                        g_dispfb2_write_count.load(std::memory_order_relaxed),
                        (unsigned long long)g_dispfb2_last_value.load(std::memory_order_relaxed),
                        (unsigned long long)dispfb2);
                }
                break;
            }
        }
    }
    // [R64 TEMP_DIAG] @@DISPFB2_MONITOR@@ — frequent DISPFB2 tracking during animation
    // Removal condition: BIOS animationdisplayafter confirmed
    {
        extern std::atomic<uint32_t> g_dispfb2_write_count;
        extern std::atomic<uint64_t> g_dispfb2_last_value;
        static u32 s_last_dispfb2_report = 0;
        if (gsvsync_count >= 50 && gsvsync_count <= 400 && gsvsync_count >= s_last_dispfb2_report + 30) {
            s_last_dispfb2_report = gsvsync_count;
            const u64 dispfb2_mem = *(const u64*)(g_RealGSMem + 0x90);
            Console.WriteLn("@@DISPFB2_MONITOR@@ vsync=%d writes=%u last_api=%016llx mem=%016llx",
                gsvsync_count,
                g_dispfb2_write_count.load(std::memory_order_relaxed),
                (unsigned long long)g_dispfb2_last_value.load(std::memory_order_relaxed),
                (unsigned long long)dispfb2_mem);
        }
    }

    // [R61] @@GS_MEM_CORRUPTION_DETECT@@ — g_RealGSMem 直接overwritedetect
    // SMODE2(offset 0x20) が gsWrite64_page_00 を経由せずに破損するissueのcause特定
    // Removal condition: GS register破損のroot causeafter determined
    {
        static bool s_gs_corruption_detected = false;
        static u64 s_last_good_smode2 = 0;
        const u64 cur_smode2 = *(const u64*)(g_RealGSMem + 0x20);
        // Track last known-good value
        if (cur_smode2 != 0 && (u32)(cur_smode2 >> 32) != (u32)(cur_smode2))
            s_last_good_smode2 = cur_smode2;
        // Detect first corruption: upper32 == lower32 && non-zero
        if (!s_gs_corruption_detected && cur_smode2 != 0 &&
            (u32)(cur_smode2 >> 32) == (u32)(cur_smode2)) {
            s_gs_corruption_detected = true;
            Console.WriteLn("@@GS_MEM_CORRUPTION_DETECT@@ FIRST CORRUPTION at vsync=%d cycle=%u ee_pc=%08x",
                gsvsync_count, cpuRegs.cycle, cpuRegs.pc);
            Console.WriteLn("@@GS_MEM_CORRUPTION_DETECT@@ last_good_smode2=%016llx corrupted=%016llx",
                (unsigned long long)s_last_good_smode2, (unsigned long long)cur_smode2);
            // Dump full g_RealGSMem 0x00-0xFF in 16-byte rows
            for (u32 off = 0; off < 0x100; off += 0x10) {
                const u32* p = (const u32*)(g_RealGSMem + off);
                Console.WriteLn("@@GS_MEM_DUMP@@ [+%03x]: %08x %08x %08x %08x",
                    off, p[0], p[1], p[2], p[3]);
            }
            // Dump g_RealGSMem address for buffer overflow analysis
            Console.WriteLn("@@GS_MEM_CORRUPTION_DETECT@@ g_RealGSMem=%p size=0x%x",
                (void*)g_RealGSMem, (u32)sizeof(g_RealGSMem));
        }
        // Also check every vsync between 300-600 for more precise timing
        if (gsvsync_count >= 300 && gsvsync_count <= 600 && (gsvsync_count % 20 == 0)) {
            Console.WriteLn("@@GS_MEM_SCAN@@ vsync=%d smode2=%016llx pmode=%016llx dispfb2=%016llx cycle=%u",
                gsvsync_count,
                (unsigned long long)cur_smode2,
                (unsigned long long)*(const u64*)(g_RealGSMem + 0x00),
                (unsigned long long)*(const u64*)(g_RealGSMem + 0x90),
                cpuRegs.cycle);
        }
    }


    // [TEMP_DIAG] @@MEM_6E50_13AA8_DUMP@@ — 0x80006e50-6f20 および 0x80013a80-13ac0 の内容ダンプ (one-shot)
    // 目的: J 0x80013aa8 への到達パスと 0x80013aa8 の内容verify
    // Removal condition: 0x80001578 loop脱出メカニズムafter determined
    {
        static bool s_memdump_done = false;
        if (!s_memdump_done && gsvsync_count == 6) {
            s_memdump_done = true;
            Console.WriteLn("@@MEM_6E50_DUMP@@ vsync=%d [0x80006e50-0x80006f2f] (ee_pc=%08x):", gsvsync_count, cpuRegs.pc);
            for (u32 _a = 0x80006e50u; _a < 0x80006f30u; _a += 0x10u)
                Console.WriteLn("  [%08x] %08x %08x %08x %08x", _a,
                    memRead32(_a), memRead32(_a+4), memRead32(_a+8), memRead32(_a+12));
            Console.WriteLn("@@MEM_13A80_DUMP@@ vsync=%d [0x80013a80-0x80013ac0]:", gsvsync_count);
            for (u32 _a = 0x80013a80u; _a < 0x80013ac0u; _a += 0x10u)
                Console.WriteLn("  [%08x] %08x %08x %08x %08x", _a,
                    memRead32(_a), memRead32(_a+4), memRead32(_a+8), memRead32(_a+12));
        }
    }

    // [TEMP_DIAG] @@EE_1564_VSYNC_CHECK@@ — vsync boundaryで EE PC が 0x80001564/0x80001578 にあるかverify
    // 目的: Interpreter vs JIT の 0x80001564 loop到達差異を比較
    // Removal condition: loop脱出メカニズムafter identified
    {
        static int s_1564_n = 0;
        if (s_1564_n < 10 && (cpuRegs.pc == 0x80001564u || cpuRegs.pc == 0x80001578u)) {
            const u32 sr = cpuRegs.CP0.n.Status.val;
            Console.WriteLn("@@EE_1564_VSYNC_CHECK@@ n=%d vsync=%d pc=%08x exl=%d ra=%08x a0=%08x epc=%08x",
                s_1564_n, gsvsync_count, cpuRegs.pc,
                (int)((sr >> 1) & 1),
                cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.a0.UL[0],
                cpuRegs.CP0.n.EPC);
            s_1564_n++;
        }
    }

    // [P11] @@CKPT@@ — vsync checkpoint (iPSX2_CHECKPOINT_LOG=1 でenabled)
    // 目的: interpreter / JIT / JIT-no-HLE の3conditionで同一 vsync のstateを比較し乖離点を特定。
    // Removal condition: JIT BIOS browser到達after confirmed
    {
        static const bool s_ckpt_en = iPSX2_GetRuntimeEnvBool("iPSX2_CHECKPOINT_LOG", false);
        static bool s_ckpt_done[6] = {};
        static const int s_ckpt_vs[6] = {1, 2, 8, 29, 60, 120};
        if (s_ckpt_en) {
            for (int _i = 0; _i < 6; _i++) {
                if (!s_ckpt_done[_i] && gsvsync_count == s_ckpt_vs[_i]) {
                    s_ckpt_done[_i] = true;
                    extern R5900cpu intCpu;
                    const bool _hle = (Cpu != &intCpu) && (DarwinMisc::iPSX2_JIT_HLE != 0);
                    Console.WriteLn("@@CKPT@@ vsync=%d pc=0x%08x bev=%d status=0x%08x dmac_ctrl=0x%08x sif_f230=0x%08x hle=%d",
                        gsvsync_count, cpuRegs.pc,
                        (int)cpuRegs.CP0.n.Status.b.BEV,
                        cpuRegs.CP0.n.Status.val,
                        psHu32(0xE000), psHu32(0xF230), (int)_hle);
                }
            }
        }
    }

    // [TEMP_DIAG] iter642: @@LOADCORE_BOOT_INJECT@@ — MEM[0x14A0]=0x200000 force注入
    // 目的: LOADCORE fn#14 が boot list head (0x14A0) を参照して OSDSYS をロードするが、
    //       SIF deadlock により [0x14A0] が 0 のまま → vsync==1 に直接注入して deadlock 回避。
    // Removal condition: OSDSYS ロードafter confirmed (IOP PC が OSDSYS コードへ到達)
    if (gsvsync_count == 1) {
        u32 before = iopMemRead32(0x14A0u);
        // [P2 DISABLED] iopMemWrite32(0x14A0u, 0x200000u);  // state injection disabled for clean boot test
        Console.WriteLn("@@LOADCORE_BOOT_INJECT@@ vsync=1 MEM[0x14A0]=0x%08x (inject disabled)", before);

    }
    // [P12] @@OSDSYS_CODE_DUMP@@ — OSDSYS コードダンプ (one-shot at vsync=5)
    // 目的: OSDSYS のモジュールロード分岐conditionを特定
    // Removal condition: ディスプレイモジュールロードissue解決後
    if (gsvsync_count == 5) {
        Console.WriteLn("@@OSDSYS_CODE_DUMP@@ vsync=5 OSDSYS 0x100000-0x100200:");
        for (u32 _a = 0x100000u; _a < 0x100200u; _a += 0x10u)
            Console.WriteLn("  [%06x] %08x %08x %08x %08x", _a,
                memRead32(_a), memRead32(_a+4), memRead32(_a+8), memRead32(_a+12));
        // [P12] OSDSYS loader argument at 0x100d80 (string/path?) and extended code
        Console.WriteLn("@@OSDSYS_LOADER_ARG@@ 0x100d80:");
        for (u32 _a = 0x100d80u; _a < 0x100e00u; _a += 0x10u)
            Console.WriteLn("  [%06x] %08x %08x %08x %08x", _a,
                memRead32(_a), memRead32(_a+4), memRead32(_a+8), memRead32(_a+12));
        // OSDSYS state variable at 0x168780
        Console.WriteLn("@@OSDSYS_STATE@@ *(0x168780)=%08x *(0x168784)=%08x",
            memRead32(0x168780u), memRead32(0x168784u));
        // OSDSYS functions at 0x100ad8 and 0x100af8
        Console.WriteLn("@@OSDSYS_FUNC@@ 0x100ad0-0x100b80:");
        for (u32 _a = 0x100ad0u; _a < 0x100b80u; _a += 0x10u)
            Console.WriteLn("  [%06x] %08x %08x %08x %08x", _a,
                memRead32(_a), memRead32(_a+4), memRead32(_a+8), memRead32(_a+12));
        // Content at 0x200000 (what was loaded)
        Console.WriteLn("@@OSDSYS_200000@@ content at 0x200000:");
        for (u32 _a = 0x200000u; _a < 0x200040u; _a += 0x10u)
            Console.WriteLn("  [%06x] %08x %08x %08x %08x", _a,
                memRead32(_a), memRead32(_a+4), memRead32(_a+8), memRead32(_a+12));
    }

    // [TEMP_DIAG] iter614: @@IOP_CP0SR_TRACE@@ — cp0sr 変化を捕捉 (全vsync で変化detect, 最大20件 + vsync=1-10 force)
    // 目的: IEC=0 (cp0sr=0x404) になるタイミングと IOP PC を特定する (D-1 診断)
    // Removal condition: IEC=0 のroot causeafter identified
    {
        u32 cp0sr_now = psxRegs.CP0.r[12];
        bool changed  = (cp0sr_now != s_cp0sr_trace_prev);
        bool force    = (gsvsync_count <= 10);
        if ((changed || force) && s_cp0sr_trace_n < 30) {
            s_cp0sr_trace_n++;
            s_cp0sr_trace_prev = cp0sr_now;
            Console.WriteLn("@@IOP_CP0SR_TRACE@@ n=%d vsync=%d cp0sr=%08x IEc=%d iop_pc=%08x INTC_MASK=%04x INTC_STAT=%04x",
                s_cp0sr_trace_n, gsvsync_count, cp0sr_now, (int)(cp0sr_now & 1u),
                psxRegs.pc, (u16)psxHu32(0x1074u), (u16)psxHu32(0x1070u));
        }
    }

    // [TEMP_DIAG] iter623: @@IOP_35224_DUMP@@ — DataReady ISR return先コード解析 (vsync=30, freeze 前)
    // CDR_FAKE_DR n=1 (vsync~55) の直前に IOP RAM 0x35224 + 0x35284 を dump する。
    // 目的: BIOS ISR が 0x80035808 から戻った後に実行されるコードを特定 → EE スタベーションcauseの解明。
    // Removal condition: EE スタベーションcauseafter identified
    if (gsvsync_count == 30) {
        Console.WriteLn("@@IOP_35224_DUMP@@ vsync=30 IOP 0x35224-0x3527f (DataReady ISR return path):");
        for (u32 _a = 0x35224u; _a < 0x35280u; _a += 0x10u)
            Console.WriteLn("  [%06x] %08x %08x %08x %08x", _a,
                iopMemRead32(_a), iopMemRead32(_a+4), iopMemRead32(_a+8), iopMemRead32(_a+12));
        Console.WriteLn("@@IOP_35284_DUMP@@ vsync=30 IOP 0x35284-0x352df (BLEZ branch target):");
        for (u32 _a = 0x35284u; _a < 0x352e0u; _a += 0x10u)
            Console.WriteLn("  [%06x] %08x %08x %08x %08x", _a,
                iopMemRead32(_a), iopMemRead32(_a+4), iopMemRead32(_a+8), iopMemRead32(_a+12));
        Console.WriteLn("@@IOP_COUNTER_VAL@@ vsync=30 MEM[0x56f34]=%08x (DataReady ISR counter init)",
            iopMemRead32(0x56f34u));
        // [TEMP_DIAG] iter624: @@IOP_34BF0_DUMP@@ — counter fn 戻り後のコード解析 (one-shot)
        // Removal condition: EE スタベーションcauseafter identified
        Console.WriteLn("@@IOP_34BF0_DUMP@@ vsync=30 IOP 0x34b94-0x34c8f (JAL chain + post-counter code):");
        for (u32 _a = 0x34b94u; _a < 0x34c90u; _a += 0x10u)
            Console.WriteLn("  [%05x] %08x %08x %08x %08x", _a,
                iopMemRead32(_a), iopMemRead32(_a+4), iopMemRead32(_a+8), iopMemRead32(_a+12));

    }

    // [TEMP_DIAG] iter626: @@PCB5_FORCE_DELIVER@@ — vsync=600 (PCB初期化after completed) に PCB[5] をforce 0x4000
    // PCB base=mem[0x120] verify → entry[5].state を 0x4000 にforce書き込み
    // TestEvent(F1000005)=1 → IOP GPU dispatch (0x3118c) が psxGPUw を呼ぶかverify。
    // Removal condition: PGIF GP0 データ受信 (@@PGIF_GP0_WRITE@@ 発火) after confirmed
    if (gsvsync_count == 600) {
        // MEM[0x120] = PCB base (phys addr stored in KSEG0 or KSEG1 form)
        u32 pcb_base_raw = iopMemRead32(0x120u);
        u32 pcb_base_phys = pcb_base_raw & 0x1FFFFFFFu; // strip KSEG bits
        Console.WriteLn("@@PCB5_FORCE_DELIVER@@ vsync=600 MEM[0x120]=0x%08x pcb_base_phys=0x%05x",
            pcb_base_raw, pcb_base_phys);
        // dump PCB entries 0-7 using actual base
        for (int _i = 0; _i < 8; _i++) {
            u32 sa = pcb_base_phys + (u32)_i * 28u + 4u;
            u32 s = iopMemRead32(sa);
            Console.WriteLn("  PCB[%d] state=0x%08x addr=0x%05x", _i, s, sa);
        }
        // [TEMP_DIAG] iter633: PCB[5] force moved to persistent vsync>=600 block below
        // [TEMP_DIAG] iter627: @@CDR_STATE_FORCE@@ moved to per-vsync block below (iter631)
        // [TEMP_DIAG] iter628: @@PCB4_FORCE_DELIVER@@ — PCB[4](F1000004) も 0x4000 force
        // 根拠: iter627 IOP_3E554_DUMP: IOP PC=0x1ee8, v0=&PCB[4], a0=4 → TestEvent(F1000004) で -1 → 停止
        // ReadN パス (0x32e28) 内部で F1000004 を待機中 → PCB[4]=0x4000 で通過させる
        // Removal condition: PGIF_GP0_WRITE 発火after confirmed
        const u32 pcb4_sa = pcb_base_phys + 4u * 28u + 4u;
        u32 pcb4_prev = iopMemRead32(pcb4_sa);
        // [P2 DISABLED] iopMemWrite32(pcb4_sa, 0x4000u);  // state injection disabled for clean boot test
        Console.WriteLn("@@PCB4_FORCE_DELIVER@@ PCB[4].state=0x%08x at 0x%05x (inject disabled)", pcb4_prev, pcb4_sa);
        // [TEMP_DIAG] iter633: PCB[2] force moved to persistent vsync>=600 block below
    }

    // [TEMP_DIAG] iter631: @@CDR_STATE_FORCE@@ — vsync>=600 毎に MEM[0x56f14]=0xe6 を維持
    // 根拠: iter636 解析: 0xffff は BIOS ROM A(0xa1) CDR handlerをスピンさせる (実ディスクなし)
    //        0xe6 は CDR spin (0x32e44) を脱出させつつ 0x33238 には進入しない (v0=0 即返却)
    //        CDR パスは GPU 描画とは独立。IEC=0 解決が本質 (iter637 で追跡)
    // Removal condition: PGIF_GP0_WRITE 発火after confirmed
    if (gsvsync_count >= 600)
    {
        u32 cur_cdr = iopMemRead32(0x56f14u);
        // [P2 DISABLED] CDR state force injection disabled for clean boot test
        // if (cur_cdr != 0xe6u) iopMemWrite32(0x56f14u, 0xe6u);
        (void)cur_cdr;
    }

    // [TEMP_DIAG] iter665: PCB[5]/PCB[2] force 除去
    // 根拠: iter664 確定 — @@IEC_LATE@@ ゼロヒット → vsync≥600 で IEC=0 永続。
    //        PCB[5] force が IOP を GPU dispatch 99999-loop に閉じ込め (sr=0x404 = IEc=0)、
    //        BIOS RTOS scheduler が他タスクを実行できず VBlank ISR 発火不可。
    //        PCB[5] force 除去 → IOP が WaitEvent でスリープ → scheduler が IEC=1 で他タスク実行
    //        → VBlank ISR 発火可能 → 0x125c4=0x2000 injection がenabled化される可能性。
    // Removal condition: @@IEC_LATE@@ が vsync≥600 で IEc 振動をafter confirmed
    // [TEMP_DIAG] iter655: @@VBL_ECB_FORCE@@ — MEM[0x125c4]=0x2000 (ECB OPEN) を vsync≥600 でforce
    // Removal condition: PGIF_GP0_WRITE 発火after confirmed
    if (gsvsync_count >= 600) {
        // @@VBL_ECB_FORCE@@: [P2 DISABLED] MEM[0x125c4] injection disabled for clean boot test
        // if (iopMemRead32(0x125c4u) == 0u) iopMemWrite32(0x125c4u, 0x2000u);
    }

    // [iter601] @@GPU_EVT_IDS@@ one-shot: IOP GPU dispatch loop (0x3118c) が待つイベント ID を dump
    // 根拠: R37 iter559 計画。IOP は TestEvent(MEM[0x56ee4/ef0/eec]) を呼んで GPU write をcontrol。
    // F1000005 自然発火中だが GPU write がゼロ → 待ちイベント ID を特定してcause確定。
    // Removal condition: イベント ID after identified
    // [iter601] GPU event IDs: 解析完了 (F1000002/F1000005/F1000004)。
    // [iter603] IOPhandlingパス 0x2056B0 解析完了。IOP GPU write path 0x30c64 解析完了。
    // [iter604] @@VBLANK_PATH_DUMP@@ + @@GPU_WRITE_FN@@ one-shot
    // 根拠: VBlank path (combined=0 fall-through at 0x205b78) と IOP GPU write function (0x30ecc) が未解析。
    // Removal condition: VBlank path exit conditionafter confirmed + 0x30ecc 内容after confirmed
    if (gsvsync_count == 600) {
        const auto R32ee = [](u32 va) -> u32 {
            return (eeMem && (va & 0x1FFFFFFFu) < Ps2MemSize::MainRam)
                ? *(const u32*)(eeMem->Main + (va & 0x1FFFFFFFu)) : 0u;
        };
        // EE PS1DRV VBlank path 続き: 0x205b70-0x205c60 (60 words)
        Console.WriteLn("@@VBLANK_PATH_DUMP@@ EE 0x205b70-0x205c5f (VBlank path, combined=0 branch):");
        for (u32 _v = 0x205b70u; _v < 0x205c60u; _v += 0x10u) {
            Console.WriteLn("  [%06x] %08x %08x %08x %08x", _v,
                R32ee(_v), R32ee(_v+4), R32ee(_v+8), R32ee(_v+12));
        }
        // IOP GPU write function: 0x30ecc-0x30f3f (29 words) — JAL 0x30ecc から呼ばれる
        Console.WriteLn("@@GPU_WRITE_FN@@ IOP 0x30ecc-0x30f3f:");
        for (u32 _w = 0x30eccu; _w < 0x30f40u; _w += 0x10u) {
            Console.WriteLn("  [%05x] %08x %08x %08x %08x", _w,
                iopMemRead32(_w), iopMemRead32(_w+4), iopMemRead32(_w+8), iopMemRead32(_w+12));
        }
    }


    // [TEMP_DIAG] iter663: @@VBL_RGCB_INIT@@ — vsync=606 one-shot: IOP 0x12090-0x121ff dump
    // 根拠: iter662 確定: 0x4aa0 = always v0=0 を返すスタブ。
    //        RegisterGPUCallback (0x12094) の BNE (0x120c4) は not-taken → 初期化パス (0x120c8+) が実行。
    //        MEM[0x125c4]=0 のcause: 初期化パス (0x120c8-0x121bb) が 0x125c4 をconfigしない、
    //        またはconfig後に何かがresetする。初期化パスの命令列を decode する。
    // Removal condition: 初期化パスが 0x125c4 をconfigしないcauseafter determined
    if (gsvsync_count == 606)
    {
        Console.WriteLn("@@VBL_RGCB_INIT@@ IOP 0x12090-0x121ff (RegisterGPUCallback init path):");
        for (u32 _d = 0x12090u; _d < 0x12200u; _d += 0x10u)
            Console.WriteLn("  [%05x] %08x %08x %08x %08x", _d,
                iopMemRead32(_d), iopMemRead32(_d+4), iopMemRead32(_d+8), iopMemRead32(_d+12));
        Console.WriteLn("@@VBL_RGCB_INIT@@ done.");
    }

    // [TEMP_DIAG] iter640: @@IOP_VBLANK2_DUMP@@ — vsync=601 one-shot: IOP 0x12200-0x127ff (GPU callback funcs)
    // 根拠: iter639 解析: VBlank ISR (0x11f30) で 0x12624(0x125c0) が 0 を返す間初期化繰り返し
    //        0x12624/0x12660/0x12658/0x12540/0x12598 等が GPU コマンド送信に関わるかverify
    // Removal condition: 各functionの役割after determined
    if (gsvsync_count == 601)
    {
        Console.WriteLn("@@IOP_VBLANK2_DUMP@@ IOP 0x12200-0x127ff:");
        for (u32 _v = 0x12200u; _v < 0x12800u; _v += 0x10u)
            Console.WriteLn("  [%05x] %08x %08x %08x %08x", _v,
                iopMemRead32(_v), iopMemRead32(_v+4), iopMemRead32(_v+8), iopMemRead32(_v+12));
    }

    // [TEMP_DIAG] iter639: @@IOP_VBLANK_ISR_DUMP@@ — vsync=601 one-shot: IOP VBlank ISR 0x11f00-0x12200
    // 根拠: iter638 で VBlank ISR pc=0x12068 verify。命令列未解析。psxGPUw 未発火のcauseを探る。
    // Removal condition: VBlank ISR のbehaviorと psxGPUw 未発火causeがafter identified
    if (gsvsync_count == 601)
    {
        Console.WriteLn("@@IOP_VBLANK_ISR_DUMP@@ IOP 0x11f00-0x121ff:");
        for (u32 _v = 0x11f00u; _v < 0x12200u; _v += 0x10u)
            Console.WriteLn("  [%05x] %08x %08x %08x %08x", _v,
                iopMemRead32(_v), iopMemRead32(_v+4), iopMemRead32(_v+8), iopMemRead32(_v+12));
    }

    // [TEMP_DIAG] iter636: @@IOP_ROM_SPIN_DUMP@@ — vsync=601 one-shot: IOP BIOS ROM 0xbfc58a80-0xbfc58b40
    // 根拠: iter635 IOP_INTGATE n=10+ で pc=0xbfc58ac0 (IOP BIOS ROM) に固着
    // 0xffff が 0x33238 CDRデータパスをboot → BIOS ROM CDR handler到達 → スピンloop疑い
    // Removal condition: 0xbfc58ac0 の命令列が判明しsupport策after determined
    if (gsvsync_count == 601)
    {
        Console.WriteLn("@@IOP_ROM_SPIN_DUMP@@ IOP ROM 0xbfc58a80-0xbfc58b3f:");
        for (u32 _r = 0xbfc58a80u; _r < 0xbfc58b40u; _r += 0x10u)
            Console.WriteLn("  [%08x] %08x %08x %08x %08x", _r,
                iopMemRead32(_r), iopMemRead32(_r+4), iopMemRead32(_r+8), iopMemRead32(_r+12));
    }

    // [TEMP_DIAG] iter634: @@IOP_NEWPATH_DUMP@@ — vsync=601 one-shot: GPU dispatch 内部 + 新規 CDR 領域 IOP RAM dump
    // 根拠: iter633 IOP_INTGATE n=10+ で 0x31164/0x31170/0x3117c (GPU dispatch 内部) と
    //        0x3325c/0x33238/0x33340 (新規 CDR 領域) が出現。命令列未解読。
    // Removal condition: 各addressの命令列が判明し次の障壁が特定された後
    if (gsvsync_count == 601)
    {
        Console.WriteLn("@@IOP_NEWPATH_DUMP@@ IOP 0x31100-0x311ff (GPU dispatch internals):");
        for (u32 _a = 0x31100u; _a < 0x31200u; _a += 0x10u)
            Console.WriteLn("  [%05x] %08x %08x %08x %08x", _a,
                iopMemRead32(_a), iopMemRead32(_a+4), iopMemRead32(_a+8), iopMemRead32(_a+12));
        Console.WriteLn("@@IOP_NEWPATH_DUMP@@ IOP 0x33200-0x33400 (new CDR area):");
        for (u32 _b = 0x33200u; _b < 0x33400u; _b += 0x10u)
            Console.WriteLn("  [%05x] %08x %08x %08x %08x", _b,
                iopMemRead32(_b), iopMemRead32(_b+4), iopMemRead32(_b+8), iopMemRead32(_b+12));
    }

    // [iter278] Persistent SIF24124 bypass: [0x80024124] のみを 0 に維持。
    // INTC_STAT bit1 のconfigはdelete: bit1=1 にすると 8000FDE8→8000FAC8 が呼ばれ
    // 8000FAC8 が [0x80024124]=0x27 を再初期化してしまうため逆効果。
    // bit1=0 のまま [0x80024124]=0 を維持することで、outer loop (800126cc) が 0 をdetectし脱出できる。
    // Removal condition: IOP が自然に SIF 初期化パケットを送れるようになった後
    {
        static u32 s_sif_maint_n = 0;
        static bool s_loop_body_dumped = false;
        const u32 prev24124 = memRead32(0x80024124u);
        // [P2 DISABLED] SIF 0x80024124 maintenance injection disabled for clean boot test
        // if (prev24124 != 0u) { memWrite32(0x80024124u, 0u); ... }
        if (prev24124 != 0u) {
            s_sif_maint_n++;
            if (s_sif_maint_n <= 5)
                Console.WriteLn("@@SIF_MAINT@@ n=%u vsync=%d ee24124=%08x (inject disabled)",
                    s_sif_maint_n, gsvsync_count, prev24124);
        }
        // loop本体 (800125C8-800126B0) + IOP RAM (IOP recurring PC 0x16ac0) のダンプ - 1回のみ
        if (!s_loop_body_dumped && gsvsync_count >= 5) {
            s_loop_body_dumped = true;
            // [iter279] IOP RAM dump: IOP が 0x00016ae8 付近で何を polling しているかを特定
            if (iopMem) {
                const u8* iram = iopMem->Main;
                auto iop32 = [&](u32 off) { return *reinterpret_cast<const u32*>(iram + off); };
                Console.WriteLn("@@IOP_PC_AREA_A@@ [0x16AC0]: %08x %08x %08x %08x %08x %08x %08x %08x",
                    iop32(0x16AC0), iop32(0x16AC4), iop32(0x16AC8), iop32(0x16ACC),
                    iop32(0x16AD0), iop32(0x16AD4), iop32(0x16AD8), iop32(0x16ADC));
                Console.WriteLn("@@IOP_PC_AREA_B@@ [0x16AE0]: %08x %08x %08x %08x %08x %08x %08x %08x",
                    iop32(0x16AE0), iop32(0x16AE4), iop32(0x16AE8), iop32(0x16AEC),
                    iop32(0x16AF0), iop32(0x16AF4), iop32(0x16AF8), iop32(0x16AFC));
                Console.WriteLn("@@IOP_PC_AREA_C@@ [0x16B00]: %08x %08x %08x %08x %08x %08x %08x %08x",
                    iop32(0x16B00), iop32(0x16B04), iop32(0x16B08), iop32(0x16B0C),
                    iop32(0x16B10), iop32(0x16B14), iop32(0x16B18), iop32(0x16B1C));
                // IOP RAM 0x16C00 area (IOP recurring: 0x16C1C, 0x16C2C, 0x16C38)
                Console.WriteLn("@@IOP_PC_AREA_D@@ [0x16C00]: %08x %08x %08x %08x %08x %08x %08x %08x",
                    iop32(0x16C00), iop32(0x16C04), iop32(0x16C08), iop32(0x16C0C),
                    iop32(0x16C10), iop32(0x16C14), iop32(0x16C18), iop32(0x16C1C));
                Console.WriteLn("@@IOP_PC_AREA_E@@ [0x16C20]: %08x %08x %08x %08x %08x %08x %08x %08x",
                    iop32(0x16C20), iop32(0x16C24), iop32(0x16C28), iop32(0x16C2C),
                    iop32(0x16C30), iop32(0x16C34), iop32(0x16C38), iop32(0x16C3C));
                // IOP RAM 0x17880 area (IOP recurring: 0x178e0, 0x178e8)
                Console.WriteLn("@@IOP_PC_AREA_F@@ [0x178C0]: %08x %08x %08x %08x %08x %08x %08x %08x",
                    iop32(0x178C0), iop32(0x178C4), iop32(0x178C8), iop32(0x178CC),
                    iop32(0x178D0), iop32(0x178D4), iop32(0x178D8), iop32(0x178DC));
                Console.WriteLn("@@IOP_PC_AREA_G@@ [0x178E0]: %08x %08x %08x %08x %08x %08x %08x %08x",
                    iop32(0x178E0), iop32(0x178E4), iop32(0x178E8), iop32(0x178EC),
                    iop32(0x178F0), iop32(0x178F4), iop32(0x178F8), iop32(0x178FC));
                // IOP SBUS-related: read what IOP sees at 0x00 (IOP SIF read area)
                Console.WriteLn("@@IOP_RAM_00@@ [0x0]: %08x %08x %08x %08x %08x %08x %08x %08x",
                    iop32(0x0), iop32(0x4), iop32(0x8), iop32(0xC),
                    iop32(0x10), iop32(0x14), iop32(0x18), iop32(0x1C));
            }
            Console.WriteLn("@@LOOP_BODY_A@@ [800125C8]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800125C8u), memRead32(0x800125CCu), memRead32(0x800125D0u), memRead32(0x800125D4u),
                memRead32(0x800125D8u), memRead32(0x800125DCu), memRead32(0x800125E0u), memRead32(0x800125E4u));
            Console.WriteLn("@@LOOP_BODY_B@@ [800125E8]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800125E8u), memRead32(0x800125ECu), memRead32(0x800125F0u), memRead32(0x800125F4u),
                memRead32(0x800125F8u), memRead32(0x800125FCu), memRead32(0x80012600u), memRead32(0x80012604u));
            Console.WriteLn("@@LOOP_BODY_C@@ [80012608]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012608u), memRead32(0x8001260Cu), memRead32(0x80012610u), memRead32(0x80012614u),
                memRead32(0x80012618u), memRead32(0x8001261Cu), memRead32(0x80012620u), memRead32(0x80012624u));
            Console.WriteLn("@@LOOP_BODY_D@@ [80012628]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012628u), memRead32(0x8001262Cu), memRead32(0x80012630u), memRead32(0x80012634u),
                memRead32(0x80012638u), memRead32(0x8001263Cu), memRead32(0x80012640u), memRead32(0x80012644u));
            Console.WriteLn("@@LOOP_BODY_E@@ [80012648]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012648u), memRead32(0x8001264Cu), memRead32(0x80012650u), memRead32(0x80012654u),
                memRead32(0x80012658u), memRead32(0x8001265Cu), memRead32(0x80012660u), memRead32(0x80012664u));
            Console.WriteLn("@@LOOP_BODY_F@@ [80012668]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012668u), memRead32(0x8001266Cu), memRead32(0x80012670u), memRead32(0x80012674u),
                memRead32(0x80012678u), memRead32(0x8001267Cu), memRead32(0x80012680u), memRead32(0x80012684u));
            Console.WriteLn("@@LOOP_BODY_G@@ [80012688]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012688u), memRead32(0x8001268Cu), memRead32(0x80012690u), memRead32(0x80012694u),
                memRead32(0x80012698u), memRead32(0x8001269Cu), memRead32(0x800126A0u), memRead32(0x800126A4u));
            Console.WriteLn("@@LOOP_BODY_H@@ [800126A8]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800126A8u), memRead32(0x800126ACu), memRead32(0x800126B0u), memRead32(0x800126B4u),
                memRead32(0x800126B8u), memRead32(0x800126BCu), memRead32(0x800126C0u), memRead32(0x800126C4u));
        }
    }

    // [TEMP_DIAG] @@IOP_WATCH_179A8_POLL@@ monitor IOP RAM 0x179A8 every vsync (first 30 only)
    if (gsvsync_count <= 30 && iopMem) {
        u32 val = *reinterpret_cast<u32*>(iopMem->Main + 0x179A8);
        if (val != 0 || gsvsync_count <= 5) {
            Console.WriteLn("@@IOP_WATCH_179A8_POLL@@ vsync=%d val=%08x", gsvsync_count, val);
        }
    }

    // [iter230] vsync=60 forceロードdisabled化: BIOS 自然bootを試行。
    // BIOS が DMAC/INTC/SIF を初期化してから LoadExecPS2(OSDSYS) に到達するのを待つ。
    // SYSCALL 0x3c インターセプトは残存（自然到達時のfallback）。
    // Removal condition: 自然bootsuccessafter confirmedに完全delete
    // [iter246] カーネル SIF/DMAC 未初期化のroot cause調査
    // vsync=7 で PC=800125c8 の BNE s2,zero が taken → idle。s2 の値とカーネル分岐ロジックをダンプ。
    // HLE を vsync=50 に遅延させ、カーネル初期化が自然に進むか観察。
    // Removal condition: カーネル SIF/DMAC 初期化のroot causeafter identified
    // [iter251] phys 0x78000 boot parameter block 追跡 (extend版)
    // vsync=1-8 で phys 0x78000-0x784FF のキーaddressをダンプ
    // Removal condition: BIOS ROM → phys 0x78000 ストア経路after confirmed
    if (gsvsync_count >= 1 && gsvsync_count <= 8 && eeMem) {
        u32 p78000 = *reinterpret_cast<u32*>(eeMem->Main + 0x00078000);
        u32 p78004 = *reinterpret_cast<u32*>(eeMem->Main + 0x00078004);
        u32 p78200 = *reinterpret_cast<u32*>(eeMem->Main + 0x00078200);
        u32 p78228 = *reinterpret_cast<u32*>(eeMem->Main + 0x00078228);
        u32 p7822c = *reinterpret_cast<u32*>(eeMem->Main + 0x0007822C);
        u32 p78230 = *reinterpret_cast<u32*>(eeMem->Main + 0x00078230);
        u32 p78240 = *reinterpret_cast<u32*>(eeMem->Main + 0x00078240);
        u32 p78478 = *reinterpret_cast<u32*>(eeMem->Main + 0x00078478);
        u32 p7847c = *reinterpret_cast<u32*>(eeMem->Main + 0x0007847C);
        u32 p78480 = *reinterpret_cast<u32*>(eeMem->Main + 0x00078480);
        Console.WriteLn("@@KERN_PHYS78@@ vsync=%d [0]=%08x [4]=%08x [200]=%08x [228]=%08x [22C]=%08x [230]=%08x [240]=%08x [478]=%08x [47C]=%08x [480]=%08x",
            gsvsync_count, p78000, p78004, p78200, p78228, p7822c, p78230, p78240, p78478, p7847c, p78480);
    }

    // [iter248b] cold boot パッチ + KSEG3 データ初期化
    // カーネル boot parameter block (phys 0x78000) は BIOS ROM ストアの JIT バグにより
    // 未初期化（80005b88/80005ba8 SIGBUS = カスケード障害）。HLE で補完。
    // Removal condition: カーネル JIT バグ群の根本after fixed
    {
        static bool s_kern_init_patched = false;
        // [P9 TEMP_DIAG] interpretermodeでは BIOS が自然に初期化するため JIT 専用パッチdisabled化
        if (!s_kern_init_patched && eeMem && gsvsync_count >= 3 && Cpu != &intCpu && (DarwinMisc::iPSX2_JIT_HLE != 0)) {
            u32* patch_addr = reinterpret_cast<u32*>(eeMem->Main + 0x00014144);
            if (*patch_addr == 0x24040001u) {
                *patch_addr = 0x24040002u;
                Console.WriteLn("@@KERN_INIT_PATCH@@ patched 80014144: a0=1→2 (cold boot)");
            }
            u32* p_78478 = reinterpret_cast<u32*>(eeMem->Main + 0x00078478);
            u32* p_7847C = reinterpret_cast<u32*>(eeMem->Main + 0x0007847C);
            u32* p_78480 = reinterpret_cast<u32*>(eeMem->Main + 0x00078480);
            *p_78478 = 0x00000000u;
            *p_7847C = 0x80000000u;
            *p_78480 = 0x80082000u;
            Console.WriteLn("@@KERN_INIT_PATCH@@ KSEG3 data: [78478]=%08x [7847C]=%08x [78480]=%08x",
                *p_78478, *p_7847C, *p_78480);
            s_kern_init_patched = true;
        }
    }
    if (gsvsync_count >= 3 && gsvsync_count <= 12) {
        u32 ee_pc = cpuRegs.pc;
        u32 ee_s2 = cpuRegs.GPR.r[18].UL[0]; // s2
        u32 ee_s0 = cpuRegs.GPR.r[16].UL[0]; // s0
        u32 ee_a0 = cpuRegs.GPR.r[4].UL[0];  // a0
        u32 ee_ra = cpuRegs.GPR.r[31].UL[0]; // ra
        u32 cop0_status = cpuRegs.CP0.n.Status.val;
        u32 dmac_ctrl = 0, sif_ctrl = 0;
        if (eeMem) {
            dmac_ctrl = *reinterpret_cast<u32*>(eeMem->Main + 0x1000E000 - 0x10000000 + 0); // DMAC_CTRL at HW 0x1000E000
        }
        // Read DMAC_CTRL from HW register directly
        dmac_ctrl = psHu32(0xE000); // DMAC_CTRL
        sif_ctrl = psHu32(0xF240); // SIF_CTRL
        // Key kernel data addresses:
        // [80023E24] = thread queue head (0 → skip init at 800125FC)
        // [800242E0] = module count (0 → 80012030 skips thread registration)
        // [800242F8] = event flag
        u32 init_flag = eeMem ? memRead32(0x80023E24) : 0xDEAD;
        u32 mod_count = eeMem ? memRead32(0x800242E0) : 0xDEAD;
        u32 kern_42f8 = eeMem ? memRead32(0x800242F8) : 0xDEAD;
        u32 mod_cap = eeMem ? memRead32(0x800242E4) : 0xDEAD;
        { u32 cb0=eeMem?memRead32(0x1586D8u):0; u32 cb1=cb0?memRead32(cb0):0;
        Console.WriteLn("@@KERN_TRACE@@ vsync=%d pc=%08x s2=%08x s1=%08x st=%08x dmac=%08x sif=%08x [3E24]=%08x [42E0]=%08x [42E4]=%08x [42F8]=%08x d2chcr=%08x cb=%08x->%08x",
            gsvsync_count, ee_pc, ee_s2, cpuRegs.GPR.r[17].UL[0], cop0_status, dmac_ctrl, sif_ctrl, init_flag, mod_count, mod_cap, kern_42f8, eeMem ? memRead32(0x10008000u) : 0xDEADu, cb0, cb1); }
        // vsync=8 でカーネルコードをダンプ (800125C0, 分岐先, idle, 初期化function)
        if (gsvsync_count == 8 && eeMem) {
            // 8000db78 (ra after memclear returns) - what happens after memory clear
            Console.WriteLn("@@KERN_CODE@@ 8000db60: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x8000db60), memRead32(0x8000db64), memRead32(0x8000db68), memRead32(0x8000db6c),
                memRead32(0x8000db70), memRead32(0x8000db74), memRead32(0x8000db78), memRead32(0x8000db7c));
            Console.WriteLn("@@KERN_CODE@@ 8000db80: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x8000db80), memRead32(0x8000db84), memRead32(0x8000db88), memRead32(0x8000db8c),
                memRead32(0x8000db90), memRead32(0x8000db94), memRead32(0x8000db98), memRead32(0x8000db9c));
            Console.WriteLn("@@KERN_CODE@@ 8000dba0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x8000dba0), memRead32(0x8000dba4), memRead32(0x8000dba8), memRead32(0x8000dbac),
                memRead32(0x8000dbb0), memRead32(0x8000dbb4), memRead32(0x8000dbb8), memRead32(0x8000dbbc));
            Console.WriteLn("@@KERN_CODE@@ 8000dbc0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x8000dbc0), memRead32(0x8000dbc4), memRead32(0x8000dbc8), memRead32(0x8000dbcc),
                memRead32(0x8000dbd0), memRead32(0x8000dbd4), memRead32(0x8000dbd8), memRead32(0x8000dbdc));
            // 0x80012030 function (カーネル初期化functionのエントリ)
            Console.WriteLn("@@KERN_FUNC@@ 80012030: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012030), memRead32(0x80012034), memRead32(0x80012038), memRead32(0x8001203c),
                memRead32(0x80012040), memRead32(0x80012044), memRead32(0x80012048), memRead32(0x8001204c));
            Console.WriteLn("@@KERN_FUNC@@ 80012050: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012050), memRead32(0x80012054), memRead32(0x80012058), memRead32(0x8001205c),
                memRead32(0x80012060), memRead32(0x80012064), memRead32(0x80012068), memRead32(0x8001206c));
            Console.WriteLn("@@KERN_FUNC@@ 80012070: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012070), memRead32(0x80012074), memRead32(0x80012078), memRead32(0x8001207c),
                memRead32(0x80012080), memRead32(0x80012084), memRead32(0x80012088), memRead32(0x8001208c));
            Console.WriteLn("@@KERN_FUNC@@ 80012090: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012090), memRead32(0x80012094), memRead32(0x80012098), memRead32(0x8001209c),
                memRead32(0x800120a0), memRead32(0x800120a4), memRead32(0x800120a8), memRead32(0x800120ac));
            // 初期化パス 800125E0 - 80012660 (BNE 不成立時の実行パス)
            Console.WriteLn("@@KERN_INIT_PATH@@ 800125e0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800125e0), memRead32(0x800125e4), memRead32(0x800125e8), memRead32(0x800125ec),
                memRead32(0x800125f0), memRead32(0x800125f4), memRead32(0x800125f8), memRead32(0x800125fc));
            Console.WriteLn("@@KERN_INIT_PATH@@ 80012600: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012600), memRead32(0x80012604), memRead32(0x80012608), memRead32(0x8001260c),
                memRead32(0x80012610), memRead32(0x80012614), memRead32(0x80012618), memRead32(0x8001261c));
            Console.WriteLn("@@KERN_INIT_PATH@@ 80012620: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012620), memRead32(0x80012624), memRead32(0x80012628), memRead32(0x8001262c),
                memRead32(0x80012630), memRead32(0x80012634), memRead32(0x80012638), memRead32(0x8001263c));
            Console.WriteLn("@@KERN_INIT_PATH@@ 80012640: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012640), memRead32(0x80012644), memRead32(0x80012648), memRead32(0x8001264c),
                memRead32(0x80012650), memRead32(0x80012654), memRead32(0x80012658), memRead32(0x8001265c));
            // 800125c8 の BNE 分岐先: 800125cc + 0x28*4 = 8001266C
            Console.WriteLn("@@KERN_CODE@@ 800125a0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800125a0), memRead32(0x800125a4), memRead32(0x800125a8), memRead32(0x800125ac),
                memRead32(0x800125b0), memRead32(0x800125b4), memRead32(0x800125b8), memRead32(0x800125bc));
            Console.WriteLn("@@KERN_CODE@@ 800125c0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800125c0), memRead32(0x800125c4), memRead32(0x800125c8), memRead32(0x800125cc),
                memRead32(0x800125d0), memRead32(0x800125d4), memRead32(0x800125d8), memRead32(0x800125dc));
            // BNE target: 8001266C
            Console.WriteLn("@@KERN_CODE@@ 80012660: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012660), memRead32(0x80012664), memRead32(0x80012668), memRead32(0x8001266c),
                memRead32(0x80012670), memRead32(0x80012674), memRead32(0x80012678), memRead32(0x8001267c));
            Console.WriteLn("@@KERN_CODE@@ 80012680: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012680), memRead32(0x80012684), memRead32(0x80012688), memRead32(0x8001268c),
                memRead32(0x80012690), memRead32(0x80012694), memRead32(0x80012698), memRead32(0x8001269c));
            // idle loop
            Console.WriteLn("@@KERN_CODE@@ 8000fe70: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x8000fe70), memRead32(0x8000fe74), memRead32(0x8000fe78), memRead32(0x8000fe7c),
                memRead32(0x8000fe80), memRead32(0x8000fe84), memRead32(0x8000fe88), memRead32(0x8000fe8c));
            // s2 がconfigされる場所を探すため、800125c8 以前のコードをさらに
            Console.WriteLn("@@KERN_CODE@@ 80012580: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012580), memRead32(0x80012584), memRead32(0x80012588), memRead32(0x8001258c),
                memRead32(0x80012590), memRead32(0x80012594), memRead32(0x80012598), memRead32(0x8001259c));
            // call元 (ra=800126cc 周辺)
            Console.WriteLn("@@KERN_CODE@@ 800126c0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800126c0), memRead32(0x800126c4), memRead32(0x800126c8), memRead32(0x800126cc),
                memRead32(0x800126d0), memRead32(0x800126d4), memRead32(0x800126d8), memRead32(0x800126dc));
            // 80020000 周辺のカーネルデータ (s2 が参照する可能性)
            Console.WriteLn("@@KERN_DATA@@ 80024100: %08x %08x %08x %08x 800242f8: %08x 80024108: %08x 80024114: %08x",
                memRead32(0x80024100), memRead32(0x80024104), memRead32(0x80024108), memRead32(0x8002410c),
                memRead32(0x800242f8), memRead32(0x80024108), memRead32(0x80024114));

            // [iter247d] call元 80014100-80014160 ダンプ (a0 configコード)
            Console.WriteLn("@@KERN_CODE@@ 80014100: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80014100), memRead32(0x80014104), memRead32(0x80014108), memRead32(0x8001410c),
                memRead32(0x80014110), memRead32(0x80014114), memRead32(0x80014118), memRead32(0x8001411c));
            Console.WriteLn("@@KERN_CODE@@ 80014120: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80014120), memRead32(0x80014124), memRead32(0x80014128), memRead32(0x8001412c),
                memRead32(0x80014130), memRead32(0x80014134), memRead32(0x80014138), memRead32(0x8001413c));
            Console.WriteLn("@@KERN_CODE@@ 80014140: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80014140), memRead32(0x80014144), memRead32(0x80014148), memRead32(0x8001414c),
                memRead32(0x80014150), memRead32(0x80014154), memRead32(0x80014158), memRead32(0x8001415c));
            // [iter247e] KSEG3 TLB mapping検証 - 0xFFFF8000 周辺の読み取り値
            {
                u32 kseg3_val_0000 = memRead32(0xFFFF8000);
                u32 kseg3_val_0228 = memRead32(0xFFFF8228);
                u32 kseg3_val_022c = memRead32(0xFFFF822C);
                u32 kseg3_val_0230 = memRead32(0xFFFF8230);
                u32 kseg3_val_0240 = memRead32(0xFFFF8240);
                u32 kseg3_ptr_3ffc = memRead32(0x80023FFC); // kernel data ptr
                Console.WriteLn("@@KERN_KSEG3@@ [FFFF8000]=%08x [FFFF8228]=%08x [FFFF822C]=%08x [FFFF8230]=%08x [FFFF8240]=%08x [80023FFC]=%08x",
                    kseg3_val_0000, kseg3_val_0228, kseg3_val_022c, kseg3_val_0230, kseg3_val_0240, kseg3_ptr_3ffc);
            }
            // [iter247e] COP0 TLB エントリダンプ (最初の数エントリ)
            {
                for (int ti = 0; ti < 8; ti++) {
                    auto& t = tlb[ti];
                    Console.WriteLn("@@KERN_TLB@@ [%d] mask=%08x hi=%08x lo0=%08x lo1=%08x vpn2=%08x pfn0=%08x pfn1=%08x",
                        ti, t.PageMask.UL, t.EntryHi.UL, t.EntryLo0.UL, t.EntryLo1.UL,
                        t.VPN2(), t.PFN0(), t.PFN1());
                }
            }
            // [iter247d] さらに上流のfunction開始点を探す
            Console.WriteLn("@@KERN_CODE@@ 800140c0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800140c0), memRead32(0x800140c4), memRead32(0x800140c8), memRead32(0x800140cc),
                memRead32(0x800140d0), memRead32(0x800140d4), memRead32(0x800140d8), memRead32(0x800140dc));
            Console.WriteLn("@@KERN_CODE@@ 800140e0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800140e0), memRead32(0x800140e4), memRead32(0x800140e8), memRead32(0x800140ec),
                memRead32(0x800140f0), memRead32(0x800140f4), memRead32(0x800140f8), memRead32(0x800140fc));

            // [iter247c] カーネルデータ初期化function 800119e8-80011a30 + 80012bb0-80012c30 ダンプ
            Console.WriteLn("@@KERN_CODE@@ 800119e0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800119e0), memRead32(0x800119e4), memRead32(0x800119e8), memRead32(0x800119ec),
                memRead32(0x800119f0), memRead32(0x800119f4), memRead32(0x800119f8), memRead32(0x800119fc));
            Console.WriteLn("@@KERN_CODE@@ 80011a00: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80011a00), memRead32(0x80011a04), memRead32(0x80011a08), memRead32(0x80011a0c),
                memRead32(0x80011a10), memRead32(0x80011a14), memRead32(0x80011a18), memRead32(0x80011a1c));
            Console.WriteLn("@@KERN_CODE@@ 80011a20: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80011a20), memRead32(0x80011a24), memRead32(0x80011a28), memRead32(0x80011a2c),
                memRead32(0x80011a30), memRead32(0x80011a34), memRead32(0x80011a38), memRead32(0x80011a3c));
            Console.WriteLn("@@KERN_CODE@@ 80012bb0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012bb0), memRead32(0x80012bb4), memRead32(0x80012bb8), memRead32(0x80012bbc),
                memRead32(0x80012bc0), memRead32(0x80012bc4), memRead32(0x80012bc8), memRead32(0x80012bcc));
            Console.WriteLn("@@KERN_CODE@@ 80012bd0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012bd0), memRead32(0x80012bd4), memRead32(0x80012bd8), memRead32(0x80012bdc),
                memRead32(0x80012be0), memRead32(0x80012be4), memRead32(0x80012be8), memRead32(0x80012bec));
            Console.WriteLn("@@KERN_CODE@@ 80012bf0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012bf0), memRead32(0x80012bf4), memRead32(0x80012bf8), memRead32(0x80012bfc),
                memRead32(0x80012c00), memRead32(0x80012c04), memRead32(0x80012c08), memRead32(0x80012c0c));
            Console.WriteLn("@@KERN_CODE@@ 80012c10: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012c10), memRead32(0x80012c14), memRead32(0x80012c18), memRead32(0x80012c1c),
                memRead32(0x80012c20), memRead32(0x80012c24), memRead32(0x80012c28), memRead32(0x80012c2c));

            // [iter247b] RegisterModule 内部コード 800120B0-80012110 ダンプ
            // [42E0] が 0 のまま → このrangeに increment store があるはず
            Console.WriteLn("@@KERN_CODE@@ 800120b0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800120b0), memRead32(0x800120b4), memRead32(0x800120b8), memRead32(0x800120bc),
                memRead32(0x800120c0), memRead32(0x800120c4), memRead32(0x800120c8), memRead32(0x800120cc));
            Console.WriteLn("@@KERN_CODE@@ 800120d0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800120d0), memRead32(0x800120d4), memRead32(0x800120d8), memRead32(0x800120dc),
                memRead32(0x800120e0), memRead32(0x800120e4), memRead32(0x800120e8), memRead32(0x800120ec));
            Console.WriteLn("@@KERN_CODE@@ 800120f0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800120f0), memRead32(0x800120f4), memRead32(0x800120f8), memRead32(0x800120fc),
                memRead32(0x80012100), memRead32(0x80012104), memRead32(0x80012108), memRead32(0x8001210c));

            // [iter247] 0x8000E588 (memclear 後に呼ばれるfunction) のコードダンプ
            Console.WriteLn("@@KERN_CODE@@ 8000e580: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x8000e580), memRead32(0x8000e584), memRead32(0x8000e588), memRead32(0x8000e58c),
                memRead32(0x8000e590), memRead32(0x8000e594), memRead32(0x8000e598), memRead32(0x8000e59c));
            Console.WriteLn("@@KERN_CODE@@ 8000e5a0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x8000e5a0), memRead32(0x8000e5a4), memRead32(0x8000e5a8), memRead32(0x8000e5ac),
                memRead32(0x8000e5b0), memRead32(0x8000e5b4), memRead32(0x8000e5b8), memRead32(0x8000e5bc));
            // [iter247] デバッグ文字列ダンプ (memclear関連 print 引数)
            {
                char str1[33]={}, str2[33]={}, str3[33]={};
                for (int i=0; i<32; i++) { str1[i] = (char)memRead8(0x80016270+i); str2[i] = (char)memRead8(0x80016290+i); str3[i] = (char)memRead8(0x800162B0+i); }
                Console.WriteLn("@@KERN_STR@@ [80016270]='%.32s' [80016290]='%.32s' [800162B0]='%.32s'", str1, str2, str3);
            }

            // [iter250] @@KERN_MODDATA@@ モジュール記述子 + helper コード + init ストアターゲット
            // helper 800142dc の引数 a0=800231b0 のデータと 800142dc-80014320 のコード
            // Removal condition: カーネル初期化完全behavior後
            Console.WriteLn("@@KERN_MODDATA@@ [800231a0]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800231a0), memRead32(0x800231a4), memRead32(0x800231a8), memRead32(0x800231ac),
                memRead32(0x800231b0), memRead32(0x800231b4), memRead32(0x800231b8), memRead32(0x800231bc));
            Console.WriteLn("@@KERN_MODDATA@@ [800231c0]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800231c0), memRead32(0x800231c4), memRead32(0x800231c8), memRead32(0x800231cc),
                memRead32(0x800231d0), memRead32(0x800231d4), memRead32(0x800231d8), memRead32(0x800231dc));
            // 800242C0-800242F0 module table area
            Console.WriteLn("@@KERN_MODTBL@@ [800242C0]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800242C0), memRead32(0x800242C4), memRead32(0x800242C8), memRead32(0x800242CC),
                memRead32(0x800242D0), memRead32(0x800242D4), memRead32(0x800242D8), memRead32(0x800242DC));
            Console.WriteLn("@@KERN_MODTBL@@ [800242E0]: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800242E0), memRead32(0x800242E4), memRead32(0x800242E8), memRead32(0x800242EC),
                memRead32(0x800242F0), memRead32(0x800242F4), memRead32(0x800242F8), memRead32(0x800242FC));
            // helper 800142dc code
            Console.WriteLn("@@KERN_CODE@@ 800142d0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800142d0), memRead32(0x800142d4), memRead32(0x800142d8), memRead32(0x800142dc),
                memRead32(0x800142e0), memRead32(0x800142e4), memRead32(0x800142e8), memRead32(0x800142ec));
            Console.WriteLn("@@KERN_CODE@@ 800142f0: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x800142f0), memRead32(0x800142f4), memRead32(0x800142f8), memRead32(0x800142fc),
                memRead32(0x80014300), memRead32(0x80014304), memRead32(0x80014308), memRead32(0x8001430c));
            Console.WriteLn("@@KERN_CODE@@ 80014310: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80014310), memRead32(0x80014314), memRead32(0x80014318), memRead32(0x8001431c),
                memRead32(0x80014320), memRead32(0x80014324), memRead32(0x80014328), memRead32(0x8001432c));
            // init function 80012c30-80012c60 (post-alloc stores)
            Console.WriteLn("@@KERN_CODE@@ 80012c30: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012c30), memRead32(0x80012c34), memRead32(0x80012c38), memRead32(0x80012c3c),
                memRead32(0x80012c40), memRead32(0x80012c44), memRead32(0x80012c48), memRead32(0x80012c4c));
            Console.WriteLn("@@KERN_CODE@@ 80012c50: %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80012c50), memRead32(0x80012c54), memRead32(0x80012c58), memRead32(0x80012c5c),
                memRead32(0x80012c60), memRead32(0x80012c64), memRead32(0x80012c68), memRead32(0x80012c6c));
        }
        // [P9 TEMP_DIAG] @@KERN_CAUSE9@@ vsync=9 one-shot: CP0.Cause + BadVAddr + physical 0x82000-0x8207F
        // 目的: vsync=8→9 で EE PC が 0x82018→0x00000000 に遷移したcauseの例外種別と物理コードをverify
        // Removal condition: EE カーネル init 完了 (EELOAD ロードverify) 後
        if (gsvsync_count == 9 && eeMem) {
            const u32 cause  = cpuRegs.CP0.r[13]; // CP0.Cause
            const u32 badvad = cpuRegs.CP0.r[8];  // CP0.BadVAddr
            const u32 exc    = (cause >> 2) & 0x1Fu;
            Console.WriteLn("@@KERN_CAUSE9@@ cause=%08x ExcCode=%u badvad=%08x epc=%08x status=%08x",
                cause, exc, badvad, cpuRegs.CP0.r[14], cpuRegs.CP0.r[12]);
            // Physical EE RAM 0x82000-0x8207F via KSEG0 (direct map, no TLB)
            Console.WriteLn("@@KERN_PHYS82@@ [80082000] %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80082000u), memRead32(0x80082004u), memRead32(0x80082008u), memRead32(0x8008200cu),
                memRead32(0x80082010u), memRead32(0x80082014u), memRead32(0x80082018u), memRead32(0x8008201cu));
            Console.WriteLn("@@KERN_PHYS82@@ [80082020] %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80082020u), memRead32(0x80082024u), memRead32(0x80082028u), memRead32(0x8008202cu),
                memRead32(0x80082030u), memRead32(0x80082034u), memRead32(0x80082038u), memRead32(0x8008203cu));
            Console.WriteLn("@@KERN_PHYS82@@ [80082040] %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80082040u), memRead32(0x80082044u), memRead32(0x80082048u), memRead32(0x8008204cu),
                memRead32(0x80082050u), memRead32(0x80082054u), memRead32(0x80082058u), memRead32(0x8008205cu));
            Console.WriteLn("@@KERN_PHYS82@@ [80082060] %08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x80082060u), memRead32(0x80082064u), memRead32(0x80082068u), memRead32(0x8008206cu),
                memRead32(0x80082070u), memRead32(0x80082074u), memRead32(0x80082078u), memRead32(0x8008207cu));
        }
    }

    // [iter653] @@EELOAD_PHYS_TIMELINE@@ — vsync=5,6 で物理memory 0x82000 の変化を追跡。
    // addressパターンがいつ書き込まれるか特定。
    // Removal condition: EELOAD ロードissue解決後
    if ((gsvsync_count == 5 || gsvsync_count == 6) && eeMem) {
        const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
        Console.WriteLn("@@EELOAD_PHYS_TIMELINE@@ [%s] vsync=%d phys82000=%08x phys82004=%08x phys82008=%08x phys82060=%08x",
            mode, gsvsync_count,
            *(u32*)(eeMem->Main + 0x82000), *(u32*)(eeMem->Main + 0x82004),
            *(u32*)(eeMem->Main + 0x82008), *(u32*)(eeMem->Main + 0x82060));
    }

    // [iter655] @@SIF_LATE_INJECT@@ — PS1DRV 初期化after completedに割り込みenabled化 + SIF ハンドシェイク注入。
    // ExecPS2 HLE で Status=0x70000010 (IE=0) にして OSDSYS をbootし、
    // カーネル SIF ポーリングloop (0x8000fe74) を回避する。
    // vsync=20 で PS1DRV outer loop が確立した後に Status を正しい値に戻し、SIF 注入を行う。
    // Removal condition: IOP sifman/sifcmd が自然に SIF ハンドシェイクを完了するようになった後
    // [iter655] @@SIF_LATE_INJECT@@ — 割り込みenabled化前にhandlerテーブルをダンプ。
    // vsync=20 でhandlerテーブル [0x80001000] をダンプし、enabled性をverify。
    // enabledなら IE=1 にして割り込みをenabled化する。disabledなら IE=0 のまま。
    // Removal condition: JIT 割り込みhandlerissue解決後
    // [iter655] @@KERN_INT0_DUMP@@ — INT0 handler (0x800004c0-0x80000e00) をダンプ。
    // JIT で IE=1 enabled化時に INT0 handler内の 0x80000dc0 で BREAK する。
    // Interpreter と同じコードか比較するためにダンプ。
    // Removal condition: INT0 handlerissue解決後
    if (gsvsync_count == 25 && eeMem) {
        static bool s_int0_dump_done = false;
        if (!s_int0_dump_done) {
            s_int0_dump_done = true;
            const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
            // 0x800004c0-0x80000500 (INT0 handler先頭)
            for (u32 base = 0x4c0; base <= 0x540; base += 0x20) {
                Console.WriteLn("@@KERN_INT0_DUMP@@ [%s] 8000%04x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, base,
                    *(u32*)(eeMem->Main + base), *(u32*)(eeMem->Main + base+4),
                    *(u32*)(eeMem->Main + base+8), *(u32*)(eeMem->Main + base+12),
                    *(u32*)(eeMem->Main + base+16), *(u32*)(eeMem->Main + base+20),
                    *(u32*)(eeMem->Main + base+24), *(u32*)(eeMem->Main + base+28));
            }
            // 0x80000d80-0x80000e20 (BREAK 付近)
            for (u32 base = 0xd80; base <= 0xe20; base += 0x20) {
                Console.WriteLn("@@KERN_INT0_DUMP@@ [%s] 8000%04x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, base,
                    *(u32*)(eeMem->Main + base), *(u32*)(eeMem->Main + base+4),
                    *(u32*)(eeMem->Main + base+8), *(u32*)(eeMem->Main + base+12),
                    *(u32*)(eeMem->Main + base+16), *(u32*)(eeMem->Main + base+20),
                    *(u32*)(eeMem->Main + base+24), *(u32*)(eeMem->Main + base+28));
            }
        }
    }
    // [iter655] @@EXCVEC_WATCH@@ — vsync=7-20 で 0x220 の値を監視
    if (gsvsync_count >= 7 && gsvsync_count <= 20 && eeMem) {
        const u32 v220 = *(u32*)(eeMem->Main + 0x220);
        static u32 s_prev_220 = 0;
        if (v220 != s_prev_220) {
            Console.WriteLn("@@EXCVEC_WATCH@@ vsync=%d [0x220] CHANGED: %08x → %08x  pc=%08x",
                gsvsync_count, s_prev_220, v220, cpuRegs.pc);
            s_prev_220 = v220;
        }
    }
    // [iter655] @@EXCVEC_VERIFY@@ — vsync=20 で例外ベクタが正しくconfigされたかverify
    if (gsvsync_count == 20 && eeMem) {
        static bool s_excvec_done = false;
        if (!s_excvec_done) {
            s_excvec_done = true;
            const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
            for (u32 off = 0x200; off <= 0x280; off += 0x20) {
                Console.WriteLn("@@EXCVEC_DUMP@@ [%s] [0x%03x]=%08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, off,
                    *(u32*)(eeMem->Main + off), *(u32*)(eeMem->Main + off+4),
                    *(u32*)(eeMem->Main + off+8), *(u32*)(eeMem->Main + off+12),
                    *(u32*)(eeMem->Main + off+16), *(u32*)(eeMem->Main + off+20),
                    *(u32*)(eeMem->Main + off+24), *(u32*)(eeMem->Main + off+28));
            }
            Console.WriteLn("@@INT_PRE_STATE@@ [%s] INTC_STAT=%08x INTC_MASK=%08x status=%08x pc=%08x",
                mode, psHu32(INTC_STAT), psHu32(INTC_MASK), cpuRegs.CP0.n.Status.val, cpuRegs.pc);
            // @@INT_DISPATCH_TABLE@@ — 割り込みdispatchテーブル 0x80015380 をダンプ（両mode共通）
            for (u32 off = 0x15380; off <= 0x153C0; off += 0x10) {
                Console.WriteLn("@@INT_DISPATCH_TABLE@@ [%s] [0x%05x]=%08x %08x %08x %08x",
                    mode, off,
                    *(u32*)(eeMem->Main + off), *(u32*)(eeMem->Main + off+4),
                    *(u32*)(eeMem->Main + off+8), *(u32*)(eeMem->Main + off+12));
            }
        }
    }

    // [iter653] @@ROMDIR_EELOAD_SCAN@@ — BIOS ROM から EELOAD エントリを検索。
    // BiosTools.cpp と同じロジック: "RESET" を先に見つけて ROMDIR テーブル先頭を特定。
    // Removal condition: EELOAD ロードissue解決後
    if (gsvsync_count == 3 && eeMem) {
        static bool s_romdir_done = false;
        if (!s_romdir_done) {
            s_romdir_done = true;
            const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
            const u8* rom = eeMem->ROM;
            const u32 romsize = 0x400000u; // 4MB
            // Step 1: Find "RESET" entry (start of ROMDIR table)
            u32 dir_offset = 0xFFFFFFFFu;
            for (u32 i = 0; i + 16 <= romsize; i++) {
                if (strncmp((const char*)(rom + i), "RESET", 5) == 0 && rom[i+5] == 0) {
                    dir_offset = i;
                    break;
                }
            }
            if (dir_offset == 0xFFFFFFFFu) {
                Console.WriteLn("@@ROMDIR_EELOAD_SCAN@@ [%s] RESET not found!", mode);
            } else {
                Console.WriteLn("@@ROMDIR_EELOAD_SCAN@@ [%s] RESET at offset %08x", mode, dir_offset);
                // Step 2: Iterate entries to find EELOAD
                u32 file_offset = 0;
                bool found = false;
                for (u32 n = 0; n < 512; n++) {
                    u32 entry_off = dir_offset + n * 16;
                    if (entry_off + 16 > romsize) break;
                    const char* name = (const char*)(rom + entry_off);
                    if (name[0] == 0) break;
                    u32 file_sz = *(u32*)(rom + entry_off + 12);
                    // Log first 5 entries + any entry containing "EELOAD" in the name
                    bool is_eeload = (strncmp(name, "EELOAD", 7) == 0); // exact match: "EELOAD\0"
                    if (n < 5 || strstr(name, "EELOAD") != nullptr) {
                        Console.WriteLn("@@ROMDIR_ENTRY@@ [%s] n=%u name='%.10s' foff=%08x fsz=%08x exact=%d",
                            mode, n, name, file_offset, file_sz, is_eeload ? 1 : 0);
                    }
                    if (is_eeload) {
                        // Dump first 32 bytes of EELOAD in ROM
                        if (file_offset + 32 <= romsize) {
                            const u32* d = (const u32*)(rom + file_offset);
                            Console.WriteLn("@@ROMDIR_EELOAD_DATA@@ [%s] ROM+%08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                                mode, file_offset, d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]);
                        }
                        found = true;
                        break;
                    }
                    file_offset += (file_sz + 15) & ~15u;
                }
                if (!found) Console.WriteLn("@@ROMDIR_EELOAD_SCAN@@ [%s] EELOAD not found in ROMDIR!", mode);
            }
        }
    }

    // [iter653] @@EELOAD_PHYS_DUMP@@ — vsync=7 で物理 EE RAM 0x82000-0x8207F をダンプ。
    // JIT vs Interpreter で EELOAD の実体を直接比較。memRead32(KUSEG) は TLB 依存で信頼できない。
    // Removal condition: JIT EELOAD ロードissueのcauseafter identified
    if (gsvsync_count == 7 && eeMem) {
        static bool s_phys_dump_done = false;
        if (!s_phys_dump_done) {
            s_phys_dump_done = true;
            const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
            u32 w[32] = {};
            for (int i = 0; i < 32; i++) w[i] = *(u32*)(eeMem->Main + 0x82000 + i*4);
            Console.WriteLn("@@EELOAD_PHYS_DUMP@@ [%s] vsync=7 phys 00082000: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode, w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7]);
            Console.WriteLn("@@EELOAD_PHYS_DUMP@@ [%s] vsync=7 phys 00082020: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode, w[8],w[9],w[10],w[11],w[12],w[13],w[14],w[15]);
            Console.WriteLn("@@EELOAD_PHYS_DUMP@@ [%s] vsync=7 phys 00082040: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode, w[16],w[17],w[18],w[19],w[20],w[21],w[22],w[23]);
            Console.WriteLn("@@EELOAD_PHYS_DUMP@@ [%s] vsync=7 phys 00082060: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode, w[24],w[25],w[26],w[27],w[28],w[29],w[30],w[31]);
            // [iter656] @@EELOAD_ENTRY_DUMP@@ — EELOAD エントリポイント 0x82180-0x82280 ダンプ
            // 分岐determineコードを解読し、JIT が FlushCache をskipするcauseを特定
            // Removal condition: EELOAD 分岐パス分岐causeafter identified
            // [iter657] 0x82080-0x82180 もadd: ra=0x8208c FlushCache call元の解読
            // [iter659] 0x83840-0x838A0 add: FlushCache wrapper function at JAL target 0x83860
            for (u32 off = 0x83840; off < 0x838a0; off += 0x20) {
                const u32 p = off & 0x01FFFFFFu;
                Console.WriteLn("@@EELOAD_FLUSH_DUMP@@ [%s] %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, off,
                    *(u32*)(eeMem->Main + p), *(u32*)(eeMem->Main + p+4),
                    *(u32*)(eeMem->Main + p+8), *(u32*)(eeMem->Main + p+12),
                    *(u32*)(eeMem->Main + p+16), *(u32*)(eeMem->Main + p+20),
                    *(u32*)(eeMem->Main + p+24), *(u32*)(eeMem->Main + p+28));
            }
            // 0x820A0 付近: 0x82084 の JAL 戻り先以降のコード
            for (u32 off = 0x82080; off < 0x82280; off += 0x20) {
                const u32 p = off & 0x01FFFFFFu;
                Console.WriteLn("@@EELOAD_ENTRY_DUMP@@ [%s] %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, off,
                    *(u32*)(eeMem->Main + p), *(u32*)(eeMem->Main + p+4),
                    *(u32*)(eeMem->Main + p+8), *(u32*)(eeMem->Main + p+12),
                    *(u32*)(eeMem->Main + p+16), *(u32*)(eeMem->Main + p+20),
                    *(u32*)(eeMem->Main + p+24), *(u32*)(eeMem->Main + p+28));
            }
            // [iter657] @@EELOAD_RETPATH_DUMP@@ — 0x82540-0x82640 (ra=0x82574 戻り先コード)
            // poll function 0x82180 から戻った後のコードパスを解読
            // Removal condition: EELOAD 分岐パス分岐causeafter identified
            for (u32 off = 0x82540; off < 0x82640; off += 0x20) {
                const u32 p = off & 0x01FFFFFFu;
                Console.WriteLn("@@EELOAD_RETPATH_DUMP@@ [%s] %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, off,
                    *(u32*)(eeMem->Main + p), *(u32*)(eeMem->Main + p+4),
                    *(u32*)(eeMem->Main + p+8), *(u32*)(eeMem->Main + p+12),
                    *(u32*)(eeMem->Main + p+16), *(u32*)(eeMem->Main + p+20),
                    *(u32*)(eeMem->Main + p+24), *(u32*)(eeMem->Main + p+28));
            }
            // [iter675] @@EELOAD_83DF0_BODY@@ — 0x83DF0 loop function body
            for (u32 off = 0x83DF0; off < 0x83F00; off += 0x20) {
                const u32 p = off & 0x01FFFFFFu;
                Console.WriteLn("@@EELOAD_83DF0_BODY@@ [%s] %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, off,
                    *(u32*)(eeMem->Main + p), *(u32*)(eeMem->Main + p+4),
                    *(u32*)(eeMem->Main + p+8), *(u32*)(eeMem->Main + p+12),
                    *(u32*)(eeMem->Main + p+16), *(u32*)(eeMem->Main + p+20),
                    *(u32*)(eeMem->Main + p+24), *(u32*)(eeMem->Main + p+28));
            }
            // [iter676c] MIPS パッチ無し — JIT compileパス診断のみ
            // [iter674] @@EELOAD_SIFCALLER_DUMP@@ — sceSifGetReg 戻り先 0x84280-0x84300 + sceSifSetDma caller 0x83F00-0x84060
            for (u32 off = 0x83F00; off < 0x84060; off += 0x20) {
                const u32 p = off & 0x01FFFFFFu;
                Console.WriteLn("@@EELOAD_SIFCALLER_DUMP@@ [%s] %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, off,
                    *(u32*)(eeMem->Main + p), *(u32*)(eeMem->Main + p+4),
                    *(u32*)(eeMem->Main + p+8), *(u32*)(eeMem->Main + p+12),
                    *(u32*)(eeMem->Main + p+16), *(u32*)(eeMem->Main + p+20),
                    *(u32*)(eeMem->Main + p+24), *(u32*)(eeMem->Main + p+28));
            }
            // [iter674] @@EELOAD_83FA0_TAIL@@ — 83FA0 functionの残り (84060-840C0 + 83EC0-83F00)
            for (u32 off = 0x83EC0; off < 0x83F00; off += 0x20) {
                const u32 p = off & 0x01FFFFFFu;
                Console.WriteLn("@@EELOAD_83FA0_TAIL@@ [%s] %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, off,
                    *(u32*)(eeMem->Main + p), *(u32*)(eeMem->Main + p+4),
                    *(u32*)(eeMem->Main + p+8), *(u32*)(eeMem->Main + p+12),
                    *(u32*)(eeMem->Main + p+16), *(u32*)(eeMem->Main + p+20),
                    *(u32*)(eeMem->Main + p+24), *(u32*)(eeMem->Main + p+28));
            }
            for (u32 off = 0x84060; off < 0x840E0; off += 0x20) {
                const u32 p = off & 0x01FFFFFFu;
                Console.WriteLn("@@EELOAD_83FA0_TAIL@@ [%s] %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, off,
                    *(u32*)(eeMem->Main + p), *(u32*)(eeMem->Main + p+4),
                    *(u32*)(eeMem->Main + p+8), *(u32*)(eeMem->Main + p+12),
                    *(u32*)(eeMem->Main + p+16), *(u32*)(eeMem->Main + p+20),
                    *(u32*)(eeMem->Main + p+24), *(u32*)(eeMem->Main + p+28));
            }
            for (u32 off = 0x84260; off < 0x84320; off += 0x20) {
                const u32 p = off & 0x01FFFFFFu;
                Console.WriteLn("@@EELOAD_SIFGETREG_RET@@ [%s] %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, off,
                    *(u32*)(eeMem->Main + p), *(u32*)(eeMem->Main + p+4),
                    *(u32*)(eeMem->Main + p+8), *(u32*)(eeMem->Main + p+12),
                    *(u32*)(eeMem->Main + p+16), *(u32*)(eeMem->Main + p+20),
                    *(u32*)(eeMem->Main + p+24), *(u32*)(eeMem->Main + p+28));
            }
            // SBUS_F230 比較
            Console.WriteLn("@@EELOAD_SIF_STATE@@ [%s] F220=%08x F230=%08x F240=%08x INTC_STAT=%08x INTC_MASK=%08x D_CTRL=%08x",
                mode, psHu32(0xF220), psHu32(0xF230), psHu32(0xF240),
                psHu32(INTC_STAT), psHu32(INTC_MASK), psHu32(0xE000));
            // [iter674] @@SIF_RPC_BUF@@ — SIF RPC buffer比較 (83FCC 分岐condition)
            // 0x93698 が指すaddressの先頭バイトが IOP 応答データ
            {
                const u32 ptr_addr = 0x93698u & 0x01FFFFFFu;
                const u32 buf_ptr = *(u32*)(eeMem->Main + ptr_addr);
                Console.WriteLn("@@SIF_RPC_BUF@@ [%s] MEM[93698]=%08x", mode, buf_ptr);
                if (buf_ptr && buf_ptr < 0x02000000u) {
                    const u32 bp = buf_ptr & 0x01FFFFFFu;
                    Console.WriteLn("@@SIF_RPC_BUF@@ [%s] *%08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                        mode, buf_ptr,
                        *(u32*)(eeMem->Main + bp), *(u32*)(eeMem->Main + bp+4),
                        *(u32*)(eeMem->Main + bp+8), *(u32*)(eeMem->Main + bp+12),
                        *(u32*)(eeMem->Main + bp+16), *(u32*)(eeMem->Main + bp+20),
                        *(u32*)(eeMem->Main + bp+24), *(u32*)(eeMem->Main + bp+28));
                }
            }
            // [iter653] @@KERN_CALLER_DUMP@@ — EELOAD ジャンプ元のカーネルコード (0x80005160-0x800051A0) ダンプ
            // ra=8000517c が EELOAD へのcall元。このコードを解読して EELOAD ロードhandlingを追跡。
            // Removal condition: EELOAD ロードissue解決後
            Console.WriteLn("@@KERN_CALLER_DUMP@@ [%s] 80005160: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode,
                *(u32*)(eeMem->Main + 0x5160), *(u32*)(eeMem->Main + 0x5164),
                *(u32*)(eeMem->Main + 0x5168), *(u32*)(eeMem->Main + 0x516c),
                *(u32*)(eeMem->Main + 0x5170), *(u32*)(eeMem->Main + 0x5174),
                *(u32*)(eeMem->Main + 0x5178), *(u32*)(eeMem->Main + 0x517c));
            Console.WriteLn("@@KERN_CALLER_DUMP@@ [%s] 80005180: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode,
                *(u32*)(eeMem->Main + 0x5180), *(u32*)(eeMem->Main + 0x5184),
                *(u32*)(eeMem->Main + 0x5188), *(u32*)(eeMem->Main + 0x518c),
                *(u32*)(eeMem->Main + 0x5190), *(u32*)(eeMem->Main + 0x5194),
                *(u32*)(eeMem->Main + 0x5198), *(u32*)(eeMem->Main + 0x519c));
            // Also dump the memclear / EELOAD copy source area in BIOS ROM
            // The EELOAD is at BIOS ROM. Check if the kernel code at 0x80005xxx has the copy loop.
            // Dump wider kernel context: 0x80005100-0x80005160
            Console.WriteLn("@@KERN_CALLER_DUMP@@ [%s] 80005100: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode,
                *(u32*)(eeMem->Main + 0x5100), *(u32*)(eeMem->Main + 0x5104),
                *(u32*)(eeMem->Main + 0x5108), *(u32*)(eeMem->Main + 0x510c),
                *(u32*)(eeMem->Main + 0x5110), *(u32*)(eeMem->Main + 0x5114),
                *(u32*)(eeMem->Main + 0x5118), *(u32*)(eeMem->Main + 0x511c));
            Console.WriteLn("@@KERN_CALLER_DUMP@@ [%s] 80005120: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode,
                *(u32*)(eeMem->Main + 0x5120), *(u32*)(eeMem->Main + 0x5124),
                *(u32*)(eeMem->Main + 0x5128), *(u32*)(eeMem->Main + 0x512c),
                *(u32*)(eeMem->Main + 0x5130), *(u32*)(eeMem->Main + 0x5134),
                *(u32*)(eeMem->Main + 0x5138), *(u32*)(eeMem->Main + 0x513c));
            Console.WriteLn("@@KERN_CALLER_DUMP@@ [%s] 80005140: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode,
                *(u32*)(eeMem->Main + 0x5140), *(u32*)(eeMem->Main + 0x5144),
                *(u32*)(eeMem->Main + 0x5148), *(u32*)(eeMem->Main + 0x514c),
                *(u32*)(eeMem->Main + 0x5150), *(u32*)(eeMem->Main + 0x5154),
                *(u32*)(eeMem->Main + 0x5158), *(u32*)(eeMem->Main + 0x515c));
        }
    }

    // [iter654] @@KERN_IDLE_DUMP@@ — カーネル SIF init loopのコードダンプ (vsync=8)
    // Removal condition: カーネルスタックissue解決後
    if (gsvsync_count == 8 && eeMem) {
        static bool s_idle_dump_done = false;
        if (!s_idle_dump_done) {
            s_idle_dump_done = true;
            const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
            // 0x8000fe20-0x8000fea0 の完全ダンプ
            for (u32 base = 0xfe00; base <= 0xfea0; base += 0x20) {
                Console.WriteLn("@@KERN_IDLE_DUMP@@ [%s] 8000%04x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, base,
                    *(u32*)(eeMem->Main + base), *(u32*)(eeMem->Main + base+4),
                    *(u32*)(eeMem->Main + base+8), *(u32*)(eeMem->Main + base+12),
                    *(u32*)(eeMem->Main + base+16), *(u32*)(eeMem->Main + base+20),
                    *(u32*)(eeMem->Main + base+24), *(u32*)(eeMem->Main + base+28));
            }
            // [iter658] @@KERN_MEMCPY_DUMP@@ — change#2 caller 0x8000db78 と memclear 0x8000e5e8 付近
            // Removal condition: 0x991F0 差異のcauseafter identified
            for (u32 base = 0xdb60; base <= 0xdc00; base += 0x20) {
                Console.WriteLn("@@KERN_MEMCPY_DUMP@@ [%s] 8000%04x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, base,
                    *(u32*)(eeMem->Main + base), *(u32*)(eeMem->Main + base+4),
                    *(u32*)(eeMem->Main + base+8), *(u32*)(eeMem->Main + base+12),
                    *(u32*)(eeMem->Main + base+16), *(u32*)(eeMem->Main + base+20),
                    *(u32*)(eeMem->Main + base+24), *(u32*)(eeMem->Main + base+28));
            }
            // [iter658] @@KERN_ELFCOPY_DUMP@@ — 0x80005500 のコピーloop (WRITE_82000 pc)
            for (u32 base = 0x54e0; base <= 0x5560; base += 0x20) {
                Console.WriteLn("@@KERN_ELFCOPY_DUMP@@ [%s] 8000%04x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, base,
                    *(u32*)(eeMem->Main + base), *(u32*)(eeMem->Main + base+4),
                    *(u32*)(eeMem->Main + base+8), *(u32*)(eeMem->Main + base+12),
                    *(u32*)(eeMem->Main + base+16), *(u32*)(eeMem->Main + base+20),
                    *(u32*)(eeMem->Main + base+24), *(u32*)(eeMem->Main + base+28));
            }
            for (u32 base = 0xe560; base <= 0xe620; base += 0x20) {
                Console.WriteLn("@@KERN_MEMCPY_DUMP@@ [%s] 8000%04x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, base,
                    *(u32*)(eeMem->Main + base), *(u32*)(eeMem->Main + base+4),
                    *(u32*)(eeMem->Main + base+8), *(u32*)(eeMem->Main + base+12),
                    *(u32*)(eeMem->Main + base+16), *(u32*)(eeMem->Main + base+20),
                    *(u32*)(eeMem->Main + base+24), *(u32*)(eeMem->Main + base+28));
            }
            // [iter657] @@KERN_LOADER_DUMP@@ — カーネルfunction 0x80001460 のコードダンプ
            // ra=0x8000517C から呼ばれる EELOAD ロード関連function
            // Removal condition: EELOAD ロードissueafter identified
            for (u32 base = 0x1460; base <= 0x1580; base += 0x20) {
                Console.WriteLn("@@KERN_LOADER_DUMP@@ [%s] 8000%04x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, base,
                    *(u32*)(eeMem->Main + base), *(u32*)(eeMem->Main + base+4),
                    *(u32*)(eeMem->Main + base+8), *(u32*)(eeMem->Main + base+12),
                    *(u32*)(eeMem->Main + base+16), *(u32*)(eeMem->Main + base+20),
                    *(u32*)(eeMem->Main + base+24), *(u32*)(eeMem->Main + base+28));
            }
            // [iter657] @@KERN_ELFLOADER_DUMP@@ — ELF ローダーfunction 0x80005A58 と関連コード
            // 0x80005A58: JAL 0x80005A58 at 0x800050F0 から呼ばれる
            // 0x80005AF8: JAL 0x80005AF8 at 0x80005024 から呼ばれる
            // Removal condition: EELOAD ロードissueafter identified
            for (u32 base = 0x5A00; base <= 0x5C00; base += 0x20) {
                Console.WriteLn("@@KERN_ELFLOADER_DUMP@@ [%s] 8000%04x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, base,
                    *(u32*)(eeMem->Main + base), *(u32*)(eeMem->Main + base+4),
                    *(u32*)(eeMem->Main + base+8), *(u32*)(eeMem->Main + base+12),
                    *(u32*)(eeMem->Main + base+16), *(u32*)(eeMem->Main + base+20),
                    *(u32*)(eeMem->Main + base+24), *(u32*)(eeMem->Main + base+28));
            }
            // 800125c0-80012700 もダンプ (outer loop)
            for (u32 base = 0x125c0; base <= 0x12700; base += 0x20) {
                Console.WriteLn("@@KERN_IDLE_DUMP@@ [%s] 8000%04x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, base,
                    *(u32*)(eeMem->Main + base), *(u32*)(eeMem->Main + base+4),
                    *(u32*)(eeMem->Main + base+8), *(u32*)(eeMem->Main + base+12),
                    *(u32*)(eeMem->Main + base+16), *(u32*)(eeMem->Main + base+20),
                    *(u32*)(eeMem->Main + base+24), *(u32*)(eeMem->Main + base+28));
            }
            // memory値ダンプ: SIF init で参照されるaddress
            Console.WriteLn("@@KERN_SIF_STATE@@ [0x80024100]=%08x [0x80023fc8]=%08x [0x80024124]=%08x [0x800242f8]=%08x [0x80023fcc]=%08x",
                *(u32*)(eeMem->Main + 0x24100), *(u32*)(eeMem->Main + 0x23fc8),
                *(u32*)(eeMem->Main + 0x24124), *(u32*)(eeMem->Main + 0x242f8),
                *(u32*)(eeMem->Main + 0x23fcc));
        }
    }

    // [iter660] @@KERN_SIF_CODE@@ – カーネル SIF DMA configfunctionフルダンプ
    // Removal condition: SIF タグ破損causeafter identified
    if (gsvsync_count == 7 && eeMem) {
        static bool s_sifcode_done = false;
        if (!s_sifcode_done) {
            s_sifcode_done = true;
            const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
            // Dump 0x80006410-0x80006640 (full sceSifSetDma kernel function)
            for (u32 base = 0x6410; base < 0x6640; base += 0x20) {
                Console.WriteLn("@@KERN_SIF_CODE@@ [%s] 8000%04x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, base,
                    *(u32*)(eeMem->Main + base), *(u32*)(eeMem->Main + base+4),
                    *(u32*)(eeMem->Main + base+8), *(u32*)(eeMem->Main + base+12),
                    *(u32*)(eeMem->Main + base+16), *(u32*)(eeMem->Main + base+20),
                    *(u32*)(eeMem->Main + base+24), *(u32*)(eeMem->Main + base+28));
            }
            // 0x800068c0-0x80006920 (caller loop)
            for (u32 base = 0x68c0; base < 0x6920; base += 0x20) {
                Console.WriteLn("@@KERN_SIF_CODE@@ [%s] 8000%04x: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode, base,
                    *(u32*)(eeMem->Main + base), *(u32*)(eeMem->Main + base+4),
                    *(u32*)(eeMem->Main + base+8), *(u32*)(eeMem->Main + base+12),
                    *(u32*)(eeMem->Main + base+16), *(u32*)(eeMem->Main + base+20),
                    *(u32*)(eeMem->Main + base+24), *(u32*)(eeMem->Main + base+28));
            }
        }
    }

    // [iter676h] @@EELOAD_STUCK_DUMP@@ — JIT stuck PC dump (one-shot, vsync=100)
    // Removal condition: OSDSYS stuck causeafter identified
    if (gsvsync_count == 100 && eeMem) {
        static bool s_stuck_dump_done = false;
        if (!s_stuck_dump_done) {
            s_stuck_dump_done = true;
            const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
            const u32 pc = cpuRegs.pc;
            // Dump 32 instructions around stuck PC
            const u32 base = (pc >= 0x40) ? (pc - 0x40) : 0;
            const u32 phys_base = base & 0x01FFFFFFu;
            if (phys_base + 0x100 <= Ps2MemSize::MainRam) {
                for (u32 off = 0; off < 0x100; off += 0x20) {
                    const u32 a = phys_base + off;
                    Console.WriteLn("@@EELOAD_STUCK_DUMP@@ [%s] %08x: %08x %08x %08x %08x %08x %08x %08x %08x%s",
                        mode, base + off,
                        *(u32*)(eeMem->Main + a), *(u32*)(eeMem->Main + a+4),
                        *(u32*)(eeMem->Main + a+8), *(u32*)(eeMem->Main + a+12),
                        *(u32*)(eeMem->Main + a+16), *(u32*)(eeMem->Main + a+20),
                        *(u32*)(eeMem->Main + a+24), *(u32*)(eeMem->Main + a+28),
                        ((base + off) <= pc && pc < (base + off + 0x20)) ? " <<<PC" : "");
                }
            }
            // Also dump DMA5 (SIF0) and SIF handshake registers
            Console.WriteLn("@@EELOAD_STUCK_DMA@@ [%s] D5_CHCR=%08x D5_MADR=%08x D5_QWC=%08x D5_TADR=%08x D6_CHCR=%08x D6_MADR=%08x D6_QWC=%08x D6_TADR=%08x SBUS_F220=%08x F230=%08x F240=%08x",
                mode, psHu32(0xC000), psHu32(0xC010), psHu32(0xC020), psHu32(0xC030),
                psHu32(0xC400), psHu32(0xC410), psHu32(0xC420), psHu32(0xC430),
                psHu32(0xF220), psHu32(0xF230), psHu32(0xF240));
            // [TEMP_DIAG] INTC/DMAC interrupt state dump
            Console.WriteLn("@@EELOAD_STUCK_INTC@@ [%s] INTC_STAT=%08x INTC_MASK=%08x DMAC_CTRL=%08x DMAC_STAT=%08x COP0_Status=%08x COP0_Cause=%08x",
                mode, psHu32(0xF000), psHu32(0xF010), psHu32(0xE000), psHu32(0xE010),
                cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause);
            // Dump SIF shared memory area — BIOS stores RPC completion flags near 0x93680 (s2 region from syscall trace)
            if (eeMem) {
                for (u32 sif_base = 0x93780; sif_base < 0x93800; sif_base += 0x10) {
                    const u32 p = sif_base & 0x01FFFFFFu;
                    Console.WriteLn("@@SIF_SHMEM@@ [%s] %06x: %08x %08x %08x %08x",
                        mode, sif_base,
                        *(u32*)(eeMem->Main + p), *(u32*)(eeMem->Main + p+4),
                        *(u32*)(eeMem->Main + p+8), *(u32*)(eeMem->Main + p+12));
                }
            }
            // [TEMP_DIAG] Dump sceSifCheckStatRpc function code at 0x83A80
            {
                const u32 fn_base = 0x83A80;
                const u32 fn_phys = fn_base & 0x01FFFFFFu;
                if (fn_phys + 0x80 <= Ps2MemSize::MainRam) {
                    for (u32 off = 0; off < 0x80; off += 0x20) {
                        const u32 a = fn_phys + off;
                        Console.WriteLn("@@SIF_CHECKSTAT_CODE@@ [%s] %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                            mode, fn_base + off,
                            *(u32*)(eeMem->Main + a), *(u32*)(eeMem->Main + a+4),
                            *(u32*)(eeMem->Main + a+8), *(u32*)(eeMem->Main + a+12),
                            *(u32*)(eeMem->Main + a+16), *(u32*)(eeMem->Main + a+20),
                            *(u32*)(eeMem->Main + a+24), *(u32*)(eeMem->Main + a+28));
                    }
                }
            }
            // Dump sceSifSetDma function code at 0x83BF0 (first 0x40 bytes)
            {
                const u32 fn_base = 0x83BF0;
                const u32 fn_phys = fn_base & 0x01FFFFFFu;
                if (fn_phys + 0x40 <= Ps2MemSize::MainRam) {
                    for (u32 off = 0; off < 0x40; off += 0x20) {
                        const u32 a = fn_phys + off;
                        Console.WriteLn("@@SIF_SETDMA_CODE@@ [%s] %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
                            mode, fn_base + off,
                            *(u32*)(eeMem->Main + a), *(u32*)(eeMem->Main + a+4),
                            *(u32*)(eeMem->Main + a+8), *(u32*)(eeMem->Main + a+12),
                            *(u32*)(eeMem->Main + a+16), *(u32*)(eeMem->Main + a+20),
                            *(u32*)(eeMem->Main + a+24), *(u32*)(eeMem->Main + a+28));
                    }
                }
            }
        }
    }

    // [iter657] @@EELOAD_FNPTR_TRACK@@ — 0x991F0 の値を追跡 (LoadExecPS2 filename)
    // JIT では "rom0:OSDSYS" が存在、Interpreter ではゼロ → いつ差が生じるか特定
    // Removal condition: 0x991F0 差異のcauseafter identified
    if (gsvsync_count <= 10 && eeMem) {
        const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
        const u32 p = 0x991F0 & 0x01FFFFFFu;
        Console.WriteLn("@@EELOAD_FNPTR_TRACK@@ [%s] vsync=%d 0x991F0: %08x %08x %08x %08x ('%c%c%c%c%c%c%c%c')",
            mode, gsvsync_count,
            *(u32*)(eeMem->Main + p), *(u32*)(eeMem->Main + p+4),
            *(u32*)(eeMem->Main + p+8), *(u32*)(eeMem->Main + p+12),
            *(u8*)(eeMem->Main + p+0) >= 0x20 ? *(u8*)(eeMem->Main + p+0) : '.',
            *(u8*)(eeMem->Main + p+1) >= 0x20 ? *(u8*)(eeMem->Main + p+1) : '.',
            *(u8*)(eeMem->Main + p+2) >= 0x20 ? *(u8*)(eeMem->Main + p+2) : '.',
            *(u8*)(eeMem->Main + p+3) >= 0x20 ? *(u8*)(eeMem->Main + p+3) : '.',
            *(u8*)(eeMem->Main + p+4) >= 0x20 ? *(u8*)(eeMem->Main + p+4) : '.',
            *(u8*)(eeMem->Main + p+5) >= 0x20 ? *(u8*)(eeMem->Main + p+5) : '.',
            *(u8*)(eeMem->Main + p+6) >= 0x20 ? *(u8*)(eeMem->Main + p+6) : '.',
            *(u8*)(eeMem->Main + p+7) >= 0x20 ? *(u8*)(eeMem->Main + p+7) : '.');
    }

    // [iter59] @@EELOAD_STATE_V2@@ – EE/IOP PC, cycle counts
    // [iter177] extended: also fire at vsync=60,120,240,360 to track EE progress past ROM
    if (gsvsync_count <= 50 || gsvsync_count == 60 || gsvsync_count == 80 || gsvsync_count == 100 ||
        gsvsync_count == 120 || gsvsync_count == 150 || gsvsync_count == 200 ||
        gsvsync_count == 240 || gsvsync_count == 300 ||
        gsvsync_count == 310 || gsvsync_count == 320 || gsvsync_count == 330 ||
        gsvsync_count == 340 || gsvsync_count == 350 || gsvsync_count == 360 ||
        gsvsync_count == 600 || gsvsync_count == 1200 || gsvsync_count == 2400 ||
        gsvsync_count == 3600 || gsvsync_count == 4800 || gsvsync_count == 6000 ||
        gsvsync_count == 9000 || gsvsync_count == 12000 || gsvsync_count == 18000) {
        // [iter178] add ee_s0/ee_v0 to identify what EE polls in stuck loop at 0x9FC437E8
        // LB v0, 0(s0): s0 = target address, v0 = polled value
        // [iter299] ee_ra + eeRam19dc: $ra=0 loopcause追跡。Removal condition: PC=0 loop解消後
        // [iter648] D5/D6 SIF DMA state comparison JIT vs Interpreter
        // [iter660] ADDRESS FIX: D5(SIF0)=0xC000, D6(SIF1)=0xC400 (previously wrong: 0xC400/0xC800)
        // Removal condition: EELOAD loading divergence root causeafter identified
        // [iter_BEV] cop0_status added: BEV bit(22) check. JIT hypothesis: BEV=1 when interrupt fires
        // → exception goes to 0xBFC00200 instead of 0x80000180 → IOP interrupt not dispatched
        // Removal condition: BEV=0/1 at vsync=7 confirmed in JIT
        Console.WriteLn("@@EELOAD_STATE_V2@@ vsync=%d pc=%08x iop_pc=%08x ee_cyc=%u iop_cyc=%u eeMem[82000]=%08x ee_s0=%08x ee_v0=%08x ee_ra=%08x D2_CHCR=%08x D2_QWC=%08x D_CTRL=%08x D_ENABLER=%08x gif_stat=%08x cop0_status=%08x",
            gsvsync_count, cpuRegs.pc, psxRegs.pc,
            cpuRegs.cycle, psxRegs.cycle,
            memRead32(0x82000),
            cpuRegs.GPR.r[16].UL[0],   // s0
            cpuRegs.GPR.r[2].UL[0],    // v0
            cpuRegs.GPR.r[31].UL[0],   // $ra
            psHu32(0xA000),             // D2_CHCR (GIF DMA) — correct addr!
            psHu32(0xA020),             // D2_QWC
            psHu32(0xE000),             // D_CTRL (DMAC control)
            psHu32(0xF520),             // DMAC_ENABLER
            psHu32(0x3020),             // GIF_STAT
            cpuRegs.CP0.r[12]);         // COP0 Status
        // [TEMP_DIAG] bfc00560 polling loop diagnosis — dump a3 (poll addr), v0/v1 (read values), a0 (counter)
        // Removal condition: bfc00560 loopcauseafter identified
        if (cpuRegs.pc == 0xbfc00560 && gsvsync_count <= 10) {
            u32 a3_val = cpuRegs.GPR.r[7].UL[0]; // $a3
            u32 poll_val = 0;
            if (a3_val >= 0x10000000 && a3_val < 0x20000000) {
                // HW register range — read via psHu32
                poll_val = psHu32(a3_val & 0xFFFF);
            } else if (a3_val < 0x02000000) {
                poll_val = memRead32(a3_val);
            }
            Console.WriteLn("@@BFC00560_POLL@@ vsync=%d a3=%08x poll_val=%08x v0=%08x v1=%08x a0=%08x a2=%08x t0=%08x status=%08x cause=%08x epc=%08x",
                gsvsync_count, a3_val, poll_val,
                cpuRegs.GPR.r[2].UL[0],   // v0
                cpuRegs.GPR.r[3].UL[0],   // v1
                cpuRegs.GPR.r[4].UL[0],   // a0
                cpuRegs.GPR.r[6].UL[0],   // a2
                cpuRegs.GPR.r[8].UL[0],   // t0
                cpuRegs.CP0.r[12],         // Status
                cpuRegs.CP0.r[13],         // Cause
                cpuRegs.CP0.r[14]);        // EPC
        }
        // [P15] D1 (VIF1) DMA stateスナップショット
        // Removal condition: VIF1 DMA issue解消後
        if (gsvsync_count == 300 || gsvsync_count == 600 || gsvsync_count == 1200) {
            Console.WriteLn("@@D1_VIF1_STATE@@ vsync=%d D1_CHCR=%08x D1_MADR=%08x D1_QWC=%04x D1_TADR=%08x DMAC_STAT=%08x VIF1_STAT=%08x",
                gsvsync_count,
                psHu32(0x9000), psHu32(0x9010), (u16)psHu32(0x9020), psHu32(0x9030),
                psHu32(0xE010),   // DMAC_STAT
                psHu32(0x3C00));  // VIF1_STAT
            // [P15] @@GAME_EE_STATE@@ EE PC + COP0 state + DMA 全チャネル概要
            // 目的: ゲームがどこで停滞しているか特定 (カーネル WaitSema loop? ゲームコード?)
            // Removal condition: VIF1 DMA draws>0 達成後
            Console.WriteLn("@@GAME_EE_STATE@@ vsync=%d pc=%08x cause=%08x status=%08x epc=%08x INTC_STAT=%08x INTC_MASK=%08x",
                gsvsync_count, cpuRegs.pc,
                cpuRegs.CP0.r[13], cpuRegs.CP0.r[12], cpuRegs.CP0.r[14],
                psHu32(0xF000), psHu32(0xF010));
            // [P15] @@CDVD_RPC_RESP@@ sceCdDiskReady/SearchFile 応答buffer
            // 0074f200 = cdvdfsv RPC 受信buffer (16 bytes), 0074f648 = client struct area
            // Removal condition: CDVD issue解消後
            if (eeMem) {
                Console.WriteLn("@@CDVD_RPC_RESP@@ vsync=%d [0074f200]=%08x %08x %08x %08x [0074f648]=%08x %08x %08x %08x cdvd_ready=%02x cdvd_status=%02x",
                    gsvsync_count,
                    *(u32*)(eeMem->Main + 0x74f200), *(u32*)(eeMem->Main + 0x74f204),
                    *(u32*)(eeMem->Main + 0x74f208), *(u32*)(eeMem->Main + 0x74f20c),
                    *(u32*)(eeMem->Main + 0x74f648), *(u32*)(eeMem->Main + 0x74f64c),
                    *(u32*)(eeMem->Main + 0x74f650), *(u32*)(eeMem->Main + 0x74f654),
                    cdvd.Ready, cdvd.Status);
            }
            // [P15] SIF0 transferカウント — IOP→EE RPC 応答がゲームフェーズで届いているかverify
            Console.WriteLn("@@SIF0_TOTAL@@ vsync=%d sif0_xfer_total=%u", gsvsync_count, g_sif0_ee_xfer_total);
            // [P15] @@CDVD_CLIENT_STRUCT@@ — cdvdfsv 0x80000596 client struct dump (0x20753c40 → phys 0x00753c40)
            // server_data は offset +0x24 にあるはず (fake bind injection のオフセット)
            // Removal condition: CDVD RPC issue解消後
            if (eeMem) {
                const u32 cbase = 0x753c40;
                Console.WriteLn("@@CDVD_CLIENT_STRUCT@@ vsync=%d [+00]=%08x %08x %08x %08x [+10]=%08x %08x %08x %08x [+20]=%08x %08x %08x %08x [+30]=%08x %08x %08x %08x",
                    gsvsync_count,
                    *(u32*)(eeMem->Main + cbase + 0x00), *(u32*)(eeMem->Main + cbase + 0x04),
                    *(u32*)(eeMem->Main + cbase + 0x08), *(u32*)(eeMem->Main + cbase + 0x0c),
                    *(u32*)(eeMem->Main + cbase + 0x10), *(u32*)(eeMem->Main + cbase + 0x14),
                    *(u32*)(eeMem->Main + cbase + 0x18), *(u32*)(eeMem->Main + cbase + 0x1c),
                    *(u32*)(eeMem->Main + cbase + 0x20), *(u32*)(eeMem->Main + cbase + 0x24),
                    *(u32*)(eeMem->Main + cbase + 0x28), *(u32*)(eeMem->Main + cbase + 0x2c),
                    *(u32*)(eeMem->Main + cbase + 0x30), *(u32*)(eeMem->Main + cbase + 0x34),
                    *(u32*)(eeMem->Main + cbase + 0x38), *(u32*)(eeMem->Main + cbase + 0x3c));
            }
            // DMA enable + all channel CHCR summary
            Console.WriteLn("@@GAME_DMA_STATE@@ vsync=%d CTRL=%08x D0=%08x D1=%08x D2=%08x D3=%08x D4=%08x D5=%08x D6=%08x D8=%08x D9=%08x",
                gsvsync_count,
                psHu32(0xE000),  // DMAC_CTRL
                psHu32(0x8000),  // D0 VIF0
                psHu32(0x9000),  // D1 VIF1
                psHu32(0xA000),  // D2 GIF
                psHu32(0xB000),  // D3 IPU_FROM
                psHu32(0xB400),  // D4 IPU_TO
                psHu32(0xC000),  // D5 SIF0
                psHu32(0xC400),  // D6 SIF1
                psHu32(0xD000),  // D8 SPR_FROM
                psHu32(0xD400)); // D9 SPR_TO
            // [P15] @@P15_MEM_TIMELINE@@ — exception vector + game code timeline
            // Removal condition: exception vector / game code zeros issue解消後
            if (eeMem) {
                Console.WriteLn("@@P15_MEM_TIMELINE@@ stage=VSYNC vsync=%d excvec[80]=%08x [84]=%08x [180]=%08x [184]=%08x [188]=%08x [18C]=%08x",
                    gsvsync_count,
                    *(u32*)(eeMem->Main + 0x80), *(u32*)(eeMem->Main + 0x84),
                    *(u32*)(eeMem->Main + 0x180), *(u32*)(eeMem->Main + 0x184),
                    *(u32*)(eeMem->Main + 0x188), *(u32*)(eeMem->Main + 0x18C));
                Console.WriteLn("@@P15_MEM_TIMELINE@@ stage=VSYNC vsync=%d mem[26FDE0]=%08x %08x %08x %08x [26FDF0]=%08x %08x %08x %08x",
                    gsvsync_count,
                    *(u32*)(eeMem->Main + 0x26FDE0), *(u32*)(eeMem->Main + 0x26FDE4),
                    *(u32*)(eeMem->Main + 0x26FDE8), *(u32*)(eeMem->Main + 0x26FDEC),
                    *(u32*)(eeMem->Main + 0x26FDF0), *(u32*)(eeMem->Main + 0x26FDF4),
                    *(u32*)(eeMem->Main + 0x26FDF8), *(u32*)(eeMem->Main + 0x26FDFC));
                Console.WriteLn("@@P15_MEM_TIMELINE@@ stage=VSYNC vsync=%d mem[270000]=%08x %08x %08x %08x [100000]=%08x %08x %08x %08x",
                    gsvsync_count,
                    *(u32*)(eeMem->Main + 0x270000), *(u32*)(eeMem->Main + 0x270004),
                    *(u32*)(eeMem->Main + 0x270008), *(u32*)(eeMem->Main + 0x27000C),
                    *(u32*)(eeMem->Main + 0x100000), *(u32*)(eeMem->Main + 0x100004),
                    *(u32*)(eeMem->Main + 0x100008), *(u32*)(eeMem->Main + 0x10000C));
                // [P15] @@P15_ZERO_SCAN@@ — find zero hole boundaries in loaded ELF area
                // ELF filesz covers 0x100000-0x54C98B. Scan for first/last zero page.
                // Removal condition: game code zeros issue解消後
                {
                    u32 first_zero_page = 0, last_zero_page = 0;
                    u32 zero_pages = 0;
                    // Scan pages from 0x100000 to 0x54C000 in 0x1000 (4KB) steps
                    for (u32 pg = 0x100000; pg < 0x54D000; pg += 0x1000) {
                        bool all_zero = true;
                        for (u32 off = 0; off < 0x1000; off += 4) {
                            if (*(u32*)(eeMem->Main + pg + off) != 0) {
                                all_zero = false;
                                break;
                            }
                        }
                        if (all_zero) {
                            zero_pages++;
                            if (first_zero_page == 0) first_zero_page = pg;
                            last_zero_page = pg;
                        }
                    }
                    Console.WriteLn("@@P15_ZERO_SCAN@@ vsync=%d zero_pages=%u first=%08x last=%08x (ELF filesz range 100000-54C98B)",
                        gsvsync_count, zero_pages, first_zero_page, last_zero_page);
                    // Scan near the known zero area 0x26F000-0x270000 in 256-byte granularity
                    for (u32 blk = 0x26E000; blk < 0x272000; blk += 0x100) {
                        bool all_zero = true;
                        for (u32 off = 0; off < 0x100; off += 4) {
                            if (*(u32*)(eeMem->Main + blk + off) != 0) {
                                all_zero = false;
                                break;
                            }
                        }
                        if (all_zero) {
                            Console.WriteLn("@@P15_ZERO_DETAIL@@ vsync=%d zero_block=%08x-%08x",
                                gsvsync_count, blk, blk + 0xFF);
                        }
                    }
                }
                // [P15] game init function at 0x247a48 — where game spends vsync 310-350
                Console.WriteLn("@@P15_INIT_CODE@@ vsync=%d [247a40]=%08x %08x %08x %08x %08x %08x %08x %08x [247a60]=%08x %08x %08x %08x %08x %08x %08x %08x",
                    gsvsync_count,
                    *(u32*)(eeMem->Main + 0x247a40), *(u32*)(eeMem->Main + 0x247a44),
                    *(u32*)(eeMem->Main + 0x247a48), *(u32*)(eeMem->Main + 0x247a4C),
                    *(u32*)(eeMem->Main + 0x247a50), *(u32*)(eeMem->Main + 0x247a54),
                    *(u32*)(eeMem->Main + 0x247a58), *(u32*)(eeMem->Main + 0x247a5C),
                    *(u32*)(eeMem->Main + 0x247a60), *(u32*)(eeMem->Main + 0x247a64),
                    *(u32*)(eeMem->Main + 0x247a68), *(u32*)(eeMem->Main + 0x247a6C),
                    *(u32*)(eeMem->Main + 0x247a70), *(u32*)(eeMem->Main + 0x247a74),
                    *(u32*)(eeMem->Main + 0x247a78), *(u32*)(eeMem->Main + 0x247a7C));
                // Also dump 0x2478e0 (PC at vsync 330 when v0=0xFFFFFFFF) and 0x247e80 (ra target)
                Console.WriteLn("@@P15_INIT_CODE@@ vsync=%d [2478e0]=%08x %08x %08x %08x %08x %08x %08x %08x",
                    gsvsync_count,
                    *(u32*)(eeMem->Main + 0x2478e0), *(u32*)(eeMem->Main + 0x2478e4),
                    *(u32*)(eeMem->Main + 0x2478e8), *(u32*)(eeMem->Main + 0x2478eC),
                    *(u32*)(eeMem->Main + 0x2478f0), *(u32*)(eeMem->Main + 0x2478f4),
                    *(u32*)(eeMem->Main + 0x2478f8), *(u32*)(eeMem->Main + 0x2478fC));
            }
        }
        // [P14-game] dump game code instructions when EE PC is in game region (one-shot)
        if ((cpuRegs.pc >= 0x001e0000 && cpuRegs.pc <= 0x001f0000) ||
            (cpuRegs.pc >= 0x00260000 && cpuRegs.pc <= 0x00280000)) {
            static int s_game_insn_cnt = 0;
            if (s_game_insn_cnt < 5) {
                s_game_insn_cnt++;
                u32 pc = cpuRegs.pc;
                Console.WriteLn("@@GAME_INSN@@ vsync=%d pc=%08x insn: %08x %08x %08x %08x | %08x %08x %08x %08x",
                    gsvsync_count, pc,
                    memRead32(pc-8), memRead32(pc-4), memRead32(pc), memRead32(pc+4),
                    memRead32(pc+8), memRead32(pc+12), memRead32(pc+16), memRead32(pc+20));
                // Also dump all GPRs to understand game state
                Console.WriteLn("@@GAME_REGS@@ v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x a3=%08x s0=%08x s1=%08x s2=%08x s3=%08x t0=%08x t1=%08x sp=%08x ra=%08x",
                    cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[3].UL[0],
                    cpuRegs.GPR.r[4].UL[0], cpuRegs.GPR.r[5].UL[0],
                    cpuRegs.GPR.r[6].UL[0], cpuRegs.GPR.r[7].UL[0],
                    cpuRegs.GPR.r[16].UL[0], cpuRegs.GPR.r[17].UL[0],
                    cpuRegs.GPR.r[18].UL[0], cpuRegs.GPR.r[19].UL[0],
                    cpuRegs.GPR.r[8].UL[0], cpuRegs.GPR.r[9].UL[0],
                    cpuRegs.GPR.r[29].UL[0], cpuRegs.GPR.r[31].UL[0]);
            }
        }
        // [iter676h_diag] dump loop-critical registers when pc in OSDSYS math range (one-shot)
        if (cpuRegs.pc >= 0x00265500 && cpuRegs.pc <= 0x00265700) {
            static int s_stuck_reg_cnt = 0;
            if (s_stuck_reg_cnt < 10) {
                s_stuck_reg_cnt++;
            Console.WriteLn("@@STUCK_REGS@@ vsync=%d a0=%016llx s1=%016llx v0=%016llx v1=%016llx a1=%016llx a2=%016llx a3=%016llx t0=%016llx sp=%08x",
                gsvsync_count,
                cpuRegs.GPR.r[4].UD[0],   // a0
                cpuRegs.GPR.r[17].UD[0],  // s1
                cpuRegs.GPR.r[2].UD[0],   // v0
                cpuRegs.GPR.r[3].UD[0],   // v1
                cpuRegs.GPR.r[5].UD[0],   // a1
                cpuRegs.GPR.r[6].UD[0],   // a2
                cpuRegs.GPR.r[7].UD[0],   // a3
                cpuRegs.GPR.r[8].UD[0],   // t0
                cpuRegs.GPR.r[29].UL[0]); // sp
            }
        }
        // [TEMP_DIAG] @@JIT_OSDSYS_DUMP@@ — Dump MIPS code at 0x21c580-0x21c780 and 0x21d860-0x21d8a0
        // to understand the tight loop JIT gets stuck in. Also dump key regs.
        // Removal condition: JIT OSDSYS loop divergence root causeafter identified
        if (gsvsync_count == 35) {
            static bool s_osdsys_init_dump_done = false;
            if (!s_osdsys_init_dump_done) {
                s_osdsys_init_dump_done = true;
                // [TEMP_DIAG] Dump OSDSYS init code at 0x200d00-0x200f00
                // Removal condition: OSDSYS init divergence root causeafter identified
                Console.WriteLn("@@OSDSYS_INIT_DUMP@@ MIPS code 0x200d00-0x200f00:");
                for (u32 addr = 0x200d00; addr < 0x200f00; addr += 16) {
                    Console.WriteLn("  %08x: %08x %08x %08x %08x",
                        addr, memRead32(addr), memRead32(addr+4), memRead32(addr+8), memRead32(addr+12));
                }
            }
        }
        // [iter683] Dump OSDSYS main loop PCs to understand what the loop is doing
        if (gsvsync_count == 100) {
            static bool s_osdsys_loop_dump_done = false;
            if (!s_osdsys_loop_dump_done) {
                s_osdsys_loop_dump_done = true;
                // Dump around 0x22fd30-0x22fda0 (Interpreter main loop PC)
                Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ MIPS code 0x22fd30-0x22fda0:");
                for (u32 addr = 0x22fd30; addr < 0x22fda0; addr += 16) {
                    Console.WriteLn("  %08x: %08x %08x %08x %08x",
                        addr, memRead32(addr), memRead32(addr+4), memRead32(addr+8), memRead32(addr+12));
                }
                // Dump around 0x235e60-0x235f00 (second main loop PC)
                Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ MIPS code 0x235e60-0x235f00:");
                for (u32 addr = 0x235e60; addr < 0x235f00; addr += 16) {
                    Console.WriteLn("  %08x: %08x %08x %08x %08x",
                        addr, memRead32(addr), memRead32(addr+4), memRead32(addr+8), memRead32(addr+12));
                }
                // Dump 0x236350-0x236390 (ra=0x236374 area)
                Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ MIPS code 0x236350-0x236390:");
                for (u32 addr = 0x236350; addr < 0x236390; addr += 16) {
                    Console.WriteLn("  %08x: %08x %08x %08x %08x",
                        addr, memRead32(addr), memRead32(addr+4), memRead32(addr+8), memRead32(addr+12));
                }
                // Dump 0x219a80-0x219bc0 (Interpreter active range)
                Console.WriteLn("@@OSDSYS_LOOP_DUMP@@ MIPS code 0x219a80-0x219bc0:");
                for (u32 addr = 0x219a80; addr < 0x219bc0; addr += 16) {
                    Console.WriteLn("  %08x: %08x %08x %08x %08x",
                        addr, memRead32(addr), memRead32(addr+4), memRead32(addr+8), memRead32(addr+12));
                }
            }
        }
        // [iter319/322] @@EELOAD_PC_DUMP@@ one-shot: dump EELOAD instructions at current PC + called function
        if (gsvsync_count == 5) {
            static bool s_dump322_done = false;
            if (!s_dump322_done) {
                s_dump322_done = true;
                const u32 pc = cpuRegs.pc;
                Console.WriteLn("@@EELOAD_PC_DUMP@@ BEGIN pc=%08x v0=%08x ra=%08x s0=%08x s2=%08x",
                    pc, cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[31].UL[0],
                    cpuRegs.GPR.r[16].UL[0], cpuRegs.GPR.r[18].UL[0]);
                // dump loop body at pc
                for (u32 a = (pc & ~0x1Fu); a <= (pc & ~0x1Fu) + 0x40; a += 4)
                    Console.WriteLn("  [%08x]=%08x", a, memRead32(a));
                // decode JAL target from first instruction and dump called function
                u32 jal_insn = memRead32(pc);
                if ((jal_insn >> 26) == 3) { // JAL
                    u32 jal_target = (pc & 0xF0000000u) | ((jal_insn & 0x03FFFFFFu) << 2);
                    Console.WriteLn("@@EELOAD_CALLEE_DUMP@@ target=%08x", jal_target);
                    for (u32 a = jal_target; a <= jal_target + 0x40; a += 4)
                        Console.WriteLn("  [%08x]=%08x", a, memRead32(a));
                }
            }
        }
        // [iter650 ハック除去] @@EELOAD_LATE_RECOPY@@ statechangeを全撤去 – 自然進行テスト
        // BIOS自然進行: SIF0 DMAがEELOADをコピーするまで待機することを期待
        // vsync=4時点のstateを観測のみ (JIT vs Interpreter 比較用)
        if (gsvsync_count == 4) {
            u32 ee82000 = eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x82000) : 0xDEAD;
            u32 excvec180 = eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x180) : 0;
            static bool s_eeload_obs_done = false;
            if (!s_eeload_obs_done) {
                s_eeload_obs_done = true;
                const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
                Console.WriteLn("@@EELOAD_NATURAL_OBS@@ vsync=4 %s: ee82000=%08x excvec180=%08x "
                    "pc=%08x D5_CHCR=%08x D6_CHCR=%08x sif_f230=%08x bev=%d status=%08x",
                    mode, ee82000, excvec180, cpuRegs.pc,
                    psHu32(0xC000), psHu32(0xC400), psHu32(0xF230),
                    (cpuRegs.CP0.r[12] >> 22) & 1, cpuRegs.CP0.r[12]);
                // [iter_LOWRAM] @@EE_LOW_RAM_DUMP@@ one-shot: vsync=4 に EE 低 RAM 0x19C0-0x1A10 をダンプ
                // JIT: eeRam19dc=0 vs Interpreter: eeRam19dc=0x3c03aaaa → 広域欠落かピンポイント欠落か判別
                // Removal condition: JIT でのストア欠落rangeafter determined
                if (eeMem) {
                    u32 w[20] = {};
                    for (int i = 0; i < 20; i++) w[i] = *(u32*)(eeMem->Main + 0x19C0 + i*4);
                    Console.WriteLn("@@EE_LOW_RAM_DUMP@@ [%s] 000019C0: %08x %08x %08x %08x %08x %08x %08x %08x",
                        mode, w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7]);
                    Console.WriteLn("@@EE_LOW_RAM_DUMP@@ [%s] 000019E0: %08x %08x %08x %08x %08x %08x %08x %08x",
                        mode, w[8],w[9],w[10],w[11],w[12],w[13],w[14],w[15]);
                    Console.WriteLn("@@EE_LOW_RAM_DUMP@@ [%s] 00001A00: %08x %08x %08x %08x",
                        mode, w[16],w[17],w[18],w[19]);
                }
                // [iter_DIAG] @@LOGO_POST_TGE@@ one-shot: dump EE RAM around TGE sites
                // TGE n=0 at 0x8003E1C0, n=1 at 0x8003E23C. Dump 30 words starting 0x10 before n=0.
                // This shows what LOGO code runs after TGEs, leading to PC=0.
                // Removal condition: PC=0 の直接causeafter identified
                if (eeMem) {
                    const u32 base = 0x8003E1B0u & 0x01FFFFFFu; // phys
                    u32 words[30] = {};
                    for (int i = 0; i < 30; i++) words[i] = *(u32*)(eeMem->Main + base + i*4);
                    Console.WriteLn("@@LOGO_POST_TGE@@ 8003E1B0: %08x %08x %08x %08x %08x %08x %08x %08x",
                        words[0],words[1],words[2],words[3],words[4],words[5],words[6],words[7]);
                    Console.WriteLn("@@LOGO_POST_TGE@@ 8003E1D0: %08x %08x %08x %08x %08x %08x %08x %08x",
                        words[8],words[9],words[10],words[11],words[12],words[13],words[14],words[15]);
                    Console.WriteLn("@@LOGO_POST_TGE@@ 8003E1F0: %08x %08x %08x %08x %08x %08x %08x %08x",
                        words[16],words[17],words[18],words[19],words[20],words[21],words[22],words[23]);
                    Console.WriteLn("@@LOGO_POST_TGE@@ 8003E210: %08x %08x %08x %08x %08x %08x",
                        words[24],words[25],words[26],words[27],words[28],words[29]);
                }
            }
        }
        // [iter_E5E8] @@EE_E5E8_CODE@@ one-shot: vsync=5 に EE RAM 8000e5e8-8000e640 をダンプ
        // 目的: 8000e5e8 loopの exit path 構造を解析 (JIT→80001578 vs Interp→00082180)
        // Removal condition: exit path 分岐causeafter identified
        if (gsvsync_count == 5) {
            static bool s_e5e8_done = false;
            if (!s_e5e8_done && eeMem) {
                s_e5e8_done = true;
                const char* mode2 = (Cpu != &intCpu) ? "JIT" : "Interp";
                // Physical: 0x8000e5e8 & 0x01FFFFFF = 0x0000e5e8
                const u32 phys = 0x0000e5e8u;
                const int N = 24; // 24 instructions = 96 bytes
                u32 insn[N] = {};
                for (int i = 0; i < N; i++) insn[i] = *(u32*)(eeMem->Main + phys + i*4);
                Console.WriteLn("@@EE_E5E8_CODE@@ [%s] 8000e5e8: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode2, insn[0],insn[1],insn[2],insn[3],insn[4],insn[5],insn[6],insn[7]);
                Console.WriteLn("@@EE_E5E8_CODE@@ [%s] 8000e608: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode2, insn[8],insn[9],insn[10],insn[11],insn[12],insn[13],insn[14],insn[15]);
                Console.WriteLn("@@EE_E5E8_CODE@@ [%s] 8000e628: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode2, insn[16],insn[17],insn[18],insn[19],insn[20],insn[21],insn[22],insn[23]);
                // Also dump COP0 Status to check BEV
                Console.WriteLn("@@EE_E5E8_CODE@@ [%s] cop0_status=%08x bev=%d ee_pc=%08x s0=%08x",
                    mode2, cpuRegs.CP0.r[12], (cpuRegs.CP0.r[12]>>22)&1, cpuRegs.pc, cpuRegs.GPR.r[16].UL[0]);
                // Dump 80001578 (EE idle/stuck) + 80007d5c (80007be0 jump target) + 80007bac (retry loop start)
                // JIT: EE stuck here from vsync=7. What is this code? Why does EE go here in JIT?
                // 80007c94: BEQ $zero,$zero,49 → 80007d5c. What's there?
                // Removal condition: 80001578 stuck causeafter identified
                const u32 p3 = 0x00001578u; // physical 0x80001578
                u32 idle[16] = {};
                for (int i = 0; i < 16; i++) idle[i] = *(u32*)(eeMem->Main + p3 + i*4);
                Console.WriteLn("@@EE_IDLE_CODE@@ [%s] 80001578: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode2, idle[0],idle[1],idle[2],idle[3],idle[4],idle[5],idle[6],idle[7]);
                Console.WriteLn("@@EE_IDLE_CODE@@ [%s] 80001598: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode2, idle[8],idle[9],idle[10],idle[11],idle[12],idle[13],idle[14],idle[15]);
                // 80007d5c = target of unconditional jump at 80007c94 (end of 80007be0 HW init)
                const u32 p4 = 0x00007d5cu; // physical 0x80007d5c
                u32 after[24] = {};
                for (int i = 0; i < 24; i++) after[i] = *(u32*)(eeMem->Main + p4 + i*4);
                Console.WriteLn("@@EE_7D5C_CODE@@ [%s] 80007d5c: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode2, after[0],after[1],after[2],after[3],after[4],after[5],after[6],after[7]);
                Console.WriteLn("@@EE_7D5C_CODE@@ [%s] 80007d7c: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode2, after[8],after[9],after[10],after[11],after[12],after[13],after[14],after[15]);
                Console.WriteLn("@@EE_7D5C_CODE@@ [%s] 80007d9c: %08x %08x %08x %08x %08x %08x %08x %08x",
                    mode2, after[16],after[17],after[18],after[19],after[20],after[21],after[22],after[23]);
            }
        }
        // [iter652] @@KERNEL_DT_DUMP@@ one-shot: vsync=7 に dispatch table (0x80015340) と
        // 例外ベクタ (0x80000180) をダンプ。JIT で dt[8]=0 (SYSCALL handler 未設置) が
        // 0x80001578 loopのcause。Interp と比較して kernel init 完了stateをverifyする。
        // Removal condition: dispatch table 初期化乖離のroot causeafter identified
        if (gsvsync_count == 7) {
            static bool s_dt_done = false;
            if (!s_dt_done && eeMem) {
                s_dt_done = true;
                const char* m = (Cpu != &intCpu) ? "JIT" : "Interp";
                // Dispatch table at 0x80015340 (physical 0x15340), 16 entries (64 bytes)
                u32 dt[16] = {};
                for (int i = 0; i < 16; i++) dt[i] = *(u32*)(eeMem->Main + 0x15340 + i*4);
                Console.WriteLn("@@KERNEL_DT_DUMP@@ [%s] dt[0-7]: %08x %08x %08x %08x %08x %08x %08x %08x",
                    m, dt[0],dt[1],dt[2],dt[3],dt[4],dt[5],dt[6],dt[7]);
                Console.WriteLn("@@KERNEL_DT_DUMP@@ [%s] dt[8-15]: %08x %08x %08x %08x %08x %08x %08x %08x",
                    m, dt[8],dt[9],dt[10],dt[11],dt[12],dt[13],dt[14],dt[15]);
                // Exception vector at 0x80000180 (physical 0x180), 8 words
                u32 ev[8] = {};
                for (int i = 0; i < 8; i++) ev[i] = *(u32*)(eeMem->Main + 0x180 + i*4);
                Console.WriteLn("@@KERNEL_DT_DUMP@@ [%s] excvec180: %08x %08x %08x %08x %08x %08x %08x %08x",
                    m, ev[0],ev[1],ev[2],ev[3],ev[4],ev[5],ev[6],ev[7]);
                // Key kernel data: 0x19dc, 0x19e0
                Console.WriteLn("@@KERNEL_DT_DUMP@@ [%s] 19dc=%08x 19e0=%08x ee_pc=%08x v1=%08x",
                    m, *(u32*)(eeMem->Main+0x19dc), *(u32*)(eeMem->Main+0x19e0),
                    cpuRegs.pc, cpuRegs.GPR.r[3].UL[0]);
            }
        }
        // [iter382] @@OSDSYS_FUNC_DUMP@@ one-shot: vsync=10 に 0x100000/0x100180/0x100480 をダンプ
        // 0x100180 が単純な Exit stub か、それとも描画コードを持つfunctionかをverifyする
        // Removal condition: 0x100180 内容verify・OSDSYS 描画パスafter identified
        if (gsvsync_count == 10) {
            static bool s_func382_done = false;
            if (!s_func382_done && eeMem) {
                s_func382_done = true;
                Console.WriteLn("@@OSDSYS_FUNC_DUMP@@ === 0x100000-0x10002C (main loop) ===");
                for (u32 a = 0x100000u; a <= 0x10002Cu; a += 4)
                    Console.WriteLn("  [%08x]=%08x", a, memRead32(a));
                Console.WriteLn("@@OSDSYS_FUNC_DUMP@@ === 0x100180-0x1001BC (Exit stub?) ===");
                for (u32 a = 0x100180u; a <= 0x1001BCu; a += 4)
                    Console.WriteLn("  [%08x]=%08x", a, memRead32(a));
                Console.WriteLn("@@OSDSYS_FUNC_DUMP@@ === 0x100480-0x1004A0 (stub table head) ===");
                for (u32 a = 0x100480u; a <= 0x1004A0u; a += 4)
                    Console.WriteLn("  [%08x]=%08x", a, memRead32(a));
                Console.WriteLn("@@OSDSYS_FUNC_DUMP@@ ee_pc=%08x ra=%08x v0=%08x",
                    cpuRegs.pc, cpuRegs.GPR.r[31].UL[0], cpuRegs.GPR.r[2].UL[0]);
            }
        }
        // [iter230] @@EXCVEC_CHECK@@ – exception vector installation check
        // Removal condition: exception vector 設置のroot causeが確定した時点
        if (gsvsync_count <= 10) {
            Console.WriteLn("@@EXCVEC_CHECK@@ vsync=%d excvec[80]=%08x [84]=%08x [88]=%08x [8c]=%08x "
                "excvec[180]=%08x [184]=%08x [188]=%08x [18c]=%08x "
                "eeMem82000=%08x [4]=%08x [8]=%08x [c]=%08x "
                "kern_3fc8=%08x kern_3fcc=%08x bev=%d status=%08x",
                gsvsync_count,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x80) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x84) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x88) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x8C) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x180) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x184) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x188) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x18C) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x82000) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x82004) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x82008) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x8200C) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x23fc8) : 0xDEAD,
                eeMem ? *reinterpret_cast<u32*>(eeMem->Main + 0x23fcc) : 0xDEAD,
                (cpuRegs.CP0.r[12] >> 22) & 1,  // BEV bit
                cpuRegs.CP0.r[12]);
            // [P12] syscall table check: MEM[0x8002a64c] = function ptr for syscall#0
            // Removal condition: syscall テーブル初期化乖離のroot causeafter determined
            if (eeMem) {
                Console.WriteLn("@@SYSCALL_TABLE@@ vsync=%d sc0_fptr=%08x sc1_fptr=%08x sc2_fptr=%08x sc4_fptr=%08x v1=%08x",
                    gsvsync_count,
                    *reinterpret_cast<u32*>(eeMem->Main + 0x2a64c),         // syscall#0 func ptr
                    *reinterpret_cast<u32*>(eeMem->Main + 0x2a64c + 0x4C),  // syscall#1 func ptr
                    *reinterpret_cast<u32*>(eeMem->Main + 0x2a64c + 0x98),  // syscall#2 func ptr
                    *reinterpret_cast<u32*>(eeMem->Main + 0x2a64c + 0x130), // syscall#4 func ptr
                    cpuRegs.GPR.r[3].UL[0]);  // v1 = current syscall number
            }
        }
        // [iter231] @@EE_EXCEPTION_HANDLER_EXEC@@ – trace exception handler entry and syscall dispatch
        // Removal condition: ExecPS2 syscall handler がverifyされるか、kernel syscall dispatch が明確になった時点
        if (cpuRegs.pc >= 0x80000180 && cpuRegs.pc < 0x80010000 && gsvsync_count <= 30) {
            static u32 last_exc_pc = 0xFFFFFFFF;
            if (cpuRegs.pc != last_exc_pc) {
                u32 v0_syscall = cpuRegs.GPR.r[2].UL[0];
                u32 epc = cpuRegs.CP0.r[14];  // COP0.EPC – exception program counter
                Console.WriteLn("@@EE_EXCEPTION_HANDLER_EXEC@@ vsync=%d pc=%08x epc=%08x v0(syscall)=%08x [pc+0]=%08x [pc+4]=%08x",
                    gsvsync_count, cpuRegs.pc, epc, v0_syscall,
                    eeMem ? *reinterpret_cast<u32*>(eeMem->Main + (cpuRegs.pc & 0x1FFFFFF)) : 0xDEAD,
                    eeMem ? *reinterpret_cast<u32*>(eeMem->Main + ((cpuRegs.pc + 4) & 0x1FFFFFF)) : 0xDEAD);
                last_exc_pc = cpuRegs.pc;
            }
        }
        // [iter212] @@BIOS_EARLY_INSN@@ – vsync 1-2 での BIOS コードダンプ (PC 周辺 + COP0.Status)
        // Removal condition: BIOS HW 初期化skipcauseafter determined
        if (gsvsync_count <= 2) {
            u32 bpc = cpuRegs.pc & ~0xFu;
            Console.WriteLn("@@BIOS_EARLY_INSN@@ vsync=%d pc=%08x status=%08x cause=%08x "
                "insn: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
                gsvsync_count, cpuRegs.pc,
                cpuRegs.CP0.r[12], cpuRegs.CP0.r[13],  // Status, Cause
                memRead32(bpc+0),  memRead32(bpc+4),  memRead32(bpc+8),  memRead32(bpc+12),
                memRead32(bpc+16), memRead32(bpc+20), memRead32(bpc+24), memRead32(bpc+28),
                memRead32(bpc+32), memRead32(bpc+36), memRead32(bpc+40), memRead32(bpc+44),
                memRead32(bpc+48), memRead32(bpc+52), memRead32(bpc+56), memRead32(bpc+60));
            // DMAC at this point
            Console.WriteLn("@@BIOS_EARLY_HW@@ vsync=%d DMAC_CTRL=%08x DMAC_STAT=%08x DMAC_ENABLER=%08x "
                "INTC_STAT=%08x INTC_MASK=%08x spr_3FF0=%08x rom_89c=%08x",
                gsvsync_count, psHu32(0xE000), psHu32(0xE010), psHu32(0xF520),
                psHu32(0xF000), psHu32(0xF010),
                *reinterpret_cast<u32*>(eeMem->Scratch + 0x3FF0),
                *reinterpret_cast<u32*>(eeMem->ROM + 0x089C));
            // [iter212] BFC00680 area dump (last BIOS block before idle loop)
            Console.WriteLn("@@BIOS_BFC00680@@ "
                "%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
                memRead32(0xBFC00680u), memRead32(0xBFC00684u), memRead32(0xBFC00688u), memRead32(0xBFC0068Cu),
                memRead32(0xBFC00690u), memRead32(0xBFC00694u), memRead32(0xBFC00698u), memRead32(0xBFC0069Cu),
                memRead32(0xBFC006A0u), memRead32(0xBFC006A4u), memRead32(0xBFC006A8u), memRead32(0xBFC006ACu),
                memRead32(0xBFC006B0u), memRead32(0xBFC006B4u), memRead32(0xBFC006B8u), memRead32(0xBFC006BCu));
            // BFC0089C area (return from JAL 9FC41000) – what does BIOS do after init subroutines?
            Console.WriteLn("@@BIOS_BFC00890@@ "
                "%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
                memRead32(0xBFC00890u), memRead32(0xBFC00894u), memRead32(0xBFC00898u), memRead32(0xBFC0089Cu),
                memRead32(0xBFC008A0u), memRead32(0xBFC008A4u), memRead32(0xBFC008A8u), memRead32(0xBFC008ACu),
                memRead32(0xBFC008B0u), memRead32(0xBFC008B4u), memRead32(0xBFC008B8u), memRead32(0xBFC008BCu),
                memRead32(0xBFC008C0u), memRead32(0xBFC008C4u), memRead32(0xBFC008C8u), memRead32(0xBFC008CCu));
            // 9FC43438-9FC43458 (polling loop area)
            Console.WriteLn("@@BIOS_9FC43438@@ "
                "%08x %08x %08x %08x | %08x %08x %08x %08x",
                memRead32(0x9FC43438u), memRead32(0x9FC4343Cu), memRead32(0x9FC43440u), memRead32(0x9FC43444u),
                memRead32(0x9FC43448u), memRead32(0x9FC4344Cu), memRead32(0x9FC43450u), memRead32(0x9FC43454u));
            // 9FC4115C area (return from JAL 9FC43460)
            Console.WriteLn("@@BIOS_9FC41150@@ "
                "%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
                memRead32(0x9FC41150u), memRead32(0x9FC41154u), memRead32(0x9FC41158u), memRead32(0x9FC4115Cu),
                memRead32(0x9FC41160u), memRead32(0x9FC41164u), memRead32(0x9FC41168u), memRead32(0x9FC4116Cu),
                memRead32(0x9FC41170u), memRead32(0x9FC41174u), memRead32(0x9FC41178u), memRead32(0x9FC4117Cu),
                memRead32(0x9FC41180u), memRead32(0x9FC41184u), memRead32(0x9FC41188u), memRead32(0x9FC4118Cu));
        }
    }
        // [iter242-diag] kernel init phase HW state probe (vsync 3-8)
        if (gsvsync_count >= 3 && gsvsync_count <= 8) {
            u32 phys_pc = cpuRegs.pc & 0x1FFFFFFFu;
            u32 insn[4] = {0};
            for (int i = 0; i < 4 && phys_pc + i*4 < Ps2MemSize::MainRam; i++)
                insn[i] = *reinterpret_cast<u32*>(eeMem->Main + phys_pc + i*4);
            Console.WriteLn("@@KERN_INIT@@ vsync=%d pc=%08x phys=%08x "
                "insn: %08x %08x %08x %08x "
                "DMAC_CTRL=%08x DMAC_ENABLE=%08x INTC_STAT=%08x INTC_MASK=%08x "
                "SIF_CTRL=%08x SIF_F200=%08x SIF_F230=%08x SIF_F260=%08x "
                "Status=%08x Cause=%08x ra=%08x s0=%08x s1=%08x gp=%08x",
                gsvsync_count, cpuRegs.pc, phys_pc,
                insn[0], insn[1], insn[2], insn[3],
                psHu32(0xE000), psHu32(0xF520), psHu32(0xF000), psHu32(0xF010),
                psHu32(0xF240), psHu32(0xF200), psHu32(0xF230), psHu32(0xF260),
                cpuRegs.CP0.r[12], cpuRegs.CP0.r[13],
                cpuRegs.GPR.r[31].UL[0], cpuRegs.GPR.r[16].UL[0],
                cpuRegs.GPR.r[17].UL[0], cpuRegs.GPR.r[28].UL[0]);
        }

    // [P12] @@NULLCALL_517C@@ – null functionポインタcall元 0x80005174 付近コードダンプ ONE-SHOT
    // JIT で ra=0x8000517c の NOP_SLED_START が発火 → 0x80005174 の JALR が null を呼んでいる
    // Removal condition: null ポインタcallroot causeafter determined
    {
        static bool s_nullcall_done = false;
        if (!s_nullcall_done && gsvsync_count == 3 && Cpu != &intCpu && eeMem) {
            s_nullcall_done = true;
            Console.WriteLn("@@NULLCALL_517C@@ 0x80005140-0x80005200 (null call context):");
            for (u32 a = 0x80005140u; a <= 0x80005200u; a += 16) {
                const u32 p = a & 0x1FFFFFFFu;
                Console.WriteLn("  [%08x] %08x %08x %08x %08x", a,
                    *reinterpret_cast<u32*>(eeMem->Main + p),
                    *reinterpret_cast<u32*>(eeMem->Main + p + 4),
                    *reinterpret_cast<u32*>(eeMem->Main + p + 8),
                    *reinterpret_cast<u32*>(eeMem->Main + p + 12));
            }
        }
    }

    // [P12] @@FUNC_1568_ENTRY@@ – 0x80001568 function入口 ONE-SHOT
    // JIT が 0x80001568 を呼んだ「call元 $ra」を捕捉する (JAL 0x80001570前の $ra=call元戻り先)
    // Removal condition: call経路after determined
    {
        static bool s_f1568_done = false;
        // 0x80001570 = JAL 0x800073E0 (sets $ra=0x80001578) → EEをloopに送り込む命令
        // 0x80001568 より先に 0x80001570 をチェック (JIT は 0x80001568 をskipして直接 0x80001570 に到達の可能性)
        if (!s_f1568_done && (cpuRegs.pc == 0x80001568u || cpuRegs.pc == 0x80001570u) && eeMem) {
            s_f1568_done = true;
            Console.WriteLn("@@FUNC_1568_ENTRY@@ FIRED at pc=%08x vsync=%d ra=%08x a0=%08x a1=%08x v0=%08x v1=%08x sp=%08x s0=%08x s1=%08x",
                cpuRegs.pc, gsvsync_count,
                cpuRegs.GPR.r[31].UL[0], // ra = call元の return address
                cpuRegs.GPR.r[4].UL[0],  // a0
                cpuRegs.GPR.r[5].UL[0],  // a1
                cpuRegs.GPR.r[2].UL[0],  // v0
                cpuRegs.GPR.r[3].UL[0],  // v1
                cpuRegs.GPR.r[29].UL[0], // sp
                cpuRegs.GPR.r[16].UL[0], // s0
                cpuRegs.GPR.r[17].UL[0]); // s1
            // call元 ($ra前後) のコードをダンプして JAL 命令の位置をverify
            const u32 caller_ra = cpuRegs.GPR.r[31].UL[0];
            if (caller_ra >= 0x80000000u && caller_ra < 0x802fffffu) {
                const u32 cp = (caller_ra - 0x10u) & 0x1FFFFFFFu;
                Console.WriteLn("@@FUNC_1568_ENTRY@@ caller code at ra-0x10=%08x:", caller_ra - 0x10);
                for (u32 i = 0; i < 10; i++)
                    Console.WriteLn("  [%08x] %08x", (caller_ra - 0x10u) + i*4,
                        *reinterpret_cast<u32*>(eeMem->Main + cp + i*4));
            }
        }
    }

    // [P12] @@CALLER_DB78_DUMP@@ – SQloop返却先 0x8000DB78 前後コードダンプ ONE-SHOT
    // JIT で $ra が 0x8000DB78 から 0x80001578 に変化するcause: 0x8000DB78 以降の分岐経路をverify
    // Removal condition: 誤ブランチ経路after identified
    {
        static bool s_caller_done = false;
        if (!s_caller_done && gsvsync_count == 3 && eeMem) {
            s_caller_done = true;
            Console.WriteLn("@@CALLER_DB78_DUMP@@ 0x8000DB40-0x8000DC40 (SQ loop caller context):");
            for (u32 a = 0x8000DB40u; a <= 0x8000DC40u; a += 16) {
                const u32 p = a & 0x1FFFFFFFu;
                Console.WriteLn("  [%08x] %08x %08x %08x %08x", a,
                    *reinterpret_cast<u32*>(eeMem->Main + p),
                    *reinterpret_cast<u32*>(eeMem->Main + p + 4),
                    *reinterpret_cast<u32*>(eeMem->Main + p + 8),
                    *reinterpret_cast<u32*>(eeMem->Main + p + 12));
            }
        }
    }

    // [P12] @@SIF0_STUCK_80001578@@ – EE 0x80001578 固着調査 ONE-SHOT
    // JIT+HLE=0 で vsync=7 に EE が 0x80001578 に固着し D5_CHCR=0 のroot cause調査
    // Removal condition: 誤ブランチ経路after identified
    {
        static bool s_sif0_stuck_done = false;
        if (!s_sif0_stuck_done && cpuRegs.pc == 0x80001578u && eeMem) {
            s_sif0_stuck_done = true;
            // code dump: 0x80001560-0x800015C0 (stuck loop area)
            Console.WriteLn("@@SIF0_STUCK_80001578@@ vsync=%d CODE DUMP:", gsvsync_count);
            for (u32 a = 0x80001560u; a <= 0x800015C0u; a += 16) {
                Console.WriteLn("  [%08x] %08x %08x %08x %08x", a,
                    *reinterpret_cast<u32*>(eeMem->Main + (a & 0x1FFFFFFFu)),
                    *reinterpret_cast<u32*>(eeMem->Main + (a & 0x1FFFFFFFu) + 4),
                    *reinterpret_cast<u32*>(eeMem->Main + (a & 0x1FFFFFFFu) + 8),
                    *reinterpret_cast<u32*>(eeMem->Main + (a & 0x1FFFFFFFu) + 12));
            }
            Console.WriteLn("@@SIF0_STUCK_80001578@@ REGS: "
                "v0=%08x v1=%08x a0=%08x a1=%08x "
                "s0=%08x s1=%08x s2=%08x s3=%08x "
                "ra=%08x sp=%08x gp=%08x",
                cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[3].UL[0],
                cpuRegs.GPR.r[4].UL[0], cpuRegs.GPR.r[5].UL[0],
                cpuRegs.GPR.r[16].UL[0], cpuRegs.GPR.r[17].UL[0],
                cpuRegs.GPR.r[18].UL[0], cpuRegs.GPR.r[19].UL[0],
                cpuRegs.GPR.r[31].UL[0], cpuRegs.GPR.r[29].UL[0],
                cpuRegs.GPR.r[28].UL[0]);
            Console.WriteLn("@@SIF0_STUCK_80001578@@ SIF/DMA: "
                "D5_CHCR=%08x D5_MADR=%08x D5_QWC=%08x D5_TADR=%08x "
                "D6_CHCR=%08x D6_MADR=%08x D6_QWC=%08x "
                "SIF_F200=%08x SIF_F210=%08x SIF_F220=%08x SIF_F230=%08x SIF_F240=%08x SIF_F260=%08x "
                "INTC_STAT=%08x INTC_MASK=%08x DMAC_CTRL=%08x",
                psHu32(0xC000), psHu32(0xC010), psHu32(0xC020), psHu32(0xC030),
                psHu32(0xC400), psHu32(0xC410), psHu32(0xC420),
                psHu32(0xF200), psHu32(0xF210), psHu32(0xF220),
                psHu32(0xF230), psHu32(0xF240), psHu32(0xF260),
                psHu32(0xF000), psHu32(0xF010), psHu32(0xE000));
            // IOP state
            Console.WriteLn("@@SIF0_STUCK_80001578@@ IOP: "
                "iop_pc=%08x iop_sp=%08x iop_ra=%08x iop_a0=%08x iop_v0=%08x",
                psxRegs.pc, psxRegs.GPR.r[29], psxRegs.GPR.r[31],
                psxRegs.GPR.r[4], psxRegs.GPR.r[2]);
        }
    }

    // [P11] @@EELOAD_82180_REGS@@ – 0x82180 ポーリングloop調査 ONE-SHOT x2
    // iter1: entry 時 a0/v1/*v1 + コードダンプ(0x82170-0x821A4)
    // iter2: 0x82188 (LW+AND 実行後) の v0 → vtlb LW 実戻り値をverify
    // Removal condition: 0x82180 停滞root causeafter determined
    {
        static bool s_entry_done = false;
        static bool s_post_done  = false;
        if (!s_entry_done && cpuRegs.pc == 0x82180u && eeMem) {
            s_entry_done = true;
            const u32 v0 = cpuRegs.GPR.r[2].UL[0];
            const u32 v1 = cpuRegs.GPR.r[3].UL[0];
            const u32 a0 = cpuRegs.GPR.r[4].UL[0];
            u32 star_v1 = 0;
            if (v1 >= 0x10000000u && v1 < 0x20000000u)
                star_v1 = psHu32(v1 & 0x0FFFFFFFu);
            else if (v1 < Ps2MemSize::MainRam)
                star_v1 = *reinterpret_cast<u32*>(eeMem->Main + v1);
            Console.WriteLn("@@EELOAD_82180_REGS@@ vsync=%d v1=%08x a0=%08x v0_pre=%08x *v1_pshu=%08x",
                gsvsync_count, v1, a0, v0, star_v1);
            // code dump: 0x82170-0x821A4 (14 words = LW..branch..target)
            Console.WriteLn("@@EELOAD_82180_CODE@@ "
                "[82170]=%08x [74]=%08x [78]=%08x [7c]=%08x "
                "[82180]=%08x [84]=%08x [88]=%08x [8c]=%08x "
                "[82190]=%08x [94]=%08x [98]=%08x [9c]=%08x "
                "[821a0]=%08x [a4]=%08x",
                *reinterpret_cast<u32*>(eeMem->Main + 0x82170),
                *reinterpret_cast<u32*>(eeMem->Main + 0x82174),
                *reinterpret_cast<u32*>(eeMem->Main + 0x82178),
                *reinterpret_cast<u32*>(eeMem->Main + 0x8217c),
                *reinterpret_cast<u32*>(eeMem->Main + 0x82180),
                *reinterpret_cast<u32*>(eeMem->Main + 0x82184),
                *reinterpret_cast<u32*>(eeMem->Main + 0x82188),
                *reinterpret_cast<u32*>(eeMem->Main + 0x8218c),
                *reinterpret_cast<u32*>(eeMem->Main + 0x82190),
                *reinterpret_cast<u32*>(eeMem->Main + 0x82194),
                *reinterpret_cast<u32*>(eeMem->Main + 0x82198),
                *reinterpret_cast<u32*>(eeMem->Main + 0x8219c),
                *reinterpret_cast<u32*>(eeMem->Main + 0x821a0),
                *reinterpret_cast<u32*>(eeMem->Main + 0x821a4));
        }
        if (!s_post_done && cpuRegs.pc == 0x82188u) {
            s_post_done = true;
            Console.WriteLn("@@EELOAD_82188_V0@@ vsync=%d v0_post_and=%08x (LW+AND 実行後)",
                gsvsync_count, cpuRegs.GPR.r[2].UL[0]);
        }
    }

    // [P11] @@EE_PC0_FIRST_HIT@@ – EE PC=0x00000000 を最初に踏んだ瞬間の ONE-SHOT registerダンプ
    // PC=0 は null functionポインタcallか JIT controlフローcorruptを示す。ra/epc/Cause でcall元を特定する。
    // Removal condition: PC=0 のroot cause（call元と理由）after determined
    {
        static bool s_pc0_done = false;
        if (!s_pc0_done && cpuRegs.pc == 0x00000000u && eeMem) {
            s_pc0_done = true;
            const u32 ra  = cpuRegs.GPR.r[31].UL[0];
            const u32 epc = cpuRegs.CP0.r[14];
            const u32 cause = cpuRegs.CP0.r[13];
            const u32 status = cpuRegs.CP0.r[12];
            const u32 v0  = cpuRegs.GPR.r[2].UL[0];
            const u32 v1  = cpuRegs.GPR.r[3].UL[0];
            const u32 a0  = cpuRegs.GPR.r[4].UL[0];
            const u32 sp  = cpuRegs.GPR.r[29].UL[0];
            Console.WriteLn("@@EE_PC0_FIRST_HIT@@ vsync=%d ra=%08x epc=%08x cause=%08x status=%08x "
                "v0=%08x v1=%08x a0=%08x sp=%08x iop_pc=%08x",
                gsvsync_count, ra, epc, cause, status, v0, v1, a0, sp, psxRegs.pc);
            // code at addr 0 (what would execute)
            Console.WriteLn("@@EE_PC0_FIRST_HIT@@ [000000]=%08x [04]=%08x [08]=%08x [0c]=%08x "
                "[10]=%08x [14]=%08x [18]=%08x [1c]=%08x",
                *reinterpret_cast<u32*>(eeMem->Main + 0x00), *reinterpret_cast<u32*>(eeMem->Main + 0x04),
                *reinterpret_cast<u32*>(eeMem->Main + 0x08), *reinterpret_cast<u32*>(eeMem->Main + 0x0c),
                *reinterpret_cast<u32*>(eeMem->Main + 0x10), *reinterpret_cast<u32*>(eeMem->Main + 0x14),
                *reinterpret_cast<u32*>(eeMem->Main + 0x18), *reinterpret_cast<u32*>(eeMem->Main + 0x1c));
            // code at ra-8 (caller context)
            if (ra >= 8u && ra < Ps2MemSize::MainRam) {
                Console.WriteLn("@@EE_PC0_FIRST_HIT@@ caller[ra-8]=%08x [ra-4]=%08x [ra]=%08x [ra+4]=%08x",
                    *reinterpret_cast<u32*>(eeMem->Main + ra - 8),
                    *reinterpret_cast<u32*>(eeMem->Main + ra - 4),
                    *reinterpret_cast<u32*>(eeMem->Main + ra),
                    *reinterpret_cast<u32*>(eeMem->Main + ra + 4));
            }
        }
    }

    // [TEMP_DIAG] iter179: @@IOP_16C38_CODE@@ – IOP stuck at 0x16C38 の待機コードをダンプ
    // Removal condition: IOP 待機cause（割り込み種別）after determineddelete
    if (gsvsync_count == 60) {
        Console.WriteLn("@@IOP_16C38_CODE@@ "
            "w0=%08x w1=%08x w2=%08x w3=%08x w4=%08x w5=%08x w6=%08x w7=%08x",
            iopMemRead32(0x16C30), iopMemRead32(0x16C34),
            iopMemRead32(0x16C38), iopMemRead32(0x16C3C),
            iopMemRead32(0x16C40), iopMemRead32(0x16C44),
            iopMemRead32(0x16C48), iopMemRead32(0x16C4C));
    }

    // [TEMP_DIAG] iter07: one-shot MIPS opcode read, extended to find BEQ (9FC41040-9FC4106C)
    // Removal condition: BEQ target after determineddelete
    if (gsvsync_count == 2) {
        Console.WriteLn("[TEMP_DIAG] @@BIOS_LOOP@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x",
            memRead32(0x9FC41040), memRead32(0x9FC41044),
            memRead32(0x9FC41048), memRead32(0x9FC4104C),
            memRead32(0x9FC41050), memRead32(0x9FC41054),
            memRead32(0x9FC41058), memRead32(0x9FC4105C),
            memRead32(0x9FC41060), memRead32(0x9FC41064),
            memRead32(0x9FC41068), memRead32(0x9FC4106C));

        // [TEMP_DIAG] iter08: dump 9FC433E8-9FC43424 (EE PC hot zone in 9FC43320 subroutine)
        // Removal condition: 9FC433E8 loop脱出conditionafter identifieddelete
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43320_LO@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x",
            memRead32(0x9FC433E8), memRead32(0x9FC433EC),
            memRead32(0x9FC433F0), memRead32(0x9FC433F4),
            memRead32(0x9FC433F8), memRead32(0x9FC433FC),
            memRead32(0x9FC43400), memRead32(0x9FC43404),
            memRead32(0x9FC43408), memRead32(0x9FC4340C),
            memRead32(0x9FC43410), memRead32(0x9FC43414),
            memRead32(0x9FC43418), memRead32(0x9FC4341C),
            memRead32(0x9FC43420), memRead32(0x9FC43424));

        // [TEMP_DIAG] iter08: dump 9FC43910-9FC43930 (EE PC hot zone near 9FC43918/43920)
        // Removal condition: 同上
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43320_HI@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x",
            memRead32(0x9FC43910), memRead32(0x9FC43914),
            memRead32(0x9FC43918), memRead32(0x9FC4391C),
            memRead32(0x9FC43920), memRead32(0x9FC43924),
            memRead32(0x9FC43928), memRead32(0x9FC4392C));

        // [TEMP_DIAG] iter09: dump 9FC434C0-9FC43500 (BNE loop target from 9FC43928 = infinite loop body)
        // Removal condition: 無限looproot causeafter identifieddelete
        Console.WriteLn("[TEMP_DIAG] @@BIOS_434C0_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x",
            memRead32(0x9FC434C0), memRead32(0x9FC434C4),
            memRead32(0x9FC434C8), memRead32(0x9FC434CC),
            memRead32(0x9FC434D0), memRead32(0x9FC434D4),
            memRead32(0x9FC434D8), memRead32(0x9FC434DC),
            memRead32(0x9FC434E0), memRead32(0x9FC434E4),
            memRead32(0x9FC434E8), memRead32(0x9FC434EC),
            memRead32(0x9FC434F0), memRead32(0x9FC434F4),
            memRead32(0x9FC434F8), memRead32(0x9FC434FC));

        // [TEMP_DIAG] iter10: dump 9FC43120-9FC4315C (48回呼ばれるfunction = 無限loopのroot cause候補)
        // Removal condition: 9FC43120 内の無限looproot causeafter identifieddelete
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43120_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x",
            memRead32(0x9FC43120), memRead32(0x9FC43124),
            memRead32(0x9FC43128), memRead32(0x9FC4312C),
            memRead32(0x9FC43130), memRead32(0x9FC43134),
            memRead32(0x9FC43138), memRead32(0x9FC4313C),
            memRead32(0x9FC43140), memRead32(0x9FC43144),
            memRead32(0x9FC43148), memRead32(0x9FC4314C),
            memRead32(0x9FC43150), memRead32(0x9FC43154),
            memRead32(0x9FC43158), memRead32(0x9FC4315C));

        // [TEMP_DIAG] iter11: dump 9FC43320-9FC4337C (BIOS main subroutine entry = outer loop)
        // Removal condition: 外側loop脱出conditionafter identifieddelete
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43320_ENTRY@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x "
            "j0=%08x j1=%08x j2=%08x j3=%08x j4=%08x j5=%08x j6=%08x j7=%08x",
            memRead32(0x9FC43320), memRead32(0x9FC43324),
            memRead32(0x9FC43328), memRead32(0x9FC4332C),
            memRead32(0x9FC43330), memRead32(0x9FC43334),
            memRead32(0x9FC43338), memRead32(0x9FC4333C),
            memRead32(0x9FC43340), memRead32(0x9FC43344),
            memRead32(0x9FC43348), memRead32(0x9FC4334C),
            memRead32(0x9FC43350), memRead32(0x9FC43354),
            memRead32(0x9FC43358), memRead32(0x9FC4335C),
            memRead32(0x9FC43360), memRead32(0x9FC43364),
            memRead32(0x9FC43368), memRead32(0x9FC4336C),
            memRead32(0x9FC43370), memRead32(0x9FC43374),
            memRead32(0x9FC43378), memRead32(0x9FC4337C));

        // [TEMP_DIAG] iter12: dump 9FC43380-9FC433DC (loop body after SIO init = 0xBF801040 read location)
        // Removal condition: 0xBF801040 読み取り命令after identifieddelete
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43380_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x "
            "j0=%08x j1=%08x j2=%08x j3=%08x j4=%08x j5=%08x",
            memRead32(0x9FC43380), memRead32(0x9FC43384),
            memRead32(0x9FC43388), memRead32(0x9FC4338C),
            memRead32(0x9FC43390), memRead32(0x9FC43394),
            memRead32(0x9FC43398), memRead32(0x9FC4339C),
            memRead32(0x9FC433A0), memRead32(0x9FC433A4),
            memRead32(0x9FC433A8), memRead32(0x9FC433AC),
            memRead32(0x9FC433B0), memRead32(0x9FC433B4),
            memRead32(0x9FC433B8), memRead32(0x9FC433BC),
            memRead32(0x9FC433C0), memRead32(0x9FC433C4),
            memRead32(0x9FC433C8), memRead32(0x9FC433CC),
            memRead32(0x9FC433D0), memRead32(0x9FC433D4));

        // [TEMP_DIAG] iter13: dump 9FC43420-9FC43464 (function called from 9FC43918, hot at 9FC43450)
        // Removal condition: 無限looproot causeafter identifieddelete
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43420_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x "
            "j0=%08x",
            memRead32(0x9FC43420), memRead32(0x9FC43424),
            memRead32(0x9FC43428), memRead32(0x9FC4342C),
            memRead32(0x9FC43430), memRead32(0x9FC43434),
            memRead32(0x9FC43438), memRead32(0x9FC4343C),
            memRead32(0x9FC43440), memRead32(0x9FC43444),
            memRead32(0x9FC43448), memRead32(0x9FC4344C),
            memRead32(0x9FC43450), memRead32(0x9FC43454),
            memRead32(0x9FC43458), memRead32(0x9FC4345C),
            memRead32(0x9FC43460));

        // [TEMP_DIAG] iter14: dump 9FC43930-9FC43990 (after printf null-term, outer loop?)
        // Removal condition: 100s 外側loopの backward branch after identifieddelete
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43930_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x "
            "j0=%08x j1=%08x j2=%08x j3=%08x j4=%08x j5=%08x",
            memRead32(0x9FC43930), memRead32(0x9FC43934),
            memRead32(0x9FC43938), memRead32(0x9FC4393C),
            memRead32(0x9FC43940), memRead32(0x9FC43944),
            memRead32(0x9FC43948), memRead32(0x9FC4394C),
            memRead32(0x9FC43950), memRead32(0x9FC43954),
            memRead32(0x9FC43958), memRead32(0x9FC4395C),
            memRead32(0x9FC43960), memRead32(0x9FC43964),
            memRead32(0x9FC43968), memRead32(0x9FC4396C),
            memRead32(0x9FC43970), memRead32(0x9FC43974),
            memRead32(0x9FC43978), memRead32(0x9FC4397C),
            memRead32(0x9FC43980), memRead32(0x9FC43984));

        // [TEMP_DIAG] iter15: dump 9FC41114-9FC41160 (after JAL 9FC43320 returns = outer loop)
        // Removal condition: 外側loop脱出conditionafter identifieddelete
        Console.WriteLn("[TEMP_DIAG] @@BIOS_41114_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x "
            "j0=%08x j1=%08x j2=%08x j3=%08x",
            memRead32(0x9FC41114), memRead32(0x9FC41118),
            memRead32(0x9FC4111C), memRead32(0x9FC41120),
            memRead32(0x9FC41124), memRead32(0x9FC41128),
            memRead32(0x9FC4112C), memRead32(0x9FC41130),
            memRead32(0x9FC41134), memRead32(0x9FC41138),
            memRead32(0x9FC4113C), memRead32(0x9FC41140),
            memRead32(0x9FC41144), memRead32(0x9FC41148),
            memRead32(0x9FC4114C), memRead32(0x9FC41150),
            memRead32(0x9FC41154), memRead32(0x9FC41158),
            memRead32(0x9FC4115C), memRead32(0x9FC41160));

        // [TEMP_DIAG] iter16: dump 9FC41164-9FC411B0 (after TLB_init 2nd call)
        // Removal condition: 外側loop backward branch after identifieddelete
        Console.WriteLn("[TEMP_DIAG] @@BIOS_41164_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x "
            "j0=%08x j1=%08x j2=%08x j3=%08x",
            memRead32(0x9FC41164), memRead32(0x9FC41168),
            memRead32(0x9FC4116C), memRead32(0x9FC41170),
            memRead32(0x9FC41174), memRead32(0x9FC41178),
            memRead32(0x9FC4117C), memRead32(0x9FC41180),
            memRead32(0x9FC41184), memRead32(0x9FC41188),
            memRead32(0x9FC4118C), memRead32(0x9FC41190),
            memRead32(0x9FC41194), memRead32(0x9FC41198),
            memRead32(0x9FC4119C), memRead32(0x9FC411A0),
            memRead32(0x9FC411A4), memRead32(0x9FC411A8),
            memRead32(0x9FC411AC), memRead32(0x9FC411B0));

        // [TEMP_DIAG] iter17: dump 9FC411B4-9FC41200 (after TLB_init 2nd call, bit manipulation)
        // Removal condition: 外側loop backward branch after identifieddelete
        Console.WriteLn("[TEMP_DIAG] @@BIOS_411B4_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x "
            "j0=%08x j1=%08x j2=%08x j3=%08x",
            memRead32(0x9FC411B4), memRead32(0x9FC411B8),
            memRead32(0x9FC411BC), memRead32(0x9FC411C0),
            memRead32(0x9FC411C4), memRead32(0x9FC411C8),
            memRead32(0x9FC411CC), memRead32(0x9FC411D0),
            memRead32(0x9FC411D4), memRead32(0x9FC411D8),
            memRead32(0x9FC411DC), memRead32(0x9FC411E0),
            memRead32(0x9FC411E4), memRead32(0x9FC411E8),
            memRead32(0x9FC411EC), memRead32(0x9FC411F0),
            memRead32(0x9FC411F4), memRead32(0x9FC411F8),
            memRead32(0x9FC411FC), memRead32(0x9FC41200));

        // [TEMP_DIAG] iter18: dump 9FC433D8-9FC43424 (SIO write fn: poll=0xB000F130 bit15, write=0xB000F180) CONFIRMED
        // Removal condition: 解析完了済み - 次回クリーンアップ時delete
        Console.WriteLn("[TEMP_DIAG] @@BIOS_433D8_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x "
            "j0=%08x j1=%08x j2=%08x j3=%08x",
            memRead32(0x9FC433D8), memRead32(0x9FC433DC),
            memRead32(0x9FC433E0), memRead32(0x9FC433E4),
            memRead32(0x9FC433E8), memRead32(0x9FC433EC),
            memRead32(0x9FC433F0), memRead32(0x9FC433F4),
            memRead32(0x9FC433F8), memRead32(0x9FC433FC),
            memRead32(0x9FC43400), memRead32(0x9FC43404),
            memRead32(0x9FC43408), memRead32(0x9FC4340C),
            memRead32(0x9FC43410), memRead32(0x9FC43414),
            memRead32(0x9FC43418), memRead32(0x9FC4341C),
            memRead32(0x9FC43420), memRead32(0x9FC43424));

        // [TEMP_DIAG] iter19: 9FC4124C=func epilogue(JR ra), 9FC41268=new fn entry CONFIRMED
        // (kept for reference,Removal condition: 次回クリーンアップ)
        Console.WriteLn("[TEMP_DIAG] @@BIOS_4124C_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x "
            "j0=%08x j1=%08x j2=%08x j3=%08x",
            memRead32(0x9FC4124C), memRead32(0x9FC41250),
            memRead32(0x9FC41254), memRead32(0x9FC41258),
            memRead32(0x9FC4125C), memRead32(0x9FC41260),
            memRead32(0x9FC41264), memRead32(0x9FC41268),
            memRead32(0x9FC4126C), memRead32(0x9FC41270),
            memRead32(0x9FC41274), memRead32(0x9FC41278),
            memRead32(0x9FC4127C), memRead32(0x9FC41280),
            memRead32(0x9FC41284), memRead32(0x9FC41288),
            memRead32(0x9FC4128C), memRead32(0x9FC41290),
            memRead32(0x9FC41294), memRead32(0x9FC41298));

        // [TEMP_DIAG] iter20: fmt@9FC43E18="# Initialize memory (rev:%d.%02d" CONFIRMED (LE)
        // (kept for reference, Removal condition: 次回クリーンアップ)
        {
            u32 w0=memRead32(0x9FC43E18), w1=memRead32(0x9FC43E1C),
                w2=memRead32(0x9FC43E20), w3=memRead32(0x9FC43E24),
                w4=memRead32(0x9FC43E28), w5=memRead32(0x9FC43E2C),
                w6=memRead32(0x9FC43E30), w7=memRead32(0x9FC43E34);
            Console.WriteLn("[TEMP_DIAG] @@BIOS_FMT_3E18@@ "
                "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
                "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                (w0>>24)&0xFF, (w0>>16)&0xFF, (w0>>8)&0xFF, w0&0xFF,
                (w1>>24)&0xFF, (w1>>16)&0xFF, (w1>>8)&0xFF, w1&0xFF,
                (w2>>24)&0xFF, (w2>>16)&0xFF, (w2>>8)&0xFF, w2&0xFF,
                (w3>>24)&0xFF, (w3>>16)&0xFF, (w3>>8)&0xFF, w3&0xFF,
                (w4>>24)&0xFF, (w4>>16)&0xFF, (w4>>8)&0xFF, w4&0xFF,
                (w5>>24)&0xFF, (w5>>16)&0xFF, (w5>>8)&0xFF, w5&0xFF,
                (w6>>24)&0xFF, (w6>>16)&0xFF, (w6>>8)&0xFF, w6&0xFF,
                (w7>>24)&0xFF, (w7>>16)&0xFF, (w7>>8)&0xFF, w7&0xFF);
        }

        // [TEMP_DIAG] iter21: 9FC41000=fn prologue (ADDIU sp,-0x60); 9FC4101C=JAL 9FC42AE8 CONFIRMED
        // (kept for reference, Removal condition: 次回クリーンアップ)
        Console.WriteLn("[TEMP_DIAG] @@BIOS_41000_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x i5=%08x i6=%08x i7=%08x "
            "i8=%08x i9=%08x iA=%08x iB=%08x iC=%08x iD=%08x iE=%08x iF=%08x",
            memRead32(0x9FC41000), memRead32(0x9FC41004),
            memRead32(0x9FC41008), memRead32(0x9FC4100C),
            memRead32(0x9FC41010), memRead32(0x9FC41014),
            memRead32(0x9FC41018), memRead32(0x9FC4101C),
            memRead32(0x9FC41020), memRead32(0x9FC41024),
            memRead32(0x9FC41028), memRead32(0x9FC4102C),
            memRead32(0x9FC41030), memRead32(0x9FC41034),
            memRead32(0x9FC41038), memRead32(0x9FC4103C));

        // [TEMP_DIAG] iter22: 9FC40F80-9FC40FFC = DATA table (not code!) CONFIRMED
        // (kept for reference, Removal condition: 次回クリーンアップ)
        // (probe body removed to save space - result confirmed as data table)

        // [TEMP_DIAG] iter23: fmt string ~60 chars ending ")\n\0\0", null at 9FC43E54 CONFIRMED
        // (kept for reference, Removal condition: 次回クリーンアップ)

        // [TEMP_DIAG] iter24: sp=0x70003DE0 confirmed as char_write frame at pc=9FC43450 CONFIRMED
        // Stack: char_write(0x10)+printf(0x100)+9FC41000(0x60) → saved_ra at sp+0x160
        // Removal condition: 外側loop caller addressafter identifieddelete

        // [TEMP_DIAG] iter25: sp+0x160+ all zero - stack frame calc may be off CONFIRMED
        // Stack top must be near sp+0x160, outer frame must be very small

        // [TEMP_DIAG] iter26: scan SPR [sp-0x60 .. sp+0x200] for 9FC4xxxx return addr pattern
        // Removal condition: 外側loopcall元addressafter identifieddelete
        {
            u32 sp = cpuRegs.GPR.r[29].UL[0];
            // Scan EE SPR (0x70000000-0x70003FFF) range above current sp
            // Look for values matching 0x9FC4xxxx pattern (= BIOS return addresses)
            u32 found_addr = 0, found_val = 0, found_count = 0;
            for (u32 off = 0; off <= 0x200; off += 4) {
                u32 addr = sp + off;
                if (addr > 0x70003FFC) break;
                u32 val = memRead32(addr);
                if ((val & 0xFFF00000) == 0x9FC40000 || (val & 0xFFF00000) == 0x9FC00000) {
                    if (found_count < 4) {
                        Console.WriteLn("[TEMP_DIAG] @@SPR_RA_SCAN@@ "
                            "sp=%08x off=+%03x addr=%08x val=%08x",
                            sp, off, addr, val);
                    }
                    found_count++;
                }
            }
            if (found_count == 0) {
                Console.WriteLn("[TEMP_DIAG] @@SPR_RA_SCAN@@ sp=%08x pc=%08x no_9FC4_found in +0..+200",
                    sp, cpuRegs.pc);
            }
        }

        // [TEMP_DIAG] iter28: scan BIOS ROM 9FC00000-9FC7FFFC for JAL 0x9FC41000 (encoding=0x0FF10400)
        // to find all callers of 9FC41000 without relying on stack
        {
            const u32 JAL_9FC41000 = 0x0FF10400u;
            u32 hit_count = 0;
            for (u32 addr = 0x9FC00000u; addr <= 0x9FC7FFFCu && hit_count < 8; addr += 4) {
                if (memRead32(addr) == JAL_9FC41000) {
                    Console.WriteLn("[TEMP_DIAG] @@JAL_41000_SCAN@@ "
                        "jal_at=%08x delay=%08x ra_target=%08x",
                        addr, memRead32(addr + 4), addr + 8);
                    hit_count++;
                }
            }
            if (hit_count == 0) {
                Console.WriteLn("[TEMP_DIAG] @@JAL_41000_SCAN@@ no JAL 0x9FC41000 found in ROM");
            } else {
                Console.WriteLn("[TEMP_DIAG] @@JAL_41000_SCAN@@ total_hits=%u", hit_count);
            }
        }

        // [TEMP_DIAG] iter27: dump BIOS ROM near ra candidates to confirm JAL 0x9FC41000
        // (confirmed: both 0x9FC43C10 and 0x9FC43E58 are data, not return addresses)
        {
            // dump 9FC43BF8-9FC43C28 (9 words, includes JAL candidate at 9FC43C08)
            Console.WriteLn("[TEMP_DIAG] @@BIOS_43BF8_CODE@@ "
                "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x "
                "i5=%08x i6=%08x i7=%08x i8=%08x",
                memRead32(0x9FC43BF8), memRead32(0x9FC43BFC),
                memRead32(0x9FC43C00), memRead32(0x9FC43C04),
                memRead32(0x9FC43C08), // JAL candidate
                memRead32(0x9FC43C0C), // delay slot
                memRead32(0x9FC43C10), // instruction after delay slot (= returned-to addr)
                memRead32(0x9FC43C14),
                memRead32(0x9FC43C18));
            // dump 9FC43E40-9FC43E68 (11 words, includes JAL candidate at 9FC43E50)
            Console.WriteLn("[TEMP_DIAG] @@BIOS_43E40_CODE@@ "
                "j0=%08x j1=%08x j2=%08x j3=%08x j4=%08x "
                "j5=%08x j6=%08x j7=%08x j8=%08x j9=%08x",
                memRead32(0x9FC43E40), memRead32(0x9FC43E44),
                memRead32(0x9FC43E48), memRead32(0x9FC43E4C),
                memRead32(0x9FC43E50), // JAL candidate
                memRead32(0x9FC43E54), // delay slot
                memRead32(0x9FC43E58), // instruction at returned-to addr
                memRead32(0x9FC43E5C),
                memRead32(0x9FC43E60),
                memRead32(0x9FC43E64));
        }
    }

    // [TEMP_DIAG] @@EXECPS2_DUMP@@ — one-shot dump of ExecPS2 loop at 0x158800+
    {
        static bool s_execps2_dumped = false;
        if (!s_execps2_dumped && gsvsync_count == 5 && cpuRegs.pc >= 0x158800 && cpuRegs.pc < 0x159000) {
            s_execps2_dumped = true;
            Console.WriteLn("@@EXECPS2_DUMP@@ pc=%08x v0=%08x v1=%08x a0=%08x a1=%08x sp=%08x ra=%08x epc=%08x",
                cpuRegs.pc, cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[3].UL[0],
                cpuRegs.GPR.r[4].UL[0], cpuRegs.GPR.r[5].UL[0],
                cpuRegs.GPR.r[29].UL[0], cpuRegs.GPR.r[31].UL[0], cpuRegs.CP0.r[14]);
            // Dump 32 instructions starting at 0x158810
            for (u32 addr = 0x158810; addr < 0x158910; addr += 16) {
                Console.WriteLn("@@EXECPS2_CODE@@ %08x: %08x %08x %08x %08x",
                    addr, memRead32(addr), memRead32(addr+4), memRead32(addr+8), memRead32(addr+12));
            }
            // Also check SBUS F240 register state
            Console.WriteLn("@@EXECPS2_SBUS@@ F240=%08x F260=%08x SIF0_CHCR=%08x SIF1_CHCR=%08x",
                psHu32(0xF240), psHu32(0xF260),
                psHu32(0xC000), psHu32(0xC400));
        }
    }

    // [iter650 ハック除去] @@BIOS_MEMLOOP_FIX@@ statechangeを撤去 – 観測のみ
    // 未fixJITバグ: 0x9FC42210でs2にscratchpadaddressが入る
    // root cause調査のためstatechangeを停止し、JITが自力でhandlingできるか観測
    {
        static bool s_memloop_logged = false;
        if (!s_memloop_logged && cpuRegs.pc >= 0x9FC41A20u && cpuRegs.pc <= 0x9FC41A30u) {
            u32 s2_val = cpuRegs.GPR.r[18].UL[0];
            s_memloop_logged = true;
            Console.WriteLn("@@BIOS_MEMLOOP_OBS@@ pc=%08x s2=%08x sp28=%08x (JITバグ観測; statechange停止)",
                cpuRegs.pc, s2_val, memRead32(cpuRegs.GPR.r[29].UL[0] + 0x28));
            // [ハック除去] memWrite32/cpuRegs.GPR.r[18] 書き換えを停止
        }
    }

    // [iter222] @@BIOS_D90_DIAG@@ 9FC00D90functionのfailcause診断
    // 9FC00C58でスタック時、sp周辺とregisterをダンプ
    // Removal condition: 9FC00D90のfailcauseが特定された後
    {
        static bool s_d90_dumped = false;
        if (!s_d90_dumped && cpuRegs.pc == 0x9FC00C58u) {
            s_d90_dumped = true;
            u32 sp = cpuRegs.GPR.r[29].UL[0];
            Console.WriteLn("@@BIOS_D90_DIAG@@ pc=9FC00C58 sp=%08x a0=%08x a1=%08x v0=%08x v1=%08x ra=%08x",
                sp, cpuRegs.GPR.r[4].UL[0], cpuRegs.GPR.r[5].UL[0],
                cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[3].UL[0], cpuRegs.GPR.r[31].UL[0]);
            // Dump stack: sp+0 to sp+48
            if (sp >= 0x70000000u && sp < 0x70004000u) {
                Console.WriteLn("@@BIOS_D90_STACK@@ sp[0]=%08x sp[4]=%08x sp[8]=%08x sp[C]=%08x sp[10]=%08x sp[14]=%08x sp[18]=%08x sp[1C]=%08x",
                    memRead32(sp+0), memRead32(sp+4), memRead32(sp+8), memRead32(sp+0xC),
                    memRead32(sp+0x10), memRead32(sp+0x14), memRead32(sp+0x18), memRead32(sp+0x1C));
                Console.WriteLn("@@BIOS_D90_STACK2@@ sp[20]=%08x sp[24]=%08x sp[28]=%08x sp[2C]=%08x",
                    memRead32(sp+0x20), memRead32(sp+0x24), memRead32(sp+0x28), memRead32(sp+0x2C));
            }
            // Read BIOS ROM string at 0xBFC01CA8 (9FC00D90's a1 arg)
            Console.WriteLn("@@BIOS_ROM_STR@@ bfc01ca8: %02x %02x %02x %02x %02x %02x %02x %02x",
                memRead8(0xBFC01CA8), memRead8(0xBFC01CA9), memRead8(0xBFC01CAA), memRead8(0xBFC01CAB),
                memRead8(0xBFC01CAC), memRead8(0xBFC01CAD), memRead8(0xBFC01CAE), memRead8(0xBFC01CAF));
            // Direct ROMDIR read via memRead32 — verify BIOS ROM data access
            Console.WriteLn("@@BIOS_ROMDIR_DIRECT@@ 9fc02740=%08x 9fc02744=%08x 9fc02d80=%08x 9fc02d84=%08x bfc02740=%08x bfc02d80=%08x",
                memRead32(0x9FC02740), memRead32(0x9FC02744),
                memRead32(0x9FC02D80), memRead32(0x9FC02D84),
                memRead32(0xBFC02740), memRead32(0xBFC02D80));
            // Also check LB from KSEG0 vs KSEG1
            Console.WriteLn("@@BIOS_LB_TEST@@ kseg0_9fc01ca8=%02x kseg1_bfc01ca8=%02x kseg0_9fc02740_b0=%02x",
                memRead8(0x9FC01CA8), memRead8(0xBFC01CA8), memRead8(0x9FC02740));
        }
    }

    // [iter650 ハック除去] @@BIOS_ROMDIR_FIX@@ statechangeを撤去 – 観測のみ
    // 未fixJITバグ: 0x9FC00D90 ROMDIR検索functionで比較/分岐がfailする
    // statechange停止: C代行ROMDIR検索 + PCforceリダイレクトを停止し、JIT自力での進行を観測
    {
        static bool s_romdir_obs_done = false;
        if (!s_romdir_obs_done && cpuRegs.pc == 0x9FC00C58u) {
            s_romdir_obs_done = true;
            u32 sp = cpuRegs.GPR.r[29].UL[0];
            u32 romdir_base = memRead32(sp + 4);  // sp[4] = ROMDIR table ptr
            u32 rom_base = memRead32(sp + 0);     // sp[0] = ROM base

            // C側でROMMDIR走査 (観測のみ、statechangeなし)
            u32 search0 = 0x4E52454B;  // "KERN"
            u32 search1 = 0x00004C45;  // "EL\0\0"
            bool found = false;
            u32 entry_ptr = 0;
            for (u32 a = romdir_base; a < romdir_base + 0x1000; a += 16) {
                u32 w0 = memRead32(a);
                if (w0 == 0) break;
                u32 w1 = memRead32(a + 4);
                if (w0 == search0 && w1 == search1) {
                    entry_ptr = a;
                    found = true;
                    break;
                }
            }
            // [ハック除去] memWrite32/cpuRegs.pc/GPR/CP0 への書き込みを停止
            // JIT v0 の実際の値 (not found = 0 or ptr) を観測
            Console.WriteLn("@@BIOS_ROMDIR_OBS@@ JITバグ観測: pc=9FC00C58 v0(JIT_result)=%08x "
                "romdir_base=%08x rom_base=%08x C_search_found=%d entry=%08x",
                cpuRegs.GPR.r[2].UL[0], romdir_base, rom_base, found ? 1 : 0, entry_ptr);
            Console.WriteLn("@@BIOS_ROMDIR_OBS@@ JIT sp=%08x a0=%08x a1=%08x v0=%08x v1=%08x",
                sp, cpuRegs.GPR.r[4].UL[0], cpuRegs.GPR.r[5].UL[0],
                cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[3].UL[0]);
        }
    }

    // @@EE_PC_SAMPLE@@ - Log EE PC + IOP PC every 600 vsyncs (~10s) to identify stuck loops
    // [iter105] added iop_pc to diagnose IOP boot progress
    // [iter144] added EE RAM instruction dump at current PC
    // [iter230] vsync=3 早期例外ベクタダンプ
    if (gsvsync_count == 3) {
        Console.WriteLn("@@EARLY_EXCVEC@@ vsync=3 bev=%d pc=%08x 80000180: %08x %08x %08x %08x 80000080: %08x %08x",
            (int)cpuRegs.CP0.n.Status.b.BEV, cpuRegs.pc,
            memRead32(0x80000180u), memRead32(0x80000184u), memRead32(0x80000188u), memRead32(0x8000018Cu),
            memRead32(0x80000080u), memRead32(0x80000084u));
    }
    // [iter643] @@PS1_ECB_DELIVER@@ HLE: PS1DRV パッチ版 TestEvent への VBlank 配送
    // 根拠: PS1DRV は BIOS TestEvent を独自implでパッチ済み。パッチ版は
    //       0x1e8c: LW t1, 4(ecb); 0x1e94: BNE a0, t1, -3 → ECB.word[1] == idx まで spin
    //       配送メカニズム: VBlank 割り込みhandlerが ECB.word[1] = idx (evid & 0xffff) を書く
    //       IEc=0 で割り込みhandler未実行 → ECB.word[1] 永続 0x2000 → TestEvent 脱出不可
    //       fix: ECB.word[1] が "待機値" (0x2000=mode) のとき、idx を書いて配送をシミュレート
    // [iter650 ハック除去] @@PS1_ECB_DELIVER@@ statechangeを撤去 – 観測のみ
    // 未fixJITバグ: IOP VBlank ISR未発火でECBシグナルが自然に配送されない
    // 撤去理由: BIOS自然進行のためHLE注入を停止。IOP JITのTestEvent自力配送を待つ。
    if (Cpu != &intCpu) {
        static bool s_ecb_obs_logged = false;
        if (!s_ecb_obs_logged && gsvsync_count <= 30) {
            u32 pcb_base_va = iopMemRead32(0x120u);
            u32 pcb_base = pcb_base_va & 0x1FFFFFFFu;
            if (pcb_base > 0x1000u && pcb_base < 0x1E0000u) {
                const u32 evid1 = iopMemRead32(0x56ee4u);
                const u32 evid2 = iopMemRead32(0x56ef0u);
                const u32 evid3 = iopMemRead32(0x56eecu);
                for (u32 evid : {evid1, evid2, evid3}) {
                    if (!evid) continue;
                    u32 idx = evid & 0x0000FFFFu;
                    if (idx >= 0x200u) continue;
                    u32 ecb = pcb_base + idx * 0x1Cu;
                    u32 mode = iopMemRead32(ecb + 4u);
                    Console.WriteLn("@@PS1_ECB_OBS@@ vsync=%d evid=%08x ecb=%08x idx=%d mode=%08x (statechange停止)",
                        gsvsync_count, evid, ecb, idx, mode);
                    // [ハック除去] iopMemWrite32(ecb + 4u, 0x4000u) を停止
                }
                s_ecb_obs_logged = true;
            }
        }
    }
    // [P11] @@TESTEVENT_IDS@@ vsync==600 ONE-SHOT: IOP TestEvent event IDs from GPU dispatch loop
    // 根拠: 0x31100 が TestEvent(MEM[0x56ee4]/MEM[0x56ef0]) を呼ぶがalways -1 返却。
    //       イベントID の実体と未シグナルのcauseを特定する。
    // Removal condition: TestEvent シグナルされ 0x313b4 (display init) 到達after confirmed
    {
        static bool s_teid_done = false;
        if (!s_teid_done && gsvsync_count >= 600) {
            s_teid_done = true;
            const u32 evid1 = iopMemRead32(0x56ee4u);
            const u32 evid2 = iopMemRead32(0x56ef0u);
            const u32 evid3 = iopMemRead32(0x4282cu);  // from 0x30c40 init code
            Console.WriteLn("@@TESTEVENT_IDS@@ vsync=%d evid[56ee4]=%08x evid[56ef0]=%08x evid[4282c]=%08x",
                gsvsync_count, evid1, evid2, evid3);
            auto dump_ecb = [](u32 evid, const char* tag) {
                if (!evid) { Console.WriteLn("  %s: evid=NULL", tag); return; }
                u32 pcb_base_va = iopMemRead32(0x120u);
                u32 pcb_base = pcb_base_va & 0x1FFFFFFFu;  // VA→phys
                Console.WriteLn("  %s: evid=%08x pcb_base_va=%08x phys=%08x", tag, evid, pcb_base_va, pcb_base);
                u32 idx = evid & 0x0000FFFFu;
                if (idx < 0x200u && pcb_base > 0 && pcb_base < 0x200000u) {
                    // ECB size in PS1 IOP BIOS: 28 bytes (7 u32s)
                    // struct: status, class, spec, mode, count, handler, next
                    u32 ecb = pcb_base + idx * 0x1Cu;  // 0x1c = 28 bytes per ECB
                    Console.WriteLn("  %s: ECB[idx=%d]@%08x: %08x %08x %08x %08x %08x %08x %08x",
                        tag, idx, ecb,
                        iopMemRead32(ecb), iopMemRead32(ecb+4), iopMemRead32(ecb+8), iopMemRead32(ecb+12),
                        iopMemRead32(ecb+16), iopMemRead32(ecb+20), iopMemRead32(ecb+24));
                }
            };
            dump_ecb(evid1, "evid1[56ee4]");
            dump_ecb(evid2, "evid2[56ef0]");
            dump_ecb(evid3, "evid3[4282c]");
            Console.WriteLn("@@TESTEVENT_IDS@@ iop_pc=%08x v0=%08x a0=%08x a1=%08x",
                psxRegs.pc, psxRegs.GPR.r[2], psxRegs.GPR.r[4], psxRegs.GPR.r[5]);
            Console.WriteLn("@@TESTEVENT_IDS@@ IOP 0x31100-0x3113f (GPU poll loop):");
            for (int _di = 0; _di < 16; _di++) {
                u32 da = 0x31100u + (u32)_di * 4u;
                Console.WriteLn("  [%05x] %08x", da, iopMemRead32(da));
            }
        }
    }
    // [iter425] PS1DRV 脱出トリガー (継続書き込み版)
    // iter424 判明: vsync=120 1回だけの書き込みでは PS1DRV が 0x205B20 handling後 scratchpad を -1 にresetして再loop
    // 対策: vsync>=120 かつ PC が PS1DRV loop内 (0x200000-0x220000) の間、毎 vsync scratchpad[0x700000a0]=0 を維持
    // PS1DRV コード 0x205B08: BNE v0,a3 → 0x205B20 (真の脱出) if scratchpad[0x700000a0] != -1
    // IOP PS1DRV.IRX が本来 continously セットすべき値を HLE で模擬
    // Removal condition: BIOS browser画面が視覚verifyできた時点で整理
    if (gsvsync_count >= 120
        && cpuRegs.pc >= 0x200000u && cpuRegs.pc < 0x220000u)
    {
        // [iter613] W1 (PS1DRV_BNE_PATCH) deleteテスト: 0x205678 BNE→NOP パッチをdelete (P5 Phase C-6)
        // T0 wait loop が tight loop を起こすか、または自然に脱出するか観測する
        const u32 spad_addr = 0x700000a0u;
        const u32 cur_spad = memRead32(spad_addr);
        // [iter446] spad[0xA0]=-1 の書き戻し停止
        // PS1DRV は 0x2058AC で spad[0xA0]=-1 をconfigし「次GP0コマンド待ち」stateに遷移する
        // iter425 がこれを 0 に戻すと 0x2056D8 (GP0受信パス) に入れない → 書き戻しdisabled化
        // Removal condition: PS1DRV が 0x2056D8 パスで GP0コマンドを正常受信after confirmed
        if (cur_spad == 0xffffffffu) {
            if (gsvsync_count <= 125 || (gsvsync_count % 60) == 0) {
                Console.WriteLn("@@PS1DRV_SPAD_A0_MINUS1@@ vsync=%d pc=%08x spad[a0]=ff (GP0 wait state)",
                    gsvsync_count, cpuRegs.pc);
            }
            // 書き戻しはしない — PS1DRV が自然に -1 → cmd_byte 遷移するまで待つ
        }
        // [iter554] @@INIT_JAL_DUMP@@ 0x3efe0 (display init 2nd JAL, a0=0x4fffc) 内容解読
        // 根拠: iter553 で vsync=1200 でも [0x51060] = null。
        //   初期化 seq の 2nd JAL (0x3efe0, a0=0x4fffc) が [0x51060] を埋める候補。
        //   また 0x3ef20/0x3eaa0/0x3ebc0/0x3ef38 も候補。それぞれ 16 words ダンプ。
        // Removal condition: [0x51060] 埋まりafter confirmed
        {
            static bool s_initjal554_done = false;
            if (!s_initjal554_done && gsvsync_count >= 120) {
                s_initjal554_done = true;
                const u32 jals[] = { 0x3efe0u, 0x3ef20u, 0x3eaa0u, 0x3ebc0u, 0x3ef38u };
                const char* names[] = { "3efe0(2nd)", "3ef20(4th)", "3eaa0(5th)", "3ebc0(6th)", "3ef38(7th)" };
                for (int _ji = 0; _ji < 5; _ji++) {
                    Console.WriteLn("@@INIT_JAL_DUMP@@ 0x%x (%s, 16 words):", jals[_ji], names[_ji]);
                    for (int _di = 0; _di < 16; _di++) {
                        u32 da = jals[_ji] + (u32)_di * 4u;
                        u32 insn = iopMemRead32(da);
                        u32 opc = insn >> 26;
                        if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                        else Console.WriteLn("  %08x: %08x", da, insn);
                    }
                }
            }
        }
        // [iter553] @@LATE_FPTR_DUMP@@ 解析済み: vsync=1200 でも [0x51060] null
        // Removal condition: 解析after completed
        {
            static bool s_late553_done = false;
            if (!s_late553_done && gsvsync_count >= 1200) {
                s_late553_done = true;
                Console.WriteLn("@@LATE_FPTR_DUMP@@ vsync=%d iop_pc=%08x", gsvsync_count, psxRegs.pc);
                // VBlank nested fn ptr [0x51060] (8 entries)
                Console.WriteLn("@@LATE_FPTR_DUMP@@ [0x51060] (8 entries, VBlank nested):");
                for (int _di = 0; _di < 8; _di++) {
                    u32 da = 0x51060u + (u32)_di * 4u;
                    u32 fp = iopMemRead32(da);
                    Console.WriteLn("  [%08x] = %08x%s", da, fp, fp ? " <- non-null" : "");
                }
                // [0x80051090] (DMA control struct ptr) and DMA2 CHCR
                u32 dma_ptr = iopMemRead32(0x51090u);
                u32 dma_chcr = iopMemRead32(0x100a8u);  // IOP DMA2 CHCR
                u32 dma_madr = iopMemRead32(0x100a0u);  // IOP DMA2 MADR
                u32 dma_bcr  = iopMemRead32(0x100a4u);  // IOP DMA2 BCR
                Console.WriteLn("@@LATE_FPTR_DUMP@@ [51090]=%08x DMA2_CHCR=%08x MADR=%08x BCR=%08x",
                    dma_ptr, dma_chcr, dma_madr, dma_bcr);
                // fn ptr array [0x4ff8c] current state
                Console.WriteLn("@@LATE_FPTR_DUMP@@ fn ptr array [0x4ff8c] (11 entries):");
                for (int _di = 0; _di < 11; _di++) {
                    u32 da = 0x4ff8cu + (u32)_di * 4u;
                    u32 fp = iopMemRead32(da);
                    Console.WriteLn("  bit%2d [%08x] = %08x%s", _di, da, fp, fp ? " <- non-null" : "");
                }
            }
        }
        // [iter552] @@DMA_HANDLER_DUMP@@ 解析済み: 0x3ec0c は DMA completion handler
        // Removal condition: 解析after completed
        {
            static bool s_dma552_done = false;
            if (!s_dma552_done && gsvsync_count >= 120) {
                s_dma552_done = true;
                // 0x3ec0c (bit3 DMA handler, 24 words)
                Console.WriteLn("@@DMA_HANDLER_DUMP@@ vsync=%d 0x3ec0c-0x3ec6b (24 words, DMA handler):", gsvsync_count);
                for (int _di = 0; _di < 24; _di++) {
                    u32 da = 0x3ec0cu + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                // VBlank nested fn ptr array at [0x80051060] (8 entries, phys 0x51060)
                Console.WriteLn("@@DMA_HANDLER_DUMP@@ VBlank nested fn ptr [0x51060] (8 entries):");
                for (int _di = 0; _di < 8; _di++) {
                    u32 da = 0x51060u + (u32)_di * 4u;
                    u32 fp = iopMemRead32(da);
                    Console.WriteLn("  [%08x] = %08x%s", da, fp, fp ? " <- non-null" : "");
                }
            }
        }
        // [iter551] @@FPTR_ARRAY_DUMP@@ 解析済み: bit0=0x3eaf8, bit3=0x3ec0c, bit2=null
        // Removal condition: 解析after completed
        {
            static bool s_fptr551_done = false;
            if (!s_fptr551_done && gsvsync_count >= 120) {
                s_fptr551_done = true;
                const u32 arr_base = 0x4ff8cu;  // s4 = s1+4 = 0x4ff8c
                Console.WriteLn("@@FPTR_ARRAY_DUMP@@ vsync=%d fn ptr array [0x4ff8c] (11 entries):", gsvsync_count);
                for (int _di = 0; _di < 11; _di++) {
                    u32 da = arr_base + (u32)_di * 4u;
                    u32 fp = iopMemRead32(da);
                    Console.WriteLn("  bit%2d [%08x] = %08x%s", _di, da, fp, fp ? " <- non-null" : "");
                }
                // 0x3eaf8 (bit0/VBlank fn, 20 words)
                Console.WriteLn("@@FPTR_ARRAY_DUMP@@ 0x3eaf8-0x3eb47 (20 words, bit0 VBlank handler):");
                for (int _di = 0; _di < 20; _di++) {
                    u32 da = 0x3eaf8u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
            }
        }
        // [iter550] @@INTC_REGDUMP@@ 解析済み: s1=0x4ff88, [s1+4]=0x3eaf8, s0=8(DMA)
        // Removal condition: 解析after completed
        {
            static int s_intc_reg_n = 0;
            const u32 _iop_phys = psxRegs.pc & 0x1FFFFFFFu;
            if (s_intc_reg_n < 5 && _iop_phys >= 0x3e490u && _iop_phys <= 0x3e4b0u) {
                s_intc_reg_n++;
                u32 s1_val = psxRegs.GPR.r[17];  // $s1
                u32 s2_val = psxRegs.GPR.r[18];  // $s2
                u32 s4_val = psxRegs.GPR.r[20];  // $s4
                Console.WriteLn("@@INTC_REGDUMP@@ #%d iop_pc=%08x s1=%08x s2=%08x s4=%08x v0=%08x",
                    s_intc_reg_n, psxRegs.pc, s1_val, s2_val, s4_val, psxRegs.GPR.r[2]);
                // s1+4 = function pointer array start (s2 = s4 = old_s1+4 after 0x3e4a8)
                u32 s1_phys = s1_val & 0x1FFFFFFFu;
                if (s1_phys >= 0x1000u && s1_phys < 0x200000u) {
                    Console.WriteLn("@@INTC_REGDUMP@@   [s1]=%08x [s1+4]=%08x [s1+8]=%08x [s1+0x30]=%04x",
                        iopMemRead32(s1_phys), iopMemRead32(s1_phys+4u), iopMemRead32(s1_phys+8u),
                        iopMemRead16(s1_phys+0x30u));
                }
            }
        }
        // [iter549] @@FPTR_DISPATCH_DUMP@@ 解析済み
        // 根拠: s0+0x38=0x4fffc は全 null → 仮定ミス。s2=s1+4 (動的)。
        // Removal condition: 解析after completed
        {
            static bool s_fptr549_done = false;
            if (!s_fptr549_done && gsvsync_count >= 1200) {
                s_fptr549_done = true;
                Console.WriteLn("@@FPTR_DISPATCH_DUMP@@ vsync=%d iop_pc=%08x", gsvsync_count, psxRegs.pc);
                // functionポインタ配列: s2 = MEM[MEM[0x80051010]+offset]? 先に [0x80051010] 付近をverify
                // s0 = 0x8004ffc4 (0x3e3b0 で LUI s0,0x8005 + ADDIU -60 = 0x8004ffc4)
                // s2 = s0+0x38 (ADDIU s2,s0,0x38 = 0x8004fffc) → phys 0x4fffc
                u32 s0_base = 0x4ffc4u;  // phys of s0 = 0x8004ffc4
                Console.WriteLn("@@FPTR_DISPATCH_DUMP@@ func ptr array at s2=0x%08x (s0+0x38):", s0_base + 0x38u);
                for (int _di = 0; _di < 16; _di++) {
                    u32 da = s0_base + 0x38u + (u32)_di * 4u;
                    u32 fp = iopMemRead32(da);
                    Console.WriteLn("  [s2+%2d*4 @ %08x] = %08x%s", _di, da, fp, fp ? " <- non-null" : "");
                }
                // 0x3e83c (JAL at 0x3e38c: display init candidate) 16 words
                Console.WriteLn("@@FPTR_DISPATCH_DUMP@@ 0x3e83c-0x3e87b (16 words, display init):");
                for (int _di = 0; _di < 16; _di++) {
                    u32 da = 0x3e83cu + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                // [0x80051014]/[0x80051018] の値 (mask ポインタ)
                u32 ptr14 = iopMemRead32(0x51014u);
                u32 ptr18 = iopMemRead32(0x51018u);
                u32 ptr1c = iopMemRead32(0x5101cu);
                u32 ptr20 = iopMemRead32(0x51020u);
                Console.WriteLn("@@FPTR_DISPATCH_DUMP@@ [51014]=%08x [51018]=%08x [5101c]=%08x [51020]=%08x",
                    ptr14, ptr18, ptr1c, ptr20);
            }
        }
        // [iter548] @@IOP_3EXXX_DUMP@@ 解析完了
        // Removal condition: 解析after completed
        {
            static bool s_3exxx_done = false;
            if (!s_3exxx_done && gsvsync_count >= 1200) {
                s_3exxx_done = true;
                Console.WriteLn("@@IOP_3EXXX_DUMP@@ vsync=%d iop_pc=%08x", gsvsync_count, psxRegs.pc);
                // 0x3e360-0x3e45f (40 words)
                Console.WriteLn("@@IOP_3EXXX_DUMP@@ 0x3e360-0x3e45f (40 words):");
                for (int _di = 0; _di < 40; _di++) {
                    u32 da = 0x3e360u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                // 0x3e460-0x3e55f (40 words)
                Console.WriteLn("@@IOP_3EXXX_DUMP@@ 0x3e460-0x3e55f (40 words):");
                for (int _di = 0; _di < 40; _di++) {
                    u32 da = 0x3e460u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                // kernel 0x1b80-0x1bbf (16 words)
                Console.WriteLn("@@IOP_3EXXX_DUMP@@ kernel 0x1b80-0x1bbf (16 words):");
                for (int _di = 0; _di < 16; _di++) {
                    u32 da = 0x1b80u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                // kernel 0x0ce8-0x0d27 (16 words)
                Console.WriteLn("@@IOP_3EXXX_DUMP@@ kernel 0x0ce8-0x0d27 (16 words):");
                for (int _di = 0; _di < 16; _di++) {
                    u32 da = 0x0ce8u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
            }
        }
        // [iter547] @@IOP_3E554_DUMP@@ vsync=600 でIOP PC が 0x8003e554 に移動 → 周辺 24 words ダンプ
        // 根拠: iter547 verify済み: NOP@0x30090 enabled, IOP が 0x3e554 に移動。
        // Removal condition: 解析after completed
        {
            static bool s_3e554_done = false;
            if (!s_3e554_done && gsvsync_count >= 600) {
                s_3e554_done = true;
                u32 cur_iop_pc = psxRegs.pc & 0x1FFFFFFFu;
                Console.WriteLn("@@IOP_3E554_DUMP@@ vsync=%d iop_pc=%08x phys=%08x", gsvsync_count, psxRegs.pc, cur_iop_pc);
                // ±16 words around current PC
                u32 base = (cur_iop_pc >= 0x40u) ? cur_iop_pc - 0x40u : 0u;
                for (int _di = 0; _di < 24; _di++) {
                    u32 da = base + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x%s", da, insn, (insn&0x03FFFFFFu)<<2u, da==cur_iop_pc?" <-- PC":"");
                    else Console.WriteLn("  %08x: %08x%s", da, insn, da==cur_iop_pc?" <-- PC":"");
                }
                // Also dump IOP regs: v0, a0, a1, sp, ra
                Console.WriteLn("@@IOP_3E554_DUMP@@ v0=%08x a0=%08x a1=%08x sp=%08x ra=%08x",
                    psxRegs.GPR.r[2], psxRegs.GPR.r[4], psxRegs.GPR.r[5], psxRegs.GPR.r[29], psxRegs.GPR.r[31]);
            }
        }
        // [iter612] W2 (BNE_30090_NOP) deleteテスト: IOP 0x30090 retry loop NOP パッチをdelete (P5 Phase C-5)
        // IOP が 0x313b4 に自然到達するか、またはloopが復活するか観測する
        // [iter544] @@DISPLAY_DUMP@@ 0x313b4 (BIOS browser display candidate) + 0x300d8 (success path)
        // 根拠: 解析完了済み。0x313b4 は 0x3eeb0 を2回呼ぶラッパー。
        // Removal condition: 解析after completed
        {
            static bool s_display544_done = false;
            if (!s_display544_done && gsvsync_count >= 120) {
                s_display544_done = true;
                Console.WriteLn("@@DISPLAY_DUMP@@ vsync=%d 0x313b4-0x31433 (32 words, BIOS browser candidate):", gsvsync_count);
                for (int _di = 0; _di < 32; _di++) {
                    u32 da = 0x313b4u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                Console.WriteLn("@@DISPLAY_DUMP@@ 0x300d8-0x30117 (16 words, 0x300b0 success path):");
                for (int _di = 0; _di < 16; _di++) {
                    u32 da = 0x300d8u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
            }
        }
        // [iter543] @@MAINFUNC_DUMP@@ 0x30080 (JAL 0x300b0 返却後) + 0x3e860 (GPU setup)
        // 根拠: 0x30000 function: init→ROM string→GPU setup(0x3e860)→0x31984 etc.→JAL 0x300b0
        //   0x300b0 が -1 を返した後の 0x30080+ が BIOS browser display の可能性。
        //   0x3e860 (GPU setup, a0=1) の内容も未解読。
        // Removal condition: 解析after completed
        {
            static bool s_mainfunc543_done = false;
            if (!s_mainfunc543_done && gsvsync_count >= 120) {
                s_mainfunc543_done = true;
                Console.WriteLn("@@MAINFUNC_DUMP@@ vsync=%d 0x30080-0x300af (12 words, after JAL 0x300b0):", gsvsync_count);
                for (int _di = 0; _di < 12; _di++) {
                    u32 da = 0x30080u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                Console.WriteLn("@@MAINFUNC_DUMP@@ 0x3e860-0x3e8bf (24 words, GPU setup fn):");
                for (int _di = 0; _di < 24; _di++) {
                    u32 da = 0x3e860u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                // IOP DMA2 CHCR (GPU DMA status): phys 0x1F8010A8 & 0x1FFFFFFF = 0x100A8
                u32 dma2_chcr = iopMemRead32(0x100a8u);
                Console.WriteLn("@@MAINFUNC_DUMP@@ DMA2_CHCR=0x%08x (bit31=running)", dma2_chcr);
            }
        }
        // [iter542] @@ERRPATH_DUMP@@ error path (0x301ac) + outer caller 前後 (0x30090-0x300d7)
        // 根拠: JAL 0x30c40 は 0x300c0。v0=-1 error → BGEZ not taken → BEQ 0x301ac。
        //   0x301ac が BIOS browserdisplayトリガーか否かをdetermineする。
        // Removal condition: 解析after completed
        {
            static bool s_errpath542_done = false;
            if (!s_errpath542_done && gsvsync_count >= 120) {
                s_errpath542_done = true;
                Console.WriteLn("@@ERRPATH_DUMP@@ vsync=%d 0x300a0-0x300d7 (14 words, caller ctx):", gsvsync_count);
                for (int _di = 0; _di < 14; _di++) {
                    u32 da = 0x300a0u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                Console.WriteLn("@@ERRPATH_DUMP@@ 0x301a0-0x3021f (32 words, error path 0x301ac):");
                for (int _di = 0; _di < 32; _di++) {
                    u32 da = 0x301a0u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                // Also read the saved RA of the function containing 0x300c0 (outer caller)
                // The function at ~0x30000 has its own frame; read stack at 0x1ffd18+0x68+0x14
                u32 outer3_ra_addr = 0x1ffd80u + 0x14u;
                Console.WriteLn("@@ERRPATH_DUMP@@ stack[0x1ffd94]=0x%08x (outer3 RA guess)", iopMemRead32(outer3_ra_addr));
                // Dump 0x30000-0x3007f (32 words): function containing JAL 0x30c40
                Console.WriteLn("@@ERRPATH_DUMP@@ 0x30000-0x3007f (32 words, fn top):");
                for (int _di = 0; _di < 32; _di++) {
                    u32 da = 0x30000u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
            }
        }
        // [iter541] @@CALLCHAIN_DUMP@@ 0x30c40 caller RA + 0x33ec0 function解読
        // 根拠: 0x30e40 が即 return と判明 → 0x30c40 が -1 を caller に返すのみ。
        //   caller が何をするか不明。IOP sp から RA を読んで特定する。
        //   0x30e50 functionの JAL 0x33ec0 も未解読 → GPU write 経路の可能性。
        //   outer_sp = 0x801ffd18 (sp+0x68 from iter535), saved RA at outer_sp+0x14 = 0x801ffd2c
        // Removal condition: 解析after completed
        {
            static bool s_callchain_done = false;
            if (!s_callchain_done && gsvsync_count >= 120) {
                s_callchain_done = true;
                // outer frame (caller of 0x30c40): sp = 0x801ffd18, saved RA at sp+0x14 = 0x1ffd2c
                u32 caller_ra = iopMemRead32(0x1ffd2cu);
                Console.WriteLn("@@CALLCHAIN_DUMP@@ vsync=%d caller_of_0x30c40_RA=0x%08x", gsvsync_count, caller_ra);
                // also dump outer-outer frame RA (one more level up)
                // 0x30c40 outer_sp = 0x1ffd18, its prologue saves RA at sp+0x14. Who set that sp?
                // Try reading further up: assume outer_sp+0x68 = 0x1ffd80 for next frame
                u32 outer2_ra = iopMemRead32(0x1ffd80u + 0x14u);
                Console.WriteLn("@@CALLCHAIN_DUMP@@ outer2_RA_guess @ 0x1ffd94=0x%08x", outer2_ra);
                // Dump 0x33ec0-0x33f3f (16 words): called by fn at 0x30e50
                Console.WriteLn("@@CALLCHAIN_DUMP@@ 0x33ec0-0x33f3f (16 words):");
                for (int _di = 0; _di < 16; _di++) {
                    u32 da = 0x33ec0u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                // Dump caller RA context (±16 words around caller_ra)
                if (caller_ra > 0x10000u && caller_ra < 0x80000u) {
                    Console.WriteLn("@@CALLCHAIN_DUMP@@ around caller_RA (0x%08x ±16):", caller_ra);
                    for (int _di = -4; _di < 12; _di++) {
                        u32 da = caller_ra + (u32)((s32)_di * 4);
                        u32 insn = iopMemRead32(da);
                        u32 opc = insn >> 26;
                        if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x%s", da, insn, (insn&0x03FFFFFFu)<<2u, _di==0?" <-- RA":"");
                        else Console.WriteLn("  %08x: %08x%s", da, insn, _di==0?" <-- RA":"");
                    }
                }
            }
        }
        // [iter540] @@ERROR_PATH_DUMP@@ error path (0x30e00-0x30e7f) + function先頭 (0x30ecc-0x30f4b)
        // 根拠: F1000005 delivery で PHASE1 が -1 を返すはずだが PGIF_GP0_WRITE は n=1 のまま。
        //   caller (0x30c64) error path → 0x30e40 → 何が起きるか未解読。
        //   また 0x30ecc functionの先頭部 (PHASE1 前の setup コード) も未解読。
        // Removal condition: 解析after completed
        {
            static bool s_errpath_done = false;
            if (!s_errpath_done && gsvsync_count >= 120) {
                s_errpath_done = true;
                Console.WriteLn("@@ERROR_PATH_DUMP@@ vsync=%d 0x30e00-0x30e7f (32 words):", gsvsync_count);
                for (int _di = 0; _di < 32; _di++) {
                    u32 da = 0x30e00u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                Console.WriteLn("@@ERROR_PATH_DUMP@@ 0x30ecc-0x30f4b (32 words, fn entry):");
                for (int _di = 0; _di < 32; _di++) {
                    u32 da = 0x30eccu + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn&0x03FFFFFFu)<<2u);
                    else Console.WriteLn("  %08x: %08x", da, insn);
                }
                Console.WriteLn("@@ERROR_PATH_DUMP@@ iop_pc=%08x sp=%08x ra=%08x v0=%08x s5=%08x",
                    psxRegs.pc, psxRegs.GPR.r[29], psxRegs.GPR.r[31], psxRegs.GPR.r[2], psxRegs.GPR.r[21]);
            }
        }
        // [iter539] @@F1000005_DELIVER@@ → [iter558] delete
        // 理由: F1000005 注入は誤仮説に基づく。TestEvent(F1000005)=1 → EPILOG v0=-1 → return -1
        //   となり GPU write path に到達しない。毎 vsync 注入が GPU write を永久ブロック。
        // [iter556] @@30C64_DUMP@@ 0x30c64 caller + "no disc" path 0x30e40 のコードダンプ
        // 根拠: 0x311fc epilog で v0=-1 → caller(0x30c64) BGTZ v0 不成立 → 0x30e40 ("no disc browser")
        //   この path が GPU write を行うはず。0x30c64-0x30e70 (72 words) をダンプしてverify。
        // Removal condition: GPU write 箇所 or "no disc" path after confirmed
        {
            static bool s_30c64_dump_done = false;
            if (!s_30c64_dump_done && gsvsync_count >= 120) {
                s_30c64_dump_done = true;
                Console.WriteLn("@@30C64_DUMP@@ vsync=%d 0x30c64-0x30e73 (72 words):", gsvsync_count);
                for (int _di = 0; _di < 72; _di++) {
                    u32 da = 0x30c64u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                // 0x30e40 surrounding context (24 words)
                Console.WriteLn("@@30C64_DUMP@@ 0x30e40 context:");
                for (int _di = 0; _di < 24; _di++) {
                    u32 da = 0x30e40u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                // 0x30eb8 (TestEvent(F1000002) success path) + 0x33ec0 (called from 0x30e64)
                Console.WriteLn("@@30C64_DUMP@@ 0x30eb8 (F1000002 success path, 40 words):");
                for (int _di = 0; _di < 40; _di++) {
                    u32 da = 0x30eb8u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                Console.WriteLn("@@30C64_DUMP@@ 0x33ec0 (JAL target from 0x30e64, 24 words):");
                for (int _di = 0; _di < 24; _di++) {
                    u32 da = 0x33ec0u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                // [iter559] @@GPU_EVT_IDS@@ GPU dispatch loop event IDs at 0x56ee4/ef0/eec + IOP code at 0x3e498
                Console.WriteLn("@@GPU_EVT_IDS@@ event IDs at 0x56ee4=%08x 0x56ef0=%08x 0x56eec=%08x",
                    iopMemRead32(0x56ee4u), iopMemRead32(0x56ef0u), iopMemRead32(0x56eecu));
                Console.WriteLn("@@GPU_EVT_IDS@@ GPU dispatch caller 0x3118c-0x311fc (28 words):");
                for (int _di = 0; _di < 28; _di++) {
                    u32 da = 0x3118cu + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                Console.WriteLn("@@GPU_EVT_IDS@@ IOP code at 0x3e498 (24 words):");
                for (int _di = 0; _di < 24; _di++) {
                    u32 da = 0x3e498u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
            }
        }
        // [iter561] @@GPU_PROLOG_DUMP@@ GPU dispatch function prolog 0x310b0-0x3118b (55 words)
        // 根拠: GPU dispatch loop (0x3118c) の s0/s1/s2/s5 初期値が不明。
        //   s5>0 かつ s3>0 でないと GPU write path に届かない。
        //   prolog (0x310b0-0x3118b, 55 words) でこれらregisterの由来をverify。
        // Removal condition: s0/s1/s2/s5 初期値の由来after identified
        {
            static bool s_gpu_prolog_done = false;
            if (!s_gpu_prolog_done && gsvsync_count >= 120) {
                s_gpu_prolog_done = true;
                Console.WriteLn("@@GPU_PROLOG_DUMP@@ vsync=%d 0x310b0-0x3118b (55 words):", gsvsync_count);
                for (int _di = 0; _di < 55; _di++) {
                    u32 da = 0x310b0u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u)
                        Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn & 0x03FFFFFFu) << 2u);
                    else
                        Console.WriteLn("  %08x: %08x", da, insn);
                }
                // caller check: what calls 0x310b0? scan 0x30c50-0x30cb0 for JAL 0x310b0
                Console.WriteLn("@@GPU_PROLOG_DUMP@@ caller area 0x30c50-0x30cbf (28 words):");
                for (int _di = 0; _di < 28; _di++) {
                    u32 da = 0x30c50u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u)
                        Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, (insn & 0x03FFFFFFu) << 2u);
                    else
                        Console.WriteLn("  %08x: %08x", da, insn);
                }
            }
        }
        // [iter610] W4b (IOP RAM[0x0000] bit3=1) deleteテスト: 自然stateを観測 (P5 Phase C-3)
        // [iter611] W3 (32EFC_NOP) deleteテスト: IOP 0x32efc SW→NOP パッチをdelete (P5 Phase C-4)
        // [56f14] が 0xf6 のままで GPU dispatch に影響があるか観測する
        // [iter512] @@CDROM_HW_READ@@ iopMemRead8(0x1F801800) vs psxHu8 を比較 (1回)
        // 目的: 0x32e18 が LBU v0,0(t7) (t7=0x1F801800) で読む値を IOP JIT と同じパスでverify
        //   psxHu8 は静的配列; iopMemRead8 はhandlerー経由 → 両者が一致するかverify
        // Removal condition: 値after confirmed
        {
            static bool s_cdrom_read_done = false;
            if (!s_cdrom_read_done) {
                s_cdrom_read_done = true;
                u8 psxhu = psxHu8(0x1F801800u);
                u8 iopmem = (u8)iopMemRead8(0x1F801800u);
                Console.WriteLn("@@CDROM_HW_READ@@ vsync=%d psxHu8=%02x iopMemRead8=%02x bit3_psxhu=%d bit3_iop=%d",
                    gsvsync_count, (u32)psxhu, (u32)iopmem, (psxhu>>3)&1, (iopmem>>3)&1);
            }
        }
        // [iter610] W4 (CDROM_PRMEMPT_FIX psxHu8[0x1F801800] bit3=1) deleteテスト (P5 Phase C-3)
        // [iter609] W5 (IOP_IEC_FORCE) deleteテスト: IOP が自力で IEc=1 を維持するかverify (P5 Phase C-2)
        // delete内容: CP0.r[12]|=1u + iopTestIntc() (毎vsync IEc=1 force)
        {
            // [iter576] [56f14]=0xFFFF force書き込みdelete
            // 根拠: iter575 で判明: 0x33238 実行後 0x33330 が [56f14]=0xFE を書き込む (SetMode in-progress)
            //   IEC_FORCE の 0xFFFF force書き込みが毎 vsync 0x33238 を再bootさせ 78回 SetMode → CDROM error state
            //   正しい流れ: 0x33238 一度だけ実行 → CDROM SetMode → [56f14]=0xFE → CDROM ISR が error 返答
            //     → IOP が no-disc detect → BIOS display (direct GPU write or different DMA path)
            // 撤去: PGIF_GP0_WRITE n>1 after confirmed
            // throttled log of key pointers (corrected addresses)
            if (gsvsync_count <= 125 || (gsvsync_count % 120) == 0) {
                Console.WriteLn("@@IOP_IEC_FORCE@@ vsync=%d cp0sr=%08x [56f14]=%08x [4f5c8]=%08x [4f5cc]=%08x [4f5d0]=%08x [4f5d4]=%08x",
                    gsvsync_count, psxRegs.CP0.r[12],
                    iopMemRead32(0x56f14u), iopMemRead32(0x4f5c8u),
                    iopMemRead32(0x4f5ccu), iopMemRead32(0x4f5d0u),
                    iopMemRead32(0x4f5d4u));
            }
            // [R63] Post-restart IEC force moved to outer scope (not gated by EE PC range)
        }
        // [iter514] @@GPU_DISPATCH_DUMP@@ 0x31190 (GPU dispatch loop) + 0x33238 (GPU write func)
        // 目的: 0x33238 が [0x56f14]=0xFFFF で通っても PGIF_GP0_WRITE n>1 にならないcause
        //   GPU dispatch loop と 0x33238 の本体コードでaddのconditionを発見する
        // Removal condition: issue箇所after identified
        {
            static bool s_gpu_dispatch_dump_done = false;
            if (!s_gpu_dispatch_dump_done) {
                s_gpu_dispatch_dump_done = true;
                Console.WriteLn("@@GPU_DISPATCH_DUMP@@ vsync=%d: 0x31190:", gsvsync_count);
                for (int _di = 0; _di < 28; _di++) {
                    u32 da = 0x31190u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                Console.WriteLn("@@GPU_DISPATCH_DUMP@@ 0x33238:");
                Console.WriteLn("@@GPU_DISPATCH_DUMP@@ [10000]=%08x [10004]=%08x [56f14]=%08x [56f34]=%08x",
                    iopMemRead32(0x10000u), iopMemRead32(0x10004u),
                    iopMemRead32(0x56f14u), iopMemRead32(0x56f34u));
                for (int _di = 0; _di < 56; _di++) {
                    u32 da = 0x33238u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
            }
        }
        // [iter508] @@32E18_DUMP@@ 0x32e18 コード延長ダンプ (32 words) + 0x56f14/1ffd10 コンテキスト
        // iter507: 0x32e54 で v1=0xE6, a0=[0x56f14] まで判明。0x32e58+ が比較/return 箇所
        // 目的: v0=1 になるconditionを特定する
        // Removal condition: 0x32e18 の return-1 conditionが判明し次第delete
        {
            static bool s_32e18_dump_done = false;
            if (!s_32e18_dump_done) {
                s_32e18_dump_done = true;
                Console.WriteLn("@@32E18_DUMP@@ vsync=%d [56f14]=%08x [56f1c]=%08x:", gsvsync_count,
                    iopMemRead32(0x56f14u), iopMemRead32(0x56f1cu));
                for (int _di = 0; _di < 80; _di++) {
                    u32 da = 0x32e18u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
            }
        }
        // [iter525] @@31108_DUMP@@ 0x31104 loop exit後のコード + IOP DMA2 register
        // 根拠: [56f14]=0xFFFF forceにより 0x32e18 が 1 を返すはず → 0x31108以降のコードを解読
        // 目的: GPU write 命令または DMA setup を特定 → PGIF_GP0_WRITE n>1 のconditionをverify
        // Removal condition: 0x31108-0x31190 コード解読after completed
        {
            static bool s_31108_dump_done = false;
            if (!s_31108_dump_done) {
                s_31108_dump_done = true;
                Console.WriteLn("@@31108_DUMP@@ vsync=%d 0x31100-0x31190 (gap code):", gsvsync_count);
                for (int _di = 0; _di < 36; _di++) {
                    u32 da = 0x31100u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                // IOP DMA ch2 (GPU): MADR=0x1f8010a0, BCR=0x1f8010a4, CHCR=0x1f8010a8
                Console.WriteLn("@@31108_DUMP@@ IOP DMA2 regs:");
                Console.WriteLn("  MADR=%08x BCR=%08x CHCR=%08x",
                    psxHu32(0x10a0u), psxHu32(0x10a4u), psxHu32(0x10a8u));
                // IOP DMA channel 6 (OTC) and DMA DPCR
                Console.WriteLn("  DPCR=%08x DICR=%08x", psxHu32(0x10f0u), psxHu32(0x10f4u));
            }
        }
        // [iter516] @@IOP_0F30_DUMP@@ IOP RAM 0x0f00-0x1100 + 0x1ec0-0x1f00 ダンプ
        // 根拠: vsync=2400+ で iop_pc=0x0f30 に固定 → IOP kernel idle loopの疑い
        // 目的: 0x0f30 の命令列と 0x1ee8 (vsync=1200 時の PC) 周辺をverifyし停止理由を特定
        // Removal condition: 0x0f30 停止causeafter identified
        {
            static bool s_0f30_dump_done = false;
            if (!s_0f30_dump_done && psxRegs.pc >= 0x0f00u && psxRegs.pc < 0x1100u) {
                s_0f30_dump_done = true;
                Console.WriteLn("@@IOP_0F30_DUMP@@ vsync=%d iop_pc=%08x cp0stat=%08x 0x0f00:",
                    gsvsync_count, psxRegs.pc, psxRegs.CP0.r[12]);
                for (int _di = 0; _di < 32; _di++) {
                    u32 da = 0x0f00u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                Console.WriteLn("@@IOP_0F30_DUMP@@ 0x1ec0:");
                for (int _di = 0; _di < 16; _di++) {
                    u32 da = 0x1ec0u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
            }
        }
        // [iter517] @@3586C_DUMP@@ 0x3586c (CDROM setup function) 64 words ダンプ
        // 根拠: 0x33238 の 0x332f8 から JAL 0x3586c をcall → PS1DRV thread がブロック
        //   同時刻に "CD err" (Ps1CD.cpp:582) occur → CDROM Read 待ちがcause
        // 目的: 0x3586c 内の JAL (WaitSema/WaitEventFlag) と引数 (sema/event ID) を特定
        // Removal condition: blocking syscall + ID after identified
        // [iter526] gate fix: IOP PC が 0x0f30 に固定されなくなったため unconditional にchange
        {
            static bool s_3586c_dump_done = false;
            if (!s_3586c_dump_done && gsvsync_count >= 120) {
                s_3586c_dump_done = true;
                Console.WriteLn("@@3586C_DUMP@@ vsync=%d [56f14]=%08x [56f34]=%08x 0x3586c:",
                    gsvsync_count, iopMemRead32(0x56f14u), iopMemRead32(0x56f34u));
                for (int _di = 0; _di < 64; _di++) {
                    u32 da = 0x3586cu + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                // 0x33238+0xc0 以降 (0x332f8 の JAL 後) もダンプ
                Console.WriteLn("@@3586C_DUMP@@ 0x332f0 (call site + after):");
                for (int _di = 0; _di < 16; _di++) {
                    u32 da = 0x332f0u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
            }
        }
        // [iter527] @@33318_DUMP@@ 0x33238 functionの続き (0x33318-0x334f0) + event descriptor 実値
        // 根拠: 0x33238 の 0x33314 以降が未verify。CDROM コマンド後の GPU write / WaitSema 箇所不明
        //   あわせて TestEvent 引数元 [0x56ee4]/[0x56ef0] 実値と s5 推定値を取得
        // Removal condition: GPU write コードまたは WaitSema + ID after determined
        {
            static bool s_33318_dump_done = false;
            if (!s_33318_dump_done && gsvsync_count >= 120) {
                s_33318_dump_done = true;
                Console.WriteLn("@@33318_DUMP@@ vsync=%d [56ee4]=%08x [56ef0]=%08x [56eec]=%08x [56f1c]=%08x [56f48]=%08x",
                    gsvsync_count,
                    iopMemRead32(0x56ee4u), iopMemRead32(0x56ef0u),
                    iopMemRead32(0x56eecu), iopMemRead32(0x56f1cu), iopMemRead32(0x56f48u));
                Console.WriteLn("@@33318_DUMP@@ 0x33318-0x334f0 (0x33238 continuation, 60 words):");
                for (int _di = 0; _di < 60; _di++) {
                    u32 da = 0x33318u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
            }
        }
        // [P12] @@A0_TABLE_DUMP@@ A-table[0xa0-0xa5] function pointers + caller context
        // 根拠: IOP が bfc58ac0 で A0[0xa1] をポーリング中。A0[0xa1] の正体を特定する。
        // PS1 A-table base: IOP MEM[0x200] (通常), B-table: MEM[0x874]
        // Removal condition: A0[0xa1] の正体after identified
        {
            static bool s_a0tbl_done = false;
            if (!s_a0tbl_done && gsvsync_count >= 120) {
                s_a0tbl_done = true;
                // A-table at 0x200: each entry = 4-byte function pointer
                Console.WriteLn("@@A0_TABLE_DUMP@@ vsync=%d A-table[0x9E-0xA5] at MEM[0x200+N*4]:", gsvsync_count);
                for (u32 idx = 0x9E; idx <= 0xA5; idx++) {
                    u32 addr = 0x200u + idx * 4u;
                    u32 fn = iopMemRead32(addr);
                    Console.WriteLn("  A0[%02x] @ %08x = %08x", idx, addr, fn);
                }
                // Also dump A0[0xa1] function body (first 16 insns)
                u32 fn_a1 = iopMemRead32(0x200u + 0xa1u * 4u);
                if (fn_a1 > 0x100u && fn_a1 < 0x200000u) {
                    Console.WriteLn("@@A0_0xA1_BODY@@ fn=%08x (first 16 insns):", fn_a1);
                    for (int i = 0; i < 16; i++) {
                        u32 da = fn_a1 + (u32)(i * 4);
                        Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                    }
                }
                // bfc58ac0 caller context: what's the loop structure?
                Console.WriteLn("@@BFC58AC0_CONTEXT@@ [bfc58ab0-bfc58af0]:");
                for (u32 a = 0xBFC58AB0u & 0x1FFFFFFFu; a <= (0xBFC58AF0u & 0x1FFFFFFFu); a += 4u) {
                    Console.WriteLn("  %08x: %08x", a | 0xBFC00000u, iopMemRead32(a));
                }
                // IOP register state
                Console.WriteLn("@@IOP_REGS_A0A1@@ v0=%08x v1=%08x a0=%08x a1=%08x t0=%08x t1=%08x t2=%08x sp=%08x ra=%08x",
                    psxRegs.GPR.n.v0, psxRegs.GPR.n.v1, psxRegs.GPR.n.a0, psxRegs.GPR.n.a1,
                    psxRegs.GPR.n.t0, psxRegs.GPR.n.t1, psxRegs.GPR.n.t2,
                    psxRegs.GPR.n.sp, psxRegs.GPR.n.ra);
            }
        }
        // [iter529] @@3EE70_DUMP@@ TestEvent impl解読 + ECB テーブル実値
        // 根拠: TestEvent(0x3ee70) は PS1DRV カスタムimpl (BIOS syscall でない)。
        //   0xF1000002/5/4 = event ID を受け取り、ECB.status == 0x4000 なら 1 を返すはず。
        //   ECB base = [0x0100] = 0xa000e004。TestEvent コードを解読して
        //   DeliverEvent 相当の直接パッチ対象 (ECB + offset) を特定する。
        // Removal condition: ECB status 直接書き換えによる TestEvent(F1000005)==1 after confirmed
        {
            static bool s_3ee70_dump_done = false;
            if (!s_3ee70_dump_done && gsvsync_count >= 120) {
                s_3ee70_dump_done = true;
                Console.WriteLn("@@3EE70_DUMP@@ vsync=%d TestEvent(0x3ee70) [20 words]:", gsvsync_count);
                for (int _di = 0; _di < 20; _di++) {
                    u32 da = 0x3ee70u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                // ECB table at [0x0100]->[phys 0xe004], 12 entries × 4 words each = 48 words
                u32 ecb_base = iopMemRead32(0x100u) & 0x1FFFFFFFu;
                Console.WriteLn("@@3EE70_DUMP@@ ECB base=[%08x] phys=%08x [48 words]:",
                    iopMemRead32(0x100u), ecb_base);
                for (int _di = 0; _di < 48; _di++) {
                    u32 da = ecb_base + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
            }
        }
        // [iter530] @@B0_TABLE_DUMP@@ B0 dispatchテーブル + ECB データ解読
        // 根拠: TestEvent=B0[0x0B] 確定 (iter529)。B0 テーブル(0x05E0)から TestEvent impladdressを取得し、
        //   ECB 候補領域 (0x6cc8, 0x5ea00) をダンプして 0xF1000005 の ECB status フィールドを特定する。
        // Removal condition: ECB 直接書き換えで TestEvent(F1000005)==1 after confirmed
        {
            static bool s_b0_dump_done = false;
            if (!s_b0_dump_done && gsvsync_count >= 120) {
                s_b0_dump_done = true;
                // B0 function table at 0x05E0 (16 entries = 64 bytes)
                Console.WriteLn("@@B0_TABLE_DUMP@@ vsync=%d B0_table[0x05E0] (16 words):", gsvsync_count);
                for (int _di = 0; _di < 16; _di++) {
                    u32 da = 0x05E0u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                // ECB data candidate at 0x6cc8 (from 0xe00c in ECB list header)
                Console.WriteLn("@@B0_TABLE_DUMP@@ ECB_cand_1[0x6cc8] (32 words):");
                for (int _di = 0; _di < 32; _di++) {
                    u32 da = 0x6cc8u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                // ECB data candidate at 0x5ea00 (from 0xe004=0x8005ea00 in ECB list header)
                Console.WriteLn("@@B0_TABLE_DUMP@@ ECB_cand_2[0x5ea00] (32 words):");
                for (int _di = 0; _di < 32; _di++) {
                    u32 da = 0x5ea00u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
            }
        }
        // [iter531] @@B0_FUNC_TABLE_DUMP@@ B0[0x0B]=TestEvent impladdress + ECB 解読
        // 根拠: B0 テーブル base=0x0874 確定 (iter530)。[0x08A0]=TestEvent fn ptr。
        //   TestEvent implコードを見て、event_id 0xF1000005 → ECB status フィールドの場所を特定し
        //   直接 0x4000 を書いて TestEvent(0xF1000005)==1 をforceする準備をする。
        // Removal condition: ECB status パッチによる TestEvent(F1000005)==1 after confirmed
        {
            static bool s_b0func_dump_done = false;
            if (!s_b0func_dump_done && gsvsync_count >= 120) {
                s_b0func_dump_done = true;
                // B0 function pointer table at 0x0874 (16 entries)
                Console.WriteLn("@@B0_FUNC_TABLE_DUMP@@ vsync=%d B0_fnptrs[0x0874] (16 entries):", gsvsync_count);
                for (int _di = 0; _di < 16; _di++) {
                    u32 ptr_addr = 0x0874u + (u32)_di * 4u;
                    Console.WriteLn("  B0[%02x] @%08x = %08x", _di, ptr_addr, iopMemRead32(ptr_addr));
                }
                // Follow TestEvent (B0[0x0B]) implementation
                u32 testev_fn = iopMemRead32(0x08A0u);  // B0[0x0B]
                u32 deliv_fn  = iopMemRead32(0x08A4u);  // B0[0x0C] = DeliverEvent
                Console.WriteLn("@@B0_FUNC_TABLE_DUMP@@ TestEvent=0x%08x DeliverEvent=0x%08x", testev_fn, deliv_fn);
                Console.WriteLn("@@B0_FUNC_TABLE_DUMP@@ TestEvent impl [20 words]:");
                for (int _di = 0; _di < 20; _di++) {
                    Console.WriteLn("  %08x: %08x", testev_fn + (u32)_di * 4u,
                        iopMemRead32(testev_fn + (u32)_di * 4u));
                }
            }
        }
        // [iter535] @@IOP_CALLER_DUMP@@ 0x310e0+ functionの caller 特定
        // 根拠: PHASE1→2→3 は毎 vsync 完走。v0=1 を返すが PGIF_GP0_WRITE 不変。
        //   caller が GPU dispatch しないcauseをverifyするため call site を特定する。
        // 目的: IOP sp/ra + stack frame 16 words + 0x311fc-0x3127b (epilogue) をダンプ
        // Removal condition: caller address・GPU dispatch 経路after confirmed
        {
            static bool s_caller_done = false;
            if (!s_caller_done && gsvsync_count >= 120) {
                s_caller_done = true;
                u32 iop_sp  = psxRegs.GPR.r[29];
                u32 iop_ra  = psxRegs.GPR.r[31];
                u32 iop_v0  = psxRegs.GPR.r[2];
                u32 iop_s0  = psxRegs.GPR.r[16];
                u32 iop_s3  = psxRegs.GPR.r[19];
                u32 iop_s4  = psxRegs.GPR.r[20];
                u32 iop_s5  = psxRegs.GPR.r[21];
                Console.WriteLn("@@IOP_CALLER_DUMP@@ vsync=%d sp=%08x ra=%08x v0=%08x s0=%08x s3=%08x s4=%08x s5=%08x",
                    gsvsync_count, iop_sp, iop_ra, iop_v0, iop_s0, iop_s3, iop_s4, iop_s5);
                // stack frame: 16 words from sp (find saved ra on stack)
                Console.WriteLn("@@IOP_CALLER_DUMP@@ stack[sp..sp+60]:");
                for (int _di = 0; _di < 16; _di++) {
                    u32 da = iop_sp + (u32)_di * 4u;
                    Console.WriteLn("  sp+%02x [%08x]=%08x", _di*4, da, iopMemRead32(da & 0x1FFFFFFFu));
                }
                // function epilogue region (0x311fc-0x3127b, 32 words)
                Console.WriteLn("@@IOP_CALLER_DUMP@@ 0x311fc-0x3127b (32 words, epilogue+caller):");
                for (int _di = 0; _di < 32; _di++) {
                    u32 da = 0x311fcu + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) {
                        u32 tgt = (insn & 0x03FFFFFFu) << 2u;
                        Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, tgt);
                    } else {
                        Console.WriteLn("  %08x: %08x", da, insn);
                    }
                }
            }
        }
        // [iter534] @@3586C_DUMP@@ 0x3586c-0x3596b (64 words) + 全 JAL target 列挙
        // 根拠: PHASE2 JAL 0x33238 → JAL 0x3586c が WaitSema 系でブロック確定。
        //   0x3586c の命令列と全 JAL target をverifyして sema_id / event_id を特定する。
        // 目的: blocking syscall 実在verify + kernel object ID → 最小注入手段確定
        // Removal condition: sema_id 判明・アンブロック方針after determined
        {
            static bool s_3586c_done = false;
            if (!s_3586c_done && gsvsync_count >= 120) {
                s_3586c_done = true;
                Console.WriteLn("@@3586C_DUMP@@ vsync=%d 0x3586c-0x3596b (64 words):", gsvsync_count);
                for (int _di = 0; _di < 64; _di++) {
                    u32 da = 0x3586cu + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    // JAL: opcode bits31-26 = 0x03
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) {
                        u32 tgt = (insn & 0x03FFFFFFu) << 2u;
                        Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, tgt);
                    } else {
                        Console.WriteLn("  %08x: %08x", da, insn);
                    }
                }
                // C0 table for WaitSema/WaitEventFlag: look for ADDIU t2,r0,0xC0 pattern nearby
                Console.WriteLn("@@3586C_DUMP@@ searching syscall stubs 0x3ee00-0x3ef40 (80 words):");
                for (int _di = 0; _di < 80; _di++) {
                    u32 da = 0x3ee00u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    // ADDIU t2,r0,0xB0 or 0xC0 dispatch stubs
                    if ((insn & 0xFFFF0000u) == 0x240a0000u) {
                        u32 vec = insn & 0xFFFFu;
                        if (vec == 0xB0u || vec == 0xC0u) {
                            u32 fn = iopMemRead32(da + 8u) & 0xFFFFu; // ADDIU t1,r0,fn#
                            Console.WriteLn("  stub@%08x: vec=%02x fn=%02x", da, vec, fn);
                        }
                    }
                }
            }
        }
        // [iter533] @@PHASE2_SETUP_DUMP@@ 0x31100-0x3118b + event_id address実値
        // 根拠: PHASE3 の BLEZ s0 が s0 初期値に依存。s0/s2/a1 の初期化コードと
        //   event_id (0x80056ee4/eec/ef0 の実値) が未verify。
        // 目的: s0 初期値 + event_id → TestEvent が正しい PCB entry を叩いているか確定
        // Removal condition: event_id after confirmed
        {
            static bool s_phase2_setup_done = false;
            if (!s_phase2_setup_done && gsvsync_count >= 120) {
                s_phase2_setup_done = true;
                Console.WriteLn("@@PHASE2_SETUP_DUMP@@ vsync=%d 0x31100-0x3118b (35 words):", gsvsync_count);
                for (int _di = 0; _di < 35; _di++) {
                    u32 da = 0x31100u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                // event_id values stored in PS1DRV data segment
                Console.WriteLn("@@PHASE2_SETUP_DUMP@@ event_id[0x56ee4]=%08x [0x56eec]=%08x [0x56ef0]=%08x",
                    iopMemRead32(0x56ee4u), iopMemRead32(0x56eecu), iopMemRead32(0x56ef0u));
            }
        }
        // [iter532] @@PHASE3_DUMP@@ 0x3118c-0x311fc (PHASE3 GPU write path 解読)
        // 根拠: PHASE3 (0x31190) で TestEvent(F1000002)=1 時の GPU write コードが未解読。
        //   IOP PC が 0x3119c (PHASE3 TestEvent 直後) に観測されるが GP0 write がない。
        //   PHASE3 のコード全体をverifyして GPU write トリガーconditionを特定する。
        // Removal condition: PGIF_GP0_WRITE n>1 after confirmed
        {
            static bool s_phase3_dump_done = false;
            if (!s_phase3_dump_done && gsvsync_count >= 120) {
                s_phase3_dump_done = true;
                Console.WriteLn("@@PHASE3_DUMP@@ vsync=%d 0x3118c-0x311fc (28 words):", gsvsync_count);
                for (int _di = 0; _di < 28; _di++) {
                    u32 da = 0x3118cu + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
                // also dump the pre-function prologue (0x310e0-0x310fc) to find function entry
                Console.WriteLn("@@PHASE3_DUMP@@ 0x310e0-0x310fc (8 words, pre-function):");
                for (int _di = 0; _di < 8; _di++) {
                    u32 da = 0x310e0u + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
            }
        }
        // [iter536] @@PHASE3_CALLER_DUMP@@ 外部caller(0x30c40-0x30cbf) + TestEvent BIOS(0x1eac-0x1f2b)
        // 根拠: PHASE3は F1000002/4/5 のallが0を返す間 0x311f0 BGTZで無限loop。
        //   F1000002を deliver すればsuccessreturn(v0=s3=s5=1)可能。
        //   外部callerがsuccess時にGPU writeを行うかverifyがneeded。
        //   TestEvent BIOSfunction(0x1eac)がどのRAM領域でECBを読むか特定してECBaddressを確定する。
        // Removal condition: PGIF_GP0_WRITE n>1 after confirmed
        {
            static bool s_phase3_caller_done = false;
            if (!s_phase3_caller_done && gsvsync_count >= 120) {
                s_phase3_caller_done = true;
                // Outer caller region: saved RA on stack = 0x30c64 → JAL must be near 0x30c5c
                Console.WriteLn("@@PHASE3_CALLER_DUMP@@ vsync=%d 0x30c40-0x30cbf (32 words):", gsvsync_count);
                for (int _di = 0; _di < 32; _di++) {
                    u32 da = 0x30c40u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) {
                        u32 tgt = (insn & 0x03FFFFFFu) << 2u;
                        Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, tgt);
                    } else {
                        Console.WriteLn("  %08x: %08x", da, insn);
                    }
                }
                // TestEvent BIOS implementation: B0[0x0B] → stub 0x3ee70 → actual fn
                // Check what 0x3ee70 does (stub) to find actual TestEvent impl address
                Console.WriteLn("@@PHASE3_CALLER_DUMP@@ TestEvent stub 0x3ee68-0x3ee7f (6 words):");
                for (int _di = 0; _di < 6; _di++) {
                    u32 da = 0x3ee68u + (u32)_di * 4u;
                    u32 insn = iopMemRead32(da);
                    u32 opc = insn >> 26;
                    if (opc == 0x03u) {
                        u32 tgt = (insn & 0x03FFFFFFu) << 2u;
                        Console.WriteLn("  %08x: %08x  <- JAL 0x%08x", da, insn, tgt);
                    } else {
                        Console.WriteLn("  %08x: %08x", da, insn);
                    }
                }
                // TestEvent BIOS kernel impl at 0x1eac (from R34 analysis)
                Console.WriteLn("@@PHASE3_CALLER_DUMP@@ BIOS TestEvent impl 0x1eac-0x1f2b (32 words):");
                for (int _di = 0; _di < 32; _di++) {
                    u32 da = 0x1eacu + (u32)_di * 4u;
                    Console.WriteLn("  %08x: %08x", da, iopMemRead32(da));
                }
            }
        }
        // [iter565] @@GPU_CMD_BUF_DUMP@@ 0x30cc0-0x30d3f (byte スキャン後コード) + MEM[0x10000..0x1001f]
        // 根拠: 0x30c8c: LBU v0,0(0xa0010000) → byte=0x00 → BEQ taken → jump 0x30cb4
        //   0x30cc0 以降が不明。GPU コマンドbuffer (IOP phys 0x10000) の内容もverify。
        // 目的: byte=0 時のhandlingと GPU buffer実値 → psxGPUw callパス特定
        // Removal condition: PGIF_GP0_WRITE n>1 after confirmed
        // [iter592] GPU_CMD_BUF_DUMP delete: one-shot 解析完了。behaviorへの影響なし。
        // [iter591] IOP kernel idle ゾーン (0x0f00-0x1100) 系 one-shot probe を一括delete:
        // TCB_CONTEXT, EPC_REGION_DUMP, 3EE60_DUMP, A0_DISPATCH_DUMP (iter518-521)
        // 根拠: IOP はこの PC rangeに入らなくなった (PS1DRV 空間 0x3e*** で活発behavior)。WaitSema 仮説は否定済み。
        // [iter523] @@IOP_INTC_FIX@@ INTC_MASK=0 → スピンloopが永久に抜けられないissueをfix
        // 根拠: INTC_WAIT_DUMP で INTC_MASK=[1f801074]=0x0000 → mask1=0x0009 & 0=0 always
        // fix: INTC_MASK bit0(VBlank)+bit3(DMA) をenabled化 + INTC_STAT bit0(VBlank) を毎 vsync 立てる
        //   → スピンloopcondition INTC_STAT & 0x0009 & INTC_MASK = 0x0001 & 0x0009 & 0x0009 = 0x0001 ≠ 0
        // [iter528] CDROM IRQ add: TestEvent(0xF1000002) は CDROM event 待ち →
        //   iopIntcIrq(2) を毎 vsync 注入して PS1 BIOS CDROM ISR をboot → DeliverEvent(F1000002)
        // Removal condition: PGIF_GP0_WRITE n>1 after confirmed
        // [iter609] W5 INTC_MASK/STAT delete: psxHu16(0x1074)|=0x0009 + psxHu16(0x1070)|=0x0001 をdelete
        // IOP が自然に INTC_MASK をconfigするかverify (P5 Phase C-2)
        if (gsvsync_count == 120 || (gsvsync_count % 600) == 0) {
            Console.WriteLn("@@IOP_INTC_FIX@@ vsync=%d INTC_MASK=%04x INTC_STAT=%04x iop_pc=%08x ECB[e064]=%08x",
                gsvsync_count, (u16)psxHu32(0x1074u), (u16)psxHu32(0x1070u), psxRegs.pc,
                iopMemRead32(0xe064u));
        }
        // [iter591] INTC_WAIT_DUMP delete (iter522): 同上、IOP が 0x0f00-0x1100 に入らなくなった。解析完了。
        // [iter490] @@PCB_ENTRY_DUMP@@ IOP PCB テーブルエントリstateダンプ
        // 根拠: PCB base = mem[0x120] = 0xa000e028 (phys 0xe028), エントリ[i].state = phys[0xe028+28*i+4]
        // Removal condition: エントリverify済み (iter490 で全 [0-6]=0x2000 verify済み)
        if ((gsvsync_count % 600) == 0) {
            const u32 pcb_base = 0xe028u;
            Console.WriteLn("@@PCB_ENTRY_DUMP@@ vsync=%d iop_pc=%08x cp0stat=%08x:", gsvsync_count, psxRegs.pc, psxRegs.CP0.r[12]);
            for (int _i = 0; _i < 8; _i++) {
                u32 sa = pcb_base + (u32)_i * 28u + 4u;
                u32 s = iopMemRead32(sa);
                Console.WriteLn("  [%d] state=%08x addr=%08x", _i, s, sa);
            }
        }
        // [iter597] H7 delete: spad[0x193C]=1 VBlank 注入を撤去。EE VBlank handler (0x204FE0) が自然にセットするかverify。
        // 根拠: PS1DRV init で AddIntcHandler VBlank(0x204FE0) + EnableIntc 済み。自然発火を期待。
        // [iter598] H8 delete: spad[0x1849]=1 Timer0 flag injection を撤去。
        // Timer0 handler (0x2052CC, HLE AddIntcHandler インストール済み) が自然にセットするかverify。
        // [iter452] PGIF fastmem path fix: eeHw 直接書き込みで JIT fastmem を正しく誘導
        // 判明: PS1DRV の JIT hot-path は vtlb handler を bypass し eeHw を直接 fastmem 読み
        //   - PGIF_CTRL (s1=0x1000F380): fastmem → eeHw[0xF380] を読む (getUpdPgifCtrlReg 経由の eeHw[0xF370] ではない)
        //   - PGPU_DAT_FIFO (s8=0x1000F3E0): fastmem → eeHw[0xF3E0] を読む (rb_gp0_Get は呼ばれない)
        // fix: 両方の eeHw addressに正しい値を書き込む
        // PGIF_CTRL: eeHw[0xF380] に BUSY bit31=1 → PGIF_combined = 0x20 ≠ 0 → IOPhandlingパスへ進む
        // PGPU_DAT_FIFO: eeHw[0xF3E0] に GP0(0xE1) → Draw Mode コマンドとして decode される
        // Removal condition: PS1DRV が IOPhandlingパス (0x2056D8) を通過し spad[0xA0]=0xE1 遷移after confirmed
        // [iter594] H9 delete: PGIF_CTRL BUSY bit 毎 vsync 注入を撤去。
        // 根拠: H10 delete後も spad[a0]=0xff → PS1DRV は BUSY=1 でも GP0 データを受け取っていない。
        // BUSY bit 注入なしの自律stateで PS1DRV のbehaviorを観測する。
        // [iter593] H10 delete: eeHw[0xF3E0]=0xE1000000 毎 vsync 注入を撤去。
        // 根拠: iter588 分析で GP0_fifo_count を更新しないため PS1DRV がデータをskipと確定。
        // spad[a0]=0xffffffff が継続しており H10 は機能していなかった。
        // [iter590] IOP_EXCVEC_DUMP + IOP_0100_DUMP delete: IOP RAM bootダンプ解析完了。behaviorへの影響なし。
        // [iter599] H12 delete: T0 bypass (spad[0x1930] + eeHw T0 update) を撤去。
        // 根拠: iter451 NOP patch (0x205678 BNE→NOP) が T0 wait loopを既にdisabled化済み。
        // H12 がnot neededであれば EE PC パターンが維持される。regression なら即リバート。
    }

    // [iter589] @@OSDSYS_B30_DUMP@@ delete: 解析完了 (iter586 で 0x1000c0=ExecPS2 stub 確定)。behaviorへの影響なし。

    if ((gsvsync_count % 600) == 0) {
        // [iter177] @@IOP_SR_PROBE@@ iop_sr=COP0.Status iop_imask=IOP INTC mask
        // Removal condition: IOPデッドロックcause（割り込みdisabled or SIF通信不全）after determined
        // [iter179] s0/s3/a0add: putcharloopの$s0(文字列ポインタ)がインクリメントされているかverify
        // [iter181] spadd: $sp(r29)がcorruptされているかverify → $sp=0x70003E90(valid)verify済み
        // [iter182] s4add: $s4=0verify → MOVNパスなし → $s0はADDIU $s0,$sp,31パスのみ
        // [iter183] sp31add: SB $0,31($sp)がnullを書いているかverify
        //   sp31=0x00 → nullあり → JIT BEQバグ確定
        //   sp31!=0x00 → nullなし → JIT SBバグ確定
        // Removal condition: BEQ/SBバグafter determined
        // [iter222] s2(r18)add: 9FC41A28memorydetectloopの外側上限値をverify
        // Removal condition: s2の値が確定しloop遅延causeが判明した後
        // [iter222] s2/v0/v1 + stack dump: 9FC41A28loop上限の由来を特定
        {
            u32 sp_val = cpuRegs.GPR.r[29].UL[0];
            u32 sp28 = 0, sp88 = 0;
            if (sp_val >= 0x70000000u && sp_val < 0x70004000u) {
                sp28 = memRead32(sp_val + 0x28);
                sp88 = memRead32(sp_val + 0x88);
            }
        {
            u32 s7_val = cpuRegs.GPR.r[23].UL[0];
            u32 s7_4124 = 0;
            if (s7_val >= 0x80000000u && s7_val < 0x82000000u)
                s7_4124 = memRead32(s7_val + 0x4124u);
            // [iter407] s1_val (D_ENABLER候補) の実際の値を memRead32 でverify
            u32 s1_val = cpuRegs.GPR.r[17].UL[0];
            u32 s1_deref = (s1_val >= 0x10000000u && s1_val < 0x20000000u) ? memRead32(s1_val) : 0xDEADDEADu;
            // [iter420] Confirm: is virtual 0x10000000 reading hw-reg(T0_COUNT) or EE RAM[0]?
            u32 eeram0 = eeMem ? *(u32*)(eeMem->Main + 0) : 0xDEADu;
            Console.WriteLn("@@EE_PC_SAMPLE@@ vsync=%d pc=%08x iop_pc=%08x s0=%08x s1=%08x s1_deref=%08x s2=%08x s3=%08x s7=%08x s7_4124=%08x v0=%08x v1=%08x sp=%08x sp28=%08x sp88=%08x a0=%08x a1=%08x a2=%08x a3=%08x eeram0=%08x",
                gsvsync_count, cpuRegs.pc, psxRegs.pc,
                cpuRegs.GPR.r[16].UL[0], s1_val, s1_deref, cpuRegs.GPR.r[18].UL[0], cpuRegs.GPR.r[19].UL[0],
                s7_val, s7_4124,
                cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[3].UL[0],
                sp_val, sp28, sp88,
                cpuRegs.GPR.r[4].UL[0], cpuRegs.GPR.r[5].UL[0],
                cpuRegs.GPR.r[6].UL[0], cpuRegs.GPR.r[7].UL[0],
                eeram0);
        }
        }
        // [iter144/145] @@EE_PC_INSN_DUMP@@ – EE RAM命令ダンプ + v0/v1/epc/ra/s1register値
        // [iter149] epc (COP0[14]) と ra (r31) をadd – ERET時の飛び先特定のため
        // [iter406] 12→20 words にextend + s1(GPR17) add – 0x205668 wait loop 後の server チェック分岐先verify
        u32 dump_base = cpuRegs.pc & ~0xFu; // 16バイトアライン
        Console.WriteLn("@@EE_PC_INSN_DUMP@@ base=%08x v0=%08x v1=%08x epc=%08x ra=%08x s1=%08x: "
            "%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | "
            "%08x %08x %08x %08x | %08x %08x %08x %08x",
            dump_base, cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[3].UL[0],
            cpuRegs.CP0.r[14], cpuRegs.GPR.r[31].UL[0], cpuRegs.GPR.r[17].UL[0],
            memRead32(dump_base+0),  memRead32(dump_base+4),
            memRead32(dump_base+8),  memRead32(dump_base+12),
            memRead32(dump_base+16), memRead32(dump_base+20),
            memRead32(dump_base+24), memRead32(dump_base+28),
            memRead32(dump_base+32), memRead32(dump_base+36),
            memRead32(dump_base+40), memRead32(dump_base+44),
            memRead32(dump_base+48), memRead32(dump_base+52),
            memRead32(dump_base+56), memRead32(dump_base+60),
            memRead32(dump_base+64), memRead32(dump_base+68),
            memRead32(dump_base+72), memRead32(dump_base+76));
        // [iter660] @@OSDSYS_218030_LOOP@@ — JIT stuck at 0x218030 rendering loop tracking
        // Removal condition: 0x218030 loopcauseafter identified
        {
            static u32 s_218030_cnt = 0;
            if (s_218030_cnt < 10 && cpuRegs.pc >= 0x00218020u && cpuRegs.pc <= 0x00218040u) {
                s_218030_cnt++;
                u32 sp = cpuRegs.GPR.r[29].UL[0];
                u32 sp18 = (sp < 0x02000000u) ? memRead32(sp + 0x18) : 0xDEADu;
                u32 sp68 = (sp < 0x02000000u) ? memRead32(sp + 0x68) : 0xDEADu;
                u32 sp70 = (sp < 0x02000000u) ? memRead32(sp + 0x70) : 0xDEADu;
                Console.WriteLn("@@OSDSYS_218030_LOOP@@ n=%u vsync=%u t8=%08x s6=%08x a2=%08x fp=%08x t9=%08x "
                    "sp=%08x sp18=%08x sp68=%08x sp70=%08x s2=%08x s3=%08x v0=%08x v1=%08x ra=%08x",
                    s_218030_cnt, gsvsync_count,
                    cpuRegs.GPR.r[24].UL[0], cpuRegs.GPR.r[22].UL[0], cpuRegs.GPR.r[6].UL[0],
                    cpuRegs.GPR.r[30].UL[0], cpuRegs.GPR.r[25].UL[0],
                    sp, sp18, sp68, sp70,
                    cpuRegs.GPR.r[18].UL[0], cpuRegs.GPR.r[19].UL[0],
                    cpuRegs.GPR.r[2].UL[0], cpuRegs.GPR.r[3].UL[0],
                    cpuRegs.GPR.r[31].UL[0]);
                // One-shot: dump code at 0x218200-0x218280 (outer loop exit/branch)
                if (s_218030_cnt == 1) {
                    for (u32 base = 0x217F00; base < 0x218300; base += 0x40) {
                        Console.WriteLn("@@OSDSYS_218030_CODE@@ %08x: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                            base,
                            memRead32(base+0x00), memRead32(base+0x04), memRead32(base+0x08), memRead32(base+0x0C),
                            memRead32(base+0x10), memRead32(base+0x14), memRead32(base+0x18), memRead32(base+0x1C),
                            memRead32(base+0x20), memRead32(base+0x24), memRead32(base+0x28), memRead32(base+0x2C),
                            memRead32(base+0x30), memRead32(base+0x34), memRead32(base+0x38), memRead32(base+0x3C));
                    }
                }
            }
        }
        // [iter664] @@EELOAD_OUTER_LOOP@@ one-shot dump of 0x82630-0x826C0
        // Reveals the outer loop condition at 0x82658 causing infinite polling at 0x82698
        // Removal condition: outer loop conditionafter identified
        {
            static bool s_done = false;
            if (!s_done && cpuRegs.pc >= 0x00082690u && cpuRegs.pc <= 0x000826C0u) {
                s_done = true;
                Console.WriteLn("@@EELOAD_OUTER_LOOP@@ [82630]: "
                    "%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
                    memRead32(0x82630u), memRead32(0x82634u), memRead32(0x82638u), memRead32(0x8263Cu),
                    memRead32(0x82640u), memRead32(0x82644u), memRead32(0x82648u), memRead32(0x8264Cu),
                    memRead32(0x82650u), memRead32(0x82654u), memRead32(0x82658u), memRead32(0x8265Cu),
                    memRead32(0x82660u), memRead32(0x82664u), memRead32(0x82668u), memRead32(0x8266Cu));
                Console.WriteLn("@@EELOAD_OUTER_LOOP@@ [82670]: "
                    "%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
                    memRead32(0x82670u), memRead32(0x82674u), memRead32(0x82678u), memRead32(0x8267Cu),
                    memRead32(0x82680u), memRead32(0x82684u), memRead32(0x82688u), memRead32(0x8268Cu),
                    memRead32(0x82690u), memRead32(0x82694u), memRead32(0x82698u), memRead32(0x8269Cu),
                    memRead32(0x826A0u), memRead32(0x826A4u), memRead32(0x826A8u), memRead32(0x826ACu));
                Console.WriteLn("@@EELOAD_OUTER_LOOP@@ [826B0]: "
                    "%08x %08x %08x %08x | %08x %08x %08x %08x",
                    memRead32(0x826B0u), memRead32(0x826B4u), memRead32(0x826B8u), memRead32(0x826BCu),
                    memRead32(0x826C0u), memRead32(0x826C4u), memRead32(0x826C8u), memRead32(0x826CCu));
                // Also dump key data values and v1 full 64-bit
                Console.WriteLn("@@EELOAD_OUTER_DATA@@ v0=%08x%08x v1=%08x%08x s0=%08x s1=%08x "
                    "mem[8FF20]=%08x mem[8FF24]=%08x mem[93580+24]=%08x",
                    cpuRegs.GPR.r[2].UL[1], cpuRegs.GPR.r[2].UL[0],
                    cpuRegs.GPR.r[3].UL[1], cpuRegs.GPR.r[3].UL[0],
                    cpuRegs.GPR.r[16].UL[0], cpuRegs.GPR.r[17].UL[0],
                    memRead32(0x8FF20u), memRead32(0x8FF24u),
                    memRead32(0x935A4u));
            }
        }
        // [iter665] @@SIF_CLIENT_DUMP@@ moved to outside %600 block (see below)
        // [iter422] @@PS1DRV_LOOP_DUMP@@ - PS1DRV ホットloop 0x00205AC0-0x00205B0F をダンプ
        // 0x00205af0 の命令内容 + 0x00205af8 のブランチターゲット内容をverifyする
        // Removal condition: 0x00205af0 の命令と T0 待機loopの終了conditionがafter determined
        Console.WriteLn("@@PS1DRV_LOOP_DUMP@@ [205AC0]: "
            "%08x %08x %08x %08x | %08x %08x %08x %08x | "
            "%08x %08x %08x %08x | %08x %08x %08x %08x | "
            "%08x %08x %08x %08x",
            memRead32(0x00205AC0u), memRead32(0x00205AC4u),
            memRead32(0x00205AC8u), memRead32(0x00205ACCu),
            memRead32(0x00205AD0u), memRead32(0x00205AD4u),
            memRead32(0x00205AD8u), memRead32(0x00205ADCu),
            memRead32(0x00205AE0u), memRead32(0x00205AE4u),
            memRead32(0x00205AE8u), memRead32(0x00205AECu),
            memRead32(0x00205AF0u), memRead32(0x00205AF4u),
            memRead32(0x00205AF8u), memRead32(0x00205AFCu),
            memRead32(0x00205B00u), memRead32(0x00205B04u),
            memRead32(0x00205B08u), memRead32(0x00205B0Cu));
        // [iter428] 0x205B10-0x205B6F ダンプ: 0x205B20 サブloopの内容verify
        // Removal condition: PS1DRV 0x205B20 loopの脱出conditionafter determined
        Console.WriteLn("@@PS1DRV_B20_DUMP@@ [205B10]: "
            "%08x %08x %08x %08x | %08x %08x %08x %08x | "
            "%08x %08x %08x %08x | %08x %08x %08x %08x | "
            "%08x %08x %08x %08x | %08x %08x %08x %08x",
            memRead32(0x00205B10u), memRead32(0x00205B14u),
            memRead32(0x00205B18u), memRead32(0x00205B1Cu),
            memRead32(0x00205B20u), memRead32(0x00205B24u),
            memRead32(0x00205B28u), memRead32(0x00205B2Cu),
            memRead32(0x00205B30u), memRead32(0x00205B34u),
            memRead32(0x00205B38u), memRead32(0x00205B3Cu),
            memRead32(0x00205B40u), memRead32(0x00205B44u),
            memRead32(0x00205B48u), memRead32(0x00205B4Cu),
            memRead32(0x00205B50u), memRead32(0x00205B54u),
            memRead32(0x00205B58u), memRead32(0x00205B5Cu),
            memRead32(0x00205B60u), memRead32(0x00205B64u),
            memRead32(0x00205B68u), memRead32(0x00205B6Cu));
        // [iter269] EE 8000fe9c 以降の命令ダンプ: 第3チェックの内容を特定するため固定addressを読む
        // Removal condition: EE PC 8000fe74 スタックのcauseが確定した後
        Console.WriteLn("@@EE_FUNC_CONT@@ [8000fea0]: "
            "%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
            memRead32(0x8000fea0u), memRead32(0x8000fea4u),
            memRead32(0x8000fea8u), memRead32(0x8000feacu),
            memRead32(0x8000feb0u), memRead32(0x8000feb4u),
            memRead32(0x8000feb8u), memRead32(0x8000febcu),
            memRead32(0x8000fec0u), memRead32(0x8000fec4u),
            memRead32(0x8000fec8u), memRead32(0x8000feccu));
        // [iter271] 呼び元 800126cc 付近と外側function 8000fdc0-8000fe70 のダンプ
        // EE PC=8000fe74 で v0=0 を返すが外側が何をするかverify
        // Removal condition: EE スタックcauseが確定した後
        Console.WriteLn("@@EE_CALLER_DUMP@@ [800126b0]: "
            "%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
            memRead32(0x800126b0u), memRead32(0x800126b4u),
            memRead32(0x800126b8u), memRead32(0x800126bcu),
            memRead32(0x800126c0u), memRead32(0x800126c4u),
            memRead32(0x800126c8u), memRead32(0x800126ccu),
            memRead32(0x800126d0u), memRead32(0x800126d4u),
            memRead32(0x800126d8u), memRead32(0x800126dcu));
        // [iter272] 8000fde8functionの残りと、BEQ先の 8000fe00-8000fe6c をaddダンプ
        Console.WriteLn("@@EE_FUNC_8FDE8@@ [8000fde8]: "
            "%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
            memRead32(0x8000fde8u), memRead32(0x8000fdecu),
            memRead32(0x8000fdf0u), memRead32(0x8000fdf4u),
            memRead32(0x8000fdf8u), memRead32(0x8000fdfcu),
            memRead32(0x8000fe00u), memRead32(0x8000fe04u),
            memRead32(0x8000fe08u), memRead32(0x8000fe0cu),
            memRead32(0x8000fe10u), memRead32(0x8000fe14u),
            memRead32(0x8000fe18u), memRead32(0x8000fe1cu),
            memRead32(0x8000fe20u), memRead32(0x8000fe24u));
        Console.WriteLn("@@EE_FUNC_8FE28@@ [8000fe28]: "
            "%08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
            memRead32(0x8000fe28u), memRead32(0x8000fe2cu),
            memRead32(0x8000fe30u), memRead32(0x8000fe34u),
            memRead32(0x8000fe38u), memRead32(0x8000fe3cu),
            memRead32(0x8000fe40u), memRead32(0x8000fe44u),
            memRead32(0x8000fe48u), memRead32(0x8000fe4cu),
            memRead32(0x8000fe50u), memRead32(0x8000fe54u),
            memRead32(0x8000fe58u), memRead32(0x8000fe5cu),
            memRead32(0x8000fe60u), memRead32(0x8000fe64u));
        // [iter188] @@EE_EPC_AREA@@ – EPC周辺4ワード + ERET例外handler全5ワード + excvec[80]
        // change理由: EPC=0x00AC4404 の Trap命令実体 + 0x80000180 ERET handlerの完全性verify
        // Removal condition: Trap命令と ERET handlerのstateが確定し、次のfix方針が決まり次第delete
        {
            const u32 epc_base = cpuRegs.CP0.r[14] & ~0xFu;
            Console.WriteLn("@@EE_EPC_AREA@@ bev=%u epc[%X]=%08x [%X]=%08x [%X]=%08x [%X]=%08x excvec[80]=%08x excvec[180]=%08x [184]=%08x [188]=%08x [18C]=%08x [190]=%08x",
                (cpuRegs.CP0.r[12] >> 22) & 1u,
                epc_base,    memRead32(epc_base),
                epc_base+4,  memRead32(epc_base+4),
                epc_base+8,  memRead32(epc_base+8),
                epc_base+12, memRead32(epc_base+12),
                memRead32(0x80u),
                memRead32(0x180u), memRead32(0x184u), memRead32(0x188u),
                memRead32(0x18Cu), memRead32(0x190u));
        }
        // [iter378] @@OSDSYS_LOOP_INSN@@ - excvec[200] (EE interrupt vector) + OSDSYS loop PC insns
        // Removal condition: OSDSYS loop内容と割込みベクタstateafter determined
        {
            Console.WriteLn("@@OSDSYS_LOOP_INSN@@ excvec[200]=%08x [204]=%08x [208]=%08x [20C]=%08x [210]=%08x [214]=%08x [218]=%08x [21C]=%08x",
                memRead32(0x200u), memRead32(0x204u), memRead32(0x208u), memRead32(0x20Cu),
                memRead32(0x210u), memRead32(0x214u), memRead32(0x218u), memRead32(0x21Cu));
            Console.WriteLn("@@OSDSYS_LOOP_INSN@@ [1000A4]=%08x [A8]=%08x [AC]=%08x [B0]=%08x [B4]=%08x [B8]=%08x [BC]=%08x [C0]=%08x",
                memRead32(0x1000A4u), memRead32(0x1000A8u), memRead32(0x1000ACu), memRead32(0x1000B0u),
                memRead32(0x1000B4u), memRead32(0x1000B8u), memRead32(0x1000BCu), memRead32(0x1000C0u));
            Console.WriteLn("@@OSDSYS_LOOP_INSN@@ [10036C]=%08x [70]=%08x [74]=%08x [78]=%08x [7C]=%08x [80]=%08x [84]=%08x [88]=%08x",
                memRead32(0x10036Cu), memRead32(0x100370u), memRead32(0x100374u), memRead32(0x100378u),
                memRead32(0x10037Cu), memRead32(0x100380u), memRead32(0x100384u), memRead32(0x100388u));
        }
        // [iter210] @@SIF_STATE@@ – SIFcontrolregister + D5(SIF0)/D6(SIF1) DMA + DMAC_CTRL
        // Removal condition: SIF DMA デッドロックcauseafter determined
        Console.WriteLn("@@SIF_STATE@@ sif_ctrl=%08x bd5=%08x bd6=%08x "
            "D5_CHCR=%08x D5_QWC=%08x D6_CHCR=%08x D6_QWC=%08x DMAC=%08x "
            "sbus_f200=%08x sbus_f210=%08x sbus_f220=%08x sbus_f230=%08x sbus_f240=%08x "
            "INTC_STAT=%08x INTC_MASK=%08x D2_CHCR=%08x D2_QWC=%08x "
            "ee23fc8=%08x ee23fcc=%08x ee23fd4=%08x ee24100=%08x ee24124=%08x",
            psHu32(0xF240), psHu32(0xF250), psHu32(0xF260),
            psHu32(0xC000), psHu32(0xC020),
            psHu32(0xC400), psHu32(0xC420),
            psHu32(0xE000),
            psHu32(0xF200), psHu32(0xF210), psHu32(0xF220),
            psHu32(0xF230), psHu32(0xF240),
            psHu32(0xF000), psHu32(0xF010),
            psHu32(0xA000), psHu32(0xA030),
            memRead32(0x80023fc8u), memRead32(0x80023fccu), memRead32(0x80023fd4u), memRead32(0x80024100u),
            memRead32(0x80024124u));
        // [iter151] @@BIOS_EXC_VEC@@ – BIOS ROM 一般例外handler 0xBFC00380 を一回だけダンプ
        // Removal condition: BIOS ROM SYSCALL #60 handlerの EPC 更新有無と実行パスが確定次第delete
        if (gsvsync_count == 600) {
            Console.WriteLn("@@BIOS_EXC_VEC@@ 0xBFC00380: "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0xBFC00380u), memRead32(0xBFC00384u),
                memRead32(0xBFC00388u), memRead32(0xBFC0038Cu),
                memRead32(0xBFC00390u), memRead32(0xBFC00394u),
                memRead32(0xBFC00398u), memRead32(0xBFC0039Cu),
                memRead32(0xBFC003A0u), memRead32(0xBFC003A4u),
                memRead32(0xBFC003A8u), memRead32(0xBFC003ACu),
                memRead32(0xBFC003B0u), memRead32(0xBFC003B4u),
                memRead32(0xBFC003B8u), memRead32(0xBFC003BCu));
            // [iter152] @@EE_SYSCALL_DISPATCH@@ – SYSCALL dispatch table entry + EELOAD LoadExecPS2 先頭
            // Removal condition: EELOAD LoadExecPS2 ロードfailのroot cause（OSDSYS 未発見 or ELF ロードfail等）確定次第delete
            Console.WriteLn("@@EE_SYSCALL_DISPATCH@@ disp[BFC00760]=%08x eeload_handler[83860]=%08x [64]=%08x [68]=%08x [6C]=%08x",
                memRead32(0xBFC00760u),
                memRead32(0x83860u), memRead32(0x83864u),
                memRead32(0x83868u), memRead32(0x8386Cu));
            // [iter153] @@BIOS_SYSCALL_HANDLER@@ – BIOS ROM SYSCALL handler 0xBFC00560 先頭12ワードダンプ
            // Removal condition: SYSCALL #60 (LoadExecPS2) の OSDSYS ロードfail箇所が確定次第delete
            Console.WriteLn("@@BIOS_SYSCALL_HANDLER@@ 0xBFC00560: "
                "%08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x",
                memRead32(0xBFC00560u), memRead32(0xBFC00564u),
                memRead32(0xBFC00568u), memRead32(0xBFC0056Cu),
                memRead32(0xBFC00570u), memRead32(0xBFC00574u),
                memRead32(0xBFC00578u), memRead32(0xBFC0057Cu),
                memRead32(0xBFC00580u), memRead32(0xBFC00584u),
                memRead32(0xBFC00588u), memRead32(0xBFC0058Cu));
            // [iter180] @@BIOS_9FC43_PROLOG_DUMP@@ – 9FC43760-9FC437C0（プロローグ先頭）をaddダンプ
            // $s0=0x75297014corruptのcause（どの命令がs0をセットするか）を特定するため
            // Removal condition: $s0config箇所とその値のroot causeafter determined
            Console.WriteLn("@@BIOS_9FC43_PROLOG_DUMP@@ P[9FC43760]: "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x9FC43760u), memRead32(0x9FC43764u),
                memRead32(0x9FC43768u), memRead32(0x9FC4376Cu),
                memRead32(0x9FC43770u), memRead32(0x9FC43774u),
                memRead32(0x9FC43778u), memRead32(0x9FC4377Cu),
                memRead32(0x9FC43780u), memRead32(0x9FC43784u),
                memRead32(0x9FC43788u), memRead32(0x9FC4378Cu),
                memRead32(0x9FC43790u), memRead32(0x9FC43794u),
                memRead32(0x9FC43798u), memRead32(0x9FC4379Cu),
                memRead32(0x9FC437A0u), memRead32(0x9FC437A4u),
                memRead32(0x9FC437A8u), memRead32(0x9FC437ACu),
                memRead32(0x9FC437B0u), memRead32(0x9FC437B4u),
                memRead32(0x9FC437B8u), memRead32(0x9FC437BCu),
                memRead32(0x9FC437C0u), memRead32(0x9FC437C4u),
                memRead32(0x9FC437C8u), memRead32(0x9FC437CCu),
                memRead32(0x9FC437D0u), memRead32(0x9FC437D4u),
                memRead32(0x9FC437D8u), memRead32(0x9FC437DCu));
            // [iter178] @@BIOS_9FC43_LOOP_DUMP@@ – 9FC433C0-9FC43820rangeを32ワード×2ブロックでダンプ
            // EEが9FC43xxxで永久loopしているcause（外側loop構造）を特定するため
            // Removal condition: 外側loopの先頭addressとloop終了conditionがafter determined
            Console.WriteLn("@@BIOS_9FC43_LOOP_DUMP@@ A[9FC433C0]: "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x9FC433C0u), memRead32(0x9FC433C4u),
                memRead32(0x9FC433C8u), memRead32(0x9FC433CCu),
                memRead32(0x9FC433D0u), memRead32(0x9FC433D4u),
                memRead32(0x9FC433D8u), memRead32(0x9FC433DCu),
                memRead32(0x9FC433E0u), memRead32(0x9FC433E4u),
                memRead32(0x9FC433E8u), memRead32(0x9FC433ECu),
                memRead32(0x9FC433F0u), memRead32(0x9FC433F4u),
                memRead32(0x9FC433F8u), memRead32(0x9FC433FCu),
                memRead32(0x9FC43400u), memRead32(0x9FC43404u),
                memRead32(0x9FC43408u), memRead32(0x9FC4340Cu),
                memRead32(0x9FC43410u), memRead32(0x9FC43414u),
                memRead32(0x9FC43418u), memRead32(0x9FC4341Cu),
                memRead32(0x9FC43420u), memRead32(0x9FC43424u),
                memRead32(0x9FC43428u), memRead32(0x9FC4342Cu),
                memRead32(0x9FC43430u), memRead32(0x9FC43434u),
                memRead32(0x9FC43438u), memRead32(0x9FC4343Cu));
            Console.WriteLn("@@BIOS_9FC43_LOOP_DUMP@@ B[9FC43440]: "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x9FC43440u), memRead32(0x9FC43444u),
                memRead32(0x9FC43448u), memRead32(0x9FC4344Cu),
                memRead32(0x9FC43450u), memRead32(0x9FC43454u),
                memRead32(0x9FC43458u), memRead32(0x9FC4345Cu),
                memRead32(0x9FC43460u), memRead32(0x9FC43464u),
                memRead32(0x9FC43468u), memRead32(0x9FC4346Cu),
                memRead32(0x9FC43470u), memRead32(0x9FC43474u),
                memRead32(0x9FC43478u), memRead32(0x9FC4347Cu),
                memRead32(0x9FC43480u), memRead32(0x9FC43484u),
                memRead32(0x9FC43488u), memRead32(0x9FC4348Cu),
                memRead32(0x9FC43490u), memRead32(0x9FC43494u),
                memRead32(0x9FC43498u), memRead32(0x9FC4349Cu),
                memRead32(0x9FC434A0u), memRead32(0x9FC434A4u),
                memRead32(0x9FC434A8u), memRead32(0x9FC434ACu),
                memRead32(0x9FC434B0u), memRead32(0x9FC434B4u),
                memRead32(0x9FC434B8u), memRead32(0x9FC434BCu));
            Console.WriteLn("@@BIOS_9FC437_LOOP_DUMP@@ C[9FC437C0]: "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x "
                "%08x %08x %08x %08x %08x %08x %08x %08x",
                memRead32(0x9FC437C0u), memRead32(0x9FC437C4u),
                memRead32(0x9FC437C8u), memRead32(0x9FC437CCu),
                memRead32(0x9FC437D0u), memRead32(0x9FC437D4u),
                memRead32(0x9FC437D8u), memRead32(0x9FC437DCu),
                memRead32(0x9FC437E0u), memRead32(0x9FC437E4u),
                memRead32(0x9FC437E8u), memRead32(0x9FC437ECu),
                memRead32(0x9FC437F0u), memRead32(0x9FC437F4u),
                memRead32(0x9FC437F8u), memRead32(0x9FC437FCu),
                memRead32(0x9FC43800u), memRead32(0x9FC43804u),
                memRead32(0x9FC43808u), memRead32(0x9FC4380Cu),
                memRead32(0x9FC43810u), memRead32(0x9FC43814u),
                memRead32(0x9FC43818u), memRead32(0x9FC4381Cu),
                memRead32(0x9FC43820u), memRead32(0x9FC43824u),
                memRead32(0x9FC43828u), memRead32(0x9FC4382Cu),
                memRead32(0x9FC43830u), memRead32(0x9FC43834u),
                memRead32(0x9FC43838u), memRead32(0x9FC4383Cu));
        }
    }
    // [iter112] @@IOP_FATAL_ENTRY@@ – detect transition INTO bfc02454 + dump a2 search key
    {
        static u32 s_prev_iop_pc = 0;
        u32 cur_iop_pc = psxRegs.pc;
        if (cur_iop_pc == 0xBFC02454u && s_prev_iop_pc != 0xBFC02454u && s_prev_iop_pc != 0) {
            const u32 a2 = psxRegs.GPR.r[6];
            const u32 a2_0 = iopMemRead32(a2 + 0);   // search key: module name[0:3]
            const u32 a2_4 = iopMemRead32(a2 + 4);   // search key: module name[4:7]
            const u32 a2_8 = iopMemRead32(a2 + 8) & 0xFFFF; // search key: ExtInfoSize (halfword)
            Console.WriteLn("@@IOP_FATAL_ENTRY@@ vsync=%d from=%08x iop_cycle=%u "
                "v0=%08x a0=%08x a1=%08x a2=%08x t0=%08x t1=%08x t4=%08x ra=%08x "
                "a2[0]=%08x a2[4]=%08x a2[8]=%04x",
                gsvsync_count, s_prev_iop_pc, psxRegs.cycle,
                psxRegs.GPR.r[2], psxRegs.GPR.r[4], psxRegs.GPR.r[5], a2,
                psxRegs.GPR.r[8], psxRegs.GPR.r[9], psxRegs.GPR.r[12],
                psxRegs.GPR.r[31],
                a2_0, a2_4, a2_8);
        }
        s_prev_iop_pc = cur_iop_pc;
    }
    // [iter163] @@BIOS_480_ENTRY@@ – EE PC が OSDSYS 実行後に 0xBFC00480 (BEV=1 割り込みhandler) に
    // 遷移した最初の N 回をdetectし COP0 Status/Cause/EPC/ErrorEPC をダンプ。
    // Removal condition: EE が 0xBFC00480 に入るcause (タイマー割り込みloop / 例外スタック) が確定次第delete
    {
        static bool s_480_seen_osdsys = false;
        static int  s_480_n = 0;
        constexpr int k480Max = 10;
        u32 cur_pc = cpuRegs.pc;
        // OSDSYS 到達を記録 (0x00100000-0x001FFFFF)
        if (cur_pc >= 0x00100000u && cur_pc < 0x00200000u)
            s_480_seen_osdsys = true;
        // OSDSYS 到達後に 0xBFC00480 を初めて踏んだとき
        if (s_480_seen_osdsys && cur_pc == 0xBFC00480u && s_480_n < k480Max) {
            u32 status    = cpuRegs.CP0.r[12];
            u32 cause     = cpuRegs.CP0.r[13];
            u32 epc       = cpuRegs.CP0.r[14];
            u32 error_epc = cpuRegs.CP0.r[30];
            Console.WriteLn("@@BIOS_480_ENTRY@@ n=%d vsync=%d "
                "Status=%08x Cause=%08x EPC=%08x ErrorEPC=%08x "
                "ra=%08x v0=%08x a0=%08x",
                s_480_n, gsvsync_count,
                status, cause, epc, error_epc,
                cpuRegs.GPR.r[31].UL[0],
                cpuRegs.GPR.r[2].UL[0],
                cpuRegs.GPR.r[4].UL[0]);
            s_480_n++;
        }
    }
    // [iter148] @@EE_SIF_INIT_WATCH@@ – EE PC が 0x9FFA1C00-0x9FFA1CFF (SIF initfunction、SBUS_F220writeみinclude) に
    // 入ったかどうかを vsync 毎に監視。遷移を初回detect時にのみregisterをダンプ。
    // Removal condition: EE_SIF_INIT_WATCH ファイアまたは非ファイアがverifyされ SBUS_F220 call経路が確定次第delete
    {
        static u32 s_sif_watch_prev_pc = 0;
        static u32 s_sif_watch_n = 0;
        u32 cur_ee_pc = cpuRegs.pc;
        bool in_sif_range = (cur_ee_pc >= 0x9FFA1C00u && cur_ee_pc < 0x9FFA1D00u);
        bool was_in_range = (s_sif_watch_prev_pc >= 0x9FFA1C00u && s_sif_watch_prev_pc < 0x9FFA1D00u);
        if (in_sif_range && !was_in_range && s_sif_watch_n < 8) {
            ++s_sif_watch_n;
            Console.WriteLn("@@EE_SIF_INIT_WATCH@@ n=%u vsync=%d pc=%08x from=%08x "
                "a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x",
                s_sif_watch_n, gsvsync_count, cur_ee_pc, s_sif_watch_prev_pc,
                cpuRegs.GPR.r[4].UL[0], cpuRegs.GPR.r[5].UL[0],
                cpuRegs.GPR.r[6].UL[0], cpuRegs.GPR.r[7].UL[0],
                cpuRegs.GPR.r[31].UL[0]);
        }
        s_sif_watch_prev_pc = cur_ee_pc;
    }
    // [iter666] @@SIF_CLIENT_DUMP@@ — SIF client structure at 0x93580 dump (vsync=8,10,12,15,20,30)
    // 目的: JIT vs Interpreter で client+0x24 (server ptr) が書かれるか比較
    // Removal condition: SIF bind completion causeafter determined
    {
        static int s_sif_dump_n = 0;
        if (s_sif_dump_n < 6 && (gsvsync_count == 8 || gsvsync_count == 10 || gsvsync_count == 12 ||
            gsvsync_count == 15 || gsvsync_count == 20 || gsvsync_count == 30)) {
            s_sif_dump_n++;
            const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
            Console.WriteLn("@@SIF_CLIENT_DUMP@@ [%s] vsync=%d [93580]+0: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode, gsvsync_count,
                memRead32(0x93580u), memRead32(0x93584u), memRead32(0x93588u), memRead32(0x9358Cu),
                memRead32(0x93590u), memRead32(0x93594u), memRead32(0x93598u), memRead32(0x9359Cu));
            Console.WriteLn("@@SIF_CLIENT_DUMP@@ [%s] vsync=%d [93580]+20: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode, gsvsync_count,
                memRead32(0x935A0u), memRead32(0x935A4u), memRead32(0x935A8u), memRead32(0x935ACu),
                memRead32(0x935B0u), memRead32(0x935B4u), memRead32(0x935B8u), memRead32(0x935BCu));
            // Also dump the D5 DMA handler code at 0x83fb0 (first 16 words)
            Console.WriteLn("@@SIF_HANDLER_CODE@@ [%s] vsync=%d [83fb0]: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode, gsvsync_count,
                memRead32(0x83FB0u), memRead32(0x83FB4u), memRead32(0x83FB8u), memRead32(0x83FBCu),
                memRead32(0x83FC0u), memRead32(0x83FC4u), memRead32(0x83FC8u), memRead32(0x83FCCu));
            Console.WriteLn("@@SIF_HANDLER_CODE@@ [%s] vsync=%d [83fd0]: %08x %08x %08x %08x %08x %08x %08x %08x",
                mode, gsvsync_count,
                memRead32(0x83FD0u), memRead32(0x83FD4u), memRead32(0x83FD8u), memRead32(0x83FDCu),
                memRead32(0x83FE0u), memRead32(0x83FE4u), memRead32(0x83FE8u), memRead32(0x83FECu));
        }
    }
    // [iter113] @@IOP_ROMDIR_DUMP@@ – dump ROMDIR entries at bfc02740 (up to 70 entries x 16B)
    // [iter154] 上限を 30 → 70 にextend: OSDSYS エントリを ROMDIR 後半で探索
    // Purpose: verify whether "IOPBOOT" module exists in SCPH-70000_JP ROMDIR
    if (gsvsync_count == 1) {
        u32 base = 0xBFC02740u;
        for (int i = 0; i < 105; i++, base += 0x10) {
            const u32 w0 = iopMemRead32(base+0x0); // name[0:3]
            if (w0 == 0) break;                    // null name = end of ROMDIR
            const u32 w4  = iopMemRead32(base+0x4);  // name[4:7]
            const u32 w8  = iopMemRead32(base+0x8);  // name[8:9] | extInfoSize[10:11]
            const u32 w12 = iopMemRead32(base+0xC);  // fileSize
            Console.WriteLn("@@IOP_ROMDIR_DUMP@@ [%02d] %08x name=%08x%08x ext=%04x size=%08x",
                i, base, w0, w4, (w8 & 0xFFFF), w12);
        }
    }

    // [TEMP_DIAG] iter29: at vsync==300, dump code at 9FC43100-9FC4313C and 9FC432B0-9FC432F0
    // to identify new loop at 9FC43120/9FC432C0/9FC432DC
    if (gsvsync_count == 300) {
        u32 cur_pc = cpuRegs.pc;
        u32 cur_sp = cpuRegs.GPR.r[29].UL[0];
        Console.WriteLn("[TEMP_DIAG] @@NEW_LOOP_DIAG@@ vsync=300 pc=%08x sp=%08x", cur_pc, cur_sp);
        // dump 9FC43100-9FC4313C (10 words)
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43100_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x "
            "i5=%08x i6=%08x i7=%08x i8=%08x i9=%08x",
            memRead32(0x9FC43100), memRead32(0x9FC43104),
            memRead32(0x9FC43108), memRead32(0x9FC4310C),
            memRead32(0x9FC43110), memRead32(0x9FC43114),
            memRead32(0x9FC43118), memRead32(0x9FC4311C),
            memRead32(0x9FC43120), memRead32(0x9FC43124));
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43128_CODE@@ "
            "i0=%08x i1=%08x i2=%08x i3=%08x i4=%08x "
            "i5=%08x i6=%08x i7=%08x i8=%08x i9=%08x",
            memRead32(0x9FC43128), memRead32(0x9FC4312C),
            memRead32(0x9FC43130), memRead32(0x9FC43134),
            memRead32(0x9FC43138), memRead32(0x9FC4313C),
            memRead32(0x9FC43140), memRead32(0x9FC43144),
            memRead32(0x9FC43148), memRead32(0x9FC4314C));
        // dump 9FC432B0-9FC432F0 (17 words, covers 9FC432C0 and 9FC432DC - already analyzed)
        Console.WriteLn("[TEMP_DIAG] @@BIOS_432B0_CODE@@ "
            "j0=%08x j1=%08x j2=%08x j3=%08x j4=%08x "
            "j5=%08x j6=%08x j7=%08x j8=%08x j9=%08x "
            "j10=%08x j11=%08x j12=%08x j13=%08x j14=%08x "
            "j15=%08x j16=%08x",
            memRead32(0x9FC432B0), memRead32(0x9FC432B4),
            memRead32(0x9FC432B8), memRead32(0x9FC432BC),
            memRead32(0x9FC432C0), memRead32(0x9FC432C4),
            memRead32(0x9FC432C8), memRead32(0x9FC432CC),
            memRead32(0x9FC432D0), memRead32(0x9FC432D4),
            memRead32(0x9FC432D8), memRead32(0x9FC432DC),
            memRead32(0x9FC432E0), memRead32(0x9FC432E4),
            memRead32(0x9FC432E8), memRead32(0x9FC432EC),
            memRead32(0x9FC432F0));
        // iter32: dump 9FC431F0-9FC432AC to fill gap between JAL@9FC431EC and BGEZ loop
        Console.WriteLn("[TEMP_DIAG] @@BIOS_431F0_CODE@@ "
            "g0=%08x g1=%08x g2=%08x g3=%08x g4=%08x "
            "g5=%08x g6=%08x g7=%08x g8=%08x g9=%08x "
            "g10=%08x g11=%08x g12=%08x g13=%08x g14=%08x "
            "g15=%08x g16=%08x g17=%08x g18=%08x g19=%08x "
            "g20=%08x g21=%08x g22=%08x g23=%08x",
            memRead32(0x9FC431F0), memRead32(0x9FC431F4),
            memRead32(0x9FC431F8), memRead32(0x9FC431FC),
            memRead32(0x9FC43200), memRead32(0x9FC43204),
            memRead32(0x9FC43208), memRead32(0x9FC4320C),
            memRead32(0x9FC43210), memRead32(0x9FC43214),
            memRead32(0x9FC43218), memRead32(0x9FC4321C),
            memRead32(0x9FC43220), memRead32(0x9FC43224),
            memRead32(0x9FC43228), memRead32(0x9FC4322C),
            memRead32(0x9FC43230), memRead32(0x9FC43234),
            memRead32(0x9FC43238), memRead32(0x9FC4323C),
            memRead32(0x9FC43240), memRead32(0x9FC43244),
            memRead32(0x9FC43248), memRead32(0x9FC432AC));
        // iter34: dump BIOS code at 9FC41164-9FC411A0 (15 words after TLB_INIT_FUNC call site)
        Console.WriteLn("[TEMP_DIAG] @@BIOS_41164_CODE@@ "
            "h0=%08x h1=%08x h2=%08x h3=%08x h4=%08x "
            "h5=%08x h6=%08x h7=%08x h8=%08x h9=%08x "
            "h10=%08x h11=%08x h12=%08x h13=%08x h14=%08x",
            memRead32(0x9FC41164), memRead32(0x9FC41168),
            memRead32(0x9FC4116C), memRead32(0x9FC41170),
            memRead32(0x9FC41174), memRead32(0x9FC41178),
            memRead32(0x9FC4117C), memRead32(0x9FC41180),
            memRead32(0x9FC41184), memRead32(0x9FC41188),
            memRead32(0x9FC4118C), memRead32(0x9FC41190),
            memRead32(0x9FC41194), memRead32(0x9FC41198),
            memRead32(0x9FC4119C));
        // iter33: dump EE RAM[0..0x7C] to understand outer loop at PC=0
        Console.WriteLn("[TEMP_DIAG] @@EE_RAM_0_CODE@@ "
            "r00=%08x r01=%08x r02=%08x r03=%08x r04=%08x r05=%08x r06=%08x r07=%08x "
            "r08=%08x r09=%08x r10=%08x r11=%08x r12=%08x r13=%08x r14=%08x r15=%08x "
            "r16=%08x r17=%08x r18=%08x r19=%08x r20=%08x r21=%08x r22=%08x r23=%08x "
            "r24=%08x r25=%08x r26=%08x r27=%08x r28=%08x r29=%08x r30=%08x r31=%08x",
            memRead32(0x00000000), memRead32(0x00000004),
            memRead32(0x00000008), memRead32(0x0000000C),
            memRead32(0x00000010), memRead32(0x00000014),
            memRead32(0x00000018), memRead32(0x0000001C),
            memRead32(0x00000020), memRead32(0x00000024),
            memRead32(0x00000028), memRead32(0x0000002C),
            memRead32(0x00000030), memRead32(0x00000034),
            memRead32(0x00000038), memRead32(0x0000003C),
            memRead32(0x00000040), memRead32(0x00000044),
            memRead32(0x00000048), memRead32(0x0000004C),
            memRead32(0x00000050), memRead32(0x00000054),
            memRead32(0x00000058), memRead32(0x0000005C),
            memRead32(0x00000060), memRead32(0x00000064),
            memRead32(0x00000068), memRead32(0x0000006C),
            memRead32(0x00000070), memRead32(0x00000074),
            memRead32(0x00000078), memRead32(0x0000007C));

        // iter31: dump 9FC43150-9FC431FC to find TLB_INIT_FUNC entry (ADDIU sp,sp,-0xA0 = 27BDFF60)
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43150_CODE@@ "
            "x0=%08x x1=%08x x2=%08x x3=%08x x4=%08x "
            "x5=%08x x6=%08x x7=%08x x8=%08x x9=%08x "
            "x10=%08x x11=%08x x12=%08x x13=%08x x14=%08x "
            "x15=%08x x16=%08x x17=%08x x18=%08x x19=%08x",
            memRead32(0x9FC43150), memRead32(0x9FC43154),
            memRead32(0x9FC43158), memRead32(0x9FC4315C),
            memRead32(0x9FC43160), memRead32(0x9FC43164),
            memRead32(0x9FC43168), memRead32(0x9FC4316C),
            memRead32(0x9FC43170), memRead32(0x9FC43174),
            memRead32(0x9FC43178), memRead32(0x9FC4317C),
            memRead32(0x9FC43180), memRead32(0x9FC43184),
            memRead32(0x9FC43188), memRead32(0x9FC4318C),
            memRead32(0x9FC43190), memRead32(0x9FC43194),
            memRead32(0x9FC43198), memRead32(0x9FC4319C));
        Console.WriteLn("[TEMP_DIAG] @@BIOS_431A0_CODE@@ "
            "y0=%08x y1=%08x y2=%08x y3=%08x y4=%08x "
            "y5=%08x y6=%08x y7=%08x y8=%08x y9=%08x "
            "y10=%08x y11=%08x y12=%08x y13=%08x y14=%08x "
            "y15=%08x y16=%08x y17=%08x y18=%08x y19=%08x",
            memRead32(0x9FC431A0), memRead32(0x9FC431A4),
            memRead32(0x9FC431A8), memRead32(0x9FC431AC),
            memRead32(0x9FC431B0), memRead32(0x9FC431B4),
            memRead32(0x9FC431B8), memRead32(0x9FC431BC),
            memRead32(0x9FC431C0), memRead32(0x9FC431C4),
            memRead32(0x9FC431C8), memRead32(0x9FC431CC),
            memRead32(0x9FC431D0), memRead32(0x9FC431D4),
            memRead32(0x9FC431D8), memRead32(0x9FC431DC),
            memRead32(0x9FC431E0), memRead32(0x9FC431E4),
            memRead32(0x9FC431E8), memRead32(0x9FC431EC));
        // also read saved ra from stack (sp+0x90) to find outer caller
        {
            u32 sp_val = cpuRegs.GPR.r[29].UL[0];
            u32 saved_ra_addr = sp_val + 0x90u;
            u32 saved_ra = memRead32(saved_ra_addr);
            Console.WriteLn("[TEMP_DIAG] @@SAVED_RA@@ sp=%08x saved_ra_addr=%08x saved_ra=%08x",
                sp_val, saved_ra_addr, saved_ra);
        }

        // iter30: dump code BEFORE TLB loop (9FC43200-9FC432AF, 44 words) to find outer loop
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43200_CODE@@ "
            "a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x "
            "a5=%08x a6=%08x a7=%08x a8=%08x a9=%08x "
            "a10=%08x a11=%08x a12=%08x a13=%08x a14=%08x "
            "a15=%08x a16=%08x a17=%08x a18=%08x a19=%08x",
            memRead32(0x9FC43200), memRead32(0x9FC43204),
            memRead32(0x9FC43208), memRead32(0x9FC4320C),
            memRead32(0x9FC43210), memRead32(0x9FC43214),
            memRead32(0x9FC43218), memRead32(0x9FC4321C),
            memRead32(0x9FC43220), memRead32(0x9FC43224),
            memRead32(0x9FC43228), memRead32(0x9FC4322C),
            memRead32(0x9FC43230), memRead32(0x9FC43234),
            memRead32(0x9FC43238), memRead32(0x9FC4323C),
            memRead32(0x9FC43240), memRead32(0x9FC43244),
            memRead32(0x9FC43248), memRead32(0x9FC4324C));
        Console.WriteLn("[TEMP_DIAG] @@BIOS_43250_CODE@@ "
            "b0=%08x b1=%08x b2=%08x b3=%08x b4=%08x "
            "b5=%08x b6=%08x b7=%08x b8=%08x b9=%08x "
            "b10=%08x b11=%08x b12=%08x b13=%08x b14=%08x "
            "b15=%08x b16=%08x b17=%08x b18=%08x b19=%08x "
            "b20=%08x b21=%08x b22=%08x",
            memRead32(0x9FC43250), memRead32(0x9FC43254),
            memRead32(0x9FC43258), memRead32(0x9FC4325C),
            memRead32(0x9FC43260), memRead32(0x9FC43264),
            memRead32(0x9FC43268), memRead32(0x9FC4326C),
            memRead32(0x9FC43270), memRead32(0x9FC43274),
            memRead32(0x9FC43278), memRead32(0x9FC4327C),
            memRead32(0x9FC43280), memRead32(0x9FC43284),
            memRead32(0x9FC43288), memRead32(0x9FC4328C),
            memRead32(0x9FC43290), memRead32(0x9FC43294),
            memRead32(0x9FC43298), memRead32(0x9FC4329C),
            memRead32(0x9FC432A0), memRead32(0x9FC432A4),
            memRead32(0x9FC432A8));
        // also dump code AFTER epilogue (9FC432F4-9FC43334) to see return context
        Console.WriteLn("[TEMP_DIAG] @@BIOS_432F4_CODE@@ "
            "c0=%08x c1=%08x c2=%08x c3=%08x c4=%08x "
            "c5=%08x c6=%08x c7=%08x c8=%08x c9=%08x "
            "c10=%08x c11=%08x c12=%08x c13=%08x c14=%08x "
            "c15=%08x c16=%08x",
            memRead32(0x9FC432F4), memRead32(0x9FC432F8),
            memRead32(0x9FC432FC), memRead32(0x9FC43300),
            memRead32(0x9FC43304), memRead32(0x9FC43308),
            memRead32(0x9FC4330C), memRead32(0x9FC43310),
            memRead32(0x9FC43314), memRead32(0x9FC43318),
            memRead32(0x9FC4331C), memRead32(0x9FC43320),
            memRead32(0x9FC43324), memRead32(0x9FC43328),
            memRead32(0x9FC4332C), memRead32(0x9FC43330),
            memRead32(0x9FC43334));

    }

    // [TEMP_DIAG] iter41: vsync==1 one-shot — dump BIOS code that causes Bus Error at 0x4000F4xx
    // 9FC42984 = DISPATCH target before crash; 9FC412F4 = EE PC reported by @@BUS_ERROR_PC@@
    // Removal condition: 0x4000F4xx ストア元命令after identifieddelete
    if (gsvsync_count == 1) {
        Console.WriteLn("[TEMP_DIAG] @@BIOS_42984_CODE@@ "
            "p0=%08x p1=%08x p2=%08x p3=%08x p4=%08x p5=%08x p6=%08x p7=%08x "
            "p8=%08x p9=%08x pA=%08x pB=%08x pC=%08x pD=%08x pE=%08x pF=%08x "
            "q0=%08x q1=%08x q2=%08x q3=%08x q4=%08x q5=%08x q6=%08x q7=%08x",
            memRead32(0x9FC42984), memRead32(0x9FC42988),
            memRead32(0x9FC4298C), memRead32(0x9FC42990),
            memRead32(0x9FC42994), memRead32(0x9FC42998),
            memRead32(0x9FC4299C), memRead32(0x9FC429A0),
            memRead32(0x9FC429A4), memRead32(0x9FC429A8),
            memRead32(0x9FC429AC), memRead32(0x9FC429B0),
            memRead32(0x9FC429B4), memRead32(0x9FC429B8),
            memRead32(0x9FC429BC), memRead32(0x9FC429C0),
            memRead32(0x9FC429C4), memRead32(0x9FC429C8),
            memRead32(0x9FC429CC), memRead32(0x9FC429D0),
            memRead32(0x9FC429D4), memRead32(0x9FC429D8),
            memRead32(0x9FC429DC), memRead32(0x9FC429E0));
        Console.WriteLn("[TEMP_DIAG] @@BIOS_429E0_CODE@@ "
            "r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x "
            "r8=%08x r9=%08x rA=%08x rB=%08x rC=%08x rD=%08x rE=%08x rF=%08x",
            memRead32(0x9FC429E4), memRead32(0x9FC429E8),
            memRead32(0x9FC429EC), memRead32(0x9FC429F0),
            memRead32(0x9FC429F4), memRead32(0x9FC429F8),
            memRead32(0x9FC429FC), memRead32(0x9FC42A00),
            memRead32(0x9FC42A04), memRead32(0x9FC42A08),
            memRead32(0x9FC42A0C), memRead32(0x9FC42A10),
            memRead32(0x9FC42A14), memRead32(0x9FC42A18),
            memRead32(0x9FC42A1C), memRead32(0x9FC42A20));
        // Also dump 9FC412F4-9FC41340: EE PC at Bus Error (reported by @@BUS_ERROR_PC@@)
        Console.WriteLn("[TEMP_DIAG] @@BIOS_412F4_CODE@@ "
            "s0=%08x s1=%08x s2=%08x s3=%08x s4=%08x s5=%08x s6=%08x s7=%08x "
            "s8=%08x s9=%08x sA=%08x sB=%08x sC=%08x sD=%08x sE=%08x sF=%08x "
            "t0=%08x t1=%08x t2=%08x t3=%08x t4=%08x t5=%08x t6=%08x t7=%08x",
            memRead32(0x9FC412F4), memRead32(0x9FC412F8),
            memRead32(0x9FC412FC), memRead32(0x9FC41300),
            memRead32(0x9FC41304), memRead32(0x9FC41308),
            memRead32(0x9FC4130C), memRead32(0x9FC41310),
            memRead32(0x9FC41314), memRead32(0x9FC41318),
            memRead32(0x9FC4131C), memRead32(0x9FC41320),
            memRead32(0x9FC41324), memRead32(0x9FC41328),
            memRead32(0x9FC4132C), memRead32(0x9FC41330),
            memRead32(0x9FC41334), memRead32(0x9FC41338),
            memRead32(0x9FC4133C), memRead32(0x9FC41340),
            memRead32(0x9FC41344), memRead32(0x9FC41348),
            memRead32(0x9FC4134C), memRead32(0x9FC41350));
    }

    // [TEMP_DIAG] iPSX2_BIOS_9FC41048_DIAG: code/snap/load probe for BIOS wait loop
    // Removal condition: BIOS 0x9FC41048loop根因after determineddelete
    {
        static int s_diag9fc_cfg = -1;
        if (s_diag9fc_cfg < 0) {
            s_diag9fc_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_BIOS_9FC41048_DIAG", false) ? 1 : 0;
            Console.WriteLn("@@CFG@@ iPSX2_BIOS_9FC41048_DIAG=%d", s_diag9fc_cfg);
        }
        if (s_diag9fc_cfg == 1) {
            // CODE: one-shot dump 0x9FC41040..0x9FC410BF (32 x u32; extended for crash analysis)
            static bool s_code_done = false;
            if (!s_code_done) {
                s_code_done = true;
                Console.WriteLn("@@BIOS_9FC41048_CODE@@ "
                    "40=%08x 44=%08x 48=%08x 4C=%08x "
                    "50=%08x 54=%08x 58=%08x 5C=%08x "
                    "60=%08x 64=%08x 68=%08x 6C=%08x "
                    "70=%08x 74=%08x 78=%08x 7C=%08x",
                    memRead32(0x9FC41040), memRead32(0x9FC41044),
                    memRead32(0x9FC41048), memRead32(0x9FC4104C),
                    memRead32(0x9FC41050), memRead32(0x9FC41054),
                    memRead32(0x9FC41058), memRead32(0x9FC4105C),
                    memRead32(0x9FC41060), memRead32(0x9FC41064),
                    memRead32(0x9FC41068), memRead32(0x9FC4106C),
                    memRead32(0x9FC41070), memRead32(0x9FC41074),
                    memRead32(0x9FC41078), memRead32(0x9FC4107C));
                // [extended] 9FC41080..9FC410BF (next 16 words) for crash PC=0x02000000 cause analysis
                Console.WriteLn("@@BIOS_9FC41080_CODE@@ "
                    "80=%08x 84=%08x 88=%08x 8C=%08x "
                    "90=%08x 94=%08x 98=%08x 9C=%08x "
                    "A0=%08x A4=%08x A8=%08x AC=%08x "
                    "B0=%08x B4=%08x B8=%08x BC=%08x",
                    memRead32(0x9FC41080), memRead32(0x9FC41084),
                    memRead32(0x9FC41088), memRead32(0x9FC4108C),
                    memRead32(0x9FC41090), memRead32(0x9FC41094),
                    memRead32(0x9FC41098), memRead32(0x9FC4109C),
                    memRead32(0x9FC410A0), memRead32(0x9FC410A4),
                    memRead32(0x9FC410A8), memRead32(0x9FC410AC),
                    memRead32(0x9FC410B0), memRead32(0x9FC410B4),
                    memRead32(0x9FC410B8), memRead32(0x9FC410BC));
                // [extended2] 9FC410C0..9FC4113F (after outer-loop exit) -- find source of PC=0x02000000
                Console.WriteLn("@@BIOS_9FC410C0_CODE@@ "
                    "C0=%08x C4=%08x C8=%08x CC=%08x "
                    "D0=%08x D4=%08x D8=%08x DC=%08x "
                    "E0=%08x E4=%08x E8=%08x EC=%08x "
                    "F0=%08x F4=%08x F8=%08x FC=%08x",
                    memRead32(0x9FC410C0), memRead32(0x9FC410C4),
                    memRead32(0x9FC410C8), memRead32(0x9FC410CC),
                    memRead32(0x9FC410D0), memRead32(0x9FC410D4),
                    memRead32(0x9FC410D8), memRead32(0x9FC410DC),
                    memRead32(0x9FC410E0), memRead32(0x9FC410E4),
                    memRead32(0x9FC410E8), memRead32(0x9FC410EC),
                    memRead32(0x9FC410F0), memRead32(0x9FC410F4),
                    memRead32(0x9FC410F8), memRead32(0x9FC410FC));
                Console.WriteLn("@@BIOS_9FC41100_CODE@@ "
                    "100=%08x 104=%08x 108=%08x 10C=%08x "
                    "110=%08x 114=%08x 118=%08x 11C=%08x "
                    "120=%08x 124=%08x 128=%08x 12C=%08x "
                    "130=%08x 134=%08x 138=%08x 13C=%08x",
                    memRead32(0x9FC41100), memRead32(0x9FC41104),
                    memRead32(0x9FC41108), memRead32(0x9FC4110C),
                    memRead32(0x9FC41110), memRead32(0x9FC41114),
                    memRead32(0x9FC41118), memRead32(0x9FC4111C),
                    memRead32(0x9FC41120), memRead32(0x9FC41124),
                    memRead32(0x9FC41128), memRead32(0x9FC4112C),
                    memRead32(0x9FC41130), memRead32(0x9FC41134),
                    memRead32(0x9FC41138), memRead32(0x9FC4113C));
            }
            // SNAP + LOAD: capped at 16 per run, only when EE PC is in loop range
            static int s_snap_n = 0;
            const u32 cur_pc = cpuRegs.pc;
            if (s_snap_n < 16 && cur_pc >= 0x9FC41040u && cur_pc <= 0x9FC41060u) {
                ++s_snap_n;
                Console.WriteLn("@@BIOS_9FC41048_SNAP@@ n=%d vsync=%d pc=%08x "
                    "ra=%08x sp=%08x "
                    "v0=%08x v1=%08x a0=%08x a1=%08x "
                    "t0=%08x t1=%08x t2=%08x s0=%08x s1=%08x cycle=%u",
                    s_snap_n, gsvsync_count, cur_pc,
                    cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.sp.UL[0],
                    cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
                    cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
                    cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.t2.UL[0],
                    cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0],
                    cpuRegs.cycle);
                // LOAD: read const-path address (0xB0000000 known from @@EE_T0_FORCE_MEMREAD32@@)
                const u32 val_const = memRead32(0xB0000000u);
                Console.WriteLn("@@BIOS_9FC41048_LOAD@@ n=%d pc=%08x "
                    "eff_addr=B0000000 val=%08x path=const",
                    s_snap_n, cur_pc, val_const);
            }
        }
    }

	// CSR is swapped and GS vBlank IRQ is triggered roughly 3.5 hblanks after VSync Start
	if (GSSMODE1reg.SINT)
		return;

	// [P48_FIX] After GS RESET, SMODE1/SYNCV are zeroed, causing IsProgressiveVideoMode()
	// to falsely report progressive. SetField() in this state is a no-op when FIELD=1,
	// breaking the field alternation parity. Use SwapField when GS registers are
	// uninitialized (SYNCV=0 or SMODE1=0), which occurs transiently after GS RESET.
	if (IsProgressiveVideoMode()
		&& (*(u32*)PS2GS_BASE(GS_SMODE1) != 0)
		&& (*(u32*)PS2GS_BASE(GS_SYNCV) != 0))
		CSRreg.SetField();
	else
		CSRreg.SwapField();

	if (!CSRreg.VSINT)
	{
		CSRreg.VSINT = true;
		if (!GSIMR.VSMSK)
			gsIrq();
	}
}

static __fi void VSyncEnd(u32 sCycle)
{
	static int vsyncend_count = 0;

	EECNT_LOG("    ================  EE COUNTER VSYNC END (frame: %d)  ================", g_FrameCount);

	QAProbe::on_vsync_end(g_FrameCount); // [V34] QA probe: emit BL_FRAME + optional SS dump

	g_FrameCount++;
	if (!GSSMODE1reg.SINT)
	{
		hwIntcIrq(INTC_VBLANK_E); // HW Irq
		psxVBlankEnd(); // psxCounters vBlank End
		rcntEndGate(true, sCycle); // Counters End Gate Code
	}

	// This doesn't seem to be needed here.  Games only seem to break with regard to the
	// vsyncstart irq.
	//cpuRegs.eCycle[30] = 2;
}

//#define VSYNC_DEBUG		// Uncomment this to enable some vSync Timer debugging features.
#ifdef VSYNC_DEBUG
static u32 hsc = 0;
static int vblankinc = 0;
#endif

__fi void rcntUpdate_vSync()
{
	if (!cpuTestCycle(vsyncCounter.startCycle, vsyncCounter.deltaCycles))
		return;

	if (vsyncCounter.Mode == MODE_VBLANK)
	{
		vsyncCounter.startCycle += vSyncInfo.Blank;
		vsyncCounter.deltaCycles = vSyncInfo.Render;

		VSyncEnd(vsyncCounter.startCycle);

		vsyncCounter.Mode = MODE_VRENDER; // VSYNC END - Render begin
	}
	else if (vsyncCounter.Mode == MODE_GSBLANK) // GS CSR Swap and interrupt
	{
		GSVSync();

		vsyncCounter.Mode = MODE_VBLANK;
		// Don't set the start cycle, makes it easier to calculate the correct Vsync End time
		vsyncCounter.deltaCycles = vSyncInfo.Blank;
	}
	else // VSYNC Start
	{
		vsyncCounter.startCycle += vSyncInfo.Render;
		vsyncCounter.deltaCycles = vSyncInfo.GSBlank;

		VSyncStart(vsyncCounter.startCycle);

		vsyncCounter.Mode = MODE_GSBLANK;

		// Accumulate hsync rounding errors:
		hsyncCounter.deltaCycles += vSyncInfo.hSyncError;

#ifdef VSYNC_DEBUG
		vblankinc++;
		if (vblankinc > 1)
		{
			if (hsc != vSyncInfo.hScanlinesPerFrame)
				Console.WriteLn(" ** vSync > Abnormal Scanline Count: %d", hsc);
			hsc = 0;
			vblankinc = 0;
		}
#endif
	}
}

static int rcnt_upd_log_limit = 0;
__fi void rcntUpdate_hScanline()
{
	if (!cpuTestCycle(hsyncCounter.startCycle, hsyncCounter.deltaCycles))
		return;
    

	//iopEventAction = 1;
	if (hsyncCounter.Mode == MODE_HBLANK)
	{ //HBLANK End / HRENDER Begin

		// Setup the hRender's start and end cycle information:
		hsyncCounter.startCycle += vSyncInfo.hBlank; // start  (absolute cycle value)
		hsyncCounter.deltaCycles = vSyncInfo.hRender; // endpoint (delta from start value)
		if (!GSSMODE1reg.SINT)
		{
			rcntEndGate(false, hsyncCounter.startCycle);
			psxHBlankEnd();
		}

		hsyncCounter.Mode = MODE_HRENDER;
	}
	else
	{ //HBLANK START / HRENDER End
		
		hsyncCounter.startCycle += vSyncInfo.hRender; // start (absolute cycle value)
		hsyncCounter.deltaCycles = vSyncInfo.hBlank;   // endpoint (delta from start value)
        
        static int sint_log = 0;
        if (sint_log < 20 && GSSMODE1reg.SINT) {
             Console.WriteLn("DEBUG: rcntUpdate_hScanline SKIPPING rcntStartGate because SINT=1");
             sint_log++;
        }

		if (!GSSMODE1reg.SINT)
		{
			if (!CSRreg.HSINT)
			{
				CSRreg.HSINT = true;
				if (!GSIMR.HSMSK)
					gsIrq();
			}

			rcntStartGate(false, hsyncCounter.startCycle);
			psxHBlankStart();
		}

		hsyncCounter.Mode = MODE_HBLANK;

#ifdef VSYNC_DEBUG
		hsc++;
#endif
	}
}

static __fi void _cpuTestTarget(int i)
{
	if (counters[i].count < counters[i].target)
		return;

	if (counters[i].mode.TargetInterrupt)
	{
		EECNT_LOG("EE Counter[%d] TARGET reached - mode=%x, count=%x, target=%x", i, counters[i].mode, counters[i].count, counters[i].target);
		if (!counters[i].mode.TargetReached)
		{
			counters[i].mode.TargetReached = 1;
			hwIntcIrq(counters[i].interrupt);
		}
	}

	if (counters[i].mode.ZeroReturn)
		counters[i].count -= counters[i].target; // Reset on target
	else
		counters[i].target |= EECNT_FUTURE_TARGET; // OR with future target to prevent a retrigger
}

static __fi void _cpuTestOverflow(int i)
{
	if (counters[i].count <= 0xffff)
		return;

	if (counters[i].mode.OverflowInterrupt)
	{
		EECNT_LOG("EE Counter[%d] OVERFLOW - mode=%x, count=%x", i, counters[i].mode, counters[i].count);
		if (!counters[i].mode.OverflowReached)
		{
			counters[i].mode.OverflowReached = 1;
			hwIntcIrq(counters[i].interrupt);
		}
	}

	// wrap counter back around zero, and enable the future target:
	counters[i].count -= 0x10000;
	counters[i].target &= 0xffff;
}


__fi bool rcntCanCount(int i)
{
	if (!counters[i].mode.IsCounting)
		return false;

	if (!counters[i].mode.EnableGate)
		return true;

	// If we're in gate mode, we can only count if it's not both gated and counting on HBLANK or GateMode is not 0 (Count only when low) or the signal is low.
	return ((counters[i].mode.GateSource == 0 && counters[i].mode.ClockSource != 3 && (hsyncCounter.Mode == MODE_HRENDER || counters[i].mode.GateMode != 0)) ||
			(counters[i].mode.GateSource == 1 && (vsyncCounter.Mode == MODE_VRENDER || counters[i].mode.GateMode != 0)));
}

__fi void rcntSyncCounter(int i)
{
	if (counters[i].mode.ClockSource != 0x3) // don't count hblank sources
	{
		const u32 change = (cpuRegs.cycle - counters[i].startCycle) / counters[i].rate;
		counters[i].startCycle += change * counters[i].rate;

		counters[i].startCycle &= ~(counters[i].rate - 1);

		if (rcntCanCount(i))
			counters[i].count += change;
	}
	else
		counters[i].startCycle = cpuRegs.cycle;
}

// forceinline note: this method is called from two locations, but one
// of them is the interpreter, which doesn't count. ;)  So might as
// well forceinline it!
__fi void rcntUpdate()
{
	if (iPSX2_IsDebugVerbose() && rcnt_upd_log_limit < 50) {
		Console.WriteLn("DEBUG: rcntUpdate called. Cycles=%u, HStart=%u, HDelta=%u, HMode=%d", cpuRegs.cycle, hsyncCounter.startCycle, hsyncCounter.deltaCycles, hsyncCounter.Mode);
        rcnt_upd_log_limit++;
	}

	// [TEMP_DIAG] @@CYCLE_PC_SAMPLE@@ — fine-grained PC sampling
	// Phase 5: 50K interval 8M-300M to capture full BIOS boot divergence
	// Removal condition: deviceBIOSdisplayafter success
	{
		static u32 s_next_sample_cyc = 8000000;
		static int s_sample_count = 0;
		if (s_sample_count < 500 && cpuRegs.cycle >= s_next_sample_cyc && cpuRegs.cycle < 300000000) {
			u32 ev80  = eeMem ? *(u32*)(eeMem->Main + 0x80)  : 0xDEAD;
			u32 ev180 = eeMem ? *(u32*)(eeMem->Main + 0x180) : 0xDEAD;
			Console.WriteLn("@@CYCLE_PC_SAMPLE@@ n=%d cyc=%u pc=%08x ra=%08x s0=%08x v0=%08x v1=%08x a0=%08x a1=%08x status=%08x D_CTRL=%08x ev80=%08x ev180=%08x f230=%08x f220=%08x iopc=%08x",
				s_sample_count, cpuRegs.cycle, cpuRegs.pc,
				cpuRegs.GPR.r[31].UL[0],  // ra
				cpuRegs.GPR.r[16].UL[0],  // s0
				cpuRegs.GPR.r[2].UL[0],   // v0
				cpuRegs.GPR.r[3].UL[0],   // v1
				cpuRegs.GPR.r[4].UL[0],   // a0
				cpuRegs.GPR.r[5].UL[0],   // a1
				cpuRegs.CP0.r[12],        // Status
				psHu32(0xE000),           // D_CTRL
				ev80, ev180,
				psHu32(0xF230),           // sbus_f230 (polled by wait loop)
				psHu32(0xF220),           // sbus_f220
				psxRegs.pc);              // IOP PC
			s_next_sample_cyc += 500000;  // sample every 500K cycles (~600 samples over 300M)
			s_sample_count++;
		}
	}

	// [TEMP_DIAG] @@WAITLOOP_DUMP@@ — dump MIPS code at 00082160-000821A0 and 000842C0-000842E0
	// when first entering wait loop (cycle 27M-28M) to see branching logic
	// Removal condition: deviceBIOSdisplayafter success
	{
		static bool s_dumped = false;
		if (!s_dumped && cpuRegs.cycle >= 27000000 && cpuRegs.cycle < 28000000 &&
			cpuRegs.pc == 0x00082180 && eeMem)
		{
			s_dumped = true;
			// Dump wait loop code
			Console.WriteLn("@@WAITLOOP_DUMP@@ pc=00082180 area (00082140-000821C0):");
			for (u32 addr = 0x00082140; addr < 0x000821C0; addr += 4)
			{
				u32 inst = *(u32*)(eeMem->Main + addr);
				Console.WriteLn("  %08x: %08x", addr, inst);
			}
			// Dump sim destination (00082680-000826B0)
			Console.WriteLn("@@WAITLOOP_DUMP@@ sim-dest area (00082660-000826B0):");
			for (u32 addr = 0x00082660; addr < 0x000826B0; addr += 4)
			{
				u32 inst = *(u32*)(eeMem->Main + addr);
				Console.WriteLn("  %08x: %08x", addr, inst);
			}
			// Dump dev destination (000842C0-000842F0)
			Console.WriteLn("@@WAITLOOP_DUMP@@ dev-dest area (000842B0-00084300):");
			for (u32 addr = 0x000842B0; addr < 0x00084300; addr += 4)
			{
				u32 inst = *(u32*)(eeMem->Main + addr);
				Console.WriteLn("  %08x: %08x", addr, inst);
			}
			// Dump thread control block / semaphore area
			// s0=00093580 on sim, v1=1000f230 on both — dump these areas
			Console.WriteLn("@@WAITLOOP_DUMP@@ TCB area (00093560-000935A0):");
			for (u32 addr = 0x00093560; addr < 0x000935A0; addr += 4)
			{
				u32 inst = *(u32*)(eeMem->Main + addr);
				Console.WriteLn("  %08x: %08x", addr, inst);
			}
		}
	}

	// [R61] @@GS_MEM_WATCHPOINT@@ — high-frequency g_RealGSMem corruption detection
	// Only active between cycle 2.4B-2.7B (around vsync 490-550, covering corruption window)
	// Removal condition: GS register破損のroot causeafter determined
	{
		static bool s_gsmw_triggered = false;
		if (!s_gsmw_triggered && cpuRegs.cycle > 2400000000u && cpuRegs.cycle < 2700000000u) {
			const u64 smode2 = *(const u64*)(g_RealGSMem + 0x20);
			if (smode2 != 0 && (u32)(smode2 >> 32) == (u32)(smode2)) {
				s_gsmw_triggered = true;
				Console.WriteLn("@@GS_MEM_WATCHPOINT@@ CAUGHT! cycle=%u ee_pc=%08x ra=%08x smode2=%016llx",
					cpuRegs.cycle, cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], (unsigned long long)smode2);
				// Dump EE state at corruption time
				Console.WriteLn("@@GS_MEM_WATCHPOINT@@ sp=%08x s0=%08x s1=%08x s2=%08x gp=%08x",
					cpuRegs.GPR.n.sp.UL[0], cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0],
					cpuRegs.GPR.n.s2.UL[0], cpuRegs.GPR.n.gp.UL[0]);
				// Dump nearby memory to find source of corruption data
				if (eeMem) {
					// Check if EE memory at 0x218000 area matches corruption data
					for (u32 base = 0x217F80; base < 0x218140; base += 0x20) {
						const u32* p = reinterpret_cast<const u32*>(eeMem->Main + base);
						Console.WriteLn("@@GS_MEM_WATCHPOINT_EEMEM@@ [0x%06x]: %08x %08x %08x %08x %08x %08x %08x %08x",
							base, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
					}
				}
				// Dump the g_RealGSMem corruption pattern
				for (u32 off = 0; off < 0x100; off += 0x10) {
					const u32* p = (const u32*)(g_RealGSMem + off);
					Console.WriteLn("@@GS_MEM_WATCHPOINT_DUMP@@ [+%03x]: %08x %08x %08x %08x",
						off, p[0], p[1], p[2], p[3]);
				}
			}
		}
	}

	// [iter681] @@KERN_CODE_DUMP@@ — one-shot dump of 0x800065A0-0x800065F0 to verify sceSifSetDma code
	{
		static bool s_kcd_done = false;
		if (!s_kcd_done && eeMem && cpuRegs.cycle > 50000000u) {
			s_kcd_done = true;
			u32* p = (u32*)(eeMem->Main + 0x65A0);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800065A0: %08x %08x %08x %08x %08x %08x %08x %08x",
				p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800065C0: %08x %08x %08x %08x %08x %08x %08x %08x",
				p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800065E0: %08x %08x %08x %08x %08x %08x %08x %08x",
				p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23]);
			// Also dump the SYSCALL dispatch table entry for SYSCALL 0x77
			// BIOS dispatch table is typically at 0x800008xx or similar
			// Dump the code at 0x80000180 (exception handler) area
			u32* exc = (u32*)(eeMem->Main + 0x180);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80000180: %08x %08x %08x %08x %08x %08x %08x %08x",
				exc[0], exc[1], exc[2], exc[3], exc[4], exc[5], exc[6], exc[7]);
			// Dump 0x80006668-0x80006798 (capacity check function called by sceSifSetDma)
			u32* cap = (u32*)(eeMem->Main + 0x6668);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006668: %08x %08x %08x %08x %08x %08x %08x %08x",
				cap[0], cap[1], cap[2], cap[3], cap[4], cap[5], cap[6], cap[7]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006688: %08x %08x %08x %08x %08x %08x %08x %08x",
				cap[8], cap[9], cap[10], cap[11], cap[12], cap[13], cap[14], cap[15]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800066A8: %08x %08x %08x %08x %08x %08x %08x %08x",
				cap[16], cap[17], cap[18], cap[19], cap[20], cap[21], cap[22], cap[23]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800066C8: %08x %08x %08x %08x %08x %08x %08x %08x",
				cap[24], cap[25], cap[26], cap[27], cap[28], cap[29], cap[30], cap[31]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800066E8: %08x %08x %08x %08x %08x %08x %08x %08x",
				cap[32], cap[33], cap[34], cap[35], cap[36], cap[37], cap[38], cap[39]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006708: %08x %08x %08x %08x %08x %08x %08x %08x",
				cap[40], cap[41], cap[42], cap[43], cap[44], cap[45], cap[46], cap[47]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006728: %08x %08x %08x %08x %08x %08x %08x %08x",
				cap[48], cap[49], cap[50], cap[51], cap[52], cap[53], cap[54], cap[55]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006748: %08x %08x %08x %08x %08x %08x %08x %08x",
				cap[56], cap[57], cap[58], cap[59], cap[60], cap[61], cap[62], cap[63]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006768: %08x %08x %08x %08x %08x %08x %08x %08x",
				cap[64], cap[65], cap[66], cap[67], cap[68], cap[69], cap[70], cap[71]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006788: %08x %08x %08x %08x",
				cap[72], cap[73], cap[74], cap[75]);
			// Dump 0x80006798-0x80006880 (sceSifSetDma entry + pre-loop code)
			u32* pre = (u32*)(eeMem->Main + 0x6798);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006798: %08x %08x %08x %08x %08x %08x %08x %08x",
				pre[0], pre[1], pre[2], pre[3], pre[4], pre[5], pre[6], pre[7]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800067B8: %08x %08x %08x %08x %08x %08x %08x %08x",
				pre[8], pre[9], pre[10], pre[11], pre[12], pre[13], pre[14], pre[15]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800067D8: %08x %08x %08x %08x %08x %08x %08x %08x",
				pre[16], pre[17], pre[18], pre[19], pre[20], pre[21], pre[22], pre[23]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800067F8: %08x %08x %08x %08x %08x %08x %08x %08x",
				pre[24], pre[25], pre[26], pre[27], pre[28], pre[29], pre[30], pre[31]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006818: %08x %08x %08x %08x %08x %08x %08x %08x",
				pre[32], pre[33], pre[34], pre[35], pre[36], pre[37], pre[38], pre[39]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006838: %08x %08x %08x %08x %08x %08x %08x %08x",
				pre[40], pre[41], pre[42], pre[43], pre[44], pre[45], pre[46], pre[47]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006858: %08x %08x %08x %08x %08x %08x %08x %08x",
				pre[48], pre[49], pre[50], pre[51], pre[52], pre[53], pre[54], pre[55]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006878: %08x %08x %08x %08x %08x %08x %08x %08x",
				pre[56], pre[57], pre[58], pre[59], pre[60], pre[61], pre[62], pre[63]);
			// Dump 0x80006880-0x80006920 (call site to 0x800065AC)
			u32* cs = (u32*)(eeMem->Main + 0x6880);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006880: %08x %08x %08x %08x %08x %08x %08x %08x",
				cs[0], cs[1], cs[2], cs[3], cs[4], cs[5], cs[6], cs[7]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800068A0: %08x %08x %08x %08x %08x %08x %08x %08x",
				cs[8], cs[9], cs[10], cs[11], cs[12], cs[13], cs[14], cs[15]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800068C0: %08x %08x %08x %08x %08x %08x %08x %08x",
				cs[16], cs[17], cs[18], cs[19], cs[20], cs[21], cs[22], cs[23]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800068E0: %08x %08x %08x %08x %08x %08x %08x %08x",
				cs[24], cs[25], cs[26], cs[27], cs[28], cs[29], cs[30], cs[31]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006900: %08x %08x %08x %08x %08x %08x %08x %08x",
				cs[32], cs[33], cs[34], cs[35], cs[36], cs[37], cs[38], cs[39]);
			// Also dump 0x80006480-0x80006580 (before store function)
			u32* sf = (u32*)(eeMem->Main + 0x6480);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x80006480: %08x %08x %08x %08x %08x %08x %08x %08x",
				sf[0], sf[1], sf[2], sf[3], sf[4], sf[5], sf[6], sf[7]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800064A0: %08x %08x %08x %08x %08x %08x %08x %08x",
				sf[8], sf[9], sf[10], sf[11], sf[12], sf[13], sf[14], sf[15]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800064C0: %08x %08x %08x %08x %08x %08x %08x %08x",
				sf[16], sf[17], sf[18], sf[19], sf[20], sf[21], sf[22], sf[23]);
			Console.WriteLn("@@KERN_CODE_DUMP@@ 0x800064E0: %08x %08x %08x %08x %08x %08x %08x %08x",
				sf[24], sf[25], sf[26], sf[27], sf[28], sf[29], sf[30], sf[31]);
		}
	}

	// [TEMP_DIAG] @@TAG_WATCH@@ — monitor DMA tag memory at 0x21380
	// Removal condition: SIF DMA tag 書き込みissue解決後
	if (eeMem && 0x213A0 <= Ps2MemSize::MainRam) {
		static u32 s_tag_prev = 0;
		static int s_tag_watch_n = 0;
		u32 tag_val = *(u32*)(eeMem->Main + 0x21380);
		if (tag_val != s_tag_prev && s_tag_watch_n < 50) {
			Console.WriteLn("@@TAG_WATCH@@ n=%d old=%08x new=%08x ee_pc=%08x ee_cyc=%u tag=[%08x %08x %08x %08x]",
				s_tag_watch_n++, s_tag_prev, tag_val, cpuRegs.pc, cpuRegs.cycle,
				*(u32*)(eeMem->Main + 0x21380), *(u32*)(eeMem->Main + 0x21384),
				*(u32*)(eeMem->Main + 0x21388), *(u32*)(eeMem->Main + 0x2138C));
			s_tag_prev = tag_val;
		}
	}

	// [TEMP_DIAG] @@WALLCLOCK_PC@@ — wall-clock-based EE PC sample every 2 seconds
	{
		static uint64_t s_last_wall_us = 0;
		struct timeval tv;
		gettimeofday(&tv, nullptr);
		uint64_t now_us = (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
		if (s_last_wall_us == 0) s_last_wall_us = now_us;
		if (now_us - s_last_wall_us >= 2000000ULL) {
			s_last_wall_us = now_us;
			// [TEMP_DIAG] Also dump iop_pc, iop_cycle, IOP INTC, and SIF status word at 0x937c0
			u32 sif_stat_word = (eeMem && 0x937c0 < Ps2MemSize::MainRam) ? *(u32*)(eeMem->Main + 0x937c0) : 0xDEADDEADu;
			u32 iop_i_stat = psxHu32(0x1070); // IOP INTC status
			u32 iop_i_mask = psxHu32(0x1074); // IOP INTC mask
			u32 iop_cop0_sr = psxRegs.CP0.n.Status; // IOP COP0 Status
			Console.WriteLn("@@WALLCLOCK_PC@@ ee_pc=%08x iop_pc=%08x ee_cyc=%u iop_cyc=%u frame=%u sif_stat=%08x iop_ISTAT=%08x iop_IMASK=%08x iop_SR=%08x",
				cpuRegs.pc, psxRegs.pc, cpuRegs.cycle, psxRegs.cycle, g_FrameCount, sif_stat_word,
				iop_i_stat, iop_i_mask, iop_cop0_sr);
		}
	}

	rcntUpdate_vSync();
	// HBlank after as VSync can do error compensation
	rcntUpdate_hScanline();

	// Update counters so that we can perform overflow and target tests.

	for (int i = 0; i <= 3; i++)
	{
		rcntSyncCounter(i);

		if (counters[i].mode.ClockSource == 0x3 || !rcntCanCount(i)) // don't count hblank sources
				continue;

		_cpuTestOverflow(i);
		_cpuTestTarget(i);
	}

	cpuRcntSet();
}

static __fi void _rcntSetGate(int index)
{
	if (counters[index].mode.EnableGate)
	{
		// If the Gate Source is hblank and the clock selection is also hblank
		// the timer completely turns off (HW Tested).
		if (!(counters[index].mode.GateSource == 0 && counters[index].mode.ClockSource == 3))
			EECNT_LOG("EE Counter[%d] Using Gate!  Source=%s, Mode=%d.",
				index, counters[index].mode.GateSource ? "vblank" : "hblank", counters[index].mode.GateMode);
		else
			EECNT_LOG("EE Counter[%d] GATE DISABLED because of hblank source.", index);
	}
}

static __fi void rcntStartGate(bool isVblank, u32 sCycle)
{
    // static int gate_log = 0;
    // if (gate_log < 50) {
    //    Console.WriteLn("DEBUG: rcntStartGate Entry. isVblank=%d C0_Src=%d C0_Cnt=%d C0_En=%d", 
    //        isVblank, counters[0].mode.ClockSource, counters[0].count, counters[0].mode.IsCounting);
    //    gate_log++;
    // }

	for (int i = 0; i < 4; i++)
	{
		if (!isVblank && (counters[i].mode.ClockSource == 3))
		{
			bool canCount = rcntCanCount(i);
			if (i == 0) {
                // Log only if Src=3 (HBLANK)
                static int t0_log = 0;
                if (iPSX2_IsDebugVerbose() && t0_log < 100) {
					Console.WriteLn("DEBUG: rcntStartGate T0 Incr! Count=%d->%d. isVblank=%d",
                        counters[i].count, counters[i].count + HBLANK_COUNTER_SPEED, isVblank);
                    t0_log++;
                }
			}

			if (canCount)
			{
				counters[i].count += HBLANK_COUNTER_SPEED;
				// [iter403] JIT const-path reads psHu16(RCNT0_COUNT) directly (not via handler)
				// Must keep psHu in sync so 0x10000000 reads return updated T0 value
				if (i == 0) psHu16(RCNT0_COUNT) = (u16)counters[0].count;
				_cpuTestOverflow(i);
				_cpuTestTarget(i);
			}
		}

		if (!counters[i].mode.EnableGate)
			continue;

		if ((!!counters[i].mode.GateSource) != isVblank)
			continue;

		switch (counters[i].mode.GateMode)
		{
			case 0x0:  //Count When Signal is low (V_RENDER ONLY)

				// Just set the start cycle -- counting will be done as needed
				// for events (overflows, targets, mode changes, and the gate off below)
				rcntSyncCounter(i);
				counters[i].startCycle = sCycle & ~(counters[i].rate - 1);
				EECNT_LOG("EE Counter[%d] %s StartGate Type0, count = %x", i,
					isVblank ? "vblank" : "hblank", counters[i].count);
				break;
			case 0x2: // Reset on Vsync end
				// This is the vsync start so do nothing.
				break;
			case 0x1: // Reset on Vsync start
			case 0x3: // Reset on Vsync start and end
				rcntSyncCounter(i);
				counters[i].count = 0;
				counters[i].target &= 0xffff;
				counters[i].startCycle = sCycle & ~(counters[i].rate - 1);
				EECNT_LOG("EE Counter[%d] %s StartGate Type%d, count = %x", i,
					isVblank ? "vblank" : "hblank", counters[i].mode.GateMode, counters[i].count);
				break;
		}
	}

	// No need to update actual counts here.  Counts are calculated as needed by reads to
	// rcntRcount().  And so long as sCycleT is set properly, any targets or overflows
	// will be scheduled and handled.

	// Note: No need to set counters here.  They'll get set when control returns to
	// rcntUpdate, since we're being called from there anyway.
}

// mode - 0 means hblank signal, 8 means vblank signal.
static __fi void rcntEndGate(bool isVblank, u32 sCycle)
{
	for (int i = 0; i < 4; i++)
	{
		if (!counters[i].mode.EnableGate)
			continue;

		if ((!!counters[i].mode.GateSource) != isVblank)
			continue;

		switch (counters[i].mode.GateMode)
		{
			case 0x0: //Count When Signal is low (V_RENDER ONLY)
				counters[i].startCycle = sCycle & ~(counters[i].rate - 1);

				EECNT_LOG("EE Counter[%d] %s EndGate Type0, count = %x", i,
					isVblank ? "vblank" : "hblank", counters[i].count);
				break;

			case 0x1: // Reset on Vsync start
				// This is the vsync end so do nothing
				break;

			case 0x2: // Reset on Vsync end
			case 0x3: // Reset on Vsync start and end
				rcntSyncCounter(i);
				EECNT_LOG("EE Counter[%d]  %s EndGate Type%d, count = %x", i,
					isVblank ? "vblank" : "hblank", counters[i].mode.GateMode, counters[i].count);
				counters[i].count = 0;
				counters[i].target &= 0xffff;
				counters[i].startCycle = sCycle & ~(counters[i].rate - 1);
				break;
		}
	}
	// Note: No need to set counters here.  They'll get set when control returns to
	// rcntUpdate, since we're being called from there anyway.
}

static __fi void rcntWmode(int index, u32 value)
{
	rcntSyncCounter(index);

	// Clear OverflowReached and TargetReached flags (0xc00 mask), but *only* if they are set to 1 in the
	// given value.  (yes, the bits are cleared when written with '1's).

	counters[index].modeval &= ~(value & 0xc00);
	counters[index].modeval = (counters[index].modeval & 0xc00) | (value & 0x3ff);
	EECNT_LOG("EE Counter[%d] writeMode = %x passed value=%x", index, counters[index].modeval, value);
    if (index == 0 && iPSX2_IsDebugVerbose()) {
        Console.WriteLn("DEBUG: rcntWmode(0) Called. Val=%x. OldMode=%x. NewMode=%x. Src=%d",
            value, counters[index].modeval, (counters[index].modeval & 0xc00) | (value & 0x3ff), (value & 3));
    }

	switch (counters[index].mode.ClockSource) { //Clock rate divisers *2, they use BUSCLK speed not PS2CLK
		case 0: counters[index].rate = 2; break;
		case 1: counters[index].rate = 32; break;
		case 2: counters[index].rate = 512; break;
		case 3: counters[index].rate = vSyncInfo.hBlank+vSyncInfo.hRender; break;
	}

	// In case the rate has changed we need to set the start cycle to the previous tick.
	counters[index].startCycle = cpuRegs.cycle & ~(counters[index].rate - 1);
	_rcntSetGate(index);
	_rcntSet(index);
}

static __fi void rcntWcount(int index, u32 value)
{
	EECNT_LOG("EE Counter[%d] writeCount = %x,   oldcount=%x, target=%x", index, value, counters[index].count, counters[index].target);

	// re-calculate the start cycle of the counter based on elapsed time since the last counter update:
	rcntSyncCounter(index);

	counters[index].count = value & 0xffff;

	// reset the target, and make sure we don't get a premature target.
	counters[index].target &= 0xffff;

	if (counters[index].count >= counters[index].target)
		counters[index].target |= EECNT_FUTURE_TARGET;

	_rcntSet(index);
}

static __fi void rcntWtarget(int index, u32 value)
{
	EECNT_LOG("EE Counter[%d] writeTarget = %x", index, value);

	counters[index].target = value & 0xffff;

	// guard against premature (instant) targeting.
	// If the target is behind the current count, set it up so that the counter must
	// overflow first before the target fires:

	rcntSyncCounter(index);

	if (counters[index].target <= counters[index].count)
		counters[index].target |= EECNT_FUTURE_TARGET;

	_rcntSet(index);
}

static __fi void rcntWhold(int index, u32 value)
{
	EECNT_LOG("EE Counter[%d] Hold Write = %x", index, value);
	counters[index].hold = value;
}

__fi u32 rcntRcount(int index)
{
	u32 ret;

	rcntSyncCounter(index);
	
	ret = counters[index].count;

	// [TEMP_DIAG][REMOVE_AFTER=EE_9FC41048_T0_WAIT_ROOTCAUSE_V1]
	// 目的: EE BIOS wait-loop(9FC41040/48 の連続T0 read)で、読値変化と cycle 前進の有無を同一runで証拠化。
	// needed時のみ同じ限定conditionで返値を交互化し、EEloop脱出/BIOS前進の因果を切り分ける。
	if (index == 0)
	{
		if (!s_ee41048_t0_cfg_init)
		{
			s_ee41048_t0_probe_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_EE41048_KSEG1_PROBE", false) ? 1 : 0;
			s_ee41048_t0_toggle_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_EE41048_KSEG1_TOGGLE", false) ? 1 : 0;
			Console.WriteLn("@@CFG@@ iPSX2_EE41048_KSEG1_PROBE=%d iPSX2_EE41048_KSEG1_TOGGLE=%d", s_ee41048_t0_probe_enabled, s_ee41048_t0_toggle_enabled);
			s_ee41048_t0_cfg_init = 1;
		}

		const bool ee_wait_read = (cpuRegs.pc == 0x9FC41040u || cpuRegs.pc == 0x9FC41048u);
		if (ee_wait_read)
		{
			u16 out16 = static_cast<u16>(ret);
			if (s_ee41048_t0_toggle_enabled)
				out16 = static_cast<u16>(s_ee41048_t0_toggle_n++ & 1u);

			if ((s_ee41048_t0_probe_enabled || s_ee41048_t0_toggle_enabled) && s_ee41048_t0_probe_n < 64)
			{
				Console.WriteLn("@@EE41048_T0@@ n=%u pc=%08x cycle=%u raw=%04x out=%04x dcycle=%u count=%u isCounting=%d mode=%s",
					s_ee41048_t0_probe_n, cpuRegs.pc, cpuRegs.cycle, static_cast<u16>(ret), out16,
					cpuRegs.cycle - s_ee41048_t0_prev_cycle, counters[0].count, counters[0].mode.IsCounting ? 1 : 0,
					s_ee41048_t0_toggle_enabled ? "toggle" : "probe");
				s_ee41048_t0_probe_n++;
			}
			s_ee41048_t0_prev_cycle = cpuRegs.cycle;
			s_ee41048_t0_prev_out = out16;
			ret = out16;
		}
	}
	
	if (index == 0 && iPSX2_IsDebugVerbose()) {
        static u32 prev_ret = 0;
        static int log_limit = 0;
        if (ret != prev_ret && log_limit < 50) {
             Console.WriteLn("DEBUG: rcntRcount(0) returning %04x (Cycle diff)", (u16)ret);
             log_limit++;
             prev_ret = ret;
        }
    }

    // [TEMP_DIAG] T0_COUNT double-read probe: file-scope statics (iter05, no __fi isolation)
    // Removal condition: @@T0_READ@@ 20件after confirmed、T0 state確定次第delete
    if (index == 0) {
        if (s_t0d_n < 20) {
            u32 dcycle = cpuRegs.cycle - s_t0d_prev_cycle;
            Console.WriteLn("[TEMP_DIAG] @@T0_READ@@ n=%d pc=%08x cycle=%u ret=%u "
                "isCounting=%d count=%u prev_ret=%u dcycle=%u",
                s_t0d_n, cpuRegs.pc, cpuRegs.cycle, (unsigned)(u16)ret,
                (int)counters[0].mode.IsCounting, counters[0].count,
                (unsigned)s_t0d_prev_ret, dcycle);
            s_t0d_prev_cycle = cpuRegs.cycle;
            s_t0d_prev_ret   = (u16)ret;
            s_t0d_n++;
        }
    }

	// [iter436] PS1DRV rangeから rcntRcount(0) が呼ばれているかverify
	// 呼ばれていない → JIT fastmem が psHu16 を直接読んでいる (停滞値確定)
	// 呼ばれている  → vtlb handler 経由 (rcntSyncCounter call済み)
	// Removal condition: fastmem/slow-path どちらかafter determined
	if (index == 0 && cpuRegs.pc >= 0x200000u && cpuRegs.pc < 0x220000u)
	{
		static u32 s_ps1drv_rcnt_calls = 0;
		static bool s_ps1drv_rcnt_reported = false;
		s_ps1drv_rcnt_calls++;
		if (!s_ps1drv_rcnt_reported && s_ps1drv_rcnt_calls >= 1000u) {
			s_ps1drv_rcnt_reported = true;
			Console.WriteLn("@@PS1DRV_RCNT_CALLS@@ rcntRcount(0) called %u times from PS1DRV (pc range 200000-220000) ret=%04x",
				s_ps1drv_rcnt_calls, (u16)ret);
		}
	}
	// [iter448] spad[0xA0] ≠ -1 初回detect: PS1DRV が GP0 dispatch パスにある証拠
	// rcntRcount(0) はT0 read 毎に呼ばれる → vsyncboundaryより高頻度にサンプリング可能
	// spad[0xA0]=-1 = 「次GP0コマンド待ち」state (0x2058ACでconfig)
	// spad[0xA0]=cmd_byte = 受信中 (0x2056D8でconfig、0x202D74等 dispatch 後に-1へ戻る)
	// Removal condition: dispatch verify (spad[0xA0] に cmd_byte detect) 後
	if (index == 0 && cpuRegs.pc >= 0x200000u && cpuRegs.pc < 0x220000u)
	{
		static bool s_dispatch_seen = false;
		if (!s_dispatch_seen) {
			const u32 a0_val = memRead32(0x700000a0u);
			if (a0_val != 0xffffffffu) {
				s_dispatch_seen = true;
				Console.WriteLn("@@PS1DRV_DISPATCH_SEEN@@ cycle=%u pc=%08x spad[a0]=%08x spad[a4]=%08x spad[18]=%08x",
					cpuRegs.cycle, cpuRegs.pc, a0_val,
					memRead32(0x700000a4u), memRead32(0x70000018u));
			}
		}
	}
	// [iter676e] PS1DRV T0 ハック撤去 — BIOS 自然進行にnot needed。
	return (u16)ret;
}

template <uint page>
__fi u16 rcntRead32(u32 mem)
{
	// Important DevNote:
	// Yes this uses a u16 return value on purpose!  The upper bits 16 of the counter registers
	// are all fixed to 0, so we always truncate everything in these two pages using a u16
	// return value! --air

	switch( mem )
	{
		case(RCNT0_COUNT):	return (u16)rcntRcount(0);
		case(RCNT0_MODE):	return (u16)counters[0].modeval;
		case(RCNT0_TARGET):	return (u16)counters[0].target;
		case(RCNT0_HOLD):	return (u16)counters[0].hold;

		case(RCNT1_COUNT):	return (u16)rcntRcount(1);
		case(RCNT1_MODE):	return (u16)counters[1].modeval;
		case(RCNT1_TARGET):	return (u16)counters[1].target;
		case(RCNT1_HOLD):	return (u16)counters[1].hold;

		case(RCNT2_COUNT):	return (u16)rcntRcount(2);
		case(RCNT2_MODE):	return (u16)counters[2].modeval;
		case(RCNT2_TARGET):	return (u16)counters[2].target;

		case(RCNT3_COUNT):	return (u16)rcntRcount(3);
		case(RCNT3_MODE):	return (u16)counters[3].modeval;
		case(RCNT3_TARGET):	return (u16)counters[3].target;
	}

	return psHu16(mem);
}

template <uint page>
__fi bool rcntWrite32(u32 mem, mem32_t& value)
{
	pxAssume(mem >= RCNT0_COUNT && mem < 0x10002000);

	// [TODO] : counters should actually just use the EE's hw register space for storing
	// count, mode, target, and hold. This will allow for a simplified handler for register
	// reads.

	switch( mem )
	{
		case(RCNT0_COUNT):	return rcntWcount(0, value),	false;
		case(RCNT0_MODE):	return rcntWmode(0, value),		false;
		case(RCNT0_TARGET):	return rcntWtarget(0, value),	false;
		case(RCNT0_HOLD):	return rcntWhold(0, value),		false;

		case(RCNT1_COUNT):	return rcntWcount(1, value),	false;
		case(RCNT1_MODE):	return rcntWmode(1, value),		false;
		case(RCNT1_TARGET):	return rcntWtarget(1, value),	false;
		case(RCNT1_HOLD):	return rcntWhold(1, value),		false;

		case(RCNT2_COUNT):	return rcntWcount(2, value),	false;
		case(RCNT2_MODE):	return rcntWmode(2, value),		false;
		case(RCNT2_TARGET):	return rcntWtarget(2, value),	false;

		case(RCNT3_COUNT):	return rcntWcount(3, value),	false;
		case(RCNT3_MODE):	return rcntWmode(3, value),		false;
		case(RCNT3_TARGET):	return rcntWtarget(3, value),	false;
	}

	// unhandled .. do memory writeback.
	return true;
}

template u16 rcntRead32<0x00>(u32 mem);
template u16 rcntRead32<0x01>(u32 mem);

template bool rcntWrite32<0x00>(u32 mem, mem32_t& value);
template bool rcntWrite32<0x01>(u32 mem, mem32_t& value);

bool SaveStateBase::rcntFreeze()
{
	Freeze(counters);
	Freeze(hsyncCounter);
	Freeze(vsyncCounter);
	Freeze(nextDeltaCounter);
	Freeze(nextStartCounter);
	Freeze(vSyncInfo);
	Freeze(gsVideoMode);
	Freeze(gsIsInterlaced);

	if (IsLoading())
		cpuRcntSet();

	return IsOkay();
}

extern "C" void vtlb_LogHandlerAddr(u64 ptr, u64 id)
{
    Console.WriteLn("DEBUG: Dispatching to Handler: Ptr=%llx, ID=%llx", ptr, id);
}

extern "C" void vtlb_LogNullHandler()
{
    Console.WriteLn("DEBUG: CRITICAL: Attempted to call NULL Handler! TRAP!");
    // Force crash or loop
    // int* p = 0; *p = 1;
}

// [iter220] TEMP_DIAG: LW result after handler return
extern "C" void armsx2_probe_lw_result_9fc433f0(u32 result, u32 slot)
{
	static u32 s_cnt = 0;
	if (s_cnt < 10) {
		Console.WriteLn("@@LW_RESULT_9FC433F0@@ n=%u result=%08x slot=%u pc=%08x cycle=%u",
			s_cnt, result, slot, cpuRegs.pc, cpuRegs.cycle);
		s_cnt++;
	}
}

// [iter220] TEMP_DIAG: BNE 9FC43404 runtime v0 value probe
extern "C" void armsx2_probe_bne_9fc43404(u64 v0_val, u32 cycle)
{
	static u32 s_cnt = 0;
	if (s_cnt < 20) {
		Console.WriteLn("@@BNE_9FC43404@@ n=%u v0=%08x_%08x cycle=%u pc=%08x",
			s_cnt, (u32)(v0_val >> 32), (u32)v0_val, cycle, cpuRegs.pc);
		s_cnt++;
	}
}


// [TEMP_DIAG][REMOVE_AFTER=TBIN_REQ_ALIAS_V1] Enable TBIN request alias override
#define iPSX2_ENABLE_TBIN_REQ_ALIAS 0

extern "C" u32 recMemRead32_KSEG1(u32 addr)
{
    // Console.WriteLn("DEBUG: recMemRead32_KSEG1 CALLED addr=%08x", addr); // disabled for perf
	constexpr u32 kKseg1Rd32ProbeCap = 50;

	static int s_cfg_init = 0;
	static int s_tbin_req_alias_enabled = 0;
	static int s_lf_precheck_probe_enabled = 0;
	static int s_kseg1_rd32_log_enabled = 0;
	static int s_kseg1_rd32_probe_enabled = 0;
	static int s_kseg1_probe_enabled = 0;
	static int s_ee41048_loop_probe_enabled = 0;
	static int s_ee41048_loop_toggle_enabled = 0;
	static u32 s_kseg1_rd32_probe_count = 0;
	if (!s_cfg_init)
	{
		const bool safe_only = iPSX2_IsSafeOnlyEnabled();
		const bool diag_enabled = (!safe_only && iPSX2_GetRuntimeEnvBool("iPSX2_ENABLE_DIAG_FLAGS", false));
		s_tbin_req_alias_enabled = (diag_enabled && iPSX2_GetRuntimeEnvBool("iPSX2_ENABLE_TBIN_REQ_ALIAS", iPSX2_ENABLE_TBIN_REQ_ALIAS != 0)) ? 1 : 0;
		s_lf_precheck_probe_enabled = (diag_enabled && iPSX2_GetRuntimeEnvBool("iPSX2_LF_PRECHECK_PROBE", false)) ? 1 : 0;
		s_kseg1_rd32_log_enabled = (diag_enabled && iPSX2_GetRuntimeEnvBool("iPSX2_KSEG1_RD32_LOG", false)) ? 1 : 0;
		s_kseg1_rd32_probe_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_KSEG1_RD32_PROBE", false) ? 1 : 0;
		s_kseg1_probe_enabled = (diag_enabled && iPSX2_GetRuntimeEnvBool("iPSX2_KSEG1_PROBE", false)) ? 1 : 0;
		// [TEMP_DIAG][REMOVE_AFTER=EE_9FC41048_T0_WAIT_ROOTCAUSE_V1]
		// SAFE_ONLYでも使える最小probe/一時toggle。目的: EE BIOS待ちloop(9FC41040/48)のKSEG1読み値変化と脱出可否を同一runでverify。
		s_ee41048_loop_probe_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_EE41048_KSEG1_PROBE", false) ? 1 : 0;
		s_ee41048_loop_toggle_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_EE41048_KSEG1_TOGGLE", false) ? 1 : 0;
		Console.WriteLn(
			"@@CFG@@ iPSX2_SAFE_ONLY=%d iPSX2_ENABLE_TBIN_REQ_ALIAS=%d iPSX2_LF_PRECHECK_PROBE=%d iPSX2_KSEG1_RD32_LOG=%d iPSX2_KSEG1_PROBE=%d iPSX2_EE41048_KSEG1_PROBE=%d iPSX2_EE41048_KSEG1_TOGGLE=%d",
			safe_only ? 1 : 0, s_tbin_req_alias_enabled, s_lf_precheck_probe_enabled, s_kseg1_rd32_log_enabled, s_kseg1_probe_enabled,
			s_ee41048_loop_probe_enabled, s_ee41048_loop_toggle_enabled);
		if (s_kseg1_rd32_probe_enabled)
		{
			Console.WriteLn("@@CFG@@ iPSX2_KSEG1_RD32_PROBE=1 iPSX2_KSEG1_RD32_PROBE_CAP=%u", kKseg1Rd32ProbeCap);
		}
		s_cfg_init = 1;
	}

	const auto emit_kseg1_rd32_probe = [&](u32 retv) {
		if (!s_kseg1_rd32_probe_enabled || s_kseg1_rd32_probe_count >= kKseg1Rd32ProbeCap)
			return;
		Console.WriteLn("@@KSEG1_RD32_PROBE@@ idx=%u pc=%08x addr=%08x ret=%08x",
			s_kseg1_rd32_probe_count, cpuRegs.pc, addr, retv);
		s_kseg1_rd32_probe_count++;
	};

	if (s_tbin_req_alias_enabled)
	{
    // [TEMP_DIAG][REMOVE_AFTER=TBIN_REQ_ALIAS_V2] TBIN Request Alias Override with PC Check
    // When BIOS LoadFile compare reads from TBIN request struct (a2=0xBFC02488),
    // return data from real TBIN ROMDIR entry (t0=0xBFC02A60) so comparison passes.
    // Condition: addr in [0xBFC02488, 0xBFC02498) AND pc in LoadFile compare window.
    constexpr u32 kReqBase = 0xBFC02488;
    constexpr u32 kEntryBase = 0xBFC02A60;
    constexpr u32 kPcLo = 0xBFC026B0;  // Start of LoadFile inner compare loop
    constexpr u32 kPcHi = 0xBFC026F0;  // End of compare (before loop increment)
    
    u32 guest_pc = cpuRegs.pc;
    if (addr >= kReqBase && addr < kReqBase + 0x10 &&
        guest_pc >= kPcLo && guest_pc < kPcHi) {
        static int s_alias_hits = 0;
        if (s_alias_hits < 8) {
            Console.WriteLn("@@TBIN_REQ_ALIAS_HIT@@ pc=%08x addr=%08x -> %08x", guest_pc, addr, kEntryBase + (addr - kReqBase));
            s_alias_hits++;
        }
        u32 alias_addr = kEntryBase + (addr - kReqBase);
        u32 val = memRead32(alias_addr);
        emit_kseg1_rd32_probe(val);
        return val;
    }
	}

    // [TEMP_DIAG][REMOVE_AFTER=LF_PRECHECK_PROBE_V1] LoadFile Pre-check Runtime Probe
    // When cpuRegs.pc is in pre-check window, log t0/t1/t2/t4/a2 and size alignment
	if (s_lf_precheck_probe_enabled)
	{
        u32 guest_pc = cpuRegs.pc;
        if (guest_pc >= 0xBFC02690 && guest_pc < 0xBFC026B0) {
            static int s_precheck_hits = 0;
            if (s_precheck_hits < 16) {
                // Get register values directly from cpuRegs
                u32 t0_val = cpuRegs.GPR.n.t0.UL[0];
                u32 t1_val = cpuRegs.GPR.n.t1.UL[0];
                u32 t2_val = cpuRegs.GPR.n.t2.UL[0];
                u32 t4_val = cpuRegs.GPR.n.t4.UL[0];
                u32 a2_val = cpuRegs.GPR.n.a2.UL[0];
                
                Console.WriteLn("@@LF_PRECHECK_RT@@ pc=%08x addr=%08x val=%08x t0=%08x t1=%08x t2=%08x t4=%08x a2=%08x",
                    guest_pc, addr, memRead32(addr), t0_val, t1_val, t2_val, t4_val, a2_val);
                
                // Compute align16 of the value being read (if this is a size field read)
                u32 val_read = memRead32(addr);
                u32 aligned = (val_read + 15) & ~15;
                Console.WriteLn("@@LF_PRECHECK_ALIGN@@ pc=%08x size=%08x align=%08x t2=%08x",
                    guest_pc, val_read, aligned, t2_val);
                
                s_precheck_hits++;
            }
        }
    }

	struct Kseg1ProbeRecord
	{
		u32 addr_in;
		u32 pc;
		u32 val32;
		u32 reserved;
		u64 vmap_raw;
		u64 host_ptr;
	};
	constexpr u32 kProbeRingSize = 32;
	static Kseg1ProbeRecord s_probe_ring[kProbeRingSize];
	static u32 s_probe_write = 0;
	static u32 s_probe_count = 0;
	static int s_probe_dumped = 0;

	u64 vmap_raw = 0;
	u64 host_ptr = 0;
	if (s_kseg1_probe_enabled)
	{
		const auto vmv = vtlb_private::vtlbdata.vmap[addr >> vtlb_private::VTLB_PAGE_BITS];
		vmap_raw = static_cast<u64>(vmv.raw());
		if (!vmv.isHandler(addr))
			host_ptr = static_cast<u64>(vmv.assumePtr(addr));
	}

	u32 val = memRead32(addr);

	// [TEMP_DIAG][REMOVE_AFTER=EE_9FC41048_T0_WAIT_ROOTCAUSE_V1]
	// EE BIOS wait-loop reads two consecutive samples from KSEG1 timer mirror (9FC41040/48).
	// Probe/toggle is tightly scoped to those reads only, with capped logging.
	const bool ee41048_loop_read = (addr == 0xB0000000u) && (cpuRegs.pc == 0x9FC41040u || cpuRegs.pc == 0x9FC41048u);
	if (ee41048_loop_read)
	{
		static u32 s_ee41048_probe_n = 0;
		static u32 s_ee41048_toggle_n = 0;
		static u32 s_ee41048_prev_cycle = 0;
		static u32 s_ee41048_prev_val = 0xFFFFFFFFu;

		if (s_ee41048_loop_toggle_enabled)
		{
			const u32 injected = (s_ee41048_toggle_n++ & 1u);
			if (s_ee41048_probe_n < 64)
			{
				Console.WriteLn("@@EE41048_KSEG1@@ n=%u pc=%08x cycle=%u raw=%08x out=%08x dcycle=%u mode=toggle",
					s_ee41048_probe_n, cpuRegs.pc, cpuRegs.cycle, val, injected, cpuRegs.cycle - s_ee41048_prev_cycle);
			}
			s_ee41048_prev_cycle = cpuRegs.cycle;
			s_ee41048_prev_val = injected;
			if (s_ee41048_probe_n < 64)
				s_ee41048_probe_n++;
			emit_kseg1_rd32_probe(injected);
			return injected;
		}

		if (s_ee41048_loop_probe_enabled && s_ee41048_probe_n < 64)
		{
			Console.WriteLn("@@EE41048_KSEG1@@ n=%u pc=%08x cycle=%u raw=%08x out=%08x dcycle=%u prev=%08x mode=probe",
				s_ee41048_probe_n, cpuRegs.pc, cpuRegs.cycle, val, val, cpuRegs.cycle - s_ee41048_prev_cycle, s_ee41048_prev_val);
			s_ee41048_probe_n++;
		}
		s_ee41048_prev_cycle = cpuRegs.cycle;
		s_ee41048_prev_val = val;
	}

	static int s_probe_9fc41048_enabled = -1;
	if (s_probe_9fc41048_enabled < 0)
	{
		const bool safe_only = iPSX2_IsSafeOnlyEnabled();
		const bool diag_enabled = (!safe_only && iPSX2_GetRuntimeEnvBool("iPSX2_ENABLE_DIAG_FLAGS", false));
		s_probe_9fc41048_enabled = (diag_enabled && iPSX2_GetRuntimeEnvBool("iPSX2_PROBE_EE_9FC41048", false)) ? 1 : 0;
		Console.WriteLn("@@CFG@@ iPSX2_PROBE_EE_9FC41048=%d", s_probe_9fc41048_enabled);
	}
	if (s_probe_9fc41048_enabled && cpuRegs.pc == 0x9fc41048 && addr == 0xb0000000)
	{
		static int s_probe_9fc41048_count = 0;
		if (s_probe_9fc41048_count < 1)
		{
			Console.WriteLn(
				"@@EE41048_CTX@@ pc=%08x addr=%08x val=%08x v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x a3=%08x t0=%08x t1=%08x t2=%08x t3=%08x s0=%08x s1=%08x ra=%08x",
				cpuRegs.pc, addr, val, cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0], cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.a3.UL[0],
				cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.t2.UL[0], cpuRegs.GPR.n.t3.UL[0],
				cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0], cpuRegs.GPR.n.ra.UL[0]);
			s_probe_9fc41048_count++;
		}
	}

	if (s_kseg1_probe_enabled)
	{
		const u32 slot = s_probe_write & (kProbeRingSize - 1);
		Kseg1ProbeRecord& rec = s_probe_ring[slot];
		rec.addr_in = addr;
		rec.pc = cpuRegs.pc;
		rec.val32 = val;
		rec.reserved = 0;
		rec.vmap_raw = vmap_raw;
		rec.host_ptr = host_ptr;
		s_probe_write++;
		if (s_probe_count < kProbeRingSize)
			s_probe_count++;

		if (!s_probe_dumped && cpuRegs.pc == 0xBFC02454)
		{
			s_probe_dumped = 1;
			Console.WriteLn("@@KSEG1_PROBE_DUMP@@ count=%u write=%u panic_pc=%08x", s_probe_count, s_probe_write, cpuRegs.pc);
			for (u32 i = 0; i < s_probe_count; i++)
			{
				const u32 idx = (s_probe_write - s_probe_count + i) & (kProbeRingSize - 1);
				const Kseg1ProbeRecord& r = s_probe_ring[idx];
				Console.WriteLn(
					"@@KSEG1_PROBE@@ idx=%u pc=%08x addr_in=%08x vmap_raw=%016llx host_ptr=%016llx val32=%08x",
					i, r.pc, r.addr_in, static_cast<unsigned long long>(r.vmap_raw),
					static_cast<unsigned long long>(r.host_ptr), r.val32);
			}
		}
	}

	static int s_kseg1_read_probe_count = 0;
	if (s_kseg1_rd32_log_enabled && s_kseg1_read_probe_count < 8)
	{
		Console.WriteLn("@@KSEG1_RD32@@ pc=%08x addr=%08x val=%08x t0=%08x t1=%08x ra=%08x",
			cpuRegs.pc, addr, val, cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.ra.UL[0]);
		s_kseg1_read_probe_count++;
	}

	emit_kseg1_rd32_probe(val);
	return val;
}

extern "C" void recLogDispatcher(u32 pc, u64 target)
{
    // Filter to reduce spam: Only show BIOS area or specifically the hang PC
    if ((pc & 0xfff00000) == 0x9fc00000 || (pc & 0xffff0000) == 0xbfc00000) {
        static int count = 0;
        if (count < 500) {
             Console.WriteLn("DEBUG: JIT Dispatch: PC=%08x -> Target=%llx", pc, target);
             count++;
        }
    }
}
