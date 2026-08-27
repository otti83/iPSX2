// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// This module contains code shared by both the dynarec and interpreter versions
// of the VU0 micro.

#include "Common.h"
#include <cmath>
#include <atomic>
#include <sys/time.h> // [CLIFF_DIAG]
#include "VUmicro.h"
#include "MTVU.h"
#include "Gif_Unit.h" // for gifUnit
#include "iv2/iv2_async_vu1.h" // [P101-4 Stage C] async VU1 worker

// [TEMP_DIAG] VU1 kick counter — defined in Counters.cpp
// Removal condition: BIOS browserafter confirmed
extern std::atomic<uint32_t> g_vu1_kick_count;

#ifdef PCSX2_DEBUG
u32 vudump = 0;
#endif

// This is called by the COP2 as per the CTC instruction
void vu1ResetRegs()
{
	VU0.VI[REG_VPU_STAT].UL &= ~0xff00; // stop vu1
	VU0.VI[REG_FBRST].UL &= ~0xff00; // stop vu1
	vif1Regs.stat.VEW = false;
}

void vu1Finish(bool add_cycles) {
	if (THREAD_VU1) {
		//if (VU0.VI[REG_VPU_STAT].UL & 0x100) DevCon.Error("MTVU: VU0.VI[REG_VPU_STAT].UL & 0x100");
		if (INSTANT_VU1 || add_cycles)
		{
			vu1Thread.WaitVU();
		}
		vu1Thread.Get_MTVUChanges();
		return;
	}
	// [P101-4 Stage C] true-async: drain the worker (+ deferred EE effects) so the
	// EE observes a coherent, idle VU1 state.
	if (g_iv2_async_vu1_mode == 1 && iv2AsyncVu1_IsActive()) {
		iv2AsyncVu1_WaitIdle();
		return;
	}
	u32 vu1cycles = VU1.cycle;
	if(VU0.VI[REG_VPU_STAT].UL & 0x100) {
		VUM_LOG("vu1ExecMicro > Stalling until current microprogram finishes");
		CpuVU1->Execute(vu1RunCycles);
	}
	if (VU0.VI[REG_VPU_STAT].UL & 0x100) {
		DevCon.Warning("Force Stopping VU1, ran for too long");
		VU0.VI[REG_VPU_STAT].UL &= ~0x100;
	}
	if (add_cycles)
	{
		cpuRegs.cycle += VU1.cycle - vu1cycles;
	}
}

void vu1ExecMicro(u32 addr)
{
	// [CLIFF_DIAG] Time the entire vu1ExecMicro call. gettimeofday gated (CliffDiag::timingEnabled,
	// default OFF) to drop the per-kick gettimeofday tax in ship; bit-exact (updates diag counters only).
	struct timeval _vu1_tv0;
	if (__builtin_expect(CliffDiag::timingEnabled, 0)) gettimeofday(&_vu1_tv0, nullptr);

	g_vu1_kick_count.fetch_add(1, std::memory_order_relaxed); // [TEMP_DIAG]

	// [P24] Per-kick drain removed — replaced by VSync-level pacing in Counters.cpp.

	if (THREAD_VU1) {
		VU0.VI[REG_VPU_STAT].UL &= ~0xFF00;
		// Okay this is a little bit of a hack, but with good reason.
		// Most of the time with MTVU we want to pretend the VU has finished quickly as to gain the benefit from running another thread
		// however with T-Bit games when the T-Bit is enabled, it needs to wait in case a T-Bit happens, so we need to set "Busy"
		// We shouldn't do this all the time as it negates the extra thread and causes games like Ratchet & Clank to be no faster.
		// if (VU0.VI[REG_FBRST].UL & 0x800)
		// {
		//	VU0.VI[REG_VPU_STAT].UL |= 0x0100;
		// }
		// Update 25/06/2022: Disabled this for now, let games YOLO it, if it breaks MTVU, disable MTVU (it doesn't work properly anyway) - Refraction
		vu1Thread.ExecuteVU(addr, vif1Regs.top, vif1Regs.itop, VU0.VI[REG_FBRST].UL);
		// [CLIFF_DIAG] end timing (MTVU path — unlikely on interpreter; gated)
		if (__builtin_expect(CliffDiag::timingEnabled, 0))
		{
			struct timeval _vu1_tv1;
			gettimeofday(&_vu1_tv1, nullptr);
			uint32_t dt = (uint32_t)((_vu1_tv1.tv_sec - _vu1_tv0.tv_sec) * 1000000 + (_vu1_tv1.tv_usec - _vu1_tv0.tv_usec));
			CliffDiag::vu1ExecUs.fetch_add(dt, std::memory_order_relaxed);
			CliffDiag::vu1Kicks.fetch_add(1, std::memory_order_relaxed);
		}
		return;
	}
	static int count = 0;

	// [P101-4 Stage C] true-async kick: drain the previous job (so VU1 state is
	// free to overwrite), set up the new kick on the EE, then enqueue and return —
	// the worker runs the microprogram concurrently with the EE.
	if (g_iv2_async_vu1_mode == 1 && iv2AsyncVu1_IsActive())
	{
		// depth>1 feed-ahead: mark VU1 busy (atomic — the worker may concurrently
		// clear a prior kick's bit) and queue this kick with its TOP/ITOP snapshot;
		// the worker sets TPC/cycle and runs it. NOTE: keeping the busy bit set means
		// vif1FLUSH still serialises kicks (no true pipelining) — the MTVU busy-clear
		// alternative pipelines but breaks the game's VU1 completion sync (proven:
		// black-screen stall at ~624 kicks), so it is NOT used.
		__atomic_fetch_or(&VU0.VI[REG_VPU_STAT].UL, 0x100u, __ATOMIC_RELEASE);
		iv2AsyncVu1_Kick((s32)addr, vif1Regs.top, vif1Regs.itop);
		return;
	}

	vu1Finish(false);

	VUM_LOG("vu1ExecMicro %x (count=%d)", addr, count++);
	VU1.cycle = cpuRegs.cycle;
	VU0.VI[REG_VPU_STAT].UL &= ~0xFF00;
	VU0.VI[REG_VPU_STAT].UL |=  0x0100;
	if ((s32)addr != -1) VU1.VI[REG_TPC].UL = addr & 0x7FF;

	CpuVU1->SetStartPC(VU1.VI[REG_TPC].UL << 3);
	_vuExecMicroDebug(VU1);
	if(!INSTANT_VU1)
		CpuVU1->ExecuteBlock(1);
	else
		CpuVU1->Execute(vu1RunCycles);

	// [CLIFF_DIAG] end timing (interpreter path; gated — also feeds HLE_PROBE/SURVEY attribution,
	// which set CliffDiag::timingEnabled so the probe still works when requested).
	if (__builtin_expect(CliffDiag::timingEnabled, 0))
	{
		struct timeval _vu1_tv1;
		gettimeofday(&_vu1_tv1, nullptr);
		uint32_t dt = (uint32_t)((_vu1_tv1.tv_sec - _vu1_tv0.tv_sec) * 1000000 + (_vu1_tv1.tv_usec - _vu1_tv0.tv_usec));
		CliffDiag::vu1ExecUs.fetch_add(dt, std::memory_order_relaxed);
		CliffDiag::vu1Kicks.fetch_add(1, std::memory_order_relaxed);
		// [HLE_PROBE] attribute this kick's VU1 time to its microprogram (start PC = TPC)
		CliffDiag::vu1ProgSample(VU1.Micro, VU1.VI[REG_TPC].UL, dt);
	}
}

void MTVUInterrupt()
{
	VU0.VI[REG_VPU_STAT].UL &= ~0xFF00;
}
