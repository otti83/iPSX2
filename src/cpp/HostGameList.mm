// HostGameList.mm — game list refresh for both Apple hosts
// SPDX-License-Identifier: GPL-3.0+
//
// [F8] GameList::Refresh has exactly one entry point in upstream — Host::RefreshGameListAsync —
// and both Apple hosts left it an empty body. Nothing else in the tree calls Refresh, so
// GameList::s_entries was permanently empty and every metadata feature built on it (title,
// serial, CRC, region, compatibility, cover art, last played, total play time, custom title
// and region) was dead no matter what the UI did. The frontend worked around it by listing
// raw ISO filenames from two hardcoded directories.
//
// Scanning is deliberately refused while a VM is running: GameList::GetIsoSerialAndCRC
// assigns the global CDVD pointer (GameList.cpp:220), which is why GameList.h warns not to
// call it with the system live.

#include "HostGameList.h"

#include "pcsx2/Host.h"
#include "pcsx2/GameList.h"
#include "pcsx2/VMManager.h"

#include "common/Console.h"
#include "common/ProgressCallback.h"

#import <Foundation/Foundation.h>

#include <atomic>
#include <cstdlib>
#include <thread>

NSString* const iPSX2GameListDidRefreshNotification = @"iPSX2GameListDidRefresh";

namespace
{
	// One refresh at a time. A second request while one is in flight is dropped rather than
	// queued: the result would be identical and scanning touches global CDVD state.
	std::atomic<bool> s_refreshing{false};
	std::atomic<bool> s_cancel{false};

	// Lets Host::CancelGameListRefresh actually interrupt a scan — GameList::Refresh checks
	// IsCancelled between directories and between files.
	class CancellableProgress final : public ProgressCallback
	{
	public:
		void PushState() override {}
		void PopState() override {}
		bool IsCancelled() const override { return s_cancel.load(std::memory_order_relaxed); }
		bool IsCancellable() const override { return true; }
		void SetCancellable(bool) override {}
		void SetTitle(const char*) override {}
		void SetStatusText(const char*) override {}
		void SetProgressRange(u32 range) override { m_range = range; }
		void SetProgressValue(u32 value) override { m_value = value; }
		void IncrementProgressValue() override { m_value++; }
		void SetProgressState(ProgressState) override {}
		void DisplayError(const char* message) override { Console.Error("[GameList] %s", message); }
		void DisplayWarning(const char* message) override { Console.Warning("[GameList] %s", message); }
		void DisplayInformation(const char* message) override { Console.WriteLn("[GameList] %s", message); }
		void DisplayDebugMessage(const char* message) override { DevCon.WriteLn("[GameList] %s", message); }
		void ModalError(const char* message) override { Console.Error("[GameList] %s", message); }
		bool ModalConfirmation(const char*) override { return false; }
		void ModalInformation(const char* message) override { Console.WriteLn("[GameList] %s", message); }

	private:
		u32 m_range = 1;
		u32 m_value = 0;
	};
} // namespace

void Host::RefreshGameListAsync(bool invalidate_cache)
{
	// Scanning opens discs through the global CDVD reader, so it cannot run against a live VM.
	if (VMManager::HasValidVM())
	{
		Console.Warning("[GameList] Refresh requested while a VM is running; ignored.");
		return;
	}

	bool expected = false;
	if (!s_refreshing.compare_exchange_strong(expected, true))
	{
		Console.WriteLn("[GameList] Refresh already in progress; ignored.");
		return;
	}

	s_cancel.store(false);
	std::thread([invalidate_cache]() {
		CancellableProgress progress;
		GameList::Refresh(invalidate_cache, false, &progress);
		const u32 count = GameList::GetEntryCount();
		Console.WriteLn("[GameList] Refresh finished: %u entries.", count);
		s_refreshing.store(false);

		// The Swift layer listens for this; see the iPSX2VMDidShutdown pattern in AppState.
		dispatch_async(dispatch_get_main_queue(), ^{
			[[NSNotificationCenter defaultCenter] postNotificationName:iPSX2GameListDidRefreshNotification
															   object:nil];
		});
	}).detach();
}

void Host::CancelGameListRefresh()
{
	if (!s_refreshing.load())
		return;

	s_cancel.store(true);
	Console.WriteLn("[GameList] Refresh cancellation requested.");
}

namespace iPSX2
{
	bool IsGameListRefreshing()
	{
		return s_refreshing.load();
	}
}

// --- Diagnostic scan (default OFF, zero perturbation when unset) --------------------------
//
// Why this exists: Host::RefreshGameListAsync has no caller until the game-list UI is built,
// so without a trigger there is no way to show that scanning actually works. This runs one
// synchronous scan before the VM starts and prints what it found, which is exactly what the
// UI will later display.
//
//   iPSX2_GAMELIST_SCAN=1   scan the configured directories and print every entry
//
// Removal condition: delete once the game-list UI drives refresh and can be inspected
// directly. Until then this is the only way to verify GameList end to end.
void iPSX2::MaybeRunDiagnosticGameListScan()
{
	const char* env = std::getenv("iPSX2_GAMELIST_SCAN");
	if (!env || env[0] == '0')
		return;

	Console.WriteLn("[GameListScan] scanning...");
	CancellableProgress progress;
	GameList::Refresh(true, false, &progress);

	auto lock = GameList::GetLock();
	const u32 count = GameList::GetEntryCount();
	Console.WriteLn("[GameListScan] entries=%u", count);
	for (u32 i = 0; i < count; i++)
	{
		const GameList::Entry* e = GameList::GetEntryByIndex(i);
		if (!e)
			continue;
		Console.WriteLn("[GameListScan] serial=%s crc=%08X region=%s type=%s compat=%d size=%llu title=%s",
			e->serial.c_str(), e->crc, GameList::RegionToString(e->region, false),
			GameList::EntryTypeToString(e->type, false), static_cast<int>(e->compatibility_rating),
			static_cast<unsigned long long>(e->total_size), e->GetTitle().c_str());
		Console.WriteLn("[GameListScan]   cover=%s", GameList::GetCoverImagePathForEntry(e).c_str());
	}
}
