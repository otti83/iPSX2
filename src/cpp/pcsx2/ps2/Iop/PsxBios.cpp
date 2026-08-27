// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "R3000A.h"
#include "IopMem.h"

#include "fmt/format.h"

static std::string psxout_buf;

// This filtering should almost certainly be done in the console classes instead
static std::string psxout_last;
static unsigned psxout_repeat;

static void flush_stdout(bool closing = false)
{
    while (!psxout_buf.empty()) {
        size_t linelen = psxout_buf.find_first_of("\n\0", 0, 2);
        if (linelen == std::string::npos) {
            if (!closing)
                return;
        } else
            psxout_buf[linelen++] = '\n';
        if (linelen != 1) {
            if (!psxout_buf.compare(0, linelen, psxout_last))
                psxout_repeat++;
            else {
                if (psxout_repeat) {
                    iopConLog(fmt::format("[{} more]\n", psxout_repeat));
                    psxout_repeat = 0;
                }
                psxout_last = psxout_buf.substr(0, linelen);
                iopConLog(ShiftJIS_ConvertString(psxout_last.data()));
            }
        }
        psxout_buf.erase(0, linelen);
    }
    if (closing && psxout_repeat) {
        iopConLog(fmt::format("[{} more]\n", psxout_repeat));
        psxout_repeat = 0;
    }
}

void psxBiosReset()
{
    flush_stdout(true);
}

// Called for PlayStation BIOS calls at 0xA0, 0xB0 and 0xC0 in kernel reserved memory (seemingly by actually calling those addresses)
// Returns true if we internally process the call, not that we're likely to do any such thing
bool psxBiosCall()
{
    // TODO: Tracing
    // TODO (maybe, psx is hardly a priority): HLE framework

    const u32 biosKey = ((psxRegs.pc << 4) & 0xf00) | (psxRegs.GPR.n.t1 & 0xff);

    // [P12] @@PSX_BIOS_TRACE@@ — PS1 BIOS A0/B0/C0 callトレース (cap=500)
    // 目的: interpretermodeでの BIOS 自然シーケンスを記録し、JIT との差異を特定
    // Removal condition: JIT modeで BIOS browser描画after confirmed
    {
        static int s_bios_trace_n = 0;
        if (s_bios_trace_n < 500) {
            const char tbl = (psxRegs.pc & 0x1fffffffU) == 0xa0 ? 'A'
                           : (psxRegs.pc & 0x1fffffffU) == 0xb0 ? 'B' : 'C';
            ++s_bios_trace_n;
            Console.WriteLn("@@PSX_BIOS_TRACE@@ n=%d %c0[%02x] a0=%08x a1=%08x v0=%08x ra=%08x cyc=%u",
                s_bios_trace_n, tbl, psxRegs.GPR.n.t1 & 0xff,
                psxRegs.GPR.n.a0, psxRegs.GPR.n.a1,
                psxRegs.GPR.n.v0, psxRegs.GPR.n.ra, psxRegs.cycle);
        }
    }

    // [P12] HLE: A0[0xa1] SystemErrorBootOrDiskFailure
    // SCPH-70000 BIOS does not implement this (self-referencing trampoline → infinite loop).
    // PS1DRV calls it after CD-ROM check fails (no disc).
    // HLE: return immediately so IOP can proceed to browser display.
    // Removal condition: IOP が A0[0xa1] loopを脱出し BIOS browser描画after confirmed
    if (biosKey == 0xaa1) {
        static int s_a0a1_cnt = 0;
        if (++s_a0a1_cnt <= 5) {
            Console.WriteLn("@@HLE_A0_0xA1@@ n=%d SystemErrorBootOrDiskFailure a0=%08x a1=%08x ra=%08x pc=%08x",
                s_a0a1_cnt, psxRegs.GPR.n.a0, psxRegs.GPR.n.a1, psxRegs.GPR.n.ra, psxRegs.pc);
        }
        psxRegs.GPR.n.v0 = 0;
        psxRegs.pc = psxRegs.GPR.n.ra;
        return true;
    }

    switch (biosKey) {
        case 0xa03:
        case 0xb35:
            // write(fd, data, size)
            {
                int fd = psxRegs.GPR.n.a0;
                if (fd != 1)
                    return false;

                u32 data = psxRegs.GPR.n.a1;
                u32 size = psxRegs.GPR.n.a2;
                while (size--)
                    psxout_buf.push_back(iopMemRead8(data++));
                flush_stdout(false);
                return false;
            }
        case 0xa09:
        case 0xb3b:
            // putc(c, fd)
            if (psxRegs.GPR.n.a1 != 1)
                return false;
            [[fallthrough]];
        // fd=1, fall through to putchar
        case 0xa3c:
        case 0xb3d:
            // putchar(c)
            psxout_buf.push_back((char)psxRegs.GPR.n.a0);
            flush_stdout(false);
            return false;
        case 0xa3e:
        case 0xb3f:
            // puts(s)
            {
                u32 str = psxRegs.GPR.n.a0;
                while (char c = iopMemRead8(str++))
                    psxout_buf.push_back(c);
                psxout_buf.push_back('\n');
                flush_stdout(false);
                return false;
            }
    }

    return false;
}
