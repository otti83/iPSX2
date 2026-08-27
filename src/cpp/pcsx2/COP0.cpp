// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "COP0.h"

// Updates the CPU's mode of operation (either, Kernel, Supervisor, or User modes).
// Currently the different modes are not implemented.
// Given this function is called so much, it's commented out for now. (rama)
__ri void cpuUpdateOperationMode()
{

	//u32 value = cpuRegs.CP0.n.Status.val;

	//if (value & 0x06 ||
	//	(value & 0x18) == 0) { // Kernel Mode (KSU = 0 | EXL = 1 | ERL = 1)*/
	//	memSetKernelMode();	// Kernel memory always
	//} else { // User Mode
	//	memSetUserMode();
	//}
}

void WriteCP0Status(u32 value)
{
    // [iter156] stderr→Console.WriteLn にchange: pcsx2_log.txt で BEV クリア履歴をverify
    // Removal condition: BEV クリアの有無とタイミング確定時
    static int status_log_count = 0;
    if (status_log_count < 50) {
        Console.WriteLn("@@COP0@@ WriteCP0Status n=%d value=%08x pc=%08x bev=%d ra=%08x",
            status_log_count, value, cpuRegs.pc, (value >> 22) & 1,
            cpuRegs.GPR.n.ra.UL[0]);
        // [P12] BEV クリア時点の EELOAD memorystateとregisterを記録 (Removal condition: 分岐causeafter identified)
        if (status_log_count == 1 && eeMem) {
            Console.WriteLn("@@BEV_CLEAR_EELOAD@@ eeMem82000=%08x [82004]=%08x [82008]=%08x [8200c]=%08x",
                *reinterpret_cast<u32*>(eeMem->Main + 0x82000),
                *reinterpret_cast<u32*>(eeMem->Main + 0x82004),
                *reinterpret_cast<u32*>(eeMem->Main + 0x82008),
                *reinterpret_cast<u32*>(eeMem->Main + 0x8200c));
            Console.WriteLn("@@BEV_CLEAR_REGS@@ a0=%08x a1=%08x a2=%08x s0=%08x s1=%08x t0=%08x t1=%08x epc=%08x",
                cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
                cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.s0.UL[0],
                cpuRegs.GPR.n.s1.UL[0], cpuRegs.GPR.n.t0.UL[0],
                cpuRegs.GPR.n.t1.UL[0], cpuRegs.CP0.n.EPC);
            // [P12] a2=bfc49200 ROM template dump — dispatch table source (Removal condition: 分岐causeafter identified)
            {
                u32 a2_va = cpuRegs.GPR.n.a2.UL[0];
                u32 a2_phys = a2_va & 0x1FFFFFFFu;
                const u32 rom0_start = 0x1FC00000u;
                if (a2_phys >= rom0_start && a2_phys + 16 < rom0_start + Ps2MemSize::Rom && eeMem) {
                    u32 off = a2_phys - rom0_start;
                    const u32* p = reinterpret_cast<const u32*>(eeMem->ROM + off);
                    Console.WriteLn("@@ROM_TMPL@@ a2=%08x rom[0]=%08x [4]=%08x [8]=%08x [c]=%08x",
                        a2_va, p[0], p[1], p[2], p[3]);
                }
            }
        }
        // [iter240] Status=0 書き込みの命令コンテキストをダンプ
        if (value == 0 && eeMem) {
            u32 phys = cpuRegs.pc & 0x1FFFFFFFu;
            if (phys >= 8 && phys + 12 < Ps2MemSize::MainRam) {
                Console.WriteLn("@@COP0_STATUS0@@ insn[-2]=%08x [-1]=%08x [0]=%08x [+1]=%08x [+2]=%08x "
                    "GPR: v0=%08x v1=%08x a0=%08x a1=%08x a2=%08x t0=%08x t1=%08x s0=%08x ra=%08x",
                    *(u32*)(eeMem->Main + phys - 8), *(u32*)(eeMem->Main + phys - 4),
                    *(u32*)(eeMem->Main + phys), *(u32*)(eeMem->Main + phys + 4),
                    *(u32*)(eeMem->Main + phys + 8),
                    cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
                    cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
                    cpuRegs.GPR.n.a2.UL[0], cpuRegs.GPR.n.t0.UL[0],
                    cpuRegs.GPR.n.t1.UL[0], cpuRegs.GPR.n.s0.UL[0],
                    cpuRegs.GPR.n.ra.UL[0]);
            }
        }
        status_log_count++;
    }

	COP0_UpdatePCCR();
	cpuRegs.CP0.n.Status.val = value;
	cpuSetNextEventDelta(4);
}

void WriteCP0Config(u32 value)
{
	// Protect the read-only ICacheSize (IC) and DataCacheSize (DC) bits
	cpuRegs.CP0.n.Config = value & ~0xFC0;
	cpuRegs.CP0.n.Config |= 0x440;
}

//////////////////////////////////////////////////////////////////////////////////////////
// Performance Counters Update Stuff!
//
// Note regarding updates of PERF and TIMR registers: never allow increment to be 0.
// That happens when a game loads the MFC0 twice in the same recompiled block (before the
// cpuRegs.cycles update), and can cause games to lock up since it's an unexpected result.
//
// PERF Overflow exceptions:  The exception is raised when the MSB of the Performance
// Counter Register is set.  I'm assuming the exception continues to re-raise until the
// app clears the bit manually (needs testing).
//
// PERF Events:
//  * Event 0 on PCR 0 is unused (counter disable)
//  * Event 16 is usable as a specific counter disable bit (since CTE affects both counters)
//  * Events 17-31 are reserved (act as counter disable)
//
// Most event mode aren't supported, and issue a warning and do a standard instruction
// count.  But only mode 1 (instruction counter) has been found to be used by games thus far.
//

static __fi bool PERF_ShouldCountEvent(uint evt)
{
	switch (evt)
	{
			// This is a rough table of actions for various PCR modes.  Some of these
			// can be implemented more accurately later.  Others (WBBs in particular)
			// probably cannot without some severe complications.

			// left sides are PCR0 / right sides are PCR1

		case 1: // cpu cycle counter.
		case 2: // single/dual instruction issued
		case 3: // Branch issued / Branch mispredicated
			return true;

		case 4: // BTAC/TLB miss
		case 5: // ITLB/DTLB miss
		case 6: // Data/Instruction cache miss
			return false;

		case 7: // Access to DTLB / WBB single request fail
		case 8: // Non-blocking load / WBB burst request fail
		case 9:
		case 10:
			return false;

		case 11: // CPU address bus busy / CPU data bus busy
			return false;

		case 12: // Instruction completed
		case 13: // non-delayslot instruction completed
		case 14: // COP2/COP1 instruction complete
		case 15: // Load/Store completed
			return true;
	}

	return false;
}

// Diagnostics for event modes that we just ignore for now.  Using these perf units could
// cause compat issues in some very odd/rare games, so if this msg comes up who knows,
// might save some debugging effort. :)
void COP0_DiagnosticPCCR()
{
	if (cpuRegs.PERF.n.pccr.b.Event0 >= 7 && cpuRegs.PERF.n.pccr.b.Event0 <= 10)
		Console.Warning("PERF/PCR0 Unsupported Update Event Mode = 0x%x", cpuRegs.PERF.n.pccr.b.Event0);

	if (cpuRegs.PERF.n.pccr.b.Event1 >= 7 && cpuRegs.PERF.n.pccr.b.Event1 <= 10)
		Console.Warning("PERF/PCR1 Unsupported Update Event Mode = 0x%x", cpuRegs.PERF.n.pccr.b.Event1);
}
extern int branch;
static int s_tlbwi_probe_cfg = -1;
static bool s_tlbwi_probe_wi_done = false;
static bool s_tlbwi_probe_wr_done = false;
static int s_tlbwi_probe_wi_count = 0;
static int s_tlbwi_probe_wr_count = 0;
__fi void COP0_UpdatePCCR()
{
	// Counting and counter exceptions are not performed if we are currently executing a Level 2 exception (ERL)
	// or the counting function is not enabled (CTE)
	if (cpuRegs.CP0.n.Status.b.ERL || !cpuRegs.PERF.n.pccr.b.CTE)
	{
		cpuRegs.lastPERFCycle[0] = cpuRegs.cycle;
		cpuRegs.lastPERFCycle[1] = cpuRegs.lastPERFCycle[0];
		return;
	}

	// Implemented memory mode check (kernel/super/user)

	if (cpuRegs.PERF.n.pccr.val & ((1 << (cpuRegs.CP0.n.Status.b.KSU + 2)) | (cpuRegs.CP0.n.Status.b.EXL << 1)))
	{
		// ----------------------------------
		//    Update Performance Counter 0
		// ----------------------------------

		if (PERF_ShouldCountEvent(cpuRegs.PERF.n.pccr.b.Event0))
		{
			u32 incr = cpuRegs.cycle - cpuRegs.lastPERFCycle[0];
			if (incr == 0)
				incr++;

			// use prev/XOR method for one-time exceptions (but likely less correct)
			//u32 prev = cpuRegs.PERF.n.pcr0;
			cpuRegs.PERF.n.pcr0 += incr;
			//DevCon.Warning("PCR VAL %x", cpuRegs.PERF.n.pccr.val);
			//prev ^= (1UL<<31);		// XOR is fun!
			//if( (prev & cpuRegs.PERF.n.pcr0) & (1UL<<31) )
			if ((cpuRegs.PERF.n.pcr0 & 0x80000000))
			{
				// TODO: Vector to the appropriate exception here.
				// This code *should* be correct, but is untested (and other parts of the emu are
				// not prepared to handle proper Level 2 exception vectors yet)

				//branch == 1 is probably not the best way to check for the delay slot, but it beats nothing! (Refraction)
				/*	if( branch == 1 )
				{
					cpuRegs.CP0.n.ErrorEPC = cpuRegs.pc - 4;
					cpuRegs.CP0.n.Cause |= 0x40000000;
				}
				else
				{
					cpuRegs.CP0.n.ErrorEPC = cpuRegs.pc;
					cpuRegs.CP0.n.Cause &= ~0x40000000;
				}

				if( cpuRegs.CP0.n.Status.b.DEV )
				{
					// Bootstrap vector
					cpuRegs.pc = 0xbfc00280;
				}
				else
				{
					cpuRegs.pc = 0x80000080;
				}
				cpuRegs.CP0.n.Status.b.ERL = 1;
				cpuRegs.CP0.n.Cause |= 0x20000;*/
			}
		}
	}

	if (cpuRegs.PERF.n.pccr.val & ((1 << (cpuRegs.CP0.n.Status.b.KSU + 12)) | (cpuRegs.CP0.n.Status.b.EXL << 11)))
	{
		// ----------------------------------
		//    Update Performance Counter 1
		// ----------------------------------

		if (PERF_ShouldCountEvent(cpuRegs.PERF.n.pccr.b.Event1))
		{
			u32 incr = cpuRegs.cycle - cpuRegs.lastPERFCycle[1];
			if (incr == 0)
				incr++;

			cpuRegs.PERF.n.pcr1 += incr;

			if ((cpuRegs.PERF.n.pcr1 & 0x80000000))
			{
				// TODO: Vector to the appropriate exception here.
				// This code *should* be correct, but is untested (and other parts of the emu are
				// not prepared to handle proper Level 2 exception vectors yet)

				//branch == 1 is probably not the best way to check for the delay slot, but it beats nothing! (Refraction)

				/*if( branch == 1 )
				{
					cpuRegs.CP0.n.ErrorEPC = cpuRegs.pc - 4;
					cpuRegs.CP0.n.Cause |= 0x40000000;
				}
				else
				{
					cpuRegs.CP0.n.ErrorEPC = cpuRegs.pc;
					cpuRegs.CP0.n.Cause &= ~0x40000000;
				}

				if( cpuRegs.CP0.n.Status.b.DEV )
				{
					// Bootstrap vector
					cpuRegs.pc = 0xbfc00280;
				}
				else
				{
					cpuRegs.pc = 0x80000080;
				}
				cpuRegs.CP0.n.Status.b.ERL = 1;
				cpuRegs.CP0.n.Cause |= 0x20000;*/
			}
		}
	}
	cpuRegs.lastPERFCycle[0] = cpuRegs.cycle;
	cpuRegs.lastPERFCycle[1] = cpuRegs.cycle;
}

//////////////////////////////////////////////////////////////////////////////////////////
//


void MapTLB(const tlbs& t, int i)
{
	// [iter216] Guard: skip map if TLB entry looks corrupted (same as UnmapTLB).
	if (t.PageMask.UL & 0xFE001FFFu) {
		static u32 s_mp_skip = 0;
		if (s_mp_skip < 5) {
			Console.Warning("@@MAP_TLB_SKIP@@ n=%u i=%d pc=%08x PM=0x%08x EHi=%08x ELo0=%08x ELo1=%08x (corrupted, skip)",
				s_mp_skip, i, cpuRegs.pc, t.PageMask.UL, t.EntryHi.UL, t.EntryLo0.UL, t.EntryLo1.UL);
			s_mp_skip++;
		}
		return;
	}

	u32 mask, addr;
	u32 saddr, eaddr;

	// @@MAPTLB_PROBE@@ count MapTLB calls, log first 200 with VPN2/Mask
	{
		static std::atomic<u32> s_maptlb_cnt{0};
		const u32 n = s_maptlb_cnt.fetch_add(1, std::memory_order_relaxed);
		if (n < 200)
			Console.WriteLn("@@MAPTLB_PROBE@@ n=%u idx=%d spr=%d vaddr=%08x pfn0=%08x pfn1=%08x mask=%08x pc=%08x",
				n, i, (int)(t.isSPR() != 0), t.VPN2(), t.PFN0(), t.PFN1(), t.Mask(), cpuRegs.pc);
		else if (n == 200)
			Console.WriteLn("@@MAPTLB_PROBE@@ n=%u (cap reached)", n);
	}

	// [iter44] Log ALL TLB entries with vaddr < 0x02000000 (EE RAM KUSEG range), capped at 20.
	// Removal condition: BIOS の EE RAM KUSEG mappingの有無after determined。
	{
		static std::atomic<u32> s_eeram_tlb_cnt{0};
		const u32 vaddr = t.VPN2();
		if (!t.isSPR() && vaddr < 0x02000000u && (t.EntryLo0.V || t.EntryLo1.V)) {
			const u32 n = s_eeram_tlb_cnt.fetch_add(1, std::memory_order_relaxed);
			if (n < 20)
				Console.WriteLn("@@EERAM_TLB@@ n=%u idx=%d vaddr=%08x pfn0=%08x pfn1=%08x lo0=%08x lo1=%08x mask=%08x pc=%08x",
					n, i, vaddr, t.PFN0(), t.PFN1(), t.EntryLo0.UL, t.EntryLo1.UL, t.Mask(), cpuRegs.pc);
			else if (n == 20)
				Console.WriteLn("@@EERAM_TLB@@ (cap reached)");
		}
	}

	// @@MAPTLB_VALID@@ iter10: one-shot when first valid TLB entry (V=1) is mapped
	{
		static bool s_valid_seen = false;
		if (!s_valid_seen && (t.EntryLo0.V || t.EntryLo1.V))
		{
			s_valid_seen = true;
			Console.WriteLn("[TEMP_DIAG] @@MAPTLB_VALID@@ FIRST_VALID idx=%d vaddr=%08x pfn0=%08x pfn1=%08x lo0=%08x lo1=%08x mask=%08x pc=%08x",
				i, t.VPN2(), t.PFN0(), t.PFN1(), t.EntryLo0.UL, t.EntryLo1.UL, t.Mask(), cpuRegs.pc);
		}
	}

	COP0_LOG("MAP TLB %d: 0x%08X-> [0x%08X 0x%08X] S=%d G=%d ASID=%d Mask=0x%03X EntryLo0 PFN=%x EntryLo0 Cache=%x EntryLo1 PFN=%x EntryLo1 Cache=%x VPN2=%x",
		i, t.VPN2(), t.PFN0(), t.PFN1(), t.isSPR() >> 31, t.isGlobal(), t.EntryHi.ASID,
		t.Mask(), t.EntryLo0.PFN, t.EntryLo0.C, t.EntryLo1.PFN, t.EntryLo1.C, t.VPN2());

	// According to the manual
	// 'It [SPR] must be mapped into a contiguous 16 KB of virtual address space that is
	// aligned on a 16KB boundary.Results are not guaranteed if this restriction is not followed.'
	// Assume that the game isn't doing anything less-than-ideal with the scratchpad mapping and map it directly to eeMem->Scratch.
	if (t.isSPR())
	{
		// [iter216] Only allow SPR mapping at 0x70000000 (standard scratchpad address).
		// Corrupted TLB entries may have S=1 with garbage VPN2, causing scratchpad
		// to overwrite EE RAM or ROM vtlb mappings. Skip those.
		if (t.VPN2() != 0x70000000) {
			static u32 s_spr_skip = 0;
			if (s_spr_skip < 5)
				Console.Warning("@@SPR_SKIP@@ n=%u i=%d pc=%08x VPN2=%08x (non-standard, skip)",
					s_spr_skip++, i, cpuRegs.pc, t.VPN2());
			return;
		}

		vtlb_VMapBuffer(t.VPN2(), eeMem->Scratch, Ps2MemSize::Scratch);
		// [iter214] Map scratchpad mirror at +16KB.
		vtlb_VMapBuffer(t.VPN2() + Ps2MemSize::Scratch, eeMem->Scratch, Ps2MemSize::Scratch);
		Console.WriteLn("@@SPR_MIRROR@@ VPN2=%08x mapped_mirror=%08x-%08x scratch=%p",
			t.VPN2(), t.VPN2() + Ps2MemSize::Scratch,
			t.VPN2() + Ps2MemSize::Scratch * 2 - 1, (void*)eeMem->Scratch);
	}
	else
	{
		if (t.EntryLo0.V)
		{
			mask = ((~t.Mask()) << 1) & 0xfffff;
			saddr = t.VPN2() >> 12;
			eaddr = saddr + t.Mask() + 1;

			for (addr = saddr; addr < eaddr; addr++)
			{
				if ((addr & mask) == ((t.VPN2() >> 12) & mask))
				{ //match
					memSetPageAddr(addr << 12, t.PFN0() + ((addr - saddr) << 12));
					Cpu->Clear(addr << 12, 0x400);
				}
			}
		}

		if (t.EntryLo1.V)
		{
			mask = ((~t.Mask()) << 1) & 0xfffff;
			saddr = (t.VPN2() >> 12) + t.Mask() + 1;
			eaddr = saddr + t.Mask() + 1;

			for (addr = saddr; addr < eaddr; addr++)
			{
				if ((addr & mask) == ((t.VPN2() >> 12) & mask))
				{ //match
					memSetPageAddr(addr << 12, t.PFN1() + ((addr - saddr) << 12));
					Cpu->Clear(addr << 12, 0x400);
				}
			}
		}
	}
}

__inline u32 ConvertPageMask(const u32 PageMask)
{
	const u32 mask = std::popcount(PageMask >> 13);

	// [iter215] Soft-fail: BIOS kernel init may set non-standard page masks.
	if ((mask & 1) || mask > 12) {
		static u32 s_pm_warn = 0;
		if (s_pm_warn < 3) {
			Console.Warning("@@TLB_PAGEMASK@@ n=%u invalid PageMask=0x%08x popcount=%u, clamping to 12",
				s_pm_warn, PageMask, mask);
			s_pm_warn++;
		}
		return (1 << (12 + std::min(mask & ~1u, 12u))) - 1;
	}

	return (1 << (12 + mask)) - 1;
}

void UnmapTLB(const tlbs& t, int i)
{
	// [iter216] Guard: skip unmap if TLB entry looks corrupted.
	// Valid PageMask only uses bits 13-24; bits 0-12 and 25-31 must be zero.
	// Corrupted entries (e.g. ASCII text overwriting tlb[]) would unmap
	// valid KSEG0/KSEG1 ranges, causing SIGBUS on ROM access.
	if (t.PageMask.UL & 0xFE001FFFu) {
		static u32 s_um_skip = 0;
		if (s_um_skip < 5) {
			Console.Warning("@@UNMAP_TLB_SKIP@@ n=%u i=%d pc=%08x PM=0x%08x EHi=%08x ELo0=%08x ELo1=%08x (corrupted, skip)",
				s_um_skip, i, cpuRegs.pc, t.PageMask.UL, t.EntryHi.UL, t.EntryLo0.UL, t.EntryLo1.UL);
			s_um_skip++;
		}
		return;
	}
	u32 mask, addr;
	u32 saddr, eaddr;

	if (t.isSPR())
	{
		// [iter216] Only unmap SPR at standard address (same guard as MapTLB).
		if (t.VPN2() == 0x70000000)
			vtlb_VMapUnmap(t.VPN2(), 0x4000);
		return;
	}

	if (t.EntryLo0.V)
	{
		mask = ((~t.Mask()) << 1) & 0xfffff;
		saddr = t.VPN2() >> 12;
		eaddr = saddr + t.Mask() + 1;
		//	Console.WriteLn("Clear TLB: %08x ~ %08x",saddr,eaddr-1);
		for (addr = saddr; addr < eaddr; addr++)
		{
			if ((addr & mask) == ((t.VPN2() >> 12) & mask))
			{ //match
				memClearPageAddr(addr << 12);
				Cpu->Clear(addr << 12, 0x400);
			}
		}
	}

	if (t.EntryLo1.V)
	{
		mask = ((~t.Mask()) << 1) & 0xfffff;
		saddr = (t.VPN2() >> 12) + t.Mask() + 1;
		eaddr = saddr + t.Mask() + 1;
		//	Console.WriteLn("Clear TLB: %08x ~ %08x",saddr,eaddr-1);
		for (addr = saddr; addr < eaddr; addr++)
		{
			if ((addr & mask) == ((t.VPN2() >> 12) & mask))
			{ //match
				memClearPageAddr(addr << 12);
				Cpu->Clear(addr << 12, 0x400);
			}
		}
	}

	for (size_t i = 0; i < cachedTlbs.count; i++)
	{
		if (cachedTlbs.PFN0s[i] == t.PFN0() && cachedTlbs.PFN1s[i] == t.PFN1() && cachedTlbs.PageMasks[i] == ConvertPageMask(t.PageMask.UL))
		{
			for (size_t j = i; j < cachedTlbs.count - 1; j++)
			{
				cachedTlbs.CacheEnabled0[j] = cachedTlbs.CacheEnabled0[j + 1];
				cachedTlbs.CacheEnabled1[j] = cachedTlbs.CacheEnabled1[j + 1];
				cachedTlbs.PFN0s[j] = cachedTlbs.PFN0s[j + 1];
				cachedTlbs.PFN1s[j] = cachedTlbs.PFN1s[j + 1];
				cachedTlbs.PageMasks[j] = cachedTlbs.PageMasks[j + 1];
			}
			cachedTlbs.count--;
			break;
		}
	}
}

void WriteTLB(int i)
{
	// [iter217] Debug: trace WriteTLB calls - total count + selected entries
	{
		static u32 s_wt_total = 0;
		static u32 s_wt_log = 0;
		s_wt_total++;
		const bool bad_pm = (cpuRegs.CP0.n.PageMask & 0xFE001FFFu) != 0;
		// Log first 5 entries, entries i>=30, entries with bad PageMask, and every 48th
		if ((i < 5 || i >= 30 || bad_pm || (s_wt_total % 48 == 0)) && s_wt_log < 50) {
			Console.WriteLn("@@WRITE_TLB_TRACE@@ n=%u tot=%u i=%d pc=%08x PM=%08x EHi=%08x ELo0=%08x ELo1=%08x | s0=%08x s1=%08x s2=%08x s3=%08x v0=%08x",
				s_wt_log, s_wt_total, i, cpuRegs.pc,
				cpuRegs.CP0.n.PageMask,
				cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1,
				cpuRegs.GPR.n.s0.UL[0], cpuRegs.GPR.n.s1.UL[0],
				cpuRegs.GPR.n.s2.UL[0], cpuRegs.GPR.n.s3.UL[0],
				cpuRegs.GPR.n.v0.UL[0]);
			s_wt_log++;
		}
	}
	tlb[i].PageMask.UL = cpuRegs.CP0.n.PageMask;
	tlb[i].EntryHi.UL = cpuRegs.CP0.n.EntryHi;
	tlb[i].EntryLo0.UL = cpuRegs.CP0.n.EntryLo0;
	tlb[i].EntryLo1.UL = cpuRegs.CP0.n.EntryLo1;

	// Setting the cache mode to reserved values is vaguely defined in the manual.
	// I found that SPR is set to cached regardless.
	// Non-SPR entries default to uncached on reserved cache modes.
	if (tlb[i].isSPR())
	{
		tlb[i].EntryLo0.C = 3;
		tlb[i].EntryLo1.C = 3;
	}
	else
	{
		if (!tlb[i].EntryLo0.isValidCacheMode())
			tlb[i].EntryLo0.C = 2;
		if (!tlb[i].EntryLo1.isValidCacheMode())
			tlb[i].EntryLo1.C = 2;
	}

	if (!tlb[i].isSPR() && cachedTlbs.count < 48 && ((tlb[i].EntryLo0.V && tlb[i].EntryLo0.isCached()) || (tlb[i].EntryLo1.V && tlb[i].EntryLo1.isCached())))
	{
		const size_t idx = cachedTlbs.count;
		cachedTlbs.CacheEnabled0[idx] = tlb[i].EntryLo0.isCached() ? ~0 : 0;
		cachedTlbs.CacheEnabled1[idx] = tlb[i].EntryLo1.isCached() ? ~0 : 0;
		cachedTlbs.PFN1s[idx] = tlb[i].PFN1();
		cachedTlbs.PFN0s[idx] = tlb[i].PFN0();
		cachedTlbs.PageMasks[idx] = ConvertPageMask(tlb[i].PageMask.UL);

		cachedTlbs.count++;
	}

	MapTLB(tlb[i], i);
}

// [P11] g_FrameCount / intCpu をファイルスコープで extern 宣言 (namespace 外で宣言しないとマングリング誤り)
#include "R5900.h"
extern uint g_FrameCount;
extern R5900cpu intCpu;
extern int g_sifgetreg_track; // [iter672] defined in R5900OpcodeImpl.cpp

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {
namespace COP0 {

	void TLBR()
	{
		COP0_LOG("COP0_TLBR %d:%x,%x,%x,%x",
			cpuRegs.CP0.n.Index, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
			cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);

		const u8 i = cpuRegs.CP0.n.Index & 0x3f;

		if (i > 47)
		{
			Console.Warning("TLBR with index > 47! (%d)", i);
			return;
		}

		cpuRegs.CP0.n.PageMask = tlb[i].PageMask.Mask << 13;
		cpuRegs.CP0.n.EntryHi = tlb[i].EntryHi.UL & ~((tlb[i].PageMask.Mask << 13) | 0x1f00);
		cpuRegs.CP0.n.EntryLo0 = tlb[i].EntryLo0.UL & ~(0xFC000000) & ~1;
		cpuRegs.CP0.n.EntryLo1 = tlb[i].EntryLo1.UL & ~(0x7C000000) & ~1;
		// "If both the Global bit of EntryLo0 and EntryLo1 are set to 1, the processor ignores the ASID during TLB lookup."
		// This is reflected during TLBR, where G is only set if both EntryLo0 and EntryLo1 are global.
		cpuRegs.CP0.n.EntryLo0 |= (tlb[i].EntryLo0.UL & 1) & (tlb[i].EntryLo1.UL & 1);
		cpuRegs.CP0.n.EntryLo1 |= (tlb[i].EntryLo0.UL & 1) & (tlb[i].EntryLo1.UL & 1);
	}



		void TLBWI()
		{
			if (s_tlbwi_probe_cfg < 0)
			{
				s_tlbwi_probe_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_TLBWI_PROBE", false) ? 1 : 0;
				Console.WriteLn("@@CFG@@ iPSX2_TLBWI_PROBE=%d", s_tlbwi_probe_cfg);
			}
			if (s_tlbwi_probe_cfg == 1 && !s_tlbwi_probe_wi_done)
			{
				if ((cpuRegs.CP0.n.EntryHi & 0xFFFFF000u) == 0x70004000u)
				{
					Console.Error("@@TLBWI_PROBE@@ op=TLBWI pc=%08x index=%08x entryHi=%08x entryLo0=%08x entryLo1=%08x pageMask=%08x badVAddr=%08x",
						cpuRegs.pc, cpuRegs.CP0.n.Index, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0,
						cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.BadVAddr);
					s_tlbwi_probe_wi_done = true;
				}
			}
			if (s_tlbwi_probe_cfg == 1 && s_tlbwi_probe_wi_count < 16)
			{
				Console.Error("@@TLBWI_TRACE@@ op=TLBWI n=%d pc=%08x index=%08x entryHi=%08x entryLo0=%08x entryLo1=%08x pageMask=%08x badVAddr=%08x",
					s_tlbwi_probe_wi_count, cpuRegs.pc, cpuRegs.CP0.n.Index, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0,
					cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.BadVAddr);
				s_tlbwi_probe_wi_count++;
			}
			const u8 j = cpuRegs.CP0.n.Index & 0x3f;

		if (j > 47)
		{
			static int s_tlbwi_oob_count = 0;
			if (s_tlbwi_oob_count < 1)
			{
				s_tlbwi_oob_count++;
				Console.Warning("TLBWI with index > 47! (%d) [further suppressed]", j);
			}
			return;
		}

		COP0_LOG("COP0_TLBWI %d:%x,%x,%x,%x",
			cpuRegs.CP0.n.Index, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
			cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);

		// iter35: @@TLBWI_IDX41@@ — 第2TLBloop(Index>=41)のcallをトレースしs1停滞をverify
		// Removal condition: loop脱出root causeafter determined
		{
			static int s_cfg = -1;
			static u32 s_n = 0;
			if (s_cfg < 0) {
				s_cfg = std::getenv("iPSX2_TLBWI_IDX41_PROBE") ? 1 : 0;
				Console.WriteLn("@@CFG@@ iPSX2_TLBWI_IDX41_PROBE=%d", s_cfg);
			}
			if (s_cfg == 1 && j >= 41 && s_n < 10) {
				Console.WriteLn("[TEMP_DIAG] @@TLBWI_IDX41@@ n=%u index=%u s1=%u pc=%08x",
					s_n, (unsigned)j, cpuRegs.GPR.r[17].UL[0], cpuRegs.pc);
				s_n++;
			}
		}

		UnmapTLB(tlb[j], j);
		WriteTLB(j);
	}

		void TLBWR()
		{
			if (s_tlbwi_probe_cfg < 0)
			{
				s_tlbwi_probe_cfg = iPSX2_GetRuntimeEnvBool("iPSX2_TLBWI_PROBE", false) ? 1 : 0;
				Console.WriteLn("@@CFG@@ iPSX2_TLBWI_PROBE=%d", s_tlbwi_probe_cfg);
			}
			if (s_tlbwi_probe_cfg == 1 && !s_tlbwi_probe_wr_done)
			{
				if ((cpuRegs.CP0.n.EntryHi & 0xFFFFF000u) == 0x70004000u)
				{
					Console.Error("@@TLBWI_PROBE@@ op=TLBWR pc=%08x random=%08x entryHi=%08x entryLo0=%08x entryLo1=%08x pageMask=%08x badVAddr=%08x",
						cpuRegs.pc, cpuRegs.CP0.n.Random, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0,
						cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.BadVAddr);
					s_tlbwi_probe_wr_done = true;
				}
			}
			if (s_tlbwi_probe_cfg == 1 && s_tlbwi_probe_wr_count < 16)
			{
				Console.Error("@@TLBWI_TRACE@@ op=TLBWR n=%d pc=%08x random=%08x entryHi=%08x entryLo0=%08x entryLo1=%08x pageMask=%08x badVAddr=%08x",
					s_tlbwi_probe_wr_count, cpuRegs.pc, cpuRegs.CP0.n.Random, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0,
					cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.BadVAddr);
				s_tlbwi_probe_wr_count++;
			}
			const u8 j = cpuRegs.CP0.n.Random & 0x3f;

		if (j > 47)
		{
			Console.Warning("TLBWR with random > 47! (%d)", j);
			return;
		}

		DevCon.Warning("COP0_TLBWR %d:%x,%x,%x,%x\n",
			cpuRegs.CP0.n.Random, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
			cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);

		UnmapTLB(tlb[j], j);
		WriteTLB(j);
	}

	void TLBP()
	{
		int i;

		union
		{
			struct
			{
				u32 VPN2 : 19;
				u32 VPN2X : 2;
				u32 G : 3;
				u32 ASID : 8;
			} s;
			u32 u;
		} EntryHi32;

		EntryHi32.u = cpuRegs.CP0.n.EntryHi;

		cpuRegs.CP0.n.Index = 0xFFFFFFFF;
		for (i = 0; i < 48; i++)
		{
			if (tlb[i].VPN2() == ((~tlb[i].Mask()) & (EntryHi32.s.VPN2)) && ((tlb[i].isGlobal()) || ((tlb[i].EntryHi.ASID & 0xff) == EntryHi32.s.ASID)))
			{
				cpuRegs.CP0.n.Index = i;
				break;
			}
		}
		if (cpuRegs.CP0.n.Index == 0xFFFFFFFF)
			cpuRegs.CP0.n.Index = 0x80000000;
	}

	void MFC0()
	{
		// Note on _Rd_ Condition 9: CP0.Count should be updated even if _Rt_ is 0.
		if ((_Rd_ != 9) && !_Rt_)
			return;

		//if(bExecBIOS == FALSE && _Rd_ == 25) Console.WriteLn("MFC0 _Rd_ %x = %x", _Rd_, cpuRegs.CP0.r[_Rd_]);
		switch (_Rd_)
		{
			case 12:
				cpuRegs.GPR.r[_Rt_].SD[0] = (s32)(cpuRegs.CP0.r[_Rd_] & 0xf0c79c1f);
				break;

			case 25:
				if (0 == (_Imm_ & 1)) // MFPS, register value ignored
				{
					cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.PERF.n.pccr.val;
				}
				else if (0 == (_Imm_ & 2)) // MFPC 0, only LSB of register matters
				{
					COP0_UpdatePCCR();
					cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.PERF.n.pcr0;
				}
				else // MFPC 1
				{
					COP0_UpdatePCCR();
					cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.PERF.n.pcr1;
				}
				/*Console.WriteLn("MFC0 PCCR = %x PCR0 = %x PCR1 = %x IMM= %x",  params
cpuRegs.PERF.n.pccr, cpuRegs.PERF.n.pcr0, cpuRegs.PERF.n.pcr1, _Imm_ & 0x3F);*/
				break;

			case 24:
				COP0_LOG("MFC0 Breakpoint debug Registers code = %x", cpuRegs.code & 0x3FF);
				break;

			case 9:
			{
				u32 incr = cpuRegs.cycle - cpuRegs.lastCOP0Cycle;
				if (incr == 0)
					incr++;
				cpuRegs.CP0.n.Count += incr;
				cpuRegs.lastCOP0Cycle = cpuRegs.cycle;
				if (!_Rt_)
					break;
			}
				[[fallthrough]];

			default:
				cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.CP0.r[_Rd_];
		}
	}

	void MTC0()
	{
		//if(bExecBIOS == FALSE && _Rd_ == 25) Console.WriteLn("MTC0 _Rd_ %x = %x", _Rd_, cpuRegs.CP0.r[_Rd_]);
		switch (_Rd_)
		{
			case 9:
				cpuRegs.lastCOP0Cycle = cpuRegs.cycle;
				cpuRegs.CP0.r[9] = cpuRegs.GPR.r[_Rt_].UL[0];
				break;

			case 12:
				WriteCP0Status(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 16:
				WriteCP0Config(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 24:
				COP0_LOG("MTC0 Breakpoint debug Registers code = %x", cpuRegs.code & 0x3FF);
				break;

			case 25:
				/*if(bExecBIOS == FALSE && _Rd_ == 25) Console.WriteLn("MTC0 PCCR = %x PCR0 = %x PCR1 = %x IMM= %x", params
	cpuRegs.PERF.n.pccr, cpuRegs.PERF.n.pcr0, cpuRegs.PERF.n.pcr1, _Imm_ & 0x3F);*/
				if (0 == (_Imm_ & 1)) // MTPS
				{
					if (0 != (_Imm_ & 0x3E)) // only effective when the register is 0
						break;
					// Updates PCRs and sets the PCCR.
					COP0_UpdatePCCR();
					cpuRegs.PERF.n.pccr.val = cpuRegs.GPR.r[_Rt_].UL[0];
					COP0_DiagnosticPCCR();
				}
				else if (0 == (_Imm_ & 2)) // MTPC 0, only LSB of register matters
				{
					cpuRegs.PERF.n.pcr0 = cpuRegs.GPR.r[_Rt_].UL[0];
					cpuRegs.lastPERFCycle[0] = cpuRegs.cycle;
				}
				else // MTPC 1
				{
					cpuRegs.PERF.n.pcr1 = cpuRegs.GPR.r[_Rt_].UL[0];
					cpuRegs.lastPERFCycle[1] = cpuRegs.cycle;
				}
				break;

			default:
				cpuRegs.CP0.r[_Rd_] = cpuRegs.GPR.r[_Rt_].UL[0];
				break;
		}
	}

	int CPCOND0()
	{
		return (((dmacRegs.stat.CIS | ~dmacRegs.pcr.CPC) & 0x3FF) == 0x3ff);
	}

	//#define CPCOND0	1

	void BC0F()
	{
		if (CPCOND0() == 0)
			intDoBranch(_BranchTarget_);
	}

	void BC0T()
	{
		if (CPCOND0() == 1)
			intDoBranch(_BranchTarget_);
	}

	void BC0FL()
	{
		if (CPCOND0() == 0)
			intDoBranch(_BranchTarget_);
		else
			cpuRegs.pc += 4;
	}

	void BC0TL()
	{
		if (CPCOND0() == 1)
			intDoBranch(_BranchTarget_);
		else
			cpuRegs.pc += 4;
	}

	void ERET()
	{
        // [iter672] @@ERET_SIFGETREG@@ sceSifGetReg(0x80000002) 戻り値キャプチャ
        // Removal condition: sceSifGetReg 戻り値差異のroot causeafter identified
        {
            // sceSifGetReg(4) ERET return value tracking
            if (::g_sifgetreg_track == 4) {
                ::g_sifgetreg_track = 0;
                static int s_reg4_eret_n = 0;
                static u32 s_last_v0 = 0xFFFFFFFF;
                s_reg4_eret_n++;
                // ログ: 値が変化した時、または最初の5回、または1000回ごと
                u32 cur_v0 = cpuRegs.GPR.n.v0.UL[0];
                if (cur_v0 != s_last_v0 || s_reg4_eret_n <= 5 || (s_reg4_eret_n % 10000) == 0) {
                    Console.WriteLn("@@ERET_SIFGETREG4@@ n=%d v0=%08x epc=%08x cycle=%u",
                        s_reg4_eret_n, cur_v0,
                        cpuRegs.CP0.n.Status.b.ERL ? cpuRegs.CP0.n.ErrorEPC : cpuRegs.CP0.n.EPC,
                        cpuRegs.cycle);
                    s_last_v0 = cur_v0;
                }
            }
            if (::g_sifgetreg_track == 1) {
                ::g_sifgetreg_track = 0;
                static int s_sifget_eret_n = 0;
                if (s_sifget_eret_n < 5) {
                    const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
                    const u32 _epc = cpuRegs.CP0.n.Status.b.ERL ? cpuRegs.CP0.n.ErrorEPC : cpuRegs.CP0.n.EPC;
                    const u32 buf_ptr_addr = 0x93698u & 0x01FFFFFFu;
                    const u32 buf_ptr = *(u32*)(eeMem->Main + buf_ptr_addr);
                    u32 bd[4] = {};
                    if (buf_ptr) {
                        const u32 bp = (buf_ptr & 0x01FFFFFFu);
                        if (bp + 16 <= 0x02000000u) {
                            bd[0] = *(u32*)(eeMem->Main + bp);
                            bd[1] = *(u32*)(eeMem->Main + bp + 4);
                            bd[2] = *(u32*)(eeMem->Main + bp + 8);
                            bd[3] = *(u32*)(eeMem->Main + bp + 12);
                        }
                    }
                    Console.WriteLn("@@ERET_SIFGETREG@@ [%s] n=%d epc=%08x v0=%08x v1=%08x a0=%08x smflg=%08x buf=%08x *buf=%08x_%08x_%08x_%08x",
                        mode, s_sifget_eret_n++, _epc,
                        cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
                        cpuRegs.GPR.n.a0.UL[0], psHu32(0xF230),
                        buf_ptr, bd[0], bd[1], bd[2], bd[3]);
                }
            }
        }
        // [P11] @@ERET_BEFORE@@ — ERET 直前の Status キャプチャ (vsync 4-6, JIT only, EELOAD area EPC)
        // 目的: カーネルhandler実行後 ERET 直前の Status が正しいかverify
        // Removal condition: JIT Status corruptcauseafter identified
        {
            static const bool s_ckpt_en = iPSX2_GetRuntimeEnvBool("iPSX2_CHECKPOINT_LOG", false);
            const u32 _epc = cpuRegs.CP0.n.Status.b.ERL ? cpuRegs.CP0.n.ErrorEPC : cpuRegs.CP0.n.EPC;
            if (s_ckpt_en && Cpu != &intCpu && (_epc < 0x00200000u || _epc >= 0x9fc00000u)) // EELOAD/kernel + BIOS ROM
            {
                static int s_n = 0;
                if (s_n < 20) {
                    Console.WriteLn("@@ERET_BEFORE@@ n=%d vsync=%u epc=%08x erl=%d status=%08x",
                        s_n++, g_FrameCount, _epc,
                        (int)cpuRegs.CP0.n.Status.b.ERL, cpuRegs.CP0.n.Status.val);
                }
            }
        }

#ifdef ENABLE_VTUNE
		// Allow to stop vtune in a predictable way to compare runs
		// Of course, the limit will depend on the game.
		const u32 million = 1000 * 1000;
		static u32 vtune = 0;
		vtune++;

		// quick_exit vs exit: quick_exit won't call static storage destructor (OS will manage). It helps
		// avoiding the race condition between threads destruction.
		if (vtune > 30 * million)
		{
			Console.WriteLn("VTUNE: quick_exit");
			std::quick_exit(EXIT_SUCCESS);
		}
		else if (!(vtune % million))
		{
			Console.WriteLn("VTUNE: ERET was called %uM times", vtune / million);
		}

#endif

		if (cpuRegs.CP0.n.Status.b.ERL)
		{
			cpuRegs.pc = cpuRegs.CP0.n.ErrorEPC;
			cpuRegs.CP0.n.Status.b.ERL = 0;
		}
		else
		{
			cpuRegs.pc = cpuRegs.CP0.n.EPC;
			cpuRegs.CP0.n.Status.b.EXL = 0;
		}

        // [TEMP_DIAG] @@ERET_TO_NULL@@ — ERET to PC < 0x1000 detection
        // Removal condition: PC=0 到達causeafter identified
        if (cpuRegs.pc < 0x1000u) {
            static int s_eret_null_n = 0;
            if (s_eret_null_n++ < 5) {
                Console.WriteLn("@@ERET_TO_NULL@@ n=%d retpc=%08x EPC=%08x ErrorEPC=%08x Status=%08x Cause=%08x ra=%08x sp=%08x v1=%08x cycle=%u",
                    s_eret_null_n, cpuRegs.pc,
                    cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.ErrorEPC,
                    cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause,
                    cpuRegs.GPR.n.ra.UL[0], cpuRegs.GPR.n.sp.UL[0],
                    cpuRegs.GPR.n.v1.UL[0], cpuRegs.cycle);
                // Dump kernel stack at 0x80018E70 (positive SYSCALL frame)
                if (eeMem) {
                    u32* stk = (u32*)(eeMem->Main + 0x00018E70u);
                    Console.WriteLn("@@ERET_TO_NULL_STK@@ [18E70]: ra=%08x old_sp=%08x epc4=%08x [+C]=%08x [+10]=%08x [+14]=%08x",
                        stk[0], stk[1], stk[2], stk[3], stk[4], stk[5]);
                    // Also dump negative SYSCALL frame at current sp
                    u32 sp_phys = cpuRegs.GPR.n.sp.UL[0] & 0x01FFFFFFu;
                    if (sp_phys + 0x20 < 0x02000000u) {
                        u32* nstk = (u32*)(eeMem->Main + sp_phys);
                        Console.WriteLn("@@ERET_TO_NULL_NSTK@@ [sp=%08x]: %08x %08x %08x %08x %08x %08x %08x %08x",
                            cpuRegs.GPR.n.sp.UL[0], nstk[0], nstk[1], nstk[2], nstk[3], nstk[4], nstk[5], nstk[6], nstk[7]);
                    }
                }
            }
        }

        // [iter659] @@ERET_EELOAD@@ — ERET returns to EELOAD area (0x82000-0x90000)
        // 目的: LoadExecPS2 SYSCALL 後に JIT/Interp がどの PC に戻るか比較
        // Removal condition: EELOAD SYSCALL sequence divergence root cause after identified
        {
            const u32 phys_pc = cpuRegs.pc & 0x1FFFFFFFu;
            if (phys_pc >= 0x00082000u && phys_pc < 0x00090000u) {
                static int s_eret_eeload_n = 0;
                if (s_eret_eeload_n < 20) {
                    const char* mode = (Cpu != &intCpu) ? "JIT" : "Interp";
                    Console.WriteLn("@@ERET_EELOAD@@ [%s] n=%d pc=%08x epc=%08x status=%08x v0=%08x v1=%08x a0=%08x ra=%08x sp=%08x",
                        mode, s_eret_eeload_n++, cpuRegs.pc,
                        cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.Status.val,
                        cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
                        cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.ra.UL[0],
                        cpuRegs.GPR.n.sp.UL[0]);
                }
            }
        }

		cpuUpdateOperationMode();
		cpuSetNextEventDelta(4);
		intSetBranch();
	}

	void DI()
	{
		if (cpuRegs.CP0.n.Status.b._EDI || cpuRegs.CP0.n.Status.b.EXL ||
			cpuRegs.CP0.n.Status.b.ERL || (cpuRegs.CP0.n.Status.b.KSU == 0))
		{
			cpuRegs.CP0.n.Status.b.EIE = 0;
			// IRQs are disabled so no need to do a cpu exception/event test...
			//cpuSetNextEventDelta();
		}
	}

	void EI()
	{
		// [iter_EI_PROBE] @@EE_EI_CALL@@: log EI call context (cap=30)
		// Removal condition: EIcallとstatus変化がJIT/Interpreter両modeでverifyできたら
		{
			static u32 s_ei_count = 0;
			if (s_ei_count < 30) {
				++s_ei_count;
				const bool jit_mode = (Cpu != &intCpu);
				Console.WriteLn("@@EE_EI_CALL@@ n=%u [%s] pc=%08x status=%08x edi=%d exl=%d erl=%d ksu=%d eie=%d ie=%d",
					s_ei_count, jit_mode ? "JIT" : "Interp",
					cpuRegs.pc, cpuRegs.CP0.n.Status.val,
					(int)cpuRegs.CP0.n.Status.b._EDI,
					(int)cpuRegs.CP0.n.Status.b.EXL,
					(int)cpuRegs.CP0.n.Status.b.ERL,
					(int)cpuRegs.CP0.n.Status.b.KSU,
					(int)cpuRegs.CP0.n.Status.b.EIE,
					(int)cpuRegs.CP0.n.Status.b.IE);
			}
		}
		if (cpuRegs.CP0.n.Status.b._EDI || cpuRegs.CP0.n.Status.b.EXL ||
			cpuRegs.CP0.n.Status.b.ERL || (cpuRegs.CP0.n.Status.b.KSU == 0))
		{
			cpuRegs.CP0.n.Status.b.EIE = 1;
			// schedule an event test, which will check for and raise pending IRQs.
			cpuSetNextEventDelta(4);
		}
	}

} // namespace COP0
} // namespace OpcodeImpl
} // namespace Interpreter
} // namespace R5900
