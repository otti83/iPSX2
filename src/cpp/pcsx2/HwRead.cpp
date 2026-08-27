// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "Hardware.h"
#include "IopHw.h"
#include "ps2/HwInternal.h"
#include "ps2/eeHwTraceLog.inl"

#include "ps2/pgif.h"

using namespace R5900;

// [TEMP_DIAG] @@INTCHACK@@ counter — Removal condition: VIF1 gap root causeafter identified
u32 g_intchack_fire_cnt = 0;
u32 g_intchack_call_cnt = 0;

static __fi void IntCHackCheck()
{
	g_intchack_call_cnt++;
	// Sanity check: To protect from accidentally "rewinding" the cyclecount
	// on the few times nextBranchCycle can be behind our current cycle.
	s32 diff = cpuRegs.nextEventCycle - cpuRegs.cycle;
	if (diff > 0 && (cpuRegs.cycle - cpuRegs.lastEventCycle) > 8) {
		g_intchack_fire_cnt++;
		cpuRegs.cycle = cpuRegs.nextEventCycle;
	}
}

template< uint page > RETURNS_R128 _hwRead128(u32 mem);

template< uint page, bool intcstathack >
mem32_t _hwRead32_impl(u32 mem)
{
	pxAssume( (mem & 0x03) == 0 );

	switch( page )
	{
		case 0x00:	return rcntRead32<0x00>( mem );
		case 0x01:	return rcntRead32<0x01>( mem );

		case 0x02:	return ipuRead32( mem );

		case 0x03:
			if (mem >= EEMemoryMap::VIF0_Start)
			{
				if(mem >= EEMemoryMap::VIF1_Start)
					return vifRead32<1>(mem);
				else
					return vifRead32<0>(mem);
			}
			return dmacRead32<0x03>( mem );

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{
			// [Ps2Confirm] Reading from FIFOs using non-128 bit reads is a complete mystery.
			// No game is known to attempt such a thing (yay!), so probably nothing for us to
			// worry about.  Chances are, though, doing so is "legal" and yields some sort
			// of reproducible behavior.  Candidate for real hardware testing.
			// Current assumption: Reads 128 bits and discards the unused portion.

			r128 out128 = _hwRead128<page>(mem & ~0x0f);
			return reinterpret_cast<u32*>(&out128)[(mem >> 2) & 0x3];
		}
		break;

		case 0x0f:
		{
			// INTC_STAT shortcut for heavy spinning.
			// Performance Note: Visual Studio handles this best if we just manually check for it here,
			// outside the context of the switch statement below.  This is likely fixed by PGO also,
			// but it's an easy enough conditional to account for anyways.

			if (mem == INTC_STAT)
			{
				// Disable INTC hack when in PS1 mode as it seems to break games.
				if (intcstathack && !(psxHu32(HW_ICFG) & (1 << 3))) IntCHackCheck();
				return psHu32(INTC_STAT);
			}

            // [iPSX2] Force PS2 Mode (Clear bit 3 of ICFG)
            if (mem == HW_ICFG) {
                u32 val = psHu32(HW_ICFG);
                static bool s_force_checked = false;
                static bool s_force_enabled = false;
                if (!s_force_checked) {
                    const char* env = std::getenv("iPSX2_FORCE_PS2MODE");
                    s_force_enabled = (env && env[0] == '1');
                    s_force_checked = true;
                }
                
                if (s_force_enabled) {
                    u32 orig = val;
                    val &= ~8; // Clear bit 3 (PlayStation 2 Mode)
                    static bool s_log_once = false;
                    if (!s_log_once) {
                        Console.WriteLn("@@MODE450_OVR@@ addr=bf801450 orig=%08x new=%08x bit3=%d enabled=1", 
                            orig, val, (val >> 3) & 1);
                        s_log_once = true;
                    }
                }
                return val;
            }

			// todo: psx mode: this is new
			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
				return PGIFr((mem & 0x1FFFFFFF));
			}

			// WARNING: this code is never executed anymore due to previous condition.
			// It requires investigation of what to do.
			if ((mem & 0x1000ff00) == 0x1000f300)
			{
				int ret = 0;
				u32 sif2fifosize = std::min(sif2.fifo.size, 7);

				switch (mem & 0xf0)
				{
				case 0x00:
					ret = psxHu32(0x1f801814);
					break;
				case 0x80:
#if PSX_EXTRALOGS
					DevCon.Warning("FIFO Size %x", sif2fifosize);
#endif
					ret = psHu32(mem) | (sif2fifosize << 16);
					if (sif2.fifo.size > 0) ret |= 0x80000000;
					break;
				case 0xc0:
					ReadFifoSingleWord();
					ret = psHu32(mem);
					break;
				case 0xe0:
					//ret = 0xa000e1ec;
					if (sif2.fifo.size > 0)
					{
						ReadFifoSingleWord();
						ret = psHu32(mem);
					}
					else ret = 0;
					break;
				}
#if PSX_EXTRALOGS
				DevCon.Warning("SBUS read %x value sending %x", mem, ret);
#endif
				return ret;


			}
			/*if ((mem & 0x1000ff00) == 0x1000f200)
			{
				if((mem & 0xffff) != 0xf230)DevCon.Warning("SBUS read %x value sending %x", mem, psHu32(mem));
			}*/
			switch( mem )
			{
				case SIO_LSR:
					// [iter215] BIOS putchar polls SIO_LSR bit5 (THRE) for TX ready.
					// PCSX2 processes TX writes instantly, so always report ready.
					return 0x60; // THRE(bit5) + TEMT(bit6) = TX ready + TX empty

				case SIO_ISR:

				// [iter218] BIOS at 9FC433F0 polls SIO_TXFIFO bit15 waiting for TX ready.
				// PCSX2 does not emulate SIO TX, so always report FIFO empty (0).
				case SIO_TXFIFO:
				{
					// [iter219] TEMP_DIAG: confirm SIO_TXFIFO handler is reached
					static u32 s_txfifo_cnt = 0;
					if (s_txfifo_cnt < 5) {
						Console.WriteLn("@@SIO_TXFIFO_HIT@@ n=%u pc=%08x mem=%08x", s_txfifo_cnt, cpuRegs.pc, mem);
						s_txfifo_cnt++;
					}
					return 0;
				}

				case 0x1000f410:
				case MCH_RICM:
					return 0;

				case SBUS_F230:
				{
					// [iter672] @@EE_SBUS_F230_READ@@ – cap 8→30, bit18 追跡
					// Removal condition: SMFLG bit18 差異のroot causeafter identified
					static u32 s_f230_n = 0;
					static u32 s_f230_total = 0;
					static bool s_bit18_first_read = false;
					++s_f230_total;
					const u32 val = psHu32(SBUS_F230);
					if (s_f230_n < 30) {
						++s_f230_n;
						Console.WriteLn("@@EE_SBUS_F230_READ@@ n=%u total=%u pc=0x%08x f230=0x%08x",
							s_f230_n, s_f230_total, cpuRegs.pc, val);
					} else if (s_f230_total == 10000 || s_f230_total == 100000) {
						Console.WriteLn("@@EE_SBUS_F230_READ@@ total=%u pc=0x%08x f230=0x%08x",
							s_f230_total, cpuRegs.pc, val);
					}
					if (!s_bit18_first_read && (val & 0x40000)) {
						s_bit18_first_read = true;
						Console.WriteLn("@@SMFLG_BIT18_FIRST_READ@@ pc=0x%08x f230=0x%08x total_reads=%u eecyc=%u",
							cpuRegs.pc, val, s_f230_total, cpuRegs.cycle);
					}
					return val;
				}

				case SBUS_F240:
#if PSX_EXTRALOGS
					DevCon.Warning("Read  SBUS_F240  %x ", psHu32(SBUS_F240));
#endif
					return psHu32(SBUS_F240) | 0xF0000102;
				case SBUS_F260:
#if PSX_EXTRALOGS
					DevCon.Warning("Read  SBUS_F260  %x ", psHu32(SBUS_F260));
#endif
					return psHu32(SBUS_F260);
				case MCH_DRD:
					// [iter56] @@MCH_DRD_READ@@ probe: confirm hwRead32 is reached + show psHu32(MCH_RICM).
					// Removal condition: MCH_RICM 書き込み経路確定・after fixed。
					{
						static int s_drd_count = 0;
						if (s_drd_count < 20) {
							s_drd_count++;
							Console.WriteLn("@@MCH_DRD_READ@@ #%d ricm=%08x SA=%03x sdevid=%d",
								s_drd_count, psHu32(MCH_RICM),
								(psHu32(MCH_RICM) >> 16) & 0xFFF, rdram_sdevid);
						}
					}
					if( !((psHu32(MCH_RICM) >> 6) & 0xF) )
					{
						switch ((psHu32(MCH_RICM)>>16) & 0xFFF)
						{
							//MCH_RICM: x:4|SA:12|x:5|SDEV:1|SOP:4|SBC:1|SDEV:5

							case 0x21://INIT
								if(rdram_sdevid < rdram_devices)
								{
									rdram_sdevid++;
									return 0x1F;
								}
							return 0;

							case 0x23://CNFGA
								return 0x0D0D;	//PVER=3 | MVER=16 | DBL=1 | REFBIT=5

							case 0x24://CNFGB
								//0x0110 for PSX  SVER=0 | CORG=8(5x9x7) | SPT=1 | DEVTYP=0 | BYTE=0
								return 0x0090;	//SVER=0 | CORG=4(5x9x6) | SPT=1 | DEVTYP=0 | BYTE=0

							case 0x40://DEVID
								return psHu32(MCH_RICM) & 0x1F;	// =SDEV
						}
					}
				return 0;
			}
		}
		break;
		default: break;
	}
	//Hack for Transformers and Test Drive Unlimited to simulate filling the VIF FIFO
	//It actually stalls VIF a few QW before the end of the transfer, so we need to pretend its all gone
	//else itll take aaaaaaaaages to boot.
	if(mem == (D1_CHCR + 0x10) && CHECK_VIFFIFOHACK)
		return psHu32(mem) + (vif1ch.qwc * 16);

	/*if((mem == GIF_CHCR) && !vif1ch.chcr.STR && gifRegs.stat.M3P && gifRegs.stat.APATH != 3)
	{
		//Hack for Wallace and Gromit Curse Project Zoo - Enabled the mask, then starts a new
		//GIF DMA, the mask never comes off and it won't proceed until this is unset.
		//Unsetting it works too but messes up other PATH3 games.
		//If STR is already unset, it won't make the slightest difference.
		return (psHu32(mem) & ~0x100);
	}*/
	return psHu32(mem);
}

template< uint page, bool intcstathack >
mem32_t _hwRead32(u32 mem)
{
    static bool once = false;
    if (!once) { Console.WriteLn("@@HWMON_HOOK@@ _hwRead32_impl alive"); once = true; }
    
    mem32_t ret = _hwRead32_impl<page, intcstathack>(mem);

    // [TEMP_DIAG][REMOVE_AFTER=EE_9FC41048_T0_HWREAD_ROOTCAUSE_V1]
    // 目的: EE BIOS wait-loop (9FC41048付近) の RCNT0_COUNT 読み値と cycle/pc を直接証拠化し、
    // needed時に限定交互値を返して EE 停滞脱出可否を切り分ける。既定OFF・ログ上限あり。
    if constexpr (page == 0x00)
    {
        static int s_cfg_init = 0;
        static int s_probe_enabled = 0;
        static int s_toggle_enabled = 0;
        static u32 s_n = 0;
        static u32 s_toggle_n = 0;
        static u32 s_prev_cycle = 0;
        if (!s_cfg_init)
        {
            s_probe_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_EE_T0HW_PROBE", false) ? 1 : 0;
            s_toggle_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_EE_T0HW_TOGGLE", false) ? 1 : 0;
            Console.WriteLn("@@CFG@@ iPSX2_EE_T0HW_PROBE=%d iPSX2_EE_T0HW_TOGGLE=%d", s_probe_enabled, s_toggle_enabled);
            s_cfg_init = 1;
        }

        const bool is_rcnt0_count = ((mem & 0xff) == 0x00); // page 0x00 local offset 0 == RCNT0_COUNT
        const bool bios_loop_window = (cpuRegs.pc >= 0x9FC41000u && cpuRegs.pc < 0x9FC41100u);
        if (is_rcnt0_count && bios_loop_window)
        {
            mem32_t out = ret;
            if (s_toggle_enabled)
                out = (s_toggle_n++ & 1u);

            if ((s_probe_enabled || s_toggle_enabled) && s_n < 64)
            {
                Console.WriteLn("@@EE_T0HW@@ n=%u pc=%08x mem=%08x raw=%08x out=%08x cycle=%u dcycle=%u",
                    s_n, cpuRegs.pc, mem, static_cast<u32>(ret), static_cast<u32>(out),
                    cpuRegs.cycle, cpuRegs.cycle - s_prev_cycle);
                s_n++;
            }
            s_prev_cycle = cpuRegs.cycle;
            ret = out;
        }
    }

    g_HwRegRing.Push(cpuRegs.pc, mem, mem, 32, ret, false);
    return ret;
}

template< uint page >
mem32_t hwRead32(u32 mem)
{
	mem32_t retval = _hwRead32<page,false>(mem);
	eeHwTraceLog( mem, retval, true );
    // g_HwRegRing.Push merged into _hwRead32
    
    static bool once = false;
    if (!once) { Console.WriteLn("@@HWMON_HOOK@@ hwRead32 alive"); once = true; }
    
	return retval;
}

mem32_t hwRead32_page_0F_INTC_HACK(u32 mem)
{
	mem32_t retval = _hwRead32<0x0f,true>(mem);
	eeHwTraceLog( mem, retval, true );
    g_HwRegRing.Push(cpuRegs.pc, mem, mem, 32, retval, false);
	return retval;
}

// --------------------------------------------------------------------------------------
//  hwRead8 / hwRead16 / hwRead64 / hwRead128
// --------------------------------------------------------------------------------------

template< uint page >
mem8_t _hwRead8_impl(u32 mem)
{
	u32 ret32 = _hwRead32<page, false>(mem & ~0x03);
	return ((u8*)&ret32)[mem & 0x03];
}

template< uint page >
mem8_t _hwRead8(u32 mem)
{
    mem8_t ret = _hwRead8_impl<page>(mem);
    g_HwRegRing.Push(cpuRegs.pc, mem, mem, 8, ret, false);
    return ret;
}

template< uint page >
mem8_t hwRead8(u32 mem)
{
	mem8_t ret8 = _hwRead8<page>(mem);
	eeHwTraceLog( mem, ret8, true );
	return ret8;
}

template< uint page >
mem16_t _hwRead16_impl(u32 mem)
{
	pxAssume( (mem & 0x01) == 0 );

	u32 ret32 = _hwRead32<page, false>(mem & ~0x03);
	return ((u16*)&ret32)[(mem>>1) & 0x01];
}

template< uint page >
mem16_t _hwRead16(u32 mem)
{
    mem16_t ret = _hwRead16_impl<page>(mem);
    g_HwRegRing.Push(cpuRegs.pc, mem, mem, 16, ret, false);
    return ret;
}

template< uint page >
mem16_t hwRead16(u32 mem)
{
	u16 ret16 = _hwRead16<page>(mem);
	eeHwTraceLog( mem, ret16, true );
	return ret16;
}

mem16_t hwRead16_page_0F_INTC_HACK(u32 mem)
{
	pxAssume( (mem & 0x01) == 0 );

	u32 ret32 = _hwRead32<0x0f, true>(mem & ~0x03);
	u16 ret16 = ((u16*)&ret32)[(mem>>1) & 0x01];

	eeHwTraceLog( mem, ret16, true );
	return ret16;
}

template< uint page >
static u64 _hwRead64_impl(u32 mem)
{
	pxAssume( (mem & 0x07) == 0 );

	switch (page)
	{
		case 0x02:
			return ipuRead64(mem);

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{
			// [Ps2Confirm] Reading from FIFOs using non-128 bit reads is a complete mystery.
			// No game is known to attempt such a thing (yay!), so probably nothing for us to
			// worry about.  Chances are, though, doing so is "legal" and yields some sort
			// of reproducible behavior.  Candidate for real hardware testing.

			// Current assumption: Reads 128 bits and discards the unused portion.

			uint wordpart = (mem >> 3) & 0x1;
			r128 full = _hwRead128<page>(mem & ~0x0f);
			return *(reinterpret_cast<u64*>(&full) + wordpart);
		}
		case 0x0F:
			if ((mem & 0xffffff00) == 0x1000f300)
			{
				DevCon.Warning("64bit read from %x wibble", mem);
				if (mem == 0x1000f3E0)
				{

					ReadFifoSingleWord();
					u32 lo = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 hi = psHu32(0x1000f3E0);
					return static_cast<u64>(lo) | (static_cast<u64>(hi) << 32);
				}
			}
		default: break;
	}

	return static_cast<u64>(_hwRead32<page, false>(mem));
}

template< uint page >
static u64 _hwRead64(u32 mem)
{
    u64 ret = _hwRead64_impl<page>(mem);
    g_HwRegRing.Push(cpuRegs.pc, mem, mem, 64, ret, false);
    return ret;
}

template< uint page >
mem64_t hwRead64(u32 mem)
{
	u64 res = _hwRead64<page>(mem);
	eeHwTraceLog(mem, res, true);
	return res;
}

template< uint page >
RETURNS_R128 _hwRead128_impl(u32 mem)
{
	pxAssume( (mem & 0x0f) == 0 );

	alignas(16) mem128_t result;

	// FIFOs are the only "legal" 128 bit registers, so we Handle them first.
	// All other registers fall back on the 64-bit handler (and from there
	// all non-IPU reads fall back to the 32-bit handler).

	switch (page)
	{
		case 0x05:
			ReadFIFO_VIF1(&result);
			break;

		case 0x07:
			if (mem & 0x10)
				return r128_zero(); // IPUin is write-only
			else
				ReadFIFO_IPUout(&result);
			break;

		case 0x04:
		case 0x06:
			// VIF0 and GIF are write-only.
			// [Ps2Confirm] Reads from these FIFOs (and IPUin) do one of the following:
			// return zero, leave contents of the dest register unchanged, or in some
			// indeterminate state.  The actual behavior probably isn't important.
			return r128_zero();
		case 0x0F:
			// todo: psx mode: this is new
			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
				PGIFrQword((mem & 0x1FFFFFFF), &result);
				break;
			}

			// WARNING: this code is never executed anymore due to previous condition.
			// It requires investigation of what to do.
			if ((mem & 0xffffff00) == 0x1000f300)
			{
				DevCon.Warning("128bit read from %x wibble", mem);
				if (mem == 0x1000f3E0)
				{

					ReadFifoSingleWord();
					u32 part0 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part1 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part2 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part3 = psHu32(0x1000f3E0);
					return r128_from_u32x4(part0, part1, part2, part3);
				}
			}
			break;

		default:
			return r128_from_u64_dup(_hwRead64<page>(mem));
	}
	return r128_load(&result);
}

template< uint page >
RETURNS_R128 _hwRead128(u32 mem)
{
    r128 ret = _hwRead128_impl<page>(mem);
    g_HwRegRing.Push(cpuRegs.pc, mem, mem, 128, *(u64*)&ret, false);
    return ret;
}

template< uint page >
RETURNS_R128 hwRead128(u32 mem)
{
	r128 res = _hwRead128<page>(mem);
	eeHwTraceLog(mem, res, true);
    // Log lower 64 bits only - merged into _hwRead128
	return res;
}

#define InstantizeHwRead(pageidx) \
	template mem8_t hwRead8<pageidx>(u32 mem); \
	template mem16_t hwRead16<pageidx>(u32 mem); \
	template mem32_t hwRead32<pageidx>(u32 mem); \
	template mem64_t hwRead64<pageidx>(u32 mem); \
	template RETURNS_R128 hwRead128<pageidx>(u32 mem); \
	template mem32_t _hwRead32<pageidx, false>(u32 mem);

InstantizeHwRead(0x00);	InstantizeHwRead(0x08);
InstantizeHwRead(0x01);	InstantizeHwRead(0x09);
InstantizeHwRead(0x02);	InstantizeHwRead(0x0a);
InstantizeHwRead(0x03);	InstantizeHwRead(0x0b);
InstantizeHwRead(0x04);	InstantizeHwRead(0x0c);
InstantizeHwRead(0x05);	InstantizeHwRead(0x0d);
InstantizeHwRead(0x06);	InstantizeHwRead(0x0e);
InstantizeHwRead(0x07);	InstantizeHwRead(0x0f);
