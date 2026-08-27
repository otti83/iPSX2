// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0

#include "common/Console.h"
#include "MTVU.h"
#include "SaveState.h"
#include "vtlb.h"
#include "R3000A.h"
#include "R5900.h"
#include "VUmicro.h"
#include "common/Darwin/DarwinMisc.h"
#include "common/Assertions.h"
#include <unistd.h>
#include <atomic>

#if !defined(iPSX2_REAL_REC)
void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr, u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits, bool is_signed, bool is_load, bool is_fpr)
{
  pxFailRel("Not implemented.");
}
#endif

// [iPSX2] Note: vu1Thread and CpuMicroVU0/CpuMicroVU1 are now
// defined in x86/microVU.cpp (real microVU JIT implementation).

#if !defined(iPSX2_REAL_REC)
R3000Acpu psxRec;
R5900cpu recCpu;
#endif

// [iPSX2] Diagnostics Setup
static void Stub_Reserve() { }
static void Stub_Reset() { }
static void Stub_Shutdown() { }
static s32 Stub_ExecuteBlock(s32 cycles) { return cycles; }
static void Stub_Clear(u32, u32) { }

struct RecStubsInit {
    RecStubsInit() {
        const char* s = getenv("iPSX2_REC_DIAG");
        bool diag_enabled = (s && s[0] == '1');

        if (diag_enabled) {
             fprintf(stderr, "@@CFG@@ iPSX2_REC_DIAG=1\n");
        }

#if !defined(iPSX2_REAL_REC)
        // Init R3000A (IOP) - Minimal stubs
        psxRec.Reserve = Stub_Reserve;
        psxRec.Reset = Stub_Reset;
        psxRec.ExecuteBlock = Stub_ExecuteBlock;
        psxRec.Clear = Stub_Clear;
        psxRec.Shutdown = Stub_Shutdown;

        // Init R5900 (EE) - Partial
        recCpu.Reserve = Stub_Reserve;
        recCpu.Reset = Stub_Reset;

        // [iPSX2] Diagnostic Hook for recCpu.Execute
        if (diag_enabled) {
            recCpu.Execute = []() {
                static std::atomic<bool> s_logged_enter{false};
                if (!s_logged_enter.exchange(true)) {
                    Console.WriteLn("@@REC_EXEC_ENTER@@ pc=0x%08x tid=%d", cpuRegs.pc, (int)getpid());
                }

                static std::atomic<bool> s_logged_fail{false};
                if (!s_logged_fail.exchange(true)) {
                    Console.WriteLn("@@REC_BLOCK_FAIL@@ pc=0x%08x reason=STUB", cpuRegs.pc);
                }
            };
        } else {
            recCpu.Execute = [](){}; // Void function
        }
#endif

        // [iPSX2] New Config Log
        fprintf(stderr, "@@CFG@@ iPSX2_REAL_REC=%d iPSX2_USE_REC_STUBS=%d\n",
            #if defined(iPSX2_REAL_REC)
            1, 0
            #else
            0, 1
            #endif
        );

    }
} g_RecStubsInit;

namespace R5900 {
namespace Dynarec {
namespace OpcodeImpl {
#if !defined(iPSX2_REAL_REC)
    void recMULTU1() { pxFailRel("recMULTU1 Not implemented"); }
#endif
}
}
}

// [iPSX2] Note: vuJITFreeze() is now provided by x86/microVU.cpp
// with real microVU state serialization.
