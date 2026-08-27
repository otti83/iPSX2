// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Gif_Unit.h"
#include "Hardware.h"
#include "SPU2/spu2.h"
#include "USB/USB.h"

#include "common/WrappedMemCopy.h"

#include "fmt/format.h"

using namespace R5900;

extern "C" void LogUnified(const char* fmt, ...);

HwRegRingBuffer g_HwRegRing;
const int HwRegRingBuffer::SIZE;

HwRegRingBuffer::HwRegRingBuffer() : idx(0), write_count(0), read_count(0), push_count(0) {
    Console.WriteLn("@@HWMON_BUILD@@ file=Hw.cpp line=%d", __LINE__);
}

void HwRegRingBuffer::Push(u32 pc, u32 vaddr, u32 paddr, u32 width, u64 val, bool is_write) {
    // Filter: MMIO (0x1000xxxx), BIOS regs (0x1F80xxxx), or typical Handler mapped area (0xB...)
    if ((paddr < 0x10000000 || paddr > 0x1001FFFF) && 
        (paddr < 0x1F800000 || paddr > 0x1F80FFFF) &&
        (vaddr < 0xB0000000 || vaddr > 0xBFFFFFFF)) {
        return;
    }

    static bool once = false;
    if (!once) {
        Console.WriteLn("@@HWMON_PATH@@ HwRegRingBuffer::Push passed filter! paddr=%x RW=%d", paddr, is_write);
        once = true;
    }

    int i = idx;
    entries[i].caller_pc = pc;
    entries[i].vaddr = vaddr;
    entries[i].paddr = paddr;
    entries[i].width = width;
    entries[i].val = val;
    entries[i].is_write = is_write;
    
    idx = (i + 1) % SIZE;
    push_count++;
    if (is_write) write_count++;
    else read_count++;
}

void HwRegRingBuffer::DumpLast(int n) {
    int total = push_count; // Atomic load
    int count = std::min(total, std::min(n, SIZE));
    int start = (idx - count + SIZE) % SIZE;
    
    for (int i = 0; i < count; i++) {
        int curr = (start + i) % SIZE;
        const HwRegEntry& e = entries[curr];
        Console.WriteLn("@@HWREG@@ i=%02d caller_pc=%08x paddr=%08x vaddr=%08x w=%u val=%016llx RD=%d",
            i, e.caller_pc, e.paddr, e.vaddr, e.width, e.val, !e.is_write);
    }
}

void HwRegRingBuffer::DumpStats() {
    struct Stat { u32 paddr; int hits; bool is_write; u64 val; };
    Stat r_stats[SIZE];
    Stat w_stats[SIZE];
    int r_count = 0;
    int w_count = 0;
    
    int total_limit = std::min((int)push_count, (int)SIZE);
    
    // Iterate ring buffer
    for (int i = 0; i < total_limit; i++) {
        int idx_ring = (idx - 1 - i + SIZE) % SIZE;
        const HwRegEntry& e = entries[idx_ring];
        
        if (e.is_write) {
            bool found = false;
            for (int j=0; j<w_count; j++) {
                if (w_stats[j].paddr == e.paddr) {
                    w_stats[j].hits++;
                    w_stats[j].val = e.val; // Update latest value
                    found = true;
                    break;
                }
            }
            if (!found) {
                w_stats[w_count] = { e.paddr, 1, true, e.val };
                w_count++;
            }
        } else {
            bool found = false;
            for (int j=0; j<r_count; j++) {
                if (r_stats[j].paddr == e.paddr) {
                    r_stats[j].hits++;
                    r_stats[j].val = e.val; // Update latest value
                    found = true;
                    break;
                }
            }
            if (!found) {
                r_stats[r_count] = { e.paddr, 1, false, e.val };
                r_count++;
            }
        }
    }
    
    // Sort Reads
    for (int i = 0; i < r_count - 1; i++) {
        for (int j = 0; j < r_count - i - 1; j++) {
            if (r_stats[j].hits < r_stats[j+1].hits) {
                Stat temp = r_stats[j];
                r_stats[j] = r_stats[j+1];
                r_stats[j+1] = temp;
            }
        }
    }
    // Sort Writes
    for (int i = 0; i < w_count - 1; i++) {
        for (int j = 0; j < w_count - i - 1; j++) {
            if (w_stats[j].hits < w_stats[j+1].hits) {
                Stat temp = w_stats[j];
                w_stats[j] = w_stats[j+1];
                w_stats[j+1] = temp;
            }
        }
    }
    
    // Print READ Top 8
    if (r_count == 0) {
        Console.WriteLn("@@HWREG_TOP@@ READ <none> total=%d", (int)push_count);
    } else {
        for (int i = 0; i < r_count && i < 8; i++) {
             Console.WriteLn("@@HWREG_TOP@@ READ #%d paddr=%08x hits=%d last_val=%08x", 
                i+1, r_stats[i].paddr, r_stats[i].hits, (u32)r_stats[i].val);
        }
    }
    
    // Print WRITE Top 8
    if (w_count == 0) {
        Console.WriteLn("@@HWREG_TOP@@ WRITE <none> total=%d", (int)push_count);
    } else {
        for (int i = 0; i < w_count && i < 8; i++) {
             Console.WriteLn("@@HWREG_TOP@@ WRITE #%d paddr=%08x hits=%d last_val=%08x", 
                i+1, w_stats[i].paddr, w_stats[i].hits, (u32)w_stats[i].val);
        }
    }
}

const int rdram_devices = 2;	// put 8 for TOOL and 2 for PS2 and PSX
int rdram_sdevid = 0;

void hwReset()
{
    Console.WriteLn("@@HWMON_BUILD@@ file=Hw.cpp func=hwReset line=%d", __LINE__);
	std::memset(eeHw, 0, sizeof(eeHw));

	psHu32(SBUS_F260) = 0x1D000060;

	// i guess this is kinda a version, it's used by some bioses
	psHu32(DMAC_ENABLEW) = 0x1201;
	psHu32(DMAC_ENABLER) = 0x1201;

	rcntInit();

	// Sets SPU2 sample rate to PS2 standard (48KHz) whenever emulator is reset.
	// For PSX mode sample rate setting, see HwWrite.cpp
	SPU2::Reset(false);

	sifReset();
	gsReset();
	gifUnit.Reset();
	ipuReset();
	vif0Reset();
	vif1Reset();
	gif_fifo.init();
	USBreset();
}

__fi uint intcInterrupt()
{
	if ((psHu32(INTC_STAT)) == 0) {
		//DevCon.Warning("*PCSX2*: intcInterrupt already cleared");
		return 0;
	}
	if ((psHu32(INTC_STAT) & psHu32(INTC_MASK)) == 0)
	{
		//DevCon.Warning("*PCSX2*: No valid interrupt INTC_MASK: %x INTC_STAT: %x", psHu32(INTC_MASK), psHu32(INTC_STAT));
		return 0;
	}

	HW_LOG("intcInterrupt %x", psHu32(INTC_STAT) & psHu32(INTC_MASK));
	if(psHu32(INTC_STAT) & 0x2){
		counters[0].hold = rcntRcount(0);
		counters[1].hold = rcntRcount(1);
	}

	//cpuException(0x400, cpuRegs.branch);
	return 0x400;
}

__fi uint dmacInterrupt()
{
	if( ((psHu16(DMAC_STAT + 2) & psHu16(DMAC_STAT)) == 0 ) &&
		( psHu16(DMAC_STAT) & 0x8000) == 0 )
	{
		//DevCon.Warning("No valid DMAC interrupt MASK %x STAT %x", psHu16(DMAC_STAT+2), psHu16(DMAC_STAT));
		return 0;
	}

	if (!dmacRegs.ctrl.DMAE || psHu8(DMAC_ENABLER+2) == 1)
	{
		//DevCon.Warning("DMAC Suspended or Disabled on interrupt");
		return 0;
	}

	DMA_LOG("dmacInterrupt %x",
		((psHu16(DMAC_STAT + 2) & psHu16(DMAC_STAT)) |
		 (psHu16(DMAC_STAT) & 0x8000))
	);

	//cpuException(0x800, cpuRegs.branch);
	return 0x800;
}

void hwIntcIrq(int n)
{
	psHu32(INTC_STAT) |= 1<<n;
	static int sbus_log_count = 0;
	if (n == INTC_SBUS && sbus_log_count < 8)
	{
		LogUnified("@@SBUS_IRQ@@ n=%d stat=%08x mask=%08x\n", n, psHu32(INTC_STAT), psHu32(INTC_MASK));
		sbus_log_count++;
	}
	// [TEMP_DIAG] @@INTC_VBLANK@@
	if (n == INTC_VBLANK_S) {
		static int s_vblank_irq_n = 0;
		if (s_vblank_irq_n < 10 || (s_vblank_irq_n % 100 == 0 && s_vblank_irq_n < 500)) {
			u32 stat = psHu32(INTC_STAT);
			u32 mask = psHu32(INTC_MASK);
			bool will_fire = (mask & (1 << n)) != 0;
			Console.WriteLn("@@INTC_VBLANK@@ n=%d stat=%08x mask=%08x will_fire=%d ee_pc=%08x",
				s_vblank_irq_n, stat, mask, will_fire ? 1 : 0, cpuRegs.pc);
		}
		s_vblank_irq_n++;
	}
	if(psHu32(INTC_MASK) & (1<<n))cpuTestINTCInts();
}

void hwDmacIrq(int n)
{
	psHu32(DMAC_STAT) |= 1<<n;
	if(psHu16(DMAC_STAT+2) & (1<<n))cpuTestDMACInts();
}

void FireMFIFOEmpty()
{
	SPR_LOG("MFIFO Data Empty");
	hwDmacIrq(DMAC_MFIFO_EMPTY);

	if (dmacRegs.ctrl.MFD == MFD_VIF1) vif1Regs.stat.FQC = 0;
	if (dmacRegs.ctrl.MFD == MFD_GIF)  gifRegs.stat.FQC  = 0;
}

// Write 'size' bytes to memory address 'addr' from 'data'.
__ri bool hwMFIFOWrite(u32 addr, const u128* data, uint qwc)
{
	// all FIFO addresses should always be QWC-aligned.
	pxAssert((dmacRegs.rbor.ADDR & 15) == 0);
	pxAssert((addr & 15) == 0);

	if(qwc > ((dmacRegs.rbsr.RMSK + 16u) >> 4u)) DevCon.Warning("MFIFO Write bigger than MFIFO! QWC=%x FifoSize=%x", qwc, ((dmacRegs.rbsr.RMSK + 16) >> 4));
	// DMAC Address resolution:  FIFO can be placed anywhere in the *physical* memory map
	// for the PS2.  Its probably a serious error for a PS2 app to have the buffer cross
	// valid/invalid page areas of ram, so realistically we only need to test the base address
	// of the FIFO for address validity.

	if (u128* dst = (u128*)PSM(dmacRegs.rbor.ADDR))
	{
		const u32 ringsize = (dmacRegs.rbsr.RMSK / 16) + 1;
		pxAssertMsg( PSM(dmacRegs.rbor.ADDR+ringsize-1) != NULL, "Scratchpad/MFIFO ringbuffer spans into invalid (unmapped) physical memory!" );
		uint startpos = (addr & dmacRegs.rbsr.RMSK)/16;
		MemCopy_WrappedDest( data, dst, startpos, ringsize, qwc );
	}
	else
	{
		SPR_LOG( "Scratchpad/MFIFO: invalid base physical address: 0x%08x", dmacRegs.rbor.ADDR );
		pxFail( fmt::format( "Scratchpad/MFIFO: Invalid base physical address: 0x{:08x}", u32(dmacRegs.rbor.ADDR)).c_str() );
		return false;
	}

	return true;
}

__ri void hwMFIFOResume() {

	switch (dmacRegs.ctrl.MFD)
	{
		case MFD_VIF1: // Most common case.
		{
			SPR_LOG("Resuming VIF1 MFIFO, Vif CHCR %x Stalled %x done %x", vif1ch.chcr._u32, vif1.vifstalled.enabled, vif1.done);
			if (vif1.inprogress & 0x10)
			{
				vif1.inprogress &= ~0x10;
				//Don't resume if stalled or already looping
				if (vif1ch.chcr.STR && !(cpuRegs.interrupt & (1 << DMAC_MFIFO_VIF)) && !vif1Regs.stat.INT)
				{
					SPR_LOG("Data Added, Resuming");
					//Need to simulate the time it takes to copy here, if the VIF resumes before the SPR has finished, it isn't happy.
					CPU_INT(DMAC_MFIFO_VIF, cpuRegs.eCycle[DMAC_FROM_SPR]);
				}

				//Apparently this is bad, i guess so, the data is going to memory rather than the FIFO
				//vif1Regs.stat.FQC = 0x10; // FQC=16
			}
			break;
		}
		case MFD_GIF:
		{
			SPR_LOG("Resuming GIF MFIFO, Gif CHCR %x done %x", gifch.chcr._u32, gif.gspath3done);
			if ((gif.gifstate & GIF_STATE_EMPTY)) {
				CPU_INT(DMAC_MFIFO_GIF, cpuRegs.eCycle[DMAC_FROM_SPR]);
				gif.gifstate = GIF_STATE_READY;
			}
			break;
		}
		default:
			break;
	}
}

__ri bool hwDmacSrcChainWithStack(DMACh& dma, int id) {
	switch (id) {
		case TAG_REFE: // Refe - Transfer Packet According to ADDR field
			dma.tadr += 16;
            //End Transfer
			return true;

		case TAG_CNT: // CNT - Transfer QWC following the tag.
            // Set MADR to QW afer tag, and set TADR to QW following the data.
			dma.tadr += 16;
			dma.madr = dma.tadr;
			//dma.tadr = dma.madr + (dma.qwc << 4);
			return false;

		case TAG_NEXT: // Next - Transfer QWC following tag. TADR = ADDR
		{
		    // Set MADR to QW following the tag, and set TADR to the address formerly in MADR.
			u32 temp = dma.madr;
			dma.madr = dma.tadr + 16;
			dma.tadr = temp;
			return false;
		}
		case TAG_REF: // Ref - Transfer QWC from ADDR field
		case TAG_REFS: // Refs - Transfer QWC from ADDR field (Stall Control)
            //Set TADR to next tag
			dma.tadr += 16;
			return false;

		case TAG_CALL: // Call - Transfer QWC following the tag, save succeeding tag
		{
		    // Store the address in MADR in temp, and set MADR to the data following the tag.
			u32 temp = dma.madr;
			dma.madr = dma.tadr + 16;

			// Stash an address on the address stack pointer.
			switch(dma.chcr.ASP)
            {
                case 0: //Check if ASR0 is empty
                    // Store the succeeding tag in asr0, and mark chcr as having 1 address.
                    dma.asr0 = dma.madr + (dma.qwc << 4);
                    dma.chcr.ASP++;
                    break;

                case 1:
                    // Store the succeeding tag in asr1, and mark chcr as having 2 addresses.
                    dma.asr1 = dma.madr + (dma.qwc << 4);
                    dma.chcr.ASP++;
                    break;

                default:
                    Console.Warning("Call Stack Overflow (report if it fixes/breaks anything)");
                    return true;
			}

			// Set TADR to the address from MADR we stored in temp.
			dma.tadr = temp;

			return false;
		}

		case TAG_RET: // Ret - Transfer QWC following the tag, load next tag
            //Set MADR to data following the tag.
			dma.madr = dma.tadr + 16;

			// Snag an address from the address stack pointer.
			switch(dma.chcr.ASP)
            {
                case 2:
                    // Pull asr1 from the stack, give it to TADR, and decrease the # of addresses.
                    dma.tadr = dma.asr1;
                    dma.asr1 = 0;
                    dma.chcr.ASP--;
                    break;

                case 1:
                    // Pull asr0 from the stack, give it to TADR, and decrease the # of addresses.
                    dma.tadr = dma.asr0;
                    dma.asr0 = 0;
                    dma.chcr.ASP--;
                    break;

                case 0:
                    // There aren't any addresses to pull, so end the transfer.
                    //dma.tadr += 16;						   //Clear tag address - Kills Klonoa 2
                    return true;

                default:
                    // If ASR1 and ASR0 are messed up, end the transfer.
                    //Console.Error("TAG_RET: ASR 1 & 0 == 1. This shouldn't happen!");
                    //dma.tadr += 16;						   //Clear tag address - Kills Klonoa 2
                    return true;
            }
			return false;

		case TAG_END: // End - Transfer QWC following the tag
            //Set MADR to data following the tag, and end the transfer.
			dma.madr = dma.tadr + 16;
			//Don't Increment tadr; breaks Soul Calibur II and III
			return true;
	}

	return false;
}


/********TADR NOTES***********
From what i've gathered from testing tadr increment stuff (with CNT) is that we might not be 100% accurate in what
increments it and what doesnt. Previously we presumed REFE and END didn't increment the tag, but SIF and IPU never
liked this.

From what i've deduced, REFE does in fact increment, but END doesn't, after much testing, i've concluded this is how
we can standardize DMA chains, so i've modified the code to work like this.   The below function controls the increment
of the TADR along with the MADR on VIF, GIF and SPR1 when using the CNT tag, the others don't use it yet, but they
can probably be modified to do so now.

Reason for this:- Many games  (such as clock tower 3 and FFX Videos) watched the TADR to see when a transfer has finished,
so we need to simulate this wherever we can!  Even the FFX video gets corruption and tries to fire multiple DMA Kicks
if this doesnt happen, which was the reasoning for the hacked up SPR timing we had, that is no longer required.

-Refraction
******************************/

void hwDmacSrcTadrInc(DMACh& dma)
{
	//Don't touch it if in normal/interleave mode.
	if (dma.chcr.STR == 0) return;
	if (dma.chcr.MOD != 1) return;

	u16 tagid = (dma.chcr.TAG >> 12) & 0x7;

	if (tagid == TAG_CNT)
	{
		dma.tadr = dma.madr;
	}
}
bool hwDmacSrcChain(DMACh& dma, int id)
{
	u32 temp;

	switch (id)
	{
		case TAG_REFE: // Refe - Transfer Packet According to ADDR field
			dma.tadr += 16;
            // End the transfer.
			return true;

		case TAG_CNT: // CNT - Transfer QWC following the tag.
            // Set MADR to QW after the tag, and TADR to QW following the data.
			dma.madr = dma.tadr + 16;
			dma.tadr = dma.madr;
			return false;

		case TAG_NEXT: // Next - Transfer QWC following tag. TADR = ADDR
            // Set MADR to QW following the tag, and set TADR to the address formerly in MADR.
			temp = dma.madr;
			dma.madr = dma.tadr + 16;
			dma.tadr = temp;
			return false;

		case TAG_REF: // Ref - Transfer QWC from ADDR field
		case TAG_REFS: // Refs - Transfer QWC from ADDR field (Stall Control)
            //Set TADR to next tag
			dma.tadr += 16;
			return false;

		case TAG_END: // End - Transfer QWC following the tag
            //Set MADR to data following the tag, and end the transfer.
			dma.madr = dma.tadr + 16;
			//Don't Increment tadr; breaks Soul Calibur II and III
			return true;
		// Undefined Tag handling ends the DMA, maintaining the bad TADR and Tag in upper CHCR
		// Some games such as DT racer try to use RET tags on IPU, which it doesn't support
		default:
			return true;
	}

	return false;
}
