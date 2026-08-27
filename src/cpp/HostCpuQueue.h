// HostCpuQueue.h — deferred work queue for the emulation (CPU) thread
// SPDX-License-Identifier: GPL-3.0+
//
// [F1] Upstream PCSX2 frontends implement Host::RunOnCPUThread by handing a closure to the
// emulation thread and letting it run at a safe point. Both Apple hosts got this wrong:
// the iOS build had an empty body (work vanished with no error) and the macOS build ran the
// closure straight on the caller's thread (wrong thread affinity for anything touching VM
// state). Every UI action that has to reach the emulator goes through here — save states,
// memory cards, disc changes, patch reload, settings re-apply — so both defects silently
// void whatever is built on top of them.
//
// This is the single definition for both Apple targets. Host::RunOnCPUThread is defined in
// HostCpuQueue.cpp and must NOT be redefined in ios_main.mm or MacOSStubs.cpp.

#pragma once

namespace iPSX2
{
	/// Runs everything queued by Host::RunOnCPUThread. MUST be called at the very top of
	/// Host::PumpMessagesOnCPUThread — both implementations return early further down, so a
	/// drain placed after those points would be skipped for the rest of the session.
	/// Calling this also marks the calling thread as the CPU thread.
	void DrainCPUThreadTasks();

	/// True when called from the thread that most recently ran DrainCPUThreadTasks.
	bool IsCPUThread();

	/// Drops anything still queued and forgets the CPU thread. Call when the VM is torn down
	/// so a stale closure cannot run against a destroyed VM.
	void ResetCPUThreadQueue();
}
