// HostGameList.h — game list refresh for both Apple hosts
// SPDX-License-Identifier: GPL-3.0+
// See HostGameList.mm. Host::RefreshGameListAsync / Host::CancelGameListRefresh are declared
// in pcsx2/Host.h and defined there; this header carries only the frontend-facing extras.

#pragma once

namespace iPSX2
{
	/// True while a background game-list scan is in flight.
	bool IsGameListRefreshing();

	/// Runs one synchronous scan and prints every entry, when iPSX2_GAMELIST_SCAN=1.
	/// Default OFF and zero-perturbation otherwise. Must be called with no VM alive, because
	/// scanning reassigns the global CDVD reader.
	void MaybeRunDiagnosticGameListScan();
}
