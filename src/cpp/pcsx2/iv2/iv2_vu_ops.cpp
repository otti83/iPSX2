// SPDX-License-Identifier: GPL-3.0+
// P101-2 Phase T 真実装: VU1 Upper FMAC op の NEON native 実装 (ARM64)
//
// Phase T は従来 structural wrapper (vu1Exec 丸委譲) だった。
// 本ファイルは Upper opcode の NEON 4-way 化を担う。
// 下記の semantics を interpreter (VUops.cpp) と bit-exact に合わせる:
//   1. 入力 VF lane に vuDouble() 等価の PS2 入力フィルタを適用
//        (denormal → signed zero, Inf/NaN → signed max (overflow check 時))
//   2. 実演算 (ADD/SUB/MUL/MADD/MSUB/MAX/MINI) を NEON で実行
//   3. 結果を VU_MACx_UPDATE() 等価に分類し MAC flag (per-lane Z/S/U/O 4 nibble) を書く
//   4. VU_STAT_UPDATE() 等価で STATUS flag を集約 (Z/S/U/O 各 nibble OR)
//   5. _X/_Y/_Z/_W マスクに従い VF[_Fd_] を書く。未書込 lane は MAC も 0 (CLEAR 相当)。
//
// interpreter の per-lane scalar ループ (applyBinaryMACOp) を 4-way に展開した形。

#include "PrecompiledHeader.h"
#include "Common.h"
#include "VUmicro.h"
#include "Vif.h"    // vif1Regs (XITOP/XTOP で参照)
#include "iv2/iv2_vu_ops.h"
#include "iv2/iv2_async_vu1.h"  // [overlap] g_iv2_vu1_on_worker + TOP/ITOP snapshot

#include <arm_neon.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

// [P101-15 T1] iv2_vu.cpp のグローバル (exec-plan 経路が既定であること = 変換の前提)
extern bool g_iv2_exec_plan_enabled;

// ────────────────────────────────────────────────────────────
// [P101-9 §5.7] Read/Write-set capture (read-only eligibility probe).
// During a registered hot program (g_hle_hotkick), record every VU-mem
// load/store as {pc, addr, qword value, kind}. Lets offline RE verify whether
// the program's read-set is reconstructible from the program-start snapshot
// (= eligibility for snapshot-replace HLE) and pin vertex base/stride/format.
// env iPSX2_RWSET=1, default-off, ship-safe. Consumed by iv2_vu.cpp dump.
// ────────────────────────────────────────────────────────────
// Compile gate: 0 (default) = rwset_log is an empty inline = ZERO ship cost in the hottest VU1
// memory-op path. Build a measurement binary with -DIPSX2_RWSET_PROBE=1 to enable capture.
#ifndef IPSX2_RWSET_PROBE
#define IPSX2_RWSET_PROBE 0
#endif
extern bool g_hle_hotkick;
bool g_rwset_on = []{ const char* e = std::getenv("iPSX2_RWSET"); return e && e[0] == '1'; }();
#define RWSET_N 65536u
struct IV2_RWSetEntry { u32 pc; u32 addr; u8 val[16]; u8 kind; };  // kind 0=LQ 1=ILW 2=SQ 3=ISW
IV2_RWSetEntry g_rwset[RWSET_N];
std::atomic<u32> g_rwset_seq{0};
static __fi void rwset_log(VURegs* VU, u32 addr, const void* qw_at_addr, u8 kind)
{
#if IPSX2_RWSET_PROBE
    if (__builtin_expect(!(g_rwset_on && g_hle_hotkick), 1)) return;
    const u32 i = g_rwset_seq.fetch_add(1, std::memory_order_relaxed) & (RWSET_N - 1);
    g_rwset[i].pc = VU->VI[REG_TPC].UL - 8;
    g_rwset[i].addr = addr & 0x3fff;
    std::memcpy(g_rwset[i].val, qw_at_addr, 16);
    g_rwset[i].kind = kind;
#else
    (void)VU; (void)addr; (void)qw_at_addr; (void)kind;
#endif
}

// ────────────────────────────────────────────────────────────
// env gate
// ────────────────────────────────────────────────────────────
namespace iv2 { namespace vu_ops {
bool g_iv2vu_upper_en = false;
bool g_iv2vu_lower_en = false;
bool g_iv2vu_diff_en  = false;

static bool s_initialized   = false;

// [P101-4 BISECT] Lower op group mask for bisection.
// bit 0: memory (LQ/LQI/LQD/SQ/SQI/SQD/ILW/ILWR/ISW/ISWR)
// bit 1: move   (MOVE/MR32/MFIR/MTIR)
// bit 2: alu    (IADD/ISUB/IADDI/IADDIU/ISUBIU/IAND/IOR)
// bit 3: branch (IBEQ/IBNE/IBGEZ/IBGTZ/IBLEZ/IBLTZ/B/BAL/JR)
// bit 4: rseries (RINIT/RGET/RNEXT/RXOR)
// bit 5: flag   (FSAND/FSEQ/FSOR/FCAND/FCEQ/FCOR/FCSET/FCGET)
// bit 6: misc   (WAITQ/WAITP/XITOP/XTOP)
// 0x7F = all (default when LOWER=1 & no MASK env), 0x00 = none
static u32 s_lower_mask = 0x7F;
constexpr u32 LMASK_MEM   = 0x01;
constexpr u32 LMASK_MOVE  = 0x02;
constexpr u32 LMASK_ALU   = 0x04;
constexpr u32 LMASK_BR    = 0x08;
constexpr u32 LMASK_R     = 0x10;
constexpr u32 LMASK_FLAG  = 0x20;
constexpr u32 LMASK_MISC  = 0x40;

// Diff mode 集計
static std::atomic<u64> s_diff_total{0};
static std::atomic<u64> s_diff_mismatch{0};
static std::atomic<u32> s_diff_log_quota{0};

// Dispatch 統計
static std::atomic<u64> s_dispatch_called{0};
static std::atomic<u64> s_dispatch_handled{0};
static std::atomic<u64> s_dispatch_fallback{0};

// Lower dispatch 統計
static std::atomic<u64> s_lower_called{0};
static std::atomic<u64> s_lower_handled{0};
static std::atomic<u64> s_lower_fallback{0};

// [P101-5 Phase T Lower] Fallback profiling: packed (op7, op6, sub) → count.
// Key = (op7 << 11) | (op6 << 5) | sub. op7 is 7 bits, op6 is 6 bits, sub 5 bits.
// But only specific combinations matter: for op7 != 0x40, op6/sub irrelevant → encode as op7 only.
// Array size: 8192 atomics (32 KB) — covers all keys.
static std::atomic<u32> s_lower_fb_hist[8192];
static std::atomic<u64> s_lower_fb_total{0};
static bool s_lower_fb_prof_enabled = false;

static __fi void lower_fb_record(u32 op7, u32 op6, u32 sub)
{
    if (!s_lower_fb_prof_enabled) return;
    u32 key = (op7 << 11) | (op6 << 5) | sub;
    s_lower_fb_hist[key & 0x1FFF].fetch_add(1, std::memory_order_relaxed);
    s_lower_fb_total.fetch_add(1, std::memory_order_relaxed);
}

// Called periodically from outside to emit top-N fallback keys.
static void lower_fb_log_top(int top_n)
{
    if (!s_lower_fb_prof_enabled) return;
    struct Entry { u32 key; u32 count; };
    Entry top[32];
    int n = 0;
    for (int i = 0; i < 8192; i++) {
        u32 c = s_lower_fb_hist[i].load(std::memory_order_relaxed);
        if (c == 0) continue;
        if (n < top_n) {
            top[n++] = {(u32)i, c};
        } else {
            int worst = 0;
            for (int j = 1; j < n; j++) if (top[j].count < top[worst].count) worst = j;
            if (c > top[worst].count) top[worst] = {(u32)i, c};
        }
    }
    // sort desc
    for (int i = 0; i < n; i++)
        for (int j = i+1; j < n; j++)
            if (top[j].count > top[i].count) { auto t = top[i]; top[i] = top[j]; top[j] = t; }
    u64 tot = s_lower_fb_total.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        u32 key = top[i].key;
        u32 op7 = (key >> 11) & 0x7F;
        u32 op6 = (key >> 5) & 0x3F;
        u32 sub = key & 0x1F;
        double pct = tot ? (100.0 * top[i].count / tot) : 0.0;
        Console.WriteLn("@@VU_IV2_LFB@@ rank=%d op7=%02x op6=%02x sub=%02x count=%u pct=%.2f%% (tot=%llu)",
            i, op7, op6, sub, top[i].count, pct, (unsigned long long)tot);
    }
}

void Init()
{
    if (s_initialized)
        return;
    s_initialized = true;
    // iPSX2_VU_IV2_OPS=1 の shortcut (UPPER+LOWER 両方 enable)
    const bool ops_all = iPSX2_GetRuntimeEnvBool("iPSX2_VU_IV2_OPS", false);
    // [port 2026-05-30] Default ON: Init() only runs when iv2 is the selected VU1 provider
    // (CpuIv2VU1::Reset), so if we're here the user wants the NEON path. Previously these
    // defaulted false, which silently fell back to stock vu1Exec when iv2 was selected via
    // INI (UseIv2VU1) without the dev env — i.e. INI-selected iv2 never used NEON. Env can
    // still force them off (iPSX2_VU_IV2_OPS_UPPER=0) for A/B measurement.
    g_iv2vu_upper_en  = ops_all || iPSX2_GetRuntimeEnvBool("iPSX2_VU_IV2_OPS_UPPER", true);
    g_iv2vu_lower_en  = ops_all || iPSX2_GetRuntimeEnvBool("iPSX2_VU_IV2_OPS_LOWER", true);
    g_iv2vu_diff_en   = iPSX2_GetRuntimeEnvBool("iPSX2_VU_IV2_DIFF", false);
    // LOWER_MASK: 16進 bitmask、未指定時は 0x7F (全有効)
    const char* mask_env = iPSX2_GetRuntimeEnv("iPSX2_VU_IV2_OPS_LOWER_MASK");
    if (mask_env) s_lower_mask = (u32)std::strtoul(mask_env, nullptr, 0);
    s_diff_log_quota.store(5000, std::memory_order_relaxed);
    s_lower_fb_prof_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_VU_IV2_LFB_PROF", false);
    Console.WriteLn("@@VU_IV2_OPS@@ UpperEnabled=%d LowerEnabled=%d LowerMask=0x%02x DiffEnabled=%d LFBProf=%d",
        g_iv2vu_upper_en ? 1 : 0, g_iv2vu_lower_en ? 1 : 0, s_lower_mask, g_iv2vu_diff_en ? 1 : 0,
        s_lower_fb_prof_enabled ? 1 : 0);
}

// Public: flush top-N fallback stats from outside (e.g. per-frame log hook).
void LowerFallbackLogTop(int n) { lower_fb_log_top(n); }

bool UpperEnabled() { return g_iv2vu_upper_en; }
bool LowerEnabled() { return g_iv2vu_lower_en; }

}} // namespace iv2::vu_ops

// ────────────────────────────────────────────────────────────
// 内部ヘルパ
// ────────────────────────────────────────────────────────────
namespace {

// VU->code field helpers (VUops.cpp と同一)
static __fi u32 code_Ft(u32 code)   { return (code >> 16) & 0x1F; }
static __fi u32 code_Fs(u32 code)   { return (code >> 11) & 0x1F; }
static __fi u32 code_Fd(u32 code)   { return (code >>  6) & 0x1F; }
static __fi u32 code_XYZW(u32 code) { return (code >> 21) & 0xF; }
static __fi u32 code_Fsf(u32 code)  { return (code >> 21) & 0x03; }
static __fi u32 code_Ftf(u32 code)  { return (code >> 23) & 0x03; }

// MAC flag を VU->macflag に書く (interpreter と同一フォーマット)。
//   macflag = [O:4bit][U:4bit][S:4bit][Z:4bit]
//   各 nibble 内: x=bit3, y=bit2, z=bit1, w=bit0
// result_v: post-op 結果 (u32 x4)。
// write_mask: 書き込み有効 lane (0xFFFFFFFF or 0)。
// overflow_check: VU1 overflow flag (EmuConfig.Cpu.Recompiler.vu1Overflow)。
// out_result: post-clamp 書き込み値 (denormal→signed zero, Inf/NaN→sign|max)。
static __fi void update_mac_and_clamp(
    uint32x4_t result_v,
    uint32x4_t write_mask,
    bool overflow_check,
    VURegs* VU,
    uint32x4_t* out_result)
{
    const uint32x4_t c_sign_bit = vdupq_n_u32(0x80000000);
    const uint32x4_t c_exp_mask = vdupq_n_u32(0xFF);
    const uint32x4_t c_mant_mask = vdupq_n_u32(0x007FFFFF);
    const uint32x4_t c_exp_ff = vdupq_n_u32(0xFF);
    const uint32x4_t c_max_val = vdupq_n_u32(0x7F7FFFFF);
    const uint32x4_t c_one = vdupq_n_u32(1);

    uint32x4_t sign = vandq_u32(result_v, c_sign_bit);
    uint32x4_t exp  = vandq_u32(vshrq_n_u32(result_v, 23), c_exp_mask);

    uint32x4_t exp_is_zero  = vceqzq_u32(exp);
    uint32x4_t exp_is_ff    = vceqq_u32(exp, c_exp_ff);

    uint32x4_t mant = vandq_u32(result_v, c_mant_mask);
    uint32x4_t mant_is_zero = vceqzq_u32(mant);
    uint32x4_t mant_nonzero = vmvnq_u32(mant_is_zero);

    uint32x4_t is_denorm = vandq_u32(exp_is_zero, mant_nonzero);       // case 2
    uint32x4_t is_zero   = vandq_u32(exp_is_zero, mant_is_zero);       // case 1 (+-0)
    uint32x4_t is_inf    = exp_is_ff;                                   // case 3

    // 結果値補正:
    //   denormal → signed zero (sign only)
    //   Inf/NaN (overflow check on) → sign | 0x7F7FFFFF
    uint32x4_t ret = vbslq_u32(is_denorm, sign, result_v);
    if (overflow_check)
    {
        uint32x4_t sat = vorrq_u32(sign, c_max_val);
        ret = vbslq_u32(is_inf, sat, ret);
    }

    // 書込マスク未設定 lane は書き込まない (old VF を保持)
    // ここでは呼び出し側で final blend するため、ret はそのまま返す。
    *out_result = ret;

    // ── MAC flag ───────────────────────────────────────────
    // per-lane: S = sign bit, Z = (is_zero || is_denorm), U = is_denorm, O = is_inf
    uint32x4_t s_bit = vshrq_n_u32(sign, 31);                   // 0 or 1
    uint32x4_t z_bit = vandq_u32(vorrq_u32(is_zero, is_denorm), c_one);
    uint32x4_t u_bit = vandq_u32(is_denorm, c_one);
    uint32x4_t o_bit = vandq_u32(is_inf, c_one);

    // per_lane[i] = (o<<12) | (u<<8) | (s<<4) | z
    uint32x4_t per_lane = vorrq_u32(
        vorrq_u32(vshlq_n_u32(o_bit, 12), vshlq_n_u32(u_bit, 8)),
        vorrq_u32(vshlq_n_u32(s_bit, 4),  z_bit));

    // write_mask=0 の lane は CLEAR 相当 (全 bit 0)
    per_lane = vandq_u32(per_lane, write_mask);

    // lane ごとに異なる shift: x<<3, y<<2, z<<1, w<<0
    static const int32_t shifts_arr[4] = { 3, 2, 1, 0 };
    int32x4_t shifts = vld1q_s32(shifts_arr);
    uint32x4_t shifted = vshlq_u32(per_lane, shifts);

    // 4 lane 全て disjoint bit なので、OR==ADD
    u32 macflag = vaddvq_u32(shifted);
    VU->macflag = macflag;

    // ── STATUS flag 集約 (VU_STAT_UPDATE 等価) ─────────────
    u32 newflag = 0;
    if (macflag & 0x000F) newflag = 0x1;
    if (macflag & 0x00F0) newflag |= 0x2;
    if (macflag & 0x0F00) newflag |= 0x4;
    if (macflag & 0xF000) newflag |= 0x8;
    VU->statusflag = newflag;
}

// vuDouble 相当の per-lane 入力フィルタ (applyBinaryMACOp が直接呼ぶ Fn の内部で行う処理)。
// denormal → signed zero, Inf/NaN → sign | 0x7F7FFFFF (overflow check 時)。
static __fi uint32x4_t input_filter_vuDouble(uint32x4_t v, bool overflow_check)
{
    const uint32x4_t c_sign_bit = vdupq_n_u32(0x80000000);
    const uint32x4_t c_exp_mask = vdupq_n_u32(0xFF);
    const uint32x4_t c_mant_mask = vdupq_n_u32(0x007FFFFF);
    const uint32x4_t c_exp_ff = vdupq_n_u32(0xFF);
    const uint32x4_t c_max_val = vdupq_n_u32(0x7F7FFFFF);

    uint32x4_t sign = vandq_u32(v, c_sign_bit);
    uint32x4_t exp  = vandq_u32(vshrq_n_u32(v, 23), c_exp_mask);
    uint32x4_t mant = vandq_u32(v, c_mant_mask);

    uint32x4_t exp_is_zero  = vceqzq_u32(exp);
    uint32x4_t exp_is_ff    = vceqq_u32(exp, c_exp_ff);

    // denormal → signed zero
    uint32x4_t ret = vbslq_u32(exp_is_zero, sign, v);
    if (overflow_check)
    {
        uint32x4_t sat = vorrq_u32(sign, c_max_val);
        ret = vbslq_u32(exp_is_ff, sat, ret);
    }
    return ret;
}

// write mask 4-lane u32 vector 生成 (_X/_Y/_Z/_W bits)
// [2026-08-06 P101-14] 16-entry constant table instead of building the mask on
// the stack. The old form (4 tests -> 4 selects -> 4 stores -> vector load) had
// to materialise the array in the frame and then load it back, so every masked
// FMAC paid a store-to-load round trip. `xyzw` is the 4-bit destination field of
// the opcode (`_XYZW_` = (code >> 21) & 0xF) so the index is provably 0..15; the
// explicit `& 0xF` keeps that true even if a future caller passes wider bits.
// Values are identical to the old expression, so this is bit-exact by
// construction.
// bits: X=bit3, Y=bit2, Z=bit1, W=bit0
alignas(16) static const uint32_t s_write_mask_tab[16][4] = {
    { 0u,          0u,          0u,          0u          }, // 0000
    { 0u,          0u,          0u,          0xFFFFFFFFu }, // 000W
    { 0u,          0u,          0xFFFFFFFFu, 0u          }, // 00Z0
    { 0u,          0u,          0xFFFFFFFFu, 0xFFFFFFFFu }, // 00ZW
    { 0u,          0xFFFFFFFFu, 0u,          0u          }, // 0Y00
    { 0u,          0xFFFFFFFFu, 0u,          0xFFFFFFFFu }, // 0Y0W
    { 0u,          0xFFFFFFFFu, 0xFFFFFFFFu, 0u          }, // 0YZ0
    { 0u,          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu }, // 0YZW
    { 0xFFFFFFFFu, 0u,          0u,          0u          }, // X000
    { 0xFFFFFFFFu, 0u,          0u,          0xFFFFFFFFu }, // X00W
    { 0xFFFFFFFFu, 0u,          0xFFFFFFFFu, 0u          }, // X0Z0
    { 0xFFFFFFFFu, 0u,          0xFFFFFFFFu, 0xFFFFFFFFu }, // X0ZW
    { 0xFFFFFFFFu, 0xFFFFFFFFu, 0u,          0u          }, // XY00
    { 0xFFFFFFFFu, 0xFFFFFFFFu, 0u,          0xFFFFFFFFu }, // XY0W
    { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u          }, // XYZ0
    { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu }, // XYZW
};

static __fi uint32x4_t make_write_mask(u32 xyzw)
{
    return vld1q_u32(s_write_mask_tab[xyzw & 0xF]);
}

// 書き込み先 VECTOR* を取得。_Fd_==0 はダミー (interpreter の RDzero 相当)。
alignas(16) static VECTOR s_rd_zero;

static __fi VECTOR* get_dst_fd(VURegs* VU, u32 fd_idx)
{
    if (fd_idx == 0)
        return &s_rd_zero;
    return &VU->VF[fd_idx];
}

// 演算 op 種別
enum class FmOp { ADD, SUB, MUL, MADD, MSUB, MAX, MINI };

// Ft source 種別
enum class FtSrc { Direct, BcX, BcY, BcZ, BcW, RegI, RegQ };

// Dst 種別 — Fd (VF[_Fd_]) or Acc (ACC)
enum class FmDst { Fd, Acc };

// 本体: 4-way FMAC。
// 互換: fmac_fd<Op, FSrc> = fmac_gen<Op, FSrc, FmDst::Fd>
template <FmOp Op, FtSrc FSrc, FmDst Dst = FmDst::Fd>
static __fi void fmac_gen(VURegs* VU);

template <FmOp Op, FtSrc FSrc>
static __fi void fmac_fd(VURegs* VU)
{
    fmac_gen<Op, FSrc, FmDst::Fd>(VU);
}

template <FmOp Op, FtSrc FSrc>
static __fi void fmac_acc(VURegs* VU)
{
    fmac_gen<Op, FSrc, FmDst::Acc>(VU);
}

template <FmOp Op, FtSrc FSrc, FmDst Dst>
static __fi void fmac_gen(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 fs   = code_Fs(code);
    const u32 ft   = code_Ft(code);
    const u32 fd   = code_Fd(code);
    const u32 xyzw = code_XYZW(code);

    const bool overflow_check = CHECK_VU_OVERFLOW(VU == &VU1 ? 1 : 0);

    // ACC-dest は MAX/MINI 非対応 (PS2 ISA にも存在しない)
    static_assert(!(Dst == FmDst::Acc && Op == FmOp::MAX), "MAXA not supported");
    static_assert(!(Dst == FmDst::Acc && Op == FmOp::MINI), "MINIA not supported");

    // ─── MAX/MINI は interpreter も vuDouble を通さない (applyMinMax は raw u32 比較)。
    //     _Fd_==0 なら interpreter は早期 return するのでここでも no-op。
    //     input_filter を掛けないので早期 path 化。
    if constexpr (Op == FmOp::MAX || Op == FmOp::MINI)
    {
        if (fd == 0)
            return;
        uint32x4_t fs_u = vld1q_u32(&VU->VF[fs].UL[0]);
        uint32x4_t ft_u;
        if constexpr (FSrc == FtSrc::Direct)
            ft_u = vld1q_u32(&VU->VF[ft].UL[0]);
        else if constexpr (FSrc == FtSrc::BcX)
            ft_u = vdupq_n_u32(VU->VF[ft].UL[0]);
        else if constexpr (FSrc == FtSrc::BcY)
            ft_u = vdupq_n_u32(VU->VF[ft].UL[1]);
        else if constexpr (FSrc == FtSrc::BcZ)
            ft_u = vdupq_n_u32(VU->VF[ft].UL[2]);
        else if constexpr (FSrc == FtSrc::BcW)
            ft_u = vdupq_n_u32(VU->VF[ft].UL[3]);
        else if constexpr (FSrc == FtSrc::RegI)
            ft_u = vdupq_n_u32(VU->VI[REG_I].UL);
        else
            ft_u = vdupq_n_u32(VU->VI[REG_Q].UL);

        // fp_max/fp_min: 両負なら s32 min/max を反転 (負の float は絶対値が小さい方が大)。
        // 両非負 or 混合: s32 max/min (IEEE 正 float ≒ 非負 s32、負 float は負 s32 より常に大)。
        const uint32x4_t one = vdupq_n_u32(1);
        uint32x4_t both_neg = vandq_u32(
            vandq_u32(vshrq_n_u32(fs_u, 31), one),
            vandq_u32(vshrq_n_u32(ft_u, 31), one));
        uint32x4_t both_neg_mask = vceqq_u32(both_neg, one);

        int32x4_t fs_s = vreinterpretq_s32_u32(fs_u);
        int32x4_t ft_s = vreinterpretq_s32_u32(ft_u);
        uint32x4_t cmp_gt = vcgtq_s32(fs_s, ft_s);
        uint32x4_t cmp_lt = vcltq_s32(fs_s, ft_s);
        uint32x4_t pick_fs;
        if constexpr (Op == FmOp::MAX)
        {
            // 非両負: fs>ft で fs / 両負: fs<ft で fs
            pick_fs = vbslq_u32(both_neg_mask, cmp_lt, cmp_gt);
        }
        else
        {
            // MINI: 非両負: fs<ft で fs / 両負: fs>ft で fs
            pick_fs = vbslq_u32(both_neg_mask, cmp_gt, cmp_lt);
        }
        uint32x4_t res_u = vbslq_u32(pick_fs, fs_u, ft_u);

        uint32x4_t write_mask = make_write_mask(xyzw);
        uint32x4_t old_dst = vld1q_u32(&VU->VF[fd].UL[0]);
        uint32x4_t new_dst = vbslq_u32(write_mask, res_u, old_dst);
        vst1q_u32(&VU->VF[fd].UL[0], new_dst);
        return;
    }

    // Fs vector (4-way) を読込 → vuDouble フィルタ
    uint32x4_t fs_v = vld1q_u32(&VU->VF[fs].UL[0]);
    fs_v = input_filter_vuDouble(fs_v, overflow_check);

    // Ft vector (4-way) を source 種別に応じて作る
    uint32x4_t ft_v;
    if constexpr (FSrc == FtSrc::Direct)
    {
        ft_v = vld1q_u32(&VU->VF[ft].UL[0]);
    }
    else if constexpr (FSrc == FtSrc::BcX)
    {
        ft_v = vdupq_n_u32(VU->VF[ft].UL[0]);
    }
    else if constexpr (FSrc == FtSrc::BcY)
    {
        ft_v = vdupq_n_u32(VU->VF[ft].UL[1]);
    }
    else if constexpr (FSrc == FtSrc::BcZ)
    {
        ft_v = vdupq_n_u32(VU->VF[ft].UL[2]);
    }
    else if constexpr (FSrc == FtSrc::BcW)
    {
        ft_v = vdupq_n_u32(VU->VF[ft].UL[3]);
    }
    else if constexpr (FSrc == FtSrc::RegI)
    {
        ft_v = vdupq_n_u32(VU->VI[REG_I].UL);
    }
    else // RegQ
    {
        ft_v = vdupq_n_u32(VU->VI[REG_Q].UL);
    }
    ft_v = input_filter_vuDouble(ft_v, overflow_check);

    // 実演算 (float 4-way)
    float32x4_t fs_f = vreinterpretq_f32_u32(fs_v);
    float32x4_t ft_f = vreinterpretq_f32_u32(ft_v);
    float32x4_t res_f;
    if constexpr (Op == FmOp::ADD)
    {
        res_f = vaddq_f32(fs_f, ft_f);
    }
    else if constexpr (Op == FmOp::SUB)
    {
        res_f = vsubq_f32(fs_f, ft_f);
    }
    else if constexpr (Op == FmOp::MUL)
    {
        res_f = vmulq_f32(fs_f, ft_f);
    }
    else if constexpr (Op == FmOp::MADD)
    {
        // ACC + Fs*Ft  (ACC も vuDouble 入力フィルタ適用)
        uint32x4_t acc_v = vld1q_u32(&VU->ACC.UL[0]);
        acc_v = input_filter_vuDouble(acc_v, overflow_check);
        float32x4_t acc_f = vreinterpretq_f32_u32(acc_v);
        // interp: _vuOpMADD(acc,fs,ft) = vuDouble(acc) + vuDouble(fs)*vuDouble(ft)
        float32x4_t mul_f = vmulq_f32(fs_f, ft_f);
        res_f = vaddq_f32(acc_f, mul_f);
    }
    else if constexpr (Op == FmOp::MSUB)
    {
        uint32x4_t acc_v = vld1q_u32(&VU->ACC.UL[0]);
        acc_v = input_filter_vuDouble(acc_v, overflow_check);
        float32x4_t acc_f = vreinterpretq_f32_u32(acc_v);
        float32x4_t mul_f = vmulq_f32(fs_f, ft_f);
        res_f = vsubq_f32(acc_f, mul_f);
    }
    // MAX/MINI は上部で早期 return 済。ここには来ない。

    // ── MAC/Clamp (ADD/SUB/MUL/MADD/MSUB) ──────────────────
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t res_u = vreinterpretq_u32_f32(res_f);
    uint32x4_t clamped;
    update_mac_and_clamp(res_u, write_mask, overflow_check, VU, &clamped);

    // Dst による書込先選択: Fd = VF[_Fd_] (fd==0 は dummy), Acc = &VU->ACC
    VECTOR* dst_ptr;
    if constexpr (Dst == FmDst::Acc)
        dst_ptr = &VU->ACC;
    else
        dst_ptr = get_dst_fd(VU, fd);

    uint32x4_t old_dst = vld1q_u32(&dst_ptr->UL[0]);
    uint32x4_t new_dst = vbslq_u32(write_mask, clamped, old_dst);
    vst1q_u32(&dst_ptr->UL[0], new_dst);
}

// ABS: 符号 bit を落とすだけ。MAC/STATUS 非更新。Ft 宛 (interpreter の applyUnaryFunction と同じ)。
static __fi void op_abs(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 ft_idx = code_Ft(code);
    const u32 fs_idx = code_Fs(code);
    const u32 xyzw   = code_XYZW(code);
    if (ft_idx == 0)
        return;

    const uint32x4_t c_mask = vdupq_n_u32(0x7FFFFFFF);
    uint32x4_t fs_v = vld1q_u32(&VU->VF[fs_idx].UL[0]);
    uint32x4_t res  = vandq_u32(fs_v, c_mask);

    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t old_dst = vld1q_u32(&VU->VF[ft_idx].UL[0]);
    uint32x4_t new_dst = vbslq_u32(write_mask, res, old_dst);
    vst1q_u32(&VU->VF[ft_idx].UL[0], new_dst);
}

// ─── [P101-3 Phase N-残] UPPER sub-op native 拡張 ─────────────
// profile で VU1 fallback 51% の主要因: FTOI/ITOF/CLIP/OPMULA/OPMSUB
// いずれも VU_MACx/y/z_UPDATE + VU_STAT_UPDATE でのみ状態書換えるので native 呼出で OK。

// PS2 scalar vuDouble (VUops.cpp の static コピー)。denormal → signed zero。
// Inf/NaN は CHECK_VU_OVERFLOW 時のみ clamp、それ以外 raw bits。
static __fi float scalar_vuDouble(u32 f)
{
#ifndef NO_VU_OVERFLOW
    const u32 exp = f & 0x7f800000;
    if (exp == 0) {
        f &= 0x80000000;  // denormal → signed zero
    } else if (exp == 0x7f800000 && CHECK_VU_OVERFLOW(0)) {
        f = (f & 0x80000000) | 0x7f7fffff;
    }
#endif
    return std::bit_cast<float>(f);
}

// FTOI<N>: VF[Ft] = floatToInt<N>(VF[Fs]) per xyzw lane.
// interpreter の applyUnaryFunction + floatToInt<Offset> を per-lane scalar で再現。
template<u32 N>
static __fi void op_ftoi_n(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 ft_idx = code_Ft(code);
    const u32 fs_idx = code_Fs(code);
    const u32 xyzw   = code_XYZW(code);
    if (ft_idx == 0) return;

    u32 fs[4]; std::memcpy(fs, &VU->VF[fs_idx].UL[0], 16);
    u32 res[4];
    for (int i = 0; i < 4; i++) {
        float fv = std::bit_cast<float>(fs[i]);
        if constexpr (N != 0)
            fv *= std::bit_cast<float>(0x3f800000 + (N << 23));
        const u32 bits = std::bit_cast<u32>(fv);
        if ((bits & 0x7f800000) >= 0x4f000000)
            res[i] = (bits & 0x80000000) ? 0x80000000u : 0x7fffffffu;
        else
            res[i] = static_cast<u32>(static_cast<s32>(fv));
    }
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t old_dst = vld1q_u32(&VU->VF[ft_idx].UL[0]);
    uint32x4_t new_v   = vld1q_u32(res);
    uint32x4_t new_dst = vbslq_u32(write_mask, new_v, old_dst);
    vst1q_u32(&VU->VF[ft_idx].UL[0], new_dst);
}

// ITOF<N>: VF[Ft] = (float)(s32)VF[Fs] × 2^-N per lane (applyUnaryFunction + intToFloat)。
template<u32 N>
static __fi void op_itof_n(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 ft_idx = code_Ft(code);
    const u32 fs_idx = code_Fs(code);
    const u32 xyzw   = code_XYZW(code);
    if (ft_idx == 0) return;

    int32x4_t   fs_s = vreinterpretq_s32_u32(vld1q_u32(&VU->VF[fs_idx].UL[0]));
    float32x4_t fv   = vcvtq_f32_s32(fs_s);
    if constexpr (N != 0) {
        const float32x4_t scale = vdupq_n_f32(std::bit_cast<float>(0x3f800000 - (N << 23)));
        fv = vmulq_f32(fv, scale);
    }
    uint32x4_t res = vreinterpretq_u32_f32(fv);

    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t old_dst = vld1q_u32(&VU->VF[ft_idx].UL[0]);
    uint32x4_t new_dst = vbslq_u32(write_mask, res, old_dst);
    vst1q_u32(&VU->VF[ft_idx].UL[0], new_dst);
}

// CLIP: Fs.x/y/z を |Ft.w| と比較し clipflag (24-bit 回転, 6bit per op) を更新。
// 書先: VU->clipflag + VU->VI[REG_CLIP_FLAG].UL (interp と同一 semantics)。
static __fi void op_clip(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 fs_idx = code_Fs(code);
    const u32 ft_idx = code_Ft(code);

    s32 value = static_cast<s32>(VU->VF[ft_idx].UL[3]);
    value = (value & 0x7f800000) ? (value & 0x7fffffff) : 0x007fffff;
    const u32 sx = VU->VF[fs_idx].UL[0];
    const u32 sy = VU->VF[fs_idx].UL[1];
    const u32 sz = VU->VF[fs_idx].UL[2];

    VU->clipflag <<= 6;
    if (static_cast<s32>(sx ^ 0x00000000u) > value) VU->clipflag |= 0x01;
    if (static_cast<s32>(sx ^ 0x80000000u) > value) VU->clipflag |= 0x02;
    if (static_cast<s32>(sy ^ 0x00000000u) > value) VU->clipflag |= 0x04;
    if (static_cast<s32>(sy ^ 0x80000000u) > value) VU->clipflag |= 0x08;
    if (static_cast<s32>(sz ^ 0x00000000u) > value) VU->clipflag |= 0x10;
    if (static_cast<s32>(sz ^ 0x80000000u) > value) VU->clipflag |= 0x20;
    VU->clipflag &= 0xFFFFFF;
    // [port 2026-05-30 bug fix] Do NOT write VI[REG_CLIP_FLAG] here. Stock _vuCLIP
    // (VUops.cpp:920-936) only sets VU->clipflag; VI[REG_CLIP_FLAG] is written with
    // FMAC pipe latency via _vuAddFMACStalls -> _vuFlushAll. An immediate write here
    // makes the clip flag visible too early (premature divergence the Upper diff
    // harness cannot catch, since it does not compare clipflag).
}

// OPMULA: ACC.x = VU_MACx(Fs.y × Ft.z), ACC.y = VU_MACy(Fs.z × Ft.x), ACC.z = VU_MACz(Fs.x × Ft.y)
// w component は未変更 (interp 同様)。VU_STAT_UPDATE で STATUS flag 集約。
static __fi void op_opmula(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 fs_idx = code_Fs(code);
    const u32 ft_idx = code_Ft(code);
    const float fsx = scalar_vuDouble(VU->VF[fs_idx].UL[0]);
    const float fsy = scalar_vuDouble(VU->VF[fs_idx].UL[1]);
    const float fsz = scalar_vuDouble(VU->VF[fs_idx].UL[2]);
    const float ftx = scalar_vuDouble(VU->VF[ft_idx].UL[0]);
    const float fty = scalar_vuDouble(VU->VF[ft_idx].UL[1]);
    const float ftz = scalar_vuDouble(VU->VF[ft_idx].UL[2]);

    VU->ACC.UL[0] = VU_MACx_UPDATE(VU, fsy * ftz);
    VU->ACC.UL[1] = VU_MACy_UPDATE(VU, fsz * ftx);
    VU->ACC.UL[2] = VU_MACz_UPDATE(VU, fsx * fty);
    VU_STAT_UPDATE(VU);
}

// OPMSUB: dst.x = VU_MACx(ACC.x - Fs.y × Ft.z), dst.y = ..., dst.z = ...
// dst は Fd (fd=0 なら RDzero 相当 — MAC 更新は走るが書込先破棄)。
static __fi void op_opmsub(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 fs_idx = code_Fs(code);
    const u32 ft_idx = code_Ft(code);
    const u32 fd_idx = code_Fd(code);
    const float fsx = scalar_vuDouble(VU->VF[fs_idx].UL[0]);
    const float fsy = scalar_vuDouble(VU->VF[fs_idx].UL[1]);
    const float fsz = scalar_vuDouble(VU->VF[fs_idx].UL[2]);
    const float ftx = scalar_vuDouble(VU->VF[ft_idx].UL[0]);
    const float fty = scalar_vuDouble(VU->VF[ft_idx].UL[1]);
    const float ftz = scalar_vuDouble(VU->VF[ft_idx].UL[2]);
    const float ax  = scalar_vuDouble(VU->ACC.UL[0]);
    const float ay  = scalar_vuDouble(VU->ACC.UL[1]);
    const float az  = scalar_vuDouble(VU->ACC.UL[2]);

    const u32 rx = VU_MACx_UPDATE(VU, ax - fsy * ftz);
    const u32 ry = VU_MACy_UPDATE(VU, ay - fsz * ftx);
    const u32 rz = VU_MACz_UPDATE(VU, az - fsx * fty);
    VU_STAT_UPDATE(VU);

    if (fd_idx != 0) {
        VU->VF[fd_idx].UL[0] = rx;
        VU->VF[fd_idx].UL[1] = ry;
        VU->VF[fd_idx].UL[2] = rz;
        // w は非更新 (interp 同様)
    }
}

} // anonymous namespace

// ────────────────────────────────────────────────────────────
// Public dispatch
// ────────────────────────────────────────────────────────────
// VU1_UPPER_OPCODE / VU1_LOWER_OPCODE 宣言は VUops.h で extern。
// ここから interpreter fallback の参照のみ必要。
#include "VUops.h"

// _vuBackupVI / _branchAddr のため。_vuBackupVI は extern in VUops.h。
extern s32 _branchAddr(VURegs* VU);

namespace { // Lower helpers in anonymous namespace

// ─────── VU->code field (Lower 用拡張) ────────
static __fi u32 code_Is(u32 code) { return (code_Fs(code) & 0xF); }
static __fi u32 code_It(u32 code) { return (code_Ft(code) & 0xF); }
static __fi u32 code_Id(u32 code) { return (code_Fd(code) & 0xF); }

static __fi s32 code_Imm11(u32 code)
{
    // _Imm11_ マクロ: 11-bit 符号拡張
    return (s32)((code & 0x400) ? 0xfffffc00 | (code & 0x3ff) : code & 0x3ff);
}
static __fi s32 code_SImm(u32 code)
{
    // LQ/SQ 等の 11-bit signed 変位 (VI[...] + imm) * 16 だが、interpreter は
    // s16 に sign extend してから int 加算しているので 11-bit sign extend で十分。
    return code_Imm11(code);
}

// VU1 memory pointer (16KB)
static __fi u8* vu1_mem_ptr(VURegs* VU, u32 addr)
{
    return VU->Mem + (addr & 0x3fff);
}

// VIBackup (interpreter の _vuBackupVI 相当、VIBACKUP が定義されている前提)
static __fi void vi_backup(VURegs* VU, u32 reg)
{
    if (VU->VIBackupCycles && reg == VU->VIRegNumber) {
        VU->VIBackupCycles = 2;
        return;
    }
    VU->VIBackupCycles = 2;
    VU->VIRegNumber = reg;
    VU->VIOldValue = VU->VI[reg].US[0];
}

// Branch 設定 (interpreter _setBranch 相当)
static __fi void set_branch(VURegs* VU, u32 bpc)
{
    if (VU->branch == 1) {
        VU->delaybranchpc = bpc;
        VU->takedelaybranch = true;
    } else {
        VU->branch = 2;
        VU->branchpc = bpc;
    }
}

// VIBackup を考慮した VI 読取 (branch 命令内で古い値を使う必要がある時)
static __fi s16 vi_read_with_backup(VURegs* VU, u32 reg)
{
    s16 v = VU->VI[reg].SS[0];
    if (VU->VIBackupCycles > 0 && VU->VIRegNumber == reg)
        v = (s16)VU->VIOldValue;
    return v;
}

// ─────── Lower native ops ────────

// LQ : VF[ft] ← Mem[VI[is] + imm]
static __fi void lower_lq(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 ft = code_Ft(code);
    if (ft == 0) return;
    const u32 is = code_Is(code);
    const s16 imm = (s16)code_Imm11(code);
    const u16 addr = (u16)((imm + VU->VI[is].SS[0]) * 16);
    uint32x4_t v = vld1q_u32((u32*)vu1_mem_ptr(VU, addr));
    rwset_log(VU, addr, vu1_mem_ptr(VU, addr), 0);
    const u32 xyzw = code_XYZW(code);
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t old_v = vld1q_u32(&VU->VF[ft].UL[0]);
    uint32x4_t new_v = vbslq_u32(write_mask, v, old_v);
    vst1q_u32(&VU->VF[ft].UL[0], new_v);
}

// LQI : VF[ft] ← Mem[VI[is] * 16], VI[is]++
static __fi void lower_lqi(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 is = code_Is(code);
    const u32 ft = code_Ft(code);
    const u32 fs = code_Fs(code);
    vi_backup(VU, is);
    if (ft) {
        const u32 addr = VU->VI[is].US[0] * 16;
        uint32x4_t v = vld1q_u32((u32*)vu1_mem_ptr(VU, addr));
        rwset_log(VU, addr, vu1_mem_ptr(VU, addr), 0);
        const u32 xyzw = code_XYZW(code);
        uint32x4_t write_mask = make_write_mask(xyzw);
        uint32x4_t old_v = vld1q_u32(&VU->VF[ft].UL[0]);
        uint32x4_t new_v = vbslq_u32(write_mask, v, old_v);
        vst1q_u32(&VU->VF[ft].UL[0], new_v);
    }
    if (fs != 0)   // interpreter は _Fs_ (not _Is_) でガード
        VU->VI[is].US[0]++;
}

// LQD : VI[is]--, VF[ft] ← Mem[VI[is] * 16]
static __fi void lower_lqd(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 is = code_Is(code);
    const u32 ft = code_Ft(code);
    vi_backup(VU, is);
    if (is != 0)
        VU->VI[is].US[0]--;
    if (ft == 0) return;
    const u32 addr = VU->VI[is].US[0] * 16;
    uint32x4_t v = vld1q_u32((u32*)vu1_mem_ptr(VU, addr));
    rwset_log(VU, addr, vu1_mem_ptr(VU, addr), 0);
    const u32 xyzw = code_XYZW(code);
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t old_v = vld1q_u32(&VU->VF[ft].UL[0]);
    uint32x4_t new_v = vbslq_u32(write_mask, v, old_v);
    vst1q_u32(&VU->VF[ft].UL[0], new_v);
}

// SQ : Mem[VI[it] + imm] ← VF[fs]
static __fi void lower_sq(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 fs = code_Fs(code);
    const u32 it = code_It(code);
    const s16 imm = (s16)code_Imm11(code);
    const u16 addr = (u16)((imm + VU->VI[it].SS[0]) * 16);
    u32* ptr = (u32*)vu1_mem_ptr(VU, addr);
    const u32 xyzw = code_XYZW(code);
    uint32x4_t src = vld1q_u32(&VU->VF[fs].UL[0]);
    uint32x4_t old_v = vld1q_u32(ptr);
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t new_v = vbslq_u32(write_mask, src, old_v);
    vst1q_u32(ptr, new_v);
    rwset_log(VU, addr, ptr, 2);   // [P101-9 §5.7] write-set (VF->mem store)
}

// SQI : Mem[VI[it] * 16] ← VF[fs], VI[it]++
static __fi void lower_sqi(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 fs = code_Fs(code);
    const u32 it = code_It(code);
    const u32 ft = code_Ft(code);
    vi_backup(VU, it);
    const u32 addr = VU->VI[it].US[0] * 16;
    u32* ptr = (u32*)vu1_mem_ptr(VU, addr);
    const u32 xyzw = code_XYZW(code);
    uint32x4_t src = vld1q_u32(&VU->VF[fs].UL[0]);
    uint32x4_t old_v = vld1q_u32(ptr);
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t new_v = vbslq_u32(write_mask, src, old_v);
    vst1q_u32(ptr, new_v);
    rwset_log(VU, addr, ptr, 2);   // [P101-9 §5.7] write-set (VF->mem store)
    if (ft != 0)
        VU->VI[it].US[0]++;
}

// SQD : VI[it]--, Mem[VI[it] * 16] ← VF[fs]
static __fi void lower_sqd(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 fs = code_Fs(code);
    const u32 it = code_It(code);
    const u32 ft = code_Ft(code);
    vi_backup(VU, it);
    if (ft != 0)
        VU->VI[it].US[0]--;
    const u32 addr = VU->VI[it].US[0] * 16;
    u32* ptr = (u32*)vu1_mem_ptr(VU, addr);
    const u32 xyzw = code_XYZW(code);
    uint32x4_t src = vld1q_u32(&VU->VF[fs].UL[0]);
    uint32x4_t old_v = vld1q_u32(ptr);
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t new_v = vbslq_u32(write_mask, src, old_v);
    vst1q_u32(ptr, new_v);
    rwset_log(VU, addr, ptr, 2);   // [P101-9 §5.7] write-set (VF->mem store)
}

// MOVE : VF[ft] ← VF[fs] (lane-masked)
static __fi void lower_move(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 ft = code_Ft(code);
    if (ft == 0) return;
    const u32 fs = code_Fs(code);
    const u32 xyzw = code_XYZW(code);
    uint32x4_t src = vld1q_u32(&VU->VF[fs].UL[0]);
    uint32x4_t old_v = vld1q_u32(&VU->VF[ft].UL[0]);
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t new_v = vbslq_u32(write_mask, src, old_v);
    vst1q_u32(&VU->VF[ft].UL[0], new_v);
}

// MR32 : VF[ft].xyzw ← VF[fs].yzwx  (rotate)
static __fi void lower_mr32(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 ft = code_Ft(code);
    if (ft == 0) return;
    const u32 fs = code_Fs(code);
    const u32 xyzw = code_XYZW(code);
    // interpreter: tx=fs.x; ft.x=fs.y; ft.y=fs.z; ft.z=fs.w; ft.w=tx (with _X/_Y/_Z/_W mask)
    const u32 tx = VU->VF[fs].UL[0];
    uint32_t src_arr[4] = {
        VU->VF[fs].UL[1], // ft.x = fs.y
        VU->VF[fs].UL[2], // ft.y = fs.z
        VU->VF[fs].UL[3], // ft.z = fs.w
        tx                // ft.w = fs.x (saved before any potential clobber)
    };
    uint32x4_t src = vld1q_u32(src_arr);
    uint32x4_t old_v = vld1q_u32(&VU->VF[ft].UL[0]);
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t new_v = vbslq_u32(write_mask, src, old_v);
    vst1q_u32(&VU->VF[ft].UL[0], new_v);
}

// MFIR : VF[ft].xyzw ← sign-extend VI[is] (per-lane)
static __fi void lower_mfir(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 ft = code_Ft(code);
    if (ft == 0) return;
    const u32 is = code_Is(code);
    const u32 xyzw = code_XYZW(code);
    const s32 v = (s32)(s16)VU->VI[is].SS[0];
    uint32x4_t src = vdupq_n_u32((u32)v);
    uint32x4_t old_v = vld1q_u32(&VU->VF[ft].UL[0]);
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t new_v = vbslq_u32(write_mask, src, old_v);
    vst1q_u32(&VU->VF[ft].UL[0], new_v);
}

// MTIR : VI[it].US[0] ← VF[fs].UL[fsf] の lower 16-bit
static __fi void lower_mtir(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 it = code_It(code);
    if (it == 0) return;
    const u32 fs = code_Fs(code);
    const u32 fsf = code_Fsf(code);
    vi_backup(VU, it);
    VU->VI[it].US[0] = *(u16*)&VU->VF[fs].F[fsf];
}

// ILW : VI[it] ← half-word at memory + imm, per lane mask (interpreter: only last lane bit written wins)
static __fi void lower_ilw(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 it = code_It(code);
    if (it == 0) return;
    const u32 is = code_Is(code);
    const s16 imm = (s16)code_Imm11(code);
    const u16 addr = (u16)((imm + VU->VI[is].SS[0]) * 16);
    const u16* ptr = (const u16*)vu1_mem_ptr(VU, addr);
    rwset_log(VU, addr, ptr, 1);
    if (code_XYZW(code) & 0x8) VU->VI[it].US[0] = ptr[0];
    if (code_XYZW(code) & 0x4) VU->VI[it].US[0] = ptr[2];
    if (code_XYZW(code) & 0x2) VU->VI[it].US[0] = ptr[4];
    if (code_XYZW(code) & 0x1) VU->VI[it].US[0] = ptr[6];
}

// ILWR
static __fi void lower_ilwr(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 it = code_It(code);
    if (it == 0) return;
    const u32 is = code_Is(code);
    const u32 addr = VU->VI[is].US[0] * 16;
    const u16* ptr = (const u16*)vu1_mem_ptr(VU, addr);
    rwset_log(VU, addr, ptr, 1);
    if (code_XYZW(code) & 0x8) VU->VI[it].US[0] = ptr[0];
    if (code_XYZW(code) & 0x4) VU->VI[it].US[0] = ptr[2];
    if (code_XYZW(code) & 0x2) VU->VI[it].US[0] = ptr[4];
    if (code_XYZW(code) & 0x1) VU->VI[it].US[0] = ptr[6];
}

// ISW : half-word store (per lane with pad 0)
static __fi void lower_isw(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 it = code_It(code);
    const u32 is = code_Is(code);
    const s16 imm = (s16)code_Imm11(code);
    const u16 addr = (u16)((imm + VU->VI[is].SS[0]) * 16);
    u16* ptr = (u16*)vu1_mem_ptr(VU, addr);
    const u16 val = VU->VI[it].US[0];
    if (code_XYZW(code) & 0x8) { ptr[0] = val; ptr[1] = 0; }
    if (code_XYZW(code) & 0x4) { ptr[2] = val; ptr[3] = 0; }
    if (code_XYZW(code) & 0x2) { ptr[4] = val; ptr[5] = 0; }
    if (code_XYZW(code) & 0x1) { ptr[6] = val; ptr[7] = 0; }
}

// ISWR
static __fi void lower_iswr(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 it = code_It(code);
    const u32 is = code_Is(code);
    const u32 addr = VU->VI[is].US[0] * 16;
    u16* ptr = (u16*)vu1_mem_ptr(VU, addr);
    const u16 val = VU->VI[it].US[0];
    if (code_XYZW(code) & 0x8) { ptr[0] = val; ptr[1] = 0; }
    if (code_XYZW(code) & 0x4) { ptr[2] = val; ptr[3] = 0; }
    if (code_XYZW(code) & 0x2) { ptr[4] = val; ptr[5] = 0; }
    if (code_XYZW(code) & 0x1) { ptr[6] = val; ptr[7] = 0; }
}

// IADD : VI[id] ← VI[is] + VI[it] (s16 add)
static __fi void lower_iadd(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 id = code_Id(code);
    if (id == 0) return;
    const u32 is = code_Is(code);
    const u32 it = code_It(code);
    vi_backup(VU, id);
    VU->VI[id].SS[0] = (s16)(VU->VI[is].SS[0] + VU->VI[it].SS[0]);
}

// ISUB
static __fi void lower_isub(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 id = code_Id(code);
    if (id == 0) return;
    const u32 is = code_Is(code);
    const u32 it = code_It(code);
    vi_backup(VU, id);
    VU->VI[id].SS[0] = (s16)(VU->VI[is].SS[0] - VU->VI[it].SS[0]);
}

// IADDI : VI[it] ← VI[is] + imm5 (sign-extended)
static __fi void lower_iaddi(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 it = code_It(code);
    if (it == 0) return;
    const u32 is = code_Is(code);
    s16 imm = (s16)((code >> 6) & 0x1F);
    imm = (s16)((imm & 0x10) ? (0xFFF0 | (imm & 0xF)) : (imm & 0xF));
    vi_backup(VU, it);
    VU->VI[it].SS[0] = (s16)(VU->VI[is].SS[0] + imm);
}

// IADDIU : VI[it] ← VI[is] + (15-bit unsigned imm)
static __fi void lower_iaddiu(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 it = code_It(code);
    if (it == 0) return;
    const u32 is = code_Is(code);
    const u16 imm = (u16)(((code >> 10) & 0x7800) | (code & 0x7ff));
    vi_backup(VU, it);
    VU->VI[it].SS[0] = (s16)(VU->VI[is].SS[0] + (s16)imm);
}

// ISUBIU
static __fi void lower_isubiu(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 it = code_It(code);
    if (it == 0) return;
    const u32 is = code_Is(code);
    const u16 imm = (u16)(((code >> 10) & 0x7800) | (code & 0x7ff));
    vi_backup(VU, it);
    VU->VI[it].SS[0] = (s16)(VU->VI[is].SS[0] - (s16)imm);
}

// IAND
static __fi void lower_iand(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 id = code_Id(code);
    if (id == 0) return;
    const u32 is = code_Is(code);
    const u32 it = code_It(code);
    vi_backup(VU, id);
    VU->VI[id].US[0] = VU->VI[is].US[0] & VU->VI[it].US[0];
}

// IOR
static __fi void lower_ior(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 id = code_Id(code);
    if (id == 0) return;
    const u32 is = code_Is(code);
    const u32 it = code_It(code);
    vi_backup(VU, id);
    VU->VI[id].US[0] = VU->VI[is].US[0] | VU->VI[it].US[0];
}

// Branches (IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ)
static __fi void lower_ibeq(VURegs* VU)
{
    const u32 code = VU->code;
    const s16 dest = vi_read_with_backup(VU, code_It(code));
    const s16 src  = vi_read_with_backup(VU, code_Is(code));
    if (dest == src) {
        s32 bpc = _branchAddr(VU);
        set_branch(VU, (u32)bpc);
    }
}
static __fi void lower_ibne(VURegs* VU)
{
    const u32 code = VU->code;
    const s16 dest = vi_read_with_backup(VU, code_It(code));
    const s16 src  = vi_read_with_backup(VU, code_Is(code));
    if (dest != src) {
        s32 bpc = _branchAddr(VU);
        set_branch(VU, (u32)bpc);
    }
}
static __fi void lower_ibltz(VURegs* VU)
{
    const u32 code = VU->code;
    const s16 src = vi_read_with_backup(VU, code_Is(code));
    if (src < 0) { set_branch(VU, (u32)_branchAddr(VU)); }
}
static __fi void lower_ibgtz(VURegs* VU)
{
    const u32 code = VU->code;
    const s16 src = vi_read_with_backup(VU, code_Is(code));
    if (src > 0) { set_branch(VU, (u32)_branchAddr(VU)); }
}
static __fi void lower_iblez(VURegs* VU)
{
    const u32 code = VU->code;
    const s16 src = vi_read_with_backup(VU, code_Is(code));
    if (src <= 0) { set_branch(VU, (u32)_branchAddr(VU)); }
}
static __fi void lower_ibgez(VURegs* VU)
{
    const u32 code = VU->code;
    const s16 src = vi_read_with_backup(VU, code_Is(code));
    if (src >= 0) { set_branch(VU, (u32)_branchAddr(VU)); }
}

// B (unconditional)
static __fi void lower_b(VURegs* VU)
{
    s32 bpc = _branchAddr(VU);
    set_branch(VU, (u32)bpc);
}

// BAL : branch and link. VI[it] ← (TPC+8)/8 (PC of instruction AFTER delay slot, in 8-byte units)
static __fi void lower_bal(VURegs* VU)
{
    s32 bpc = _branchAddr(VU);
    const u32 code = VU->code;
    const u32 it = code_It(code);
    if (it) {
        if (VU->branch == 1)
            VU->VI[it].US[0] = (VU->branchpc + 8) / 8;
        else
            VU->VI[it].US[0] = (VU->VI[REG_TPC].UL + 8) / 8;
    }
    set_branch(VU, (u32)bpc);
}

// JR : Jump via VI[is] * 8
static __fi void lower_jr(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 is = code_Is(code);
    const u32 bpc = VU->VI[is].US[0] * 8;
    set_branch(VU, bpc);
}

// WAITQ : pipeline wait, interpreter は _vuWAITQ で no-op、stall は別処理なので native も no-op。
static __fi void lower_waitq(VURegs* /*VU*/) {}

// WAITP : 同上
static __fi void lower_waitp(VURegs* /*VU*/) {}

// R-series LFSR
static __fi void lower_rinit(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 fs = code_Fs(code);
    const u32 fsf = code_Fsf(code);
    VU->VI[REG_R].UL = 0x3F800000 | (VU->VF[fs].UL[fsf] & 0x007FFFFF);
}
static __fi void lower_advance_lfsr(VURegs* VU)
{
    int x = (VU->VI[REG_R].UL >> 4) & 1;
    int y = (VU->VI[REG_R].UL >> 22) & 1;
    VU->VI[REG_R].UL <<= 1;
    VU->VI[REG_R].UL ^= x ^ y;
    VU->VI[REG_R].UL = (VU->VI[REG_R].UL & 0x7fffff) | 0x3f800000;
}
static __fi void lower_rget(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 ft = code_Ft(code);
    if (ft == 0) return;
    const u32 xyzw = code_XYZW(code);
    uint32x4_t src = vdupq_n_u32(VU->VI[REG_R].UL);
    uint32x4_t old_v = vld1q_u32(&VU->VF[ft].UL[0]);
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t new_v = vbslq_u32(write_mask, src, old_v);
    vst1q_u32(&VU->VF[ft].UL[0], new_v);
}
static __fi void lower_rnext(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 ft = code_Ft(code);
    if (ft == 0) return;
    lower_advance_lfsr(VU);
    const u32 xyzw = code_XYZW(code);
    uint32x4_t src = vdupq_n_u32(VU->VI[REG_R].UL);
    uint32x4_t old_v = vld1q_u32(&VU->VF[ft].UL[0]);
    uint32x4_t write_mask = make_write_mask(xyzw);
    uint32x4_t new_v = vbslq_u32(write_mask, src, old_v);
    vst1q_u32(&VU->VF[ft].UL[0], new_v);
}
static __fi void lower_rxor(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 fs = code_Fs(code);
    const u32 fsf = code_Fsf(code);
    VU->VI[REG_R].UL = 0x3F800000 | ((VU->VI[REG_R].UL ^ VU->VF[fs].UL[fsf]) & 0x007FFFFF);
}

// FLAG ops (status/mac/clip)
static __fi void lower_fsand(VURegs* VU)
{
    const u32 code = VU->code;
    const u16 imm = (u16)((((code >> 21) & 0x1) << 11) | (code & 0x7ff));
    const u32 it = code_It(code);
    if (it == 0) return;
    VU->VI[it].US[0] = (u16)((VU->VI[REG_STATUS_FLAG].US[0] & 0xFFF) & imm);
}
static __fi void lower_fseq(VURegs* VU)
{
    const u32 code = VU->code;
    const u16 imm = (u16)((((code >> 21) & 0x1) << 11) | (code & 0x7ff));
    const u32 it = code_It(code);
    if (it == 0) return;
    VU->VI[it].US[0] = ((VU->VI[REG_STATUS_FLAG].US[0] & 0xFFF) == imm) ? 1 : 0;
}
static __fi void lower_fsor(VURegs* VU)
{
    const u32 code = VU->code;
    const u16 imm = (u16)((((code >> 21) & 0x1) << 11) | (code & 0x7ff));
    const u32 it = code_It(code);
    if (it == 0) return;
    VU->VI[it].US[0] = (u16)((VU->VI[REG_STATUS_FLAG].US[0] & 0xFFF) | imm);
}
// FCAND/FCEQ/FCOR: 結果は常に VI[1] に書き込む (VU ISA 仕様、_It_ フィールドは無視)
static __fi void lower_fcand(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 imm = (u32)(code & 0xFFFFFF);
    VU->VI[1].US[0] = (((VU->VI[REG_CLIP_FLAG].UL & 0xFFFFFF) & imm) != 0) ? 1 : 0;
}
static __fi void lower_fceq(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 imm = (u32)(code & 0xFFFFFF);
    VU->VI[1].US[0] = ((VU->VI[REG_CLIP_FLAG].UL & 0xFFFFFF) == imm) ? 1 : 0;
}
static __fi void lower_fcor(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 imm = (u32)(code & 0xFFFFFF);
    const u32 hold = (VU->VI[REG_CLIP_FLAG].UL & 0xFFFFFF) | imm;
    VU->VI[1].US[0] = (hold == 0xFFFFFF) ? 1 : 0;
}
// FCSET: clipflag スカラーのみ更新 (interpreter と一致)。VI[REG_CLIP_FLAG] は FMAC pipe flush で反映される。
static __fi void lower_fcset(VURegs* VU)
{
    const u32 code = VU->code;
    VU->clipflag = (u32)(code & 0xFFFFFF);
}
static __fi void lower_fcget(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 it = code_It(code);
    if (it == 0) return;
    VU->VI[it].US[0] = (u16)(VU->VI[REG_CLIP_FLAG].UL & 0x0FFF);
}

// XITOP / XTOP : return vif top / itop (MTVU 対応不要、本 build では THREAD_VU1=false)
static __fi void lower_xitop(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 it = code_It(code);
    if (it == 0) return;
    // [overlap] on the async worker, read the kick-time snapshot (the EE may be
    // concurrently unpacking the next buffer, mutating live vif1Regs.itop).
    VU->VI[it].US[0] = (u16)(g_iv2_vu1_on_worker ? g_iv2_async_itop : vif1Regs.itop);
}
static __fi void lower_xtop(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 it = code_It(code);
    if (it == 0) return;
    VU->VI[it].US[0] = (u16)(g_iv2_vu1_on_worker ? g_iv2_async_top : vif1Regs.top);
}

} // anonymous namespace (Lower helpers)

namespace iv2 { namespace vu_ops {

// Lower native dispatcher. 戻り値 true = 処理完了、false = fallback 必要。
// 各 op は group mask で gate (bisection 用)。
static bool LowerDispatchNativeImpl(VURegs* VU);
bool LowerDispatchNative(VURegs* VU)
{
    bool r = LowerDispatchNativeImpl(VU);
    if (!r && s_lower_fb_prof_enabled) {
        const u32 code = VU->code;
        const u32 op7 = code >> 25;
        const u32 op6 = code & 0x3F;
        const u32 sub = (code >> 6) & 0x1F;
        lower_fb_record(op7, op6, sub);
    }
    return r;
}
// [P101-15 T1] GATED=false = group mask を定数 0x7F (= 既定) とみなす版。
// オフライン変換 (T1) は「mask が既定である」ことを entry gate で確認済みなので、
// `VU->code` が定数のとき switch と mask 判定が丸ごと畳まれて 1 op への直呼びになる。
// GATED=true は従来どおり実 mask を読む (bisection 用 env が効く)。挙動は同一。
// FOP7/FOP6/FSUB >= 0 のとき、その op key を定数として畳み込む (T1 の per-op thunk 用)。
// 命令語そのものは畳み込まない = op body は key ごとに 1 本だけ実体化され共有される。
template <bool GATED, int FOP7 = -1, int FOP6 = -1, int FSUB = -1>
static __fi bool LowerDispatchNativeImplT(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 op7 = (FOP7 >= 0) ? (u32)FOP7 : (code >> 25);
    const u32 M = GATED ? s_lower_mask : 0x7Fu;

    switch (op7) {
        case 0x00: if (M & LMASK_MEM) { lower_lq(VU);     return true; } return false;
        case 0x01: if (M & LMASK_MEM) { lower_sq(VU);     return true; } return false;
        case 0x04: if (M & LMASK_MEM) { lower_ilw(VU);    return true; } return false;
        case 0x05: if (M & LMASK_MEM) { lower_isw(VU);    return true; } return false;
        case 0x08: if (M & LMASK_ALU) { lower_iaddiu(VU); return true; } return false;
        case 0x09: if (M & LMASK_ALU) { lower_isubiu(VU); return true; } return false;
        case 0x10: if (M & LMASK_FLAG){ lower_fceq(VU);   return true; } return false;
        case 0x11: if (M & LMASK_FLAG){ lower_fcset(VU);  return true; } return false;
        case 0x12: if (M & LMASK_FLAG){ lower_fcand(VU);  return true; } return false;
        case 0x13: if (M & LMASK_FLAG){ lower_fcor(VU);   return true; } return false;
        case 0x14: if (M & LMASK_FLAG){ lower_fseq(VU);   return true; } return false;
        // case 0x15 FSSET: statusflag 書換で注意 → fallback
        case 0x16: if (M & LMASK_FLAG){ lower_fsand(VU);  return true; } return false;
        case 0x17: if (M & LMASK_FLAG){ lower_fsor(VU);   return true; } return false;
        // case 0x18 FMEQ, 0x1A FMAND, 0x1B FMOR: macflag 系、interpreter 委譲
        case 0x1C: if (M & LMASK_FLAG){ lower_fcget(VU);  return true; } return false;
        case 0x20: if (M & LMASK_BR)  { lower_b(VU);      return true; } return false;
        case 0x21: if (M & LMASK_BR)  { lower_bal(VU);    return true; } return false;
        case 0x24: if (M & LMASK_BR)  { lower_jr(VU);     return true; } return false;
        // 0x25 JALR: fallback
        case 0x28: if (M & LMASK_BR)  { lower_ibeq(VU);   return true; } return false;
        case 0x29: if (M & LMASK_BR)  { lower_ibne(VU);   return true; } return false;
        case 0x2C: if (M & LMASK_BR)  { lower_ibltz(VU);  return true; } return false;
        case 0x2D: if (M & LMASK_BR)  { lower_ibgtz(VU);  return true; } return false;
        case 0x2E: if (M & LMASK_BR)  { lower_iblez(VU);  return true; } return false;
        case 0x2F: if (M & LMASK_BR)  { lower_ibgez(VU);  return true; } return false;

        case 0x40: { // LowerOP sub-dispatch
            const u32 op6 = (FOP6 >= 0) ? (u32)FOP6 : (code & 0x3F);
            switch (op6) {
                case 0x30: if (M & LMASK_ALU) { lower_iadd(VU);   return true; } return false;
                case 0x31: if (M & LMASK_ALU) { lower_isub(VU);   return true; } return false;
                case 0x32: if (M & LMASK_ALU) { lower_iaddi(VU);  return true; } return false;
                case 0x34: if (M & LMASK_ALU) { lower_iand(VU);   return true; } return false;
                case 0x35: if (M & LMASK_ALU) { lower_ior(VU);    return true; } return false;
                case 0x3C: { // T3_00
                    const u32 sub = (FSUB >= 0) ? (u32)FSUB : ((code >> 6) & 0x1F);
                    switch (sub) {
                        case 0x0C: if (M & LMASK_MOVE){ lower_move(VU);   return true; } return false;
                        case 0x0D: if (M & LMASK_MEM) { lower_lqi(VU);    return true; } return false;
                        case 0x0F: if (M & LMASK_MOVE){ lower_mtir(VU);   return true; } return false;
                        case 0x10: if (M & LMASK_R)   { lower_rnext(VU);  return true; } return false;
                        case 0x16: if (M & LMASK_MISC){ lower_xtop(VU);   return true; } return false;
                        default: return false;
                    }
                }
                case 0x3D: { // T3_01
                    const u32 sub = (FSUB >= 0) ? (u32)FSUB : ((code >> 6) & 0x1F);
                    switch (sub) {
                        case 0x0C: if (M & LMASK_MOVE){ lower_mr32(VU);   return true; } return false;
                        case 0x0D: if (M & LMASK_MEM) { lower_sqi(VU);    return true; } return false;
                        case 0x0F: if (M & LMASK_MOVE){ lower_mfir(VU);   return true; } return false;
                        case 0x10: if (M & LMASK_R)   { lower_rget(VU);   return true; } return false;
                        case 0x16: if (M & LMASK_MISC){ lower_xitop(VU);  return true; } return false;
                        default: return false;
                    }
                }
                case 0x3E: { // T3_10
                    const u32 sub = (FSUB >= 0) ? (u32)FSUB : ((code >> 6) & 0x1F);
                    switch (sub) {
                        case 0x0D: if (M & LMASK_MEM) { lower_lqd(VU);    return true; } return false;
                        case 0x0F: if (M & LMASK_MEM) { lower_ilwr(VU);   return true; } return false;
                        case 0x10: if (M & LMASK_R)   { lower_rinit(VU);  return true; } return false;
                        default: return false;
                    }
                }
                case 0x3F: { // T3_11
                    const u32 sub = (FSUB >= 0) ? (u32)FSUB : ((code >> 6) & 0x1F);
                    switch (sub) {
                        case 0x0D: if (M & LMASK_MEM) { lower_sqd(VU);    return true; } return false;
                        case 0x0E: if (M & LMASK_MISC){ lower_waitq(VU);  return true; } return false;
                        case 0x0F: if (M & LMASK_MEM) { lower_iswr(VU);   return true; } return false;
                        case 0x10: if (M & LMASK_R)   { lower_rxor(VU);   return true; } return false;
                        case 0x1A: if (M & LMASK_MISC){ lower_waitp(VU);  return true; } return false;
                        default: return false;
                    }
                }
                default: return false;
            }
        }
        default: return false;
    }
}

static bool LowerDispatchNativeImpl(VURegs* VU) { return LowerDispatchNativeImplT<true>(VU); }

// 内部 native dispatcher (純粋に NEON で実行)。
// [P101-15 T1] 本体を always_inline の内部関数にし、定数 code での畳み込みを可能にした
// (公開シンボルはそのまま = 呼び手からは無変更)。
template <int FOP6 = -1, int FSUB = -1>
static __fi bool UpperDispatchNativeT(VURegs* VU)
{
    const u32 code = VU->code;
    const u32 op6  = (FOP6 >= 0) ? (u32)FOP6 : (code & 0x3F);

    switch (op6)
    {
        // ADDbc
        case 0x00: fmac_fd<FmOp::ADD, FtSrc::BcX>(VU); return true;
        case 0x01: fmac_fd<FmOp::ADD, FtSrc::BcY>(VU); return true;
        case 0x02: fmac_fd<FmOp::ADD, FtSrc::BcZ>(VU); return true;
        case 0x03: fmac_fd<FmOp::ADD, FtSrc::BcW>(VU); return true;
        // SUBbc
        case 0x04: fmac_fd<FmOp::SUB, FtSrc::BcX>(VU); return true;
        case 0x05: fmac_fd<FmOp::SUB, FtSrc::BcY>(VU); return true;
        case 0x06: fmac_fd<FmOp::SUB, FtSrc::BcZ>(VU); return true;
        case 0x07: fmac_fd<FmOp::SUB, FtSrc::BcW>(VU); return true;
        // MADDbc
        case 0x08: fmac_fd<FmOp::MADD, FtSrc::BcX>(VU); return true;
        case 0x09: fmac_fd<FmOp::MADD, FtSrc::BcY>(VU); return true;
        case 0x0A: fmac_fd<FmOp::MADD, FtSrc::BcZ>(VU); return true;
        case 0x0B: fmac_fd<FmOp::MADD, FtSrc::BcW>(VU); return true;
        // MSUBbc
        case 0x0C: fmac_fd<FmOp::MSUB, FtSrc::BcX>(VU); return true;
        case 0x0D: fmac_fd<FmOp::MSUB, FtSrc::BcY>(VU); return true;
        case 0x0E: fmac_fd<FmOp::MSUB, FtSrc::BcZ>(VU); return true;
        case 0x0F: fmac_fd<FmOp::MSUB, FtSrc::BcW>(VU); return true;
        // MAXbc
        case 0x10: fmac_fd<FmOp::MAX, FtSrc::BcX>(VU); return true;
        case 0x11: fmac_fd<FmOp::MAX, FtSrc::BcY>(VU); return true;
        case 0x12: fmac_fd<FmOp::MAX, FtSrc::BcZ>(VU); return true;
        case 0x13: fmac_fd<FmOp::MAX, FtSrc::BcW>(VU); return true;
        // MINIbc
        case 0x14: fmac_fd<FmOp::MINI, FtSrc::BcX>(VU); return true;
        case 0x15: fmac_fd<FmOp::MINI, FtSrc::BcY>(VU); return true;
        case 0x16: fmac_fd<FmOp::MINI, FtSrc::BcZ>(VU); return true;
        case 0x17: fmac_fd<FmOp::MINI, FtSrc::BcW>(VU); return true;
        // MULbc
        case 0x18: fmac_fd<FmOp::MUL, FtSrc::BcX>(VU); return true;
        case 0x19: fmac_fd<FmOp::MUL, FtSrc::BcY>(VU); return true;
        case 0x1A: fmac_fd<FmOp::MUL, FtSrc::BcZ>(VU); return true;
        case 0x1B: fmac_fd<FmOp::MUL, FtSrc::BcW>(VU); return true;

        // scalar q / i variants
        case 0x1C: fmac_fd<FmOp::MUL,  FtSrc::RegQ>(VU); return true; // MULq
        case 0x1D: fmac_fd<FmOp::MAX,  FtSrc::RegI>(VU); return true; // MAXi
        case 0x1E: fmac_fd<FmOp::MUL,  FtSrc::RegI>(VU); return true; // MULi
        case 0x1F: fmac_fd<FmOp::MINI, FtSrc::RegI>(VU); return true; // MINIi
        case 0x20: fmac_fd<FmOp::ADD,  FtSrc::RegQ>(VU); return true; // ADDq
        case 0x21: fmac_fd<FmOp::MADD, FtSrc::RegQ>(VU); return true; // MADDq

        // NOTE: ADDi (0x22) は TriAce hack (CHECK_VUADDSUBHACK) が絡むので fallback。
        //       MSUBi/SUBi 系は非 hack の ADD/SUB と同じだが、念のため 0x26/0x27 も fallback。
        //       Stage B で vuADDbc_addsubhack を NEON 化した後に有効化する。
        case 0x22: return false; // ADDi  fallback
        case 0x23: fmac_fd<FmOp::MADD, FtSrc::RegI>(VU); return true; // MADDi
        case 0x24: fmac_fd<FmOp::SUB,  FtSrc::RegQ>(VU); return true; // SUBq
        case 0x25: fmac_fd<FmOp::MSUB, FtSrc::RegQ>(VU); return true; // MSUBq
        case 0x26: return false; // SUBi fallback (対称のため念のため保守)
        case 0x27: fmac_fd<FmOp::MSUB, FtSrc::RegI>(VU); return true; // MSUBi

        // 4-way (Fd = Fs op Ft)
        case 0x28: fmac_fd<FmOp::ADD,  FtSrc::Direct>(VU); return true;
        case 0x29: fmac_fd<FmOp::MADD, FtSrc::Direct>(VU); return true;
        case 0x2A: fmac_fd<FmOp::MUL,  FtSrc::Direct>(VU); return true;
        case 0x2B: fmac_fd<FmOp::MAX,  FtSrc::Direct>(VU); return true;
        case 0x2C: fmac_fd<FmOp::SUB,  FtSrc::Direct>(VU); return true;
        case 0x2D: fmac_fd<FmOp::MSUB, FtSrc::Direct>(VU); return true;
        case 0x2E: op_opmsub(VU); return true; // OPMSUB [port 2026-05-30: re-wired; writes VF[fd] so Upper diff validates it]
        case 0x2F: fmac_fd<FmOp::MINI, FtSrc::Direct>(VU); return true;

        // sub-op tables (0x38=FD_00, 0x39=FD_01, 0x3A=FD_10, 0x3B=FD_11)
        // Dst=Acc の FMAC 系をここで native 化する。
        // FTOI/ITOF/CLIP/OPMULA は未対応 (fallback)。
        case 0x3C: // UPPER_FD_00 [port 2026-05-30 BUG FIX: was 0x38; stock _UPPER_OPCODE places FD_00 at op6=0x3C, so all FD-type ops were falling back]: ADDAx/SUBAx/MADDAx/MSUBAx/ITOF0/FTOI0/MULAx/MULAq/ADDAq/SUBAq/ADDA/SUBA
        {
            const u32 sub = (FSUB >= 0) ? (u32)FSUB : ((code >> 6) & 0x1F);
            switch (sub) {
                case 0x00: fmac_acc<FmOp::ADD,  FtSrc::BcX>(VU);    return true; // ADDAx
                case 0x01: fmac_acc<FmOp::SUB,  FtSrc::BcX>(VU);    return true; // SUBAx
                case 0x02: fmac_acc<FmOp::MADD, FtSrc::BcX>(VU);    return true; // MADDAx
                case 0x03: fmac_acc<FmOp::MSUB, FtSrc::BcX>(VU);    return true; // MSUBAx
                case 0x06: fmac_acc<FmOp::MUL,  FtSrc::BcX>(VU);    return true; // MULAx
                case 0x07: fmac_acc<FmOp::MUL,  FtSrc::RegQ>(VU);   return true; // MULAq
                case 0x08: fmac_acc<FmOp::ADD,  FtSrc::RegQ>(VU);   return true; // ADDAq
                case 0x09: fmac_acc<FmOp::SUB,  FtSrc::RegQ>(VU);   return true; // SUBAq
                case 0x0A: fmac_acc<FmOp::ADD,  FtSrc::Direct>(VU); return true; // ADDA
                case 0x0B: fmac_acc<FmOp::SUB,  FtSrc::Direct>(VU); return true; // SUBA
                case 0x04: op_itof_n<0>(VU);                        return true; // ITOF0 [port 2026-05-30: wired]
                case 0x05: op_ftoi_n<0>(VU);                        return true; // FTOI0 [port 2026-05-30: wired]
                default:   return false;
            }
        }
        case 0x3D: // UPPER_FD_01 [port 2026-05-30 BUG FIX: was 0x39]: ADDAy/SUBAy/MADDAy/MSUBAy/ITOF4/FTOI4/MULAy/ABS/MADDAq/MSUBAq/MADDA/MSUBA
        {
            const u32 sub = (FSUB >= 0) ? (u32)FSUB : ((code >> 6) & 0x1F);
            switch (sub) {
                case 0x00: fmac_acc<FmOp::ADD,  FtSrc::BcY>(VU);    return true; // ADDAy
                case 0x01: fmac_acc<FmOp::SUB,  FtSrc::BcY>(VU);    return true; // SUBAy
                case 0x02: fmac_acc<FmOp::MADD, FtSrc::BcY>(VU);    return true; // MADDAy
                case 0x03: fmac_acc<FmOp::MSUB, FtSrc::BcY>(VU);    return true; // MSUBAy
                case 0x06: fmac_acc<FmOp::MUL,  FtSrc::BcY>(VU);    return true; // MULAy
                case 0x07: op_abs(VU);                              return true; // ABS
                case 0x08: fmac_acc<FmOp::MADD, FtSrc::RegQ>(VU);   return true; // MADDAq
                case 0x09: fmac_acc<FmOp::MSUB, FtSrc::RegQ>(VU);   return true; // MSUBAq
                case 0x0A: fmac_acc<FmOp::MADD, FtSrc::Direct>(VU); return true; // MADDA
                case 0x0B: fmac_acc<FmOp::MSUB, FtSrc::Direct>(VU); return true; // MSUBA
                case 0x04: op_itof_n<4>(VU);                        return true; // ITOF4 [port 2026-05-30: wired]
                case 0x05: op_ftoi_n<4>(VU);                        return true; // FTOI4 [port 2026-05-30: wired]
                default:   return false;
            }
        }
        case 0x3E: // UPPER_FD_10 [port 2026-05-30 BUG FIX: was 0x3A]: ADDAz/SUBAz/MADDAz/MSUBAz/ITOF12/FTOI12/MULAz/MULAi/ADDAi/SUBAi/MULA/OPMULA
        {
            const u32 sub = (FSUB >= 0) ? (u32)FSUB : ((code >> 6) & 0x1F);
            switch (sub) {
                case 0x00: fmac_acc<FmOp::ADD,  FtSrc::BcZ>(VU);    return true; // ADDAz
                case 0x01: fmac_acc<FmOp::SUB,  FtSrc::BcZ>(VU);    return true; // SUBAz
                case 0x02: fmac_acc<FmOp::MADD, FtSrc::BcZ>(VU);    return true; // MADDAz
                case 0x03: fmac_acc<FmOp::MSUB, FtSrc::BcZ>(VU);    return true; // MSUBAz
                case 0x06: fmac_acc<FmOp::MUL,  FtSrc::BcZ>(VU);    return true; // MULAz
                case 0x07: fmac_acc<FmOp::MUL,  FtSrc::RegI>(VU);   return true; // MULAi
                case 0x08: fmac_acc<FmOp::ADD,  FtSrc::RegI>(VU);   return true; // ADDAi
                case 0x09: fmac_acc<FmOp::SUB,  FtSrc::RegI>(VU);   return true; // SUBAi
                case 0x0A: fmac_acc<FmOp::MUL,  FtSrc::Direct>(VU); return true; // MULA
                case 0x0B: op_opmula(VU);                           return true; // OPMULA [port 2026-05-30: wired]
                case 0x04: op_itof_n<12>(VU);                       return true; // ITOF12 [port 2026-05-30: wired]
                case 0x05: op_ftoi_n<12>(VU);                       return true; // FTOI12 [port 2026-05-30: wired]
                default:   return false;
            }
        }
        case 0x3F: // UPPER_FD_11 [port 2026-05-30 BUG FIX: was 0x3B]: ADDAw/SUBAw/MADDAw/MSUBAw/ITOF15/FTOI15/MULAw/CLIP/MADDAi/MSUBAi/NOP
        {
            const u32 sub = (FSUB >= 0) ? (u32)FSUB : ((code >> 6) & 0x1F);
            switch (sub) {
                case 0x00: fmac_acc<FmOp::ADD,  FtSrc::BcW>(VU);    return true; // ADDAw
                case 0x01: fmac_acc<FmOp::SUB,  FtSrc::BcW>(VU);    return true; // SUBAw
                case 0x02: fmac_acc<FmOp::MADD, FtSrc::BcW>(VU);    return true; // MADDAw
                case 0x03: fmac_acc<FmOp::MSUB, FtSrc::BcW>(VU);    return true; // MSUBAw
                case 0x06: fmac_acc<FmOp::MUL,  FtSrc::BcW>(VU);    return true; // MULAw
                case 0x07: op_clip(VU);                             return true; // CLIP [port 2026-05-30: wired]
                case 0x08: fmac_acc<FmOp::MADD, FtSrc::RegI>(VU);   return true; // MADDAi
                case 0x09: fmac_acc<FmOp::MSUB, FtSrc::RegI>(VU);   return true; // MSUBAi
                case 0x0B: /* NOP */                                return true;
                case 0x04: op_itof_n<15>(VU);                       return true; // ITOF15 [port 2026-05-30: wired]
                case 0x05: op_ftoi_n<15>(VU);                       return true; // FTOI15 [port 2026-05-30: wired]
                default:   return false;
            }
        }
        default:
            return false;
    }
}

bool UpperDispatchNative(VURegs* VU) { return UpperDispatchNativeT<>(VU); }

// Public dispatch。UpperEnabled が false なら即 false 返しで interpreter fallback。
// DiffEnabled のとき: native → snapshot → 復元 → interpreter → diff log。
// いずれにせよ「最終的に VU 状態に残るのは interpreter の結果」(diff mode 時)。
// これにより native にバグがあっても emulation は壊れない。
bool UpperDispatchFull(VURegs* VU)
{
    if (!g_iv2vu_upper_en)
        return false;

    // [P101-5 Phase 1] Fast path: skip per-op atomic stats and diff-mode branch
    // when diff mode off. Stats are only for initial validation (kept behind
    // VU_IV2_DISPATCH_STATS=1 env for on-demand diagnostics).
    if (__builtin_expect(!g_iv2vu_diff_en, 1))
    {
        return UpperDispatchNative(VU);
    }

    // Dispatch 統計 (diff mode時のみ) - 最初 10 回 + 以後 64k ごとに summary 出力
    u64 called = s_dispatch_called.fetch_add(1, std::memory_order_relaxed) + 1;
    if (called <= 10 || (called & 0xFFFF) == 0)
    {
        Console.WriteLn("@@VU_IV2_DISPATCH@@ called=%llu handled=%llu fallback=%llu code=%08x",
            (unsigned long long)called,
            (unsigned long long)s_dispatch_handled.load(std::memory_order_relaxed),
            (unsigned long long)s_dispatch_fallback.load(std::memory_order_relaxed),
            VU->code);
    }

    // Diff mode: native → 比較 → interpreter → 差異 log
    //   書き換え対象は: VF[_Fd_] (FMAC/MAX/MINI), VF[_Ft_] (ABS),
    //                   macflag (FMAC), statusflag (FMAC), ACC (ACC-dest系は本 native 未対応)
    //   ClipFlag は CLIP (本 native 未対応) が更新。FMAC/MAX/MINI は触らない。
    const u32 code = VU->code;
    const u32 fd = code_Fd(code);
    const u32 ft = code_Ft(code);

    // 保存 pre-state (書き換え可能性があるもの全て)
    VECTOR pre_vf_fd = VU->VF[fd];
    VECTOR pre_vf_ft = VU->VF[ft];
    const u32 pre_mac  = VU->macflag;
    const u32 pre_stat = VU->statusflag;
    // [port 2026-05-30] ACC-dest ops (MADDA/MSUBA) READ and WRITE ACC, and CLIP
    // reads+writes clipflag. The native run mutates them; if we do not restore
    // them before re-running the interpreter, the interpreter reads the native's
    // mutated ACC/clipflag and produces a spurious mismatch. Snapshot+restore both.
    const VECTOR pre_acc  = VU->ACC;
    const u32    pre_clip = VU->clipflag;

    // native 実行
    bool handled = UpperDispatchNative(VU);
    if (!handled)
    {
        s_dispatch_fallback.fetch_add(1, std::memory_order_relaxed);
        return false;  // native 未対応 → caller が interpreter fallback する
    }
    s_dispatch_handled.fetch_add(1, std::memory_order_relaxed);

    // native 実行後の結果を保存
    VECTOR post_native_vf_fd = VU->VF[fd];
    VECTOR post_native_vf_ft = VU->VF[ft];
    const u32 post_native_mac  = VU->macflag;
    const u32 post_native_stat = VU->statusflag;

    // pre-state 復元
    VU->VF[fd]       = pre_vf_fd;
    VU->VF[ft]       = pre_vf_ft;
    VU->macflag      = pre_mac;
    VU->statusflag   = pre_stat;
    VU->ACC          = pre_acc;   // [port 2026-05-30] restore ACC for ACC-dest op re-run
    VU->clipflag     = pre_clip;  // [port 2026-05-30] restore clipflag for CLIP re-run

    // interpreter 実行 — VU0 macro path から呼ばれる場合は VU0_UPPER_OPCODE を使う必要がある
    // (VU1_UPPER_OPCODE を無条件呼出は VU0 op なのに VU1 state を叩く重大バグ)。
    if (VU == &VU0)
        VU0_UPPER_OPCODE[code & 0x3f]();
    else
        VU1_UPPER_OPCODE[code & 0x3f]();

    // interpreter 結果 (この値が最終的に VU に残る)
    const VECTOR post_interp_vf_fd = VU->VF[fd];
    const VECTOR post_interp_vf_ft = VU->VF[ft];
    const u32 post_interp_mac  = VU->macflag;
    const u32 post_interp_stat = VU->statusflag;

    // 比較
    bool vf_fd_diff = (post_native_vf_fd.UD[0] != post_interp_vf_fd.UD[0]) ||
                      (post_native_vf_fd.UD[1] != post_interp_vf_fd.UD[1]);
    bool vf_ft_diff = (post_native_vf_ft.UD[0] != post_interp_vf_ft.UD[0]) ||
                      (post_native_vf_ft.UD[1] != post_interp_vf_ft.UD[1]);
    bool mac_diff   = (post_native_mac  != post_interp_mac);
    bool stat_diff  = (post_native_stat != post_interp_stat);

    s_diff_total.fetch_add(1, std::memory_order_relaxed);

    if (vf_fd_diff || vf_ft_diff || mac_diff || stat_diff)
    {
        s_diff_mismatch.fetch_add(1, std::memory_order_relaxed);

        // [P101-T audit] per-op histogram keyed by (op6, sub) — 64 * 32 = 2048 slots.
        // Separate counters for vf_fd / mac / stat fields so we know which ops have
        // which kind of bug.
        {
            const u32 op6 = code & 0x3F;
            const u32 sub = (code >> 6) & 0x1F;
            const u32 key = (op6 << 5) | sub;
            static std::atomic<u32> s_hist_vf[2048];
            static std::atomic<u32> s_hist_mac[2048];
            static std::atomic<u32> s_hist_stat[2048];
            static std::atomic<u64> s_hist_total{0};
            if (vf_fd_diff || vf_ft_diff) s_hist_vf[key].fetch_add(1, std::memory_order_relaxed);
            if (mac_diff)                 s_hist_mac[key].fetch_add(1, std::memory_order_relaxed);
            if (stat_diff)                s_hist_stat[key].fetch_add(1, std::memory_order_relaxed);
            u64 h = s_hist_total.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((h & 0xFFFFF) == 0) {
                // Log top-20 (by total across fields) every ~1M mismatches.
                struct E { u32 key; u32 vf, mac, stat, sum; };
                E top[20]; int n = 0;
                for (u32 k = 0; k < 2048; k++) {
                    u32 v = s_hist_vf[k].load(std::memory_order_relaxed);
                    u32 m = s_hist_mac[k].load(std::memory_order_relaxed);
                    u32 st= s_hist_stat[k].load(std::memory_order_relaxed);
                    u32 s = v + m + st;
                    if (s == 0) continue;
                    if (n < 20) top[n++] = {k, v, m, st, s};
                    else {
                        int w = 0;
                        for (int j = 1; j < n; j++) if (top[j].sum < top[w].sum) w = j;
                        if (s > top[w].sum) top[w] = {k, v, m, st, s};
                    }
                }
                for (int i = 0; i < n; i++)
                    for (int j = i+1; j < n; j++)
                        if (top[j].sum > top[i].sum) { auto t=top[i]; top[i]=top[j]; top[j]=t; }
                for (int i = 0; i < n; i++) {
                    u32 o6 = top[i].key >> 5;
                    u32 sb = top[i].key & 0x1F;
                    Console.WriteLn("@@VU_IV2_DIFF_HIST@@ rank=%d op6=%02x sub=%02x vf=%u mac=%u stat=%u sum=%u",
                        i, o6, sb, top[i].vf, top[i].mac, top[i].stat, top[i].sum);
                }
            }
        }
        u32 quota = s_diff_log_quota.load(std::memory_order_relaxed);
        if (quota > 0)
        {
            s_diff_log_quota.store(quota - 1, std::memory_order_relaxed);
            // 出力: op code, pc (TPC はすでに +=8 されているので -8 で元 pc)
            u32 pc = VU->VI[REG_TPC].UL - 8;
            u32 op6 = code & 0x3F;
            u32 sub = (code >> 6) & 0x1F;
            if (vf_fd_diff)
            {
                Console.WriteLn("@@VU_IV2_DIFF@@ pc=%08x op=%02x sub=%02x fd=%u xyzw=%x field=VF[fd] "
                    "nat=%08x_%08x_%08x_%08x int=%08x_%08x_%08x_%08x",
                    pc, op6, sub, fd, code_XYZW(code),
                    post_native_vf_fd.UL[0], post_native_vf_fd.UL[1],
                    post_native_vf_fd.UL[2], post_native_vf_fd.UL[3],
                    post_interp_vf_fd.UL[0], post_interp_vf_fd.UL[1],
                    post_interp_vf_fd.UL[2], post_interp_vf_fd.UL[3]);
            }
            if (vf_ft_diff)
            {
                Console.WriteLn("@@VU_IV2_DIFF@@ pc=%08x op=%02x sub=%02x ft=%u field=VF[ft] "
                    "nat=%08x_%08x_%08x_%08x int=%08x_%08x_%08x_%08x",
                    pc, op6, sub, ft,
                    post_native_vf_ft.UL[0], post_native_vf_ft.UL[1],
                    post_native_vf_ft.UL[2], post_native_vf_ft.UL[3],
                    post_interp_vf_ft.UL[0], post_interp_vf_ft.UL[1],
                    post_interp_vf_ft.UL[2], post_interp_vf_ft.UL[3]);
            }
            if (mac_diff)
            {
                Console.WriteLn("@@VU_IV2_DIFF@@ pc=%08x op=%02x sub=%02x field=macflag nat=%08x int=%08x",
                    pc, op6, sub, post_native_mac, post_interp_mac);
            }
            if (stat_diff)
            {
                Console.WriteLn("@@VU_IV2_DIFF@@ pc=%08x op=%02x sub=%02x field=statusflag nat=%08x int=%08x",
                    pc, op6, sub, post_native_stat, post_interp_stat);
            }
        }

        // 1000 件ごとに summary 出力
        u64 mis = s_diff_mismatch.load(std::memory_order_relaxed);
        u64 tot = s_diff_total.load(std::memory_order_relaxed);
        if ((mis & 0x3FF) == 0)
        {
            Console.WriteLn("@@VU_IV2_DIFF_SUM@@ mismatch=%llu total=%llu rate=%.2f%%",
                (unsigned long long)mis, (unsigned long long)tot,
                (tot ? (100.0 * mis / tot) : 0.0));
        }
    }

    return true;  // interpreter が実行済 → caller は fallback しない
}

// ────────────────────────────────────────────────────────────
// Public Lower dispatch
// ────────────────────────────────────────────────────────────
// 最小限: env gate + stats。diff mode は将来拡張 (現状 Upper のみ対応)。
bool LowerDispatchFull(VURegs* VU)
{
    if (!g_iv2vu_lower_en)
        return false;

    // [P101-5 Phase 1] Fast path: bypass atomic stats and diff branches
    if (__builtin_expect(!g_iv2vu_diff_en, 1))
    {
        return LowerDispatchNative(VU);
    }

    u64 called = s_lower_called.fetch_add(1, std::memory_order_relaxed) + 1;
    if (called <= 5 || (called & 0xFFFFF) == 0)
    {
        Console.WriteLn("@@VU_IV2_OPS_LOWER@@ called=%llu handled=%llu fallback=%llu code=%08x",
            (unsigned long long)called,
            (unsigned long long)s_lower_handled.load(std::memory_order_relaxed),
            (unsigned long long)s_lower_fallback.load(std::memory_order_relaxed),
            VU->code);
    }

    // [P101-T LOWER DIFF] diff mode: native 実行 → state snapshot → 復元 → interp 実行 → bit-exact 比較
    // 副作用: 最終 state は interpreter 結果 (安全)。
    // cost: VF[32]+VI[32]+scalars (~1.5KB) + VU1.Mem (16KB) snapshot/restore per op。
    //       1M ops 限定で自動 disable (perf 劣化防止)。
    if (__builtin_expect(g_iv2vu_diff_en && g_iv2vu_lower_en, 0))
    {
        static std::atomic<u64> s_ldiff_total{0};
        static std::atomic<u64> s_ldiff_mismatch{0};
        static std::atomic<u32> s_ldiff_log_quota{200};
        static constexpr u64 LDIFF_LIMIT = 2000000;

        u64 ltotal = s_ldiff_total.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ltotal > LDIFF_LIMIT)
        {
            if (ltotal == LDIFF_LIMIT + 1)
                Console.Error("@@VU_IV2_L_DIFF@@ DISABLED after %llu ops. total_mismatch=%llu",
                    (unsigned long long)LDIFF_LIMIT,
                    (unsigned long long)s_ldiff_mismatch.load(std::memory_order_relaxed));
            // diff 限度超えたら通常 native path
            bool h = LowerDispatchNative(VU);
            if (h) s_lower_handled.fetch_add(1, std::memory_order_relaxed);
            else   s_lower_fallback.fetch_add(1, std::memory_order_relaxed);
            return h;
        }

        // Pre-snapshot: affected fields + VU1.Mem 全体 (16KB)
        struct LDiffSnap {
            VECTOR vf[32];           // 512B
            REG_VI vi[32];           // 512B (32 × 16B)
            VECTOR acc;              // 16B
            u32 branch, branchpc, delaybranchpc;
            bool takedelaybranch;
            u32 ebit;
            u32 pending_q, pending_p;
            u8 VIBackupCycles;
            u32 VIRegNumber, VIOldValue;
            u32 macflag, statusflag, clipflag;
            REG_VI q_, p_;
            u8 mem[0x4000];          // 16KB
        };
        static thread_local LDiffSnap s_pre, s_post_native;

        auto snap = [&](LDiffSnap& s) {
            std::memcpy(s.vf, VU->VF, sizeof(s.vf));
            std::memcpy(s.vi, VU->VI, sizeof(s.vi));
            s.acc = VU->ACC;
            s.branch = VU->branch;
            s.branchpc = VU->branchpc;
            s.delaybranchpc = VU->delaybranchpc;
            s.takedelaybranch = VU->takedelaybranch;
            s.ebit = VU->ebit;
            s.pending_q = VU->pending_q;
            s.pending_p = VU->pending_p;
            s.VIBackupCycles = VU->VIBackupCycles;
            s.VIRegNumber = VU->VIRegNumber;
            s.VIOldValue = VU->VIOldValue;
            s.macflag = VU->macflag;
            s.statusflag = VU->statusflag;
            s.clipflag = VU->clipflag;
            s.q_ = VU->q;
            s.p_ = VU->p;
            std::memcpy(s.mem, VU->Mem, 0x4000);
        };
        auto restore = [&](const LDiffSnap& s) {
            std::memcpy(VU->VF, s.vf, sizeof(s.vf));
            std::memcpy(VU->VI, s.vi, sizeof(s.vi));
            VU->ACC = s.acc;
            VU->branch = s.branch;
            VU->branchpc = s.branchpc;
            VU->delaybranchpc = s.delaybranchpc;
            VU->takedelaybranch = s.takedelaybranch;
            VU->ebit = s.ebit;
            VU->pending_q = s.pending_q;
            VU->pending_p = s.pending_p;
            VU->VIBackupCycles = s.VIBackupCycles;
            VU->VIRegNumber = s.VIRegNumber;
            VU->VIOldValue = s.VIOldValue;
            VU->macflag = s.macflag;
            VU->statusflag = s.statusflag;
            VU->clipflag = s.clipflag;
            VU->q = s.q_;
            VU->p = s.p_;
            std::memcpy(VU->Mem, s.mem, 0x4000);
        };

        snap(s_pre);

        bool handled = LowerDispatchNative(VU);
        if (!handled)
        {
            // native 未対応 → fallback 予定。diff する必要なし (interp が必ず同じ結果を出す)。
            s_lower_fallback.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        s_lower_handled.fetch_add(1, std::memory_order_relaxed);

        snap(s_post_native);

        // Restore pre → run interpreter
        restore(s_pre);
        const u32 raw = VU->code;
        if (VU == &VU0)
            VU0_LOWER_OPCODE[raw >> 25]();
        else
            VU1_LOWER_OPCODE[raw >> 25]();

        // Compare s_post_native vs current (post-interp)
        u32 vf_diff = 0;    // bitmask of differing VF indices
        u32 vi_diff = 0;    // bitmask of differing VI indices
        bool scalar_diff = false;
        u32 mem_diff_addr = 0xFFFFFFFF;

        for (int i = 0; i < 32; i++) {
            if (std::memcmp(&s_post_native.vf[i], &VU->VF[i], sizeof(VECTOR)) != 0)
                vf_diff |= (1u << i);
        }
        for (int i = 0; i < 32; i++) {
            if (s_post_native.vi[i].UL != VU->VI[i].UL)
                vi_diff |= (1u << i);
        }
        if (std::memcmp(&s_post_native.acc, &VU->ACC, sizeof(VECTOR)) != 0) scalar_diff = true;
        if (s_post_native.branch != VU->branch ||
            s_post_native.branchpc != VU->branchpc ||
            s_post_native.takedelaybranch != VU->takedelaybranch ||
            s_post_native.delaybranchpc != VU->delaybranchpc) scalar_diff = true;
        if (s_post_native.macflag != VU->macflag ||
            s_post_native.statusflag != VU->statusflag ||
            s_post_native.clipflag != VU->clipflag) scalar_diff = true;
        if (s_post_native.VIBackupCycles != VU->VIBackupCycles ||
            s_post_native.VIRegNumber != VU->VIRegNumber ||
            s_post_native.VIOldValue != VU->VIOldValue) scalar_diff = true;
        for (int i = 0; i < 0x4000; i += 16) {
            if (std::memcmp(&s_post_native.mem[i], &VU->Mem[i], 16) != 0) {
                mem_diff_addr = i;
                break;
            }
        }

        const bool differ = (vf_diff || vi_diff || scalar_diff || mem_diff_addr != 0xFFFFFFFF);
        if (differ)
        {
            u64 mis = s_ldiff_mismatch.fetch_add(1, std::memory_order_relaxed) + 1;
            u32 quota = s_ldiff_log_quota.load(std::memory_order_relaxed);
            if (quota > 0) {
                s_ldiff_log_quota.store(quota - 1, std::memory_order_relaxed);
                const u32 op7 = raw >> 25;
                const u32 op6 = raw & 0x3F;
                const u32 sub = (raw >> 6) & 0x1F;
                Console.Error("@@VU_IV2_L_DIFF@@ #%llu code=%08x op7=%02x op6=%02x sub=%02x vf_diff=%08x vi_diff=%08x scalar=%d mem_off=%x",
                    (unsigned long long)mis, raw, op7, op6, sub, vf_diff, vi_diff, scalar_diff ? 1 : 0, mem_diff_addr);
                // VF 詳細
                for (int i = 0; i < 32; i++) {
                    if (vf_diff & (1u << i)) {
                        Console.Error("@@VU_IV2_L_DIFF@@   VF[%d] nat=%08x_%08x_%08x_%08x int=%08x_%08x_%08x_%08x",
                            i,
                            s_post_native.vf[i].UL[0], s_post_native.vf[i].UL[1],
                            s_post_native.vf[i].UL[2], s_post_native.vf[i].UL[3],
                            VU->VF[i].UL[0], VU->VF[i].UL[1], VU->VF[i].UL[2], VU->VF[i].UL[3]);
                    }
                }
                // VI 詳細
                for (int i = 0; i < 32; i++) {
                    if (vi_diff & (1u << i)) {
                        Console.Error("@@VU_IV2_L_DIFF@@   VI[%d] nat=%08x int=%08x",
                            i, s_post_native.vi[i].UL, VU->VI[i].UL);
                    }
                }
                // Mem 詳細
                if (mem_diff_addr != 0xFFFFFFFF) {
                    const u32* na = (const u32*)&s_post_native.mem[mem_diff_addr];
                    const u32* in_ = (const u32*)&VU->Mem[mem_diff_addr];
                    Console.Error("@@VU_IV2_L_DIFF@@   Mem[%04x] nat=%08x_%08x_%08x_%08x int=%08x_%08x_%08x_%08x",
                        mem_diff_addr, na[0], na[1], na[2], na[3], in_[0], in_[1], in_[2], in_[3]);
                }
            }
            // 1024 mismatch 毎に summary
            if ((mis & 0x3FF) == 0) {
                Console.Error("@@VU_IV2_L_DIFF_SUM@@ mismatch=%llu total=%llu",
                    (unsigned long long)mis, (unsigned long long)ltotal);
            }
        }
        return true;  // interpreter 結果を保持
    }

    bool handled = LowerDispatchNative(VU);
    if (handled) s_lower_handled.fetch_add(1, std::memory_order_relaxed);
    else         s_lower_fallback.fetch_add(1, std::memory_order_relaxed);
    return handled;
}

// ════════════════════════════════════════════════════════════════════════════════
// [P101-15] オフライン変換 T1 — build 時に C++ へ機械変換した VU1 microprogram
//
// 設計 = docs/report/P101-13_structural_levers_offline_translation_2026-07-02.md §2。
// 変換器 = Scripts/t1_gen.py。生成物 = iv2_t1_*.inc (per-PC ラベル + 特殊化本体)。
//
// **意味は一切理解しない**。iv2_vu1Exec の本体を「命令語が定数」という前提で展開し、
// 同じ helper を同じ順序で呼ぶだけ。よって bit-exact by construction。
// 削れるのは固定オーバーヘッド (fetch / decode cache 参照 / E-D-T bit 判定 /
// dispatch switch / hazard plan 分岐) のみで、演算本体はそのまま走る。
//
// 安全性:
//   ・entry gate = hash 一致 + 16KB image の該当語 memcmp + decode 前提の実表照合。
//     1 つでも食い違えば T1 は自分を無効化してインタプリタへ (静かに壊れない)。
//   ・分岐で TPC が変換範囲外へ出たら即 bail (全状態が VURegs に実体化しているので
//     任意の op 境界で bail が健全)。
//   ・E/D/T bit を持つ命令は変換対象外 (= インタプリタが処理)。
//   ・default OFF。kill-switch / 有効化 = env `iPSX2_VU1_T1=1`。
// ════════════════════════════════════════════════════════════════════════════════

// ★ pipeline stall シミュレータはインタプリタと同じく out-of-line 呼び出しに固定する。
//   call site の _VURegsNum が定数アドレスになると LTO が本体 (4 段リングの走査) を
//   各ステップへ inline + 4 倍展開し、85 命令で 460KB に膨らむのを実測 (2026-08-06)。
//   T1 の狙いは orchestration の固定オーバーヘッド除去であって op body の複製ではない。
// [P101-15 T2 上限 probe] compile-gate。1 でビルドすると T1 経路の pipeline stall
// シミュレーションを丸ごと省略する = 「T2 が理論上取り切れる最大値」を測るための診断専用。
// **cycle 会計が壊れるので content が変わる = 計測専用・視覚破綻**。既定 0 = 機械語も同一。
// 撤去条件: T2 実装方針が決まったら削除可 (数値は docs/report/P101-15 に残す)。
#ifndef IV2_T2_CEIL
#define IV2_T2_CEIL 0
#endif

static __attribute__((noinline)) void t1_test_upper(VURegs* VU, _VURegsNum* r) {
#if !IV2_T2_CEIL
    _vuTestUpperStalls(VU, r);
#else
    (void)VU; (void)r;
#endif
}
static __attribute__((noinline)) void t1_test_lower(VURegs* VU, _VURegsNum* r) {
#if !IV2_T2_CEIL
    _vuTestLowerStalls(VU, r);
#else
    (void)VU; (void)r;
#endif
}
static __attribute__((noinline)) void t1_add_upper (VURegs* VU, _VURegsNum* r) {
#if !IV2_T2_CEIL
    _vuAddUpperStalls(VU, r);
#else
    (void)VU; (void)r;
#endif
}
static __attribute__((noinline)) void t1_add_lower (VURegs* VU, _VURegsNum* r) {
#if !IV2_T2_CEIL
    _vuAddLowerStalls(VU, r);
#else
    (void)VU; (void)r;
#endif
}
static __attribute__((noinline)) void t1_test_pipes(VURegs* VU) {
#if !IV2_T2_CEIL
    _vuTestPipes(VU);
#else
    (void)VU;
#endif
}
// [P122] 呼び出し側が判定した ready ビットだけを処理する版 (VUops.cpp の _vuTestPipesReady)。
static __attribute__((noinline)) void t1_test_pipes_ready(VURegs* VU, u32 ready) {
#if !IV2_T2_CEIL
    _vuTestPipesReady(VU, ready);
#else
    (void)VU; (void)ready;
#endif
}
static __attribute__((noinline)) void t1_clear_fmac(VURegs* VU)                { _vuClearFMAC(VU); }

// ── 1 命令ステップの構成要素 (iv2_vu.cpp の iv2_vu1Exec と逐語対応) ──────────────
#define T1_HEAD()                                                                    \
    do {                                                                             \
        if ((VU1.cycle - startcycles) >= cycles) return;                             \
        if (!(VU0.VI[REG_VPU_STAT].UL & 0x100)) {                                    \
            if (VU->branch == 1) { VU->VI[REG_TPC].UL = VU->branchpc; VU->branch = 0; } \
            return;                                                                  \
        }                                                                            \
        VU1.cycle++;                                                                 \
    } while (0)

// pipes-idle fast-exit (iv2_vu1Exec の pipes_active_check と同一。T1 は VU1 専用なので
// `VU == &VU1` は定数 true)。
// ★ [P120] 「何か入っているか」ではなく **「フラッシュできる実体があるか」** で呼ぶ。
//   旧実装 (`_pi` = 4 本とも空か) は、**入っているがまだ完了していない**間ずっと
//   `t1_test_pipes` を呼び、4 つの flush が全部 false を返して帰るだけの空呼び出しになる。
//   GoW II 実戦闘の profile では `t1_test_pipes` が leaf 16.62% = 全 wall の最大項だった。
//   ★ 厳密等価である根拠 (VUops.cpp):
//     - `_vuFMACflush` / `_vuIALUflush` は **リング先頭が未完了なら即 return** する FIFO なので、
//       状態を変えうるのは「count != 0 かつ先頭が完了」のときだけ。
//     - `_vuFDIVflush` / `_vuEFUflush` は単一エントリで「enable かつ完了」のときだけ。
//     - do-while はどれかが flush したときだけ再走する。
//     - 末尾の XGKICK 処理は `xgkickenable` のときだけ。
//   よって下の条件が全部偽なら `_vuTestPipes` は **純粋な no-op** = 呼ばなくてよい。
#define T1_PIPES()                                                                   \
    do {                                                                             \
        const u32 _cyc = VU->cycle;                                                  \
        u32 _rdy = 0;                                                                \
        if (VU->fmaccount != 0 &&                                                    \
            (_cyc - VU->fmac[VU->fmacreadpos].sCycle) >= VU->fmac[VU->fmacreadpos].Cycle) _rdy |= 1u; \
        if (VU->fdiv.enable != 0 && (_cyc - VU->fdiv.sCycle) >= VU->fdiv.Cycle)      _rdy |= 2u; \
        if (VU->efu.enable  != 0 && (_cyc - VU->efu.sCycle)  >= VU->efu.Cycle)       _rdy |= 4u; \
        if (VU->ialucount != 0 &&                                                    \
            (_cyc - VU->ialu[VU->ialureadpos].sCycle) >= VU->ialu[VU->ialureadpos].Cycle) _rdy |= 8u; \
        if (__builtin_expect(_rdy != 0 || VU1.xgkickenable, 0)) t1_test_pipes_ready(VU, _rdy); \
    } while (0)

#define T1_VIBACKUP(CBO)                                                             \
    do {                                                                             \
        if (VU->VIBackupCycles > 0)                                                  \
            VU->VIBackupCycles -= std::min((u8)(VU1.cycle - (CBO)), VU->VIBackupCycles); \
    } while (0)

#define T1_BRANCH_EPI()                                                              \
    do {                                                                             \
        if (VU->branch > 0) {                                                        \
            if (VU->branch-- == 1) {                                                 \
                VU->VI[REG_TPC].UL = VU->branchpc;                                   \
                if (VU->takedelaybranch) {                                           \
                    VU->branch = 1; VU->branchpc = VU->delaybranchpc;                 \
                    VU->takedelaybranch = false;                                     \
                }                                                                    \
            }                                                                        \
        }                                                                            \
    } while (0)

// iv2_vu1ExecUpper / iv2_vu1ExecLower と同じ「native → 未対応なら stock table」の 2 段。
// code が定数なので switch は畳まれ、native 対応 op なら helper への直呼び 1 本に、
// 非対応 op なら table 直引き 1 本になる。
// ★ op body は「命令語ごと」ではなく「op key ごと」に 1 本だけ実体化する (noinline)。
//   命令語自体は VU->code 経由で渡す = インタプリタと同じ generic body を共有。
//   ここで消えるのは dispatch switch とその間接分岐だけ。
//   (命令語まで畳み込むと op body が命令ごとに複製され、85 命令で 483KB に爆発する
//    ことを実測済 — 2026-08-06。本 project が 2 度負けた「ホット関数肥大」そのもの。)
template <int OP6, int SUB>
static __attribute__((noinline)) void t1_upper_thunk(VURegs* VU)
{
    if (!UpperDispatchNativeT<OP6, SUB>(VU)) VU1_UPPER_OPCODE[OP6]();
}
template <int OP7, int OP6, int SUB>
static __attribute__((noinline)) void t1_lower_thunk(VURegs* VU)
{
    if (!LowerDispatchNativeImplT<false, OP7, OP6, SUB>(VU)) VU1_LOWER_OPCODE[OP7]();
}

#define T1_UPPER(HI_, OP6_, SUB_)                                                    \
    do { VU->code = (HI_); t1_upper_thunk<OP6_, SUB_>(VU); } while (0)

#define T1_LOWER(LO_, OP7_, OP6_, SUB_)                                              \
    do { VU->code = (LO_); t1_lower_thunk<OP7_, OP6_, SUB_>(VU); } while (0)

// ── [P116] 一致集合を u64 1 語から固定長ビットセットへ ────────────────────────────
// 旧実装は「一致した program 集合」を `u64` 1 語 (1 program = 1 bit) で持っていたため、
// **program は 64 本までという上限がビット幅だけの理由で存在した**
// (`iv2_t1_registry.inc` の static_assert = ビルド時ハードストップ)。
// 同一性判定 (t1_words_match) には一切関係しない純粋な容量制約なので、語数を増やして外す。
// ★ 意味は変わらない: どの program を採るかの条件は変更していない。
// ★ [P119] 4 → 12 語 (256 → 768 本)。God of War II の実戦闘は (位置, 本体) の相異なる組が
//   545 個あり、既存 GoW 1 と共有する 37 組を除いた 508 組 + コア 1 本を全部登録するには
//   registry 628 本が要る (256 では 100 本しか入らず被覆 94.32% で止まっていた)。
static constexpr int kT1MaskWords = 12;                // 64 * 12 = 768 program まで
struct IV2_T1Mask
{
    u64 w[kT1MaskWords];
    void clear()            { for (int i = 0; i < kT1MaskWords; ++i) w[i] = 0; }
    void set(int i)         { w[i >> 6] |= 1ull << (i & 63); }
    bool test(int i)  const { return ((w[i >> 6] >> (i & 63)) & 1) != 0; }
    bool any()        const { u64 o = 0; for (int i = 0; i < kT1MaskWords; ++i) o |= w[i]; return o != 0; }
    bool eq(const IV2_T1Mask& o) const
    {
        for (int i = 0; i < kT1MaskWords; ++i) if (w[i] != o.w[i]) return false;
        return true;
    }
};

// 変換済み program 1 本の記述子。生成 .inc がこの実体を 1 個ずつ吐く。
struct IV2_T1_Prog {
    u64 hash;
    u32 lo;                    // 変換窓の先頭 PC
    u32 n;                     // 窓の長さ
    const u32 (*words)[2];     // 起動時の image 照合用 {lower, upper}
    const u8*  have;           // 1 = 変換済み
    const u8  (*dec)[6];       // 生成時に畳んだ判定 (pipe/haz)
    _VURegsNum* u;             // 実表から充填する _VURegsNum
    _VURegsNum* l;
    void (*body)(VURegs*, u32, u32, u32);
    bool checked;
    bool ok;
};

// 変換済み program の登録表は **生成物** (Scripts/t1_registry_gen.py)。
// program 追加時にこのファイルを手で触る必要はない = 足し忘れ事故が構造的に起きない。
//
// ★ 登録表と iv2_t1_*.inc を同梱しないツリー (= 配布用ソース) では 0 本として組む。
//   kT1Progs を空配列にはできない (C++ は要素数 0 の配列を許さない) ので、本数を
//   別の定数に持たせ、走査側はその定数だけを見る。0 本なら T1_ProgramReady が即 false を
//   返すので、実行経路は kill-switch (iPSX2_VU1_T1=0) と同じ純インタプリタになる。
//   登録表が在るときは #else 側が消えるだけで、生成されるコードは一切変わらない
//   (iv2_vu_ops.o の SHA-256 が導入前後で一致することを確認済み)。
#if __has_include("iv2/iv2_t1_registry.inc")
#include "iv2/iv2_t1_registry.inc"
static constexpr int kT1ProgCount = (int)(sizeof(kT1Progs) / sizeof(kT1Progs[0]));
#else
static IV2_T1_Prog* const* const kT1Progs = nullptr;
static constexpr int kT1ProgCount = 0;
#endif

#undef T1_HEAD
#undef T1_PIPES
#undef T1_VIBACKUP
#undef T1_BRANCH_EPI
#undef T1_UPPER
#undef T1_LOWER

// ── entry gate / 検証 ───────────────────────────────────────────────────────────
// [2026-08-07] ★ 既定 ON に昇格 (GTA VC +14.0% bit-exact / 6-6 / p=0.016 / user 視覚 PASS)。
// kill-switch = `iPSX2_VU1_T1=0` で完全に旧経路 (= インタプリタのみ、A/B baseline・退行調査用)。
// 未登録 program / 検証不一致 / dirty pipe は元から常にインタプリタへ落ちる。
bool g_iv2_t1_enabled = []{
    const char* e = std::getenv("iPSX2_VU1_T1");
    return !(e && e[0] == '0' && e[1] == '\0');   // 既定 ON; "0" のみ OFF
}();
static const bool s_t1_env_on = g_iv2_t1_enabled;

static u64  s_t1_checked_hash = 0;
static bool s_t1_checked_ok   = false;

// [P101-18] image hash -> 一致した program 集合 (bit mask) の memo (direct-mapped)。
// 命令語照合は image が変わったときだけ走ればよい。GoW は 150 秒で 591 image を
// 循環するので、kick 毎に照合すると無駄 (照合自体は memcmp 相当だが kick は 4.5M 回)。
struct IV2_T1_Memo { u64 img; IV2_T1Mask mask; bool valid; };
static constexpr u32 kT1MemoSlots = 1024;
static IV2_T1_Memo s_t1_memo[kT1MemoSlots];
static u64 s_t1_last_hash = ~0ULL;

// [P101-18 複数 program 同時アクティブ] 1 つの image に **複数の変換 program が同時に一致する**
// ことがある。God of War は「共通コア 1 本 + 差し替えスロットの中身 N 本」という構造で、
// コアだけを選ぶと実行 ops の 44% しか覆えなかった (残りはスロット内 = オブジェクト別シェーダ)。
//
// 一致した program 全部をアクティブにし、**PC ごとにどの program が担当するかを 1 枚の表
// (pcmap) に畳む**。こうすると hot path (T1_TryRun) は「表を 1 回引く」だけで済み、
// program 本数に依存しない。表は image が変わったときだけ作り直す。
//
// 同じ PC を 2 本が変換していても意味は同一 (どちらも「その PC の命令語が live Micro と
// 一致すること」を検証済み = 同じ命令を同じ順で実行する)。先に登録された方を採る。
// ── ★ [P101-18 C] 被覆率ゲート (実測で導出、2026-08-07) ───────────────────────────
// T1 の収支は次のモデルで実測と合う (God of War 3 構成 + 作業量固定 A/B から連立):
//     利得 = I·s − (I+B)·d      s ≒ 9.3 ns/命令 (変換命令あたりの削減)
//                                d ≒ 1.9 ns/命令 (T1 判定そのものの税)
//   I = T1 が実行した命令数 / B = T1 有効下でインタプリタが実行した命令数。
// **税は変換していない命令にもかかる** (T1 ループは 1 命令ごとに T1_TryRun を通すため)。
// よって損益分岐は「連続長」ではなく **被覆率 d/s ≒ 21%**。
// 実測: 被覆 55%/66% の構成は +9.3%/+12.2%、被覆 13% の構成は **−0.5% (0/8, p=0.008)**。
//
// そこで active set ごとに最初の数十万命令で被覆率を測り、閾値未満なら **その active set では
// T1 を使わない** (= 素のインタプリタループへ)。T1 を使わない = 結果は同一なので bit-exact。
// 閾値は env `iPSX2_VU1_T1_MINCOV` (%、既定 25、0 でゲート無効 = 旧挙動)。
struct IV2_T1_Active {
    IV2_T1Mask mask;              // どの program が一致したか
    IV2_T1_Prog* progs[24];       // pcmap が指す実体 (1 image あたり最大 24 本まで畳む)
    int n;
    bool valid;
    bool probing;                 // 被覆率を測定中
    bool useful;                  // 測定の結果 T1 を使う価値があるか
    u32 cov, tot;                 // 測定中の 変換済み命令 / 全命令
    u8 pcmap[2048];               // PC -> progs[] の添字、0xff = 変換なし (インタプリタ)
};
static const u32 s_t1_mincov = []{ const char* e = std::getenv("iPSX2_VU1_T1_MINCOV");
                                   return e ? (u32)atoi(e) : 25u; }();
static constexpr u32 kT1ProbeInsns = 200000;   // この命令数で被覆率を判定する
// ★ [P116] distinct mask のキャッシュ本数。**128 では GoW で足りていなかった**:
//   591 image を循環する GoW の distinct active-set は実測で 213 個 (スロット窓 45 本のとき) /
//   349 個 (80 本のとき)。退避は LRU ではなく round-robin なので、128 だと毎回ミスして
//   ① pcmap[2048] の再構築が image 切替ごとに走り ② probing がリセットされ続けて
//   被覆率ゲートが完走しない (= T1 が無条件 ON) という 2 つの劣化が起きる。
//   1 スロット約 2.3 KB (.bss) なので 512 本で約 1.2 MB。
//   ★ [P119] 512 → 1024。**mask は「登録済み program のうち一致した集合」なので、
//   登録本数を増やすと distinct active-set も増える**。GoW II 実戦闘は 221 本の時点で
//   distinct active-set = 132 個 (thrash 無し) だったが、545 本体を全部登録すると
//   image ごとに (コア, 7 本体) の組が別 mask になるため distinct は image 数 895 に近づく。
//   1 スロット約 2.3 KB (.bss) なので 1024 本で約 2.4 MB。
static constexpr int kT1ActSlots = 1024;
static IV2_T1_Active s_t1_act[kT1ActSlots];
static int s_t1_act_next = 0;
static IV2_T1_Active* s_t1_curact = nullptr;

// mask に対応する pcmap を作る (mask ごとにキャッシュ。distinct mask はタイトルあたり数個)。
static IV2_T1_Active* t1_build_active(const IV2_T1Mask& mask)
{
    for (int i = 0; i < kT1ActSlots; ++i)
        if (s_t1_act[i].valid && s_t1_act[i].mask.eq(mask)) return &s_t1_act[i];
    IV2_T1_Active* a = &s_t1_act[s_t1_act_next];
    s_t1_act_next = (s_t1_act_next + 1) % kT1ActSlots;
    a->mask = mask; a->n = 0; a->valid = true;
    a->probing = (s_t1_mincov > 0); a->useful = true; a->cov = a->tot = 0;
    std::memset(a->pcmap, 0xff, sizeof(a->pcmap));
    const int nprog = kT1ProgCount;
    int matched = 0;
    for (int i = 0; i < nprog; ++i)
    {
        if (!mask.test(i)) continue;
        ++matched;
        // ★ [P116] progs[] は 1 image あたり 24 本まで。**旧実装は超過を無言で捨てていた**
        //   (bit-exact のまま被覆率だけ落ちるので誰も気づかない)。構造上の上界は
        //   「コア 1 + 尾部 1 + スロット 7 = 9 本」だが、増やしたときに気づけるよう明示する。
        if (a->n >= 24) continue;
        IV2_T1_Prog* p = kT1Progs[i];
        const u8 k = (u8)a->n;
        a->progs[a->n++] = p;
        for (u32 j = 0; j < p->n; ++j) {
            const u32 pc = p->lo + j;
            if (p->have[j] && pc < 2048 && a->pcmap[pc] == 0xff) a->pcmap[pc] = k;
        }
    }
    if (__builtin_expect(matched > a->n, 0))
        Console.WriteLn("@@IV2_T1@@ ★ progs[] 上限 24 で %d 本を捨てた (一致 %d 本)。"
                        "被覆率が落ちるので上限を上げること", matched - a->n, matched);
    return a;
}

// [P101-18] 変換窓の命令語が **今の VU1.Micro と一致するか** だけを見る (高速判定)。
// これが T1 の正しさの条件そのもの: 変換していない PC はインタプリタが処理するので、
// 16KB のうち窓の外が何であっても影響しない。
static bool t1_words_match(const IV2_T1_Prog& p)
{
    for (u32 i = 0; i < p.n; ++i)
    {
        if (!p.have[i]) continue;
        const u32* m = (const u32*)&VU1.Micro[(p.lo + i) * 8];
        if (m[0] != p.words[i][0] || m[1] != p.words[i][1]) return false;
    }
    return true;
}

// 生成時の前提を実行時の実表と突き合わせる。1 つでも違えば false = T1 使用禁止。
// decode (pipe/hazard) は **命令語だけの純関数**なので、命令語一致を確認済みなら
// この検証は program につき生涯 1 回でよい (t1_words_match が毎 image チェックする)。
static bool t1_verify_program(IV2_T1_Prog& p)
{
    const u32 saved = VU1.code;
    bool ok = true;
    for (u32 i = 0; i < p.n; ++i)
    {
        if (!p.have[i]) continue;
        const u32* m = (const u32*)&VU1.Micro[(p.lo + i) * 8];
        if (m[0] != p.words[i][0] || m[1] != p.words[i][1]) { ok = false; break; }

        _VURegsNum u{}, l{};
        VU1.code = m[1]; VU1regs_UPPER_OPCODE[m[1] & 0x3f](&u);
        VU1.code = m[0]; VU1regs_LOWER_OPCODE[m[0] >> 25](&l);

        u8 discard = 0, vfreg = 0, vireg = 0;
        if (u.VFwrite) {
            if (l.VFwrite == u.VFwrite) discard = 1;
            if (l.VFread0 == u.VFwrite || l.VFread1 == u.VFwrite) vfreg = u.VFwrite;
        }
        if (u.VIwrite & (1u << REG_CLIP_FLAG)) {
            if (l.VIwrite & (1u << REG_CLIP_FLAG)) discard = 1;
            if (l.VIread & (1u << REG_CLIP_FLAG)) vireg = REG_CLIP_FLAG;
        }
        const u8 fast = (u.VFwrite == 0 && (u.VIwrite & (1u << REG_CLIP_FLAG)) == 0) ? 1 : 0;

        const u8* d = p.dec[i];
        if (d[0] != u.pipe || d[1] != l.pipe || d[2] != fast ||
            d[3] != discard || d[4] != vfreg || d[5] != vireg) { ok = false; break; }

        p.u[i] = u;
        p.l[i] = l;
    }
    VU1.code = saved;
    return ok;
}

bool T1_ProgramReady(u64 hash)
{
    // 変換済み program を 1 本も同梱しない構成 (配布用ソース) では、ここから先は無意味。
    // 登録表が在るビルドでは discarded statement なので codegen に現れない。
    if constexpr (kT1ProgCount == 0) return false;
    if (__builtin_expect(!s_t1_env_on, 1)) return false;
    // 変換時に前提した実行環境か (どれかが崩れていれば畳み込みの前提が壊れる)
    if (!(g_iv2vu_upper_en && g_iv2vu_lower_en && !g_iv2vu_diff_en &&
          s_lower_mask == 0x7Fu && !s_lower_fb_prof_enabled &&
          g_iv2_exec_plan_enabled && !g_iv2_vu1_on_worker))
        return false;
    // [2026-08-07] program は kick 毎に変わらないことが大半なので、直前の解決結果を再利用する。
    // registry の線形探索と gate 再評価を kick 毎に回さない (J-Phoenix は 4.3M kicks/60s で
    // `CpuIv2VU1::ExecuteSync` が profile 6.58% = per-kick 前処理が無視できない)。
    if (__builtin_expect(hash == s_t1_last_hash, 1))
        return s_t1_curact != nullptr;

    // ── [P101-18] ★ 同一性判定を「16KB 全体 hash 一致」から「変換窓の命令語一致」へ ──
    // 16KB 全体 hash はゲームが micro memory の **一部だけ** を差し替えると変わる。
    // God of War は共通コア 1659 命令 + 106 命令スロット 7 本の差し替え構造で、
    // 同じコアが 145 通りの 16KB hash に散っていた (= T1 が一度も発火しない原因)。
    // T1 の正しさは「変換した PC の命令語が一致していること」だけで決まる (変換外の PC は
    // インタプリタが live な Micro を読む) ので、これが正しい粒度。
    // 16KB hash は **memo のキー** としてのみ使う (image が変わらない限り再判定しない)。
    s_t1_last_hash = hash;
    const u32 slot = (u32)((hash ^ (hash >> 29)) & (kT1MemoSlots - 1));
    IV2_T1Mask mask;
    if (s_t1_memo[slot].img == hash && s_t1_memo[slot].valid) {
        mask = s_t1_memo[slot].mask;
    } else {
        mask.clear();
        const int nprog = kT1ProgCount;
        for (int i = 0; i < nprog; ++i)
        {
            IV2_T1_Prog* p = kT1Progs[i];
            if (!t1_words_match(*p)) continue;      // 窓の命令語が違う = この program ではない
            if (!p->checked) {                      // decode 検証は命令語の純関数なので生涯 1 回
                p->checked = true;
                p->ok = t1_verify_program(*p);
                Console.WriteLn("@@IV2_T1@@ program %016llx verify=%s (pc %u..%u, img hash %016llx)",
                                (unsigned long long)p->hash, p->ok ? "OK" : "MISMATCH->disabled",
                                p->lo, p->lo + p->n - 1, (unsigned long long)hash);
            }
            if (p->ok) mask.set(i);
        }
        s_t1_memo[slot].img = hash;
        s_t1_memo[slot].mask = mask;
        s_t1_memo[slot].valid = true;
    }
    s_t1_curact = mask.any() ? t1_build_active(mask) : nullptr;
    // 被覆率が損益分岐を割る active set では T1 を使わない (= 素のインタプリタへ)。
    return s_t1_curact != nullptr && (s_t1_curact->probing || s_t1_curact->useful);
}

// [P101-18 A] 償却の実測 (既定 OFF)。
bool g_t1_stats_on = []{ const char* e = std::getenv("iPSX2_VU1_T1_STATS"); return e && e[0] == '1'; }();
u64 g_t1_entries = 0, g_t1_insns = 0, g_t1_bail_insns = 0;
void T1_StatsDump()
{
    if (!g_t1_stats_on) return;
    const double run = g_t1_entries ? (double)g_t1_insns / (double)g_t1_entries : 0.0;
    const u64 tot = g_t1_insns + g_t1_bail_insns;
    Console.WriteLn("@@IV2_T1_STATS@@ entries=%llu t1_insns=%llu bail_insns=%llu "
                    "mean_run=%.2f t1_share=%.2f%%",
                    (unsigned long long)g_t1_entries, (unsigned long long)g_t1_insns,
                    (unsigned long long)g_t1_bail_insns, run,
                    tot ? 100.0 * (double)g_t1_insns / (double)tot : 0.0);
}

bool T1_TryRun(VURegs* VU, u32 tpc, u32 startcycles, u32 cycles)
{
    IV2_T1_Active* const a = s_t1_curact;
    const u32 pc = (tpc >> 3) & 0x7ffu;
    const u8 k = a->pcmap[pc];
    // 被覆率の測定 (この active set の最初の kT1ProbeInsns 命令だけ)。
    if (__builtin_expect(a->probing, 0)) {
        ++a->tot;
        if (k != 0xff) ++a->cov;
        if (a->tot >= kT1ProbeInsns) {
            a->probing = false;
            a->useful = ((u64)a->cov * 100u >= (u64)a->tot * s_t1_mincov);
            // ★ [P119] 語数に追随させる (旧実装は w[3..0] を直書きしていたので、
            //   kT1MaskWords を増やすと上位語が黙って落ちて別 mask が同じ表示になっていた)。
            char mbuf[kT1MaskWords * 16 + 1];
            for (int i = 0; i < kT1MaskWords; ++i)
                snprintf(mbuf + i * 16, 17, "%016llx",
                         (unsigned long long)a->mask.w[kT1MaskWords - 1 - i]);
            Console.WriteLn("@@IV2_T1@@ active-set n=%d mask=%s 被覆=%.1f%% -> %s",
                            a->n, mbuf,
                            100.0 * (double)a->cov / (double)a->tot,
                            a->useful ? "T1 使用" : "T1 不使用 (損益分岐割れ)");
        }
    }
    if (k == 0xff) return false;                // この PC は変換していない = インタプリタへ
    // 保留中の分岐 / E-bit がある状態では入らない (epilogue を畳み込んであるため)
    if (VU->branch != 0 || VU->ebit != 0) return false;
    const IV2_T1_Prog* const p = a->progs[k];
    if (__builtin_expect(g_t1_stats_on, 0)) {
        const u32 c0 = VU1.cycle;               // T1_HEAD が 1 命令ごとに cycle++ する
        p->body(VU, startcycles, cycles, pc - p->lo);
        ++g_t1_entries; g_t1_insns += (u64)(VU1.cycle - c0);
        if ((g_t1_entries & 0x3fffff) == 0) T1_StatsDump();
        return true;
    }
    p->body(VU, startcycles, cycles, pc - p->lo);
    return true;
}

}} // namespace iv2::vu_ops
