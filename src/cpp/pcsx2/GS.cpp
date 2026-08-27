// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Counters.h"
#include "Common.h"
#include "Config.h"
#include "Gif_Unit.h"
#include "MTGS.h"
#include "VMManager.h"
#include "GS/GSRegs.h"

// [iPSX2]
extern "C" void LogUnified(const char* fmt, ...);

#include <atomic>
#include <list>

alignas(16) u8 g_RealGSMem[Ps2MemSize::GSregs];
static bool s_GSRegistersWritten = false;

// [R64 TEMP_DIAG] @@DISPFB2_COUNTER@@ globals
// Removal condition: BIOS animationdisplayafter confirmed
std::atomic<uint32_t> g_dispfb2_write_count{0};
std::atomic<uint64_t> g_dispfb2_last_value{0};

void gsSetVideoMode(GS_VideoMode mode)
{
	gsVideoMode = mode;
	UpdateVSyncRate(false);
}

// Make sure framelimiter options are in sync with GS capabilities.
void gsReset()
{
	MTGS::ResetGS(true);
	gsVideoMode = GS_VideoMode::Uninitialized;
	std::memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
	UpdateVSyncRate(true);
}

static __fi void gsCSRwrite( const tGS_CSR& csr )
{
	if (csr.RESET) {
		GUNIT_WARN("GUNIT_WARN: csr.RESET");
		//Console.Warning( "csr.RESET" );
		//gifUnit.Reset(true); // Don't think gif should be reset...
		gifUnit.gsSIGNAL.queued = false;
		gifUnit.gsFINISH.gsFINISHFired = true;
		gifUnit.gsFINISH.gsFINISHPending = false;
		// Privilage registers also reset.
		// [P48_FIX] Preserve CSR FIELD bit across GS RESET to prevent field parity inversion.
		// memset zeros the FIELD bit, and CSRreg.Reset() does not restore it.
		// This causes permanent interlace field parity inversion if the RESET timing
		// differs between JIT and Interpreter relative to vsync boundaries.
		const u32 saved_field = CSRreg._u32 & 0x2000;
		std::memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
		GSIMR.reset();
		CSRreg.Reset();
		CSRreg._u32 |= saved_field;
		MTGS::ResetGS(false);
	}

	if(csr.FLUSH)
	{
		// Our emulated GS has no FIFO, but if it did, it would flush it here...
		//Console.WriteLn("GS_CSR FLUSH GS fifo: %x (CSRr=%x)", value, GSCSRr);
	}

	if(csr.SIGNAL)
	{
		const bool resume = CSRreg.SIGNAL;
		// SIGNAL : What's not known here is whether or not the SIGID register should be updated
		//  here or when the IMR is cleared (below).
		GUNIT_LOG("csr.SIGNAL");
		if (gifUnit.gsSIGNAL.queued) {
			//DevCon.Warning("Firing pending signal");
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~gifUnit.gsSIGNAL.data[1])
				        | (gifUnit.gsSIGNAL.data[0]&gifUnit.gsSIGNAL.data[1]);

			if (!GSIMR.SIGMSK) gsIrq();
			CSRreg.SIGNAL  = true; // Just to be sure :p
		}
		else CSRreg.SIGNAL = false;
		gifUnit.gsSIGNAL.queued = false;

		if (resume)
			gifUnit.Execute(false, true); // Resume paused transfers
	}

	if (csr.FINISH)	{
		CSRreg.FINISH = false;
		gifUnit.gsFINISH.gsFINISHFired = false; //Clear the previously fired FINISH (YS, Indiecar 2005, MGS3)
		gifUnit.gsFINISH.gsFINISHPending = false;
	}
	if(csr.HSINT)	CSRreg.HSINT	= false;
	if(csr.VSINT)	CSRreg.VSINT	= false;
	if(csr.EDWINT)	CSRreg.EDWINT	= false;
}

static __fi void IMRwrite(u32 value)
{
	GUNIT_LOG("IMRwrite()");

	if (CSRreg.GetInterruptMask() & (~value & GSIMR._u32) >> 8)
		gsIrq();

	GSIMR._u32 = (value & 0x1f00)|0x6000;
}

__fi void gsWrite8(u32 mem, u8 value)
{
	switch (mem)
	{
		// CSR 8-bit write handlers.
		// I'm quite sure these would just write the CSR portion with the other
		// bits set to 0 (no action).  The previous implementation masked the 8-bit
		// write value against the previous CSR write value, but that really doesn't
		// make any sense, given that the real hardware's CSR circuit probably has no
		// real "memory" where it saves anything.  (for example, you can't write to
		// and change the GS revision or ID portions -- they're all hard wired.) --air

		case GS_CSR: // GS_CSR
			gsCSRwrite( tGS_CSR((u32)value) );			break;
		case GS_CSR + 1: // GS_CSR
			gsCSRwrite( tGS_CSR(((u32)value) <<  8) );	break;
		case GS_CSR + 2: // GS_CSR
			gsCSRwrite( tGS_CSR(((u32)value) << 16) );	break;
		case GS_CSR + 3: // GS_CSR
			gsCSRwrite( tGS_CSR(((u32)value) << 24) );	break;

		default:
			*PS2GS_BASE(mem) = value;
		break;
	}
	GIF_LOG("GS write 8 at %8.8lx with data %8.8lx", mem, value);
}

//////////////////////////////////////////////////////////////////////////
// GS Write 16 bit

__fi void gsWrite16(u32 mem, u16 value)
{
	GIF_LOG("GS write 16 at %8.8lx with data %8.8lx", mem, value);

	switch (mem)
	{
		// See note above about CSR 8 bit writes, and handling them as zero'd bits
		// for all but the written parts.

		case GS_CSR:
			gsCSRwrite( tGS_CSR((u32)value) );
		return; // do not write to MTGS memory

		case GS_CSR+2:
			gsCSRwrite( tGS_CSR(((u32)value) << 16) );
		return; // do not write to MTGS memory

		case GS_IMR:
			IMRwrite(value);
		return; // do not write to MTGS memory
	}

	*(u16*)PS2GS_BASE(mem) = value;
}

//////////////////////////////////////////////////////////////////////////
// GS Write 32 bit

__fi void gsWrite32(u32 mem, u32 value)
{
	pxAssume( (mem & 3) == 0 );
	GIF_LOG("GS write 32 at %8.8lx with data %8.8lx", mem, value);
	// [iter101] catch 32-bit writes to key GS display registers
	if (mem == GS_PMODE || mem == GS_DISPLAY1 || mem == GS_DISPLAY2 ||
	    mem == GS_DISPFB1 || mem == GS_DISPFB2) {
		static u32 s_gs32_n = 0;
		if (s_gs32_n < 16) {
			LogUnified("@@GS_WRITE32@@ n=%u mem=%08x val=%08x\n", s_gs32_n, mem, value);
			s_gs32_n++;
		}
	}

	switch (mem)
	{
		case GS_CSR:
			gsCSRwrite(tGS_CSR(value));
		return;

		case GS_IMR:
			IMRwrite(value);
		return;
	}

	// [R61] @@GS_W32_LATE@@ — 32-bit GS display reg late write detection
	// Removal condition: GS register破損のroot causeafter determined
	{
		const bool is_disp = (mem >= 0x12000010u && mem <= 0x120000F0u);
		if (is_disp && cpuRegs.cycle > 100000000u) {
			static int s_w32l_n = 0;
			if (s_w32l_n < 30) {
				Console.WriteLn("@@GS_W32_LATE@@ n=%d mem=%08x val=%08x ee_pc=%08x ra=%08x cycle=%u",
					s_w32l_n++, mem, value, cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.cycle);
			}
		}
	}
	*(u32*)PS2GS_BASE(mem) = value;
}

//////////////////////////////////////////////////////////////////////////
// GS Write 64 bit

void gsWrite64_generic( u32 mem, u64 value )
{
	GIF_LOG("GS Write64 at %8.8lx with data %8.8x_%8.8x", mem, (u32)(value >> 32), (u32)value);

	std::memcpy(PS2GS_BASE(mem), &value, sizeof(value));
}

void gsWrite64_page_00( u32 mem, u64 value )
{
	// [iter101] @@GS_PAGE00_W64@@ – log first 20 unique GS page-00 64-bit writes
	{
		static u32 s_p00_n = 0;
		if (s_p00_n < 80) { // [iter244] expanded from 20 to catch LOGO writes
			LogUnified("@@GS_PAGE00_W64@@ n=%u mem=%08x val=%016llx\n", s_p00_n, mem, value);
			s_p00_n++;
		}
	}
	// [P12] @@GS_PAGE00_W64_LOG@@ — Console.WriteLn版 (pcsx2_log.txt へ出力, cap=50)
	// 目的: JIT vs Interpreter の GS page0 書き込みシーケンス比較
	// Removal condition: Sony ロゴdisplayafter confirmed
	{
		static int s_p00_log_n = 0;
		if (s_p00_log_n < 50) {
			++s_p00_log_n;
			Console.WriteLn("@@GS_PAGE00_W64_LOG@@ n=%d mem=%08x val=%016llx ee_pc=%08x",
				s_p00_log_n, mem, (unsigned long long)value, cpuRegs.pc);
		}
	}
	// [R61] @@GS_DISP_CORRUPT_DETECT@@ — GS display reg 64-bit upper=lower 破損detect
	// 目的: SMODE2/DISPFB/DISPLAY/BGCOLOR への破損書き込みをキャッチ
	// Removal condition: GS register破損のroot causeafter determined
	{
		const u32 lo = (u32)(value);
		const u32 hi = (u32)(value >> 32);
		const bool is_disp_reg = (mem == GS_SMODE2 || mem == GS_DISPFB1 || mem == GS_DISPLAY1 ||
		                          mem == GS_DISPFB2 || mem == GS_DISPLAY2 || mem == GS_BGCOLOR);
		// Detect: upper32==lower32 (and non-zero) — the known corruption pattern
		if (is_disp_reg && hi == lo && lo != 0) {
			static int s_corrupt_n = 0;
			if (s_corrupt_n < 30) {
				Console.WriteLn("@@GS_DISP_CORRUPT_DETECT@@ n=%d mem=%08x val=%016llx hi=%08x==lo=%08x ee_pc=%08x ra=%08x cycle=%u",
					s_corrupt_n++, mem, (unsigned long long)value, hi, lo, cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.cycle);
			}
		}
		// Also log ALL writes to these display regs after cycle 100M (late writes)
		if (is_disp_reg && cpuRegs.cycle > 100000000u) {
			static int s_late_n = 0;
			if (s_late_n < 50) {
				Console.WriteLn("@@GS_DISP_LATE_WRITE@@ n=%d mem=%08x val=%016llx ee_pc=%08x ra=%08x cycle=%u",
					s_late_n++, mem, (unsigned long long)value, cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.cycle);
			}
		}
	}
	// [iPSX2] One-Shot GS Register Logging
	if (mem == GS_SMODE1) {
		static bool s_log_smode1 = false;
		if (!s_log_smode1) {
			GSRegSMODE1 s;
			s.U64 = value;
			const char* std_name = "UNKNOWN";
			if (s.CMOD == 2) std_name = "NTSC";
			else if (s.CMOD == 3) std_name = "PAL";
			else if (s.CMOD == 0) std_name = "VESA";

			LogUnified("@@GS_SMODE1@@ raw=%016llx cmod=%d std=%s\n", value, s.CMOD, std_name);
			s_log_smode1 = true;
		}

		// [iter238c] GS_FORCE_PMODE/DISPFB2/DISPLAY2 ブロック撤去
		// iter221 で導入したが、VRAM 未初期化データが青画面としてdisplayされるcause。
		// LOGO が自然に GS registerをconfigするまで待つ。
	}
	if (mem == GS_PMODE) {
		static bool s_log_pmode = false;
		if (!s_log_pmode) {
			GSRegPMODE p;
			p.U64 = value;
			LogUnified("@@GS_PMODE@@ raw=%016llx en1=%d en2=%d crtmd=%d alp=%d\n", value, p.EN1, p.EN2, p.CRTMD, p.ALP);
			s_log_pmode = true;
		}
		// [P12] @@GS_PMODE_TRACE@@ — PMODE 全書き込みを追跡 (cap=30)
		// 目的: JIT vs Interpreter の PMODE 書き込みシーケンス比較
		// Removal condition: JIT modeで Sony ロゴ描画after confirmed
		{
			static int s_pmode_trace_n = 0;
			if (s_pmode_trace_n < 30) {
				++s_pmode_trace_n;
				Console.WriteLn("@@GS_PMODE_TRACE@@ n=%d val=%016llx EN1=%d EN2=%d ee_pc=%08x",
					s_pmode_trace_n, (unsigned long long)value,
					(int)(value & 1), (int)((value >> 1) & 1), cpuRegs.pc);
			}
		}
	}
	if (mem == GS_DISPFB1) {
		 static bool s_log_dispfb1 = false;
		 if (!s_log_dispfb1) {
			 GSRegDISPFB d;
			 d.U64 = value;
			 LogUnified("@@GS_DISPFB1@@ raw=%016llx fbp=%d fbw=%d psm=%d dbx=%d dby=%d\n", value, d.FBP, d.FBW, d.PSM, d.DBX, d.DBY);
			 s_log_dispfb1 = true;
		 }
	}
	if (mem == GS_DISPFB2) {
		 static bool s_log_dispfb2 = false;
		 if (!s_log_dispfb2) {
			 GSRegDISPFB d;
			 d.U64 = value;
			 LogUnified("@@GS_DISPFB2@@ raw=%016llx fbp=%d fbw=%d psm=%d dbx=%d dby=%d\n", value, d.FBP, d.FBW, d.PSM, d.DBX, d.DBY);
			 s_log_dispfb2 = true;
		 }
	}
	// [iter101] @@GS_DISPLAY1/2@@ – log first write to DISPLAY regs (64-bit path)
	if (mem == GS_DISPLAY1) {
		static bool s_log_disp1 = false;
		if (!s_log_disp1) {
			GSRegDISPLAY d; d.U64 = value;
			LogUnified("@@GS_DISPLAY1_64@@ raw=%016llx dx=%d dy=%d magh=%d magv=%d dw=%d dh=%d\n",
				value, d.DX, d.DY, d.MAGH, d.MAGV, d.DW, d.DH);
			s_log_disp1 = true;
		}
	}
	if (mem == GS_DISPLAY2) {
		static bool s_log_disp2 = false;
		if (!s_log_disp2) {
			GSRegDISPLAY d; d.U64 = value;
			LogUnified("@@GS_DISPLAY2_64@@ raw=%016llx dx=%d dy=%d magh=%d magv=%d dw=%d dh=%d\n",
				value, d.DX, d.DY, d.MAGH, d.MAGV, d.DW, d.DH);
			s_log_disp2 = true;
		}
	}

	// [R64 TEMP_DIAG] @@DISPFB2_COUNTER@@ — track total DISPFB2 write count + last value + GPR snapshot
	// Removal condition: BIOS animationdisplayafter confirmed
	if (mem == GS_DISPFB2) {
		extern std::atomic<uint32_t> g_dispfb2_write_count;
		extern std::atomic<uint64_t> g_dispfb2_last_value;
		u32 cnt = g_dispfb2_write_count.fetch_add(1, std::memory_order_relaxed);
		g_dispfb2_last_value.store(value, std::memory_order_relaxed);
		// Log GPR snapshot for writes 12-32 (after initial alternation should stop)
		if (cnt >= 12 && cnt < 32) {
			Console.WriteLn("@@DISPFB2_GPR@@ n=%u val=%016llx pc=%08x ra=%08x v0=%08x v1=%08x a0=%08x a1=%08x t0=%08x t1=%08x s0=%08x s1=%08x s2=%08x",
				cnt, (unsigned long long)value, cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0],
				cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
				cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
				cpuRegs.GPR.n.t0.UL[0], cpuRegs.GPR.n.t1.UL[0],
				cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0], cpuRegs.GPR.n.s2.UL[0]);
		}
	}

	s_GSRegistersWritten |= (mem == GS_DISPFB1 || mem == GS_DISPFB2 || mem == GS_PMODE);
	bool reqUpdate = false;
	if (mem == GS_SMODE1 || mem == GS_SMODE2)
	{
		if (value != *(u64*)PS2GS_BASE(mem))
			reqUpdate = true;
	}

	gsWrite64_generic( mem, value );

	if (reqUpdate)
		UpdateVSyncRate(false);
}

void gsWrite64_page_01( u32 mem, u64 value )
{
	GIF_LOG("GS Write64 at %8.8lx with data %8.8x_%8.8x", mem, (u32)(value >> 32), (u32)value);

	switch( mem )
	{
		case GS_BUSDIR:

			gifUnit.stat.DIR = static_cast<u32>(value) & 1;
			if (gifUnit.stat.DIR) {      // Assume will do local->host transfer
				gifUnit.stat.OPH = true; // Should we set OPH here?
				gifUnit.FlushToMTGS();   // Send any pending GS Primitives to the GS
				GUNIT_LOG("Busdir - GS->EE Download");
			}
			else {
				GUNIT_LOG("Busdir - EE->GS Upload");
			}

			gsWrite64_generic( mem, value );
		return;

		case GS_CSR:
			gsCSRwrite(tGS_CSR(value));
		return;

		case GS_IMR:
			IMRwrite(static_cast<u32>(value));
		return;
	}

	gsWrite64_generic( mem, value );
}

//////////////////////////////////////////////////////////////////////////
// GS Write 128 bit

void TAKES_R128 gsWrite128_page_00( u32 mem, r128 value )
{
	gsWrite128_generic( mem, value );
}

void TAKES_R128 gsWrite128_page_01( u32 mem, r128 value )
{
	switch( mem )
	{
		case GS_CSR:
			gsCSRwrite(r128_to_u32(value));
		return;

		case GS_IMR:
			IMRwrite(r128_to_u32(value));
		return;
	}

	gsWrite128_generic( mem, value );
}

void TAKES_R128 gsWrite128_generic( u32 mem, r128 value )
{
	alignas(16) const u128 uvalue = r128_to_u128(value);
	GIF_LOG("GS Write128 at %8.8lx with data %8.8x_%8.8x_%8.8x_%8.8x", mem,
		uvalue._u32[3], uvalue._u32[2], uvalue._u32[1], uvalue._u32[0]);

	// [R61] @@GS_W128_CORRUPT_DETECT@@ — 128-bit 書き込みで upper32==lower32 破損detect
	// Removal condition: GS register破損のroot causeafter determined
	{
		const u32 lo = uvalue._u32[0];
		const u32 hi = uvalue._u32[1];
		const u32 u2 = uvalue._u32[2];
		const u32 u3 = uvalue._u32[3];
		const bool is_disp_area = (mem >= 0x12000010u && mem <= 0x120000F0u);
		if (is_disp_area && hi == lo && lo != 0) {
			static int s_w128c_n = 0;
			if (s_w128c_n < 30) {
				Console.WriteLn("@@GS_W128_CORRUPT_DETECT@@ n=%d mem=%08x u0=%08x u1=%08x u2=%08x u3=%08x ee_pc=%08x ra=%08x cycle=%u",
					s_w128c_n++, mem, lo, hi, u2, u3, cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.cycle);
			}
		}
		// Also log ALL 128-bit writes to GS page 0 after cycle 100M
		if (is_disp_area && cpuRegs.cycle > 100000000u) {
			static int s_w128l_n = 0;
			if (s_w128l_n < 50) {
				Console.WriteLn("@@GS_W128_LATE@@ n=%d mem=%08x u0=%08x u1=%08x u2=%08x u3=%08x ee_pc=%08x ra=%08x cycle=%u",
					s_w128l_n++, mem, lo, hi, u2, u3, cpuRegs.pc, cpuRegs.GPR.n.ra.UL[0], cpuRegs.cycle);
			}
		}
	}

	r128_store(PS2GS_BASE(mem), value);
}

__fi u8 gsRead8(u32 mem)
{
	GIF_LOG("GS read 8 from %8.8lx  value: %8.8lx", mem, *(u8*)PS2GS_BASE(mem));

	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u8*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u8*)PS2GS_BASE(GS_CSR + (mem & 0xF));
	}
}

__fi u16 gsRead16(u32 mem)
{
	GIF_LOG("GS read 16 from %8.8lx  value: %8.8lx", mem, *(u16*)PS2GS_BASE(mem));
	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u16*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u16*)PS2GS_BASE(GS_CSR + (mem & 0x7));
	}
}

__fi u32 gsRead32(u32 mem)
{
	GIF_LOG("GS read 32 from %8.8lx  value: %8.8lx", mem, *(u32*)PS2GS_BASE(mem));

	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u32*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u32*)PS2GS_BASE(GS_CSR + (mem & 0xC));
	}
}

__fi u64 gsRead64(u32 mem)
{
	// fixme - PS2GS_BASE(mem+4) = (g_RealGSMem+(mem + 4 & 0x13ff))
	GIF_LOG("GS read 64 from %8.8lx  value: %8.8lx_%8.8lx", mem, *(u32*)PS2GS_BASE(mem+4), *(u32*)PS2GS_BASE(mem) );

	switch (mem & ~0xF)
	{
		case GS_SIGLBLID:
			return *(u64*)PS2GS_BASE(mem);
		default: // Only SIGLBLID and CSR are readable, everything else mirrors CSR
			return *(u64*)PS2GS_BASE(GS_CSR + (mem & 0x8));
	}
}

__fi u128 gsNonMirroredRead(u32 mem)
{
	return *(u128*)PS2GS_BASE(mem);
}

void gsIrq() {
	hwIntcIrq(INTC_GS);
}

//These are done at VSync Start.  Drawing is done when VSync is off, then output the screen when Vsync is on
//The GS needs to be told at the start of a vsync else it loses half of its picture (could be responsible for some halfscreen issues)
//We got away with it before i think due to our awful GS timing, but now we have it right (ish)
void gsPostVsyncStart()
{
	//gifUnit.FlushToMTGS();  // Needed for some (broken?) homebrew game loaders

	const bool registers_written = s_GSRegistersWritten;
	s_GSRegistersWritten = false;
	MTGS::PostVsyncStart(registers_written);
}

bool SaveStateBase::gsFreeze()
{
	FreezeMem(PS2MEM_GS, 0x2000);
	Freeze(gsVideoMode);
	return IsOkay();
}

