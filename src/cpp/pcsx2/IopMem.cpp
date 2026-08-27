// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/AlignedMalloc.h"
#include "R3000A.h"
#include "Common.h"
#include "ps2/pgif.h" // for PSX kernel TTY in iopMemWrite32
#include "SPU2/spu2.h"
#include "DEV9/DEV9.h"
#include "IopHw.h"
#include "Hw.h" // [P32] hwIntcIrq for IOP→EE SBUS interrupt

extern "C" void LogUnified(const char* fmt, ...);

// [iv2 accel Phase 0 2026-06-07] IOP メモリ診断 probe の compile-gate。
// default 0 = release で完全除去。中身は read-only 診断 (Console 出力) のみで
// emulation 状態を一切変えない (logic-preserving)。iopMemRead32 は hot path のため
// per-read の range 比較群を release で消す。退行調査時は -DIPSX2_IOP_EVT_DIAG=1 で復活。
#ifndef IPSX2_IOP_EVT_DIAG
#define IPSX2_IOP_EVT_DIAG 0
#endif

uptr *psxMemWLUT = nullptr;
const uptr *psxMemRLUT = nullptr;

IopVM_MemoryAllocMess* iopMem = nullptr;

alignas(__pagealignsize) u8 iopHw[Ps2MemSize::IopHardware];

void iopMemAlloc()
{
	// TODO: Move to memmap
	psxMemWLUT = (uptr*)_aligned_malloc(0x2000 * sizeof(uptr) * 2, 16);
	if (!psxMemWLUT)
		pxFailRel("Failed to allocate IOP memory lookup table");

	psxMemRLUT = psxMemWLUT + 0x2000; //(uptr*)_aligned_malloc(0x10000 * sizeof(uptr),16);

	iopMem = reinterpret_cast<IopVM_MemoryAllocMess*>(SysMemory::GetIOPMem());
}

void iopMemRelease()
{
	safe_aligned_free(psxMemWLUT);
	psxMemRLUT = nullptr;
	iopMem = nullptr;
}

// Note!  Resetting the IOP's memory state is dependent on having *all* psx memory allocated,
// which is performed by MemInit and PsxMemInit()
void iopMemReset()
{
	pxAssert(iopMem);

	DbgCon.WriteLn("IOP resetting main memory...");

	memset(psxMemWLUT, 0, 0x2000 * sizeof(uptr) * 2); // clears both allocations, RLUT and WLUT

	// Trick!  We're accessing RLUT here through WLUT, since it's the non-const pointer.
	// So the ones with a 0x2000 prefixed are RLUT tables.

	// Map IOP main memory, which is Read/Write, and mirrored three times
	// at 0x0, 0x8000, and 0xa000:
	for (int i = 0; i < 0x0080; i++)
	{
		psxMemWLUT[i + 0x0000] = (uptr)&iopMem->Main[(i & 0x1f) << 16];

		// RLUTs, accessed through WLUT.
		psxMemWLUT[i + 0x2000] = (uptr)&iopMem->Main[(i & 0x1f) << 16];
	}

	// A few single-page allocations for things we store in special locations.
	psxMemWLUT[0x2000 + 0x1f00] = (uptr)iopMem->P;
	psxMemWLUT[0x2000 + 0x1f80] = (uptr)iopHw;
	//psxMemWLUT[0x1bf80] = (uptr)iopHw;

	psxMemWLUT[0x1f00] = (uptr)iopMem->P;
	psxMemWLUT[0x1f80] = (uptr)iopHw;
	//psxMemWLUT[0xbf80] = (uptr)iopHw;

	// Read-only memory areas, so don't map WLUT for these...
	for (int i = 0; i < 0x0040; i++)
	{
		psxMemWLUT[i + 0x2000 + 0x1fc0] = (uptr)&eeMem->ROM[i << 16];
	}

	for (int i = 0; i < 0x0040; i++)
	{
		psxMemWLUT[i + 0x2000 + 0x1e00] = (uptr)&eeMem->ROM1[i << 16];
	}

	for (int i = 0; i < 0x0040; i++)
	{
		psxMemWLUT[i + 0x2000 + 0x1e40] = (uptr)&eeMem->ROM2[i << 16];
	}

	// sif!! (which is read only? (air))
	psxMemWLUT[0x2000 + 0x1d00] = (uptr)iopMem->Sif;
	//psxMemWLUT[0x1bd00] = (uptr)iopMem->Sif;

	// this one looks like an old hack for some special write-only memory area,
	// but leaving it in for reference (air)
	//for (i=0; i<0x0008; i++) psxMemWLUT[i + 0xbfc0] = (uptr)&psR[i << 16];

	std::memset(iopMem, 0, sizeof(*iopMem));
}

u8 iopMemRead8(u32 mem)
{
	mem &= 0x1fffffff;
	u32 t = mem >> 16;

	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: return IopMemory::iopHwRead8_Page1(mem);
			case 0x3000: return IopMemory::iopHwRead8_Page3(mem);
			case 0x8000: return IopMemory::iopHwRead8_Page8(mem);

			default:
				return psxHu8(mem);
		}
	}
	else if (t == 0x1f40)
	{
		return psxHw4Read8(mem);
	}
	else
	{
		const u8* p = (const u8*)(psxMemRLUT[mem >> 16]);
		if (p != NULL)
		{
			return *(const u8 *)(p + (mem & 0xffff));
		}
		else
		{
			if (t == 0x1000)
				return DEV9read8(mem);
			PSXMEM_LOG("err lb %8.8lx", mem);
			return 0;
		}
	}
}

u16 iopMemRead16(u32 mem)
{
	mem &= 0x1fffffff;
	u32 t = mem >> 16;

	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: return IopMemory::iopHwRead16_Page1(mem);
			case 0x3000: return IopMemory::iopHwRead16_Page3(mem);
			case 0x8000: return IopMemory::iopHwRead16_Page8(mem);

			default:
				return psxHu16(mem);
		}
	}
	else
	{
		const u8* p = (const u8*)(psxMemRLUT[mem >> 16]);
		if (p != NULL)
		{
			if (t == 0x1d00)
			{
				u16 ret;
				switch(mem & 0xF0)
				{
				case 0x00:
					ret= psHu16(SBUS_F200);
					break;
				case 0x10:
					ret= psHu16(SBUS_F210);
					break;
				case 0x40:
					ret= psHu16(SBUS_F240) | 0x0002;
					break;
				case 0x60:
					ret = 0;
					break;
				default:
					ret = psxHu16(mem);
					break;
				}
				//SIF_LOG("Sif reg read %x value %x", mem, ret);
				return ret;
			}
			return *(const u16 *)(p + (mem & 0xffff));
		}
		else
		{
			if (t == 0x1F90)
				return SPU2read(mem);
			if (t == 0x1000)
				return DEV9read16(mem);
			PSXMEM_LOG("err lh %8.8lx", mem);
			return 0;
		}
	}
}

u32 iopMemRead32(u32 mem)
{
	mem &= 0x1fffffff;
	u32 t = mem >> 16;

	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: return IopMemory::iopHwRead32_Page1(mem);
			case 0x3000: return IopMemory::iopHwRead32_Page3(mem);
			case 0x8000: return IopMemory::iopHwRead32_Page8(mem);

			default:
				return psxHu32(mem);
		}
	} else
	{
		//see also Hw.c
		const u8* p = (const u8*)(psxMemRLUT[mem >> 16]);
#if IPSX2_IOP_EVT_DIAG
		// [iter100] @@IOP_ROM_SCAN32@@ – magic scan range 0x1FC02600-0x1FC02900 (first 30)
		{
			static u32 s_iop_scan_n = 0;
			if (mem >= 0x1FC02600u && mem < 0x1FC02900u && s_iop_scan_n < 30) {
				u32 val = (p != NULL) ? *(const u32*)(p + (mem & 0xffff)) : 0xDEADBEEFu;
				Console.WriteLn("@@IOP_ROM_SCAN32@@ n=%u mem=%08x p=%s val=%08x",
					s_iop_scan_n, mem, (p != NULL) ? "OK" : "NULL", val);
				s_iop_scan_n++;
			}
		}
#endif
		if (p != NULL)
		{
			if (t == 0x1d00)
			{
				u32 ret;
				switch(mem & 0x8F0)
				{
				case 0x00:
					ret= psHu32(SBUS_F200);
					break;
				case 0x10:
					ret= psHu32(SBUS_F210);
					break;
				case 0x20:
					ret= psHu32(SBUS_F220);
					break;
				case 0x30:	// EE Side
					ret= psHu32(SBUS_F230);
					break;
				case 0x40:
					ret= psHu32(SBUS_F240) | 0xF0000002;
					break;
				case 0x60:
					ret = 0;
					break;

				default:
					ret = psxHu32(mem);
					break;
				}
				//SIF_LOG("Sif reg read %x value %x", mem, ret);
				return ret;
			}
			{
				const u32 result = *(const u32 *)(p + (mem & 0xffff));
#if IPSX2_IOP_EVT_DIAG
				// [iter132] probe: detect low IOP RAM reads (pc=0 source hypothesis)
				if (mem < 0x00001000u) {
					static u32 s_lowrd_n = 0;
					if (s_lowrd_n < 8) {
						++s_lowrd_n;
						Console.WriteLn("@@IOP_LOWRD32@@ n=%u mem=0x%08x result=0x%08x",
							s_lowrd_n, mem, result);
					}
				}
				// [iter133] probe: log all iopMemRead32 calls for bfc4b138 dispatch address (cap 8)
				if (mem == 0x1FC4B138u) {
					static u32 s_b138_n = 0;
					if (s_b138_n < 8) {
						++s_b138_n;
						Console.WriteLn("@@IOP_MEM_RD32_4B138@@ n=%u p=%p off=0x%x result=0x%08x",
							s_b138_n, (const void*)p, (mem & 0xffff), result);
					}
				}
#endif
				return result;
			}
		}
		else
		{
			if (t == 0x1000)
				return DEV9read32(mem);
			return 0;
		}
	}
}

void iopMemWrite8(u32 mem, u8 value)
{
	static u32 s_iop2070_count = 0;
	static bool s_iop2070_cond_logged = false;
	mem &= 0x1fffffff;
	u32 t = mem >> 16;

	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: IopMemory::iopHwWrite8_Page1(mem,value); break;
			case 0x3000: IopMemory::iopHwWrite8_Page3(mem,value); break;
			case 0x8000: IopMemory::iopHwWrite8_Page8(mem,value); break;

			default:
				bool log_cond = false;
				if (mem == 0x1f802070 && s_iop2070_count < 8)
				{
					LogUnified("@@IOP2070_WR@@ addr=%08x val=%02x src=iopMemWrite8\n", mem, value);
					s_iop2070_count++;
					log_cond = true;
				}
				psxHu8(mem) = value;
				if (log_cond && !s_iop2070_cond_logged)
				{
					const u8 rb = psxHu8(mem);
					const u8 reg3204 = psxHu8(0x1f803204);
					LogUnified("@@COND@@ addr=%08x write=%02x readback=%02x reg3204=%02x\n", mem, value, rb, reg3204);
					s_iop2070_cond_logged = true;
				}
			break;
		}
	}
	else if (t == 0x1f40)
	{
		psxHw4Write8(mem, value);
	}
	else
	{
		u8* p = (u8 *)(psxMemWLUT[mem >> 16]);
		if (p != NULL && !(psxRegs.CP0.n.Status & 0x10000) )
		{
			*(u8  *)(p + (mem & 0xffff)) = value;
			psxCpu->Clear(mem&~3, 1);
		}
		else
		{
			if (t == 0x1d00)
			{
				Console.WriteLn("sw8 [0x%08X]=0x%08X", mem, value);
				psxSu8(mem) = value;
				return;
			}
			if (t == 0x1000)
			{
				DEV9write8(mem, value); return;
			}
			PSXMEM_LOG("err sb %8.8lx = %x", mem, value);
		}
	}
}

void iopMemWrite16(u32 mem, u16 value)
{
	mem &= 0x1fffffff;
	u32 t = mem >> 16;

	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: IopMemory::iopHwWrite16_Page1(mem,value); break;
			case 0x3000: IopMemory::iopHwWrite16_Page3(mem,value); break;
			case 0x8000: IopMemory::iopHwWrite16_Page8(mem,value); break;

			default:
				psxHu16(mem) = value;
				break;
		}
	} else
	{
		u8* p = (u8 *)(psxMemWLUT[mem >> 16]);
		if (p != NULL && !(psxRegs.CP0.n.Status & 0x10000) )
		{
			if( t==0x1D00 ) Console.WriteLn("sw16 [0x%08X]=0x%08X", mem, value);
			*(u16 *)(p + (mem & 0xffff)) = value;
			psxCpu->Clear(mem&~3, 1);
		}
		else
		{
			if (t == 0x1d00)
			{
				switch (mem & 0x8f0)
				{
					case 0x10:
						// write to ps2 mem
						psHu16(SBUS_F210) = value;
						return;
					case 0x40:
					{
						// [iter230] TEMP_DIAG: IOP 16-bit SBUS_F240 write
						{
							static u32 s_f240_16_n = 0;
							if (s_f240_16_n++ < 10)
								Console.WriteLn("@@IOP_SBUS_F240_W16@@ n=%u val=%04x f240_before=%08x iop_pc=%08x",
									s_f240_16_n, (unsigned)value, psHu32(SBUS_F240), psxRegs.pc);
						}
						u32 temp = value & 0xF0;
						// write to ps2 mem
						if(value & 0x20 || value & 0x80)
						{
							psHu16(SBUS_F240) &= ~0xF000;
							psHu16(SBUS_F240) |= 0x2000;
						}


						if(psHu16(SBUS_F240) & temp)
							psHu16(SBUS_F240) &= ~temp;
						else
							psHu16(SBUS_F240) |= temp;
						return;
					}
					case 0x60:
						psHu32(SBUS_F260) = 0;
						return;
				}
#if PSX_EXTRALOGS
				DevCon.Warning("IOP 16 Write to %x value %x", mem, value);
#endif
				psxSu16(mem) = value; return;
			}
			if (t == 0x1F90) {
				SPU2write(mem, value); return;
			}
			if (t == 0x1000) {
				DEV9write16(mem, value); return;
			}
			PSXMEM_LOG("err sh %8.8lx = %x", mem, value);
		}
	}
}

void iopMemWrite32(u32 mem, u32 value)
{
	mem &= 0x1fffffff;
	// [TEMP_DIAG] iter645: @@IOP_125C4_WRITE@@ — 0x125c4 書き込みdetect (cap=20)
	// 根拠: RegisterGPUCallback 全パス decode で 0x125c4 書き込みなし確定。
	//        VBlank ISR が永続 0x125c4=0 を見て毎回 RegisterGPUCallback を呼ぶcauseを確定。
	// Removal condition: 0x125c4 の書き込み元 (IOP PC) がafter identified
	if ((mem & 0x1FFFFFu) == 0x125c4u) {
		static u32 s_125c4_w_n = 0;
		if (s_125c4_w_n < 20) {
			Console.WriteLn("@@IOP_125C4_WRITE@@ n=%u val=%08x ioppc=%08x",
				s_125c4_w_n, value, psxRegs.pc);
			s_125c4_w_n++;
		}
	}
	// [TEMP_DIAG] @@IOP_EXCVEC_WRITE@@ — 0x80-0x8C への異常書き込みdetect
	// Removal condition: IOP excvec 破損causeafter identified
	if ((mem & 0x1FFFFF) >= 0x80 && (mem & 0x1FFFFF) <= 0x8C) {
		// Only fire if value looks like float data (not normal MIPS instructions)
		// Normal handler: ac010400, ac1a0410, 401a7000, 40016000
		bool suspicious = (value == 0x4b2ab970 || value == 0xc2400000 ||
		                   value == 0x43060000 || value == 0x3f800000 ||
		                   (value & 0xFC000000) == 0x40000000); // COP1/float-like
		if (suspicious) {
			Console.Error("@@IOP_EXCVEC_WRITE_BAD@@ mem=%08x val=%08x ioppc=%08x ee_pc=%08x iop_cyc=%u",
				mem, value, psxRegs.pc, cpuRegs.pc, psxRegs.cycle);
		}
	}
	// [TEMP_DIAG] @@IOP_WATCH_179A8_W@@ write watchpoint
	if ((mem & 0x1FFFFF) == 0x179A8) {
		static u32 s_watch_w_n = 0;
		if (s_watch_w_n < 20) {
			Console.WriteLn("@@IOP_WATCH_179A8_W@@ n=%u mem=%08x val=%08x ioppc=%08x",
				s_watch_w_n, mem, value, psxRegs.pc);
			s_watch_w_n++;
		}
	}
	u32 t = mem >> 16;

	// removed: 0x179BC watchpoint (BSS is correctly zeroed)
	if (t == 0x1f80)
	{
		switch( mem & 0xf000 )
		{
			case 0x1000: IopMemory::iopHwWrite32_Page1(mem,value); break;
			case 0x3000: IopMemory::iopHwWrite32_Page3(mem,value); break;
			case 0x8000: IopMemory::iopHwWrite32_Page8(mem,value); break;

			default:
				psxHu32(mem) = value;
			break;
		}
	} else
	{
		//see also Hw.c
		u8* p = (u8 *)(psxMemWLUT[mem >> 16]);
		if( p != NULL && !(psxRegs.CP0.n.Status & 0x10000) )
		{
			*(u32 *)(p + (mem & 0xffff)) = value;
			psxCpu->Clear(mem&~3, 1);
		}
		else
		{
			if (t == 0x1d00)
			{
				MEM_LOG("iop Sif reg write %x value %x", mem, value);
				switch (mem & 0x8f0)
				{
					case 0x00:		// EE write path (EE/IOP readable)
						return;		// this is the IOP, so read-only (do nothing)

					case 0x10:		// IOP write path (EE/IOP readable)
						// [TEMP_DIAG] @@IOP_SMCOM_WRITE@@ - IOP SMCOM write tracker (remove after root cause found)
						{
							static u32 s_smcom_n = 0;
							if (s_smcom_n < 20) {
								++s_smcom_n;
								Console.WriteLn("@@IOP_SMCOM_WRITE@@ n=%u iop_pc=0x%08x val=0x%08x old=0x%08x iopcyc=%u",
									s_smcom_n, psxRegs.pc, value, psHu32(SBUS_F210), psxRegs.cycle);
							}
						}
						psHu32(SBUS_F210) = value;
						return;

					case 0x20:		// Bits cleared when written from IOP.
						psHu32(SBUS_F220) &= ~value;
						return;

					case 0x30:		// bits set when written from IOP
						// [iter672] @@IOP_SBUS_F230_WRITE@@ – IOP が EE向けメッセージ(SMFLG)をconfig
						// cap 8→30, bit18 特別追跡
						// Removal condition: SMFLG bit18 差異のroot causeafter identified
						{
							static u32 s_iop_f230_n = 0;
							static bool s_bit18_logged = false;
							if (s_iop_f230_n < 30) {
								++s_iop_f230_n;
								Console.WriteLn("@@IOP_SBUS_F230_WRITE@@ n=%u iop_pc=0x%08x val=0x%08x f230_before=0x%08x",
									s_iop_f230_n, psxRegs.pc, value, psHu32(SBUS_F230));
							}
							if (!s_bit18_logged && (value & 0x40000)) {
								s_bit18_logged = true;
								Console.WriteLn("@@SMFLG_BIT18_SET@@ iop_pc=0x%08x val=0x%08x f230_before=0x%08x iopcyc=%u",
									psxRegs.pc, value, psHu32(SBUS_F230), psxRegs.cycle);
							}
						}
						psHu32(SBUS_F230) |= value;
						return;

					case 0x40:		// Control Register
					{
						// [iter99] @@IOP_SBUS_F240_WRITE@@ – IOP が SBUS_F240 controlregisterに書き込む = SIF ハンドシェイク到達verify
						{
							static u32 s_iop_sbus_n = 0;
							if (s_iop_sbus_n < 8)
							{
								Console.WriteLn("@@IOP_SBUS_F240_WRITE@@ n=%u mem=%08x val=%08x f240_before=%08x",
									s_iop_sbus_n, mem, value, psHu32(SBUS_F240));
								s_iop_sbus_n++;
							}
						}
						u32 temp = value & 0xF0;
						if (value & 0x20 || value & 0x80)
						{
							psHu32(SBUS_F240) &= ~0xF000;
							psHu32(SBUS_F240) |= 0x2000;
							// [P32] hwIntcIrq(INTC_SBUS) は逆効果: カーネル SBUS handlerが
							// [0x80024124] を 0x27 に再書き込みし、P31 フラグをcorruptする。
							// 実機準拠の割り込みは P31 フラグ撤去後にenabled化する。
						}


						if (psHu32(SBUS_F240) & temp)
							psHu32(SBUS_F240) &= ~temp;
						else
							psHu32(SBUS_F240) |= temp;
						return;
					}

					case 0x60:
						psHu32(SBUS_F260) = 0;
						return;

				}
#if PSX_EXTRALOGS
				DevCon.Warning("IOP 32 Write to %x value %x", mem, value);
#endif
				psxSu32(mem) = value;

				// wtf?  why were we writing to the EE's sif space?  Commenting this out doesn't
				// break any of my games, and should be more correct, but I guess we'll see.  --air
				//*(u32*)(eeHw+0xf200+(mem&0xf0)) = value;
				return;
			}
			else if (t == 0x1000)
			{
				DEV9write32(mem, value); return;
			}
		}
	}
}

int iopMemSafeCmpBytes(u32 mem, const void* src, u32 size)
{
	// can memcpy so long as pages aren't crossed
	const u8* sptr = static_cast<const u8*>(src);
	const u8* const sptr_end = sptr + size;
	while (sptr != sptr_end)
	{
		u8* dst = iopVirtMemW<u8>(mem);
		if (!dst)
			return -1;

		const u32 remaining_in_page = std::min(0x1000 - (mem & 0xfff), static_cast<u32>(sptr_end - sptr));
		const int res = std::memcmp(sptr, dst, remaining_in_page);
		if (res != 0)
			return res;

		sptr += remaining_in_page;
		mem += remaining_in_page;
	}

	return 0;
}

bool iopMemSafeReadBytes(u32 mem, void* dst, u32 size)
{
	// can memcpy so long as pages aren't crossed
	u8* dptr = static_cast<u8*>(dst);
	u8* const dptr_end = dptr + size;
	while (dptr != dptr_end)
	{
		const u8* src = iopVirtMemR<u8>(mem);
		if (!src)
			return false;

		const u32 remaining_in_page = std::min(0x1000 - (mem & 0xfff), static_cast<u32>(dptr_end - dptr));
		std::memcpy(dptr, src, remaining_in_page);
		dptr += remaining_in_page;
		mem += remaining_in_page;
	}

	return true;
}

bool iopMemSafeWriteBytes(u32 mem, const void* src, u32 size)
{
	// can memcpy so long as pages aren't crossed
	const u8* sptr = static_cast<const u8*>(src);
	const u8* const sptr_end = sptr + size;
	while (sptr != sptr_end)
	{
		u8* dst = iopVirtMemW<u8>(mem);
		if (!dst)
			return false;

		const u32 remaining_in_page = std::min(0x1000 - (mem & 0xfff), static_cast<u32>(sptr_end - sptr));
		std::memcpy(dst, sptr, remaining_in_page);
		sptr += remaining_in_page;
		mem += remaining_in_page;
	}

	return true;
}

std::string iopMemReadString(u32 mem, int maxlen)
{
	std::string ret;
	char c;

	while ((c = iopMemRead8(mem++)) && maxlen--)
		ret.push_back(c);

	return ret;
}
