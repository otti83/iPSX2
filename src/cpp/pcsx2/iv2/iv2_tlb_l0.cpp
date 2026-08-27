// SPDX-License-Identifier: GPL-3.0+
// P101: iv2 — VTLB L0 キャッシュ実装

#include "PrecompiledHeader.h"
#include "Common.h"
#include "iv2/iv2_tlb_l0.h"
#include "vtlbDef.h"
#include "vtlb.h"
#include "Memory.h"
#include "Hw.h"
#include "Config.h"

using namespace vtlb_private;

namespace iv2 {

alignas(64) TlbL0Entry g_tlb_l0_r[TLB_L0_WAYS];
alignas(64) TlbL0Entry g_tlb_l0_w[TLB_L0_WAYS];
alignas(64) TlbL0Entry g_tlb_l0_rp[TLB_L0_PRI_SIZE];
alignas(64) TlbL0Entry g_tlb_l0_wp[TLB_L0_PRI_SIZE];
u32 g_tlb_l0_lru_r = 0;
u32 g_tlb_l0_lru_w = 0;

void FlushTlbL0()
{
    for (u32 i = 0; i < TLB_L0_WAYS; i++) {
        g_tlb_l0_r[i].vpage = 0xFFFFFFFFu;
        g_tlb_l0_r[i].host_ptr = 0;
        g_tlb_l0_w[i].vpage = 0xFFFFFFFFu;
        g_tlb_l0_w[i].host_ptr = 0;
    }
    for (u32 i = 0; i < TLB_L0_PRI_SIZE; i++) {
        g_tlb_l0_rp[i].vpage = 0xFFFFFFFFu;
        g_tlb_l0_rp[i].host_ptr = 0;
        g_tlb_l0_wp[i].vpage = 0xFFFFFFFFu;
        g_tlb_l0_wp[i].host_ptr = 0;
    }
}

// 初期化: 必ず呼ばれる前に最初の slow path で 1 entry 目に入る前提で、
// 起動時に invalid マーカー設定
static struct L0Init { L0Init() { FlushTlbL0(); } } s_l0_init_dummy;

// ─── slow path: vtlb 経由で読み、ハンドラでないページを L0 に入れる ───
static __attribute__((always_inline)) bool try_install_r(u32 addr, u32 vpage)
{
    auto vmv = vtlbdata.vmap[vpage];
    if (vmv.isHandler(addr)) return false;
    // ページ全体の host pointer を算出して install
    // vmv.raw() + addr が page 内の host pointer
    // page base は vmv.raw() + (addr & ~0xFFF) と等価
    uptr base = (uptr)((sptr)vmv.raw() + (sptr)(addr & ~0xFFFu));
    u32 victim = g_tlb_l0_lru_r++ & (TLB_L0_WAYS - 1);
    g_tlb_l0_r[victim].vpage = vpage;
    g_tlb_l0_r[victim].host_ptr = base;
    // Phase M: primary direct-mapped にも同時 install
    const u32 pidx = vpage & TLB_L0_PRI_MASK;
    g_tlb_l0_rp[pidx].vpage = vpage;
    g_tlb_l0_rp[pidx].host_ptr = base;
    return true;
}

static __attribute__((always_inline)) bool try_install_w(u32 addr, u32 vpage)
{
    auto vmv = vtlbdata.vmap[vpage];
    if (vmv.isHandler(addr)) return false;
    uptr base = (uptr)((sptr)vmv.raw() + (sptr)(addr & ~0xFFFu));
    u32 victim = g_tlb_l0_lru_w++ & (TLB_L0_WAYS - 1);
    g_tlb_l0_w[victim].vpage = vpage;
    g_tlb_l0_w[victim].host_ptr = base;
    const u32 pidx = vpage & TLB_L0_PRI_MASK;
    g_tlb_l0_wp[pidx].vpage = vpage;
    g_tlb_l0_wp[pidx].host_ptr = base;
    return true;
}

u8 TlbL0_Read8Slow(u32 addr)
{
    const u32 vpage = addr >> 12;
    if (try_install_r(addr, vpage))
        return *(const u8*)(g_tlb_l0_r[(g_tlb_l0_lru_r - 1) & (TLB_L0_WAYS - 1)].host_ptr + (addr & 0xFFF));
    return vtlb_memRead<u8>(addr);
}

u16 TlbL0_Read16Slow(u32 addr)
{
    const u32 vpage = addr >> 12;
    if ((addr & 1) == 0 && try_install_r(addr, vpage))
        return *(const u16*)(g_tlb_l0_r[(g_tlb_l0_lru_r - 1) & (TLB_L0_WAYS - 1)].host_ptr + (addr & 0xFFF));
    return vtlb_memRead<u16>(addr);
}

u32 TlbL0_Read32Slow(u32 addr)
{
    const u32 vpage = addr >> 12;
    if ((addr & 3) == 0 && try_install_r(addr, vpage))
        return *(const u32*)(g_tlb_l0_r[(g_tlb_l0_lru_r - 1) & (TLB_L0_WAYS - 1)].host_ptr + (addr & 0xFFF));

    // [P101-5 speedhack] INTC_STAT spin detection.
    // Games heavily spin-poll INTC_STAT (0x1000F000). The full vtlb handler
    // dispatch for each read is expensive. PCSX2 JIT (recVTLB.cpp:1161) does
    // the same shortcut when EmuConfig.Speedhacks.IntcStat is enabled.
    if (EmuConfig.Speedhacks.IntcStat && addr == INTC_STAT)
        return psHu32(INTC_STAT);

    return vtlb_memRead<u32>(addr);
}

u64 TlbL0_Read64Slow(u32 addr)
{
    const u32 vpage = addr >> 12;
    if ((addr & 7) == 0 && try_install_r(addr, vpage))
        return *(const u64*)(g_tlb_l0_r[(g_tlb_l0_lru_r - 1) & (TLB_L0_WAYS - 1)].host_ptr + (addr & 0xFFF));
    return vtlb_memRead<u64>(addr);
}

void TlbL0_Write8Slow(u32 addr, u8 v)
{
    const u32 vpage = addr >> 12;
    if (try_install_w(addr, vpage)) {
        *(u8*)(g_tlb_l0_w[(g_tlb_l0_lru_w - 1) & (TLB_L0_WAYS - 1)].host_ptr + (addr & 0xFFF)) = v;
        return;
    }
    vtlb_memWrite<u8>(addr, v);
}

void TlbL0_Write16Slow(u32 addr, u16 v)
{
    const u32 vpage = addr >> 12;
    if ((addr & 1) == 0 && try_install_w(addr, vpage)) {
        *(u16*)(g_tlb_l0_w[(g_tlb_l0_lru_w - 1) & (TLB_L0_WAYS - 1)].host_ptr + (addr & 0xFFF)) = v;
        return;
    }
    vtlb_memWrite<u16>(addr, v);
}

void TlbL0_Write32Slow(u32 addr, u32 v)
{
    const u32 vpage = addr >> 12;
    if ((addr & 3) == 0 && try_install_w(addr, vpage)) {
        *(u32*)(g_tlb_l0_w[(g_tlb_l0_lru_w - 1) & (TLB_L0_WAYS - 1)].host_ptr + (addr & 0xFFF)) = v;
        return;
    }
    vtlb_memWrite<u32>(addr, v);
}

void TlbL0_Write64Slow(u32 addr, u64 v)
{
    const u32 vpage = addr >> 12;
    if ((addr & 7) == 0 && try_install_w(addr, vpage)) {
        *(u64*)(g_tlb_l0_w[(g_tlb_l0_lru_w - 1) & (TLB_L0_WAYS - 1)].host_ptr + (addr & 0xFFF)) = v;
        return;
    }
    vtlb_memWrite<u64>(addr, v);
}

void TlbL0_Read128Slow(u32 addr, void* dst)
{
    const u32 vpage = addr >> 12;
    if ((addr & 0xF) == 0 && try_install_r(addr, vpage)) {
        __builtin_memcpy(dst,
            (const void*)(g_tlb_l0_r[(g_tlb_l0_lru_r - 1) & (TLB_L0_WAYS - 1)].host_ptr + (addr & 0xFFF)),
            16);
        return;
    }
    // handler page or misaligned → 64-bit twice via vtlb_memRead
    u64 lo = vtlb_memRead<u64>(addr);
    u64 hi = vtlb_memRead<u64>(addr + 8);
    ((u64*)dst)[0] = lo;
    ((u64*)dst)[1] = hi;
}

void TlbL0_Write128Slow(u32 addr, const void* src)
{
    const u32 vpage = addr >> 12;
    if ((addr & 0xF) == 0 && try_install_w(addr, vpage)) {
        __builtin_memcpy(
            (void*)(g_tlb_l0_w[(g_tlb_l0_lru_w - 1) & (TLB_L0_WAYS - 1)].host_ptr + (addr & 0xFFF)),
            src, 16);
        return;
    }
    const u64 lo = ((const u64*)src)[0];
    const u64 hi = ((const u64*)src)[1];
    vtlb_memWrite<u64>(addr, lo);
    vtlb_memWrite<u64>(addr + 8, hi);
}

} // namespace iv2
