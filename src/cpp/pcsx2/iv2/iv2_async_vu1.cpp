// SPDX-License-Identifier: GPL-3.0+
// P101-4 — iv2 interp-aware Async VU1 (MTVU re-design). See iv2_async_vu1.h.

#include "PrecompiledHeader.h"
#include "Common.h"
#include "VMManager.h"
#include "MTVU.h"
#include "Gif_Unit.h"
#include "common/Threading.h"

#include "iv2/iv2_async_vu1.h"
#include "iv2/iv2_vu.h"

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>

int  g_iv2_async_vu1_mode = []{
    const char* e = std::getenv("iPSX2_IV2_ASYNC_VU1");
    if (!e) return 0;
    if (e[0] == '1' && e[1] == '\0') return 1;   // true async
    if (e[0] == '2' && e[1] == '\0') return 2;   // full sync (bit-exact regression)
    return 0;                                     // "0"/unset/other = off
}();
bool g_iv2_async_vu1         = (g_iv2_async_vu1_mode != 0);
bool g_iv2_async_vu1_overlap = (g_iv2_async_vu1_mode == 1);
bool g_iv2_vu1_on_worker     = false;   // worker-thread-only writer
u32  g_iv2_async_top         = 0;       // VIF TOP snapshot at kick (overlap)
u32  g_iv2_async_itop        = 0;       // VIF ITOP snapshot at kick (overlap)

namespace {

// Single-producer (EE) / single-consumer (worker) command ring. Small per P101-4
// P1 — the interp worker keeps pace with the EE, so no large backlog forms; a
// full ring triggers EE-side backpressure (WaitIdle), never a deadlock.
constexpr u32 RING_SIZE = 16;   // power of two
static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE must be pow2");

// mode 2 uses {cycles}; mode 1 (true-async, depth>1 feed-ahead) uses {pc,top,itop}
// and the worker sets VU1 state itself so the EE never drains at the kick.
struct Job { u32 cycles; s32 pc; u32 top; u32 itop; };
Job s_ring[RING_SIZE];
alignas(__cachelinesize) std::atomic<u32> s_write_pos{0};   // EE thread only
alignas(__cachelinesize) std::atomic<u32> s_read_pos{0};    // worker thread only

Threading::WorkSema s_sema;
Threading::Thread   s_thread;
std::atomic<bool>   s_shutdown{false};
bool                s_open = false;   // EE-thread-only lifecycle flag

// Deferred EE effects raised by the worker, drained on the EE thread.
std::atomic<u32> s_deferred{0};
enum { DEF_INTC_VU1 = 1u, DEF_VEW = 2u, DEF_VGW = 4u };

// VU cycle history for cycle-stealing (mirror of MTVU vuCycles[4]).
std::atomic<u32> s_vu_cycles[4]{};
u32              s_vu_cycle_idx = 0;   // worker-thread-only

// XGKICK byte capture (EE-replay model). Fixed-slot SPSC ring: the worker copies
// each GS packet's bytes into a slot + publishes a descriptor; the EE drains
// (ReplayXGKick) and submits them via the normal sync GIF path. One XGKICK
// transfer is <= 16KB (road-hole barrier caps at EOP). Drained at every WaitIdle
// (= per kick) + per EE block, so one program's output fits and the worker never
// blocks (no deadlock against the EE drain).
constexpr u32 XGK_SLOT  = 16 * 1024;   // max bytes per XGKICK transfer
constexpr u32 XGK_DESC  = 512;         // ring depth (power of two)
static_assert((XGK_DESC & (XGK_DESC - 1)) == 0, "XGK_DESC pow2");
alignas(16) u8 s_xgk_buf[XGK_DESC * XGK_SLOT];
u32 s_xgk_size[XGK_DESC];
alignas(__cachelinesize) std::atomic<u32> s_xgk_w{0};   // worker
alignas(__cachelinesize) std::atomic<u32> s_xgk_r{0};   // EE
std::atomic<u64> s_xgk_dropped{0};

std::atomic<u64> s_xgk_captured{0};
std::atomic<u64> s_xgk_replayed{0};
std::atomic<u64> s_kick_count{0};
std::atomic<u64> s_waitidle_count{0};   // [DIAG] EE full-drain count (feed-ahead killer)
std::atomic<u64> s_depth_sum{0};        // [DIAG] sum of ring depth at each kick
std::atomic<u32> s_depth_max{0};        // [DIAG] max ring depth seen at kick

void ReplayXGKick()
{
    u32 r = s_xgk_r.load(std::memory_order_relaxed);
    const u32 w = s_xgk_w.load(std::memory_order_acquire);
    while (r != w)
    {
        const u32 slot = r & (XGK_DESC - 1);
        // Submit via the normal sync GIF path (THREAD_VU1 false, mode!=overlap):
        // identical to the proven Stage B XGKICK flow, just from a copied buffer.
        gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &s_xgk_buf[slot * XGK_SLOT], s_xgk_size[slot], true);
        s_xgk_replayed.fetch_add(1, std::memory_order_relaxed);
        ++r;
        s_xgk_r.store(r, std::memory_order_release);
    }
}

void WorkerLoop()
{
    Threading::SetNameOfCurrentThread("iv2 Async VU1");
    Threading::SetCurrentThreadAsHighPriorityEmulation();   // [P101-11] QoS (only when async VU1 enabled)

    for (;;)
    {
        s_sema.WaitForWork();
        if (s_shutdown.load(std::memory_order_acquire))
            break;

        while (s_read_pos.load(std::memory_order_relaxed) !=
               s_write_pos.load(std::memory_order_acquire))
        {
            const u32 rp = s_read_pos.load(std::memory_order_relaxed);
            const Job job = s_ring[rp & (RING_SIZE - 1)];

            if (g_iv2_async_vu1_mode == 1)
            {
                // TRUE ASYNC (depth>1 feed-ahead): the worker owns VU1 state setup
                // so the EE never drains at the kick. Set TPC + the TOP/ITOP
                // snapshot for this kick (XTOP/XITOP read these while on_worker).
                g_iv2_async_top  = job.top;
                g_iv2_async_itop = job.itop;
                if (job.pc != -1)
                    VU1.VI[REG_TPC].UL = (u32)job.pc & 0x7FF;
                VU1.cycle = cpuRegs.cycle;
                const u32 c0 = VU1.cycle;
                g_iv2_vu1_on_worker = true;
                CpuIv2VU1Inst.ExecuteSync(vu1RunCycles);
                g_iv2_vu1_on_worker = false;

                s_vu_cycles[s_vu_cycle_idx & 3].store(VU1.cycle - c0, std::memory_order_release);
                s_vu_cycle_idx++;
                // XGKICK output was captured into s_xgk_buf during execution; the
                // EE replays it at the next drain. No MTGS coordination here.
            }
            else
            {
                // FULL SYNC (mode 2): EE is parked in WaitForEmpty; direct effects
                // are race-free and bit-exact (Stage B).
                CpuIv2VU1Inst.ExecuteSync(job.cycles);
            }

            s_read_pos.store(rp + 1, std::memory_order_release);
        }
    }

    s_sema.Kill();
}

} // anonymous namespace

void iv2AsyncVu1_Open()
{
    if (!g_iv2_async_vu1 || s_open)
        return;

    s_write_pos.store(0, std::memory_order_relaxed);
    s_read_pos.store(0, std::memory_order_relaxed);
    s_deferred.store(0, std::memory_order_relaxed);
    s_shutdown.store(false, std::memory_order_release);
    s_sema.Reset();
    s_thread.SetStackSize(VMManager::EMU_THREAD_STACK_SIZE);
    s_thread.Start([]{ WorkerLoop(); });
    s_open = true;

    Console.WriteLn("@@VU_IV2_ASYNC@@ worker started mode=%d (%s, iPSX2_IV2_ASYNC_VU1)",
        g_iv2_async_vu1_mode, g_iv2_async_vu1_mode == 1 ? "true-async Stage C" : "full-sync Stage B");
}

void iv2AsyncVu1_Close()
{
    if (!s_open)
        return;

    s_shutdown.store(true, std::memory_order_release);
    s_sema.NotifyOfWork();
    s_thread.Join();
    s_open = false;

    Console.WriteLn("@@VU_IV2_ASYNC@@ worker stopped");
}

bool iv2AsyncVu1_IsActive()
{
    return s_open;
}

void iv2AsyncVu1_DrainDeferred()
{
    if (g_iv2_async_vu1_mode != 1 || !s_open)
        return;
    // Submit the worker's captured XGKICK packets via the normal sync GIF path
    // (handles GS SIGNAL/FINISH/LABEL directly on the EE — no deferred GS irqs).
    ReplayXGKick();

    // [DIAG] periodic stats — confirm capture/replay flow (env-gated, removable).
    {
        static u64 s_dbg = 0;
        if ((++s_dbg & 0xFFFFFF) == 0) {
            const u64 k = s_kick_count.load(std::memory_order_relaxed);
            Console.WriteLn("@@VU_IV2_ASYNC@@ kicks=%llu drop=%llu waitidle=%llu avgdepth=%.2f maxdepth=%u",
                (unsigned long long)k,
                (unsigned long long)s_xgk_dropped.load(std::memory_order_relaxed),
                (unsigned long long)s_waitidle_count.load(std::memory_order_relaxed),
                k ? (double)s_depth_sum.load(std::memory_order_relaxed) / (double)k : 0.0,
                s_depth_max.load(std::memory_order_relaxed));
        }
    }

    if (s_deferred.load(std::memory_order_acquire) == 0)
        return;
    const u32 d = s_deferred.exchange(0, std::memory_order_acq_rel);
    if (d & DEF_INTC_VU1)
        hwIntcIrq(INTC_VU1);
    if (d & DEF_VEW)
        vif1Regs.stat.VEW = false;
    if (d & DEF_VGW)
    {
        // VGW (VIF waiting for GIF) is an EE-thread bitfield; the worker only
        // requests this check. Replay the original _vuXGKICKTransfer semantics.
        if (vif1Regs.stat.VGW)
        {
            vif1Regs.stat.VGW = false;
            CPU_INT(DMAC_VIF1, 8);
        }
    }
}

void iv2AsyncVu1_WaitIdle()
{
    if (!s_open)
        return;
    if (g_iv2_async_vu1_mode == 1)
        s_waitidle_count.fetch_add(1, std::memory_order_relaxed);
    // Spin-drain the worker's captured XGKICK packets while it finishes. If the EE
    // simply parked in WaitForEmpty, a long program could overrun the capture ring
    // (the EE is its only consumer). Draining here keeps it bounded + submits the
    // path1 packets promptly.
    while (s_read_pos.load(std::memory_order_acquire) != s_write_pos.load(std::memory_order_relaxed))
    {
        ReplayXGKick();
        std::this_thread::yield();
    }
    s_sema.WaitForEmpty();
    iv2AsyncVu1_DrainDeferred();
}

static u32 iv2AsyncVu1_GetAvgCycles()
{
    return (s_vu_cycles[0].load(std::memory_order_acquire) +
            s_vu_cycles[1].load(std::memory_order_acquire) +
            s_vu_cycles[2].load(std::memory_order_acquire) +
            s_vu_cycles[3].load(std::memory_order_acquire)) >> 2;
}

void iv2AsyncVu1_RunCycles(u32 cycles)
{
    // mode 2 (full sync). Precondition: g_iv2_async_vu1 && s_open (checked by caller).
    const u32 wp = s_write_pos.load(std::memory_order_relaxed);
    s_ring[wp & (RING_SIZE - 1)] = Job{ cycles, -1, 0, 0 };
    s_write_pos.store(wp + 1, std::memory_order_release);
    s_sema.NotifyOfWork();
    s_sema.WaitForEmpty();   // full sync
}

void iv2AsyncVu1_Kick(s32 pc, u32 top, u32 itop)
{
    // mode 1 (true async, depth>1 feed-ahead). The worker sets VU1 state from the
    // command, so the EE does NOT drain here — it queues and keeps running, letting
    // the worker stay continuously busy (the real MTVU overlap).

    // Backpressure: wait only for ONE ring slot to free (one worker kick to finish),
    // draining captured XGKICK meanwhile so the worker never blocks. Not a full
    // drain — that would defeat the feed-ahead.
    while ((s_write_pos.load(std::memory_order_relaxed) -
            s_read_pos.load(std::memory_order_acquire)) >= RING_SIZE)
    {
        ReplayXGKick();
        std::this_thread::yield();
    }

    const u32 wp = s_write_pos.load(std::memory_order_relaxed);
    const u32 depth = wp - s_read_pos.load(std::memory_order_acquire);   // [DIAG]
    s_depth_sum.fetch_add(depth, std::memory_order_relaxed);
    { u32 m = s_depth_max.load(std::memory_order_relaxed); if (depth > m) s_depth_max.store(depth, std::memory_order_relaxed); }
    s_ring[wp & (RING_SIZE - 1)] = Job{ vu1RunCycles, pc, top, itop };
    s_write_pos.store(wp + 1, std::memory_order_release);
    s_sema.NotifyOfWork();
    s_kick_count.fetch_add(1, std::memory_order_relaxed);

    // Cycle stealing (mirror MTVU; ~0 with EECycleSkip default 0 = same cpuRegs.cycle
    // behaviour as sync INSTANT_VU1, so async timing matches the synchronous path).
    const u32 cyc  = std::max(iv2AsyncVu1_GetAvgCycles(), 4u);
    const u32 skip = std::min(cyc, 3000u);
    cpuRegs.cycle += skip * EmuConfig.Speedhacks.EECycleSkip;
    VU0.cycle     += skip * EmuConfig.Speedhacks.EECycleSkip;
}

void iv2AsyncVu1_DeferIntcVu1() { s_deferred.fetch_or(DEF_INTC_VU1, std::memory_order_release); }
void iv2AsyncVu1_DeferVEW()     { s_deferred.fetch_or(DEF_VEW,      std::memory_order_release); }
void iv2AsyncVu1_DeferVGW()     { s_deferred.fetch_or(DEF_VGW,      std::memory_order_release); }

bool iv2AsyncVu1_BufferXGKick(const void* data, u32 size)
{
    // Worker thread. Copy the GS packet bytes into a fixed slot for EE replay.
    // Never blocks: if the ring is momentarily full (EE drain behind), drop +
    // count — that cannot happen in practice (drained every kick) but guarantees
    // the worker can never deadlock waiting on the EE.
    if (size == 0 || size > XGK_SLOT)
    {
        s_xgk_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const u32 w = s_xgk_w.load(std::memory_order_relaxed);
    // Backpressure (zero-drop): wait for the EE to free a ring slot. Safe from
    // deadlock — the EE always drains (per EE block + WaitIdle spin-drain), and
    // its WaitIdle keeps draining until this program finishes, so a free slot is
    // guaranteed to appear. A dropped GS packet (even one FINISH) would stall the
    // game, so we never drop on a full ring.
    while (w - s_xgk_r.load(std::memory_order_acquire) >= XGK_DESC)
        std::this_thread::yield();
    const u32 slot = w & (XGK_DESC - 1);
    std::memcpy(&s_xgk_buf[slot * XGK_SLOT], data, size);
    s_xgk_size[slot] = size;
    s_xgk_w.store(w + 1, std::memory_order_release);
    s_xgk_captured.fetch_add(1, std::memory_order_relaxed);
    return true;
}
