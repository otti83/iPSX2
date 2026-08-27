// SPDX-License-Identifier: GPL-3.0+
// P101-4 — iv2 interp-aware Async VU1 (MTVU re-design)
//
// Drives the iv2 VU1 *interpreter* (iv2_vu1Exec, the bit-exact NEON path) on a
// dedicated worker thread, gated by env `iPSX2_IV2_ASYNC_VU1` (default OFF).
//
// Why this is NOT the historical (forbidden) MTVU path:
//   - It does NOT set EmuConfig.Cpu.Recompiler.EnableVU1 / THREAD_VU1, so the
//     Gif_Unit road-hole barrier (active when EnableVU1==false) stays ON.
//   - The worker runs the INTERPRETER, never microVU ARM JIT. The historical
//     "road hole" was proven (V24/V25) to be a microVU ARM-emit precision bug
//     (EFU/FDIV 17 ops), not an MTVU-synchronisation bug.
//
// Modes (env value):
//   "1" = TRUE ASYNC (Stage C): EE kicks VU1 and continues; the worker runs the
//         microprogram concurrently. EE-side effects raised by the worker
//         (hwIntcIrq, VPU_STAT, XGKICK→GIF, vif1Regs) are deferred / atomic /
//         routed through the existing MTVU GIF machinery. Real speedup.
//   "2" = FULL SYNC (Stage B): EE enqueues and immediately blocks until the
//         worker finishes (WaitForEmpty). Zero overlap but bit-exact — kept as a
//         regression-validation mode.
//   unset/"0" = OFF (synchronous, default).

#pragma once

#include "common/Pcsx2Defs.h"

// 0 = off, 1 = true async, 2 = full sync. `g_iv2_async_vu1` = (mode != 0).
extern int  g_iv2_async_vu1_mode;
extern bool g_iv2_async_vu1;          // any async mode active
extern bool g_iv2_async_vu1_overlap;  // mode 1 only (true async) — used by GIF routing
// True only while the worker thread is mid-execution of a microprogram in TRUE
// ASYNC mode. iv2_vu1Exec / _vuXGKICKTransfer read this to decide whether to
// defer EE-side effects. (Worker-thread-only writer, same-thread reader.)
extern bool g_iv2_vu1_on_worker;

// [P101-4 Stage C overlap] VIF TOP/ITOP snapshot captured by the EE at kick time.
// The double-buffer overlap lets the EE keep unpacking the next buffer (which
// mutates vif1Regs.top/itop) while the worker runs the just-kicked program, so the
// worker's XTOP/XITOP must read the kick-time snapshot, not the live (racing) regs.
extern u32 g_iv2_async_top;
extern u32 g_iv2_async_itop;

// Lifecycle (tied to VMManager Initialize/ShutdownCPUProviders). Self-guarded.
void iv2AsyncVu1_Open();
void iv2AsyncVu1_Close();
bool iv2AsyncVu1_IsActive();

// Sync barrier: block until the worker has drained all pending VU1 work, then
// drain deferred EE effects so the EE observes a coherent VU1/GIF/INTC state.
void iv2AsyncVu1_WaitIdle();

// Drain deferred EE effects WITHOUT waiting for the worker (cheap, early-outs if
// nothing pending). Call at EE event points so worker-raised interrupts fire.
void iv2AsyncVu1_DrainDeferred();

// Stage B (mode 2) dispatch: enqueue a VU1 run of `cycles` and block.
void iv2AsyncVu1_RunCycles(u32 cycles);

// Stage C (mode 1) kick (depth>1 feed-ahead): enqueue {pc,top,itop} and return
// immediately — the worker sets VU1 state from this, so the EE never drains at the
// kick. Backpressure waits only for one ring slot (not a full drain).
void iv2AsyncVu1_Kick(s32 pc, u32 top, u32 itop);

// Worker-raised deferred-effect setters (called on the worker thread).
void iv2AsyncVu1_DeferIntcVu1();   // D/T-bit: hwIntcIrq(INTC_VU1)
void iv2AsyncVu1_DeferVEW();       // E-bit:   vif1Regs.stat.VEW = false
void iv2AsyncVu1_DeferVGW();       // XGKICK:  vif1Regs.stat.VGW = false + CPU_INT(DMAC_VIF1)

// XGKICK byte capture (called on the worker thread). The worker copies the GS
// packet bytes here instead of touching the GIF directly; the EE replays them via
// the normal sync GIF path at drain time (DrainDeferred). Returns false if the
// capture buffer is momentarily full (caller logs + drops — never blocks, so the
// worker can never deadlock against the EE drain).
bool iv2AsyncVu1_BufferXGKick(const void* data, u32 size);
