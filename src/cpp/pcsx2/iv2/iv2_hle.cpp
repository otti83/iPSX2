// SPDX-License-Identifier: GPL-3.0+
// iv2 kernel-HLE framework — harness implementation (P101-7/8/9; extracted from iv2_vu.cpp 2026-06-27)
//
// Game-AGNOSTIC machinery for the native-T&L HLE described in iv2_hle.h:
//   - kernel registry aggregator (iv2_hle_lookup over per-title tables)
//   - mode/env selection (iPSX2_HLE_KERNEL / EVAL_PC / CAP_PC / CAP_HASH)
//   - GIF packet sinks + sink hook (VUops.cpp feeds interp XGKICK fragments here)
//   - I/O capture + microprogram disasm probes (iPSX2_HLE_CAP / HLE_DIS)
//   - GS-bound packet census + per-vertex self-check (interp ground-truth vs native)
//   - per-program VU1 coverage survey (iPSX2_VU1_SURVEY)
// Per-title kernels are in iv2_hle_<game>.cpp; the VU float-prim lib is iv2_hle_vumath.h.
// All paths are env-gated default-off => ship-safe (kmode=0 => lookup never runs).
#include "PrecompiledHeader.h"
#include "Common.h"
#include "VUmicro.h"
#include "Gif_Unit.h"             // CliffDiag::vu1CurProgHash (program content hash)
#include "DebugTools/Debug.h"     // disVU1MicroUF/LF (hot-program disasm)
#include "iv2/iv2_hle.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <atomic>

// ──────────────────────────────────────────────────────────────────────────────────────────
// Entry-state snapshot + accounting globals (extern-linkage; ExecuteSync writes, harness reads).
// ──────────────────────────────────────────────────────────────────────────────────────────
bool g_hle_hotkick = false;          // true while a registered hot VU1 program is executing
u32  g_sc_cur_pc = 0;                // entryPC of the program currently under self-check
IV2_HleLiveOut g_iv2_hle_liveout{};  // [J-Phoenix scene-2] kernel-published VU1 exit registers (mode-2 write-back)
u64  g_iv2_hle_active_hash = 0;      // [J-Phoenix scene-2] hash of the kernel ExecuteSync is about to call
u64  g_pg_declined[6] = {0,0,0,0,0,0};  // declined per program (0=pc1454 1=pc1435 2=pc1365 3=other 4=f8ea 5=603a)
u8   g_hle_snap_mem[0x4000];         // the entry snapshot the kernel uses
u32  g_hle_snap_vi[16];              // VI regs of the snapshot kick
u32  g_hle_snap_top = 0;             // XTOP of the snapshot kick
u32  g_hle_snap_itop = 0;            // VIF1 ITOP = vertex count (static-T&L kernels)
u32  g_hle_snap_itops = 0, g_hle_snap_tops = 0;  // VIF double-buffer shadow (deep-RE count source)
u32  g_hle_snap_vf[32*4];            // VF regs at entry (GTA matrices live in VF, not mem)
// rolling ring of the last 8 VU1 programs' EXIT integer-register file + hash (loader-hook probe)
u32  g_vu1exit_vi[8][16];
u64  g_vu1exit_hash[8];
u32  g_vu1exit_itop[8];
int  g_vu1exit_pos = 0, g_vu1exit_n = 0;
// Compute-time accounting: interp loop (VU1 compute, incl XGKICK->GS) vs native kernel (compute only).
u64  g_sc_interp_ns = 0, g_sc_native_ns = 0, g_sc_time_kicks = 0;

// ── GIF packet sinks (PktSink defined in iv2_hle.h; instances shared with VUops.cpp/ExecuteSync) ──
PktSink g_hle_interp_sink;
PktSink g_hle_native_sink;

// _vuXGKICKTransfer (VUops.cpp) appends each emitted packet fragment here while active.
bool g_iv2_hle_sink_active = false;
bool g_iv2_suppress_interp_xgk = false;   // [P101-9 feasibility test] interp side-effects without GS draw
extern "C" void iv2_hle_sink_packet(const u8* data, u32 bytes);
extern "C" void iv2_hle_sink_packet(const u8* data, u32 bytes)
{
    if (g_iv2_hle_sink_active) g_hle_interp_sink.add(data, bytes);
}

// [HLE_KERNEL PoC] one-shot disassembly dump of each distinct hot T&L program
// (env iPSX2_HLE_DIS=1). Writes to /tmp/ipsx2_hle_dis.txt (file, not Console — the
// log allowlist drops some @@ markers). entryPC is in instruction units (Micro is
// byte-indexed: instr i lives at micro[i*8], lower=word0, upper=word1).
void iv2_hle_disasm_once(const u8* micro, u32 entryPC)
{
    static uint64_t dumped[32] = {0};
    static int ndump = 0;
    const uint64_t h = CliffDiag::vu1CurProgHash(micro);
    const uint64_t key = h ^ ((uint64_t)entryPC << 48);   // distinguish entry points into same code
    for (int i = 0; i < ndump; ++i) if (dumped[i] == key) return;
    if (ndump < 32) dumped[ndump++] = key; else return;

    FILE* f = std::fopen("/tmp/ipsx2_hle_dis.txt", "a");
    if (!f) return;

    // [HLE_KERNEL Stage2-b] Also dump the full pc[0,212) region once per distinct program
    // hash — this captures BAL subroutines (e.g. the MVP-matrix loader at pc~71) that the
    // entry-point-only walk skips (it stops at the first E-bit).
    {
        static uint64_t fulldumped[8] = {0};
        static int nfull = 0;
        bool seen = false;
        for (int i = 0; i < nfull; ++i) if (fulldumped[i] == h) { seen = true; break; }
        if (!seen && nfull < 8) {
            fulldumped[nfull++] = h;
            std::fprintf(f, "==== FULL program hash=%016llx pc[0,512) ====\n", (unsigned long long)h);
            for (u32 pc = 0; pc < 512; ++pc) {
                const u32* p = (const u32*)&micro[pc * 8];
                const u32 lo = p[0], up = p[1];
                std::string u = disVU1MicroUF(up, pc * 8);
                std::string l = disVU1MicroLF(lo, pc * 8);
                std::fprintf(f, "%4u  %08x:%08x  %-36s | %s\n", pc, up, lo, u.c_str(), l.c_str());
            }
            std::fprintf(f, "==== end FULL (hash=%016llx) ====\n\n", (unsigned long long)h);
        }
    }

    std::fprintf(f, "==== program hash=%016llx entryPC=%u ====\n", (unsigned long long)h, entryPC);
    int after_e = -1;
    for (u32 pc = entryPC; pc < 2048; ++pc)
    {
        const u32* p = (const u32*)&micro[pc * 8];
        const u32 lo = p[0], up = p[1];
        std::string u = disVU1MicroUF(up, pc * 8);
        std::string l = disVU1MicroLF(lo, pc * 8);
        std::fprintf(f, "%4u  %08x:%08x  %-36s | %s\n", pc, up, lo, u.c_str(), l.c_str());
        if (after_e >= 0) { if (++after_e >= 1) break; }   // E-bit: run the delay-slot instr then stop
        else if (up & 0x40000000) after_e = 0;             // [E] program-end bit
        if (pc - entryPC > 400) { std::fprintf(f, "... (truncated at 400)\n"); break; }
    }
    std::fprintf(f, "==== end (hash=%016llx) ====\n\n", (unsigned long long)h);
    std::fclose(f);
}

// [HLE_KERNEL Stage2-b] XGKICK-time output capture. When g_iv2_hle_cap_xgk_active is
// set (during a captured hot-program run), _vuXGKICKTransfer (VUops.cpp) calls
// iv2_hle_cap_xgk_packet for every emitted GIF packet → pins the EXACT GS output
// (GIFtag + register data). This is the ground truth a native kernel must reproduce.
bool g_iv2_hle_cap_xgk_active = false;
extern "C" void iv2_hle_cap_xgk_packet(const u8* data, u32 bytes, u32 addr);
extern "C" void iv2_hle_cap_xgk_packet(const u8* data, u32 bytes, u32 addr)
{
    FILE* f = std::fopen("/tmp/ipsx2_hle_cap.txt", "a");
    if (!f) return;
    std::fprintf(f, "-- XGKICK OUT addr=0x%x bytes=%u --\n", addr, bytes);
    for (u32 o = 0; o < bytes; o += 16) {
        const u32* w = (const u32*)(data + o);
        const float* fl = (const float*)w;
        std::fprintf(f, "  +%04x: %08x %08x %08x %08x | %13.5f %13.5f %13.5f %13.5f\n",
            o, w[0], w[1], w[2], w[3], fl[0], fl[1], fl[2], fl[3]);
    }
    std::fclose(f);
}

// [HLE_KERNEL Stage2-b] I/O capture entry: snapshot input VU mem (matrices/constants/
// vertex stream) + all 16 VI registers (vertex stream base via XTOP/ILW) for one hot
// routine invocation. Output packets are appended in-between via iv2_hle_cap_xgk_packet.
void iv2_hle_cap_entry(const u8* in, const u32* vi, u64 hash, u32 pc, u32 xgkaddr)
{
    FILE* f = std::fopen("/tmp/ipsx2_hle_cap.txt", "a");
    if (!f) return;
    std::fprintf(f, "======== HOT KICK START hash=%016llx pc=%u xgkickaddr=0x%x ========\n",
        (unsigned long long)hash, pc, xgkaddr);
    std::fprintf(f, "-- VI[0..15] at entry --\n");
    for (int i = 0; i < 16; ++i)
        std::fprintf(f, "  vi%02d=0x%08x (%d)\n", i, vi[i], (int)(s16)(vi[i] & 0xFFFF));
    std::fprintf(f, "-- INPUT VU mem (non-zero quadwords) off | hex | float --\n");
    for (u32 o = 0; o < 0x4000; o += 16) {
        const u32* w = (const u32*)(in + o);
        if (w[0] | w[1] | w[2] | w[3]) {
            const float* fl = (const float*)w;
            std::fprintf(f, "  %04x: %08x %08x %08x %08x | %13.5f %13.5f %13.5f %13.5f\n",
                o, w[0], w[1], w[2], w[3], fl[0], fl[1], fl[2], fl[3]);
        }
    }
    std::fprintf(f, "-- OUTPUT PACKETS (XGKICK-time) --\n");
    std::fclose(f);
}

// [HLE_KERNEL Stage2-b] I/O capture exit: VF regs at program end.
void iv2_hle_cap_exit(const VURegs& vu)
{
    FILE* f = std::fopen("/tmp/ipsx2_hle_cap.txt", "a");
    if (!f) return;
    std::fprintf(f, "-- VF[0..31] at exit --\n");
    for (int i = 0; i < 32; ++i) {
        const float* v = (const float*)&vu.VF[i];
        const u32* w = (const u32*)&vu.VF[i];
        std::fprintf(f, "  vf%02d: %08x %08x %08x %08x | %13.5f %13.5f %13.5f %13.5f\n",
            i, w[0], w[1], w[2], w[3], v[0], v[1], v[2], v[3]);
    }
    std::fprintf(f, "======== HOT KICK END ========\n\n");
    std::fclose(f);
}

int iv2_hle_kernel_mode()
{
    // Debug override (backward compat, CLAUDE.md rule 5c): an explicit env var still wins so A/B and
    // self-check (=1) work unchanged. Cached once; <0 means "unset, fall through to INI".
    static const int env = []{ const char* e = std::getenv("iPSX2_HLE_KERNEL"); return e ? atoi(e) : -1; }();
    if (env >= 0)
        return env;
    // Ship default: driven by INI [EmuCore] EnableGameHLE (default ON). On => mode 2 (native-only, drives
    // GS with `validated` kernels only). This is the device path — no env vars available on real hardware.
    return EmuConfig.EnableGameHLE ? 2 : 0;
}
// [P101-9 eval] mode-2 may drive a NON-validated kernel for visual evaluation if its entryPC matches
// iPSX2_HLE_EVAL_PC (default -1 = off). Lets the GTA geometry kernel render for user visual check
// WITHOUT flipping its ship `validated` flag (= ship stays safe). Default-off.
int iv2_hle_eval_pc()
{
    static const int p = []{ const char* e = std::getenv("iPSX2_HLE_EVAL_PC"); return e ? atoi(e) : -1; }();
    return p;
}
// [P101-9 top-3 capture] When set (iPSX2_HLE_CAP_PC=<pc>), restrict the mode-1 self-check (and thus
// op-trace / vifcap / rwset capture) to ONLY the program whose entryPC matches, so a single program
// can be RE'd in isolation. Default -1 = no filter (all registered programs self-check as before).
int iv2_hle_cap_pc()
{
    static const int p = []{ const char* e = std::getenv("iPSX2_HLE_CAP_PC"); return e ? atoi(e) : -1; }();
    return p;
}
// [P101-9 capture] iPSX2_CAP_HASH=<hex> captures an UNREGISTERED program by content hash (op-trace +
// I/O snapshot via the self-check machinery), so a new RE target can be captured without first knowing
// its entryPC. Default 0 = off.
u64 iv2_hle_cap_hash()
{
    static const u64 h = []{ const char* e = std::getenv("iPSX2_CAP_HASH"); return e ? strtoull(e, nullptr, 16) : 0ULL; }();
    return h;
}

// Self-check accounting.
static u64 s_sc_total = 0, s_sc_match = 0, s_sc_notimpl = 0, s_sc_lenmis = 0, s_sc_bytemis = 0;
static u32 s_sc_last_firstdiff = 0, s_sc_last_ilen = 0, s_sc_last_nlen = 0, s_sc_last_ndiff = 0;
// Field-classified mismatch byte counters (header 96B; then per-vertex 3 qw: ST@0,RGBAQ@16,XYZF2@32).
static u64 s_sc_b_geom = 0, s_sc_b_color = 0, s_sc_b_st = 0, s_sc_b_flag = 0, s_sc_b_hdr = 0;
static u64 s_sc_cmp_bytes = 0;
// On-screen frame accounting: a frame is "on-screen" if any interp XYZF2.z is non-saturated.
static u64 s_sc_onscr_frames = 0, s_sc_onscr_match = 0, s_sc_onscr_bytes = 0, s_sc_onscr_mis = 0;
static double s_sc_onscr_max_st_rel = 0.0;   // max relative S/T error on on-screen verts
static u64 s_sc_onscr_verts = 0, s_sc_onscr_verts_stbad = 0;  // on-screen verts; with S/T rel>1%
static inline float u2f_sc(u32 u){ float f; std::memcpy(&f,&u,4); return f; }

// ── [P101-9 deep-RE] GS-bound packet census ──────────────────────────────────────────────
// The per-vertex self-check (GTACHK) only scores kicks whose native length matches the interp
// length; every kick that emits a leading A+D GS-config packet, multiple strips, or a different
// FLG/NREG/count is silently excluded — yet in mode-2 the kernel REPLACES the whole stream for
// those too, so dropping/mismatching them desyncs the GS (= the observed collapse). This census
// PARSES the complete interp GS-bound stream (g_hle_interp_sink) into its constituent GIF packets and
// tallies the structure vs the kernel's single packet, per program. env iPSX2_HLECENSUS=1,
// default-off, mode-1 read-only, writes /tmp/ipsx2_hlecensus.txt. Combined with the user's save
// state this pins exactly what the kernel must reproduce to be mode-2-safe.
struct GifPktInfo { u32 off, bytes; u16 nloop, nreg; u8 flg, eop; u64 regs; u32 prim; u8 pre; };
static int census_parse(const u8* buf, u32 len, GifPktInfo* out, int maxp) {
    int n = 0; u32 o = 0;
    while (o + 16 <= len && n < maxp) {
        u64 lo, hi; std::memcpy(&lo, buf + o, 8); std::memcpy(&hi, buf + o + 8, 8);
        const u16 nloop = (u16)(lo & 0x7fff);
        const u8  eop   = (u8)((lo >> 15) & 1);
        const u8  pre   = (u8)((lo >> 46) & 1);
        const u32 prim  = (u32)((lo >> 47) & 0x7ff);
        const u8  flg   = (u8)((lo >> 58) & 3);
        u8 nreg = (u8)((lo >> 60) & 0xf); if (nreg == 0) nreg = 16;
        u32 data;
        if (flg == 0)      data = (u32)nloop * nreg * 16;                 // PACKED: NREG qw/loop
        else if (flg == 1) data = (((u32)nloop * nreg + 1) / 2) * 16;     // REGLIST: 2 regs/qw
        else               data = (u32)nloop * 16;                        // IMAGE / disable
        out[n].off = o; out[n].bytes = 16 + data; out[n].nloop = nloop; out[n].nreg = nreg;
        out[n].flg = flg; out[n].eop = eop; out[n].regs = hi; out[n].prim = prim; out[n].pre = pre;
        ++n;
        if (16 + data == 0) break;            // malformed guard
        o += 16 + data;
    }
    return n;
}
bool g_census_enabled()
{
    static const bool on = (std::getenv("iPSX2_HLECENSUS") != nullptr);
    return on;
}
static void iv2_hle_census(int nbytes)
{
    if (__builtin_expect(!g_census_enabled(), 1)) return;
    if (g_hle_interp_sink.ovf || g_hle_interp_sink.len < 16) return;
    const int pgi = (g_sc_cur_pc==1454)?0 : (g_sc_cur_pc==1435)?1 : (g_sc_cur_pc==1365)?2
                  : (g_sc_cur_pc==9991)?4 : (g_sc_cur_pc==9992)?5 : 3;
    // per-program tallies
    static u64 inv[6]={0}, decl[6]={0}, lenmatch[6]={0}, fullexact[6]={0}, structmatch[6]={0};
    static u64 pkts1[6]={0}, pkts2[6]={0}, pkts3[6]={0}, pkts4p[6]={0};
    static u64 has_ad[6]={0}, has_reglist[6]={0}, multivert[6]={0};
    static u64 sumpkts[6]={0};
    // [one-shot] dump raw VU microcode for f8ea/603a so the strip-loop control flow + load
    // addresses can be disassembled offline (the count/strip structure is computed, not resident).
    if (pgi==4 || pgi==5) {
        static bool ud[6]={false,false,false,false,false,false};
        if (!ud[pgi]) { ud[pgi]=true;
            FILE* uf = std::fopen(pgi==4?"/tmp/ipsx2_f8ea.ucode":"/tmp/ipsx2_603a.ucode","wb");
            if (uf) { std::fwrite(VU1.Micro, 1, 0x4000, uf); std::fclose(uf); } }
    }
    GifPktInfo ip[64]; const int np = census_parse(g_hle_interp_sink.buf, g_hle_interp_sink.len, ip, 64);
    GifPktInfo kp[8];  const int nk = (nbytes > 0) ? census_parse(g_hle_native_sink.buf, (u32)nbytes, kp, 8) : 0;
    inv[pgi]++;
    if (nbytes < 0) decl[pgi]++;
    sumpkts[pgi] += (u64)np;
    if (np==1) pkts1[pgi]++; else if (np==2) pkts2[pgi]++; else if (np==3) pkts3[pgi]++; else pkts4p[pgi]++;
    // structural features of the interp stream
    int adloops=0, vertpkts=0, reglist=0;
    for (int i=0;i<np;++i){
        if (ip[i].flg==1) reglist++;
        if (ip[i].flg==0 && ip[i].nreg==1 && (ip[i].regs & 0xf)==0xe) adloops++;      // A+D register-writes packet
        if (ip[i].flg==0 && ip[i].regs==0x412ULL) vertpkts++;                          // [ST,RGBAQ,XYZF2] vertex packet
    }
    if (adloops>0)  has_ad[pgi]++;
    if (reglist>0)  has_reglist[pgi]++;
    if (vertpkts>1) multivert[pgi]++;
    if (nbytes > 0 && (u32)nbytes == g_hle_interp_sink.len) {
        lenmatch[pgi]++;
        if (std::memcmp(g_hle_native_sink.buf, g_hle_interp_sink.buf, g_hle_interp_sink.len)==0) fullexact[pgi]++;
    }
    // structure-match = kernel's single packet has the SAME packet count/flg/nloop/nreg/regs as interp
    if (nk==np && np>=1) {
        bool sm=true;
        for (int i=0;i<np;++i)
            if (kp[i].flg!=ip[i].flg || kp[i].nloop!=ip[i].nloop || kp[i].nreg!=ip[i].nreg || kp[i].regs!=ip[i].regs) { sm=false; break; }
        if (sm) structmatch[pgi]++;
    }
    // [count-source finder] for the first few f8ea/603a kicks, locate where the TRUE per-strip
    // count (= interp NLOOP) lives in the entry snapshot / VI regs, and dump the strip-list layout.
    static int cs_dumped[6]={0};
    if ((pgi==4 || pgi==5) && np>=1 && cs_dumped[pgi] < 10) {
        cs_dumped[pgi]++;
        FILE* cf = std::fopen("/tmp/ipsx2_count_source.txt", "a");
        if (cf) {
            const u32 n0 = ip[0].nloop, n1 = (np>1)?ip[1].nloop:0, n2 = (np>2)?ip[2].nloop:0;
            const u32 ntot = n0+n1+n2;
            // [deep-RE] test VIF double-buffer fields as the count source (itop/itops + top/tops)
            std::fprintf(cf, "[%s] np=%d nloop=[%u %u %u] ntot=%u | itop=%u itops=%u | top=0x%x tops=0x%x in_top=0x%x%s%s%s%s | VI:",
                pgi==4?"f8ea":"603a", np, n0, n1, n2, ntot,
                g_hle_snap_itop & 0x7fff, g_hle_snap_itops & 0x7fff,
                g_hle_snap_top & 0x3ff, g_hle_snap_tops & 0x3ff, g_hle_snap_top,
                ((g_hle_snap_itop&0x7fff)==n0)?" itop==n0":"", ((g_hle_snap_itops&0x7fff)==n0)?" itops==n0":"",
                ((g_hle_snap_itop&0x7fff)==ntot)?" itop==ntot":"", ((g_hle_snap_itops&0x7fff)==ntot)?" itops==ntot":"");
            for (int i=0;i<16;++i) std::fprintf(cf, " %d=0x%x", i, g_hle_snap_vi[i]);
            std::fprintf(cf, "\n");
            // scan entry mem for words equal to the real counts (find count source)
            auto scan = [&](u32 target, const char* lbl){
                if (!target) return; int hits=0;
                std::fprintf(cf, "   find %s=%u @:", lbl, target);
                for (u32 q=0;q<0x400 && hits<24;++q){ const u32* m=(const u32*)(g_hle_snap_mem+q*16);
                    for (int w=0;w<4;++w) if (m[w]==target){ std::fprintf(cf," qw%u.%d",q,w); if(++hits>=24)break; } }
                std::fprintf(cf, "\n");
            };
            scan(n0, "nloop0"); if (n1) scan(n1, "nloop1"); if (n2) scan(n2, "nloop2");
            // [P101-9 B] search the recent program-EXIT ring for the true count (n0 / ntot) — is the
            // count delivered by a loader program's exit register file (= hook target)?
            {
                std::fprintf(cf, "   RING (last %d prog exits, newest first):\n", g_vu1exit_n);
                for (int d = 1; d <= g_vu1exit_n; ++d) {
                    const int j = (g_vu1exit_pos - d) & 7;
                    char hit[256]; int ho = 0; hit[0] = 0;
                    for (int r = 0; r < 16; ++r) {
                        const u32 v = g_vu1exit_vi[j][r];
                        if (v==n0)            ho += std::snprintf(hit+ho, sizeof(hit)-ho, " vi%d==n0", r);
                        else if (v==ntot)     ho += std::snprintf(hit+ho, sizeof(hit)-ho, " vi%d==ntot", r);
                        else if (n1 && v==n1) ho += std::snprintf(hit+ho, sizeof(hit)-ho, " vi%d==n1", r);
                    }
                    const u32 it = g_vu1exit_itop[j] & 0x7fff;
                    std::fprintf(cf, "     -%d hash=%016llx itop=%u vi9=%u vi1=0x%x%s%s\n",
                        d, (unsigned long long)g_vu1exit_hash[j], it, g_vu1exit_vi[j][9], g_vu1exit_vi[j][1],
                        (it==n0?"  itop==n0":it==ntot?"  itop==ntot":""), hit[0]?hit:"  (no count match)");
                }
            }
            // dump candidate header/control regions raw
            for (u32 q : {0u,1u,2u,3u, (g_hle_snap_top&0x3ffu), 1018u,1019u,1020u,1021u,1022u,1023u}) {
                const u32* m=(const u32*)(g_hle_snap_mem+(q&0x3ff)*16);
                std::fprintf(cf, "   qw%-4u: %08x %08x %08x %08x\n", q&0x3ff, m[0],m[1],m[2],m[3]);
            }
            std::fclose(cf);
        }
    }
    // dump examples of DIVERGENT kicks (np!=1 or len mismatch) for f8ea/603a, first few each
    static int ex_dumped[6]={0};
    const bool divergent = (np!=1) || (nbytes<=0) || ((u32)nbytes != g_hle_interp_sink.len);
    if ((pgi==4 || pgi==5 || pgi==0) && divergent && ex_dumped[pgi] < 8) {
        ex_dumped[pgi]++;
        FILE* ef = std::fopen("/tmp/ipsx2_hlecensus_examples.txt", "a");
        if (ef) {
            std::fprintf(ef, "[%s] kick: interp_len=%u native_len=%d interp_pkts=%d native_pkts=%d itop=%u XTOP=0x%x\n",
                pgi==4?"f8ea":pgi==5?"603a":"pc1454", g_hle_interp_sink.len, nbytes, np, nk,
                g_hle_snap_itop & 0x7fff, g_hle_snap_top & 0x3ff);
            for (int i=0;i<np && i<16;++i)
                std::fprintf(ef, "   interp p%d: off=%u bytes=%u flg=%u nloop=%u nreg=%u eop=%u pre=%u prim=0x%x regs=0x%llx%s\n",
                    i, ip[i].off, ip[i].bytes, ip[i].flg, ip[i].nloop, ip[i].nreg, ip[i].eop, ip[i].pre, ip[i].prim,
                    (unsigned long long)ip[i].regs,
                    (ip[i].flg==0 && ip[i].nreg==1 && (ip[i].regs&0xf)==0xe)?"  <-- A+D config":
                    (ip[i].flg==0 && ip[i].regs==0x412ULL)?"  <-- vertex[ST,RGBAQ,XYZF2]":"");
            for (int i=0;i<nk && i<8;++i)
                std::fprintf(ef, "   native p%d: bytes=%u flg=%u nloop=%u nreg=%u regs=0x%llx\n",
                    i, kp[i].bytes, kp[i].flg, kp[i].nloop, kp[i].nreg, (unsigned long long)kp[i].regs);
            std::fclose(ef);
        }
    }
    if ((inv[pgi] % 128) != 0) return;
    FILE* f = std::fopen("/tmp/ipsx2_hlecensus.txt", "w");
    if (!f) return;
    static const char* nm[6]={"pc1454","pc1435","pc1365","other","f8ea","603a"};
    std::fprintf(f, "=== HLECENSUS (mode-1, interp ground-truth GS stream vs kernel single packet) ===\n");
    for (int b=0;b<6;++b){
        if (!inv[b]) continue;
        const double iv=(double)inv[b];
        std::fprintf(f, "%-6s inv=%llu decl=%llu | pkts/kick: 1=%llu 2=%llu 3=%llu 4+=%llu (avg=%.2f) | "
            "lenmatch=%llu(%.1f%%) fullexact=%llu(%.1f%%) structmatch=%llu(%.1f%%) | hasA+D=%llu(%.1f%%) reglist=%llu multivert=%llu\n",
            nm[b], (unsigned long long)inv[b], (unsigned long long)decl[b],
            (unsigned long long)pkts1[b],(unsigned long long)pkts2[b],(unsigned long long)pkts3[b],(unsigned long long)pkts4p[b],
            (double)sumpkts[b]/iv,
            (unsigned long long)lenmatch[b], 100.0*(double)lenmatch[b]/iv,
            (unsigned long long)fullexact[b], 100.0*(double)fullexact[b]/iv,
            (unsigned long long)structmatch[b], 100.0*(double)structmatch[b]/iv,
            (unsigned long long)has_ad[b], 100.0*(double)has_ad[b]/iv,
            (unsigned long long)has_reglist[b], (unsigned long long)multivert[b]);
    }
    std::fclose(f);
}

void iv2_hle_selfcheck_compare(int nbytes)
{
    iv2_hle_census(nbytes);
    s_sc_total++;
    // [J-Phoenix scene-2 BLACK-BG diag] Per-program color/coverage census. The drawn-vert probe below
    // dismisses RGBAQ diff as "approx OK", but a kernel that paints LIT background geometry BLACK is a
    // VISUAL FAIL (user: "ステージの背景が真っ暗"). For each kick, key by entryPC and count drawn verts
    // (interp ADC=0) that are lit in interp (R+G+B>60) yet near-black in native (<12) = blacked-out.
    // Handles NREG=2 ([RGBAQ,XYZF2] 32B) and NREG=3 ([ST,RGBAQ,XYZF2] 48B). Read-only, env iPSX2_JPX_BG.
    if (__builtin_expect(std::getenv("iPSX2_JPX_BG") != nullptr, 0)) {
        const u32 pc = g_sc_cur_pc;
        const int bi = (pc==5)?0:(pc==18)?1:(pc==19)?2:(pc==59)?3:(pc==63)?4:(pc==87)?5:6;
        static u64 bg_kicks[7]={0}, bg_idrawn[7]={0}, bg_ndrawn[7]={0}, bg_blackout[7]={0};
        static u64 bg_isum[7]={0}, bg_nsum[7]={0}; static u32 bg_nreg[7]={0};
        static u64 bg_ia_sum[7]={0}, bg_na_sum[7]={0}, bg_transout[7]={0};  // alpha: interp opaque but native transparent
        const u8* ib = g_hle_interp_sink.buf; const u32 ilen = g_hle_interp_sink.len;
        const u8* nb = g_hle_native_sink.buf; const u32 nlen = (nbytes>0)?(u32)nbytes:0;
        if (ilen >= 16 && !g_hle_interp_sink.ovf) {
            u64 lo; std::memcpy(&lo, ib, 8);
            u32 nreg = (u32)((lo>>60)&0xf); if (nreg==0) nreg=16;
            const u32 flg = (u32)((lo>>58)&3);
            bg_nreg[bi] = nreg;
            if (flg==0 && (nreg==2 || nreg==3)) {
                const u32 stride = nreg*16;
                const u32 rgbOff = (nreg==3)?16u:0u;    // RGBAQ position within vertex
                const u32 xyzOff = (nreg==3)?32u:16u;   // XYZF2 position (word3 bit15 = ADC)
                bg_kicks[bi]++;
                for (u32 o=16; o+stride<=ilen; o+=stride) {
                    const u32 iadc = (*(const u32*)(ib+o+xyzOff+12) >> 15) & 1;
                    if (iadc) continue;                 // interp culled this vert -> not drawn
                    bg_idrawn[bi]++;
                    const u32 isum = ib[o+rgbOff+0]+ib[o+rgbOff+4]+ib[o+rgbOff+8];
                    bg_isum[bi]+=isum;
                    const u32 ia = ib[o+rgbOff+12]; bg_ia_sum[bi]+=ia;   // interp alpha (RGBAQ.A)
                    if (nlen >= o+stride) {
                        const u32 nsum = nb[o+rgbOff+0]+nb[o+rgbOff+4]+nb[o+rgbOff+8];
                        bg_nsum[bi]+=nsum; bg_ndrawn[bi]++;
                        if (isum>60 && nsum<12) bg_blackout[bi]++;   // lit in interp, black in native
                        const u32 na = nb[o+rgbOff+12]; bg_na_sum[bi]+=na;
                        if (ia>100 && na<30) bg_transout[bi]++;      // interp opaque, native transparent
                    }
                }
            }
        }
        static u64 bgtick=0;
        if ((++bgtick % 20000)==0) {
            FILE* bf = std::fopen("/tmp/ipsx2_jpx_bg.txt","w");
            if (bf) {
                const char* nm[7]={"pc5","pc18","pc19","pc59","pc63","pc87","other"};
                std::fprintf(bf,"per-program BLACK-BG diag (lit-in-interp but black-in-native = visual fail)\n");
                for (int i=0;i<7;++i) if (bg_kicks[i]) std::fprintf(bf,
                    "%-6s nreg=%u kicks=%llu idrawn=%llu meanIcol=%.1f meanNcol=%.1f BLACKOUT=%llu(%.1f%%) | meanIalpha=%.1f meanNalpha=%.1f TRANSOUT=%llu(%.1f%%)\n",
                    nm[i], bg_nreg[i],
                    (unsigned long long)bg_kicks[i],(unsigned long long)bg_idrawn[i],
                    bg_idrawn[i]?(double)bg_isum[i]/bg_idrawn[i]:0.0,
                    bg_ndrawn[i]?(double)bg_nsum[i]/bg_ndrawn[i]:0.0,
                    (unsigned long long)bg_blackout[i],
                    bg_idrawn[i]?100.0*bg_blackout[i]/bg_idrawn[i]:0.0,
                    bg_idrawn[i]?(double)bg_ia_sum[i]/bg_idrawn[i]:0.0,
                    bg_ndrawn[i]?(double)bg_na_sum[i]/bg_ndrawn[i]:0.0,
                    (unsigned long long)bg_transout[i],
                    bg_idrawn[i]?100.0*bg_transout[i]/bg_idrawn[i]:0.0);
                std::fclose(bf);
            }
        }
    }
    // [P101-8 #4 spike] Feasibility probe: for the matched program (GTA pc1454 when running
    // GTA — kernel declines, nbytes<0, but the interp output is in g_hle_interp_sink), dump the
    // captured VIF1 unpack stream + the interp GIF output, so offline RE can test whether the
    // FULL per-draw vertex input (not resident at entry) is reconstructable from the stream.
    // One-shot, env-gated (iPSX2_VIFCAP=1), read-only.
    if (__builtin_expect(g_vifcap_on, 0)) {
        static bool s_vifcap_done = false;
        if (!s_vifcap_done && g_hle_interp_sink.len > 2000 && !g_hle_interp_sink.ovf) {
            s_vifcap_done = true;
            FILE* f = std::fopen("/tmp/ipsx2_vifcap.txt", "w");
            if (f) {
                const u32 seq = g_vifcap_seq.load();
                const u32 have = seq < 32768u ? seq : 32768u;
                const u32 dumpN = have < 6000u ? have : 6000u;  // last ~6000 unpack elems = a draw's worth
                std::fprintf(f, "VIFCAP interp_out_len=%u XTOP=0x%x seq=%u dumpN=%u\n",
                    g_hle_interp_sink.len, g_hle_snap_top, seq, dumpN);
                std::fprintf(f, "VI[0..15]:");
                for (int i = 0; i < 16; ++i) std::fprintf(f, " %02d=0x%08x", i, g_hle_snap_vi[i]);
                std::fprintf(f, "\n--- VF[0..31] (entry snapshot; MVP=vf23-26) ---\n");
                for (int r = 0; r < 32; ++r)
                    std::fprintf(f, "vf%02d: %08x %08x %08x %08x\n", r,
                        g_hle_snap_vf[r*4+0], g_hle_snap_vf[r*4+1], g_hle_snap_vf[r*4+2], g_hle_snap_vf[r*4+3]);
                std::fprintf(f, "--- INTERP GIF OUTPUT (%u bytes, qw) ---\n", g_hle_interp_sink.len);
                for (u32 o = 0; o + 16 <= g_hle_interp_sink.len; o += 16) {
                    const u32* m = (const u32*)(g_hle_interp_sink.buf + o);
                    std::fprintf(f, "o%04x: %08x %08x %08x %08x | %g %g %g %g\n", o,
                        m[0],m[1],m[2],m[3], (double)u2f_sc(m[0]),(double)u2f_sc(m[1]),
                        (double)u2f_sc(m[2]),(double)u2f_sc(m[3]));
                }
                std::fprintf(f, "--- VIF1 UNPACK STREAM (last %u elems, addr+16B) ---\n", dumpN);
                for (u32 k = 0; k < dumpN; ++k) {
                    const u32 idx2 = (seq - dumpN + k) & (32768u - 1);
                    const u8* d = g_vifcap_data[idx2];
                    const u32* w = (const u32*)d;
                    std::fprintf(f, "a%04x h%d: %08x %08x %08x %08x | %g %g %g %g\n",
                        g_vifcap_addr[idx2], g_vifcap_hot[idx2], w[0],w[1],w[2],w[3],
                        (double)u2f_sc(w[0]),(double)u2f_sc(w[1]),(double)u2f_sc(w[2]),(double)u2f_sc(w[3]));
                }
                std::fclose(f);
            }
        }
    }
    // [P101-9 §5.7] Eligibility trial dump: program-start VU-mem snapshot + read-set +
    // VF(MVP) + interp output. Offline RE checks what fraction of the read-set is
    // reconstructible from the start snapshot (= snapshot-replace eligibility) and pins
    // vertex base/stride/format. One-shot, env-gated (iPSX2_RWSET=1), read-only.
    if (__builtin_expect(g_rwset_on, 0)) {
        static bool s_rwset_done = false;
        if (!s_rwset_done && g_hle_interp_sink.len > 2000 && !g_hle_interp_sink.ovf) {
            s_rwset_done = true;
            FILE* f = std::fopen("/tmp/ipsx2_rwset.txt", "w");
            if (f) {
                const u32 seq = g_rwset_seq.load();
                const u32 have = seq < 65536u ? seq : 65536u;   // = this program's reads (ring reset at entry)
                std::fprintf(f, "RWSET interp_out_len=%u XTOP=0x%x reads=%u\n",
                    g_hle_interp_sink.len, g_hle_snap_top, have);
                std::fprintf(f, "VI[0..15]:");
                for (int i = 0; i < 16; ++i) std::fprintf(f, " %02d=0x%08x", i, g_hle_snap_vi[i]);
                std::fprintf(f, "\n--- VF[0..31] (MVP=vf23-26) ---\n");
                for (int r = 0; r < 32; ++r) std::fprintf(f, "vf%02d: %08x %08x %08x %08x\n", r,
                    g_hle_snap_vf[r*4+0], g_hle_snap_vf[r*4+1], g_hle_snap_vf[r*4+2], g_hle_snap_vf[r*4+3]);
                std::fprintf(f, "--- START-SNAPSHOT VU mem (nonzero qw) ---\n");
                for (u32 q = 0; q < 0x400; ++q) {
                    const u32* m = (const u32*)(g_hle_snap_mem + q*16);
                    if (!(m[0]|m[1]|m[2]|m[3])) continue;
                    std::fprintf(f, "m%04x: %08x %08x %08x %08x\n", q*16, m[0],m[1],m[2],m[3]);
                }
                std::fprintf(f, "--- INTERP OUT (qw) ---\n");
                for (u32 o = 0; o + 16 <= g_hle_interp_sink.len; o += 16) {
                    const u32* m = (const u32*)(g_hle_interp_sink.buf + o);
                    std::fprintf(f, "o%04x: %08x %08x %08x %08x\n", o, m[0],m[1],m[2],m[3]);
                }
                std::fprintf(f, "--- READ-SET (in order, pc/kind/addr/qword) reads=%u ---\n", have);
                for (u32 k = 0; k < have; ++k) {
                    const IV2_RWSetEntry& e = g_rwset[(seq - have + k) & (65536u - 1)];
                    const u32* w = (const u32*)e.val;
                    std::fprintf(f, "r pc=%u k=%d a%04x: %08x %08x %08x %08x\n",
                        e.pc, e.kind, e.addr, w[0],w[1],w[2],w[3]);
                }
                std::fclose(f);
            }
        }
    }
    // [P101-9 Phase 2] GTA-aware (16B GIFtag header + 48B/vertex) native-vs-interp field check.
    // The default classifier assumes FF10's 96B header so its geom/st/color split is wrong for GTA;
    // this compares XYZF2(X,Y,Z) / ST(S,T,Q) / RGBAQ per vertex directly. env iPSX2_VIFCAP=1, one-shot×8.
    if (__builtin_expect(g_vifcap_on, 0) && nbytes > 16 && (u32)nbytes == g_hle_interp_sink.len && !g_hle_interp_sink.ovf
        && ((nbytes - 16) % 48) == 0) {
        static u64 g_kicks=0, g_verts=0, g_geomok=0, g_xyzfok=0, g_stok=0, g_colok=0, g_hdrok=0, g_w3ok=0;
        static u64 g_col_le4=0, g_col_le16=0, g_col_maxerr=0;
        // [P101-9 top-3] per-program breakdown: bucket 0=pc1454 1=pc1435 2=pc1365 3=other.
        static u64 pg_kicks[6]={0}, pg_verts[6]={0}, pg_geomok[6]={0}, pg_stok[6]={0}, pg_colok[6]={0}, pg_le16[6]={0};
        const int pgi = (g_sc_cur_pc==1454)?0 : (g_sc_cur_pc==1435)?1 : (g_sc_cur_pc==1365)?2 : (g_sc_cur_pc==9991)?4 : (g_sc_cur_pc==9992)?5 : 3;
        const u32 nv = ((u32)nbytes - 16) / 48;
        g_kicks++; g_verts += nv; pg_kicks[pgi]++; pg_verts[pgi]+=nv;
        if (std::memcmp(g_hle_native_sink.buf, g_hle_interp_sink.buf, 16)==0) g_hdrok++;
        for (u32 v = 0; v < nv; ++v) {
            const u32* nn = (const u32*)(g_hle_native_sink.buf + 16 + v*48);
            const u32* ii = (const u32*)(g_hle_interp_sink.buf + 16 + v*48);
            const bool gok = (nn[8]==ii[8] && nn[9]==ii[9] && nn[10]==ii[10]);
            if (gok) { g_geomok++; pg_geomok[pgi]++; }                               // XYZF2 X,Y,Z
            if (nn[8]==ii[8] && nn[9]==ii[9] && nn[10]==ii[10] && nn[11]==ii[11]) g_xyzfok++; // +F/word3
            const bool sok = (nn[0]==ii[0] && nn[1]==ii[1] && nn[2]==ii[2]);
            if (sok) { g_stok++; pg_stok[pgi]++; }                                   // ST S,T,Q
            const bool cok = (nn[4]==ii[4] && nn[5]==ii[5] && nn[6]==ii[6]);
            if (cok) { g_colok++; pg_colok[pgi]++; }                                 // RGBAQ
            // color tolerance: max abs error over R,G,B (bytes of RGBAQ qw)
            u32 e=0; const u8* nb=(const u8*)&nn[4]; const u8* ib=(const u8*)&ii[4];
            for (int k=0;k<3;++k){ int d=(int)nb[k]-(int)ib[k]; if(d<0)d=-d; if((u32)d>e)e=(u32)d; }
            if (e<=4) g_col_le4++; if (e<=16) { g_col_le16++; pg_le16[pgi]++; } if (e>g_col_maxerr) g_col_maxerr=e;
        }
        if ((g_kicks % 256) == 0) {
            FILE* pf = std::fopen("/tmp/ipsx2_gta_top3.txt", "w");   // per-program breakdown
            if (pf) {
                static const u32 pcv[6]={1454,1435,1365,0,0xf8ea,0x603a};
                for (int b=0;b<6;++b) {
                    const double dv = (double)(pg_verts[b]?pg_verts[b]:1);
                    std::fprintf(pf, "pc%-5u kicks=%llu declined=%llu verts=%llu geomXYZ=%.2f%% ST=%.2f%% COL=%.2f%% COL<=16=%.2f%%\n",
                        pcv[b], (unsigned long long)pg_kicks[b], (unsigned long long)g_pg_declined[b], (unsigned long long)pg_verts[b],
                        100.0*(double)pg_geomok[b]/dv, 100.0*(double)pg_stok[b]/dv, 100.0*(double)pg_colok[b]/dv, 100.0*(double)pg_le16[b]/dv);
                }
                std::fclose(pf);
            }
            FILE* f = std::fopen("/tmp/ipsx2_gtachk.txt", "w");   // running totals (overwrite)
            if (f) { std::fprintf(f,
                "GTACHK kicks=%llu verts=%llu | geomXYZ=%llu(%.3f%%) XYZF2=%llu(%.3f%%) ST=%llu(%.3f%%) COL=%llu(%.3f%%) hdr=%llu/%llu(%.2f%%) | COLtol: <=4=%.2f%% <=16=%.2f%% maxerr=%llu\n",
                (unsigned long long)g_kicks,(unsigned long long)g_verts,
                (unsigned long long)g_geomok,100.0*(double)g_geomok/(double)(g_verts?g_verts:1),
                (unsigned long long)g_xyzfok,100.0*(double)g_xyzfok/(double)(g_verts?g_verts:1),
                (unsigned long long)g_stok, 100.0*(double)g_stok /(double)(g_verts?g_verts:1),
                (unsigned long long)g_colok,100.0*(double)g_colok/(double)(g_verts?g_verts:1),
                (unsigned long long)g_hdrok,(unsigned long long)g_kicks,100.0*(double)g_hdrok/(double)(g_kicks?g_kicks:1),
                100.0*(double)g_col_le4/(double)(g_verts?g_verts:1),
                100.0*(double)g_col_le16/(double)(g_verts?g_verts:1),
                (unsigned long long)g_col_maxerr);
                std::fclose(f); }
        }
    }
    if (nbytes < 0) { s_sc_notimpl++; s_sc_last_ilen = g_hle_interp_sink.len;
        g_pg_declined[(g_sc_cur_pc==1454)?0:(g_sc_cur_pc==1435)?1:(g_sc_cur_pc==1365)?2:(g_sc_cur_pc==9991)?4:(g_sc_cur_pc==9992)?5:3]++; }  // [P101-9]
    else if (g_hle_interp_sink.ovf || (u32)nbytes != g_hle_interp_sink.len) {
        s_sc_lenmis++;
        s_sc_last_ilen = g_hle_interp_sink.len; s_sc_last_nlen = (u32)nbytes;
    } else {
        u32 ndiff = 0, firstdiff = 0xffffffffu;
        const u32 n = g_hle_interp_sink.len;
        s_sc_cmp_bytes += n;
        for (u32 i = 0; i < n; ++i)
            if (g_hle_native_sink.buf[i] != g_hle_interp_sink.buf[i]) {
                if (firstdiff == 0xffffffffu) firstdiff = i;
                ++ndiff;
                // Classify the mismatched byte by output field.
                if (i < 96) { s_sc_b_hdr++; }
                else {
                    const u32 r = (i - 96) % 48;        // position within a vertex (ST/RGBAQ/XYZF2)
                    if (r < 16)       s_sc_b_st++;       // ST
                    else if (r < 32)  s_sc_b_color++;    // RGBAQ
                    else if (r < 44)  s_sc_b_geom++;     // XYZF2 .x/.y/.z (geometry)
                    else              s_sc_b_flag++;     // XYZF2 .w (fog/clip/cull flag)
                }
            }
        // On-screen detection: scan interp XYZF2.z (offset 96 + v*48 + 40) for a non-saturated value.
        bool onscreen = false;
        for (u32 off = 96 + 40; off + 4 <= n; off += 48) {
            const u32 z = *(const u32*)(g_hle_interp_sink.buf + off);
            if (z != 0x7fffffffu && z != 0x80000000u && z != 0) { onscreen = true; break; }
        }
        if (onscreen) {
            s_sc_onscr_frames++; s_sc_onscr_bytes += n; s_sc_onscr_mis += ndiff;
            if (ndiff == 0) s_sc_onscr_match++;
            // Magnitude of texcoord (S/T) divergence on ON-SCREEN vertices only (XYZF2.z finite):
            // confirms whether the fused-float ST diff is sub-texel (imperceptible) or large.
            for (u32 vo = 96; vo + 48 <= n; vo += 48) {
                const u32 z = *(const u32*)(g_hle_interp_sink.buf + vo + 40);
                if (z == 0x7fffffffu || z == 0x80000000u || z == 0) continue;   // off-screen vert
                s_sc_onscr_verts++;
                bool stbad = false;
                for (int w = 0; w < 2; ++w) {                                    // S, T
                    const float fi = u2f_sc(*(const u32*)(g_hle_interp_sink.buf + vo + w*4));
                    const float fn = u2f_sc(*(const u32*)(g_hle_native_sink.buf + vo + w*4));
                    const float den = (fi < 0 ? -fi : fi);
                    if (den > 1e-6f) {
                        const float d = (fi > fn ? fi - fn : fn - fi);
                        const double rel = (double)d / (double)den;
                        if (rel > s_sc_onscr_max_st_rel) s_sc_onscr_max_st_rel = rel;
                        if (rel > 0.01) stbad = true;
                    }
                }
                if (stbad) s_sc_onscr_verts_stbad++;
            }
        }
        if (ndiff == 0) s_sc_match++;
        else {
            s_sc_bytemis++; s_sc_last_firstdiff = firstdiff; s_sc_last_ndiff = ndiff; s_sc_last_ilen = n; s_sc_last_nlen = n;
            // [J-Phoenix RE] Dump the first few imperfect kicks (any size) with in_top + header so the
            // failing field can be correlated to the double-buffer bank / vertex count. Only DIFF words.
            // [J-Phoenix] target kicks with a FALSE CULL (interp ADC=0 = drawn, but native ADC=1 = culled
            // = the kernel deleted real geometry = "limb parts disappear"). Highest priority bug.
            bool dump_geom = false;
            const u32 zj0 = *(const u32*)(g_hle_snap_mem + (g_hle_snap_top & 0x3ff) * 16 + 8);
            const u32 yj0 = *(const u32*)(g_hle_snap_mem + (g_hle_snap_top & 0x3ff) * 16 + 4);
            static u64 g_jpx_stray = 0, g_jpx_fcull = 0, g_jpx_adcok = 0;
            // [HLE verify, relaxed std] DRAWN verts: geom(XYZF2 X,Y,Z)+ST MUST be byte-exact; COLOR may be approx.
            static u64 g_jpx_drawn = 0, g_jpx_d_geom = 0, g_jpx_d_st = 0, g_jpx_d_col = 0;
            if (zj0 > 0 && zj0 <= 256) {
                for (u32 v = 0; v < zj0; ++v) {
                    const u32 ia = (*(const u32*)(g_hle_interp_sink.buf + 16 + v*48 + 44) >> 15) & 1;
                    const u32 na = (*(const u32*)(g_hle_native_sink.buf + 16 + v*48 + 44) >> 15) & 1;
                    if (ia == 1 && na == 0) { g_jpx_stray++; dump_geom = true; }      // stray (I draw, interp culls)
                    else if (ia == 0 && na == 1) { g_jpx_fcull++; dump_geom = true; } // false-cull (I delete real geom)
                    else {
                        g_jpx_adcok++;
                        if (ia == 0 && na == 0) {   // both DRAW -> geom/ST must be exact; color may be approx
                            g_jpx_drawn++;
                            const u8* ip = g_hle_interp_sink.buf + 16 + v*48;
                            const u8* np = g_hle_native_sink.buf + 16 + v*48;
                            const bool dst = std::memcmp(ip + 0,  np + 0,  16) != 0;  // ST
                            const bool dco = std::memcmp(ip + 16, np + 16, 16) != 0;  // RGBAQ (allowed)
                            const bool dge = std::memcmp(ip + 32, np + 32, 12) != 0;  // XYZF2 X,Y,Z (geom)
                            if (dst) g_jpx_d_st++;
                            if (dco) g_jpx_d_col++;
                            if (dge) g_jpx_d_geom++;
                            if (dst || dge) dump_geom = true;   // SHAPE/TEXTURE divergence -> dump (color-only OK)
                        }
                    }
                }
                {
                    static u64 s_tick = 0;
                    if ((++s_tick % 20000) == 0) {
                        FILE* df = std::fopen("/tmp/ipsx2_jpx_drawn.txt", "w");
                        if (df) { std::fprintf(df,
                            "DRAWN verts=%llu | geom-diff=%llu ST-diff=%llu (MUST be 0 = shapes/textures exact) | color-diff=%llu (approx OK) | stray=%llu false-cull=%llu (MUST be 0 = cull exact)\n",
                            (unsigned long long)g_jpx_drawn, (unsigned long long)g_jpx_d_geom, (unsigned long long)g_jpx_d_st,
                            (unsigned long long)g_jpx_d_col, (unsigned long long)g_jpx_stray, (unsigned long long)g_jpx_fcull); std::fclose(df); }
                    }
                }
            }
            static int s_dumpn = 0;
            if (s_dumpn < 6 && n >= 32 && dump_geom) {
                s_dumpn++;
                FILE* f = std::fopen("/tmp/ipsx2_hle_selfcheck.txt", s_dumpn == 1 ? "w" : "a");
                if (f) {
                    const u32* hdr = (const u32*)(g_hle_snap_mem + (g_hle_snap_top & 0x3ff) * 16);
                    std::fprintf(f, "\n=== imperfect kick #%d: in_top=0x%x hdr=[%u %u %u] len=%u firstdiff=%u ndiff=%u ===\n",
                        s_dumpn, g_hle_snap_top, hdr[0], hdr[1], hdr[2], n, firstdiff, ndiff);
                    u32 nd_col = 0, nd_oth = 0;
                    for (u32 i = 0; i < n; i += 4) {
                        const u32 iv = *(const u32*)(g_hle_interp_sink.buf + i);
                        const u32 nv = *(const u32*)(g_hle_native_sink.buf + i);
                        if (iv != nv) std::fprintf(f, "  +%04x: interp=%08x native=%08x\n", i, iv, nv);
                    }
                    // [color RE] lighting constants + first lit-vertex normals & colors (in_top-relative)
                    auto SF = [&](u32 q, int c) -> float { return *(const float*)(g_hle_snap_mem + (q & 0x3ff)*16 + c*4); };
                    std::fprintf(f, "  light AMB=%g  M1x=%g,%g,%g  M1y=%g,%g,%g  M1z=%g,%g,%g  M4=%g,%g,%g\n",
                        SF(9,0), SF(0xe,0),SF(0xf,0),SF(0x10,0), SF(0xe,1),SF(0xf,1),SF(0x10,1),
                        SF(0xe,2),SF(0xf,2),SF(0x10,2), SF(0x11,0),SF(0x12,0),SF(0x13,0));
                    const u32 it = g_hle_snap_top & 0x3ff; const u32 zz = hdr[2];
                    const u32 nrmq = it + 1 + zz;
                    for (u32 v = 0; v < 4 && v < zz; ++v)
                        std::fprintf(f, "  v%u n=%g,%g,%g  iCol=%02x%02x%02x nCol=%02x%02x%02x\n", v,
                            SF(nrmq+v,0),SF(nrmq+v,1),SF(nrmq+v,2),
                            g_hle_interp_sink.buf[16+v*48+16],g_hle_interp_sink.buf[16+v*48+17],g_hle_interp_sink.buf[16+v*48+18],
                            g_hle_native_sink.buf[16+v*48+16],g_hle_native_sink.buf[16+v*48+17],g_hle_native_sink.buf[16+v*48+18]);
                    (void)nd_col; (void)nd_oth;
                    // [ADC RE] per-vertex raw interp vs native XYZF2 + RGBAQ to see exactly what differs.
                    for (u32 v = 0; v < zj0 && v < 24; ++v) {
                        const u32* iX = (const u32*)(g_hle_interp_sink.buf + 16 + v*48 + 32);  // interp XYZF2
                        const u32* nX = (const u32*)(g_hle_native_sink.buf + 16 + v*48 + 32);  // native XYZF2
                        const float cw = (v < 256) ? g_jpx_clipw[v] : 0.0f;
                        const float sx = (v < 256) ? g_jpx_sx[v] : 0.0f, sy = (v < 256) ? g_jpx_sy[v] : 0.0f;
                        const u32 adcI = (iX[3] >> 15) & 1, adcN = (nX[3] >> 15) & 1;
                        const char* tag = (adcI==0 && adcN==1) ? " <<FALSE-CULL(missing)" : (adcI==1 && adcN==0) ? " <<stray" : "";
                        std::fprintf(f, "  v%-2u iy=%u sx=%.2f sy=%.2f clipw=%.3f | iADC=%u nADC=%u | iXYZ=%08x %08x %08x%s\n",
                            v, yj0 ? v % yj0 : 0, (double)sx, (double)sy, (double)cw, adcI, adcN, iX[0],iX[1],iX[2], tag);
                    }
                    std::fclose(f);
                }
            }
            // [J-Phoenix corruption localize] classify diff bytes by output field to learn whether
            // GEOMETRY (XYZF2) is ever wrong (= visible garble) vs only color/alpha (= benign / side-effect).
            {
                const u32 zj = *(const u32*)(g_hle_snap_mem + (g_hle_snap_top & 0x3ff) * 16 + 8);  // header.z
                if (zj > 0 && zj <= 256) {
                    const u32 dB = 16 + zj * 48;   // tagB start
                    static u64 jc_any=0, jc_geom=0, jc_b30=0, jc_b30geom=0, jc_tag=0, jc_stA=0, jc_colA=0, jc_geoA=0, jc_stB=0, jc_colB=0, jc_geoB=0;
                    bool kg=false, ka=false;
                    for (u32 i = 0; i < n; i += 4) {
                        if (*(const u32*)(g_hle_native_sink.buf+i) == *(const u32*)(g_hle_interp_sink.buf+i)) continue;
                        ka = true; u32 r;
                        if (i < 16) jc_tag++;
                        else if (i < dB) { r=(i-16)%48; if(r<16)jc_stA++; else if(r<32)jc_colA++; else {jc_geoA++;kg=true;} }
                        else if (i < dB+16) jc_tag++;
                        else { r=(i-dB-16)%48; if(r<16)jc_stB++; else if(r<32)jc_colB++; else {jc_geoB++;kg=true;} }
                    }
                    if (ka) { jc_any++; if ((g_hle_snap_top & 0x3ff)==0x30) jc_b30++; }
                    if (kg) { jc_geom++; if ((g_hle_snap_top & 0x3ff)==0x30) jc_b30geom++; }
                    if ((jc_any % 2048) == 0) {
                        FILE* jf = std::fopen("/tmp/ipsx2_jpx_fields.txt", "w");
                        if (jf) { std::fprintf(jf, "JPX imperfect=%llu geomDiff=%llu | bank0x30 imperfect=%llu geomDiff=%llu | "
                            "fields: tag=%llu stA=%llu colA=%llu geoA=%llu stB=%llu colB=%llu geoB=%llu\n",
                            (unsigned long long)jc_any,(unsigned long long)jc_geom,(unsigned long long)jc_b30,(unsigned long long)jc_b30geom,
                            (unsigned long long)jc_tag,(unsigned long long)jc_stA,(unsigned long long)jc_colA,(unsigned long long)jc_geoA,
                            (unsigned long long)jc_stB,(unsigned long long)jc_colB,(unsigned long long)jc_geoB); std::fclose(jf); }
                    }
                }
            }
            static bool dumped = false;
            if (!dumped && n >= 1500 && ndiff >= 1) {   // (GTA-size) live-input companion dump
                dumped = true;
                // [ADC/ST RE] Companion dump of the LIVE input the kernel saw for THIS kick
                // (post-VIF-unpack: live MVP + texture matrix vf18..21 + all constants/streams).
                // Lets offline RE verify ADC (clip/backface) and ST S/T byte-exact, which the
                // entry-time io.txt snapshot could not (its MVP was stale). One-shot, gated by the
                // same large-kick condition. env-independent but only written during self-check.
                FILE* fi = std::fopen("/tmp/ipsx2_hle_liveinput.txt", "w");
                if (fi) {
                    std::fprintf(fi, "LIVE INPUT for self-check kick: outlen=%u XTOP=0x%x\n", n, g_hle_snap_top);
                    std::fprintf(fi, "VI[0..15]:");
                    for (int i = 0; i < 16; ++i) std::fprintf(fi, " %02d=0x%08x", i, g_hle_snap_vi[i]);
                    std::fprintf(fi, "\nqw     w0       w1       w2       w3       | floats\n");
                    for (u32 q = 0; q < 0x400; ++q) {
                        const u32* m = (const u32*)(g_hle_snap_mem + q*16);
                        if (!(m[0]|m[1]|m[2]|m[3])) continue;   // skip zero qw
                        std::fprintf(fi, "%04x: %08x %08x %08x %08x | %g %g %g %g\n", q,
                            m[0],m[1],m[2],m[3],
                            (double)*(const float*)&m[0],(double)*(const float*)&m[1],
                            (double)*(const float*)&m[2],(double)*(const float*)&m[3]);
                    }
                    std::fclose(fi);
                }
            }
        }
    }
    if ((s_sc_total % 512) == 0) {
        const double tot = (double)(s_sc_cmp_bytes ? s_sc_cmp_bytes : 1);
        const u64 misAll = s_sc_b_geom+s_sc_b_color+s_sc_b_st+s_sc_b_flag+s_sc_b_hdr;
        const double otot = (double)(s_sc_onscr_bytes ? s_sc_onscr_bytes : 1);
        Console.WriteLn(Color_Green,
            "@@HLE_SELFCHECK@@ total=%llu match=%llu lenmis=%llu bytematch=%.4f%% | mis: geom=%llu color=%llu st=%llu flag=%llu hdr=%llu | ONSCR frames=%llu fullmatch=%llu bytematch=%.4f%%",
            (unsigned long long)s_sc_total, (unsigned long long)s_sc_match,
            (unsigned long long)s_sc_lenmis,
            100.0*(1.0-(double)misAll/tot),
            (unsigned long long)s_sc_b_geom, (unsigned long long)s_sc_b_color,
            (unsigned long long)s_sc_b_st, (unsigned long long)s_sc_b_flag,
            (unsigned long long)s_sc_b_hdr,
            (unsigned long long)s_sc_onscr_frames, (unsigned long long)s_sc_onscr_match,
            100.0*(1.0-(double)s_sc_onscr_mis/otot));
        Console.WriteLn(Color_Green, "@@HLE_STMAG@@ on-screen verts=%llu  ST-bad(rel>1%%)=%llu (%.3f%%)  max_rel=%.4g",
            (unsigned long long)s_sc_onscr_verts, (unsigned long long)s_sc_onscr_verts_stbad,
            100.0*(double)s_sc_onscr_verts_stbad/(double)(s_sc_onscr_verts?s_sc_onscr_verts:1),
            s_sc_onscr_max_st_rel);
        if (g_sc_native_ns && g_sc_time_kicks) {
            Console.WriteLn(Color_Green,
                "@@HLE_TIMING@@ kicks=%llu interp_loop=%.3fus/kick native=%.3fus/kick compute_speedup=%.2fx (interp incl GS-xfer)",
                (unsigned long long)g_sc_time_kicks,
                (double)g_sc_interp_ns/1000.0/(double)g_sc_time_kicks,
                (double)g_sc_native_ns/1000.0/(double)g_sc_time_kicks,
                (double)g_sc_interp_ns/(double)(g_sc_native_ns?g_sc_native_ns:1));
        }
    }
}

// [P101-9 coverage survey] per-program VU1 wall-time over ALL programs (env iPSX2_VU1_SURVEY=1).
// Answers "what fraction of VU1 do the top-3 HLE'd programs cover across the WHOLE game" — the user
// drives varied scenes while this accumulates, then dumps a time-sorted table to /tmp/ipsx2_vu1_survey.txt.
// Default-off (the hash-per-kick cost only applies when surveying). Lets us validate the 1-scene 96.4%
// concentration across many scenes (the representativeness question).
// [P101-18] 表を 512 -> 8192 に拡張。GoW は 512 に張り付いて「真の distinct 数が測れない」
// 状態だったため、上限で切れたかどうかが分かるように overflow 件数も数える。
struct Iv2SurveyEntry { u64 hash; u32 pc; u64 ns; u64 kicks; u64 ckey; };
static constexpr int kSurveyMax = 8192;
static Iv2SurveyEntry s_survey[kSurveyMax];
static int s_survey_n = 0;
static u64 s_survey_total_ns = 0, s_survey_seen = 0, s_survey_overflow = 0;

// [P101-18 locality probe] 閉包 (= entry PC から静的に到達する命令列だけ) の粒度で集計する 2 本目の表。
// 「16KB 全体 hash では別 program に見えるが、実行される命令列は同一」を検出するのが目的。
struct Iv2ClosureEntry { u64 key; u32 pc; u32 n; u32 lo; u32 hi; u32 ind; u64 ns; u64 kicks; u32 nvariants; };
static constexpr int kClosureMax = 4096;
static Iv2ClosureEntry s_clos[kClosureMax];
static int s_clos_n = 0;
static u64 s_clos_overflow = 0;

void iv2_vu1_survey_add(u64 h, u32 pc, u64 ns) { iv2_vu1_survey_add2(h, pc, ns, 0, 0, 0, 0, 0); }

void iv2_vu1_survey_add2(u64 h, u32 pc, u64 ns, u64 ckey, u32 cn, u32 clo, u32 chi, u32 cind)
{
    s_survey_total_ns += ns;
    int i = 0; for (; i < s_survey_n; ++i) if (s_survey[i].hash == h) break;
    if (i == s_survey_n) {
        if (s_survey_n >= kSurveyMax) { ++s_survey_overflow; }
        else s_survey[s_survey_n++] = {h, pc, 0, 0, ckey};
    }
    if (i < s_survey_n) {
        if (pc) s_survey[i].pc = pc;   // record the real entry PC (first invocation can be a pc=0 init call)
        s_survey[i].ns += ns; s_survey[i].kicks++;
    }
    if (ckey) {
        int c = 0; for (; c < s_clos_n; ++c) if (s_clos[c].key == ckey) break;
        if (c == s_clos_n) {
            if (s_clos_n >= kClosureMax) { ++s_clos_overflow; c = -1; }
            else s_clos[s_clos_n++] = {ckey, pc, cn, clo, chi, cind, 0, 0, 0};
        }
        if (c >= 0) { s_clos[c].ns += ns; s_clos[c].kicks++; }
    }
    if ((++s_survey_seen % 4096) != 0) return;
    // dump time-sorted top-N (simple selection, n is small)
    FILE* f = std::fopen("/tmp/ipsx2_vu1_survey.txt", "w");
    if (!f) return;
    const double tot = s_survey_total_ns ? (double)s_survey_total_ns : 1.0;

    // 閉包 key ごとに「何本の 16KB hash に散っているか」を数える (= 粒度の水増し率)
    for (int c = 0; c < s_clos_n; ++c) s_clos[c].nvariants = 0;
    for (int j = 0; j < s_survey_n; ++j)
        for (int c = 0; c < s_clos_n; ++c) if (s_clos[c].key == s_survey[j].ckey) { ++s_clos[c].nvariants; break; }

    std::fprintf(f, "VU1 SURVEY  distinct_programs=%d (overflow=%llu)  total_kicks=%llu  total_vu1_ns=%llu\n",
        s_survey_n, (unsigned long long)s_survey_overflow,
        (unsigned long long)s_survey_seen, (unsigned long long)s_survey_total_ns);
    std::fprintf(f, "            distinct_closures=%d (overflow=%llu)  <- entry PC からの静的到達閉包での distinct 数\n",
        s_clos_n, (unsigned long long)s_clos_overflow);
    static u8 used[kSurveyMax];
    std::memset(used, 0, sizeof(used));
    double top3 = 0.0;
    auto is_hle = [](u64 h){ return h==0x8ac5daebfa578107ULL || h==0x92d542c43042231cULL || h==0x3f150dd0a1f0a9ebULL; };
    auto hle_name = [](u64 h)->const char*{ return h==0x8ac5daebfa578107ULL?"pc1454":h==0x92d542c43042231cULL?"pc1435":h==0x3f150dd0a1f0a9ebULL?"pc1365":""; };
    for (int j = 0; j < s_survey_n; ++j) if (is_hle(s_survey[j].hash)) top3 += 100.0 * (double)s_survey[j].ns / tot;
    std::fprintf(f, "--- A. 16KB 全体 hash 粒度 (従来) ---\n");
    double cum = 0.0;
    for (int rank = 0; rank < 40 && rank < s_survey_n; ++rank) {
        int best = -1; u64 bestns = 0;
        for (int j = 0; j < s_survey_n; ++j) if (!used[j] && s_survey[j].ns >= bestns) { bestns = s_survey[j].ns; best = j; }
        if (best < 0) break; used[best] = 1;
        const double pct = 100.0 * (double)s_survey[best].ns / tot;
        cum += pct;
        const u64 hh = s_survey[best].hash;
        std::fprintf(f, "  #%2d hash=%016llx pc=%-5u time=%6.2f%% cum=%6.2f%%  kicks=%llu%s%s\n",
            rank, (unsigned long long)hh, s_survey[best].pc, pct, cum, (unsigned long long)s_survey[best].kicks,
            is_hle(hh) ? "  <-- HLE'd " : "", is_hle(hh) ? hle_name(hh) : "");
    }
    if (s_clos_n) {
        std::fprintf(f, "--- B. 閉包 hash 粒度 (P101-18: 実行される命令列だけで同一性を決める) ---\n");
        static u8 cused[kClosureMax];
        std::memset(cused, 0, sizeof(cused));
        cum = 0.0;
        for (int rank = 0; rank < 40 && rank < s_clos_n; ++rank) {
            int best = -1; u64 bestns = 0;
            for (int c = 0; c < s_clos_n; ++c) if (!cused[c] && s_clos[c].ns >= bestns) { bestns = s_clos[c].ns; best = c; }
            if (best < 0) break; cused[best] = 1;
            const double pct = 100.0 * (double)s_clos[best].ns / tot;
            cum += pct;
            std::fprintf(f, "  #%2d ckey=%016llx pc=%-5u n=%-4u pc%u..%u%s time=%6.2f%% cum=%6.2f%% kicks=%llu variants16k=%u\n",
                rank, (unsigned long long)s_clos[best].key, s_clos[best].pc, s_clos[best].n,
                s_clos[best].lo, s_clos[best].hi, s_clos[best].ind ? " +indirect" : "",
                pct, cum, (unsigned long long)s_clos[best].kicks, s_clos[best].nvariants);
        }
    }
    std::fprintf(f, "TOP-3 HLE'd (pc1454/1435/1365) cover %.2f%% of VU1 time across surveyed scenes\n", top3);
    std::fclose(f);
}

// ── Kernel registry aggregator ──────────────────────────────────────────────────────────────
// Each title contributes a table co-located with its kernels (iv2_hle_<game>.cpp). Adding a new
// game = new file + one row in `tabs` below. Order is irrelevant: every (hash, entryPC) is unique.
const IV2_HleKernel* iv2_hle_lookup(u64 hash, u32 pc) {
    // iPSX2_HLE_ONLY=<entryPC> restricts the registry to a single kernel (isolates one program's
    // self-check stats). Default (unset) = all registered kernels.
    static const int s_only = []{ const char* e = std::getenv("iPSX2_HLE_ONLY"); return e ? atoi(e) : -1; }();
    struct Tab { const IV2_HleKernel* e; int n; };
    const Tab tabs[] = {
        { kIv2HleKernels_ff10,     kIv2HleKernels_ff10_count     },
        { kIv2HleKernels_gtavc,    kIv2HleKernels_gtavc_count    },
        { kIv2HleKernels_jphoenix, kIv2HleKernels_jphoenix_count },
    };
    for (const auto& t : tabs)
        for (int i = 0; i < t.n; ++i) {
            const IV2_HleKernel& e = t.e[i];
            if (e.hash == hash && (e.entryPC == pc || e.entryPC == 0)   // entryPC==0 = hash-only wildcard
                && (s_only < 0 || (u32)s_only == pc)) return &e;
        }
    return nullptr;
}
