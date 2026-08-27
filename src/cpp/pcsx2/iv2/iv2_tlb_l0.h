// SPDX-License-Identifier: GPL-3.0+
// P101: iv2 — VTLB L0 キャッシュ
//
// Phase M (P101-3): direct-mapped primary + 8-way LRU secondary。
//   1. Primary direct-mapped 64 entries  (vpage hash → 1 compare, O(1))
//   2. Secondary 8-way LRU                (primary miss 時に線形 scan)
//   3. Slow path: vtlb_memRead (handler page) or vtlb_private 経由で install
// Primary hit で branch が 1 つで済むため、L0 hit レイテンシを 5-10 → 2-3 cyc に短縮。

#pragma once

#include "common/Pcsx2Defs.h"
#include "vtlbDef.h"

namespace iv2 {

constexpr u32 TLB_L0_WAYS = 8;
// [P101-6 L0 sizing experiment]
// 64 entries (BITS=6): baseline, 2:1 conflict on PS2 32MB RAM working set
// 128 (BITS=7): 1:1, current test
// 256 (BITS=8): 2× capacity, reduces capacity misses too
// 512 (BITS=9): diminishing returns, watch L1D pressure
constexpr u32 TLB_L0_PRI_BITS = 8;                         // 256 entries (A/B 比較で +2.2% 採用)
constexpr u32 TLB_L0_PRI_SIZE = 1u << TLB_L0_PRI_BITS;
constexpr u32 TLB_L0_PRI_MASK = TLB_L0_PRI_SIZE - 1;

struct alignas(64) TlbL0Entry {
    u32  vpage;         // virtual page (addr >> 12); 0xFFFFFFFFu = invalid
    uptr host_ptr;      // page base host pointer
};

// 8-way LRU secondary (既存実装)
extern TlbL0Entry g_tlb_l0_r[TLB_L0_WAYS];   // read cache
extern TlbL0Entry g_tlb_l0_w[TLB_L0_WAYS];   // write cache (handler page は入れない)
extern u32 g_tlb_l0_lru_r;
extern u32 g_tlb_l0_lru_w;

// Direct-mapped primary (Phase M 新設)
extern TlbL0Entry g_tlb_l0_rp[TLB_L0_PRI_SIZE];
extern TlbL0Entry g_tlb_l0_wp[TLB_L0_PRI_SIZE];

// 全 flush (vtlb_VMap 系呼び出し時)
void FlushTlbL0();

// Slow path (vtlb_memRead 経由) + 結果を L0 にインストール
u8  TlbL0_Read8Slow(u32 addr);
u16 TlbL0_Read16Slow(u32 addr);
u32 TlbL0_Read32Slow(u32 addr);
u64 TlbL0_Read64Slow(u32 addr);
void TlbL0_Read128Slow(u32 addr, void* dst);     // 16-byte align 必須
void TlbL0_Write8Slow(u32 addr, u8 v);
void TlbL0_Write16Slow(u32 addr, u16 v);
void TlbL0_Write32Slow(u32 addr, u32 v);
void TlbL0_Write64Slow(u32 addr, u64 v);
void TlbL0_Write128Slow(u32 addr, const void* src);

// Secondary 8-way に hit したら primary にも promote して次回 O(1) hit にする
static __attribute__((always_inline)) uptr TlbL0_SecondaryLookupR(u32 vpage)
{
    for (u32 i = 0; i < TLB_L0_WAYS; i++) {
        if (g_tlb_l0_r[i].vpage == vpage) {
            const uptr host_ptr = g_tlb_l0_r[i].host_ptr;
            // promote to primary
            const u32 idx = vpage & TLB_L0_PRI_MASK;
            g_tlb_l0_rp[idx].vpage = vpage;
            g_tlb_l0_rp[idx].host_ptr = host_ptr;
            return host_ptr;
        }
    }
    return 0;
}

static __attribute__((always_inline)) uptr TlbL0_SecondaryLookupW(u32 vpage)
{
    for (u32 i = 0; i < TLB_L0_WAYS; i++) {
        if (g_tlb_l0_w[i].vpage == vpage) {
            const uptr host_ptr = g_tlb_l0_w[i].host_ptr;
            const u32 idx = vpage & TLB_L0_PRI_MASK;
            g_tlb_l0_wp[idx].vpage = vpage;
            g_tlb_l0_wp[idx].host_ptr = host_ptr;
            return host_ptr;
        }
    }
    return 0;
}

// ホットパス — いずれも force_inline。primary → secondary → slow。
static __attribute__((always_inline)) u8 iv2_read8(u32 addr)
{
    const u32 vpage = addr >> 12;
    const u32 idx = vpage & TLB_L0_PRI_MASK;
    if (__builtin_expect(g_tlb_l0_rp[idx].vpage == vpage, 1))
        return *(const u8*)(g_tlb_l0_rp[idx].host_ptr + (addr & 0xFFF));
    const uptr host_ptr = TlbL0_SecondaryLookupR(vpage);
    if (host_ptr) return *(const u8*)(host_ptr + (addr & 0xFFF));
    return TlbL0_Read8Slow(addr);
}

static __attribute__((always_inline)) u16 iv2_read16(u32 addr)
{
    if (__builtin_expect(addr & 1, 0)) return TlbL0_Read16Slow(addr);
    const u32 vpage = addr >> 12;
    const u32 idx = vpage & TLB_L0_PRI_MASK;
    if (__builtin_expect(g_tlb_l0_rp[idx].vpage == vpage, 1))
        return *(const u16*)(g_tlb_l0_rp[idx].host_ptr + (addr & 0xFFF));
    const uptr host_ptr = TlbL0_SecondaryLookupR(vpage);
    if (host_ptr) return *(const u16*)(host_ptr + (addr & 0xFFF));
    return TlbL0_Read16Slow(addr);
}

static __attribute__((always_inline)) u32 iv2_read32(u32 addr)
{
    if (__builtin_expect(addr & 3, 0)) return TlbL0_Read32Slow(addr);
    const u32 vpage = addr >> 12;
    const u32 idx = vpage & TLB_L0_PRI_MASK;
    if (__builtin_expect(g_tlb_l0_rp[idx].vpage == vpage, 1))
        return *(const u32*)(g_tlb_l0_rp[idx].host_ptr + (addr & 0xFFF));
    const uptr host_ptr = TlbL0_SecondaryLookupR(vpage);
    if (host_ptr) return *(const u32*)(host_ptr + (addr & 0xFFF));
    return TlbL0_Read32Slow(addr);
}

static __attribute__((always_inline)) u64 iv2_read64(u32 addr)
{
    if (__builtin_expect(addr & 7, 0)) return TlbL0_Read64Slow(addr);
    const u32 vpage = addr >> 12;
    const u32 idx = vpage & TLB_L0_PRI_MASK;
    if (__builtin_expect(g_tlb_l0_rp[idx].vpage == vpage, 1))
        return *(const u64*)(g_tlb_l0_rp[idx].host_ptr + (addr & 0xFFF));
    const uptr host_ptr = TlbL0_SecondaryLookupR(vpage);
    if (host_ptr) return *(const u64*)(host_ptr + (addr & 0xFFF));
    return TlbL0_Read64Slow(addr);
}

// 128-bit load (PS2 LQ / LQC2 に使用)。addr が 16-byte align 必須 (そうでなければ slow path)。
// dst は 16-byte align された 128-bit 領域 (u128 / VECTOR)。
static __attribute__((always_inline)) void iv2_read128(u32 addr, void* dst)
{
    if (__builtin_expect(addr & 0xF, 0)) { TlbL0_Read128Slow(addr, dst); return; }
    const u32 vpage = addr >> 12;
    const u32 idx = vpage & TLB_L0_PRI_MASK;
    if (__builtin_expect(g_tlb_l0_rp[idx].vpage == vpage, 1)) {
        __builtin_memcpy(dst, (const void*)(g_tlb_l0_rp[idx].host_ptr + (addr & 0xFFF)), 16);
        return;
    }
    const uptr host_ptr = TlbL0_SecondaryLookupR(vpage);
    if (host_ptr) { __builtin_memcpy(dst, (const void*)(host_ptr + (addr & 0xFFF)), 16); return; }
    TlbL0_Read128Slow(addr, dst);
}

static __attribute__((always_inline)) void iv2_write8(u32 addr, u8 v)
{
    const u32 vpage = addr >> 12;
    const u32 idx = vpage & TLB_L0_PRI_MASK;
    if (__builtin_expect(g_tlb_l0_wp[idx].vpage == vpage, 1)) {
        *(u8*)(g_tlb_l0_wp[idx].host_ptr + (addr & 0xFFF)) = v;
        return;
    }
    const uptr host_ptr = TlbL0_SecondaryLookupW(vpage);
    if (host_ptr) { *(u8*)(host_ptr + (addr & 0xFFF)) = v; return; }
    TlbL0_Write8Slow(addr, v);
}

static __attribute__((always_inline)) void iv2_write16(u32 addr, u16 v)
{
    if (__builtin_expect(addr & 1, 0)) { TlbL0_Write16Slow(addr, v); return; }
    const u32 vpage = addr >> 12;
    const u32 idx = vpage & TLB_L0_PRI_MASK;
    if (__builtin_expect(g_tlb_l0_wp[idx].vpage == vpage, 1)) {
        *(u16*)(g_tlb_l0_wp[idx].host_ptr + (addr & 0xFFF)) = v;
        return;
    }
    const uptr host_ptr = TlbL0_SecondaryLookupW(vpage);
    if (host_ptr) { *(u16*)(host_ptr + (addr & 0xFFF)) = v; return; }
    TlbL0_Write16Slow(addr, v);
}

static __attribute__((always_inline)) void iv2_write32(u32 addr, u32 v)
{
    if (__builtin_expect(addr & 3, 0)) { TlbL0_Write32Slow(addr, v); return; }
    const u32 vpage = addr >> 12;
    const u32 idx = vpage & TLB_L0_PRI_MASK;
    if (__builtin_expect(g_tlb_l0_wp[idx].vpage == vpage, 1)) {
        *(u32*)(g_tlb_l0_wp[idx].host_ptr + (addr & 0xFFF)) = v;
        return;
    }
    const uptr host_ptr = TlbL0_SecondaryLookupW(vpage);
    if (host_ptr) { *(u32*)(host_ptr + (addr & 0xFFF)) = v; return; }
    TlbL0_Write32Slow(addr, v);
}

static __attribute__((always_inline)) void iv2_write64(u32 addr, u64 v)
{
    if (__builtin_expect(addr & 7, 0)) { TlbL0_Write64Slow(addr, v); return; }
    const u32 vpage = addr >> 12;
    const u32 idx = vpage & TLB_L0_PRI_MASK;
    if (__builtin_expect(g_tlb_l0_wp[idx].vpage == vpage, 1)) {
        *(u64*)(g_tlb_l0_wp[idx].host_ptr + (addr & 0xFFF)) = v;
        return;
    }
    const uptr host_ptr = TlbL0_SecondaryLookupW(vpage);
    if (host_ptr) { *(u64*)(host_ptr + (addr & 0xFFF)) = v; return; }
    TlbL0_Write64Slow(addr, v);
}

// 128-bit store (PS2 SQ / SQC2 に使用)。src は 16-byte align された 128-bit 領域。
static __attribute__((always_inline)) void iv2_write128(u32 addr, const void* src)
{
    if (__builtin_expect(addr & 0xF, 0)) { TlbL0_Write128Slow(addr, src); return; }
    const u32 vpage = addr >> 12;
    const u32 idx = vpage & TLB_L0_PRI_MASK;
    if (__builtin_expect(g_tlb_l0_wp[idx].vpage == vpage, 1)) {
        __builtin_memcpy((void*)(g_tlb_l0_wp[idx].host_ptr + (addr & 0xFFF)), src, 16);
        return;
    }
    const uptr host_ptr = TlbL0_SecondaryLookupW(vpage);
    if (host_ptr) { __builtin_memcpy((void*)(host_ptr + (addr & 0xFFF)), src, 16); return; }
    TlbL0_Write128Slow(addr, src);
}

} // namespace iv2
