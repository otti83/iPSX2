// SPDX-License-Identifier: GPL-3.0+
// P101: Interpreter v2 — 公開 API
//
// 第一ゴール: BIOS 起動 + intCpu 比 2.0× 速度。
// Phase A: intCpu パススルー (完了)
// Phase B: 独自 Execute ループ + ブロックキャッシュ (事前デコード)
// Phase C 以降: native IR opcode

#pragma once

#include "R5900.h"

extern R5900cpu iv2Cpu;

// Interpreter.cpp から公開される execI() ラッパー (static なので直接参照不可)
extern void interpExecOneInstruction();

namespace iv2 {

void Init();
void Shutdown();
void Reset();
void Execute();                 // Phase B 以降は独自ループ
void SafeExitExecution();
void CancelInstruction();
void Clear(u32 addr, u32 size); // ブロックキャッシュ無効化

} // namespace iv2
