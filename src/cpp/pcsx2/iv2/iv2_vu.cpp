// SPDX-License-Identifier: GPL-3.0+
// P101-2 Phase T/V: iv2 VU (CpuIv2VU0/CpuIv2VU1) 実装
//
// CpuIv2VU0: Phase V (構造段階 — vu0Exec 丸委譲で interpreter 等価)。
// CpuIv2VU1: Phase T 真 — Upper opcode を iv2_vu_ops (NEON native) で実行、
//            未対応 opcode は VU1_UPPER_OPCODE table に fallback。
//            env `iPSX2_VU_IV2_OPS_UPPER=1` で native 有効、未設定は従来の vu1Exec 等価。

#include "PrecompiledHeader.h"
#include "Common.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "Gif_Unit.h"
#include "iv2/iv2_vu.h"
#include "iv2/iv2_vu_ops.h"
#include "iv2/iv2_async_vu1.h"   // [P101-4] async VU1 worker dispatch
#include "iv2/iv2_hle.h"          // [P101-10] kernel-HLE framework (registry/self-check/census/snapshot)
#include "iv2/iv2_hle_vumath.h"   // [P101-10] vufp VU float-prim lib (ExecuteSync ST back-solve dump)
#include "Vif.h"                  // [HLE_KERNEL Stage2-b] vif1Regs.top (XTOP source)
#include "Vif_Dma.h"              // [ST RE] vif1.MaskRow (texcoord bias source)
#include "DebugTools/Debug.h"    // [HLE_KERNEL PoC] disVU1MicroUF/LF for hot-program disasm
#include <cstdio>
// [port 2026-05-30] AOT (iv2_aot_vu1*) and Metal (iv2_metal_vu1) paths excluded:
// both proven net-negative / NO-GO in src57 (passthrough -3.5%, codegen -8.8%,
// Metal 200us dispatch floor). Only the Phase T NEON interpreter + decode cache
// + profiler + diff harness are ported.

#include <cfenv>
#include <atomic>
#include <cstdlib>
#include <cstring>          // [bench 2026-05-30] std::memcpy for VU1 snapshot/restore
#include <cmath>            // [ST RE 2026-06-22] std::sqrt for Block-A env-map ERLENG
#include <mach/mach_time.h> // [P101-5 ORCH] fine-grained VU1 stage timing
#include <sys/stat.h>       // [P101-18] mkdir (診断: 相異なる 16KB image の採取)

// [P101-5 ORCH] Per-stage accumulators (ns, via mach_absolute_time).
// Emitted every 30 vsync from Counters.cpp. Enabled by iPSX2_VU_IV2_ORCH_PROF=1.
//
// [P101-5 post-progress] Compile-time gate IV2_ENABLE_ORCH_PROF. When unset
// (default Release), g_orch_prof_enabled is a constexpr false and every
// `if (g_orch_prof_enabled)` branch in iv2_vu1Exec becomes dead code.
#ifndef IV2_ENABLE_ORCH_PROF
#define IV2_ENABLE_ORCH_PROF 0   // [port 2026-05-30] 0 = profiler branches compile out (constexpr false). Set 1 only for bottleneck analysis.
#endif

// [2026-08-06 P101-14] Compile-time gate for the per-instruction RE trace blocks
// (iPSX2_HLE_UVTRACE / iPSX2_HLE_OPTRACE) that sit at the top of iv2_vu1Exec.
//
// Why: those blocks use function-local `static const bool` initialised from
// getenv(). C++11 requires a thread-safe init guard, so clang emits an
// acquire-load (`ldaprb`) + test + branch for EACH of them on EVERY call.
// iv2_vu1Exec is called once per emulated VU1 instruction (~50% of CPU), so the
// ship binary paid 3 acquire-loads + ~26 instructions of dead diagnostic
// preamble per VU1 instruction. Verified by disassembling the shipped binary
// (__ZL11iv2_vu1ExecP6VURegs @0x1008fcac0: ldaprb at +0x28/+0x54/+0x64, real
// work `ldr x8,[x21,#0x108]` only starts at +0x9c), plus the expanded fprintf
// tails inside the hot function hurting icache density.
//
// Removal condition (CLAUDE.md rule 3): these traces are RE tooling reused by
// the J-Phoenix / GTA kernel work, so they are gated, not deleted. Set
// IV2_VU_HOT_DIAG=1 (e.g. -DIV2_VU_HOT_DIAG=1) to bring them back verbatim.
// Same shape as IV2_ENABLE_ORCH_PROF above.
#ifndef IV2_VU_HOT_DIAG
#define IV2_VU_HOT_DIAG 0
#endif

namespace {
    std::atomic<uint64_t> g_orch_ns_fetch{0};
    std::atomic<uint64_t> g_orch_ns_decode{0};
    std::atomic<uint64_t> g_orch_ns_upper_stall{0};
    std::atomic<uint64_t> g_orch_ns_pipes{0};
    std::atomic<uint64_t> g_orch_ns_lower_stall{0};
    std::atomic<uint64_t> g_orch_ns_upper_exec{0};
    std::atomic<uint64_t> g_orch_ns_lower_exec{0};
    std::atomic<uint64_t> g_orch_ns_conflict{0};
    std::atomic<uint64_t> g_orch_ns_finalize{0};
    std::atomic<uint64_t> g_orch_inst_count{0};
#if IV2_ENABLE_ORCH_PROF
    bool g_orch_prof_enabled = false;
    bool g_orch_prof_checked = false;
    double g_orch_ns_per_tick = 1.0;
#else
    static constexpr bool g_orch_prof_enabled = false;
    static constexpr bool g_orch_prof_checked = true;
    static constexpr double g_orch_ns_per_tick = 1.0;
#endif

    static __fi void orch_init_once() {
#if IV2_ENABLE_ORCH_PROF
        if (!g_orch_prof_checked) {
            g_orch_prof_checked = true;
            const char* e = std::getenv("iPSX2_VU_IV2_ORCH_PROF");
            g_orch_prof_enabled = (e && *e == '1');
            mach_timebase_info_data_t tb;
            mach_timebase_info(&tb);
            g_orch_ns_per_tick = (double)tb.numer / (double)tb.denom;
        }
#endif
    }
    static __fi uint64_t orch_now() { return mach_absolute_time(); }
    static __fi uint64_t orch_delta_ns(uint64_t t0, uint64_t t1) {
        return (uint64_t)((t1 - t0) * g_orch_ns_per_tick);
    }
}

// Public getter for Counters.cpp per-frame log.
extern "C" void iv2_vu1_orch_emit(uint32_t vsync);
// [port] iv2_vu1_trace_record_pc removed (PC trace capture not ported)
void iv2_vu1_orch_emit(uint32_t vsync) {
    if (!g_orch_prof_enabled) return;
    static uint64_t p_fetch=0,p_decode=0,p_usta=0,p_pipes=0,p_lsta=0,p_uex=0,p_lex=0,p_conf=0,p_fin=0,p_cnt=0;
    uint64_t c_fetch = g_orch_ns_fetch.load(std::memory_order_relaxed);
    uint64_t c_decode = g_orch_ns_decode.load(std::memory_order_relaxed);
    uint64_t c_usta = g_orch_ns_upper_stall.load(std::memory_order_relaxed);
    uint64_t c_pipes = g_orch_ns_pipes.load(std::memory_order_relaxed);
    uint64_t c_lsta = g_orch_ns_lower_stall.load(std::memory_order_relaxed);
    uint64_t c_uex = g_orch_ns_upper_exec.load(std::memory_order_relaxed);
    uint64_t c_lex = g_orch_ns_lower_exec.load(std::memory_order_relaxed);
    uint64_t c_conf = g_orch_ns_conflict.load(std::memory_order_relaxed);
    uint64_t c_fin = g_orch_ns_finalize.load(std::memory_order_relaxed);
    uint64_t c_cnt = g_orch_inst_count.load(std::memory_order_relaxed);
    uint64_t d_cnt = c_cnt - p_cnt;
    if (d_cnt == 0) return;
    uint64_t d_fetch = c_fetch - p_fetch;
    uint64_t d_decode = c_decode - p_decode;
    uint64_t d_usta = c_usta - p_usta;
    uint64_t d_pipes = c_pipes - p_pipes;
    uint64_t d_lsta = c_lsta - p_lsta;
    uint64_t d_uex = c_uex - p_uex;
    uint64_t d_lex = c_lex - p_lex;
    uint64_t d_conf = c_conf - p_conf;
    uint64_t d_fin = c_fin - p_fin;
    uint64_t tot = d_fetch+d_decode+d_usta+d_pipes+d_lsta+d_uex+d_lex+d_conf+d_fin;
    Console.WriteLn("@@VU1_ORCH@@ vs=%u insts=%llu ns/inst_total=%llu fetch=%llu decode=%llu "
        "ust=%llu pipes=%llu lst=%llu uex=%llu lex=%llu conf=%llu fin=%llu",
        vsync, (unsigned long long)d_cnt,
        (unsigned long long)(tot / (d_cnt?d_cnt:1)),
        (unsigned long long)(d_fetch/(d_cnt?d_cnt:1)),
        (unsigned long long)(d_decode/(d_cnt?d_cnt:1)),
        (unsigned long long)(d_usta/(d_cnt?d_cnt:1)),
        (unsigned long long)(d_pipes/(d_cnt?d_cnt:1)),
        (unsigned long long)(d_lsta/(d_cnt?d_cnt:1)),
        (unsigned long long)(d_uex/(d_cnt?d_cnt:1)),
        (unsigned long long)(d_lex/(d_cnt?d_cnt:1)),
        (unsigned long long)(d_conf/(d_cnt?d_cnt:1)),
        (unsigned long long)(d_fin/(d_cnt?d_cnt:1)));
    // Percent breakdown (of orchestration total, not wall)
    if (tot > 0) {
        Console.WriteLn("@@VU1_ORCH@@ vs=%u pct fetch=%llu decode=%llu ust=%llu pipes=%llu lst=%llu uex=%llu lex=%llu conf=%llu fin=%llu",
            vsync,
            (unsigned long long)(d_fetch*100/tot),
            (unsigned long long)(d_decode*100/tot),
            (unsigned long long)(d_usta*100/tot),
            (unsigned long long)(d_pipes*100/tot),
            (unsigned long long)(d_lsta*100/tot),
            (unsigned long long)(d_uex*100/tot),
            (unsigned long long)(d_lex*100/tot),
            (unsigned long long)(d_conf*100/tot),
            (unsigned long long)(d_fin*100/tot));
    }
    p_fetch=c_fetch; p_decode=c_decode; p_usta=c_usta; p_pipes=c_pipes;
    p_lsta=c_lsta; p_uex=c_uex; p_lex=c_lex; p_conf=c_conf; p_fin=c_fin; p_cnt=c_cnt;
}

// VUmicro internal 流用
extern void vu0Exec(VURegs* VU);
extern void vu1Exec(VURegs* VU);
extern void _vuFlushAll(VURegs* VU);
extern void _vu1ExecUpper(VURegs* VU, u32* ptr);
extern void _vu1ExecLower(VURegs* VU, u32* ptr);

// ────────────────────────────────────────────────────────────
// iv2 native 版 VU1 実行 (interpreter の _vu1Exec を fork)
//
// 元は src/cpp/pcsx2/VU1microInterp.cpp:32 `_vu1Exec`。
// Upper opcode dispatch 箇所のみ `iv2_vu1ExecUpper` に差し替えた。
// その他 (E/D/T flag 処理、hazard capture VFc/VIc、stall pipe、branch/ebit) は完全同一。
// ────────────────────────────────────────────────────────────

namespace CliffDiag {
    extern std::atomic<uint32_t> vu1Ebit;
}



// [P101-3 Phase N-残] VU regs 解析キャッシュ (profile で VU1regs_UPPER_FD_* 等が
// 合計 ~3% CPU を占める)。microprogram 命令が同一なら stall 判定用 _VURegsNum は
// 毎回同じ → TPC (11bit) indexed cache で事前計算値を再利用。
// 自己検証: 保存時の raw code と現 code が一致すれば cache hit。差異あれば再計算。
// microprogram 更新 (VIF MSCAL / XGKICK 到達) 時にも、code 比較だけで検出可能。
struct IV2_VU_RegsCache {
    u32 code_u;
    u32 code_l;
    _VURegsNum uregs;
    _VURegsNum lregs;
    bool valid;
    // [exec-plan 2026-05-30 Codex lever] upper/lower 同ペアの hazard 判定は (uregs,lregs)
    // のみに依存する per-PC 静的値。runtime 385-426 の比較群を decode 時に畳んで O(1) 化。
    u8   haz_fast;    // 1 = upper が VF も CLIP も書かない -> hazard 無し、upper+lower 直行
    u8   haz_discard; // 1 = lower が upper と同 VF/CLIP を書く -> lower の共有書込 discard
    u8   haz_vfreg;   // lower 実行前後で save/restore する VF reg index (0 = 無し)
    u8   haz_vireg;   // 同 VI reg index (0 = 無し; CLIP=18)
};
static IV2_VU_RegsCache g_iv2_vu1_regs_cache[2048];  // 16KB / 8B-per-inst = 2048 slots

bool g_iv2_vu_regs_cache_enabled = true;

// [exec-plan 2026-05-30] runtime に渡す静的 hazard plan (cache entry から copy)。
struct IV2_HazardPlan { u8 fast; u8 discard; u8 vfreg; u8 vireg; };
bool g_iv2_exec_plan_enabled = true;   // false で旧 runtime 判定 path (bench A/B 用)

// [A: VU1 block-flatten — step 1, 2026-06-07] tight-loop executor。
//   CpuIv2VU1::Execute の steady-state ループから Step()/iv2_vu1Exec_entry の call 階層 +
//   UpperEnabled() 毎回判定 + per-instruction VF[0]/VI[0] debug assert (= microbench 非計上の
//   ship-path overhead) を hoist。TPC を毎反復読み直すため branch/jump は旧経路と同一処理
//   = bit-exact by construction。
//   ★ 2026-06-07 default-ON 昇格 (gif packet hash bit-exact + FF10 OP heavy-3D +4.0% +
//      user 視覚確認 OK)。kill-switch = `iPSX2_VU1_FLATTEN=0` で旧 Step() ループ (A/B 用)。
//   step 2 (= 直線 run の predecode + body inline で decode/dispatch/EDTI も hoist) は別 commit。
static const bool g_iv2_vu1_flatten = []{
    const char* e = std::getenv("iPSX2_VU1_FLATTEN");
    return !(e && e[0] == '0' && e[1] == '\0');   // 既定 ON; "0" のみ OFF
}();

void iv2_vu1_regs_cache_invalidate()
{
    for (auto& e : g_iv2_vu1_regs_cache) e.valid = false;
}

// cache miss 時は uregs + lregs 両方計算して保存 (次回 hit で両方 O(1) 読出)。
static __attribute__((noinline)) void iv2_vu1_decode_both_slow(const u32* ptr, IV2_VU_RegsCache& e, u32 tpc)
{
    const u32 saved_code = VU1.code;
    _VURegsNum u, l;
    std::memset(&u, 0, sizeof(u));
    VU1.code = ptr[1];
    VU1regs_UPPER_OPCODE[ptr[1] & 0x3f](&u);
    std::memset(&l, 0, sizeof(l));
    VU1.code = ptr[0];
    VU1regs_LOWER_OPCODE[ptr[0] >> 25](&l);
    VU1.code = saved_code;  // 呼出側が再設定するが念のため保存

    e.code_u = ptr[1];
    e.code_l = ptr[0];
    e.uregs = u;
    e.lregs = l;

    // [exec-plan] static hazard decision を事前計算 (== runtime 385-426 と同一論理)。
    {
        const bool no_vf   = (u.VFwrite == 0);
        const bool no_clip = ((u.VIwrite & (1u << REG_CLIP_FLAG)) == 0);
        u8 discard = 0, vfreg = 0, vireg = 0;
        if (u.VFwrite) {
            if (l.VFwrite == u.VFwrite) discard = 1;
            if (l.VFread0 == u.VFwrite || l.VFread1 == u.VFwrite) vfreg = u.VFwrite;
        }
        if (u.VIwrite & (1u << REG_CLIP_FLAG)) {
            if (l.VIwrite & (1u << REG_CLIP_FLAG)) discard = 1;
            if (l.VIread & (1u << REG_CLIP_FLAG)) vireg = REG_CLIP_FLAG;
        }
        e.haz_fast    = (no_vf && no_clip) ? 1 : 0;
        e.haz_discard = discard;
        e.haz_vfreg   = vfreg;
        e.haz_vireg   = vireg;
    }
    e.valid = true;
}

[[gnu::hot]] static __fi void iv2_vu1_decode_cached(const u32* ptr, _VURegsNum& uregs, _VURegsNum* lregs_opt, u32 tpc, IV2_HazardPlan* plan_out)
{
    if (__builtin_expect(g_iv2_vu_regs_cache_enabled, 1))
    {
        const u32 idx = (tpc >> 3) & 0x7FF;
        IV2_VU_RegsCache& e = g_iv2_vu1_regs_cache[idx];
        if (__builtin_expect(!(e.valid && e.code_u == ptr[1] && e.code_l == ptr[0]), 0))
            iv2_vu1_decode_both_slow(ptr, e, tpc);   // miss: 両方 + plan 計算して書戻し
        uregs = e.uregs;
        if (lregs_opt) *lregs_opt = e.lregs;
        if (plan_out) *plan_out = IV2_HazardPlan{ e.haz_fast, e.haz_discard, e.haz_vfreg, e.haz_vireg };
        return;
    }
    // cache disabled: そのまま decode (plan も inline 計算)
    std::memset(&uregs, 0, sizeof(uregs));
    VU1.code = ptr[1];
    VU1regs_UPPER_OPCODE[ptr[1] & 0x3f](&uregs);
    _VURegsNum ltmp; std::memset(&ltmp, 0, sizeof(ltmp));
    VU1.code = ptr[0];
    VU1regs_LOWER_OPCODE[ptr[0] >> 25](&ltmp);
    if (lregs_opt) *lregs_opt = ltmp;
    if (plan_out) {
        const bool no_vf   = (uregs.VFwrite == 0);
        const bool no_clip = ((uregs.VIwrite & (1u << REG_CLIP_FLAG)) == 0);
        u8 discard = 0, vfreg = 0, vireg = 0;
        if (uregs.VFwrite) {
            if (ltmp.VFwrite == uregs.VFwrite) discard = 1;
            if (ltmp.VFread0 == uregs.VFwrite || ltmp.VFread1 == uregs.VFwrite) vfreg = uregs.VFwrite;
        }
        if (uregs.VIwrite & (1u << REG_CLIP_FLAG)) {
            if (ltmp.VIwrite & (1u << REG_CLIP_FLAG)) discard = 1;
            if (ltmp.VIread & (1u << REG_CLIP_FLAG)) vireg = REG_CLIP_FLAG;
        }
        *plan_out = IV2_HazardPlan{ (u8)((no_vf && no_clip) ? 1 : 0), discard, vfreg, vireg };
    }
}

// [2026-08-06 P101-14] Pointer-returning decode.
//
// iv2_vu1_decode_cached copied both _VURegsNum structs (20 B each) plus the plan
// out of the cache entry on EVERY emulated VU1 instruction. Nothing in the hot
// path mutates them — _vuTest*Stalls / _vuAdd*Stalls only read their _VURegsNum
// argument (verified across VUops.cpp; the only writes to a _VURegsNum are in the
// VU1regs_* decode tables, which run on the cache-miss path) — so the caller can
// just point at the cache entry and skip 40 B of copying per instruction.
//
// The I-bit path used to memset lregs to zero; it now points at this shared
// all-zero struct instead. That is equivalent because VUPIPE_NONE == 0 (VU.h:42),
// so every later `lp->pipe` test behaves exactly as it did after the memset.
alignas(16) static const _VURegsNum s_vu_regs_zero = {};

[[gnu::hot]] static __fi void iv2_vu1_decode_cached_ptr(
    const u32* ptr, u32 tpc,
    const _VURegsNum*& up, const _VURegsNum*& lp, IV2_HazardPlan& plan,
    _VURegsNum& u_scratch, _VURegsNum& l_scratch)
{
    if (__builtin_expect(g_iv2_vu_regs_cache_enabled, 1))
    {
        const u32 idx = (tpc >> 3) & 0x7FF;
        IV2_VU_RegsCache& e = g_iv2_vu1_regs_cache[idx];
        if (__builtin_expect(!(e.valid && e.code_u == ptr[1] && e.code_l == ptr[0]), 0))
            iv2_vu1_decode_both_slow(ptr, e, tpc);
        up = &e.uregs;
        lp = &e.lregs;
        plan = IV2_HazardPlan{ e.haz_fast, e.haz_discard, e.haz_vfreg, e.haz_vireg };
        return;
    }
    // cache disabled (bench A/B path): decode into the caller's scratch storage.
    iv2_vu1_decode_cached(ptr, u_scratch, &l_scratch, tpc, &plan);
    up = &u_scratch;
    lp = &l_scratch;
}

[[gnu::hot]] static __fi void iv2_vu1ExecUpper(VURegs* VU, u32* ptr)
{
    VU->code = ptr[1];
    IdebugUPPER(VU1);
    if (iv2::vu_ops::UpperDispatch(VU))
        return;
    VU1_UPPER_OPCODE[VU->code & 0x3f]();
}

[[gnu::hot]] static __fi void iv2_vu1ExecLower(VURegs* VU, u32* ptr)
{
    VU->code = ptr[0];
    IdebugLOWER(VU1);
    if (iv2::vu_ops::LowerDispatch(VU))
        return;
    VU1_LOWER_OPCODE[VU->code >> 25]();
}

// [ST RE] same-kick capture of first-vert vf18(=ITOF12(uv) pre-MULq) + Q, for direct ST trace.
u32 g_dbg_vf18[3] = {0,0,0}, g_dbg_q = 0; bool g_dbg_done = false;

[[gnu::hot]] static void iv2_vu1Exec(VURegs* VU)
{
    _VURegsNum lregs;   // scratch, only used when the regs cache is disabled
    _VURegsNum uregs;
    const _VURegsNum* up;
    const _VURegsNum* lp;
    u32* ptr;

    // [port] PC trace + BB-AOT dispatch removed (AOT path excluded from port).
    orch_init_once();
    uint64_t _ot_start = g_orch_prof_enabled ? orch_now() : 0;
    uint64_t _ot = _ot_start;

#if IV2_VU_HOT_DIAG
    {
        static const bool s_uvtrace = (std::getenv("iPSX2_HLE_UVTRACE") != nullptr);
        if (__builtin_expect(s_uvtrace && g_hle_hotkick, 0)) {
            const u32 bpc = VU->VI[REG_TPC].UL;   // byte-PC of the instruction about to run
            const u32 TOPq = vif1Regs.top & 0x3ff;
            const u32* hdrm = (u32*)&VU->Mem[TOPq*16];
            const u32 cnt = hdrm[0] & 0xffff, ba = hdrm[1] & 0xffff;
            // At pc=121 (ITOF12) compare the RUNTIME loaded uv (vf17) to the SNAPSHOT the kernel
            // uses (g_hle_snap_mem at the uv load address vi09-3). A mismatch = snapshot staleness
            // (kernel reads stale uv) => the self-check ST mismatch is an artifact, not a real
            // native-only error. cnt!=8 focuses on the suspected-bad batches.
            // Same-kick first-vert capture: vf18 at pc=132 = ITOF12(uv0) BEFORE MULq, + Q register.
            if (bpc == 132*8 && !g_dbg_done) {
                g_dbg_done = true;
                g_dbg_vf18[0]=VU->VF[18].UL[0]; g_dbg_vf18[1]=VU->VF[18].UL[1]; g_dbg_vf18[2]=VU->VF[18].UL[2];
                g_dbg_q = VU->VI[REG_Q].UL;
            }
            // [pc162 RE] pin decode_strip: dump VI regs at XGKICK(pc254) + strip setup(pc163).
            if (bpc == 254*8 || bpc == 163*8 || bpc == 171*8) {
                static int n162 = 0;
                if (n162 < 24) { n162++;
                    FILE* f = std::fopen("/tmp/ipsx2_hle_pc162trace.txt","a");
                    if (f) { std::fprintf(f,
                        "pc=%u top=%u vi01=%u vi09=%u vi10=%u vi12=%u vi13=%u vi14=%u | mem[vi12+0..2].x=%08x %08x %08x\n",
                        bpc/8, vif1Regs.top, VU->VI[1].US[0], VU->VI[9].US[0], VU->VI[10].US[0],
                        VU->VI[12].US[0], VU->VI[13].US[0], VU->VI[14].US[0],
                        ((u32*)&VU->Mem[(VU->VI[12].US[0]&0x3ff)*16])[0],
                        ((u32*)&VU->Mem[((VU->VI[12].US[0]+1)&0x3ff)*16])[0],
                        ((u32*)&VU->Mem[((VU->VI[12].US[0]+2)&0x3ff)*16])[0]);
                        std::fclose(f); }
                }
            }
        }
    }

    // [pc162 ST/ADC RE] One-shot full per-op value trace of a hot kick (env iPSX2_HLE_OPTRACE=1).
    // Logs pc + upper/lower opcode + all VF/ACC/VI + clip/mac flags BEFORE each op, so the env-map
    // ST sub-program (pc~263-532, absent from static-value disasm) can be pinned op-by-op. One-shot
    // via a line cap; default-off, runs only during self-check (g_hle_hotkick) = zero ship impact.
    {
        static const bool s_optrace = (std::getenv("iPSX2_HLE_OPTRACE") != nullptr);
        static int s_optrace_lines = 0;
        static const int s_optrace_cap = []{ const char* e = std::getenv("iPSX2_HLE_OPTRACE_CAP"); return e ? atoi(e) : 11000; }();
        if (__builtin_expect(s_optrace && g_hle_hotkick && s_optrace_lines < s_optrace_cap, 0)) {
            const u32* mp = (u32*)&VU->Micro[VU->VI[REG_TPC].UL];
            FILE* f = std::fopen("/tmp/ipsx2_hle_optrace.txt", s_optrace_lines ? "a" : "w");
            if (f) {
                std::fprintf(f, "PC=%u HI=%08x LO=%08x ACC=%08x,%08x,%08x,%08x CLIP=%06x MAC=%04x VI=",
                    VU->VI[REG_TPC].UL/8, mp[1], mp[0],
                    VU->ACC.UL[0],VU->ACC.UL[1],VU->ACC.UL[2],VU->ACC.UL[3],
                    VU->clipflag & 0xffffff, VU->VI[REG_MAC_FLAG].UL & 0xffff);
                for (int r = 0; r < 16; ++r) std::fprintf(f, "%04x.", VU->VI[r].US[0]);
                for (int r = 0; r < 32; ++r) std::fprintf(f, " vf%02d=%08x,%08x,%08x,%08x",
                    r, VU->VF[r].UL[0],VU->VF[r].UL[1],VU->VF[r].UL[2],VU->VF[r].UL[3]);
                std::fprintf(f, "\n");
                std::fclose(f);
            }
            ++s_optrace_lines;
        }
    }
#endif // IV2_VU_HOT_DIAG

    ptr = (u32*)&VU->Micro[VU->VI[REG_TPC].UL];
    VU->VI[REG_TPC].UL += 8;

    if (ptr[1] & 0x40000000) // E flag
    {
        VU->ebit = 2;
    }
    if (ptr[1] & 0x10000000) // D flag
    {
        if (VU0.VI[REG_FBRST].UL & 0x400)
        {
            // [P101-4 Stage C] On the async worker, VPU_STAT bit is set atomically
            // and the EE interrupt is deferred to the next EE sync point.
            if (__builtin_expect(g_iv2_vu1_on_worker, 0))
            {
                __atomic_fetch_or(&VU0.VI[REG_VPU_STAT].UL, 0x200u, __ATOMIC_RELEASE);
                iv2AsyncVu1_DeferIntcVu1();
            }
            else
            {
                VU0.VI[REG_VPU_STAT].UL |= 0x200;
                hwIntcIrq(INTC_VU1);
            }
            VU->ebit = 1;
        }
    }
    if (ptr[1] & 0x08000000) // T flag
    {
        if (VU0.VI[REG_FBRST].UL & 0x800)
        {
            if (__builtin_expect(g_iv2_vu1_on_worker, 0))
            {
                __atomic_fetch_or(&VU0.VI[REG_VPU_STAT].UL, 0x400u, __ATOMIC_RELEASE);
                iv2AsyncVu1_DeferIntcVu1();
            }
            else
            {
                VU0.VI[REG_VPU_STAT].UL |= 0x400;
                hwIntcIrq(INTC_VU1);
            }
            VU->ebit = 1;
        }
    }

    VU->code = ptr[1];
    if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_fetch.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }
    // [P101-3 N-残] regs cache 経由で事前計算値を利用。
    // tpc は VU->VI[REG_TPC].UL += 8 された直後なので元 TPC は -8。
    const u32 tpc_orig = VU->VI[REG_TPC].UL - 8;
    IV2_HazardPlan plan;   // [exec-plan] cached static hazard 判定
    iv2_vu1_decode_cached_ptr(ptr, tpc_orig, up, lp, plan, uregs, lregs);
    if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_decode.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }

    u32 cyclesBeforeOp = VU1.cycle - 1;

    // [P101-5 Opt①] Inline pipe-skip: _vuTestUpperStalls does nothing unless pipe==FMAC.
    // The stall test for non-FMAC uregs (most NOPs) becomes a function call + switch to no-op.
    // Inline check saves that overhead. Correctness: exactly equivalent (same switch inside func).
    if (__builtin_expect(up->pipe == VUPIPE_FMAC, 0))
        _vuTestUpperStalls(VU, const_cast<_VURegsNum*>(up));
    if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_upper_stall.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }

    // [P101-5 Opt①] Inline pipes-idle fast-exit: _vuTestPipes early-returns when all pipes idle
    // and no XGKICK active. Hoist that check so we skip the function call entirely.
    auto pipes_active_check = [&]() -> bool {
        const bool pipes_idle = (VU->fmaccount == 0) & (VU->ialucount == 0) &
                                (VU->fdiv.enable == 0) & (VU->efu.enable == 0);
        const bool vu1_xgk = (VU == &VU1) && VU1.xgkickenable;
        return !pipes_idle || vu1_xgk;
    };

    if (ptr[1] & 0x80000000) // I Flag (Lower op is a float)
    {
        if (__builtin_expect(pipes_active_check(), 0))
            _vuTestPipes(VU);
        if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_pipes.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }

        if (VU->VIBackupCycles > 0)
            VU->VIBackupCycles -= std::min((u8)(VU1.cycle - cyclesBeforeOp), VU->VIBackupCycles);

        iv2_vu1ExecUpper(VU, ptr);
        if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_upper_exec.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }

        VU->VI[REG_I].UL = ptr[0];
        lp = &s_vu_regs_zero;   // VUPIPE_NONE == 0 => same as the old memset
    }
    else
    {
        VU->code = ptr[0];
        // lregs は上で cached 取得済。ただ cycles は decode 時 0 初期化 expected — 既に cached 済。

        // [P101-5 Opt①] Inline pipe-skip for Lower stalls (FMAC/FDIV/EFU/BRANCH only).
        if (__builtin_expect(lp->pipe != VUPIPE_NONE, 1))
            _vuTestLowerStalls(VU, const_cast<_VURegsNum*>(lp));
        if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_lower_stall.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }
        if (__builtin_expect(pipes_active_check(), 0))
            _vuTestPipes(VU);
        if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_pipes.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }

        if (VU->VIBackupCycles > 0)
            VU->VIBackupCycles -= std::min((u8)(VU1.cycle - cyclesBeforeOp), VU->VIBackupCycles);

        // [exec-plan 2026-05-30 Codex lever] hazard 判定は decode cache に事前計算済 (plan)。
        // runtime の比較群 (旧 422-462) を O(1) plan 参照に置換。dynamic な VF/VI 値
        // save/restore のみ残す。g_iv2_exec_plan_enabled=false で旧 runtime path (bench A/B)。
        if (__builtin_expect(g_iv2_exec_plan_enabled, 1))
        {
            if (__builtin_expect(plan.fast, 1))
            {
                // No hazard possible. Direct: Upper then Lower.
                iv2_vu1ExecUpper(VU, ptr);
                if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_upper_exec.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }
                iv2_vu1ExecLower(VU, ptr);
                if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_lower_exec.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }
            }
            else
            {
                VECTOR _VF, _VFc;
                REG_VI _VI, _VIc;
                const int vfreg = plan.vfreg;
                const int vireg = plan.vireg;
                if (vfreg) _VF = VU->VF[vfreg];
                if (vireg) _VI = VU->VI[vireg];

                iv2_vu1ExecUpper(VU, ptr);
                if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_upper_exec.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }

                if (!plan.discard)
                {
                    if (vfreg) { _VFc = VU->VF[vfreg]; VU->VF[vfreg] = _VF; }
                    if (vireg) { _VIc = VU->VI[vireg]; VU->VI[vireg] = _VI; }
                    if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_conflict.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }

                    iv2_vu1ExecLower(VU, ptr);
                    if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_lower_exec.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }

                    if (vfreg) VU->VF[vfreg] = _VFc;
                    if (vireg) VU->VI[vireg] = _VIc;
                    if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_conflict.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }
                }
            }
        }
        else
        {
            // [旧 runtime path] hazard を毎回判定 (plan path との bench A/B 専用、bit-exact 同一)。
            const bool upper_no_vf_write   = (up->VFwrite == 0);
            const bool upper_no_clip_write = ((up->VIwrite & (1 << REG_CLIP_FLAG)) == 0);
            if (__builtin_expect(upper_no_vf_write && upper_no_clip_write, 1))
            {
                iv2_vu1ExecUpper(VU, ptr);
                if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_upper_exec.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }
                iv2_vu1ExecLower(VU, ptr);
                if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_lower_exec.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }
            }
            else
            {
                VECTOR _VF, _VFc;
                REG_VI _VI, _VIc;
                int vfreg = 0, vireg = 0, discard = 0;
                if (up->VFwrite)
                {
                    if (lp->VFwrite == up->VFwrite) discard = 1;
                    if (lp->VFread0 == up->VFwrite || lp->VFread1 == up->VFwrite)
                    { _VF = VU->VF[up->VFwrite]; vfreg = up->VFwrite; }
                }
                if (up->VIwrite & (1 << REG_CLIP_FLAG))
                {
                    if (lp->VIwrite & (1 << REG_CLIP_FLAG)) discard = 1;
                    if (lp->VIread & (1 << REG_CLIP_FLAG))
                    { _VI = VU->VI[REG_CLIP_FLAG]; vireg = REG_CLIP_FLAG; }
                }
                iv2_vu1ExecUpper(VU, ptr);
                if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_upper_exec.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }
                if (discard == 0)
                {
                    if (vfreg) { _VFc = VU->VF[vfreg]; VU->VF[vfreg] = _VF; }
                    if (vireg) { _VIc = VU->VI[vireg]; VU->VI[vireg] = _VI; }
                    if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_conflict.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }
                    iv2_vu1ExecLower(VU, ptr);
                    if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_lower_exec.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }
                    if (vfreg) VU->VF[vfreg] = _VFc;
                    if (vireg) VU->VI[vireg] = _VIc;
                    if (g_orch_prof_enabled) { uint64_t t = orch_now(); g_orch_ns_conflict.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed); _ot = t; }
                }
            }
        }
    }

    // [P101-5 Opt③] Finalize fast-path. _vuClearFMAC is already gated on FMAC pipe.
    // Inline AddUpper/LowerStalls pipe-skip to avoid function call for NOP Upper.
    if (up->pipe == VUPIPE_FMAC || lp->pipe == VUPIPE_FMAC)
        _vuClearFMAC(VU);

    if (__builtin_expect(up->pipe != VUPIPE_NONE, 0))
        _vuAddUpperStalls(VU, const_cast<_VURegsNum*>(up));
    if (__builtin_expect(lp->pipe != VUPIPE_NONE, 1))
        _vuAddLowerStalls(VU, const_cast<_VURegsNum*>(lp));

    if (VU->branch > 0)
    {
        if (VU->branch-- == 1)
        {
            VU->VI[REG_TPC].UL = VU->branchpc;

            if (VU->takedelaybranch)
            {
                VU->branch = 1;
                VU->branchpc = VU->delaybranchpc;
                VU->takedelaybranch = false;
            }
        }
    }

    if (VU->ebit > 0)
    {
        if (VU->ebit-- == 1)
        {
            CliffDiag::vu1Ebit.fetch_add(1, std::memory_order_relaxed);
            VU->VIBackupCycles = 0;
            _vuFlushAll(VU);
            // [P101-4 Stage C] On the async worker, clear the VU1-busy bit
            // atomically and defer the VIF VEW write to the EE thread.
            if (__builtin_expect(g_iv2_vu1_on_worker, 0))
            {
                __atomic_fetch_and(&VU0.VI[REG_VPU_STAT].UL, ~0x100u, __ATOMIC_RELEASE);
                iv2AsyncVu1_DeferVEW();
            }
            else
            {
                VU0.VI[REG_VPU_STAT].UL &= ~0x100;
                vif1Regs.stat.VEW = false;
            }

            if (VU1.xgkickenable)
                _vuXGKICKTransfer(0, true);
            if (INSTANT_VU1)
                VU1.xgkicklastcycle = cpuRegs.cycle;
        }
    }

    if (up->pipe == VUPIPE_FMAC || lp->pipe == VUPIPE_FMAC)
        VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;

    if (g_orch_prof_enabled) {
        uint64_t t = orch_now();
        g_orch_ns_finalize.fetch_add(orch_delta_ns(_ot, t), std::memory_order_relaxed);
        g_orch_inst_count.fetch_add(1, std::memory_order_relaxed);
    }
}

// ─── [P101-15 T1 PoC probe 2026-08-06] per-PC 実行ヒストグラム ────────────────────
//   env `iPSX2_VU1_PCHIST=<program hash hex>` を付けたときだけ、その microprogram の
//   per-PC 実行回数を数える。オフライン変換 (T1) の対象ホットループを、RE 当時の
//   optrace (別シーン・path A のみ) でなく **実測シーン** で特定するため。
//   diagnostic-only / env-gated / ship ループには一切触れない (専用ループを別に持つ)。
//   撤去条件: 変換対象レンジ確定後に削除可 (docs/report P101-15 に結果を残す)。
static u64 g_pchist_hash = 0;      // 監視対象 program hash (0 = off)
static u32 g_pchist[2048];         // per-PC 実行回数
static u64 g_pchist_kicks = 0;
static u64 g_pchist_ops = 0;

// ── [P101-18 locality probe] program 同一性の「粒度」を測る診断 (env iPSX2_VU1_SURVEY 時のみ) ──
// 既存の program 識別は 16KB micro 全体の FNV hash。ゲームが 16KB 内に複数の小 program を
// 置いて個別に差し替えると、実行される命令列が同一でも hash が変わる = 「別 program」に見える。
// ここでは entry PC から**静的に到達できる命令の閉包**だけを hash する。これは T1 が実際に
// 変換すべき範囲そのものであり、T1 の正しさは「変換した PC の命令語が一致すること」
// (t1_verify_program の memcmp) で担保されるので、同一性としてはこちらが正しい粒度。
// 診断専用・env gated・既定 OFF。除去条件 = 粒度の結論が出て報告に反映された時点。
struct Iv2ClosureInfo { u64 key; u32 n; u32 lo; u32 hi; u32 indirect; };

// lower op7 (= lo >> 25) の分岐種別。VU1_LOWER_OPCODE の並びより:
//   0x20 B / 0x21 BAL (無条件) / 0x24 JR / 0x25 JALR (間接) /
//   0x28 IBEQ 0x29 IBNE 0x2C IBLTZ 0x2D IBGTZ 0x2E IBLEZ 0x2F IBGEZ (条件)
static inline int iv2_branch_kind(u32 hi, u32 lo)
{
    if (hi & 0x80000000u) return 0;            // I-bit: lower は即値
    const u32 op7 = lo >> 25;
    if (op7 == 0x20 || op7 == 0x21) return 1;  // 無条件
    if (op7 == 0x24 || op7 == 0x25) return 2;  // 間接 (静的に追えない)
    if (op7 == 0x28 || op7 == 0x29 || op7 == 0x2C || op7 == 0x2D || op7 == 0x2E || op7 == 0x2F) return 3;
    return 0;
}
static inline u32 iv2_branch_target(u32 pc, u32 lo)
{
    // _branchAddr: TPC(= (pc+1)*8) + Imm11*8 を 0x3fff mask
    const s32 imm = (lo & 0x400u) ? (s32)(0xfffffc00u | (lo & 0x3ffu)) : (s32)(lo & 0x3ffu);
    return (u32)((s32)pc + 1 + imm) & 0x7ffu;
}

static void iv2_vu1_closure(const u8* micro, u32 entry_pc, Iv2ClosureInfo* out)
{
    static u8  seen[2048];
    static u16 stack[2048];
    std::memset(seen, 0, sizeof(seen));
    int sp = 0;
    u32 indirect = 0;
    stack[sp++] = (u16)(entry_pc & 0x7ffu);
    while (sp > 0)
    {
        u32 pc = stack[--sp];
        while (pc < 2048 && !seen[pc])
        {
            seen[pc] = 1;
            const u32* w = (const u32*)(micro + pc * 8);
            const u32 lo = w[0], hi = w[1];
            if (hi & 0x40000000u) {                       // E bit: 本命令 + 1 命令で終了
                if (pc + 1 < 2048) seen[pc + 1] = 1;
                break;
            }
            const int bk = iv2_branch_kind(hi, lo);
            if (bk)
            {
                if (pc + 1 < 2048) {                      // delay slot は必ず実行される
                    seen[pc + 1] = 1;
                    // delay slot 自体が直接分岐なら、その飛び先も到達しうる
                    const u32* dw = (const u32*)(micro + (pc + 1) * 8);
                    const int dk = iv2_branch_kind(dw[1], dw[0]);
                    if (dk == 1 || dk == 3) { if (sp < 2048) stack[sp++] = (u16)iv2_branch_target(pc + 1, dw[0]); }
                    else if (dk == 2) indirect = 1;
                }
                if (bk == 2) { indirect = 1; break; }     // JR/JALR: 静的に追えない
                if (sp < 2048) stack[sp++] = (u16)iv2_branch_target(pc, lo);
                if (bk == 1) break;                       // 無条件: fallthrough しない
                pc += 2; continue;                        // 条件: delay slot の次から続行
            }
            ++pc;
        }
    }
    u64 h = 1469598103934665603ULL;
    u32 n = 0, lo_pc = 0, hi_pc = 0; bool first = true;
    for (u32 pc = 0; pc < 2048; ++pc) {
        if (!seen[pc]) continue;
        ++n;
        if (first) { lo_pc = pc; first = false; }
        hi_pc = pc;
        const u8* p = micro + pc * 8;
        h ^= (u64)pc; h *= 1099511628211ULL;
        for (int b = 0; b < 8; ++b) { h ^= p[b]; h *= 1099511628211ULL; }
    }
    out->key = h ? h : 1; out->n = n; out->lo = lo_pc; out->hi = hi_pc; out->indirect = indirect;
}

// 閉包は (16KB image, entry PC) で決まる。kick 毎に歩くと測定を歪めるので直接マップで memo 化する。
static void iv2_vu1_closure_cached(const u8* micro, u64 img_hash, u32 entry_pc, Iv2ClosureInfo* out)
{
    struct Slot { u64 img; u32 pc; bool valid; Iv2ClosureInfo ci; };
    static Slot tab[2048];
    const u32 idx = (u32)((img_hash ^ (img_hash >> 32) ^ ((u64)entry_pc * 2654435761ULL)) & 2047u);
    Slot& s = tab[idx];
    if (s.valid && s.img == img_hash && s.pc == entry_pc) { *out = s.ci; return; }
    iv2_vu1_closure(micro, entry_pc, out);
    s.img = img_hash; s.pc = entry_pc; s.ci = *out; s.valid = true;
}

// [P101-18] **image ごとの** 実行 PC 集合 (iPSX2_VU1_PCHIST=all のとき)。
// 「16KB hash は違うが、そのとき実際に走る命令列は同じ」を判定するには image 単位の
// 実行 PC 集合が要る (全 image を混ぜた union では、別 image が別 routine を同じ番地で
// 走らせているだけの状況と区別できない)。診断専用。
// [2026-08-07] 128 -> 1024。GoW の実 gameplay は 90 秒で 591 image を循環しており、
// 128 本で打ち切ると per-image の ops 集計が全体の 4% しか拾えず窓の最適化が破綻した。
static constexpr int kPchImg = 1024;
static u64 g_pch_img_hash[kPchImg];
static u32 (*g_pch_img)[2048] = nullptr;
static u64 g_pch_img_kicks[kPchImg];
static int g_pch_img_n = 0;
static u32* iv2_pchist_table_for(u64 hash)
{
    if (!g_pch_img) g_pch_img = (u32(*)[2048])std::calloc(kPchImg, sizeof(u32) * 2048);
    if (!g_pch_img) return nullptr;
    for (int i = 0; i < g_pch_img_n; ++i) if (g_pch_img_hash[i] == hash) { ++g_pch_img_kicks[i]; return g_pch_img[i]; }
    if (g_pch_img_n >= kPchImg) return nullptr;
    const int i = g_pch_img_n++;
    g_pch_img_hash[i] = hash; g_pch_img_kicks[i] = 1;
    return g_pch_img[i];
}
static void iv2_pchist_per_image_dump()
{
    if (!g_pch_img_n) return;
    ::mkdir("/tmp/ipsx2_pchist", 0755);
    for (int i = 0; i < g_pch_img_n; ++i) {
        char path[128];
        std::snprintf(path, sizeof(path), "/tmp/ipsx2_pchist/%016llx.txt", (unsigned long long)g_pch_img_hash[i]);
        FILE* f = std::fopen(path, "w");
        if (!f) continue;
        std::fprintf(f, "PCHIST_IMG hash=%016llx kicks=%llu\n",
                     (unsigned long long)g_pch_img_hash[i], (unsigned long long)g_pch_img_kicks[i]);
        std::fprintf(f, "--- RAW pc=count (executed only) ---\n");
        for (int q = 0; q < 2048; ++q) if (g_pch_img[i][q]) std::fprintf(f, "R %d %u\n", q, g_pch_img[i][q]);
        std::fclose(f);
    }
}

// [P101-18] 相異なる 16KB image を採取する (env iPSX2_VU1_IMGDUMP=<最大本数>)。
// 「16KB hash は違うが、実行される PC の命令語は同一」を **オフラインで機械的に検証**するための
// 生データ。survey path でのみ呼ばれる = 既定ビルドの hot path には一切入らない。
static void iv2_vu1_imgdump(const u8* micro, u64 hash)
{
    static const int s_max = []{ const char* e = std::getenv("iPSX2_VU1_IMGDUMP");
                                 return e ? atoi(e) : 0; }();
    if (s_max <= 0) return;
    static u64 seen[1024]; static int nseen = 0;
    if (nseen >= s_max || nseen >= 1024) return;
    for (int i = 0; i < nseen; ++i) if (seen[i] == hash) return;
    seen[nseen++] = hash;
    ::mkdir("/tmp/ipsx2_ucode", 0755);
    char path[128];
    std::snprintf(path, sizeof(path), "/tmp/ipsx2_ucode/%03d_%016llx.bin", nseen - 1, (unsigned long long)hash);
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(micro, 1, 0x4000, f); std::fclose(f); }
}

// [P101-15 T1 PoC] 変換 front-end 入力の吸い出し (probe と同じ env で 1 回だけ)。
//   ①16KB microcode 本体  ②per-PC decode 情報 (uregs/lregs/hazard plan) を実タブレルから。
//   T1 生成器はこの 2 つだけを入力に C++ を吐く (= 手で opcode 表を再実装しない)。
static void iv2_t1_dump_inputs()
{
    {
        FILE* uf = std::fopen("/tmp/ipsx2_t1.ucode", "wb");
        if (uf) { std::fwrite(VU1.Micro, 1, 0x4000, uf); std::fclose(uf); }
    }
    FILE* f = std::fopen("/tmp/ipsx2_t1_decode.txt", "w");
    if (!f) return;
    std::fprintf(f, "# pc hi lo | u.pipe u.VFwrite u.VFwxyzw u.VFr0xyzw u.VFr1xyzw u.VFread0 u.VFread1 u.VIwrite u.VIread u.cycles"
                    " | l.(same) | haz_fast haz_discard haz_vfreg haz_vireg | nexec\n");
    const u32 saved_code = VU1.code;
    for (u32 pc = 0; pc < 2048; ++pc)
    {
        const u32* ptr = (const u32*)&VU1.Micro[pc * 8];
        IV2_VU_RegsCache e{};
        iv2_vu1_decode_both_slow(ptr, e, pc * 8);
        const _VURegsNum& u = e.uregs; const _VURegsNum& l = e.lregs;
        std::fprintf(f, "%u %08x %08x | %u %u %u %u %u %u %u %u %u %d | %u %u %u %u %u %u %u %u %u %d | %u %u %u %u | %u\n",
            pc, ptr[1], ptr[0],
            u.pipe, u.VFwrite, u.VFwxyzw, u.VFr0xyzw, u.VFr1xyzw, u.VFread0, u.VFread1, u.VIwrite, u.VIread, u.cycles,
            l.pipe, l.VFwrite, l.VFwxyzw, l.VFr0xyzw, l.VFr1xyzw, l.VFread0, l.VFread1, l.VIwrite, l.VIread, l.cycles,
            e.haz_fast, e.haz_discard, e.haz_vfreg, e.haz_vireg, g_pchist[pc]);
    }
    VU1.code = saved_code;
    std::fclose(f);
    iv2_vu1_regs_cache_invalidate();   // 上の decode で cache を汚していないが念のため
}

static void iv2_pchist_dump()
{
    FILE* f = std::fopen("/tmp/ipsx2_vu1_pchist.txt", "w");
    if (!f) return;
    std::fprintf(f, "PCHIST hash=%016llx kicks=%llu ops=%llu\n",
                 (unsigned long long)g_pchist_hash,
                 (unsigned long long)g_pchist_kicks, (unsigned long long)g_pchist_ops);
    const double tot = g_pchist_ops ? (double)g_pchist_ops : 1.0;
    // top-40
    bool used[2048] = {false};
    for (int rank = 0; rank < 40; ++rank) {
        int best = -1; u32 bestn = 0;
        for (int p = 0; p < 2048; ++p) if (!used[p] && g_pchist[p] > bestn) { bestn = g_pchist[p]; best = p; }
        if (best < 0) break;
        used[best] = true;
        std::fprintf(f, "  pc=%4d n=%10u (%5.2f%%)\n", best, bestn, 100.0 * bestn / tot);
    }
    // 連続レンジ
    std::fprintf(f, "--- contiguous executed ranges ---\n");
    int p = 0;
    while (p < 2048) {
        if (!g_pchist[p]) { ++p; continue; }
        int a = p; u64 s = 0;
        while (p < 2048 && g_pchist[p]) { s += g_pchist[p]; ++p; }
        std::fprintf(f, "  pc %4d..%4d (%3d insts) ops=%10llu (%5.2f%%)\n",
                     a, p - 1, p - a, (unsigned long long)s, 100.0 * (double)s / tot);
    }
    // [P101-18] 機械可読な全 PC 実行回数 (Scripts/t1_locality.py が読む)。
    std::fprintf(f, "--- RAW pc=count (executed only) ---\n");
    for (int q = 0; q < 2048; ++q) if (g_pchist[q]) std::fprintf(f, "R %d %u\n", q, g_pchist[q]);
    std::fclose(f);
    iv2_pchist_per_image_dump();
}

// [P101-5 Stage D diff harness] Public wrapper for BB AOT diff.
// Single-step the VU1 interpreter. Used by iv2_aot_vu1_bb::run_interp_steps.
extern "C" void iv2_vu1Exec_entry_public(VURegs* VU);
extern "C" void iv2_vu1Exec_entry_public(VURegs* VU)
{
    VU->cycle++;
    iv2_vu1Exec(VU);
}

static void iv2_vu1Exec_entry(VURegs* VU)
{
    VU->cycle++;
    iv2_vu1Exec(VU);

    if (VU->VI[0].UL != 0)
        DbgCon.Error("VI[0] != 0!!!!\n");
    if (VU->VF[0].f.x != 0.0f)
        DbgCon.Error("VF[0].x != 0.0!!!!\n");
    if (VU->VF[0].f.y != 0.0f)
        DbgCon.Error("VF[0].y != 0.0!!!!\n");
    if (VU->VF[0].f.z != 0.0f)
        DbgCon.Error("VF[0].z != 0.0!!!!\n");
    if (VU->VF[0].f.w != 1.0f)
        DbgCon.Error("VF[0].w != 1.0!!!!\n");
}

// ────────────────────────────────────────────────────────────
// VU1 (Phase T)
// ────────────────────────────────────────────────────────────
CpuIv2VU1 CpuIv2VU1Inst;

CpuIv2VU1::CpuIv2VU1()
{
    m_Idx = 1;
    IsInterpreter = true;
    Console.WriteLn("@@VU_IV2_INIT@@ CpuIv2VU1 constructed (Phase T 真実装 scaffolding)");
}

void CpuIv2VU1::Reset()
{
    iv2::vu_ops::Init();
    iv2_vu1_regs_cache_invalidate();   // [flag liveness] clear stale decode/liveness cache
    VU1.fmacwritepos = 0;
    VU1.fmacreadpos  = 0;
    VU1.fmaccount    = 0;
    VU1.ialuwritepos = 0;
    VU1.ialureadpos  = 0;
    VU1.ialucount    = 0;
}

void CpuIv2VU1::SetStartPC(u32 startPC)
{
    VU1.start_pc = startPC;
}

void CpuIv2VU1::Step()
{
    VU1.VI[REG_TPC].UL &= VU1_PROGMASK;
    if (iv2::vu_ops::UpperEnabled())
        iv2_vu1Exec_entry(&VU1);
    else
        vu1Exec(&VU1);
}

// ─── [bench 2026-05-30 Codex recipe] 決定論 microbench ──────────────────────────────
//   diagnostic-only / env-gated / 完全 removable (CLAUDE.md rule 3 準拠)。
//   固定 VU1 snapshot の clone に対し scalar / NEON / NEON+exec-plan を同一プロセスで
//   交互実行し ns/op を直接比較する。同一 launch・同一 snapshot のため boot 非決定性・
//   scene ノイズ・thermal drift を排除 (= ±13% 測定壁の解決)。
//   発火: env iPSX2_VU_IV2_BENCH=1 + Execute 呼出 iPSX2_VU_IV2_BENCH_AT 回目 (既定 400000)。
bool g_iv2_bench_active = false;     // _vuXGKICKTransfer(VUops.cpp) が bench 中 GS 転送を skip
static bool s_iv2_bench_done = false;

namespace {

struct BenchResult { u64 ticks; u64 insts; };

// snapshot から E-bit まで step() で 1 program 実行。実行 step 数を返す (時間計測は呼出側)。
u64 iv2_bench_run_one(void (*step)(VURegs*))
{
    u64 insts = 0;
    VU1.VI[REG_TPC].UL <<= 3;
    const u32 sc = VU1.cycle;
    while ((VU1.cycle - sc) < 200000)
    {
        if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
            break;
        VU1.VI[REG_TPC].UL &= VU1_PROGMASK;
        step(&VU1);
        ++insts;
    }
    VU1.VI[REG_TPC].UL >>= 3;
    return insts;
}

void iv2_run_microbench()
{
    static VURegs snap;                  // ~2KB (VF/VI/ACC/q/p/flags/pipe/xgkick…、Mem は ptr のみ)
    static u8 snap_mem[VU1_MEMSIZE];     // 16KB: Mem 実体は SQ で書かれるため別途 snapshot/restore 必須
    std::memcpy(&snap, &VU1, sizeof(VURegs));
    std::memcpy(snap_mem, VU1.Mem, VU1_MEMSIZE);
    const u32 snap_vpustat = VU0.VI[REG_VPU_STAT].UL;

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double ns_per_tick = (double)tb.numer / (double)tb.denom;

    auto restore = [&]() {
        std::memcpy(&VU1, &snap, sizeof(VURegs));
        std::memcpy(VU1.Mem, snap_mem, VU1_MEMSIZE);
        VU0.VI[REG_VPU_STAT].UL = snap_vpustat;
    };

    const int M = 400;
    auto bench = [&](void (*step)(VURegs*)) -> BenchResult {
        restore(); (void)iv2_bench_run_one(step);   // warmup (decode cache 充填、計測外)
        u64 ticks = 0, insts = 0;
        for (int m = 0; m < M; ++m) {
            restore();
            const uint64_t t0 = mach_absolute_time();
            const u64 i = iv2_bench_run_one(step);
            const uint64_t t1 = mach_absolute_time();
            ticks += (t1 - t0);
            insts += i;
        }
        return BenchResult{ ticks, insts };
    };

    g_iv2_bench_active = true;   // XGKICK GS 転送 skip (全 variant で相殺)

    iv2_vu1_regs_cache_invalidate();
    const BenchResult A = bench([](VURegs* v){ vu1Exec(v); });                  // stock scalar
    g_iv2_exec_plan_enabled = true;
    const BenchResult B = bench([](VURegs* v){ iv2_vu1Exec_entry_public(v); }); // NEON, plan ON
    g_iv2_exec_plan_enabled = false;
    const BenchResult D = bench([](VURegs* v){ iv2_vu1Exec_entry_public(v); }); // NEON, plan OFF

    g_iv2_bench_active = false;
    g_iv2_exec_plan_enabled = true;                                             // ship 既定 (plan on)
    restore();                                                                   // 本番 VU1 state 復帰

    auto nsop = [&](const BenchResult& r){ return r.insts ? (double)r.ticks * ns_per_tick / (double)r.insts : 0.0; };
    const double a = nsop(A), b = nsop(B), d = nsop(D);
    Console.WriteLn("@@VU_IV2_BENCH@@ insts/run=%llu M=%d (deterministic same-launch A/B, XGKICK skipped)",
                    (unsigned long long)(A.insts / (u64)M), M);
    Console.WriteLn("@@VU_IV2_BENCH@@ A scalar         = %8.2f ns/op", a);
    Console.WriteLn("@@VU_IV2_BENCH@@ D NEON noplan    = %8.2f ns/op  NEON/scalar=%.3f (%+.1f%%)",
                    d, a > 0 ? d / a : 0.0, a > 0 ? (a - d) / a * 100.0 : 0.0);
    Console.WriteLn("@@VU_IV2_BENCH@@ B NEON +plan     = %8.2f ns/op  plan/noplan=%.3f (%+.1f%%)  NEON+plan/scalar=%.3f (%+.1f%%)",
                    b, d > 0 ? b / d : 0.0, d > 0 ? (d - b) / d * 100.0 : 0.0,
                    a > 0 ? b / a : 0.0, a > 0 ? (a - b) / a * 100.0 : 0.0);
}

} // anonymous namespace

void CpuIv2VU1::Execute(u32 cycles)
{
    // [P101-4] Async VU1 dispatch.
    //   mode 2 (full sync / Stage B): enqueue + block (bit-exact).
    //   mode 1 (true async / Stage C): the kick itself goes through
    //     vu1ExecMicro→iv2AsyncVu1_Kick; reaching Execute here means a non-kick
    //     "catch up VU1" call (SPR/DMA/vu1Finish) → just drain the worker.
    if (__builtin_expect(g_iv2_async_vu1 && iv2AsyncVu1_IsActive(), 0))
    {
        if (g_iv2_async_vu1_mode == 2)
            iv2AsyncVu1_RunCycles(cycles);
        // mode 1 (true async): this is a non-kick "catch up VU1" call
        // (ExecuteBlock from DMA/SPR/R5900). It is a NO-OP — the worker advances
        // VU1 independently and the EE only actually needs a coherent VU1 at a real
        // read (Memory.cpp vuMicro/Data + COP2 + vu1Finish for CTC2), which drain
        // there. Draining here too would collapse the feed-ahead pipeline.
        return;
    }
    ExecuteSync(cycles);
}

void CpuIv2VU1::ExecuteSync(u32 cycles)
{
    static u8 s_cap_entry_mem[0x4000];
    bool do_cap = false;
    g_iv2_suppress_interp_xgk = false;   // [P101-9 feasibility test] reset per program (set only during a fallthru kick)

    // [P101-9 coverage survey] time this program (all return paths) under iPSX2_VU1_SURVEY.
    static const bool s_survey_on = (std::getenv("iPSX2_VU1_SURVEY") != nullptr);
    struct SurveyTimer { u64 h; u32 pc; Iv2ClosureInfo ci; u64 t0; bool on;
        ~SurveyTimer(){ if (on) iv2_vu1_survey_add2(h, pc, orch_delta_ns(t0, orch_now()),
                                                    ci.key, ci.n, ci.lo, ci.hi, ci.indirect); } }
        _svt{0,0,{0,0,0,0,0},0,false};
    if (__builtin_expect(s_survey_on, 0)) {
        // 閉包の計算は t0 より前に済ませる (memo 化済みなので通常は table 参照 1 回)。
        const u64 imgh = CliffDiag::vu1CurProgHash(VU1.Micro);
        const u32 epc  = VU1.VI[REG_TPC].UL;
        Iv2ClosureInfo ci{};
        iv2_vu1_closure_cached(VU1.Micro, imgh, epc, &ci);
        iv2_vu1_imgdump(VU1.Micro, imgh);
        _svt = SurveyTimer{ imgh, epc, ci, orch_now(), true };
    }

    // [HLE_KERNEL Stage2-b] self-check / native-kernel entry hook.
    u8* const  s_sc_entry_mem = g_hle_snap_mem;   // [ST RE] file-scope snapshot (trace compares it)
    static u32 s_sc_entry_vi[16];
    static u32 s_sc_entry_top = 0;
    bool do_selfcheck = false;
    static const IV2_HleKernel* s_sc_kernel = nullptr;   // matched kernel for the self-check call
    {
        const int kmode = iv2_hle_kernel_mode();
        const u64 cap_hash = iv2_hle_cap_hash();
        const IV2_HleKernel* ke = nullptr;
        u64 cur_hash = 0;
        if (__builtin_expect(kmode >= 1 || cap_hash != 0, 0)) {
            cur_hash = CliffDiag::vu1CurProgHash(VU1.Micro);
            if (kmode >= 1) ke = iv2_hle_lookup(cur_hash, VU1.VI[REG_TPC].UL);
        }
        // [P101-9 capture] capture an unregistered program by hash (no kernel; I/O+op-trace only).
        const bool cap_only = (!ke && cap_hash != 0 && cur_hash == cap_hash);
        if (ke || cap_only)
        {
            // [diag] iPSX2_JPX_DECLINE=<csv entryPCs>: force-decline (interp runs) the listed programs
            // in mode-2, to isolate which kernel's HLE breaks a downstream (interp-run) program such as
            // the scene-2 background. Read-only/safe (interp fallback). Default off.
            auto jpx_declined = [](u32 pc)->bool {
                static u64 mask = []{
                    const char* e=std::getenv("iPSX2_JPX_DECLINE"); u64 m=0;
                    if(e) for(const char*s=e;*s;){ int v=atoi(s); if(v>=0&&v<64)m|=(1ull<<v);
                        while(*s&&*s!=',')++s; if(*s==',')++s; }
                    return m; }();
                return pc<64 && ((mask>>pc) & 1);
            };
            if (ke && kmode == 2 && (ke->validated || (int)ke->entryPC == iv2_hle_eval_pc()) && !jpx_declined(ke->entryPC)) {
                // [NATIVE-ONLY] Replace the interp entirely: run the matched native kernel on the
                // live VU mem, transfer its GIF packet to GS, set the program-complete VU state,
                // and return. Measures the realized speedup; produces the actual native-HLE output.
                // Only `validated` kernels drive GS here; non-validated ones stay on the interp so
                // they cannot regress the validated set's visuals.
                static u32 vi_snap[16];
                for (int i = 0; i < 16; ++i) vi_snap[i] = VU1.VI[i].UL;
                // GTA matrices live in VF — publish the live VF for the GTA kernel (FF10 ignore it).
                for (int i = 0; i < 32; ++i) { g_hle_snap_vf[i*4+0]=VU1.VF[i].UL[0]; g_hle_snap_vf[i*4+1]=VU1.VF[i].UL[1];
                                               g_hle_snap_vf[i*4+2]=VU1.VF[i].UL[2]; g_hle_snap_vf[i*4+3]=VU1.VF[i].UL[3]; }
                g_hle_snap_itop = VU1.GetVifRegs().itop;   // [P101-9] vertex count for static T&L (MUST set in mode-2 too)
                g_hle_native_sink.reset();
                g_iv2_hle_liveout.clear();   // [scene-2 live-out] kernel may republish VU1 exit regs
                g_iv2_hle_active_hash = ke->hash;   // [scene-2] pc18 branches on hash (per-channel color + ADC marker)
                const FPControlRegisterBackup fpcr_k(EmuConfig.Cpu.VU1FPCR);
                const int nb = ke->fn(VU1.Mem, vi_snap, vif1Regs.top,
                                      nullptr, g_hle_native_sink.buf, PktSink::CAP);
                if (nb > 0) {
                    // [J-Phoenix scene-2 GIF-lifecycle test] env iPSX2_JPX_XGK=1: for pc18, route the
                    // kernel packet through the proper interp XGKICK path (_vuXGKICKTransfer = GIF PATH1
                    // open/transfer/close + PATH3 arbitration) instead of the direct gifUnit transfer.
                    // If the bg-black is GIF PATH arbitration from the kernel's shortcut, this restores it
                    // while keeping the fast C++ compute. Reads the packet from VU mem (in_top+121).
                    static const bool s_xgkroute = (std::getenv("iPSX2_JPX_XGK") != nullptr);
                    bool routed = false;
                    if (s_xgkroute && ke->entryPC == 18) {
                        const u32 dst = ((vif1Regs.top + 121) & 0x3ff) * 16;
                        if (dst + (u32)nb <= 0x4000) {
                            std::memcpy(VU1.Mem + dst, g_hle_native_sink.buf, (u32)nb);
                            VU1.xgkickaddr = dst;
                            VU1.xgkicksizeremaining = (u32)nb;
                            VU1.xgkickdiff = 0x4000 - dst;
                            VU1.xgkickenable = 1;
                            if (!g_iv2_bench_active) _vuXGKICKTransfer(0, true);
                            routed = true;
                        }
                    }
                    // [scene-2 DIAGNOSTIC] iPSX2_JPX_NOEMIT=1: skip the kernel's GS emit for hash 129acad2.
                    // pc18's packet is all-ADC (0 prims), so visually it draws nothing; if NOT emitting it
                    // restores the rocky cave bg, the kernel's GIF PATH1 transfer was disrupting the EE's
                    // PATH3 cave geometry. Decisive A/B on the GS-arbitration hypothesis.
                    static const bool s_noemit = []{ const char* e=std::getenv("iPSX2_JPX_NOEMIT"); return e && e[0]=='1'; }();
                    // [scene-2 DIAGNOSTIC, Codex 2026-06-30] iPSX2_JPX_DEFERXGK=1: instead of immediately
                    // transferring pc18's packet (the kernel completes instantly), ARM the XGKICK in-flight
                    // exactly like the real _vuXGKICK (xgkick* + VPU_STAT bit12) WITHOUT flushing, and return.
                    // The real interp arms at PC90 then runs PC91-100 with PATH1 pending; the kernel skips that
                    // window. If leaving PATH1 deferred/in-flight restores the cave, the missing factor was
                    // persistent PATH1 timing state (not data). Skips the immediate emit (like NOEMIT) + arms below.
                    static const bool s_deferxgk = []{ const char* e=std::getenv("iPSX2_JPX_DEFERXGK"); return e && e[0]=='1'; }();
                    const bool is129 = (ke->hash == 0x129acad21b364844ULL);
                    const bool skip_emit = ((s_noemit || s_deferxgk) && is129);
                    if (!routed && !g_iv2_bench_active && !skip_emit)
                        gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, g_hle_native_sink.buf, (u32)nb, true);
                    // [P101-9 §6.1 side-effect 2026-06-27] J-Phoenix pc19 replicate VU-mem write.
                    // In mode-2 the interp is skipped, so pc19's SQ stores never happen. RE (write-set
                    // probe iPSX2_JPX_WRITESET) proved pc19 writes ONLY its GIF packet, contiguously, at
                    // vi02 base = (in_top+121) qw (= io-capture XGKICK OUT 0x2910 = (0x218+121)*16), and
                    // NO scratch elsewhere. The kernel output is the same contiguous [pktA|pktB] stream,
                    // so memcpy'ing it there exactly reproduces pc19's footprint -> dependent later
                    // programs (pc18/pc5/pc63/pc19-reentry) see the packet -> "parts disappear" fixed.
                    // kill-switch iPSX2_JPX_SIDEFX=0 for A/B. Gated to the J-Phoenix kernels by entryPC (5/18/
                    // 19/63 are J-Phoenix-only; FF10=84/162/42, GTA=1454/1435/1365 differ). All J-Phoenix T&L
                    // programs write their GIF packet contiguously at vi02 base (in_top+121) — pc19 family 2
                    // packets, pc18/pc5/pc63 1 packet — and the kernel output buffer IS that VU-mem layout, so
                    // the single memcpy of nb bytes reproduces the skipped interp's SQ stores for all of them.
                    if (ke->entryPC == 18 || ke->entryPC == 19 || ke->entryPC == 5 || ke->entryPC == 63 || ke->entryPC == 59 || ke->entryPC == 87) {
                        static const bool s_sidefx_off = []{ const char* e=std::getenv("iPSX2_JPX_SIDEFX"); return e && e[0]=='0'; }();
                        if (!s_sidefx_off) {
                            const u32 dst = ((vif1Regs.top + 121) & 0x3ff) * 16;
                            if (dst + (u32)nb <= 0x4000) std::memcpy(VU1.Mem + dst, g_hle_native_sink.buf, (u32)nb);
                        }
                    }
                    // [scene-2 DIAGNOSTIC, Codex 2026-06-30] DEFERXGK arm: leave PATH1 in-flight (don't flush).
                    // Mirrors _vuXGKICK (VUops.cpp:2018-2030): arm xgkick* pointing at the VU-mem packet the
                    // SIDEFX memcpy just wrote, set VPU_STAT bit12. We do NOT call _vuXGKICKTransfer; the pending
                    // transfer is flushed later by the normal XGKICK lifecycle (next program E-bit / GIF poll),
                    // reproducing the deferred PATH1 state the real interp leaves between PC90 and program end.
                    if (s_deferxgk && is129) {
                        const u32 dst = ((vif1Regs.top + 121) & 0x3ff) * 16;
                        VU1.xgkickenable = true;
                        VU1.xgkickaddr = dst;
                        VU1.xgkickdiff = 0x4000 - dst;
                        VU1.xgkicksizeremaining = 0;
                        VU1.xgkickendpacket = false;
                        VU1.xgkicklastcycle = VU1.cycle;
                        VU1.xgkickcyclecount = 1;
                        VU0.VI[REG_VPU_STAT].UL |= (1 << 12);
                    }
                    // [J-Phoenix scene-2 live-out write-back] env iPSX2_JPX_LIVEOUT=1: republish pc18's
                    // reconstructable VU1 EXIT registers (VF/VI/flags) the kernel computed, so the
                    // downstream bg consumer (later VU OR EE memory-mapped VU1 reg read) sees the same
                    // state the real microcode would have left -> fixes the black scene-2 background.
                    // Gated to hash 129acad2; default-off (A/B). Codex-designed (mode-2 dispatch).
                    {
                        static const bool s_liveout = (std::getenv("iPSX2_JPX_LIVEOUT") != nullptr);
                        if (__builtin_expect(s_liveout, 0) && g_iv2_hle_liveout.valid &&
                            g_iv2_hle_liveout.hash == ke->hash) {
                            for (u32 i = 0; i < 16; ++i)
                                if (g_iv2_hle_liveout.vi_mask & (1u << i)) VU1.VI[i].UL = g_iv2_hle_liveout.vi[i];
                            for (u32 i = 0; i < 32; ++i)
                                if (g_iv2_hle_liveout.vf_mask & (1u << i)) {
                                    VU1.VF[i].UL[0] = g_iv2_hle_liveout.vf[i][0]; VU1.VF[i].UL[1] = g_iv2_hle_liveout.vf[i][1];
                                    VU1.VF[i].UL[2] = g_iv2_hle_liveout.vf[i][2]; VU1.VF[i].UL[3] = g_iv2_hle_liveout.vf[i][3];
                                }
                            if (g_iv2_hle_liveout.flags_valid) {
                                VU1.macflag = g_iv2_hle_liveout.macflag; VU1.statusflag = g_iv2_hle_liveout.statusflag;
                                VU1.clipflag = g_iv2_hle_liveout.clipflag;
                                VU1.VI[REG_MAC_FLAG].UL = g_iv2_hle_liveout.macflag;
                                VU1.VI[REG_CLIP_FLAG].UL = g_iv2_hle_liveout.clipflag;
                                VU1.VI[REG_STATUS_FLAG].UL = (VU1.VI[REG_STATUS_FLAG].UL & 0xFC0) |
                                    (VU1.statusflag & 0xF) | ((VU1.statusflag & 0xF) << 6);
                            }
                        }
                    }
                    // [scene-2 DIAGNOSTIC] iPSX2_JPX_REGGARB=1: after the kernel (hash 129acad2 only),
                    // overwrite EVERY VU1 VF[1..31]/VI[1..15] + flags with garbage. If the bg screenshot
                    // CHANGES, a downstream/EE consumer reads pc18's exit register file (write-back is
                    // effective, our live-out values are just wrong). If UNCHANGED, the consumer does NOT
                    // read the register file in mode-2 (the bg state lives elsewhere). Decisive A/B.
                    {
                        static const bool s_reggarb = []{ const char* e=std::getenv("iPSX2_JPX_REGGARB"); return e && e[0]=='1'; }();
                        if (__builtin_expect(s_reggarb, 0) && ke->hash == 0x129acad21b364844ULL) {
                            for (u32 i = 1; i < 32; ++i) { VU1.VF[i].UL[0]=VU1.VF[i].UL[1]=VU1.VF[i].UL[2]=VU1.VF[i].UL[3]=0x7f7f7f7fu; }
                            for (u32 i = 1; i < 16; ++i) VU1.VI[i].UL = 0x5a5a;
                            VU1.macflag = 0; VU1.clipflag = 0;
                        }
                    }
                    // [P101-9 feasibility test] iPSX2_FALLTHRU_HASH=<hex>: XGKICK the kernel packet but DO NOT
                    // set VU complete / return -- fall through so the interp ALSO runs (applies the program's
                    // side-effects + re-XGKICKs). If the scene is then structurally OK, the collapse was the
                    // missing side-effects (kernel packet is GS-valid); if still collapsed, the packet corrupts GS.
                    static const u64 s_fallthru = []{ const char* e=std::getenv("iPSX2_FALLTHRU_HASH"); return e?strtoull(e,nullptr,16):0ULL; }();
                    if (!(s_fallthru && ke->hash == s_fallthru)) {
                        VU0.VI[REG_VPU_STAT].UL &= ~0x100;   // VU1 program complete (E-bit equiv)
                        vif1Regs.stat.VEW = false;
                        if (!(s_deferxgk && is129)) VU1.xgkickenable = 0;   // DEFERXGK: keep PATH1 armed/in-flight
                        // [J-Phoenix scene-2 timing test] env iPSX2_JPX_CYC=<n>: instead of instant
                        // completion (nextBlockCycles=1), advance VU1.cycle by ~the program's real cycle
                        // count so the EE/VU/GIF timeline matches interp. If pc18's instant completion is
                        // what blacks out the bg (GS/PATH arbitration), restoring its cycle cost recovers it
                        // while keeping the fast C++ compute. n scaled per drawn-vert count z.
                        static const u32 s_cyc = []{ const char* e=std::getenv("iPSX2_JPX_CYC"); return e?(u32)atoi(e):0u; }();
                        if (s_cyc && ke->entryPC == 18) {
                            const u32 zc = *(const u32*)(VU1.Mem + ((vif1Regs.top & 0x3ff)*16) + 8);  // z=vert count
                            const u32 add = s_cyc + (zc <= 256 ? zc * 4u : 0u);
                            VU1.cycle += add;
                            VU1.nextBlockCycles = (VU1.cycle - cpuRegs.cycle) + 1;
                        } else {
                            VU1.nextBlockCycles = 1;
                        }
                        return;
                    }
                    // fall through to interp for SIDE-EFFECTS ONLY (suppress its re-XGKICK so f8ea is drawn
                    // exactly once, by the kernel packet -> clean test of "is the kernel packet GS-valid")
                    g_iv2_suppress_interp_xgk = true;
                }
                // nb<0: kernel declined -> fall through to the interpreter (safe fallback).
            }
            std::memcpy(s_sc_entry_mem, VU1.Mem, 0x4000);
            for (int i = 0; i < 16; ++i) s_sc_entry_vi[i] = VU1.VI[i].UL;
            s_sc_entry_top = vif1Regs.top;   // XTOP source (VIF1 double-buffer TOP, in qw)
            for (int i = 0; i < 16; ++i) g_hle_snap_vi[i] = VU1.VI[i].UL;
            g_hle_snap_top = vif1Regs.top;
            g_hle_snap_itop = VU1.GetVifRegs().itop;   // [P101-9] XITOP source = vertex count for static T&L
            g_hle_snap_itops = vif1Regs.itops; g_hle_snap_tops = vif1Regs.tops;  // [P101-9 deep-RE] VIF shadow
            for (int i = 0; i < 32; ++i) { g_hle_snap_vf[i*4+0]=VU1.VF[i].UL[0]; g_hle_snap_vf[i*4+1]=VU1.VF[i].UL[1];
                                           g_hle_snap_vf[i*4+2]=VU1.VF[i].UL[2]; g_hle_snap_vf[i*4+3]=VU1.VF[i].UL[3]; }
            const int cappc = iv2_hle_cap_pc();
            if (cap_only || (kmode == 1 && (cappc < 0 || (int)ke->entryPC == cappc))) {
                do_selfcheck = true;
                s_sc_kernel = ke;              // matched kernel (null for cap_only -> nbytes=-1, I/O still captured)
                g_sc_cur_pc = ke ? (ke->entryPC ? ke->entryPC :
                    (ke->hash==0xf8ea3ba5a625c699ULL ? 9991u : ke->hash==0x603adeb2bbda23b6ULL ? 9992u : 3u))
                    : (VU1.VI[REG_TPC].UL & 0xffff);  // [P101-9] breakdown / capture pc (static kernels: hash markers)
                g_hle_interp_sink.reset();
                g_iv2_hle_sink_active = true;   // capture interp packets to g_hle_interp_sink
                g_hle_hotkick = true;          // [ST RE] enable runtime uv trace this kick
                g_dbg_done = false;            // [ST RE] reset same-kick first-vert capture
                if (__builtin_expect(g_rwset_on, 0)) g_rwset_seq.store(0);  // [P101-9 §5.7] read-set = this program only
            }
        }
    }

    // [HLE_KERNEL PoC] one-shot disasm of hot programs (env iPSX2_HLE_DIS=1).
    {
        static const bool s_hle_dis = (std::getenv("iPSX2_HLE_DIS") != nullptr);
        if (__builtin_expect(s_hle_dis, 0) && CliffDiag::vu1IsHotProgram(VU1.Micro))
            iv2_hle_disasm_once(VU1.Micro, VU1.VI[REG_TPC].UL);
    }

    // [HLE_KERNEL PoC] ceiling probe (env iPSX2_HLE_SKIP=1): if the current
    // microprogram is one of the measured hot T&L programs (GTA VC / FF10), mark VU1
    // complete WITHOUT executing it. This measures the maximum recoverable VU1
    // wall-time = the upper bound on any native HLE kernel's gain. Visual output is
    // intentionally broken for this measurement. Default off = zero ship impact.
    {
        static const bool s_hle_skip = (std::getenv("iPSX2_HLE_SKIP") != nullptr);
        if (__builtin_expect(s_hle_skip, 0) && CliffDiag::vu1IsHotProgram(VU1.Micro))
        {
            VU0.VI[REG_VPU_STAT].UL &= ~0x100;   // clear busy = "program finished"
            VU1.nextBlockCycles = 1;
            return;
        }
    }
    // [P101-9 perf] TARGETED skip of f8ea+603a only (env iPSX2_HLE_SKIP2=1): measures the
    // GS-confounded VPS ceiling those two infeasible programs represent = "how much would HLE'ing
    // them gain, optimistically". Visual broken; default-off; measurement only.
    {
        static const bool s_skip2 = (std::getenv("iPSX2_HLE_SKIP2") != nullptr);
        if (__builtin_expect(s_skip2, 0)) {
            const u64 h = CliffDiag::vu1CurProgHash(VU1.Micro);
            if (h == 0xf8ea3ba5a625c699ULL || h == 0x603adeb2bbda23b6ULL) {
                VU0.VI[REG_VPU_STAT].UL &= ~0x100;
                VU1.nextBlockCycles = 1;
                return;
            }
        }
    }
    // [P101-9 perf decomposition] iPSX2_HLE_NOGS2=1: f8ea+603a RUN normally (full VU1 compute) but
    // their GIF output is suppressed (not sent to GS). Isolates GS-render cost: baseline=VU1+GS,
    // skip2=neither, nogs2=VU1-only. Realistic native-kernel gain ~= baseline -> (VU1 removed, GS kept).
    {
        static const bool s_nogs2 = (std::getenv("iPSX2_HLE_NOGS2") != nullptr);
        if (__builtin_expect(s_nogs2, 0)) {
            const u64 h = CliffDiag::vu1CurProgHash(VU1.Micro);
            if (h == 0xf8ea3ba5a625c699ULL || h == 0x603adeb2bbda23b6ULL)
                g_iv2_suppress_interp_xgk = true;   // VU1 runs; its XGKICK is dropped (reset next program)
        }
    }

    // [HLE_KERNEL Stage2-b] I/O capture (env iPSX2_HLE_CAP=1): for the first few
    // invocations of EACH distinct hot T&L program (GTA VC + FF10), snapshot entry VU
    // mem + all VI regs, then enable XGKICK-time output-packet capture for that run.
    // Builds the ground-truth I/O test vectors a native kernel must reproduce.
    // iPSX2_HLE_CAP_N overrides the per-program invocation count (default 4).
    {
        static const bool s_hle_cap = (std::getenv("iPSX2_HLE_CAP") != nullptr);
        static constexpr int kCapKeys = 48;   // [J-Phoenix 2026-06-27] raised 16->48: 8 hot programs x ~4-6 entry-PCs
                                              // exceeded 16 keys, dropping the dominant program's I/O. env-gated, ship-neutral.
        static u64 cap_keys[kCapKeys] = {0};
        static int cap_cnt[kCapKeys] = {0};
        static int cap_nkeys = 0;
        static const int kCapPer = []{ const char* e = std::getenv("iPSX2_HLE_CAP_N");
                                       return e ? std::max(1, atoi(e)) : 4; }();
        if (__builtin_expect(s_hle_cap, 0) && CliffDiag::vu1IsHotProgram(VU1.Micro))
        {
            const u64 h  = CliffDiag::vu1CurProgHash(VU1.Micro);
            const u32 pc = VU1.VI[REG_TPC].UL;
            const u64 key = h ^ ((u64)pc << 48);
            int slot = -1;
            for (int i = 0; i < cap_nkeys; ++i) if (cap_keys[i] == key) { slot = i; break; }
            if (slot < 0 && cap_nkeys < kCapKeys) { slot = cap_nkeys++; cap_keys[slot] = key; }
            if (slot >= 0 && cap_cnt[slot] < kCapPer)
            {
                ++cap_cnt[slot];
                do_cap = true;
                std::memcpy(s_cap_entry_mem, VU1.Mem, 0x4000);
                u32 vi[16];
                for (int i = 0; i < 16; ++i) vi[i] = VU1.VI[i].UL;
                iv2_hle_cap_entry(s_cap_entry_mem, vi, h, pc, VU1.xgkickaddr);
                g_iv2_hle_cap_xgk_active = true;   // capture output packets during this run
            }
        }
    }

    const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU1FPCR);

    if (__builtin_expect(!s_iv2_bench_done, 0))   // [bench 2026-05-30] env-gated, removable
    {
        static const bool bench_on = (std::getenv("iPSX2_VU_IV2_BENCH") != nullptr);
        if (!bench_on) { s_iv2_bench_done = true; }   // BENCH 未設定: 初回で永久 disable = 以降ゼロ overhead
        else
        {
            static const u64 at = []{ const char* a = std::getenv("iPSX2_VU_IV2_BENCH_AT");
                                      return a ? (u64)strtoull(a, nullptr, 10) : (u64)400000; }();
            static u64 n = 0;
            if (++n >= at) { iv2_run_microbench(); s_iv2_bench_done = true; }
        }
    }

    VU1.VI[REG_TPC].UL <<= 3;
    const u32 startcycles = VU1.cycle;

    const u64 sc_loop_t0 = __builtin_expect(do_selfcheck, 0) ? orch_now() : 0;

    // [P101-15 T1 PoC probe] per-PC ヒストグラム専用ループ (env iPSX2_VU1_PCHIST=<hash hex>)。
    // ship ループとは別実体なので、既定 (env 無し) では機械語ごと通らない = 摂動ゼロ。
    // [P101-18] `iPSX2_VU1_PCHIST=all` で **program を問わず全 kick** を集計する (= 16KB image が
    // 差し替わっても実行 PC 集合を横断で見る)。GoW のように 1 本の image を entry stub + 間接分岐で
    // 使い回すタイトルでは、hash 単位の pchist では実行範囲が分からないため。診断専用。
    static const bool s_pchist_all = []{ const char* e = std::getenv("iPSX2_VU1_PCHIST");
                                         return e && std::strcmp(e, "all") == 0; }();
    static const u64 s_pchist_env = []{ const char* e = std::getenv("iPSX2_VU1_PCHIST");
                                        return (e && std::strcmp(e, "all") != 0) ? (u64)strtoull(e, nullptr, 16) : 0ULL; }();
    const bool pchist_on = __builtin_expect(s_pchist_env != 0 || s_pchist_all, 0) && !THREAD_VU1 &&
                           g_iv2_vu1_flatten && iv2::vu_ops::UpperEnabled() &&
                           (s_pchist_all || CliffDiag::vu1CurProgHash(VU1.Micro) == s_pchist_env);

    // [P101-2 MTVU 撤去後] iv2 では MTVU 常時 off。
    // VPU_STAT VBS1 を正しく立てて dispatch する main thread 同期実行経路のみを想定。
    // THREAD_VU1 が稀に true になるケース (microVU 併用等) のため safeguard として
    // E-bit ベースループも併存させる。
    if (__builtin_expect(pchist_on, 0))
    {
        g_pchist_hash = s_pchist_env;
        ++g_pchist_kicks;
        // [P101-18] all モードでは image ごとの表も同時に埋める。
        u32* const imgtab = s_pchist_all ? iv2_pchist_table_for(CliffDiag::vu1CurProgHash(VU1.Micro)) : nullptr;
        while ((VU1.cycle - startcycles) < cycles)
        {
            if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
            {
                if (VU1.branch == 1) { VU1.VI[REG_TPC].UL = VU1.branchpc; VU1.branch = 0; }
                break;
            }
            VU1.VI[REG_TPC].UL &= VU1_PROGMASK;
            const u32 pcw = (VU1.VI[REG_TPC].UL >> 3) & 0x7ff;
            ++g_pchist[pcw];
            if (imgtab) ++imgtab[pcw];
            ++g_pchist_ops;
            VU1.cycle++;
            iv2_vu1Exec(&VU1);
        }
        // [2026-08-07] 16384 kick ごと -> 初回 + 1024 kick ごと。GoW の実 gameplay は
        // program 1 本あたりの kick が数千しかなく、16384 に届かない program は
        // 「そのシーンで動いていない」と誤判定されて採取できなかった。
        if (g_pchist_kicks == 1 || (g_pchist_kicks & 0x3ff) == 0) { iv2_pchist_dump(); iv2_t1_dump_inputs(); }
    }
    // [P101-15] オフライン変換 (T1) ループ。env `iPSX2_VU1_T1=1` のときだけ通る。
    // 変換済み PC に来たら特殊化本体へ、それ以外は従来どおり 1 命令ずつ interp。
    // 変換本体の中身はインタプリタと同一 helper・同一順序 = bit-exact。
    else if (__builtin_expect(iv2::vu_ops::g_iv2_t1_enabled, 0) && !THREAD_VU1 &&
             g_iv2_vu1_flatten && iv2::vu_ops::UpperEnabled() &&
             iv2::vu_ops::T1_ProgramReady(CliffDiag::vu1CurProgHash(VU1.Micro)))
    {
        while ((VU1.cycle - startcycles) < cycles)
        {
            if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
            {
                if (VU1.branch == 1) { VU1.VI[REG_TPC].UL = VU1.branchpc; VU1.branch = 0; }
                break;
            }
            VU1.VI[REG_TPC].UL &= VU1_PROGMASK;
            if (iv2::vu_ops::T1_TryRun(&VU1, VU1.VI[REG_TPC].UL, startcycles, cycles))
                continue;
            // [P101-18 A] 変換していない PC = インタプリタ 1 命令。償却の分母になる。
            if (__builtin_expect(iv2::vu_ops::g_t1_stats_on, 0)) ++iv2::vu_ops::g_t1_bail_insns;
            VU1.cycle++;
            iv2_vu1Exec(&VU1);
        }
    }
    else if (THREAD_VU1)
    {
        bool ebit_was_set = false;
        while ((VU1.cycle - startcycles) < cycles)
        {
            Step();
            if (VU1.ebit != 0)
                ebit_was_set = true;
            else if (ebit_was_set)
            {
                if (VU1.branch == 1)
                {
                    VU1.VI[REG_TPC].UL = VU1.branchpc;
                    VU1.branch = 0;
                }
                break;
            }
        }
    }
    else if (g_iv2_vu1_flatten && iv2::vu_ops::UpperEnabled())
    {
        static bool s_flat_logged = false;
        if (__builtin_expect(!s_flat_logged, 0)) { s_flat_logged = true;
            Console.WriteLn("@@VU_IV2_FLATTEN@@ tight-loop ACTIVE (iPSX2_VU1_FLATTEN)"); }
        // [A: VU1 block-flatten step 1] tight loop = Step() の else 分岐 (= 既定の
        // upper-enabled 経路) を inline 展開し、per-call の Step()/iv2_vu1Exec_entry call
        // 境界・hoist 可能な UpperEnabled() 再判定・per-instruction VF[0]/VI[0] debug
        // assert を除去。残りは default と完全同一順序 (mask → cycle++ → iv2_vu1Exec、
        // TPC 毎反復読み直し) のため bit-exact。UpperEnabled()==false は下の default へ。
        while ((VU1.cycle - startcycles) < cycles)
        {
            if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
            {
                if (VU1.branch == 1)
                {
                    VU1.VI[REG_TPC].UL = VU1.branchpc;
                    VU1.branch = 0;
                }
                break;
            }
            VU1.VI[REG_TPC].UL &= VU1_PROGMASK;
            VU1.cycle++;
            iv2_vu1Exec(&VU1);
        }
    }
    else
    {
        while ((VU1.cycle - startcycles) < cycles)
        {
            if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
            {
                if (VU1.branch == 1)
                {
                    VU1.VI[REG_TPC].UL = VU1.branchpc;
                    VU1.branch = 0;
                }
                break;
            }
            Step();
        }
    }

    const u64 sc_loop_t1 = __builtin_expect(do_selfcheck, 0) ? orch_now() : 0;
    g_hle_hotkick = false;   // [ST RE] end runtime uv trace window

    if (__builtin_expect(do_cap, 0))
    {
        iv2_hle_cap_exit(VU1);
        g_iv2_hle_cap_xgk_active = false;
    }

    // [HLE_KERNEL Stage2-b] self-check: interp output is now in g_hle_interp_sink; run the
    // native kernel on the entry snapshot and compare. interp output already drove GS
    // (zero regression). VI[REG_TPC] is still <<3 here, but the snapshot was taken at entry.
    if (__builtin_expect(do_selfcheck, 0))
    {
        g_iv2_hle_sink_active = false;
        g_hle_native_sink.reset();
        // [P101-9 §6.1 write-set probe] env iPSX2_JPX_WRITESET=1: diff entry snapshot (pre-pc19) vs the
        // live VU1.Mem (post-pc19, interp drove) to map EXACTLY which qw pc19 writes. Confirms the
        // mode-2 side-effect memcpy target (= where the GIF packet lands, expected vi02 base = in_top+121)
        // and reveals any non-packet scratch writes later programs may read. Dumps changed-qw ranges for
        // the first kicks. Dev-only, env-gated, read-only (no state change). Removal: pin to RE doc.
        {
            static const bool s_ws = (std::getenv("iPSX2_JPX_WRITESET") != nullptr);
            if (__builtin_expect(s_ws, 0)) {
                static int wn = 0;
                if (wn < 12) { wn++;
                    const u32 itop = vif1Regs.top & 0x3ff;
                    FILE* wf = std::fopen("/tmp/ipsx2_jpx_writeset.txt", wn == 1 ? "w" : "a");
                    if (wf) {
                        std::fprintf(wf, "\n=== kick #%d in_top=0x%x (vi02=in_top+121=0x%x) ===\n",
                                     wn, itop, (itop + 121) & 0x3ff);
                        // walk all 0x400 qw; report contiguous changed runs.
                        int run0 = -1;
                        for (u32 q = 0; q <= 0x400; ++q) {
                            bool ch = false;
                            if (q < 0x400)
                                ch = std::memcmp(s_sc_entry_mem + q*16, VU1.Mem + q*16, 16) != 0;
                            if (ch && run0 < 0) run0 = (int)q;
                            else if (!ch && run0 >= 0) {
                                std::fprintf(wf, "  qw 0x%03x..0x%03x (%d qw)  [rel in_top: +%d..+%d]\n",
                                    run0, q-1, (int)q-run0, run0-(int)itop, (int)q-1-(int)itop);
                                run0 = -1;
                            }
                        }
                        std::fclose(wf);
                    }
                }
            }
        }
        // [J-Phoenix scene-2 side-effect RE] env iPSX2_JPX_VFDIFF=1 (use with CAP_PC=18): the kernel
        // never touches VU1.VF/VI, so any VF/VI a downstream program inherits from pc18's interp run is
        // lost in mode-2 (= the black background). Dump entry(pre-pc18) vs post-interp VF/VI delta for
        // the first kicks so the inherited register(s) can be pinned and reproduced cheaply in-kernel.
        {
            static const bool s_vfd = (std::getenv("iPSX2_JPX_VFDIFF") != nullptr);
            if (__builtin_expect(s_vfd, 0)) {
                static int vn = 0;
                if (vn < 8) { vn++;
                    FILE* vf = std::fopen("/tmp/ipsx2_jpx_vfdiff.txt", vn==1?"w":"a");
                    if (vf) {
                        std::fprintf(vf, "\n=== kick #%d pc=%u in_top=0x%x ===\n", vn, g_sc_cur_pc, s_sc_entry_top);
                        for (int r = 0; r < 32; ++r) {
                            const u32* e = &g_hle_snap_vf[r*4];
                            const u32* p = &VU1.VF[r].UL[0];
                            if (e[0]!=p[0]||e[1]!=p[1]||e[2]!=p[2]||e[3]!=p[3])
                                std::fprintf(vf, "  vf%02d: %08x %08x %08x %08x -> %08x %08x %08x %08x\n",
                                    r, e[0],e[1],e[2],e[3], p[0],p[1],p[2],p[3]);
                        }
                        for (int r = 0; r < 16; ++r) {
                            const u32 e = s_sc_entry_vi[r], p = VU1.VI[r].UL & 0xffff;
                            if ((e&0xffff)!=p) std::fprintf(vf, "  vi%02d: 0x%04x -> 0x%04x\n", r, e&0xffff, p);
                        }
                        std::fclose(vf);
                    }
                }
            }
        }
        // Post-run VU1.VF[5..8] hold the MVP matrix the interp actually loaded (constant
        // through the per-vertex loop) — feed it to isolate transform from matrix-load RE.
        {
            static int mlogged = 0;
            const bool nondegen = (VU1.VF[5].UL[0] | VU1.VF[5].UL[1] | VU1.VF[6].UL[0] | VU1.VF[6].UL[1]) != 0;
            if (mlogged < 8 && nondegen) { mlogged++;
                // Scan the entry snapshot for the qw offset whose 16 u32 match VF[5..8] =
                // the matrix source address (resolves the pc71/BAL matrix-load RE for mode-2).
                int found_qw = -1;
                for (u32 qw = 0; qw + 4 <= 0x400 && found_qw < 0; ++qw) {
                    const u32* m = (const u32*)(s_sc_entry_mem + qw*16);
                    bool ok = true;
                    for (int k = 0; k < 16 && ok; ++k) if (m[k] != VU1.VF[5].UL[k]) ok = false;
                    if (ok) found_qw = (int)qw;
                }
                Console.WriteLn(Color_Green, "@@HLE_MATRIX@@ top=%u found_qw=%d vf05=%08x %08x %08x %08x vf08=%08x %08x %08x %08x",
                    s_sc_entry_top, found_qw,
                    VU1.VF[5].UL[0],VU1.VF[5].UL[1],VU1.VF[5].UL[2],VU1.VF[5].UL[3],
                    VU1.VF[8].UL[0],VU1.VF[8].UL[1],VU1.VF[8].UL[2],VU1.VF[8].UL[3]);
                // [ST RE] VIF1 unpack state at entry: MaskRow/mode/mask = the suspected texcoord bias source.
                Console.WriteLn(Color_Green, "@@HLE_VIF@@ top=%u mode=%u mask=%08x MaskRow=%d %d %d %d (hex %08x %08x %08x %08x)",
                    s_sc_entry_top, vif1Regs.mode, vif1Regs.mask,
                    (int)vif1.MaskRow._u32[0],(int)vif1.MaskRow._u32[1],(int)vif1.MaskRow._u32[2],(int)vif1.MaskRow._u32[3],
                    vif1.MaskRow._u32[0],vif1.MaskRow._u32[1],vif1.MaskRow._u32[2],vif1.MaskRow._u32[3]);
            }
        }
        // Matrix source RE'd to mem[qw 52] (confirmed found_qw=52 for degenerate + on-screen,
        // both double-buffers) -> pass nullptr so the kernel reads it from the snapshot itself,
        // exactly as native-only (mode 2) would. (env iPSX2_HLE_POSTMAT=1 = use post-run VF.)
        static const bool s_postmat = (std::getenv("iPSX2_HLE_POSTMAT") != nullptr);
        const u32* matarg = s_postmat ? &VU1.VF[5].UL[0] : nullptr;  // FF10 kernels: mem path (in_matrix unused). GTA reads g_hle_snap_vf directly.
        const u64 sc_nat_t0 = orch_now();
        if (s_sc_kernel) g_iv2_hle_active_hash = s_sc_kernel->hash;   // [scene-2] pc18 hash branch
        const int nbytes = s_sc_kernel
            ? s_sc_kernel->fn(s_sc_entry_mem, s_sc_entry_vi, s_sc_entry_top,
                              matarg, g_hle_native_sink.buf, PktSink::CAP)
            : -1;
        const u64 sc_nat_t1 = orch_now();
        g_sc_interp_ns += orch_delta_ns(sc_loop_t0, sc_loop_t1);
        g_sc_native_ns += orch_delta_ns(sc_nat_t0, sc_nat_t1);
        g_sc_time_kicks++;
        // [ST debug] one-shot dump of input streams + interp/native ST for an on-screen frame.
        {
            static bool stdbg = false;
            if (!stdbg && nbytes > 3000 && nbytes == (int)g_hle_interp_sink.len) {
                // find an on-screen vert whose native ST.S differs from interp ST.S
                bool onscr = false;
                for (u32 o = 96; o+48 <= (u32)g_hle_interp_sink.len; o += 48) {
                    u32 z = *(u32*)(g_hle_interp_sink.buf+o+40);
                    if (z==0x7fffffffu || z==0x80000000u || z==0) continue;
                    if (*(u32*)(g_hle_interp_sink.buf+o) != *(u32*)(g_hle_native_sink.buf+o)) { onscr=true; break; }
                }
                if (onscr) { stdbg = true;
                    FILE* f = std::fopen("/tmp/ipsx2_hle_stdebug.txt","w");
                    if (f) {
                        const u32 TOP = s_sc_entry_top & 0x3ff;
                        const u32* hdr = (u32*)(s_sc_entry_mem + TOP*16);
                        const u32 cnt=hdr[0]&0xffff, ba=hdr[1]&0xffff, st3=3*cnt;
                        const u32 A=TOP+ba, B=A+st3, C=B+st3;
                        std::fprintf(f,"TOP=%u count=%u baseA=%u A(col)=%u B(uv)=%u C(pos)=%u\n",TOP,cnt,ba,A,B,C);
                        // [ST RE] same-kick: vf18 = ITOF12(uv0) captured at pc=132 (pre-MULq), + Q.
                        // If vf18.x == itof12(snapshot uv0.x) the ITOF12 input matches my read;
                        // compare itof12(uv0)*Q to interp ST to localize the transform.
                        { const u32* uc0=(u32*)(g_hle_snap_mem+((B)&0x3ff)*16);
                          const u32* io0=(u32*)(g_hle_interp_sink.buf+96);
                          std::fprintf(f,"  [pc132 capture] vf18(ITOF12 uv0)=%08x %08x %08x  Q=%08x | snap uv0=%08x %08x %08x | interp ST0=%08x %08x %08x\n",
                            g_dbg_vf18[0],g_dbg_vf18[1],g_dbg_vf18[2], g_dbg_q, uc0[0],uc0[1],uc0[2], io0[0],io0[1],io0[2]); }
                        int shown=0;
                        for (u32 k=0;k<cnt*3 && shown<10;k++){
                            const u32* io=(u32*)(g_hle_interp_sink.buf+96+k*48);
                            const u32* no=(u32*)(g_hle_native_sink.buf+96+k*48);
                            const bool diff = (io[0]!=no[0]||io[1]!=no[1]||io[2]!=no[2]);
                            const u32* pc=(u32*)(s_sc_entry_mem+((C+k)&0x3ff)*16);
                            const u32* uc=(u32*)(s_sc_entry_mem+((B+k)&0x3ff)*16);
                            const u32 xyzf_z = io[40/4 + 8]; // not used; keep layout
                            (void)xyzf_z;
                            if (diff || k<3) { shown++;
                            std::fprintf(f,"%sv%u pos=%08x %08x %08x %08x | uv=%08x %08x %08x | interpST=%08x %08x %08x | natST=%08x %08x %08x | iXYZ=%08x %08x %08x\n",
                                diff?"DIFF ":"     ",k,pc[0],pc[1],pc[2],pc[3], uc[0],uc[1],uc[2],
                                io[0],io[1],io[2], no[0],no[1],no[2],
                                io[8],io[9],io[10]);
                            if (diff) {
                                // back-solve the real uv source: Q = interp ST.z (uv.z=0x1000 -> 1.0 -> ST.z=Q).
                                // target itof12(uv.x) = interpS/Q -> uv.x int = that * 4096. scan input for matching qw.
                                const float Q = vufp::u2f(io[2]);
                                const int tuvx = (Q!=0.0f)? (int)(vufp::u2f(io[0])/Q*4096.0f) : 0;
                                const int tuvy = (Q!=0.0f)? (int)(vufp::u2f(io[1])/Q*4096.0f) : 0;
                                int fq=-1;
                                for (u32 q=0;q<0x400;q++){ const u32* w=(u32*)(s_sc_entry_mem+q*16);
                                    if (abs((int)w[0]-tuvx)<=1 && abs((int)w[1]-tuvy)<=1){ fq=(int)q; break; } }
                                std::fprintf(f,"   back-solve: Q=%.6g tgt_uv=(%d,%d) found_qw=%d  (my B+k=%u, pos C+k=%u)\n",
                                    Q,tuvx,tuvy,fq,(B+k)&0x3ff,(C+k)&0x3ff);
                            }
                            }
                        }
                        std::fclose(f);
                    }
                }
            }
        }
        iv2_hle_selfcheck_compare(nbytes);
    }

    VU1.VI[REG_TPC].UL >>= 3;
    VU1.nextBlockCycles = (VU1.cycle - cpuRegs.cycle) + 1;

    // [J-Phoenix scene-2 sledgehammer] env iPSX2_JPX_LOGARBAGE=1: after pc18 (hash 129acad2) runs on
    // the INTERP, ZERO its exit VF[1..31]/VI[1..10] (+flags). If the rocky-cave background then BREAKS,
    // a downstream consumer truly reads pc18's exit registers (-> live-out replay is the right fix). If
    // the bg stays correct, the registers are irrelevant and the bg depends on pc18's execution PROCESS
    // (not its register output) = kernel-HLE recovery infeasible. Decisive read-only-ish diagnostic.
    {
        static const bool s_logarb = (std::getenv("iPSX2_JPX_LOGARBAGE") != nullptr);
        if (__builtin_expect(s_logarb, 0) &&
            CliffDiag::vu1CurProgHash(VU1.Micro) == 0x129acad21b364844ULL) {
            for (int i = 1; i < 32; ++i) { VU1.VF[i].UL[0]=0; VU1.VF[i].UL[1]=0; VU1.VF[i].UL[2]=0; VU1.VF[i].UL[3]=0; }
            for (int i = 1; i <= 10; ++i) VU1.VI[i].UL = 0;
            VU1.macflag = 0; VU1.clipflag = 0;
        }
    }

    // [P101-9 B] capture this program's EXIT integer-register file into the rolling ring (census-gated).
    if (__builtin_expect(g_census_enabled(), 0)) {
        const int p = g_vu1exit_pos;
        for (int i = 0; i < 16; ++i) g_vu1exit_vi[p][i] = VU1.VI[i].UL;
        g_vu1exit_hash[p] = CliffDiag::vu1CurProgHash(VU1.Micro);
        g_vu1exit_itop[p] = VU1.GetVifRegs().itop;
        g_vu1exit_pos = (p + 1) & 7;
        if (g_vu1exit_n < 8) g_vu1exit_n++;
    }
}

void CpuIv2VU1::Clear(u32 /*addr*/, u32 /*size*/)
{
    // [flag liveness] Do NOT invalidate here: Clear fires on every microprogram upload
    // (per kick in some games), and a full invalidation defeats the per-PC decode cache
    // (every instruction becomes a miss + a fresh forward-scan = large overhead, observed
    // -10% vs no-feature). The decode cache is self-validating via per-PC code compare, so
    // when downstream code actually changes those PCs recompute their liveness on the next
    // miss. Residual risk: a PARTIAL overwrite that keeps this PC's words but changes the
    // forward window could leave stale liveness — rare, and acceptable per the
    // speed-over-minor-glitch priority. Reset() still does a full invalidation.
}

// ────────────────────────────────────────────────────────────
// VU0 (Phase V — structural wrapper のまま)
// ────────────────────────────────────────────────────────────
CpuIv2VU0 CpuIv2VU0Inst;

CpuIv2VU0::CpuIv2VU0()
{
    m_Idx = 0;
    IsInterpreter = true;
    Console.WriteLn("@@VU_IV2_INIT@@ CpuIv2VU0 constructed (Phase V structural)");
}

void CpuIv2VU0::Reset()
{
    VU0.fmacwritepos = 0;
    VU0.fmacreadpos  = 0;
    VU0.fmaccount    = 0;
    VU0.ialuwritepos = 0;
    VU0.ialureadpos  = 0;
    VU0.ialucount    = 0;
}

void CpuIv2VU0::SetStartPC(u32 startPC)
{
    VU0.start_pc = startPC;
}

void CpuIv2VU0::Step()
{
    vu0Exec(&VU0);
}

void CpuIv2VU0::Execute(u32 cycles)
{
    const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU0FPCR);

    VU0.VI[REG_TPC].UL <<= 3;
    VU0.flags &= ~VUFLAG_MFLAGSET;
    const u32 startcycles = VU0.cycle;

    while ((VU0.cycle - startcycles) < cycles)
    {
        if (!(VU0.VI[REG_VPU_STAT].UL & 0x1))
        {
            if (VU0.branch)
            {
                VU0.VI[REG_TPC].UL = VU0.branchpc;
                VU0.branch = 0;
            }
            break;
        }
        if (VU0.flags & VUFLAG_MFLAGSET)
            break;

        vu0Exec(&VU0);
    }
    VU0.VI[REG_TPC].UL >>= 3;

    if (EmuConfig.Speedhacks.EECycleRate != 0 &&
        (!EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Speedhacks.EECycleRate < 0))
    {
        u32 cycle_change = VU0.cycle - startcycles;
        VU0.cycle -= cycle_change;
        switch (std::min(static_cast<int>(EmuConfig.Speedhacks.EECycleRate),
                         static_cast<int>(cycle_change)))
        {
            case -3: cycle_change *= 2.0f; break;
            case -2: cycle_change *= 1.6666667f; break;
            case -1: cycle_change *= 1.3333333f; break;
            case 1:  cycle_change /= 1.3f; break;
            case 2:  cycle_change /= 1.8f; break;
            case 3:  cycle_change /= 3.0f; break;
            default: break;
        }
        VU0.cycle += cycle_change;
    }

    VU0.nextBlockCycles = (VU0.cycle - cpuRegs.cycle) + 1;
}

void CpuIv2VU0::Clear(u32 /*addr*/, u32 /*size*/)
{
}
