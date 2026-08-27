// SPDX-License-Identifier: GPL-3.0+
// P101-2 Phase T 真実装: VU1 Upper/Lower opcode の NEON native 実装
//
// Upper (UpperDispatch):
//   - ADD/SUB/MUL/MADD/MSUB/MAX/MINI の 4-way / bc (x/y/z/w) / i / q 変種
//   - ACC-dest 系: ADDA/SUBA/MULA/MADDA/MSUBA + bc/i/q
//   - ABS, NOP
//   - 未対応: OPMSUB, OPMULA, CLIP, FTOI*, ITOF*, ADDi/SUBi TriAce hack (全て fallback)
//
// Lower (LowerDispatch):
//   - メモリアクセス: LQ/LQI/LQD, SQ/SQI/SQD, ILW/ILWR, ISW/ISWR
//   - レジスタ間: MOVE, MR32, MFIR, MTIR, MFP
//   - 整数 ALU: IADD, ISUB, IADDI, IADDIU, ISUBIU, IAND, IOR
//   - 分岐: IBEQ, IBNE, IBGEZ, IBGTZ, IBLEZ, IBLTZ, B, BAL, JR, JALR
//   - R系: RINIT, RGET, RNEXT, RXOR
//   - FLAG: FSAND, FSEQ, FSOR, FMAND, FMEQ, FMOR, FCAND, FCEQ, FCOR, FCSET, FCGET
//   - その他: WAITQ, WAITP, XITOP, XTOP, NOP
//   - 未対応: DIV, SQRT, RSQRT (FPU pipeline setup)、E系 (ESADD/EATAN等)、
//            XGKICK (gifUnit 副作用)、FSSET (statusflag 書換で注意必要) — fallback
//
// 有効化:
//   - iPSX2_VU_IV2_OPS_UPPER=1 : Upper native
//   - iPSX2_VU_IV2_OPS_LOWER=1 : Lower native
//   - iPSX2_VU_IV2_DIFF=1     : native 実行後に interpreter 実行して結果 bit-exact 比較
//                                (Upper のみ対応。Lower は将来拡張)

#pragma once

struct VURegs;

namespace iv2 {
namespace vu_ops {

void Init();

// [P101-12 dispatch-frame collapse] The fast path (enabled && !diff -> *DispatchNative) is HEADER-INLINED into the
// caller so the per-instruction cross-TU wrapper frame disappears. The old UpperDispatch/LowerDispatch were exported
// (external-linkage) symbols that resisted LTO inlining — the profile (GTA heavy-3D) showed ~4.4% of VU1 stuck in
// those wrapper frames. Bit-exact: identical dispatch decision (enabled && !diff -> Native), only hoisted; the cold
// path (!enabled or diff-mode) stays out-of-line in *DispatchFull (verbatim original body).
extern bool g_iv2vu_upper_en;   // set once in Init() from iPSX2_VU_IV2_OPS_UPPER (default true)
extern bool g_iv2vu_lower_en;
extern bool g_iv2vu_diff_en;    // iPSX2_VU_IV2_DIFF (default false; dev-only native-vs-interp comparison)
bool UpperDispatchNative(VURegs* VU);   // the NEON switch (iv2_vu_ops.cpp)
bool LowerDispatchNative(VURegs* VU);
bool UpperDispatchFull(VURegs* VU);     // cold path: !enabled or diff-mode (original wrapper body)
bool LowerDispatchFull(VURegs* VU);

// Upper opcode を native 実行。戻り値 true = 処理完了 (caller が fallback 呼ぶ必要なし)。
// always_inline: plain `inline` was kept out-of-line by LTO (de-dup), leaving the wrapper frame in the
// hot iv2_vu1Exec path — force-inline so the fast-path check folds into the caller (the cross-TU call
// to UpperDispatchNative remains, as it always did).
__attribute__((always_inline)) inline bool UpperDispatch(VURegs* VU)
{
    if (__builtin_expect(g_iv2vu_upper_en && !g_iv2vu_diff_en, 1))
        return UpperDispatchNative(VU);
    return UpperDispatchFull(VU);
}

// [P101-15] オフライン変換 (T1)。default OFF、env `iPSX2_VU1_T1=1` で有効。
extern bool g_iv2_t1_enabled;   // env 1 回読み。false なら呼び手は hash 計算すらしない
//   T1_ProgramReady: 現 microprogram が変換済み & 前提検証 OK か (kick 毎に 1 回)
//   T1_TryRun:       TPC が変換済み PC なら特殊化本体を回して true。範囲外は false。
bool T1_ProgramReady(u64 hash);
bool T1_TryRun(VURegs* VU, u32 tpc, u32 startcycles, u32 cycles);

// [P101-18 A] T1 の「償却」を測る診断 (env `iPSX2_VU1_T1_STATS=1`、既定 OFF・摂動ゼロ)。
//   T1 の利得は **命令あたり** (fetch/decode/dispatch 除去) だが、
//   コストは **入場あたり** (間接 call + body 内 switch の間接分岐)。
//   したがって効くかどうかは窓の大きさではなく **1 回の入場で何命令流せるか** で決まる。
//   窓の断片化が損か得かを判定するための唯一の直接指標。除去条件 = 目的関数が確定したら。
extern bool g_t1_stats_on;
extern u64 g_t1_entries;      // T1 body に入った回数
extern u64 g_t1_insns;        // T1 が実行した VU 命令数
extern u64 g_t1_bail_insns;   // T1 有効な program 下でインタプリタが実行した命令数
void T1_StatsDump();

// Lower opcode を native 実行。戻り値 true = 処理完了。
__attribute__((always_inline)) inline bool LowerDispatch(VURegs* VU)
{
    if (__builtin_expect(g_iv2vu_lower_en && !g_iv2vu_diff_en, 1))
        return LowerDispatchNative(VU);
    return LowerDispatchFull(VU);
}

bool UpperEnabled();
bool LowerEnabled();

// [P101-5 Phase T Lower] Emit top-N fallback histogram lines when
// iPSX2_VU_IV2_LFB_PROF=1 is set. No-op otherwise.
void LowerFallbackLogTop(int n);

} // namespace vu_ops
} // namespace iv2
