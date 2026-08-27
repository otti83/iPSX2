// SPDX-License-Identifier: GPL-3.0+
// iv2 kernel-HLE framework — public API (P101-7/8/9; extracted from iv2_vu.cpp 2026-06-27)
//
// "kernel-HLE" replaces a hot PS2 VU1 T&L microprogram with a native C++ kernel that emits the
// SAME GIF packet byte-stream, WITHOUT runtime codegen (App Store legal). The framework here is
// game-AGNOSTIC: registry + lookup + self-check harness + GIF census + capture/disasm probes.
// Per-title kernels live in iv2_hle_<game>.cpp; the shared VU float-prim lib is iv2_hle_vumath.h.
//
// Integration point: CpuIv2VU1::ExecuteSync (iv2_vu.cpp) calls the harness pre/post a VU1 program:
//   mode 0/unset : OFF (ship; zero overhead — lookup never runs, kernels dormant).
//   mode 1       : SELF-CHECK — interp drives GS; the native kernel runs on an entry snapshot and
//                  its GIF bytes are compared (zero regression; GS always sees interp output).
//   mode 2       : NATIVE-ONLY — `validated` kernels drive GS (realized speedup + actual output).
//
// ─────────────────────────────────────────────────────────────────────────────────────────────
//  HOW TO ADD A NEW TITLE (per-game extensibility; see P101-9 §4 eligibility gate)
// ─────────────────────────────────────────────────────────────────────────────────────────────
//   1. SURVEY     iPSX2_VU1_SURVEY=1   -> find hot microprograms (time%); pick the dominant T&L ones.
//   2. SCREEN     iPSX2_HLECENSUS=1    -> EXCLUDE frustum-clippers / non-terminal programs (§4):
//                   output vertex count MUST be derivable from the entry snapshot; NO CLIP/FCGET
//                   runtime-generated verts; no cross-frame stale residue. Only pure-T&L proceeds.
//   3. CAPTURE    iPSX2_CAP_HASH=<hash> + iPSX2_HLE_OPTRACE=1  -> I/O snapshot + per-op value trace.
//   4. RE         reverse-engineer in re/<game>/ ; verify byte-exact with an offline model.
//   5. KERNEL     add iv2_hle_kernel_<game>_<pc>() + a row in kIv2HleKernels_<game>[] (validated=false),
//                 in iv2_hle_<game>.cpp. New game => new file + 1 row in kIv2HleKernelTables[] (iv2_hle.cpp).
//   6. SELF-CHECK iPSX2_HLE_KERNEL=1   -> drive interp, compare native GIF bytes; iterate to ~100%
//                 byte-exact (geom/ST/header). Zero regression while iterating.
//   7. VALIDATE   flip validated=true once byte-exact + user-visual PASS; mode-2 then drives GS.
// ─────────────────────────────────────────────────────────────────────────────────────────────
#pragma once

#include "common/Pcsx2Types.h"
#include <cstring>   // PktSink::add (std::memcpy)
#include <atomic>    // probe-ring sequence counters

struct VURegs;   // (VUmicro.h) — only referenced by reference/pointer in this header

// ── Kernel registry ─────────────────────────────────────────────────────────────────────────
// A native kernel reads a 16KB VU-mem snapshot + entry VI regs + VIF1 TOP (+ optional MVP matrix)
// and writes the GIF packet byte-stream it would XGKICK into `out`. Returns bytes written, or -1
// to decline (caller counts a miss; interp output is used). Registered by (content hash, entryPC).
typedef int (*iv2_hle_kernel_fn)(const u8* in_mem, const u32* in_vi, u32 in_top,
                                 const u32* in_matrix, u8* out, u32 out_cap);
// `validated` = byte-exact (or visually-confirmed) enough to drive GS output in native-only mode-2.
// Non-validated kernels still run in mode-1 self-check (for measurement) but are SKIPPED in mode-2
// so they cannot regress the validated set's visuals.
struct IV2_HleKernel { u64 hash; u32 entryPC; iv2_hle_kernel_fn fn; bool validated; };

// [J-Phoenix scene-2 live-out] A mode-2 kernel only emits the GIF packet; it does NOT run the
// microcode, so the VU1 EXIT register state (VF/VI/flags) the real program leaves is lost. A
// downstream consumer (later VU program OR EE-side via memory-mapped VU1 regs) reads pc18's exit
// regs to render the rocky-cave BACKGROUND -> bg goes black in mode-2. This sidecar lets a kernel
// publish the O(1)-reconstructable exit registers (last processed vertex + epilogue), which the
// mode-2 dispatch writes back into VU1 after the kernel returns. Env-gated (iPSX2_JPX_LIVEOUT).
struct IV2_HleLiveOut
{
    bool valid = false;
    bool flags_valid = false;
    u64  hash = 0;
    u32  vi_mask = 0;
    u32  vf_mask = 0;
    u32  vi[16]{};
    u32  vf[32][4]{};
    u32  macflag = 0;
    u32  statusflag = 0;
    u32  clipflag = 0;
    void clear() { std::memset(this, 0, sizeof(*this)); }
};
extern IV2_HleLiveOut g_iv2_hle_liveout;

// [J-Phoenix scene-2] the hash of the kernel currently being dispatched (set by ExecuteSync right
// before ke->fn). pc18 is shared by 4 hashes (scene-1 433853b1/0a284e6a/3c7a3d82 + scene-2
// 129acad2); the scene-2 variant needs per-channel lighting + a distinct XYZF2.w3 ADC marker.
extern u64 g_iv2_hle_active_hash;

// Per-title kernel tables — DEFINED in iv2_hle_<game>.cpp, co-located with the kernels.
extern const IV2_HleKernel kIv2HleKernels_ff10[];
extern const int           kIv2HleKernels_ff10_count;
extern const IV2_HleKernel kIv2HleKernels_gtavc[];
extern const int           kIv2HleKernels_gtavc_count;
extern const IV2_HleKernel kIv2HleKernels_jphoenix[];
extern const int           kIv2HleKernels_jphoenix_count;
extern float g_jpx_clipw[256];   // [J-Phoenix ADC RE] per-vertex clip.w the kernel computed (self-check diag)
extern float g_jpx_sx[256], g_jpx_sy[256];   // [J-Phoenix cull RE] per-vertex screen sx,sy (pre-cull diag)

// (content hash, entryPC) -> kernel, scanning every per-title table. entryPC==0 = hash-only wildcard.
// iPSX2_HLE_ONLY=<entryPC> restricts to a single kernel (isolates one program's stats). nullptr = none.
const IV2_HleKernel* iv2_hle_lookup(u64 hash, u32 pc);

// ── Mode / env selection (queried by the integration point) ─────────────────────────────────
int iv2_hle_kernel_mode();   // iPSX2_HLE_KERNEL: 0/unset=off, 1=self-check, 2=native-only
int iv2_hle_eval_pc();       // iPSX2_HLE_EVAL_PC: mode-2 drive a NON-validated kernel by entryPC (visual eval). -1=off
int iv2_hle_cap_pc();        // iPSX2_HLE_CAP_PC: restrict mode-1 self-check to one entryPC. -1=no filter
u64 iv2_hle_cap_hash();      // iPSX2_CAP_HASH: capture an UNREGISTERED program by hash (I/O + op-trace). 0=off

// ── GIF packet sink (output capture; cross-TU: VUops.cpp appends fragments here) ─────────────
struct PktSink {
    static constexpr u32 CAP = 256 * 1024;
    u8  buf[CAP];
    u32 len;
    bool ovf;
    void reset() { len = 0; ovf = false; }
    void add(const u8* d, u32 bytes) {
        if (len + bytes <= CAP) { std::memcpy(buf + len, d, bytes); len += bytes; }
        else ovf = true;
    }
};
extern PktSink g_hle_interp_sink;   // interp GS-bound stream (ground truth) during self-check
extern PktSink g_hle_native_sink;   // native kernel's emitted packet
extern bool g_iv2_hle_sink_active;        // VUops.cpp: append interp XGKICK fragments to g_hle_interp_sink
extern bool g_iv2_suppress_interp_xgk;    // VUops.cpp: interp runs for side-effects but does NOT drive GS
extern bool g_iv2_hle_cap_xgk_active;     // VUops.cpp: dump emitted packets via iv2_hle_cap_xgk_packet
extern "C" void iv2_hle_sink_packet(const u8* data, u32 bytes);
extern "C" void iv2_hle_cap_xgk_packet(const u8* data, u32 bytes, u32 addr);

// ── Entry-state snapshot (published by ExecuteSync at program entry; read by kernels + harness) ─
extern bool g_hle_hotkick;        // true while a registered hot VU1 program is executing (Vif_Unpack/iv2_vu_ops gate)
extern u8   g_hle_snap_mem[0x4000];
extern u32  g_hle_snap_vi[16];
extern u32  g_hle_snap_top;       // XTOP (VIF1 double-buffer TOP, qw)
extern u32  g_hle_snap_itop;      // VIF1 ITOP = vertex count (static-T&L kernels read this)
extern u32  g_hle_snap_itops, g_hle_snap_tops;   // VIF double-buffer shadow (deep-RE count-source probe)
extern u32  g_hle_snap_vf[32*4];  // VF regs at entry (GTA matrices live in VF, not mem)
extern u32  g_sc_cur_pc;          // entryPC of the program under self-check (per-program breakdown)
extern u64  g_pg_declined[6];     // declined per program (0=pc1454 1=pc1435 2=pc1365 3=other 4=f8ea 5=603a)
// rolling ring of the last 8 VU1 programs' EXIT integer-register file + hash (loader-hook probe)
extern u32  g_vu1exit_vi[8][16];
extern u64  g_vu1exit_hash[8];
extern u32  g_vu1exit_itop[8];
extern int  g_vu1exit_pos, g_vu1exit_n;
// compute-time accounting (ExecuteSync stamps these; the harness reports them)
extern u64  g_sc_interp_ns, g_sc_native_ns, g_sc_time_kicks;

// ── Harness entry points (called by the integration point) ──────────────────────────────────
bool g_census_enabled();
void iv2_hle_selfcheck_compare(int nbytes);
void iv2_hle_cap_entry(const u8* in, const u32* vi, u64 hash, u32 pc, u32 xgkaddr);
void iv2_hle_cap_exit(const VURegs& vu);
void iv2_hle_disasm_once(const u8* micro, u32 entryPC);
void iv2_vu1_survey_add(u64 h, u32 pc, u64 ns);
// [P101-18 locality probe] 粒度別の program 同一性を同時計測する (診断専用・env gated)。
//   h    = 従来の 16KB micro 全体 hash / ckey = entry PC からの静的到達閉包だけの hash
//   cn   = 閉包の命令数 / clo..chi = 閉包の PC 範囲 / cind = 間接分岐で閉包が不完全
void iv2_vu1_survey_add2(u64 h, u32 pc, u64 ns, u64 ckey, u32 cn, u32 clo, u32 chi, u32 cind);

// ── Read-only probe rings (DEFINED in other TUs; read by the harness/integration) ───────────
// VIF1 unpack stream ring (Vif_Unpack.cpp).
extern bool g_vifcap_on;
extern u32  g_vifcap_addr[];
extern u8   g_vifcap_data[][16];
extern u8   g_vifcap_hot[];
extern std::atomic<u32> g_vifcap_seq;
// §5.7 read/write-set ring (iv2_vu_ops.cpp). Identical struct also defined there (ODR-compatible).
extern bool g_rwset_on;
struct IV2_RWSetEntry { u32 pc; u32 addr; u8 val[16]; u8 kind; };
extern IV2_RWSetEntry g_rwset[];
extern std::atomic<u32> g_rwset_seq;
