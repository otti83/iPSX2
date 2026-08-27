// SPDX-License-Identifier: GPL-3.0+
// P101: iv2 — 独自実行ループ (Phase B)
//
// Phase B: 独自の fastjmp_buf を使った execute ループ + ブロックキャッシュ。
// 各ブロック実行は事前にデコードした OPCODE* を使い、
// memRead32 + GetInstruction を 1 ブロック 1 回に圧縮する。

#include "PrecompiledHeader.h"
#include "Common.h"
#include "iv2/iv2.h"
#include "iv2/iv2_block.h"
#include "iv2/iv2_async_vu1.h"   // [P101-4 Stage C] async VU1 deferred-effect drain
#include "iv2/iv2_tlb_l0.h"
#include "common/FPControl.h"   // [P114] DIV/SQRT の丸めモード

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#include <algorithm>
#include <vector>
#include <cstring>
#endif
#include "R5900.h"
#include "R5900OpcodeTables.h"
#include "VMManager.h"
#include "VUmicro.h"       // _vu0FinishMicro
#include "Elfheader.h"
#include "Memory.h"
#include "iv2/iv2_vu_ops.h"  // iv2::vu_ops::UpperDispatch

#include "common/FastJmp.h"

using namespace R5900;

// Interpreter.cpp より
extern u32 g_eeloadMain, g_eeloadExec;
extern u32 cpuBlockCycles;   // [P101] Interpreter.cpp 側で extern 化済み
extern u32 intLastBranchTo;  // [P101] WaitLoop 検出用
extern void eeloadHook();
extern void eeloadHook2();
extern "C" void iv2BranchFinalize_Taken(u32 target);  // [P101] Interpreter.cpp 側
extern void intEventTest();                           // Interpreter.cpp 側 (namespace 外)
extern void intEventTestGated();                      // [EE event-test gate 2026-06-07] gate 付き intEventTest
extern void intUpdateCPUCycles();                     // [iv2 fix 2026-05-30] not-taken branch cycle flush 用
// EELOAD 定数は R5900.h に宣言済み (EEKERNEL_START/EELOAD_START/...)
// eeEventTestIsActive は R5900.h で namespace 外に宣言済み
extern bool eeEventTestIsActive;

namespace iv2 {

// ───────────────────────────────────────────────────────────────
// 状態
// ───────────────────────────────────────────────────────────────
static fastjmp_buf s_jmp;
static bool s_exit_requested = false;
// cpuBlockCycles は Interpreter.cpp で extern 化されたグローバル
static bool s_initialized = false;

// ───────────────────────────────────────────────────────────────
// [2026-08-06 P101-14] Compile-time gate for the PER-INSTRUCTION diagnostics in
// run_block (op histogram, Fallback opcode buckets, MMI self-check).
//
// Those are runtime `bool` globals, and run_block calls `c.handler()` (an
// indirect call) every iteration, so the compiler must RELOAD both globals and
// re-test them for every emulated EE instruction — plus the MMI self-check
// costs an `sc_on=false` store and a trailing test per instruction, whether or
// not the op is an MMI op. Measured 2026-08-06 (GTA VC sustained 3D, leaf
// self-time via /usr/bin/sample): `iv2::Execute` = 26.2% of the CPU thread and
// EE total ≈ 33%, i.e. roughly double the 15-20% assumed by earlier reports, so
// the per-op skeleton is worth cleaning.
//
// Block-level statistics (once per block) stay unconditional — they are cheap.
// Removal condition (CLAUDE.md rule 3): these are debug tools, so they are
// gated, not deleted. Rebuild with -DIV2_EE_HOT_DIAG=1 to restore
// iPSX2_IV2_PROFILE's per-op histogram and iPSX2_IV2_MMI_SELFCHECK verbatim.
// Same shape as IV2_ENABLE_ORCH_PROF / IV2_VU_HOT_DIAG in iv2_vu.cpp.
#ifndef IV2_EE_HOT_DIAG
#define IV2_EE_HOT_DIAG 0
#endif

// ───────────────────────────────────────────────────────────────
// [P101-3R] 計測強化 — PROFILE env gate で有効化
// 全て EE 実行 thread 上でのみ更新 (単一スレッド) のため非 atomic plain u64。
// ───────────────────────────────────────────────────────────────
bool g_iv2_profile_enabled = false;
static bool s_vu0_diff_enabled = false;
// [iv2 MMI self-check 2026-06-01] =1 で MMI native op の結果を同入力で interpreter reference と
// 比較し不一致を @@QA_MMICK@@ で log + reference 値を採用 (= 正しい結果で続行)。
// determinism 非依存で「どの MMI op が誤変換か」を確定する。env iPSX2_IV2_MMI_SELFCHECK。
bool g_iv2_mmi_selfcheck = false;

// Op_* ごとの dynamic 実行回数 (Op_COUNT = 139)
static u64 s_op_count[256];
// Fallback の primary opcode bucket (raw >> 26, 0-63)
static u64 s_fb_primary[64];
// SPECIAL (primary 0) funct bucket (raw & 0x3F, 0-63)
static u64 s_fb_special[64];
// COP0 (primary 0x10) rs bucket (0-31)
static u64 s_fb_cop0_rs[32];
// COP1 (primary 0x11) rs bucket
static u64 s_fb_cop1_rs[32];
// COP2 (primary 0x12) rs bucket (0-31)
static u64 s_fb_cop2_rs[32];
// COP2 (primary 0x12) funct bucket (for rs >= 0x10 = VU macro op、funct 0-63)
static u64 s_fb_cop2_funct[64];
// Block-level 集計
static u64 s_block_total = 0;
static u64 s_block_inst_total = 0;
// Branch link hit rate (taken/not_taken/miss)
static u64 s_link_taken_hit = 0;
static u64 s_link_not_taken_hit = 0;
static u64 s_link_miss = 0;
// Event test fired
static u64 s_evt_fired = 0;

static void iv2_profile_summary_dump()
{
    // 直接 stderr へ (Console.Error が allowlist/verbose で抑制される可能性回避)
    fprintf(stderr, "@@IV2_PROFILE@@ blocks=%llu avg_inst=%.2f evt=%llu link_t=%llu link_nt=%llu link_miss=%llu\n",
        (unsigned long long)s_block_total,
        s_block_total ? (double)s_block_inst_total / (double)s_block_total : 0.0,
        (unsigned long long)s_evt_fired,
        (unsigned long long)s_link_taken_hit,
        (unsigned long long)s_link_not_taken_hit,
        (unsigned long long)s_link_miss);
    // [P101-14 E5-probe] EE 再コンパイル 1.9% の内訳: pool 枯渇 vs Cpu->Clear
    fprintf(stderr, "@@IV2_INVAL@@ compiles=%llu inval_pool=%llu inval_clear=%llu/%llu clear_insts=%llu "
                    "pool_next=%u/%u inst_next=%u/%u gen=%u\n",
        (unsigned long long)g_iv2_compiles, (unsigned long long)g_iv2_inval_pool,
        (unsigned long long)g_iv2_inval_clear, (unsigned long long)g_iv2_inval_clear_calls, (unsigned long long)g_iv2_clear_insts,
        g_block_pool_next, g_block_pool_cap, g_inst_pool_next, INST_POOL_CAP, (unsigned)g_generation);
    Console.Error("@@IV2_INVAL@@ compiles=%llu inval_pool=%llu inval_clear=%llu/%llu clear_insts=%llu "
                  "pool_next=%u/%u inst_next=%u/%u gen=%u",
        (unsigned long long)g_iv2_compiles, (unsigned long long)g_iv2_inval_pool,
        (unsigned long long)g_iv2_inval_clear, (unsigned long long)g_iv2_inval_clear_calls, (unsigned long long)g_iv2_clear_insts,
        g_block_pool_next, g_block_pool_cap, g_inst_pool_next, INST_POOL_CAP, (unsigned)g_generation);
    Console.Error("@@IV2_PROFILE@@ blocks=%llu avg_inst=%.2f evt=%llu link_t=%llu link_nt=%llu link_miss=%llu",
        (unsigned long long)s_block_total,
        s_block_total ? (double)s_block_inst_total / (double)s_block_total : 0.0,
        (unsigned long long)s_evt_fired,
        (unsigned long long)s_link_taken_hit,
        (unsigned long long)s_link_not_taken_hit,
        (unsigned long long)s_link_miss);
    // Top 10 op usage
    u64 total_ops = 0;
    for (int i = 0; i < 256; i++) total_ops += s_op_count[i];
    int idx[256];
    for (int i = 0; i < 256; i++) idx[i] = i;
    std::sort(idx, idx + 256, [](int a, int b) { return s_op_count[a] > s_op_count[b]; });
    Console.Error("@@IV2_PROFILE@@ total_ops=%llu", (unsigned long long)total_ops);
    for (int rank = 0; rank < 10; rank++) {
        const int i = idx[rank];
        if (s_op_count[i] == 0) break;
        Console.Error("@@IV2_PROFILE@@ op#%d count=%llu pct=%.2f%%",
            i, (unsigned long long)s_op_count[i],
            total_ops ? 100.0 * s_op_count[i] / total_ops : 0.0);
    }
    // Top Fallback buckets
    u64 total_fb = 0;
    for (int i = 0; i < 64; i++) total_fb += s_fb_primary[i];
    Console.Error("@@IV2_PROFILE@@ fallback_total=%llu", (unsigned long long)total_fb);
    for (int i = 0; i < 64; i++) {
        if (s_fb_primary[i] >= total_fb / 100 && s_fb_primary[i] > 0) {
            Console.Error("@@IV2_PROFILE@@ fb_primary=0x%02x count=%llu pct=%.2f%%",
                i, (unsigned long long)s_fb_primary[i],
                total_fb ? 100.0 * s_fb_primary[i] / total_fb : 0.0);
        }
    }
    for (int i = 0; i < 64; i++) {
        if (s_fb_special[i] > 0 && s_fb_special[i] >= total_fb / 200) {
            Console.Error("@@IV2_PROFILE@@ fb_special=0x%02x count=%llu",
                i, (unsigned long long)s_fb_special[i]);
        }
    }
    for (int i = 0; i < 32; i++) {
        if (s_fb_cop0_rs[i] > 0) {
            Console.Error("@@IV2_PROFILE@@ fb_cop0_rs=0x%02x count=%llu",
                i, (unsigned long long)s_fb_cop0_rs[i]);
        }
    }
    for (int i = 0; i < 32; i++) {
        if (s_fb_cop1_rs[i] > 0) {
            Console.Error("@@IV2_PROFILE@@ fb_cop1_rs=0x%02x count=%llu",
                i, (unsigned long long)s_fb_cop1_rs[i]);
        }
    }
    // COP2 (primary 0x12) breakdown — Console.Error 統一
    for (int i = 0; i < 32; i++) {
        if (s_fb_cop2_rs[i] > 0) {
            Console.Error("@@IV2_PROFILE@@ fb_cop2_rs=0x%02x count=%llu",
                i, (unsigned long long)s_fb_cop2_rs[i]);
        }
    }
    for (int i = 0; i < 64; i++) {
        if (s_fb_cop2_funct[i] > 0) {
            Console.Error("@@IV2_PROFILE@@ fb_cop2_funct=0x%02x count=%llu",
                i, (unsigned long long)s_fb_cop2_funct[i]);
        }
    }
}

// [P101] Phase E: Branch 3-op 間の状態受渡し
static u32 s_branch_target = 0;
static u8  s_branch_taken  = 0;
// [iv2 fix 2026-05-30] intCpu は BEQ/BNE の not-taken のみ intEventTest を呼ぶ。
// BLEZ/BGTZ/BLTZ/BGEZ/BLTZAL/BGEZAL/BC1F/BC1T の not-taken は呼ばない。BCheck op が設定。
static u8  s_branch_evt_nt = 0;

// [P101 Phase H] ブロックリンク用: BFinalize/JumpConst が link_taken/link_not_taken のどちらに
// 書き込むかを示すスロットポインタ。nullptr なら今回のブロック末尾では link しない (JumpReg等)。
static Block** s_link_slot = nullptr;

// 現在実行中のブロック。run_block / BFinalize から参照。
static Block* current_block = nullptr;

// ─── Phase F+: FPU クランプ関連ヘルパー (FPU.cpp と同一ロジック) ─────
static constexpr u32 FPUflagC = 0x00800000u;
static constexpr u32 FPUflagO = 0x00008000u;
static constexpr u32 FPUflagU = 0x00004000u;
static constexpr u32 FPUflagSO = 0x00000010u;
static constexpr u32 FPUflagSU = 0x00000008u;
static constexpr u32 FPUflagSI = 0x00000040u;
static constexpr u32 FPUflagSD = 0x00000020u;
static constexpr u32 FPUflagI  = 0x00000020u;    // FPUflagI だが PS2 の I フラグ位置
static constexpr u32 FPUflagD  = 0x00000010u;
static constexpr u32 PosInfinity = 0x7F800000u;
static constexpr u32 posFmax = 0x7F7FFFFFu;

// [P114 2026-08-08] PS2 の EE FPU は **除算系だけ丸めモードが違う** (DIV/SQRT/RSQRT = 最近接、
// 他 = 0 方向切り捨て)。EE 再コンパイラは iFPU.cpp:1721 / iFPUd.cpp:836 で FPCR を
// EmuConfig.Cpu.FPUDivFPCR に差し替えているが、interp / iv2 にはそれが無く常時 ChopZero で
// 割っていた。結果が 1 ULP 小さくなり、CUC (SLPS-25627) ではフェードイン用の float が進まず
// 画面が α=127 の黒で潰れたままになる。上流 v2.3.432 の EE interp でも同症状を再現できる。
// 詳細と検証 = docs/report/P114_...。FPCR 入れ替えは高価なので、再コンパイラと同じく
// 「2 つの FPCR が実際に違うときだけ」差し替える。
namespace
{
	class Iv2FpuDivRoundScope
	{
	public:
		__fi Iv2FpuDivRoundScope()
			: m_active(EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		{
			if (m_active)
			{
				m_prev = FPControlRegister::GetCurrent();
				FPControlRegister::SetCurrent(EmuConfig.Cpu.FPUDivFPCR);
				// FPControlRegister::SetCurrent の `msr FPCR` は memory clobber を持たないので、
				// LTO/最適化で後続の浮動小数演算が msr より前へ動かされ得る。明示的に固定する。
				asm volatile("" ::: "memory");
			}
		}
		__fi ~Iv2FpuDivRoundScope()
		{
			if (m_active)
			{
				asm volatile("" ::: "memory");
				FPControlRegister::SetCurrent(m_prev);
			}
		}
		Iv2FpuDivRoundScope(const Iv2FpuDivRoundScope&) = delete;
		Iv2FpuDivRoundScope& operator=(const Iv2FpuDivRoundScope&) = delete;

	private:
		bool m_active;
		FPControlRegister m_prev{};
	};
} // namespace

// [P114] `msr FPCR` の直後に浮動小数演算を確実に置くための固定子。
// memory clobber だけでは足りない: fdiv はレジスタ演算なので、オペランドが既にレジスタに
// 載っていると最適化で msr より前へ巻き上げられる (iv2 で実際に発生し、FPCR を切り替えても
// 結果が切り捨てのままだった)。オペランドと結果を FP レジスタ制約で洗って依存を作る。
#if defined(__aarch64__) || defined(_M_ARM64)
#define P114_FP_LAUNDER(x) __asm__ __volatile__("" : "+w"(x))
#else
#define P114_FP_LAUNDER(x) __asm__ __volatile__("" : "+x"(x))
#endif

static __attribute__((always_inline)) float iv2_fpuDouble(u32 f)
{
    switch (f & 0x7f800000u) {
    case 0x0: {
        u32 masked = f & 0x80000000u;
        float r; std::memcpy(&r, &masked, 4); return r;
    }
    case 0x7f800000u: {
        u32 masked = (f & 0x80000000u) | 0x7f7fffffu;
        float r; std::memcpy(&r, &masked, 4); return r;
    }
    default: {
        float r; std::memcpy(&r, &f, 4); return r;
    }
    }
}

static __attribute__((always_inline)) bool iv2_checkOverflow(u32& xReg, u32 cFlagsToSet)
{
    if ((xReg & ~0x80000000u) == PosInfinity) {
        xReg = (xReg & 0x80000000u) | posFmax;
        fpuRegs.fprc[31] |= cFlagsToSet;
        return true;
    } else if (cFlagsToSet & FPUflagO) {
        fpuRegs.fprc[31] &= ~FPUflagO;
    }
    return false;
}

static __attribute__((always_inline)) bool iv2_checkUnderflow(u32& xReg, u32 cFlagsToSet)
{
    if ((xReg & 0x7F800000u) == 0 && (xReg & 0x007FFFFFu) != 0) {
        xReg &= 0x80000000u;
        fpuRegs.fprc[31] |= cFlagsToSet;
        return true;
    } else if (cFlagsToSet & FPUflagU) {
        fpuRegs.fprc[31] &= ~FPUflagU;
    }
    return false;
}

// result (float) を ULに反映 + overflow/underflow チェック
static __attribute__((always_inline)) void iv2_fpu_store_result(u8 dest, float result,
    u32 cFlagsOvf, u32 cFlagsUnf)
{
    u32 ul; std::memcpy(&ul, &result, 4);
    if (!iv2_checkOverflow(ul, cFlagsOvf))
        iv2_checkUnderflow(ul, cFlagsUnf);
    fpuRegs.fpr[dest].UL = ul;
}

static __attribute__((always_inline)) void iv2_fpu_store_acc(float result,
    u32 cFlagsOvf, u32 cFlagsUnf)
{
    u32 ul; std::memcpy(&ul, &result, 4);
    if (!iv2_checkOverflow(ul, cFlagsOvf))
        iv2_checkUnderflow(ul, cFlagsUnf);
    fpuRegs.ACC.UL = ul;
}

// ベンチマーク
static u64 s_bench_inst_count = 0;
static u64 s_bench_block_count = 0;

// 前方宣言
static void update_cycles_and_event_test();

// ───────────────────────────────────────────────────────────────
// Init / Reset
// ───────────────────────────────────────────────────────────────
void Init()
{
    if (s_initialized) return;
    s_initialized = true;

    // プール初期化
    g_generation = 1;
    g_block_pool_next = 0;
    g_inst_pool_next = 0;
    std::memset(g_block_map, 0, sizeof(g_block_map));
    FlushTlbL0();

    // [P101-3R] env gates
    g_iv2_profile_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_IV2_PROFILE", false);
    if (g_iv2_profile_enabled) std::memset(s_op_count, 0, sizeof(s_op_count));
    g_iv2_mmi_selfcheck = iPSX2_GetRuntimeEnvBool("iPSX2_IV2_MMI_SELFCHECK", false);

    // [P101-14 E6] EE Clear の範囲判定 kill-switch。`iPSX2_EE_CLEAR_RANGE=0` で旧挙動
    // (常に全ブロック無効化 + TLB L0 全 flush)。同一バイナリ A/B 用。
    g_ee_clear_range = iPSX2_GetRuntimeEnvBool("iPSX2_EE_CLEAR_RANGE", true);
    // [P101-14 R1] compile 時 Nop 消去。`iPSX2_EE_NOP_COMPACT=0` で旧挙動 (実行時合体スキャン)。
    g_ee_nop_compact = iPSX2_GetRuntimeEnvBool("iPSX2_EE_NOP_COMPACT", true);
    Console.Error("@@IV2_CLEAR_RANGE@@ enabled=%d nop_compact=%d",
        g_ee_clear_range ? 1 : 0, g_ee_nop_compact ? 1 : 0);

    // [P101-14 E5] block pool 実効 cap の kill-switch (同一バイナリ A/B 用)。
    // 既定 = BLOCK_POOL_CAP (65536)、`iPSX2_EE_BLOCK_POOL=16384` で旧挙動。
    {
        const char* s = iPSX2_GetRuntimeEnv("iPSX2_EE_BLOCK_POOL");
        const long v = s ? strtol(s, nullptr, 10) : 0;
        g_block_pool_cap = (v > 0 && (u32)v <= BLOCK_POOL_CAP) ? (u32)v : BLOCK_POOL_CAP;
        Console.Error("@@IV2_BLOCK_POOL@@ cap=%u (max=%u)", g_block_pool_cap, BLOCK_POOL_CAP);
    }

    Console.Error("@@IV2_INIT@@ Interpreter v2 initialized (Phase D: block+tlb_l0, hash=%u, pool=%u insts)",
        BLOCK_HASH_SIZE, INST_POOL_CAP);
    Console.Error("@@IV2_P101_3R@@ profile=%d", g_iv2_profile_enabled ? 1 : 0);
}

void Shutdown()
{
    s_initialized = false;
}

void Reset()
{
    InvalidateAll();
    FlushTlbL0();
    s_exit_requested = false;
    cpuBlockCycles = 0;
    s_bench_inst_count = 0;
    s_bench_block_count = 0;
}

void Clear(u32 addr, u32 size_insts)
{
    // 既存 intClear は no-op。ブロックキャッシュが新設されたので無効化が必要。
    // size_insts は命令数 (not bytes)。
    //
    // [P101-14 E6 2026-08-06] **範囲判定を入れる**。
    //   probe 実測 (@@IV2_INVAL@@, GTA VC 定常 3D 94 秒): pool 枯渇 0 回に対し
    //   Clear 由来の全無効化が 62,532 回 (≈325 回/秒)、その結果 8,744,823 回の再コンパイル
    //   (≈93,000 回/秒、1 Clear あたり約 140 ブロック)。ブロックキャッシュは 232 個までしか育たない。
    //   = leaf profile の EE 再コンパイル 1.94% はすべてここが原因。
    //   IOP 側で同型の変更が -4.76% だったのは「呼び手が全 RAM ストア」= 判定が高すぎたからで、
    //   EE は呼び手が毎秒 325 回しかないので判定コストは無視できる (トレードが逆向き)。
    //
    // ★ FlushTlbL0 はここから外した。L0 は vtlbdata.vmap[] のキャッシュであり、
    //   その 3 つの書き手 (vtlb_VMap / vtlb_VMapBuffer / vtlb_VMapUnmap) 側で flush する
    //   (vtlb.cpp)。従来は COP0 TLB 書換えが Cpu->Clear を呼ぶことに依存した暗黙結合で、
    //   ここで早期 return すると **ログに何も出ずに壊れる** 経路だった。
    //
    // kill-switch: `iPSX2_EE_CLEAR_RANGE=0` で旧挙動 (常に全無効化 + L0 flush)。
    ++g_iv2_inval_clear_calls;
    g_iv2_clear_insts += size_insts;

    if (__builtin_expect(g_ee_clear_range, 1)) {
        if (!CodePagesOverlap(addr, size_insts * 4))
            return;                       // 消す範囲にコードが無い = 何もしない
    } else {
        FlushTlbL0();                     // 旧挙動 (A/B 用)
    }

    ++g_iv2_inval_clear;
    InvalidateAll();
    s_bench_block_count = 0;
}

void SafeExitExecution()
{
    // intCpu の intSafeExitExecution と同等:
    //   eeEventTest 中なら次の境界で抜ける
    //   そうでなければ即座に longjmp
    if (::eeEventTestIsActive) {
        s_exit_requested = true;
    } else {
        fastjmp_jmp(&s_jmp, 1);
    }
}

void CancelInstruction()
{
    fastjmp_jmp(&s_jmp, 0);
}

// ───────────────────────────────────────────────────────────────
// 1 ブロック実行 (Phase C: native IR dispatch)
// ───────────────────────────────────────────────────────────────
[[gnu::hot]] static __attribute__((always_inline)) void run_block(const Block* blk)
{
    const CachedInst* inst = &g_inst_pool[blk->inst_offset];
    const u32 n = blk->inst_count;

    // [iv2 per-inst cycle fix v2 2026-05-31] ★ 構造的整合 ★。
    // 旧: block 先頭で guest_cycles_sum 一括 charge。intCpu (Interpreter.cpp:236 で命令毎に
    // opcode.interpret() *前* に charge) と flush 境界をまたぐと cycle 会計が乖離 → 例外ハンドラ等で
    // +cycle drift → EE↔IOP catchup desync → SIF poll freeze。
    // 新: loop 内で命令実行 *直前* に c.cycles を charge (intCpu と同一順序・同一粒度)。
    // up-front charge は廃止。Op_Nop coalescing で skip する分も明示 charge (下記)。
    // block 内 Config 不変 (MTC0 block 分割済) なので cycle_mul は先頭読みで bit-exact。

    GPR_reg* const gpr = cpuRegs.GPR.r;

    // [P101-3R PROFILE] block 統計 (cheap: once per block)
    if (__builtin_expect(g_iv2_profile_enabled, 0)) {
        s_block_total++;
        s_block_inst_total += n;
        // 定期的に dump (2^18 block = ~40k blocks/frame なので数秒毎に 1 回)
        if ((s_block_total & 0x3FFFF) == 0)
            iv2_profile_summary_dump();
    }

    for (u32 i = 0; i < n; i++) {
        const CachedInst& c = inst[i];
        // pc 前進は全 op 共通。Fallback のブランチハンドラは内部で
        // 必要な pc 操作を行うため、ここで進めても破綻しない (execI と同じ挙動)。
        // [P101-14 R1 2026-08-06] compile 時に消した Nop の分がマージされているので +4 固定でなく pc_adv。
        cpuRegs.pc += c.pc_adv;

        // [iv2 per-inst cycle fix v3 2026-05-31] 命令実行 *直前* に charge。
        // ★ cycle_mul を命令ごとに CP0.Config から読む (block 先頭固定をやめる) ★。
        // intCpu (Interpreter.cpp:265 `opcode.cycles * (2-((Config>>18)&1))` を毎命令) と完全一致。
        // EE JIT も同じ GTA VC タイミング問題を src24→src25 で per-inst Config 読み
        // (armEmitPerInstCycleAdd_runtime: 毎命令 CP0.r[16]>>18&1 を load) に変えて修正済。
        // block 先頭固定だと、例外/割込みハンドラ突入 (cpuException pc 変更) で Config(ICache
        // isolate bit18) が変化した際に古い mul のまま charge → +cycle drift → SIF freeze。
        {
            const u32 pi_cycle_mul = 2u - ((cpuRegs.CP0.n.Config >> 18) & 0x1u);
            cpuBlockCycles += (u32)c.cycles * pi_cycle_mul;
        }

#if IV2_EE_HOT_DIAG
        // [P101-3R PROFILE] Op ごとの呼出回数
        if (__builtin_expect(g_iv2_profile_enabled, 0)) {
            s_op_count[c.op]++;
        }

        // [iv2 MMI self-check 2026-06-01] MMI native op (Op_Pand..Pcgtw / Op_Pmaxh..Pcpyud) の
        // 結果を同入力で interpreter reference と比較するため、実行前に dest を退避。
        GPR_reg sc_pre; bool sc_on = false;
        if (__builtin_expect(g_iv2_mmi_selfcheck, 0) &&
            ((c.op >= Op_Pand && c.op <= Op_Pcgtw) || (c.op >= Op_Pmaxh && c.op <= Op_Pcpyud))) {
            sc_pre = gpr[c.dest];
            sc_on = true;
        }
#endif // IV2_EE_HOT_DIAG

        switch (c.op) {
        case Op_Fallback:
            cpuRegs.code = c.raw;
#if IV2_EE_HOT_DIAG
            // [P101-3R PROFILE] Fallback opcode bucket
            if (__builtin_expect(g_iv2_profile_enabled, 0)) {
                const u32 primary = (c.raw >> 26) & 0x3F;
                s_fb_primary[primary]++;
                if (primary == 0)
                    s_fb_special[c.raw & 0x3F]++;
                else if (primary == 0x10)
                    s_fb_cop0_rs[(c.raw >> 21) & 0x1F]++;
                else if (primary == 0x11)
                    s_fb_cop1_rs[(c.raw >> 21) & 0x1F]++;
                else if (primary == 0x12) {
                    const u32 rs = (c.raw >> 21) & 0x1F;
                    s_fb_cop2_rs[rs]++;
                    // rs>=0x10 の VU macro op は funct を bucket 化
                    if (rs >= 0x10)
                        s_fb_cop2_funct[c.raw & 0x3F]++;
                }
            }
#endif // IV2_EE_HOT_DIAG
            c.handler();
            // 終端 handler 経由で pc が変わった場合、以降のループは
            // 本来は実行されないべきだが、compile 時に終端命令は i==n-1 に
            // しかないことを保証しているので、次反復が発生しない。
            break;

        case Op_Nop:
            // [P101-14 R1 2026-08-06] 実行時の合体スキャンは廃止。
            // Nop は CompileBlock の compact パスで消し、pc/cycle は直後の非 Nop entry へ
            // 前方マージ済み。ここに来るのは **消せない Nop だけ** =
            // ①block 先頭 (SMC 検証が entry 0 の raw を見るため残す) ②block 末尾の Nop。
            break;

        case Op_SetConst:
            gpr[c.dest].UD[0] = (s64)(s32)c.imm;
            break;

        case Op_AddIU:
            gpr[c.dest].UD[0] = (s64)(s32)((u32)gpr[c.src1].UL[0] + (u32)c.imm);
            break;

        case Op_OrI:
            gpr[c.dest].UD[0] = gpr[c.src1].UD[0] | (u64)(u32)c.imm;
            break;

        case Op_AndI:
            gpr[c.dest].UD[0] = gpr[c.src1].UD[0] & (u64)(u32)c.imm;
            break;

        case Op_XorI:
            gpr[c.dest].UD[0] = gpr[c.src1].UD[0] ^ (u64)(u32)c.imm;
            break;

        case Op_SltI:
            gpr[c.dest].UD[0] = ((s64)gpr[c.src1].SD[0] < (s64)c.imm) ? 1 : 0;
            break;

        case Op_SltIU:
            gpr[c.dest].UD[0] = ((u64)gpr[c.src1].UD[0] < (u64)(s64)c.imm) ? 1 : 0;
            break;

        case Op_AddU:
            gpr[c.dest].UD[0] = (s64)(s32)((u32)gpr[c.src1].UL[0] + (u32)gpr[c.src2].UL[0]);
            break;

        case Op_SubU:
            gpr[c.dest].UD[0] = (s64)(s32)((u32)gpr[c.src1].UL[0] - (u32)gpr[c.src2].UL[0]);
            break;

        case Op_AndR:
            gpr[c.dest].UD[0] = gpr[c.src1].UD[0] & gpr[c.src2].UD[0];
            break;

        case Op_OrR:
            gpr[c.dest].UD[0] = gpr[c.src1].UD[0] | gpr[c.src2].UD[0];
            break;

        case Op_XorR:
            gpr[c.dest].UD[0] = gpr[c.src1].UD[0] ^ gpr[c.src2].UD[0];
            break;

        case Op_NorR:
            gpr[c.dest].UD[0] = ~(gpr[c.src1].UD[0] | gpr[c.src2].UD[0]);
            break;

        case Op_SltR:
            gpr[c.dest].UD[0] = ((s64)gpr[c.src1].SD[0] < (s64)gpr[c.src2].SD[0]) ? 1 : 0;
            break;

        case Op_SltRU:
            gpr[c.dest].UD[0] = ((u64)gpr[c.src1].UD[0] < (u64)gpr[c.src2].UD[0]) ? 1 : 0;
            break;

        case Op_Sll:
            gpr[c.dest].UD[0] = (s64)(s32)((u32)gpr[c.src2].UL[0] << (c.imm & 0x1F));
            break;

        case Op_Srl:
            gpr[c.dest].UD[0] = (s64)(s32)((u32)gpr[c.src2].UL[0] >> (c.imm & 0x1F));
            break;

        case Op_Sra:
            gpr[c.dest].UD[0] = (s64)((s32)gpr[c.src2].UL[0] >> (c.imm & 0x1F));
            break;

        // ─── Load/Store (VTLB L0 キャッシュ経由) ──────
        case Op_Lb: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            gpr[c.dest].UD[0] = (s64)(s8)iv2_read8(addr);
            break;
        }
        case Op_Lbu: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            gpr[c.dest].UD[0] = (u64)(u8)iv2_read8(addr);
            break;
        }
        case Op_Lh: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            gpr[c.dest].UD[0] = (s64)(s16)iv2_read16(addr);
            break;
        }
        case Op_Lhu: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            gpr[c.dest].UD[0] = (u64)(u16)iv2_read16(addr);
            break;
        }
        case Op_Lw: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            gpr[c.dest].UD[0] = (s64)(s32)iv2_read32(addr);
            break;
        }
        case Op_Lwu: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            gpr[c.dest].UD[0] = (u64)(u32)iv2_read32(addr);
            break;
        }
        case Op_Ld: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            gpr[c.dest].UD[0] = iv2_read64(addr);
            break;
        }
        case Op_Sb: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            iv2_write8(addr, (u8)gpr[c.dest].UL[0]);
            break;
        }
        case Op_Sh: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            iv2_write16(addr, (u16)gpr[c.dest].UL[0]);
            break;
        }
        case Op_Sw: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            iv2_write32(addr, (u32)gpr[c.dest].UL[0]);
            break;
        }
        case Op_Sd: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            iv2_write64(addr, gpr[c.dest].UD[0]);
            break;
        }

        // [P101-3 Phase N] 128-bit load/store native
        case Op_Lq: {
            // LQ: EE は addr & ~0xF で 16-byte align 強制 (hw silently masks)
            u32 addr = ((u32)gpr[c.src1].UL[0] + (u32)c.imm) & ~0xFu;
            iv2_read128(addr, &gpr[c.dest]);
            break;
        }
        case Op_Sq: {
            u32 addr = ((u32)gpr[c.src1].UL[0] + (u32)c.imm) & ~0xFu;
            iv2_write128(addr, &gpr[c.dest]);
            break;
        }
        case Op_Lqc2: {
            // LQC2: addr & ~0xF、VU0.VF[dest] = mem128[addr]
            u32 addr = ((u32)gpr[c.src1].UL[0] + (u32)c.imm) & ~0xFu;
            iv2_read128(addr, &VU0.VF[c.dest]);
            break;
        }
        case Op_Sqc2: {
            u32 addr = ((u32)gpr[c.src1].UL[0] + (u32)c.imm) & ~0xFu;
            iv2_write128(addr, &VU0.VF[c.dest]);
            break;
        }
        case Op_Addi: {
            // s32 add with overflow check. 99%+ 非 overflow → inline fast path。
            const s32 rs = gpr[c.src1].SL[0];
            const s32 im = c.imm;
            const s32 r  = rs + im;
            // overflow: 同 sign 入力 + 異 sign 結果。 ((rs^r) & (im^r)) < 0
            if (__builtin_expect(((rs ^ r) & (im ^ r)) < 0, 0)) {
                cpuRegs.code = c.raw;
                c.handler();
                break;
            }
            gpr[c.dest].UD[0] = (s64)r;
            break;
        }

        // ─── Phase F: FPU (COP1) native ────────────
        case Op_Mfc1:
            // GPR[dest] = sign_ext(fpuRegs.fpr[src1].SL)
            gpr[c.dest].SD[0] = (s64)(s32)fpuRegs.fpr[c.src1].SL;
            break;
        case Op_Cfc1:
            // GPR[dest] = processed fpuRegs.fprc[src1]
            // fs == 31: (raw & 0x0083c078) | 0x01000001 (PS2 FCR31 mask)
            // fs == 0:  0x2E00
            // other:    0
            if (c.src1 == 31) {
                u32 raw = fpuRegs.fprc[31];
                raw = (raw & 0x0083c078u) | 0x01000001u;
                gpr[c.dest].SD[0] = (s64)(s32)raw;
            } else if (c.src1 == 0) {
                gpr[c.dest].SD[0] = 0x2E00;
            } else {
                gpr[c.dest].SD[0] = 0;
            }
            break;
        case Op_Mtc1:
            fpuRegs.fpr[c.dest].UL = gpr[c.src1].UL[0];
            break;
        case Op_Ctc1:
            // CTC1 は fs != 31 なら no-op
            if (c.dest == 31)
                fpuRegs.fprc[31] = gpr[c.src1].UL[0];
            break;
        case Op_MovS:
            fpuRegs.fpr[c.dest].UL = fpuRegs.fpr[c.src1].UL;
            break;
        case Op_AbsS:
            fpuRegs.fpr[c.dest].UL = fpuRegs.fpr[c.src1].UL & 0x7FFFFFFFu;
            // ABS_S は FPUflagO/U もクリアするが、現状の native は近似実装
            // (Clearing flags が稀に影響する可能性 → 必要なら Fallback に戻す)
            fpuRegs.fprc[31] &= ~((1u << 14) | (1u << 3));   // FPUflagO | FPUflagU
            break;
        case Op_NegS:
            fpuRegs.fpr[c.dest].UL = fpuRegs.fpr[c.src1].UL ^ 0x80000000u;
            break;
        case Op_Lwc1: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            fpuRegs.fpr[c.dest].UL = iv2_read32(addr);
            break;
        }
        case Op_Swc1: {
            u32 addr = (u32)gpr[c.src1].UL[0] + (u32)c.imm;
            iv2_write32(addr, fpuRegs.fpr[c.dest].UL);
            break;
        }

        // ─── Phase F+: FPU 算術 (クランプ付き native) ──
        case Op_AddS: {
            float r = iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) + iv2_fpuDouble(fpuRegs.fpr[c.src2].UL);
            iv2_fpu_store_result(c.dest, r, FPUflagO | FPUflagSO, FPUflagU | FPUflagSU);
            break;
        }
        case Op_SubS: {
            float r = iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) - iv2_fpuDouble(fpuRegs.fpr[c.src2].UL);
            iv2_fpu_store_result(c.dest, r, FPUflagO | FPUflagSO, FPUflagU | FPUflagSU);
            break;
        }
        case Op_MulS: {
            float r = iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) * iv2_fpuDouble(fpuRegs.fpr[c.src2].UL);
            iv2_fpu_store_result(c.dest, r, FPUflagO | FPUflagSO, FPUflagU | FPUflagSU);
            break;
        }
        case Op_DivS: {
            u32 fsUL = fpuRegs.fpr[c.src1].UL;
            u32 ftUL = fpuRegs.fpr[c.src2].UL;
            // checkDivideByZero: 簡略化 — ゼロ除算なら overflow flag
            if ((ftUL & 0x7F800000u) == 0) {
                // divide by zero → +/- MAX
                u32 ul = ((fsUL ^ ftUL) & 0x80000000u) | posFmax;
                fpuRegs.fpr[c.dest].UL = ul;
                fpuRegs.fprc[31] |= (FPUflagD | FPUflagSD);
                break;
            }
            float r;
            {
                float a = iv2_fpuDouble(fsUL), b = iv2_fpuDouble(ftUL);
                const Iv2FpuDivRoundScope div_round;   // [P114] 再コンパイラと同じ丸めにする
                P114_FP_LAUNDER(a); P114_FP_LAUNDER(b);
                r = a / b;
                P114_FP_LAUNDER(r);
            }
            iv2_fpu_store_result(c.dest, r, 0, 0);
            break;
        }
        case Op_MaddS: {
            float prod = iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) * iv2_fpuDouble(fpuRegs.fpr[c.src2].UL);
            u32 prodUL; std::memcpy(&prodUL, &prod, 4);
            float r = iv2_fpuDouble(fpuRegs.ACC.UL) + iv2_fpuDouble(prodUL);
            iv2_fpu_store_result(c.dest, r, FPUflagO | FPUflagSO, FPUflagU | FPUflagSU);
            break;
        }
        case Op_MsubS: {
            float prod = iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) * iv2_fpuDouble(fpuRegs.fpr[c.src2].UL);
            u32 prodUL; std::memcpy(&prodUL, &prod, 4);
            float r = iv2_fpuDouble(fpuRegs.ACC.UL) - iv2_fpuDouble(prodUL);
            iv2_fpu_store_result(c.dest, r, FPUflagO | FPUflagSO, FPUflagU | FPUflagSU);
            break;
        }
        case Op_AddaS: {
            float r = iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) + iv2_fpuDouble(fpuRegs.fpr[c.src2].UL);
            iv2_fpu_store_acc(r, FPUflagO | FPUflagSO, FPUflagU | FPUflagSU);
            break;
        }
        case Op_SubaS: {
            float r = iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) - iv2_fpuDouble(fpuRegs.fpr[c.src2].UL);
            iv2_fpu_store_acc(r, FPUflagO | FPUflagSO, FPUflagU | FPUflagSU);
            break;
        }
        case Op_MulaS: {
            float r = iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) * iv2_fpuDouble(fpuRegs.fpr[c.src2].UL);
            iv2_fpu_store_acc(r, FPUflagO | FPUflagSO, FPUflagU | FPUflagSU);
            break;
        }
        case Op_MaddaS: {
            float prod = iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) * iv2_fpuDouble(fpuRegs.fpr[c.src2].UL);
            u32 prodUL; std::memcpy(&prodUL, &prod, 4);
            float r = iv2_fpuDouble(fpuRegs.ACC.UL) + iv2_fpuDouble(prodUL);
            iv2_fpu_store_acc(r, FPUflagO | FPUflagSO, FPUflagU | FPUflagSU);
            break;
        }
        case Op_MsubaS: {
            float prod = iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) * iv2_fpuDouble(fpuRegs.fpr[c.src2].UL);
            u32 prodUL; std::memcpy(&prodUL, &prod, 4);
            float r = iv2_fpuDouble(fpuRegs.ACC.UL) - iv2_fpuDouble(prodUL);
            iv2_fpu_store_acc(r, FPUflagO | FPUflagSO, FPUflagU | FPUflagSU);
            break;
        }
        case Op_MaxS: {
            // FPU.cpp fp_max: u32 比較で符号ビット考慮しつつ大きい方
            // 正数同士なら UL 比較、負数同士なら逆、異符号は正>負
            u32 a = fpuRegs.fpr[c.src1].UL;
            u32 b = fpuRegs.fpr[c.src2].UL;
            u32 r;
            if ((a & 0x80000000u) == 0 && (b & 0x80000000u) == 0)
                r = (a >= b) ? a : b;
            else if ((a & 0x80000000u) && (b & 0x80000000u))
                r = (a <= b) ? a : b;   // 両負: UL小さい方が大きい
            else
                r = (a & 0x80000000u) ? b : a;  // a負なら b
            fpuRegs.fpr[c.dest].UL = r;
            fpuRegs.fprc[31] &= ~(FPUflagO | FPUflagU);
            break;
        }
        case Op_MinS: {
            u32 a = fpuRegs.fpr[c.src1].UL;
            u32 b = fpuRegs.fpr[c.src2].UL;
            u32 r;
            if ((a & 0x80000000u) == 0 && (b & 0x80000000u) == 0)
                r = (a <= b) ? a : b;
            else if ((a & 0x80000000u) && (b & 0x80000000u))
                r = (a >= b) ? a : b;
            else
                r = (a & 0x80000000u) ? a : b;
            fpuRegs.fpr[c.dest].UL = r;
            fpuRegs.fprc[31] &= ~(FPUflagO | FPUflagU);
            break;
        }
        case Op_SqrtS: {
            // fpr[dest] = sqrt(|fpr[src2]|), 負数は 0, NaN→FMAX
            // src2 = Ft
            u32 ftUL = fpuRegs.fpr[c.src2].UL;
            u32 absUL = ftUL & 0x7FFFFFFFu;
            float r;
            if ((ftUL & 0x7F800000u) == 0x7F800000u && (ftUL & 0x007FFFFFu) != 0) {
                // NaN
                r = 0.0f;
            } else {
                float absF; std::memcpy(&absF, &absUL, 4);
                const Iv2FpuDivRoundScope div_round;   // [P114]
                P114_FP_LAUNDER(absF);
                r = std::sqrt(absF);
                P114_FP_LAUNDER(r);
            }
            iv2_fpu_store_result(c.dest, r, 0, 0);
            break;
        }
        case Op_RsqrtS: {
            // fpr[dest] = fpr[src1] / sqrt(|fpr[src2]|)
            u32 ftUL = fpuRegs.fpr[c.src2].UL;
            u32 absUL = ftUL & 0x7FFFFFFFu;
            if ((absUL & 0x7F800000u) == 0) {
                // sqrt denom == 0 → divide-by-zero → MAX
                u32 ul = ((fpuRegs.fpr[c.src1].UL ^ ftUL) & 0x80000000u) | posFmax;
                fpuRegs.fpr[c.dest].UL = ul;
                fpuRegs.fprc[31] |= (FPUflagD | FPUflagSD);
                break;
            }
            float absF; std::memcpy(&absF, &absUL, 4);
            float r;
            {
                float num = iv2_fpuDouble(fpuRegs.fpr[c.src1].UL);
                const Iv2FpuDivRoundScope div_round;   // [P114]
                P114_FP_LAUNDER(absF); P114_FP_LAUNDER(num);
                float denom = std::sqrt(absF);
                r = num / denom;
                P114_FP_LAUNDER(r);
            }
            iv2_fpu_store_result(c.dest, r, 0, 0);
            break;
        }
        case Op_CvtS: {
            // fpr[dest] = (float)(s32)fpr[src1].SL
            float r = (float)(s32)fpuRegs.fpr[c.src1].SL;
            u32 ul; std::memcpy(&ul, &r, 4);
            fpuRegs.fpr[c.dest].UL = ul;
            break;
        }
        case Op_CvtW: {
            // fpr[dest].SL = (s32)fpr[src1] with clamping
            u32 fsUL = fpuRegs.fpr[c.src1].UL;
            if ((fsUL & 0x7F800000u) <= 0x4E800000u) {
                float fsF; std::memcpy(&fsF, &fsUL, 4);
                fpuRegs.fpr[c.dest].SL = (s32)fsF;
            } else if ((fsUL & 0x80000000u) == 0) {
                fpuRegs.fpr[c.dest].UL = 0x7FFFFFFFu;
            } else {
                fpuRegs.fpr[c.dest].UL = 0x80000000u;
            }
            break;
        }
        case Op_CfS:
            fpuRegs.fprc[31] &= ~FPUflagC;
            break;
        case Op_CeqS: {
            if (iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) == iv2_fpuDouble(fpuRegs.fpr[c.src2].UL))
                fpuRegs.fprc[31] |= FPUflagC;
            else
                fpuRegs.fprc[31] &= ~FPUflagC;
            break;
        }
        case Op_CltS: {
            if (iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) < iv2_fpuDouble(fpuRegs.fpr[c.src2].UL))
                fpuRegs.fprc[31] |= FPUflagC;
            else
                fpuRegs.fprc[31] &= ~FPUflagC;
            break;
        }
        case Op_CleS: {
            if (iv2_fpuDouble(fpuRegs.fpr[c.src1].UL) <= iv2_fpuDouble(fpuRegs.fpr[c.src2].UL))
                fpuRegs.fprc[31] |= FPUflagC;
            else
                fpuRegs.fprc[31] &= ~FPUflagC;
            break;
        }

        // ─── Phase G: MMI native (NEON 128-bit) ─────
#if defined(__aarch64__) || defined(_M_ARM64)
        case Op_Pand: {
            uint64x2_t a = vld1q_u64(&gpr[c.src1].UD[0]);
            uint64x2_t b = vld1q_u64(&gpr[c.src2].UD[0]);
            vst1q_u64(&gpr[c.dest].UD[0], vandq_u64(a, b));
            break;
        }
        case Op_Por: {
            uint64x2_t a = vld1q_u64(&gpr[c.src1].UD[0]);
            uint64x2_t b = vld1q_u64(&gpr[c.src2].UD[0]);
            vst1q_u64(&gpr[c.dest].UD[0], vorrq_u64(a, b));
            break;
        }
        case Op_Pxor: {
            uint64x2_t a = vld1q_u64(&gpr[c.src1].UD[0]);
            uint64x2_t b = vld1q_u64(&gpr[c.src2].UD[0]);
            vst1q_u64(&gpr[c.dest].UD[0], veorq_u64(a, b));
            break;
        }
        case Op_Pnor: {
            uint64x2_t a = vld1q_u64(&gpr[c.src1].UD[0]);
            uint64x2_t b = vld1q_u64(&gpr[c.src2].UD[0]);
            // NEON vmvnq は 8/16/32bit しか無いので reinterpret
            uint32x4_t nor = vmvnq_u32(vreinterpretq_u32_u64(vorrq_u64(a, b)));
            vst1q_u64(&gpr[c.dest].UD[0], vreinterpretq_u64_u32(nor));
            break;
        }
        case Op_Paddb: {
            uint8x16_t a = vld1q_u8((u8*)&gpr[c.src1].UD[0]);
            uint8x16_t b = vld1q_u8((u8*)&gpr[c.src2].UD[0]);
            vst1q_u8((u8*)&gpr[c.dest].UD[0], vaddq_u8(a, b));
            break;
        }
        case Op_Paddh: {
            uint16x8_t a = vld1q_u16((u16*)&gpr[c.src1].UD[0]);
            uint16x8_t b = vld1q_u16((u16*)&gpr[c.src2].UD[0]);
            vst1q_u16((u16*)&gpr[c.dest].UD[0], vaddq_u16(a, b));
            break;
        }
        case Op_Paddw: {
            uint32x4_t a = vld1q_u32(&gpr[c.src1].UL[0]);
            uint32x4_t b = vld1q_u32(&gpr[c.src2].UL[0]);
            vst1q_u32(&gpr[c.dest].UL[0], vaddq_u32(a, b));
            break;
        }
        case Op_Psubb: {
            uint8x16_t a = vld1q_u8((u8*)&gpr[c.src1].UD[0]);
            uint8x16_t b = vld1q_u8((u8*)&gpr[c.src2].UD[0]);
            vst1q_u8((u8*)&gpr[c.dest].UD[0], vsubq_u8(a, b));
            break;
        }
        case Op_Psubh: {
            uint16x8_t a = vld1q_u16((u16*)&gpr[c.src1].UD[0]);
            uint16x8_t b = vld1q_u16((u16*)&gpr[c.src2].UD[0]);
            vst1q_u16((u16*)&gpr[c.dest].UD[0], vsubq_u16(a, b));
            break;
        }
        case Op_Psubw: {
            uint32x4_t a = vld1q_u32(&gpr[c.src1].UL[0]);
            uint32x4_t b = vld1q_u32(&gpr[c.src2].UL[0]);
            vst1q_u32(&gpr[c.dest].UL[0], vsubq_u32(a, b));
            break;
        }
        case Op_Pceqb: {
            uint8x16_t a = vld1q_u8((u8*)&gpr[c.src1].UD[0]);
            uint8x16_t b = vld1q_u8((u8*)&gpr[c.src2].UD[0]);
            vst1q_u8((u8*)&gpr[c.dest].UD[0], vceqq_u8(a, b));
            break;
        }
        case Op_Pceqh: {
            uint16x8_t a = vld1q_u16((u16*)&gpr[c.src1].UD[0]);
            uint16x8_t b = vld1q_u16((u16*)&gpr[c.src2].UD[0]);
            vst1q_u16((u16*)&gpr[c.dest].UD[0], vceqq_u16(a, b));
            break;
        }
        case Op_Pceqw: {
            uint32x4_t a = vld1q_u32(&gpr[c.src1].UL[0]);
            uint32x4_t b = vld1q_u32(&gpr[c.src2].UL[0]);
            vst1q_u32(&gpr[c.dest].UL[0], vceqq_u32(a, b));
            break;
        }
        case Op_Pcgtb: {
            int8x16_t a = vld1q_s8((s8*)&gpr[c.src1].UD[0]);
            int8x16_t b = vld1q_s8((s8*)&gpr[c.src2].UD[0]);
            vst1q_u8((u8*)&gpr[c.dest].UD[0], vcgtq_s8(a, b));
            break;
        }
        case Op_Pcgth: {
            int16x8_t a = vld1q_s16((s16*)&gpr[c.src1].UD[0]);
            int16x8_t b = vld1q_s16((s16*)&gpr[c.src2].UD[0]);
            vst1q_u16((u16*)&gpr[c.dest].UD[0], vcgtq_s16(a, b));
            break;
        }
        case Op_Pcgtw: {
            int32x4_t a = vld1q_s32((s32*)&gpr[c.src1].UL[0]);
            int32x4_t b = vld1q_s32((s32*)&gpr[c.src2].UL[0]);
            vst1q_u32(&gpr[c.dest].UL[0], vcgtq_s32(a, b));
            break;
        }
        // ─── Phase G+: MMI 拡張 (NEON) ──────────
        case Op_Pmaxh: {
            int16x8_t a = vld1q_s16((s16*)&gpr[c.src1].UD[0]);
            int16x8_t b = vld1q_s16((s16*)&gpr[c.src2].UD[0]);
            vst1q_s16((s16*)&gpr[c.dest].UD[0], vmaxq_s16(a, b));
            break;
        }
        case Op_Pmaxw: {
            int32x4_t a = vld1q_s32((s32*)&gpr[c.src1].UL[0]);
            int32x4_t b = vld1q_s32((s32*)&gpr[c.src2].UL[0]);
            vst1q_s32((s32*)&gpr[c.dest].UL[0], vmaxq_s32(a, b));
            break;
        }
        case Op_Pminh: {
            int16x8_t a = vld1q_s16((s16*)&gpr[c.src1].UD[0]);
            int16x8_t b = vld1q_s16((s16*)&gpr[c.src2].UD[0]);
            vst1q_s16((s16*)&gpr[c.dest].UD[0], vminq_s16(a, b));
            break;
        }
        case Op_Pminw: {
            int32x4_t a = vld1q_s32((s32*)&gpr[c.src1].UL[0]);
            int32x4_t b = vld1q_s32((s32*)&gpr[c.src2].UL[0]);
            vst1q_s32((s32*)&gpr[c.dest].UL[0], vminq_s32(a, b));
            break;
        }
        case Op_Psllh: {
            uint16x8_t v = vld1q_u16((u16*)&gpr[c.src2].UD[0]);
            int8_t sh = (int8_t)(c.imm & 0xF);
            // NEON には vshlq_u16 (runtime shift) を使用
            int16x8_t sh_vec = vdupq_n_s16(sh);
            uint16x8_t r = vshlq_u16(v, sh_vec);
            vst1q_u16((u16*)&gpr[c.dest].UD[0], r);
            break;
        }
        case Op_Psrlh: {
            uint16x8_t v = vld1q_u16((u16*)&gpr[c.src2].UD[0]);
            int8_t sh = (int8_t)(c.imm & 0xF);
            int16x8_t sh_vec = vdupq_n_s16(-sh);
            uint16x8_t r = vshlq_u16(v, sh_vec);
            vst1q_u16((u16*)&gpr[c.dest].UD[0], r);
            break;
        }
        case Op_Psrah: {
            int16x8_t v = vld1q_s16((s16*)&gpr[c.src2].UD[0]);
            int8_t sh = (int8_t)(c.imm & 0xF);
            int16x8_t sh_vec = vdupq_n_s16(-sh);
            int16x8_t r = vshlq_s16(v, sh_vec);
            vst1q_s16((s16*)&gpr[c.dest].UD[0], r);
            break;
        }
        case Op_Psllw: {
            uint32x4_t v = vld1q_u32(&gpr[c.src2].UL[0]);
            int8_t sh = (int8_t)(c.imm & 0x1F);
            int32x4_t sh_vec = vdupq_n_s32(sh);
            vst1q_u32(&gpr[c.dest].UL[0], vshlq_u32(v, sh_vec));
            break;
        }
        case Op_Psrlw: {
            uint32x4_t v = vld1q_u32(&gpr[c.src2].UL[0]);
            int8_t sh = (int8_t)(c.imm & 0x1F);
            int32x4_t sh_vec = vdupq_n_s32(-sh);
            vst1q_u32(&gpr[c.dest].UL[0], vshlq_u32(v, sh_vec));
            break;
        }
        case Op_Psraw: {
            int32x4_t v = vld1q_s32((s32*)&gpr[c.src2].UL[0]);
            int8_t sh = (int8_t)(c.imm & 0x1F);
            int32x4_t sh_vec = vdupq_n_s32(-sh);
            vst1q_s32((s32*)&gpr[c.dest].UL[0], vshlq_s32(v, sh_vec));
            break;
        }
        case Op_Paddsb: {
            int8x16_t a = vld1q_s8((s8*)&gpr[c.src1].UD[0]);
            int8x16_t b = vld1q_s8((s8*)&gpr[c.src2].UD[0]);
            vst1q_s8((s8*)&gpr[c.dest].UD[0], vqaddq_s8(a, b));
            break;
        }
        case Op_Paddsh: {
            int16x8_t a = vld1q_s16((s16*)&gpr[c.src1].UD[0]);
            int16x8_t b = vld1q_s16((s16*)&gpr[c.src2].UD[0]);
            vst1q_s16((s16*)&gpr[c.dest].UD[0], vqaddq_s16(a, b));
            break;
        }
        case Op_Paddsw: {
            int32x4_t a = vld1q_s32((s32*)&gpr[c.src1].UL[0]);
            int32x4_t b = vld1q_s32((s32*)&gpr[c.src2].UL[0]);
            vst1q_s32((s32*)&gpr[c.dest].UL[0], vqaddq_s32(a, b));
            break;
        }
        case Op_Psubsb: {
            int8x16_t a = vld1q_s8((s8*)&gpr[c.src1].UD[0]);
            int8x16_t b = vld1q_s8((s8*)&gpr[c.src2].UD[0]);
            vst1q_s8((s8*)&gpr[c.dest].UD[0], vqsubq_s8(a, b));
            break;
        }
        case Op_Psubsh: {
            int16x8_t a = vld1q_s16((s16*)&gpr[c.src1].UD[0]);
            int16x8_t b = vld1q_s16((s16*)&gpr[c.src2].UD[0]);
            vst1q_s16((s16*)&gpr[c.dest].UD[0], vqsubq_s16(a, b));
            break;
        }
        case Op_Psubsw: {
            int32x4_t a = vld1q_s32((s32*)&gpr[c.src1].UL[0]);
            int32x4_t b = vld1q_s32((s32*)&gpr[c.src2].UL[0]);
            vst1q_s32((s32*)&gpr[c.dest].UL[0], vqsubq_s32(a, b));
            break;
        }
        case Op_Paddub: {
            uint8x16_t a = vld1q_u8((u8*)&gpr[c.src1].UD[0]);
            uint8x16_t b = vld1q_u8((u8*)&gpr[c.src2].UD[0]);
            vst1q_u8((u8*)&gpr[c.dest].UD[0], vqaddq_u8(a, b));
            break;
        }
        case Op_Padduh: {
            uint16x8_t a = vld1q_u16((u16*)&gpr[c.src1].UD[0]);
            uint16x8_t b = vld1q_u16((u16*)&gpr[c.src2].UD[0]);
            vst1q_u16((u16*)&gpr[c.dest].UD[0], vqaddq_u16(a, b));
            break;
        }
        case Op_Padduw: {
            uint32x4_t a = vld1q_u32(&gpr[c.src1].UL[0]);
            uint32x4_t b = vld1q_u32(&gpr[c.src2].UL[0]);
            vst1q_u32(&gpr[c.dest].UL[0], vqaddq_u32(a, b));
            break;
        }
        case Op_Psubub: {
            uint8x16_t a = vld1q_u8((u8*)&gpr[c.src1].UD[0]);
            uint8x16_t b = vld1q_u8((u8*)&gpr[c.src2].UD[0]);
            vst1q_u8((u8*)&gpr[c.dest].UD[0], vqsubq_u8(a, b));
            break;
        }
        case Op_Psubuh: {
            uint16x8_t a = vld1q_u16((u16*)&gpr[c.src1].UD[0]);
            uint16x8_t b = vld1q_u16((u16*)&gpr[c.src2].UD[0]);
            vst1q_u16((u16*)&gpr[c.dest].UD[0], vqsubq_u16(a, b));
            break;
        }
        case Op_Psubuw: {
            uint32x4_t a = vld1q_u32(&gpr[c.src1].UL[0]);
            uint32x4_t b = vld1q_u32(&gpr[c.src2].UL[0]);
            vst1q_u32(&gpr[c.dest].UL[0], vqsubq_u32(a, b));
            break;
        }
        case Op_Pextlb: {
            uint8x16_t a = vld1q_u8((u8*)&gpr[c.src1].UD[0]);
            uint8x16_t b = vld1q_u8((u8*)&gpr[c.src2].UD[0]);
            // PEXTLB: rd.[0..7,8..15] = interleave(rt.[0..7], rs.[0..7])
            // vzip1q_u8(b, a) gives: b[0], a[0], b[1], a[1], ...
            vst1q_u8((u8*)&gpr[c.dest].UD[0], vzip1q_u8(b, a));
            break;
        }
        case Op_Pextlh: {
            uint16x8_t a = vld1q_u16((u16*)&gpr[c.src1].UD[0]);
            uint16x8_t b = vld1q_u16((u16*)&gpr[c.src2].UD[0]);
            vst1q_u16((u16*)&gpr[c.dest].UD[0], vzip1q_u16(b, a));
            break;
        }
        case Op_Pextlw: {
            uint32x4_t a = vld1q_u32(&gpr[c.src1].UL[0]);
            uint32x4_t b = vld1q_u32(&gpr[c.src2].UL[0]);
            vst1q_u32(&gpr[c.dest].UL[0], vzip1q_u32(b, a));
            break;
        }
        case Op_Pextub: {
            uint8x16_t a = vld1q_u8((u8*)&gpr[c.src1].UD[0]);
            uint8x16_t b = vld1q_u8((u8*)&gpr[c.src2].UD[0]);
            vst1q_u8((u8*)&gpr[c.dest].UD[0], vzip2q_u8(b, a));
            break;
        }
        case Op_Pextuh: {
            uint16x8_t a = vld1q_u16((u16*)&gpr[c.src1].UD[0]);
            uint16x8_t b = vld1q_u16((u16*)&gpr[c.src2].UD[0]);
            vst1q_u16((u16*)&gpr[c.dest].UD[0], vzip2q_u16(b, a));
            break;
        }
        case Op_Pextuw: {
            uint32x4_t a = vld1q_u32(&gpr[c.src1].UL[0]);
            uint32x4_t b = vld1q_u32(&gpr[c.src2].UL[0]);
            vst1q_u32(&gpr[c.dest].UL[0], vzip2q_u32(b, a));
            break;
        }
        case Op_Pcpyld: {
            // rd.lo = rt.lo, rd.hi = rs.lo
            // [iv2 fix 2026-06-01] ★ rd==rs aliasing 修正 ★: dest.UD[0] 書込で src1(=dest)を
            // 破壊する前に両 source を temp へ先読み。GTA VC vs~4660 stall の真因。
            const u64 _lo = gpr[c.src2].UD[0];   // rt.lo
            const u64 _hi = gpr[c.src1].UD[0];   // rs.lo
            gpr[c.dest].UD[0] = _lo;
            gpr[c.dest].UD[1] = _hi;
            break;
        }
        case Op_Pcpyud: {
            // rd.lo = rs.hi, rd.hi = rt.hi
            // [iv2 fix 2026-06-01] aliasing 防御 (Pcpyld と同型、念のため temp 経由)。
            const u64 _lo = gpr[c.src1].UD[1];   // rs.hi
            const u64 _hi = gpr[c.src2].UD[1];   // rt.hi
            gpr[c.dest].UD[0] = _lo;
            gpr[c.dest].UD[1] = _hi;
            break;
        }
#endif

        // ─── Phase L: 64-bit ALU native ──────────
        case Op_DaddU:
            gpr[c.dest].UD[0] = gpr[c.src1].UD[0] + gpr[c.src2].UD[0];
            break;
        case Op_DsubU:
            gpr[c.dest].UD[0] = gpr[c.src1].UD[0] - gpr[c.src2].UD[0];
            break;
        case Op_DaddIU:
            gpr[c.dest].UD[0] = gpr[c.src1].UD[0] + (u64)(s64)(s32)c.imm;
            break;
        case Op_Dsll:
            gpr[c.dest].UD[0] = gpr[c.src2].UD[0] << (c.imm & 0x3F);
            break;
        case Op_Dsrl:
            gpr[c.dest].UD[0] = gpr[c.src2].UD[0] >> (c.imm & 0x3F);
            break;
        case Op_Dsra:
            gpr[c.dest].UD[0] = (u64)((s64)gpr[c.src2].UD[0] >> (c.imm & 0x3F));
            break;
        case Op_Dsll32:
            gpr[c.dest].UD[0] = gpr[c.src2].UD[0] << ((c.imm & 0x1F) + 32);
            break;
        case Op_Dsrl32:
            gpr[c.dest].UD[0] = gpr[c.src2].UD[0] >> ((c.imm & 0x1F) + 32);
            break;
        case Op_Dsra32:
            gpr[c.dest].UD[0] = (u64)((s64)gpr[c.src2].UD[0] >> ((c.imm & 0x1F) + 32));
            break;
        case Op_Dsllv:
            gpr[c.dest].UD[0] = gpr[c.src2].UD[0] << (gpr[c.src1].UL[0] & 0x3F);
            break;
        case Op_Dsrlv:
            gpr[c.dest].UD[0] = gpr[c.src2].UD[0] >> (gpr[c.src1].UL[0] & 0x3F);
            break;
        case Op_Dsrav:
            gpr[c.dest].UD[0] = (u64)((s64)gpr[c.src2].UD[0] >> (gpr[c.src1].UL[0] & 0x3F));
            break;

        // ─── Phase N: Mult/Div + HI/LO native ──────
        case Op_Mult: {
            // MULT: (s64)LO/HI = (s32)rs * (s32)rt; if (rd) GPR[rd] = (s32)LO
            s64 result = (s64)(s32)gpr[c.src1].UL[0] * (s64)(s32)gpr[c.src2].UL[0];
            cpuRegs.LO.UD[0] = (u64)(s64)(s32)(u32)result;
            cpuRegs.HI.UD[0] = (u64)(s64)(s32)(u32)(result >> 32);
            if (c.dest != 0) gpr[c.dest].UD[0] = cpuRegs.LO.UD[0];
            break;
        }
        case Op_MultU: {
            u64 result = (u64)gpr[c.src1].UL[0] * (u64)gpr[c.src2].UL[0];
            cpuRegs.LO.UD[0] = (u64)(s64)(s32)(u32)result;
            cpuRegs.HI.UD[0] = (u64)(s64)(s32)(u32)(result >> 32);
            if (c.dest != 0) gpr[c.dest].UD[0] = cpuRegs.LO.UD[0];
            break;
        }
        case Op_Div: {
            s32 a = (s32)gpr[c.src1].UL[0];
            s32 b = (s32)gpr[c.src2].UL[0];
            if (b != 0) {
                // MIPS: signed divide; -2^31 / -1 = -2^31 (overflow case)
                if (a == (s32)0x80000000 && b == -1) {
                    cpuRegs.LO.UD[0] = (u64)(s64)(s32)0x80000000u;
                    cpuRegs.HI.UD[0] = 0;
                } else {
                    cpuRegs.LO.UD[0] = (u64)(s64)(s32)(a / b);
                    cpuRegs.HI.UD[0] = (u64)(s64)(s32)(a % b);
                }
            } else {
                cpuRegs.LO.UD[0] = (a < 0) ? 1 : (u64)(s64)-1;
                cpuRegs.HI.UD[0] = (u64)(s64)a;
            }
            break;
        }
        case Op_DivU: {
            u32 a = gpr[c.src1].UL[0];
            u32 b = gpr[c.src2].UL[0];
            if (b != 0) {
                cpuRegs.LO.UD[0] = (u64)(s64)(s32)(a / b);
                cpuRegs.HI.UD[0] = (u64)(s64)(s32)(a % b);
            } else {
                cpuRegs.LO.UD[0] = (u64)(s64)-1;
                cpuRegs.HI.UD[0] = (u64)(s64)(s32)a;
            }
            break;
        }
        case Op_Mfhi:
            gpr[c.dest].UD[0] = cpuRegs.HI.UD[0];
            break;
        case Op_Mflo:
            gpr[c.dest].UD[0] = cpuRegs.LO.UD[0];
            break;
        case Op_Mthi:
            cpuRegs.HI.UD[0] = gpr[c.src1].UD[0];
            break;
        case Op_Mtlo:
            cpuRegs.LO.UD[0] = gpr[c.src1].UD[0];
            break;

        // ─── P101-4 Phase V: COP2 VU0 macro ────────────
        case Op_Vu0Macro: {
            // [BUG-FIX] cpuRegs.code は Op_Fallback 以外では更新されないため stale。
            // interp wrapper (VSUB/VADD 等) は先頭で `VU0.code = cpuRegs.code` するので
            // stale 値を拾い native / fallback 両方とも wrong funct で実行し graphics 崩壊。
            // 必ず c.raw を cpuRegs.code に設定してから下流に渡す。
            cpuRegs.code = c.raw;
            // 1) VU0 microprogram 終了待ち (VPU_STAT.VBS0 set の場合のみ実動作)
            _vu0FinishMicro();
            VU0.code = c.raw;
            const u32 funct = c.raw & 0x3F;
            // funct 分類: 0x00-0x2F の FMAC 系のみ到達 (decode で 0x30+ は Fallback)
            //   MAX (0x2B, 0x10-0x13)、MAXi (0x1D)、MAXbc は funct 0x10-0x13
            //   MINI (0x2F, 0x14-0x17)、MINIi (0x1F)
            //   OPMSUB (0x2E): UpperDispatch で fallback 返し → c.handler() (VOPMSUB 自己 SYNC)
            //   FMAC 系 (ADD/SUB/MUL/MADD/MSUB + bc/i/q): SYNCMSFLAGS 必要
            //   MAX/MINI: sync 不要
            const bool is_max_mini = (funct == 0x2B || funct == 0x2F ||
                                      (funct >= 0x10 && funct <= 0x17) ||
                                      funct == 0x1D || funct == 0x1F);

            // [P101-4 DIFF] iPSX2_IV2_VU0_DIFF=1 で native と interp (c.handler = VADD etc.) を 1 命令毎比較
            if (__builtin_expect(s_vu0_diff_enabled, 0))
            {
                static std::atomic<u64> s_v0_total{0};
                static std::atomic<u64> s_v0_mism{0};
                static std::atomic<u32> s_v0_quota{200};
                static constexpr u64 V0_LIMIT = 500000;
                u64 t = s_v0_total.fetch_add(1, std::memory_order_relaxed) + 1;
                if (t > V0_LIMIT) {
                    if (t == V0_LIMIT + 1)
                        Console.Error("@@IV2_VU0_DIFF@@ DISABLED after %llu ops. mism=%llu",
                            (unsigned long long)V0_LIMIT, (unsigned long long)s_v0_mism.load());
                    // 通常 path
                    if (iv2::vu_ops::UpperDispatch(&VU0)) {
                        if (!is_max_mini) {
                            VU0.VI[REG_STATUS_FLAG].UL = (VU0.VI[REG_STATUS_FLAG].UL & 0xFC0) | (VU0.statusflag & 0xF) | ((VU0.statusflag & 0xF) << 6);
                            VU0.VI[REG_MAC_FLAG].UL = VU0.macflag;
                        }
                    } else c.handler();
                    break;
                }

                // snap pre (VF[32] + VI[32] + ACC + q + p + macflag/statusflag/clipflag)
                VECTOR vf_pre[32]; std::memcpy(vf_pre, VU0.VF, sizeof(vf_pre));
                REG_VI vi_pre[32]; std::memcpy(vi_pre, VU0.VI, sizeof(vi_pre));
                VECTOR acc_pre = VU0.ACC;
                REG_VI q_pre = VU0.q, p_pre = VU0.p;
                u32 mf_pre = VU0.macflag, sf_pre = VU0.statusflag, cf_pre = VU0.clipflag;

                // native + sync
                if (iv2::vu_ops::UpperDispatch(&VU0)) {
                    if (!is_max_mini) {
                        VU0.VI[REG_STATUS_FLAG].UL = (VU0.VI[REG_STATUS_FLAG].UL & 0xFC0) | (VU0.statusflag & 0xF) | ((VU0.statusflag & 0xF) << 6);
                        VU0.VI[REG_MAC_FLAG].UL = VU0.macflag;
                    }
                } else {
                    c.handler(); break;  // native 未対応は diff 不要 (interp と同一)
                }

                // snap post-native
                VECTOR vf_pn[32]; std::memcpy(vf_pn, VU0.VF, sizeof(vf_pn));
                REG_VI vi_pn[32]; std::memcpy(vi_pn, VU0.VI, sizeof(vi_pn));
                VECTOR acc_pn = VU0.ACC;
                REG_VI q_pn = VU0.q, p_pn = VU0.p;
                u32 mf_pn = VU0.macflag, sf_pn = VU0.statusflag, cf_pn = VU0.clipflag;

                // restore pre
                std::memcpy(VU0.VF, vf_pre, sizeof(vf_pre));
                std::memcpy(VU0.VI, vi_pre, sizeof(vi_pre));
                VU0.ACC = acc_pre; VU0.q = q_pre; VU0.p = p_pre;
                VU0.macflag = mf_pre; VU0.statusflag = sf_pre; VU0.clipflag = cf_pre;

                // interp
                c.handler();

                // compare
                u32 vf_d = 0, vi_d = 0;
                for (int i = 0; i < 32; i++) if (std::memcmp(&vf_pn[i], &VU0.VF[i], sizeof(VECTOR))) vf_d |= (1u << i);
                for (int i = 0; i < 32; i++) if (vi_pn[i].UL != VU0.VI[i].UL) vi_d |= (1u << i);
                bool acc_d = std::memcmp(&acc_pn, &VU0.ACC, sizeof(VECTOR)) != 0;
                bool mf_d = (mf_pn != VU0.macflag) || (sf_pn != VU0.statusflag) || (cf_pn != VU0.clipflag);

                if (vf_d || vi_d || acc_d || mf_d) {
                    u64 m = s_v0_mism.fetch_add(1, std::memory_order_relaxed) + 1;
                    u32 q = s_v0_quota.load(std::memory_order_relaxed);
                    if (q > 0) {
                        s_v0_quota.store(q - 1, std::memory_order_relaxed);
                        Console.Error("@@IV2_VU0_DIFF@@ #%llu code=%08x funct=%02x vf_d=%08x vi_d=%08x acc_d=%d flags_d=%d",
                            (unsigned long long)m, c.raw, funct, vf_d, vi_d, acc_d ? 1:0, mf_d ? 1:0);
                        if (mf_d) Console.Error("@@IV2_VU0_DIFF@@   mf nat=%08x int=%08x sf nat=%08x int=%08x cf nat=%08x int=%08x",
                            mf_pn, VU0.macflag, sf_pn, VU0.statusflag, cf_pn, VU0.clipflag);
                        for (int i = 0; i < 32; i++) if (vf_d & (1u<<i))
                            Console.Error("@@IV2_VU0_DIFF@@   VF[%d] nat=%08x_%08x_%08x_%08x int=%08x_%08x_%08x_%08x",
                                i, vf_pn[i].UL[0],vf_pn[i].UL[1],vf_pn[i].UL[2],vf_pn[i].UL[3],
                                VU0.VF[i].UL[0],VU0.VF[i].UL[1],VU0.VF[i].UL[2],VU0.VF[i].UL[3]);
                        for (int i = 0; i < 32; i++) if (vi_d & (1u<<i))
                            Console.Error("@@IV2_VU0_DIFF@@   VI[%d] nat=%08x int=%08x", i, vi_pn[i].UL, VU0.VI[i].UL);
                    }
                    if ((m & 0x3FF) == 0) Console.Error("@@IV2_VU0_DIFF_SUM@@ mism=%llu total=%llu", (unsigned long long)m, (unsigned long long)t);
                }
                break;  // interp 結果保持
            }

            // 通常 path
            if (iv2::vu_ops::UpperDispatch(&VU0))
            {
                if (!is_max_mini)
                {
                    VU0.VI[REG_STATUS_FLAG].UL = (VU0.VI[REG_STATUS_FLAG].UL & 0xFC0) | (VU0.statusflag & 0xF) | ((VU0.statusflag & 0xF) << 6);
                    VU0.VI[REG_MAC_FLAG].UL = VU0.macflag;
                }
            }
            else
            {
                c.handler();
            }
            break;
        }

        // ─── Phase E: Branch condition check ─────────
        case Op_BCheckEq:
            s_branch_taken = (gpr[c.src1].SD[0] == gpr[c.src2].SD[0]) ? 1 : 0;
            s_branch_target = (u32)c.imm;
            s_branch_evt_nt = 1;   // BEQ: intCpu は not-taken で intEventTest
            break;
        case Op_BCheckNe:
            s_branch_taken = (gpr[c.src1].SD[0] != gpr[c.src2].SD[0]) ? 1 : 0;
            s_branch_target = (u32)c.imm;
            s_branch_evt_nt = 1;   // BNE: 同上
            break;
        case Op_BCheckLez:
            s_branch_taken = (gpr[c.src1].SD[0] <= 0) ? 1 : 0;
            s_branch_target = (u32)c.imm;
            s_branch_evt_nt = 0;
            break;
        case Op_BCheckGtz:
            s_branch_taken = (gpr[c.src1].SD[0] > 0) ? 1 : 0;
            s_branch_target = (u32)c.imm;
            s_branch_evt_nt = 0;
            break;
        case Op_BCheckLtz:
            s_branch_taken = (gpr[c.src1].SD[0] < 0) ? 1 : 0;
            s_branch_target = (u32)c.imm;
            s_branch_evt_nt = 0;
            break;
        case Op_BCheckGez:
            s_branch_taken = (gpr[c.src1].SD[0] >= 0) ? 1 : 0;
            s_branch_target = (u32)c.imm;
            s_branch_evt_nt = 0;
            break;
        case Op_BCheckLtzLink:
            // BLTZAL: $31 = pc+4 (pc は BCheck の post-increment 状態 = branch_pc+4)
            // なので $31 = cpuRegs.pc + 4 = branch_pc + 8 = BDS 後の pc
            gpr[31].UD[0] = (u64)(u32)(cpuRegs.pc + 4);   // [iv2 link fix 2026-05-31] intCpu _SetLink は zero-ext
            s_branch_taken = (gpr[c.src1].SD[0] < 0) ? 1 : 0;
            s_branch_target = (u32)c.imm;
            s_branch_evt_nt = 0;
            break;
        case Op_BCheckGezLink:
            gpr[31].UD[0] = (u64)(u32)(cpuRegs.pc + 4);   // [iv2 link fix 2026-05-31] intCpu _SetLink は zero-ext
            s_branch_taken = (gpr[c.src1].SD[0] >= 0) ? 1 : 0;
            s_branch_target = (u32)c.imm;
            s_branch_evt_nt = 0;
            break;
        case Op_BC1F:
            s_branch_taken = ((fpuRegs.fprc[31] & 0x00800000u) == 0) ? 1 : 0;
            s_branch_target = (u32)c.imm;
            s_branch_evt_nt = 0;
            break;
        case Op_BC1T:
            s_branch_taken = ((fpuRegs.fprc[31] & 0x00800000u) != 0) ? 1 : 0;
            s_branch_target = (u32)c.imm;
            s_branch_evt_nt = 0;
            break;
        case Op_JumpConst:
            // J / JAL (unconditional): taken 先は compile-time 確定 → link_taken にリンク可能
            if (c.dest != 0)
                gpr[c.dest].UD[0] = (u64)(u32)(cpuRegs.pc + 4);   // [iv2 link fix 2026-05-31] intCpu _SetLink は zero-ext   // $31 = branch_pc + 8
            s_branch_taken = 1;
            s_branch_target = (u32)c.imm;
            s_link_slot = &const_cast<Block*>(current_block)->link_taken;
            break;
        case Op_JumpReg:
            // JR / JALR: target は実行時確定 → リンク不可
            {
                u32 tgt = gpr[c.src1].UL[0];
                if (c.dest != 0)
                    gpr[c.dest].UD[0] = (u64)(u32)(cpuRegs.pc + 4);   // [iv2 link fix 2026-05-31] intCpu _SetLink は zero-ext
                s_branch_taken = 1;
                s_branch_target = tgt;
                s_link_slot = nullptr;   // リンク不可
            }
            break;

        case Op_BFinalize:
            // BFinalize 自体には pc 進行は無い。loop head で pc+=4 されたが、
            // 正しい pc を明示的に設定し直す (fall_through = c.imm = branch_pc+8)。
            // [Phase H] taken は link_taken、not-taken は link_not_taken に link 登録
            if (s_branch_taken) {
                s_link_slot = &const_cast<Block*>(current_block)->link_taken;
                // 内部で pc = target + WaitLoop + cycle flush + event test を行う
                iv2BranchFinalize_Taken(s_branch_target);
            } else {
                s_link_slot = &const_cast<Block*>(current_block)->link_not_taken;
                cpuRegs.pc = (u32)c.imm;   // fall_through = branch_pc + 8
                // [iv2 fix 2026-05-30] intCpu の not-taken 挙動を per-branch-type で再現:
                //   BEQ/BNE (Op_BCheckEq/Ne) not-taken → intEventTest()
                //   BGEZ/BGTZ/BLEZ/BLTZ/BC1F/BC1T 等 not-taken → 何もしない (intCpu はそうしている)
                // 従来は全 not-taken で intEventTest を呼び、EEoCycle 追跡が intCpu と乖離 →
                // IOP catchup の EEsCycle がズレ → freeze。s_branch_evt_nt は BCheck op が設定。
                if (s_branch_evt_nt)
                    ::intEventTestGated();   // [EE event-test gate 2026-06-07] not-taken BEQ/BNE も gate (既定 OFF=従来同値)
            }
            // BFinalize 後はブロック末尾なので loop を抜ける
            return;

        default:
            cpuRegs.code = c.raw;
            {
                // [iv2 per-inst cycle fix 2026-05-31] per-inst charge 化により、block 途中で
                // 同期例外 (syscall/TLB miss/overflow 等) が pc を変えても、charge は実行済み命令
                // 分のみ (未実行分は loop 未到達で未 charge) = intCpu と自動一致。旧 refund hack 廃止。
                // 例外で pc が変わったら残り命令は実行せず block を抜ける (誤実行防止)。
                const u32 pc_expected = cpuRegs.pc;   // = op_pc + 4
                if (c.handler) c.handler();
                if (__builtin_expect(cpuRegs.pc != pc_expected, 0) && (i + 1u) < n)
                    return;   // 同期例外で pc 変化 → block 中断 (cpuException の handler へ)
            }
            break;
        }
#if IV2_EE_HOT_DIAG
        // [iv2 MMI self-check] native 結果を同入力 interpreter reference と比較。不一致を log し
        // reference (= 正しい結果) を採用して続行 (= 真因 op 確定 + 同時に fix 効果の検証)。
        if (__builtin_expect(sc_on, 0)) {
            const GPR_reg sc_nat = gpr[c.dest];
            gpr[c.dest] = sc_pre;
            cpuRegs.code = c.raw;
            R5900::GetInstruction(c.raw).interpret();   // gpr[c.dest] = reference 結果
            const GPR_reg sc_ref = gpr[c.dest];
            if (sc_nat.UD[0] != sc_ref.UD[0] || sc_nat.UD[1] != sc_ref.UD[1]) {
                static int s_mck_n = 0;
                if (s_mck_n < 80)
                    Console.WriteLn("@@QA_MMICK@@ n=%d op=%u raw=%08x nat=%016llx_%016llx ref=%016llx_%016llx",
                        s_mck_n++, (unsigned)c.op, c.raw,
                        (unsigned long long)sc_nat.UD[1], (unsigned long long)sc_nat.UD[0],
                        (unsigned long long)sc_ref.UD[1], (unsigned long long)sc_ref.UD[0]);
            }
            // gpr[c.dest] は sc_ref (reference) のまま続行
        }
#endif // IV2_EE_HOT_DIAG
    }
    s_bench_inst_count += n;
}

// ───────────────────────────────────────────────────────────────
// サイクル会計 + イベントテスト
// Interpreter.cpp の intUpdateCPUCycles / intEventTest と同じロジックを
// インライン再実装 (static 関数のため直接呼べない)。
// ───────────────────────────────────────────────────────────────
// cpuBlockCycles を cpuRegs.cycle に反映
static __attribute__((always_inline)) void flush_block_cycles()
{
    const bool lowcycles = (cpuBlockCycles <= 40);
    const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
    u32 scale_cycles = 0;

    if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
        scale_cycles = cpuBlockCycles >> 3;
    else if (cyclerate > 1)
        scale_cycles = cpuBlockCycles >> (2 + cyclerate);
    else if (cyclerate == 1)
        scale_cycles = (u32)(((float)(cpuBlockCycles >> 3)) / 1.3f);
    else if (cyclerate == -1)
        scale_cycles = (cpuBlockCycles <= 80 || cpuBlockCycles > 168 ? 5 : 7) * cpuBlockCycles / 32;
    else
        scale_cycles = ((5 + (-2 * (cyclerate + 1))) * cpuBlockCycles) >> 5;

    cpuRegs.cycle += (scale_cycles < 1) ? 1 : scale_cycles;

    if (cyclerate > 1)
        cpuBlockCycles &= (0x1 << (cyclerate + 2)) - 1;
    else
        cpuBlockCycles &= 0x7;
}

static __attribute__((always_inline)) void update_cycles_and_event_test()
{
    // intUpdateCPUCycles 相当
    const bool lowcycles = (cpuBlockCycles <= 40);
    const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
    u32 scale_cycles = 0;

    if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
        scale_cycles = cpuBlockCycles >> 3;
    else if (cyclerate > 1)
        scale_cycles = cpuBlockCycles >> (2 + cyclerate);
    else if (cyclerate == 1)
        scale_cycles = (u32)(((float)(cpuBlockCycles >> 3)) / 1.3f);
    else if (cyclerate == -1)
        scale_cycles = (cpuBlockCycles <= 80 || cpuBlockCycles > 168 ? 5 : 7) * cpuBlockCycles / 32;
    else
        scale_cycles = ((5 + (-2 * (cyclerate + 1))) * cpuBlockCycles) >> 5;

    cpuRegs.cycle += (scale_cycles < 1) ? 1 : scale_cycles;

    if (cyclerate > 1)
        cpuBlockCycles &= (0x1 << (cyclerate + 2)) - 1;
    else
        cpuBlockCycles &= 0x7;

    // JIT 流: nextEventCycle でゲートし、event test 発火頻度を大幅削減
    // Interpreter は per-branch で event test を呼んでいたが (150× 頻度),
    // cpuRegs.nextEventCycle まで進んだ場合のみ発火する JIT 流が本来の正解。
    if (__builtin_expect((s32)(cpuRegs.cycle - cpuRegs.nextEventCycle) >= 0, 0)) {
        if (__builtin_expect(g_iv2_profile_enabled, 0)) s_evt_fired++;
        _cpuEventTest_Shared();
    }

    // [P101-3R PROFILE] 定期的に dump (block 数ベース、~64k block = 数秒相当)
    if (__builtin_expect(g_iv2_profile_enabled && (s_block_total & 0xFFFF) == 0 && s_block_total != 0, 0)) {
        iv2_profile_summary_dump();
    }

    if (__builtin_expect(s_exit_requested, 0)) {
        s_exit_requested = false;
        fastjmp_jmp(&s_jmp, 1);
    }
}

// ───────────────────────────────────────────────────────────────
// ブート時のフック判定 (ブロック間の安価な pc チェック)
// intExecute の !HasBootedELF 分岐と等価だが、
// ブロック境界でのみ評価 (per-inst ではなく per-block)。
// ───────────────────────────────────────────────────────────────
static __attribute__((always_inline)) bool check_boot_hooks()
{
    if (VMManager::Internal::HasBootedELF())
        return false;

    const u32 elf_entry_point = VMManager::Internal::GetCurrentELFEntryPoint();
    const u32 pc = cpuRegs.pc;

    if (pc == EELOAD_START) {
        const u32 mainjump = memRead32(EELOAD_START + 0x9c);
        if (mainjump >> 26 == 3)
            g_eeloadMain = ((EELOAD_START + 0xa0) & 0xf0000000U) | (mainjump << 2 & 0x0fffffffU);
    } else if (g_eeloadMain != 0u && pc == g_eeloadMain) {
        eeloadHook();
        if (VMManager::Internal::IsFastBootInProgress()) {
            const u32 typeAexecjump = memRead32(EELOAD_START + 0x470);
            const u32 typeBexecjump = memRead32(EELOAD_START + 0x5B0);
            const u32 typeCexecjump = memRead32(EELOAD_START + 0x618);
            const u32 typeDexecjump = memRead32(EELOAD_START + 0x600);
            if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) || (typeDexecjump >> 26 == 3))
                g_eeloadExec = EELOAD_START + 0x2B8;
            else if (typeAexecjump >> 26 == 3)
                g_eeloadExec = EELOAD_START + 0x170;
        }
    } else if (g_eeloadExec != 0u && pc == g_eeloadExec) {
        eeloadHook2();
    } else if (pc == elf_entry_point) {
        VMManager::Internal::EntryPointCompilingOnCPUThread();
    }
    return true;
}

// ───────────────────────────────────────────────────────────────
// Execute (公開)
// 常にブロック経路を使う (ブート中もブロック境界で pc フックを評価)
// ───────────────────────────────────────────────────────────────

// ── [P101-16 probe 2026-08-07] EE ブロック実行の集中度 ─────────────────────────
//   env `iPSX2_EE_BLOCKHIST=1` のときだけ、entry_pc 別のブロック実行回数を数える。
//   目的: **EE 側 T1 型変換 (VU1 と同じ「解釈オーバーヘッド除去」) の GO/NO-GO 判定**。
//   P101-13 §4 rank 7 が「EE 変換の前提計測」として挙げながら未実装だった項目。
//   VU1 は 3 program で 96% という極端な集中があったから T1 が成立した。EE も同様に
//   集中していれば変換が成立し、平坦なら成立しない — それを実測で決める。
//   diagnostic-only / env-gated / 読み取り専用 / 既定は predictable な分岐 1 本のみ。
//   撤去条件: EE 変換の GO/NO-GO 確定後に削除可 (結果は docs/report/P101-16 に残す)。
// compile-gate: 既定 0 = EE ホットループの機械語は probe 導入前と同一 (分岐すら生えない)。
// 計測時のみ `#define IV2_EE_BLOCKHIST 1` に書き換えてビルドする (P101-14 Phase 0 と同じ流儀)。
#ifndef IV2_EE_BLOCKHIST
#define IV2_EE_BLOCKHIST 0
#endif
#if IV2_EE_BLOCKHIST
static const bool s_ee_blockhist = iPSX2_GetRuntimeEnvBool("iPSX2_EE_BLOCKHIST", false);
#else
static constexpr bool s_ee_blockhist = false;
#endif
static constexpr u32 kEEBH = 1u << 17;         // 131072 スロット (open addressing)
static u32 s_bh_pc[kEEBH];
static u64 s_bh_n[kEEBH];
static u64 s_bh_total = 0;

static void ee_blockhist_dump()
{
    // 連番で吐く。**連続する 2 つを差分すると任意区間 (ムービー中など) の分布が得られる**
    // (累積のままだと boot と本編が混ざって区間比較できない)。最新は固定名にも出す。
    static int s_seq = 0;
    char path[128];
    std::snprintf(path, sizeof(path), "/tmp/ipsx2_ee_blockhist_%03d.txt", s_seq++);
    for (const char* pp : { (const char*)path, "/tmp/ipsx2_ee_blockhist.txt" }) {
    FILE* f = std::fopen(pp, "w");
    if (!f) return;
    struct E { u32 pc; u64 n; };
    std::vector<E> v;
    for (u32 i = 0; i < kEEBH; ++i)
        if (s_bh_n[i]) v.push_back(E{s_bh_pc[i], s_bh_n[i]});
    std::sort(v.begin(), v.end(), [](const E& a, const E& b) { return a.n > b.n; });
    std::fprintf(f, "EE BLOCK HIST  distinct_entry_pc=%zu  total_block_exec=%llu\n",
                 v.size(), (unsigned long long)s_bh_total);
    const double tot = s_bh_total ? (double)s_bh_total : 1.0;
    u64 acc = 0;
    const int marks[] = {1, 10, 50, 100, 200, 500, 1000, 2000, 5000};
    int mi = 0;
    std::fprintf(f, "--- cumulative coverage ---\n");
    for (size_t i = 0; i < v.size(); ++i) {
        acc += v[i].n;
        while (mi < 9 && (int)(i + 1) == marks[mi]) {
            std::fprintf(f, "  top-%-5d  %6.2f%%\n", marks[mi], 100.0 * acc / tot);
            ++mi;
        }
    }
    std::fprintf(f, "  all(%zu)   100.00%%\n", v.size());
    std::fprintf(f, "--- top 40 blocks ---\n");
    for (size_t i = 0; i < v.size() && i < 40; ++i)
        std::fprintf(f, "  pc=0x%08x  n=%12llu  %6.3f%%\n",
                     v[i].pc, (unsigned long long)v[i].n, 100.0 * v[i].n / tot);
    // 機械可読 (区間差分用): 全エントリ
    std::fprintf(f, "--- raw ---\n");
    for (size_t i = 0; i < v.size(); ++i)
        std::fprintf(f, "%08x %llu\n", v[i].pc, (unsigned long long)v[i].n);
    std::fclose(f);
    }
}

static __fi void ee_blockhist_add(u32 pc)
{
    const u32 h = (pc * 2654435761u) & (kEEBH - 1);
    for (u32 pr = 0; pr < 64; ++pr) {
        const u32 i = (h + pr) & (kEEBH - 1);
        if (s_bh_n[i] == 0) { s_bh_pc[i] = pc; s_bh_n[i] = 1; break; }
        if (s_bh_pc[i] == pc) { ++s_bh_n[i]; break; }
    }
    if ((++s_bh_total & 0x7FFFFFull) == 0) ee_blockhist_dump();   // 約 840 万ブロック毎
}

[[gnu::hot]] void Execute()
{
    static int s_exec_count = 0;
    if (s_exec_count < 3) {
        ++s_exec_count;
        Console.Error("@@IV2_EXEC@@ iv2::Execute() call#%d pc=%08x booted=%d",
            s_exec_count, cpuRegs.pc, (int)VMManager::Internal::HasBootedELF());
    }

    // [P101-3R] Reserve 経由で Init が呼ばれない経路対応。最初の Execute で env を確定。
    static bool s_p3r_env_done = false;
    if (__builtin_expect(!s_p3r_env_done, 0)) {
        s_p3r_env_done = true;
        g_iv2_profile_enabled = iPSX2_GetRuntimeEnvBool("iPSX2_IV2_PROFILE", false);
        s_vu0_diff_enabled    = iPSX2_GetRuntimeEnvBool("iPSX2_IV2_VU0_DIFF", false);
        g_iv2_mmi_selfcheck = iPSX2_GetRuntimeEnvBool("iPSX2_IV2_MMI_SELFCHECK", false);
        if (g_iv2_profile_enabled) std::memset(s_op_count, 0, sizeof(s_op_count));
        fprintf(stderr, "@@IV2_P101_3R@@ profile=%d vu0_diff=%d\n",
            g_iv2_profile_enabled ? 1 : 0,
            s_vu0_diff_enabled ? 1 : 0);
    }

    if (fastjmp_set(&s_jmp) != 0)
        return;

    // intExecute のホットループと同じ思想:
    //   steady-state では branch handler が doBranch → intEventTest を呼ぶため
    //   ブロック境界で追加の event test は不要。
    //   cycle 会計は Fallback 経由の branch 処理で intUpdateCPUCycles() が内部発火する。
    //
    // ブート中 (HasBootedELF=false) は check_boot_hooks をブロック毎に評価するが、
    // ELF 起動後はフック不要なのでフラグ分岐でタイト路にする。
    while (!VMManager::Internal::HasBootedELF()) {
        Block* blk;
        // [Phase H] 前ブロック末尾が指定した link slot を確認
        Block** slot = s_link_slot;
        s_link_slot = nullptr;
        Block* cached = slot ? *slot : nullptr;
        if (__builtin_expect(cached != nullptr
                && cached->entry_pc == cpuRegs.pc
                && cached->generation == g_generation
                // [iv2 SMC fix 2026-05-31] link-slot cache も content 検証 (RAM 領域のみ)。
                // generation だけ信頼すると store/DMA で書換わった stale block を実行し続ける。
                // [P101-14 E1 2026-08-06] 読みを iv2_read32 (L0 inline) に (block_content_valid と同じ理由)
                && ((cpuRegs.pc & 0x1FFFFFFFu) >= 0x02000000u
                    || g_inst_pool[cached->inst_offset].raw == iv2_read32(cpuRegs.pc)), 1)) {
            blk = cached;
        } else {
            blk = GetOrCompileBlock(cpuRegs.pc);
            // first miss: link slot に install (以降高速パス)
            if (slot) *slot = blk;
        }
        current_block = blk;
        if (__builtin_expect(s_ee_blockhist, 0)) ee_blockhist_add(blk->entry_pc);
        run_block(blk);
        check_boot_hooks();
        if (__builtin_expect(s_exit_requested, 0)) {
            s_exit_requested = false;
            fastjmp_jmp(&s_jmp, 1);
        }
    }
    // ELF 起動後のホットループ (最大単純化 + Phase H リンク)
    while (true) {
        // [P101-4 Stage C] Drain async-VU1 worker-deferred EE effects (VGW/CPU_INT,
        // VU1 interrupts, GIF SIGNAL/FINISH) on the EE's main loop — ungated so a
        // VIF/GIF stall cannot livelock waiting for a deferred reschedule. Throttled
        // to every Nth block to keep the hot loop cheap (still ~10^5/s, ample for
        // timely rescheduling). Zero cost unless true-async (mode 1) is active.
        if (__builtin_expect(g_iv2_async_vu1_mode == 1, 0))
        {
            static u32 s_drain_ctr = 0;
            if ((++s_drain_ctr & 0x3F) == 0)
                iv2AsyncVu1_DrainDeferred();
        }
        Block* blk;
        Block** slot = s_link_slot;
        s_link_slot = nullptr;
        Block* cached = slot ? *slot : nullptr;
        if (__builtin_expect(cached != nullptr
                && cached->entry_pc == cpuRegs.pc
                && cached->generation == g_generation
                // [iv2 SMC fix 2026-05-31] link-slot cache も content 検証 (RAM 領域のみ)。
                // [P101-14 E1 2026-08-06] 読みを iv2_read32 (L0 inline) に (block_content_valid と同じ理由)
                && ((cpuRegs.pc & 0x1FFFFFFFu) >= 0x02000000u
                    || g_inst_pool[cached->inst_offset].raw == iv2_read32(cpuRegs.pc)), 1)) {
            blk = cached;
            // [P101-3R PROFILE] link hit 計測 (slot の向きで taken/not_taken を区別)
            if (__builtin_expect(g_iv2_profile_enabled, 0)) {
                // s_link_slot が &blk->link_taken or &blk->link_not_taken の場合に対応
                // 簡易判定: BFinalize の taken path 直後 or not-taken direct
                // ここでは taken/not_taken 判別が難しいので link_hit 総数のみ
                s_link_taken_hit++;  // 同一 slot 経由 = link hit
            }
        } else {
            blk = GetOrCompileBlock(cpuRegs.pc);
            if (slot) *slot = blk;
            if (__builtin_expect(g_iv2_profile_enabled, 0)) s_link_miss++;
        }
        current_block = blk;
        if (__builtin_expect(s_ee_blockhist, 0)) ee_blockhist_add(blk->entry_pc);
        run_block(blk);
        if (__builtin_expect(s_exit_requested, 0)) {
            s_exit_requested = false;
            fastjmp_jmp(&s_jmp, 1);
        }
    }
}

} // namespace iv2
