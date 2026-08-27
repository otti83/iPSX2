// SPDX-License-Identifier: GPL-3.0+
// P101: Interpreter v2 — R5900cpu シム
//
// Phase B: 独自 Execute ループ + ブロックキャッシュ。
// その他 (Reserve/Shutdown/Step/Clear) は intCpu を参照または iv2:: 関数にディスパッチ。

#include "PrecompiledHeader.h"
#include "Common.h"
#include "iv2/iv2.h"
#include "R5900.h"

static void iv2Reserve()
{
    iv2::Init();
    intCpu.Reserve();
}

static void iv2Shutdown()
{
    intCpu.Shutdown();
    iv2::Shutdown();
}

static void iv2Reset()
{
    iv2::Reset();
    intCpu.Reset();
}

static void iv2Step()
{
    // 単一命令ステップは intCpu に委譲 (デバッガ用、Phase B では最適化しない)
    intCpu.Step();
}

static void iv2ExecuteWrapper()
{
    static int s_called = 0;
    if (s_called++ < 3)
        Console.Error("@@IV2_SHIM_EXEC@@ iv2ExecuteWrapper #%d", s_called);
    iv2::Execute();
}

static void iv2ReserveProbe()
{
    static int s_called = 0;
    if (s_called++ < 3)
        Console.Error("@@IV2_SHIM_RESERVE@@ iv2ReserveProbe #%d", s_called);
    iv2::Init();
    intCpu.Reserve();
}

static void iv2SafeExitExecutionWrapper()
{
    iv2::SafeExitExecution();
}

static void iv2CancelInstructionWrapper()
{
    iv2::CancelInstruction();
}

static void iv2Clear(u32 addr, u32 size)
{
    iv2::Clear(addr, size);
    // intCpu の Clear は no-op だが、念のため連動
    intCpu.Clear(addr, size);
}

R5900cpu iv2Cpu =
{
    iv2ReserveProbe,
    iv2Shutdown,
    iv2Reset,
    iv2Step,
    iv2ExecuteWrapper,
    iv2SafeExitExecutionWrapper,
    iv2CancelInstructionWrapper,
    iv2Clear,
};
