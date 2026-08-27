// SPDX-License-Identifier: GPL-3.0+
// P101-2 Phase T/V: iv2 VU (VU1/VU0) 単位型 + ブロックキャッシュ
//
// 方針:
//  - BaseVUmicroCPU を継承した CpuIv2VU0/CpuIv2VU1 を提供。
//    ─ VU semantics (pipe/stall/branch/E-D-T flag/XGKICK/non-IEEE float) は極めて複雑で
//      一括 native 化は高リスク。Phase T/V は構造導入 + pre-decode table を用意し、
//      実行本体は既存の vu0Exec/vu1Exec に委譲する。
//    ─ NEON FMAC は今後のイテレーションで漸進的に native 化 (この class 内の path 追加のみで可能)。
//  - Phase S (MTVU for iv2) と共存: MTVU worker は CpuVU1->Execute() を呼ぶため、
//    CpuIv2VU1 は thread-safe (別 thread で 1 つだけ走る前提)。
//
// 補足:
//  - pre-decode table は VU 内のプログラム ROM (VU0:4KB / VU1:16KB) を 8B 単位で index 化。
//    現在の interpreter は dispatch table に関数ポインタで飛ぶ (branch prediction miss 大)。
//    将来 NEON 化時はここの entry を書き換えるだけで済む。

#pragma once

#include "VUmicro.h"

class CpuIv2VU0 final : public BaseVUmicroCPU
{
public:
    CpuIv2VU0();
    ~CpuIv2VU0() override { Shutdown(); }

    const char* GetShortName() const override { return "iv2VU0"; }
    const char* GetLongName()  const override { return "VU0 iv2 (Phase V)"; }

    void Shutdown() override {}
    void Reset() override;

    void SetStartPC(u32 startPC) override;
    void Step() override;
    void Execute(u32 cycles) override;
    void Clear(u32 addr, u32 size) override;
};

class CpuIv2VU1 final : public BaseVUmicroCPU
{
public:
    CpuIv2VU1();
    ~CpuIv2VU1() override { Shutdown(); }

    const char* GetShortName() const override { return "iv2VU1"; }
    const char* GetLongName()  const override { return "VU1 iv2 (Phase T)"; }

    void Shutdown() override {}
    void Reset() override;

    void SetStartPC(u32 startPC) override;
    void Step() override;
    void Execute(u32 cycles) override;
    void Clear(u32 addr, u32 size) override;
    void ResumeXGkick() override {}

    // [P101-4] Synchronous VU1 run body. Execute() dispatches here (sync mode)
    // or hands `cycles` to the async worker (iPSX2_IV2_ASYNC_VU1), which then
    // calls this on its own thread. Both paths share identical code = bit-exact.
    void ExecuteSync(u32 cycles);
};

extern CpuIv2VU0 CpuIv2VU0Inst;
extern CpuIv2VU1 CpuIv2VU1Inst;
