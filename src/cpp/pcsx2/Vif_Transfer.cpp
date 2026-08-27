// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Vif_Dma.h"
#include "Vif_Dynarec.h"
#include <atomic>

//------------------------------------------------------------------
// VifCode Transfer Interpreter (Vif0/Vif1)
//------------------------------------------------------------------

// [TEMP_DIAG] VIF1 transfer stats — Removal condition: draws=0 root causeafter identified
static std::atomic<uint32_t> s_vif1_xfer_calls{0};
static std::atomic<uint32_t> s_vif1_cmds_total{0};
static std::atomic<uint32_t> s_vif1_cmd_nop{0};
static std::atomic<uint32_t> s_vif1_cmd_unpack{0};
static std::atomic<uint32_t> s_vif1_cmd_mscal{0};  // MSCAL+MSCNT+MSCALF
static std::atomic<uint32_t> s_vif1_cmd_direct{0}; // DIRECT+DIRECTHL
static std::atomic<uint32_t> s_vif1_cmd_other{0};
uint32_t getVif1XferCalls() { return s_vif1_xfer_calls.load(); }
uint32_t getVif1CmdsTotal() { return s_vif1_cmds_total.load(); }
uint32_t getVif1CmdNop() { return s_vif1_cmd_nop.load(); }
uint32_t getVif1CmdUnpack() { return s_vif1_cmd_unpack.load(); }
uint32_t getVif1CmdMscal() { return s_vif1_cmd_mscal.load(); }
uint32_t getVif1CmdDirect() { return s_vif1_cmd_direct.load(); }
uint32_t getVif1CmdOther() { return s_vif1_cmd_other.load(); }

// Interprets packet
_vifT void vifTransferLoop(u32* &data) {
	vifStruct& vifX = GetVifX;

	u32& pSize = vifX.vifpacketsize;

	int ret = 0;

	vifXRegs.stat.VPS |= VPS_TRANSFERRING;
	vifXRegs.stat.ER1  = false;
	//VIF_LOG("Starting VIF%d loop, pSize = %x, stalled = %x", idx, pSize, vifX.vifstalled.enabled );
	while (pSize > 0 && !vifX.vifstalled.enabled) {

		if(!vifX.cmd) { // Get new VifCode

			if(!vifXRegs.err.MII)
			{
				if(vifX.irq && !CHECK_VIF1STALLHACK)
					break;

				vifX.irq      |= data[0] >> 31;
			}

			vifXRegs.code = data[0];
			vifX.cmd	  = data[0] >> 24;

			// [TEMP_DIAG] VIF1 command classification
			if (idx == 1) {
				u8 c = vifX.cmd & 0x7f;
				s_vif1_cmds_total.fetch_add(1, std::memory_order_relaxed);
				if (c == 0) s_vif1_cmd_nop.fetch_add(1, std::memory_order_relaxed);
				else if (c >= 0x60) s_vif1_cmd_unpack.fetch_add(1, std::memory_order_relaxed);
				else if (c == 0x14 || c == 0x15 || c == 0x17) s_vif1_cmd_mscal.fetch_add(1, std::memory_order_relaxed);
				else if (c == 0x50 || c == 0x51) s_vif1_cmd_direct.fetch_add(1, std::memory_order_relaxed);
				else {
					s_vif1_cmd_other.fetch_add(1, std::memory_order_relaxed);
					static int s_other_log = 0;
					if (s_other_log < 20) {
						Console.WriteLn("@@VIF1_CMD@@ n=%d raw=%08x cmd=%02x", s_other_log, data[0], c);
						s_other_log++;
					}
				}
			}

			VIF_LOG("New VifCMD %x tagsize %x irq %d", vifX.cmd, vifX.tag.size, vifX.irq);
			if (IsDevBuild && TraceLogging.EE.VIFcode.IsActive()) {
				// Pass 2 means "log it"
				vifCmdHandler[idx][vifX.cmd & 0x7f](2, data);
			}
		}

		ret = vifCmdHandler[idx][vifX.cmd & 0x7f](vifX.pass, data);
		data   += ret;
		pSize  -= ret;
		if (vifX.vifstalled.enabled)
		{
			int current_STR = idx ? vif1ch.chcr.STR : vif0ch.chcr.STR;
			if (!current_STR)
				DevCon.Warning("Warning! VIF%d stalled during FIFO transfer!", idx);
		}
	}
}

_vifT static __fi bool vifTransfer(u32 *data, int size, bool TTE) {
	vifStruct& vifX = GetVifX;

	// irqoffset necessary to add up the right qws, or else will spin (spiderman)
	int transferred = vifX.irqoffset.enabled ? vifX.irqoffset.value : 0;

	vifX.vifpacketsize = size;
	vifTransferLoop<idx>(data);

	transferred += size - vifX.vifpacketsize;

	//Make this a minimum of 1 cycle so if it's the end of the packet it doesnt just fall through.
	//Metal Saga can do this, just to be safe :)
	if (!idx) g_vif0Cycles += std::max(1, (int)((transferred * BIAS) >> 2));
	else	  g_vif1Cycles += std::max(1, (int)((transferred * BIAS) >> 2));

	vifX.irqoffset.value = transferred % 4; // cannot lose the offset

	if (vifX.irq && vifX.cmd == 0) {
		VIF_LOG("Vif%d IRQ Triggering", idx);
		//Always needs to be set to return to the correct offset if there is data left.
		vifX.vifstalled.enabled = VifStallEnable(vifXch);
		vifX.vifstalled.value = VIF_IRQ_STALL;
	}

	if (!TTE) // *WARNING* - Tags CAN have interrupts! so lets just ignore the dma modifying stuffs (GT4)
	{
		transferred  = transferred >> 2;
		transferred = std::min((int)vifXch.qwc, transferred);
		vifXch.madr +=(transferred << 4);
		vifXch.qwc  -= transferred;

		hwDmacSrcTadrInc(vifXch);

		vifX.irqoffset.enabled = false;

		if(!vifXch.qwc)
			vifX.inprogress &= ~0x1;
		else if (vifX.irqoffset.value != 0)
			vifX.irqoffset.enabled = true;
	}
	else
	{
		if(vifX.irqoffset.value != 0){
			vifX.irqoffset.enabled = true;
		}else
			vifX.irqoffset.enabled = false;
	}

	vifExecQueue(idx);

	return !vifX.vifstalled.enabled;
}

// When TTE is set to 1, MADR and QWC are not updated as part of the transfer.
bool VIF0transfer(u32 *data, int size, bool TTE) {
	return vifTransfer<0>(data, size, TTE);
}
bool VIF1transfer(u32 *data, int size, bool TTE) {
	s_vif1_xfer_calls.fetch_add(1, std::memory_order_relaxed);
	return vifTransfer<1>(data, size, TTE);
}
