// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "IopHw_Internal.h"
#include "Sif.h"
#include "SIO/Sio2.h"
#include "SIO/Sio0.h"
#include "CDVD/Ps1CD.h"
#include "FW.h"
#include "SPU2/spu2.h"
#include "DEV9/DEV9.h"
#include <cstdio>
#include "USB/USB.h"
#include "IopCounters.h"
#include "IopDma.h"
#include "R5900.h" // For cpuRegs/verification
#include <cstdio> // For fprintf, stderr

// [DIAGNOSIS] Test Hook for 1f803204
// Controlled by iR5900.cpp recEventTest
int g_iop3204_test_val = -1;

#include "ps2/pgif.h"
#include "Mdec.h"

#define SIO0LOG_ENABLE 0
#define SIO2LOG_ENABLE 0

#define Sio0Log if (SIO0LOG_ENABLE) DevCon
#define Sio2Log if (SIO2LOG_ENABLE) DevCon

namespace IopMemory
{
using namespace Internal;

// Template-compatible version of the psxHu macro.  Used for reading.
#define psxHu(mem)	(*(u32*)&iopHw[(mem) & 0xffff])

//////////////////////////////////////////////////////////////////////////////////////////
//
mem8_t iopHwRead8_Page1( u32 addr )
{
	// all addresses are assumed to be prefixed with 0x1f801xxx:
	pxAssume( (addr >> 12) == 0x1f801 );

	const u32 masked_addr = addr & 0x0fff;

	mem8_t ret = 0; // using a return var can be helpful in debugging.
	switch( masked_addr )
	{
		case (HW_SIO_DATA & 0x0fff):
			ret = g_Sio0.GetRxData();
			break;
		case (HW_SIO_STAT & 0x0fff):
			Sio0Log.Error("%s(%08X) Unexpected SIO0 STAT 8 bit read", __FUNCTION__, addr);
			break;
		case (HW_SIO_MODE & 0x0fff):
			Sio0Log.Error("%s(%08X) Unexpected SIO0 MODE 8 bit read", __FUNCTION__, addr);
			break;
		case (HW_SIO_CTRL & 0x0fff):
			Sio0Log.Error("%s(%08X) Unexpected SIO0 CTRL 8 bit read", __FUNCTION__, addr);
			break;
		case (HW_SIO_BAUD & 0x0fff):
			Sio0Log.Error("%s(%08X) Unexpected SIO0 BAUD 8 bit read", __FUNCTION__, addr);
			break;

		// for use of serial port ignore for now
		//case 0x50: ret = serial_read8(); break;

		mcase(HW_DEV9_DATA): ret = DEV9read8( addr ); break;

		mcase(HW_CDR_DATA0): ret = cdrRead0(); break;
		mcase(HW_CDR_DATA1): ret = cdrRead1(); break;
		mcase(HW_CDR_DATA2): ret = cdrRead2(); break;
		mcase(HW_CDR_DATA3): ret = cdrRead3(); break;

		default:
			if( masked_addr >= 0x100 && masked_addr < 0x130 )
			{
				DevCon.Warning( "HwRead8 from Counter16 [ignored] @ 0x%08x = 0x%02x", addr, psxHu8(addr) );
				ret = psxHu8( addr );
			}
			else if ( masked_addr >= 0x480 && masked_addr < 0x4a0 )
			{
				DevCon.Warning( "HwRead8 from Counter32 [ignored] @ 0x%08x = 0x%02x", addr, psxHu8(addr) );
				ret = psxHu8( addr );
			}
			else if ( (masked_addr >= pgmsk(HW_USB_START)) && (masked_addr < pgmsk(HW_USB_END)) )
			{
				ret = USBread8( addr );
				PSXHW_LOG( "HwRead8 from USB @ 0x%08x = 0x%02x", addr, ret );
			}
			else
			{
				ret = psxHu8(addr);
				PSXUnkHW_LOG( "HwRead8 from Unknown @ 0x%08x = 0x%02x", addr, ret );
			}
		return ret;
	}

	IopHwTraceLog<mem8_t>( addr, ret, true );
	return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem8_t iopHwRead8_Page3( u32 addr )
{
    // [TOTAL WAR] Forward Progress Verification
    static u32 debug_last_iop_pc = 0;
    static u32 debug_stall_pc = 0xFFFFFFFF;
    static u32 debug_stall_count = 0;
    static int debug_stall_logged = 0;

    // 1. bfc02454 Hit Counter & Exit Logic
    if (cpuRegs.pc == 0xbfc02454) {
        static int bfc_hits = 0;
        if (bfc_hits < 10 || (bfc_hits % 100) == 0) {
             s64 delta = (s64)((s64)cpuRegs.nextEventCycle - (s64)cpuRegs.cycle);
             fprintf(stderr, "@@BFC02454@@ hit=%d pc=%08x status=%08x cause=%08x epc=%08x cycle=%u next=%u d=%lld\n",
                 bfc_hits, cpuRegs.pc, cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.EPC, 
                 cpuRegs.cycle, cpuRegs.nextEventCycle, (long long)delta);
        }
        bfc_hits++;
    } else {
        // Exit Detection
        if (debug_last_iop_pc == 0xbfc02454) {
             static int exit_log_count = 0;
             if (exit_log_count < 5) {
                 fprintf(stderr, "@@BFC02454_EXIT@@ from=%08x to=%08x v0=%08x v1=%08x status=%08x cause=%08x epc=%08x\n", 
                     debug_last_iop_pc, cpuRegs.pc, cpuRegs.GPR.n.v0.UL[0], cpuRegs.GPR.n.v1.UL[0],
                     cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause, cpuRegs.CP0.n.EPC);
                 exit_log_count++;
             }
        }
    }
    
    // 2. Stall Detection (Any PC stuck > 10000 times)
    if (cpuRegs.pc == debug_stall_pc) {
        debug_stall_count++;
        if (debug_stall_count == 10000) {
            s64 delta = (s64)((s64)cpuRegs.nextEventCycle - (s64)cpuRegs.cycle);
            fprintf(stderr, "@@STALL@@ pc=%08x count=%u status=%08x cycle=%u next=%u d=%lld\n",
                cpuRegs.pc, debug_stall_count, cpuRegs.CP0.n.Status.val, cpuRegs.cycle, cpuRegs.nextEventCycle, (long long)delta);
        }
    } else {
        debug_stall_pc = cpuRegs.pc;
        debug_stall_count = 0;
    }
    
    debug_last_iop_pc = cpuRegs.pc;

	// all addresses are assumed to be prefixed with 0x1f803xxx:
	pxAssume((addr >> 12) == 0x1f803);

	mem8_t ret;
	if (addr == 0x1f803100)	// PS/EE/IOP conf related
		//ret = 0x10; // Dram 2M
		ret = 0xFF; //all high bus is the corect default state for CEX PS2!
    else {
		ret = psxHu8( addr );
    }

    // 3. IOP3204 Detail Log (Safe, minimal) & Test Hook
    if ((addr & 0x1FFFFFFF) == 0x1F803204) {
        static int iop_log_limit = 0;
        static u32 last_val = 0xFFFFFFFF;
        static int rd_log_count = 0;
        u32 current_val = (u32)ret;

        // [DIAGNOSIS] Force Value if Test Mode Active
        if (g_iop3204_test_val >= 0) {
            ret = (mem8_t)g_iop3204_test_val;
            fprintf(stderr, "@@MMIO_TEST@@ mode=FORCED addr=%08x ret=%02x (val=%d)\n", addr, ret, g_iop3204_test_val);
        } else {
            // Normal Log
            if (iop_log_limit < 20 || current_val != last_val) {
                fprintf(stderr, "@@IOP3204@@ n=%d pc=%08x addr=%08x ret8=%02x handler=iopHwRead8_Page3\n", 
                     iop_log_limit, cpuRegs.pc, addr, current_val);
            }
        }
        if (rd_log_count < 4) {
            fprintf(stderr, "@@IOP3204_RD@@ pc=%08x addr=%08x ret8=%02x src=iopHwRead8_Page3\n",
                cpuRegs.pc, addr, ret);
            rd_log_count++;
        }
        last_val = current_val;
        iop_log_limit++;
    }

	IopHwTraceLog<mem8_t>( addr, ret, true );
	return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem8_t iopHwRead8_Page8( u32 addr )
{
	// all addresses are assumed to be prefixed with 0x1f808xxx:
	pxAssume( (addr >> 12) == 0x1f808 );

	mem8_t ret;

	if (addr == HW_SIO2_RX)
	{
		ret = g_Sio2.Read();
	}
	else
	{
		ret = psxHu8(addr);
	}

	IopHwTraceLog<mem8_t>( addr, ret, true );
	return ret;
}
//////////////////////////////////////////////////////////////////////////////////////////
//
template< typename T >
static __fi T _HwRead_16or32_Page1( u32 addr )
{
	// all addresses are assumed to be prefixed with 0x1f801xxx:
	// [FIX] Changed pxAssume to Console.Error to prevent crash and log the bad address.
	if ((addr >> 12) != 0x1f801) {
		Console.Error("@@IOP_HW_BAD_ADDR@@ _HwRead_16or32_Page1 called with bad addr=0x%08x (expected 0x1f801xxx) pc=0x%08x",
			addr, cpuRegs.pc);
		return 0;
	}

	// all addresses should be aligned to the data operand size:
	pxAssume(
		( sizeof(T) == 2 && (addr & 1) == 0 ) ||
		( sizeof(T) == 4 && (addr & 3) == 0 )
	);

	u32 masked_addr = pgmsk( addr );
	T ret = 0;

	// ------------------------------------------------------------------------
	// Counters, 16-bit varieties!
	//
	if( masked_addr >= 0x100 && masked_addr < 0x130 )
	{
		int cntidx = ( masked_addr >> 4 ) & 0xf;
		switch( masked_addr & 0xf )
		{
			case 0x0:
				ret = (T)psxRcntRcount16( cntidx );
			break;

			case 0x4:
				ret = psxCounters[cntidx].mode.modeval;

				psxRcntSetNewIntrMode(cntidx);
			break;

			case 0x8:
				ret = static_cast<T>(psxCounters[cntidx].target);
			break;

			default:
				DevCon.Warning("Unknown 16bit counter read %x", addr);
				ret = psxHu32(addr);
			break;
		}
	}
	// ------------------------------------------------------------------------
	// Counters, 32-bit varieties!
	//
	else if ( masked_addr >= 0x480 && masked_addr < 0x4b0 )
	{
		int cntidx = (( masked_addr >> 4 ) & 0xf) - 5;
		switch( masked_addr & 0xf )
		{
			case 0x0:
				ret = (T)psxRcntRcount32( cntidx );
			break;

			case 0x2:
				ret = (T)(psxRcntRcount32( cntidx ) >> 16);
			break;

			case 0x4:
				ret = psxCounters[cntidx].mode.modeval;
				PSXCNT_LOG("IOP Counter[%d] modeRead%d = %lx", cntidx, sizeof(T) * 8, ret);

				psxRcntSetNewIntrMode(cntidx);
			break;

			case 0x8:
				ret = static_cast<T>(psxCounters[cntidx].target);
				PSXCNT_LOG("IOP Counter[%d] targetRead%d = %lx", cntidx, sizeof(T), ret);
			break;

			case 0xa:
				ret = static_cast<T>(psxCounters[cntidx].target >> 16);
				PSXCNT_LOG("IOP Counter[%d] targetUpperRead%d = %lx", cntidx, sizeof(T), ret);
			break;

			default:
				DevCon.Warning("Unknown 32bit counter read %x", addr);
				ret = psxHu32(addr);
			break;
		}
	}
	// ------------------------------------------------------------------------
	// USB, with both 16 and 32 bit interfaces
	//
	else if ( (masked_addr >= pgmsk(HW_USB_START)) && (masked_addr < pgmsk(HW_USB_END)) )
	{
		ret = (sizeof(T) == 2) ? USBread16( addr ) : USBread32( addr );
	}
	// ------------------------------------------------------------------------
	// SPU2, accessible in 16 bit mode only!
	//
	else if ( masked_addr >= pgmsk(HW_SPU2_START) && masked_addr < pgmsk(HW_SPU2_END) )
	{
		if( sizeof(T) == 2 )
			ret = SPU2read( addr );
		else
		{
			DevCon.Warning( "HwRead32 from SPU2? @ 0x%08X .. What manner of trickery is this?!", addr );
			ret = psxHu32(addr);
		}
	}
	// ------------------------------------------------------------------------
	// PS1 GPU access
	//
	else if ( (masked_addr >= pgmsk(HW_PS1_GPU_START)) && (masked_addr < pgmsk(HW_PS1_GPU_END)) )
	{
		// todo: psx mode: this is new
		if( sizeof(T) == 2 )
			DevCon.Warning( "HwRead16 from PS1 GPU? @ 0x%08X .. What manner of trickery is this?!", addr );

		pxAssert(sizeof(T) == 4);

		ret = psxDma2GpuR(addr);
	}
	else
	{
		switch( masked_addr )
		{
			// ------------------------------------------------------------------------
			case (HW_SIO_DATA & 0x0fff):
				Console.Warning("%s(%08X) Unexpected 16 or 32 bit access to SIO0 data register!", __FUNCTION__, addr);
				ret = g_Sio0.GetRxData();
				ret |= g_Sio0.GetRxData() << 8;
				if (sizeof(T) == 4)
				{
					ret |= g_Sio0.GetRxData() << 16;
					ret |= g_Sio0.GetRxData() << 24;
				}
				break;
			case (HW_SIO_STAT & 0x0fff):
				ret = g_Sio0.GetStat();
				break;
			case (HW_SIO_MODE & 0x0fff):
				ret = g_Sio0.GetMode();
				
				if (sizeof(T) == 4)
				{
					Console.Warning("%s(%08X) Unexpected 32 bit access to SIO0 MODE register!", __FUNCTION__, addr);
				}
				
				break;
			case (HW_SIO_CTRL & 0x0fff):
				ret = g_Sio0.GetCtrl();
				break;
			case (HW_SIO_BAUD & 0x0fff):
				ret = g_Sio0.GetBaud();
				break;

			// ------------------------------------------------------------------------
			//Serial port stuff not support now ;P
			// case 0x050: hard = serial_read32(); break;
			//	case 0x054: hard = serial_status_read(); break;
			//	case 0x05a: hard = serial_control_read(); break;
			//	case 0x05e: hard = serial_baud_read(); break;

			mcase(HW_ICTRL):
				ret = psxHu32(0x1078);
				psxHu32(0x1078) = 0;
			break;

			mcase(HW_ICTRL+2):
				ret = psxHu16(0x107a);
				psxHu32(0x1078) = 0;	// most likely should clear all 32 bits here.
			break;

			// ------------------------------------------------------------------------
			// Legacy GPU  emulation
			//
			mcase(0x1f8010ac) :
				ret = psxHu32(addr);
				DevCon.Warning("SIF2 IOP TADR?? read");
			break;

			mcase(HW_PS1_GPU_DATA) :
				ret = psxGPUr(addr);
			break;

			mcase(HW_PS1_GPU_STATUS) :
				ret = psxGPUr(addr);
			break;

			mcase (0x1f801820): // MDEC
				// ret = psxHu32(addr); // old
				ret = mdecRead0();
#if PSX_EXTRALOGS
				DevCon.Warning("MDEC 1820 Read %x", ret);
#endif
			break;

			mcase (0x1f801824): // MDEC
				//ret = psxHu32(addr); // old
				ret = mdecRead1();
#if PSX_EXTRALOGS
			DevCon.Warning("MDEC 1824 Read %x", ret);
#endif
			break;

			// ------------------------------------------------------------------------

			mcase(0x1f80146e):
				ret = DEV9read16( addr );
			break;

			default:
				ret = psxHu32(addr);
			break;
		}
	}

	IopHwTraceLog<T>( addr, ret, true );
	return ret;
}

// Some Page 2 mess?  I love random question marks for comments!
//case 0x1f802030: hard =   //int_2000????
//case 0x1f802040: hard =//dip switches...??

//////////////////////////////////////////////////////////////////////////////////////////
//
mem16_t iopHwRead16_Page1( u32 addr )
{
	return _HwRead_16or32_Page1<mem16_t>( addr );
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem16_t iopHwRead16_Page3( u32 addr )
{
	// all addresses are assumed to be prefixed with 0x1f803xxx:
	pxAssume( (addr >> 12) == 0x1f803 );

	mem16_t ret = psxHu16(addr);
	IopHwTraceLog<mem16_t>( addr, ret, true );
	return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem16_t iopHwRead16_Page8( u32 addr )
{
	// all addresses are assumed to be prefixed with 0x1f808xxx:
	pxAssume( (addr >> 12) == 0x1f808 );

	mem16_t ret = psxHu16(addr);
	IopHwTraceLog<mem16_t>( addr, ret, true );
	return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem32_t iopHwRead32_Page1( u32 addr )
{
	// Log SIF register access (value-change only)
	if (addr == 0x1f801020) {
		static bool first_read = true;
		static u32 last_value = 0;
		u32 current_value = _HwRead_16or32_Page1<mem32_t>(addr);
		
		if (first_read || current_value != last_value) {
			Console.WriteLn("IOP_HW_READ: addr=%08x val=%08x PC=%08x", addr, current_value, cpuRegs.pc);
			first_read = false;
			last_value = current_value;
		}
		return current_value;
	}
	
	// [TEMP_DIAG] DMA9 CHCR read detection (smart logging)
	if (addr == 0x1f801528) {
		mem32_t val = _HwRead_16or32_Page1<mem32_t>(addr);
		static int s_d9r = 0;
		s_d9r++;
		// Log first 10, then every 1000th
		if (s_d9r <= 10 || (s_d9r % 1000 == 0))
			Console.WriteLn("@@D9_CHCR_R@@ n=%d val=%08x ioppc=%08x cyc=%u",
				s_d9r, val, psxRegs.pc, psxRegs.cycle);
		return val;
	}
	return _HwRead_16or32_Page1<mem32_t>( addr );
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem32_t iopHwRead32_Page3( u32 addr )
{
	// all addresses are assumed to be prefixed with 0x1f803xxx:
	pxAssume( (addr >> 12) == 0x1f803 );

	const mem32_t ret = psxHu32(addr);
	IopHwTraceLog<mem32_t>( addr, ret, true );
	return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem32_t iopHwRead32_Page8( u32 addr )
{
	// all addresses are assumed to be prefixed with 0x1f808xxx:
	pxAssume( (addr >> 12) == 0x1f808 );

	u32 masked_addr = addr & 0x0fff;
	mem32_t ret;

	if( masked_addr >= 0x200 )
	{
		if( masked_addr < 0x240 )
		{
			const int parm = (masked_addr-0x200) / 4;
			ret = g_Sio2.CmdQueue[parm];
			Sio2Log.WriteLn("%s(%08X) SIO2 SEND3 Read (%08X)", __FUNCTION__, addr, ret);
		}
		else if ( masked_addr < 0x260 )
		{
			// SIO2 Send commands alternate registers.  First reg maps to Send1, second
			// to Send2, third to Send1, etc.  And the following clever code does this:
			const int parm = (masked_addr-0x240) / 8;
			ret = (masked_addr & 4) ? g_Sio2.PortCtrl1[parm] : g_Sio2.PortCtrl0[parm];
			Sio2Log.WriteLn("%s(%08X) SIO2 SEND1/2 Read (%08X)", __FUNCTION__, addr, ret);
		}
		else if ( masked_addr <= 0x280 )
		{
			switch( masked_addr )
			{
				case (HW_SIO2_TX & 0x0fff):
					ret = psxHu32(addr);
					Sio2Log.Warning("%s(%08X) Unexpected 32 bit read of HW_SIO2_DATAIN (%08X)", __FUNCTION__, addr, ret);
					break;
				case (HW_SIO2_RX & 0x0fff):
					ret = psxHu32(addr);
					Sio2Log.Warning("%s(%08X) Unexpected 32 bit read of HW_SIO2_FIFO (%08X)", __FUNCTION__, addr, ret);
					break;
				case (HW_SIO2_CTRL & 0x0fff):
					ret = g_Sio2.ctrl;
					Sio2Log.WriteLn("%s(%08X) SIO2 CTRL Read (%08X)", __FUNCTION__, addr, ret);
					break;
				case (HW_SIO2_CMD_STAT & 0xfff):
					ret = g_Sio2.CmdStat;
					Sio2Log.WriteLn("%s(%08X) SIO2 RECV1 Read (%08X)", __FUNCTION__, addr, ret);
					break;
				case (HW_SIO2_PORT_STAT & 0x0fff):
					ret = g_Sio2.PortStat;
					Sio2Log.WriteLn("%s(%08X) SIO2 RECV2 Read (%08X)", __FUNCTION__, addr, ret);
					break;
				case (HW_SIO2_FIFO_STAT & 0x0fff):
					ret = g_Sio2.FifoStat;
					Sio2Log.WriteLn("%s(%08X) SIO2 RECV3 Read (%08X)", __FUNCTION__, addr, ret);
					break;
				case (0x1f808278 & 0x0fff):
					ret = g_Sio2.FifoTxPos;
					Sio2Log.WriteLn("%s(%08X) SIO2 UNK1 Read (%08X)", __FUNCTION__, addr, ret);
					break;
				case (0x1f80827C & 0x0fff):
					ret = g_Sio2.FifoRxPos;
					Sio2Log.WriteLn("%s(%08X) SIO2 UNK2 Read (%08X)", __FUNCTION__, addr, ret);
					break;
				case (HW_SIO2_INTR & 0x0fff):
					ret = g_Sio2.iStat;
					Sio2Log.WriteLn("%s(%08X) SIO2 ISTAT Read (%08X)", __FUNCTION__, addr, ret);
					break;
				default:
					ret = psxHu32(addr);
					break;
			}
		}
		else if ( masked_addr >= pgmsk(HW_FW_START) && masked_addr <= pgmsk(HW_FW_END) )
		{
			ret = FWread32( addr );
		} else {
			ret = psxHu32(addr);
		}
	}
	else ret = psxHu32(addr);

	IopHwTraceLog<mem32_t>( addr, ret, true );
	return ret;
}

}
