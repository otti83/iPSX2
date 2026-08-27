// GameListPaths.h — which directories the game library scans, and how those
// entries survive an iOS container change.
// SPDX-License-Identifier: GPL-3.0+
//
// GameList::Refresh reads exactly two INI keys and nothing else:
//
//   [GameList] Paths           — each directory scanned one level deep
//   [GameList] RecursivePaths  — each directory scanned including subfolders
//
// Both host branches of ios_main.mm seed them through SeedGameListPaths below, so the
// rule lives in one place instead of being written twice with a subtle difference.

#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "common/Console.h"
#include "pcsx2/INISettingsInterface.h"

namespace iPSX2::GameListPaths
{
	inline constexpr const char* kSection = "GameList";
	inline constexpr const char* kFlatKey = "Paths";           // one level deep
	inline constexpr const char* kRecursiveKey = "RecursivePaths"; // whole tree

	/// True when `path` names a directory inside an iOS app container that is not ours.
	///
	/// Absolute paths into an app container do not survive on iOS — the container UUID changes
	/// on reinstall, and on device across app updates and restores — so an entry of this shape
	/// is pointing at a directory that no longer exists, and it is dead weight in the scan list.
	///
	/// These entries are DROPPED rather than rewritten onto the live container. Rewriting looks
	/// helpful and is not: the only way to tell "a stale copy of our own Documents" from "a
	/// folder the user picked inside some other app's container" is the container UUID, which is
	/// exactly the part that changed — so a rewrite would silently move the user's chosen folder
	/// onto ours. Dropping is safe because nothing is lost: the two built-in locations are
	/// re-seeded from the live dataRoot immediately below, and a folder the user added is
	/// re-registered from its security-scoped bookmark on every launch (ScanFolderStore).
	inline bool IsDeadContainerPath(const std::string& path, const std::string& dataRoot)
	{
		static constexpr std::string_view kContainer = "/Containers/Data/Application/";

		if (path.rfind(dataRoot, 0) == 0)
			return false; // under the live container — ours, keep it
		return path.find(kContainer) != std::string::npos;
	}

	/// Drops entries naming a container that no longer exists, and duplicates. Returns true when
	/// the list changed.
	///
	/// Deduplication is not decorative: the write-back uses SetStringList, which deletes and
	/// re-appends with no dedup of its own (INISettingsInterface.cpp), so a list that already
	/// contained the same directory twice would make the scanner walk that tree twice.
	inline bool PruneKey(INISettingsInterface* si, const std::string& dataRoot, const char* key)
	{
		const std::vector<std::string> paths(si->GetStringList(kSection, key));
		std::vector<std::string> kept;
		kept.reserve(paths.size());
		bool changed = false;

		for (const std::string& path : paths)
		{
			if (IsDeadContainerPath(path, dataRoot))
			{
				Console.WriteLn("[GameList] Dropping path from a container that no longer exists:\n  %s",
					path.c_str());
				changed = true;
				continue;
			}
			if (std::find(kept.begin(), kept.end(), path) != kept.end())
			{
				changed = true; // already listed
				continue;
			}
			kept.push_back(path);
		}

		if (changed)
			si->SetStringList(kSection, key, kept);
		return changed;
	}

	/// The two locations the app owns and always scans. They are re-derived from the live
	/// dataRoot on every launch rather than trusted from the INI, which is what makes them
	/// immune to a container change (the stale copies are pruned above). The folder-management UI presents them as built-in rows
	/// that cannot be removed, because removing one here would only last until the next launch.
	inline std::string DefaultFlatDirectory(const std::string& dataRoot) { return dataRoot; }
	inline std::string DefaultRecursiveDirectory(const std::string& dataRoot) { return dataRoot + "/iso"; }

	/// Drops dead entries and guarantees the two defaults are present. Idempotent:
	/// AddToStringList returns false without writing when the value is already in the list.
	/// The caller is responsible for Save().
	inline void Seed(INISettingsInterface* si, const std::string& dataRoot)
	{
		PruneKey(si, dataRoot, kRecursiveKey);
		PruneKey(si, dataRoot, kFlatKey);

		// Documents/iso is scanned recursively so a user can organise games into subfolders.
		si->AddToStringList(kSection, kRecursiveKey, DefaultRecursiveDirectory(dataRoot).c_str());

		// Documents itself is scanned NON-recursively, and that is the only correct choice:
		// every one of the app's own folders (bios, covers, textures, cache, savestates, …)
		// lives directly under Documents, and ".bin" is a scannable disc extension — so a
		// recursive scan here would try to open each BIOS image as a disc on every rescan.
		// One level deep cannot descend into them (FileSystem's find drops directory entries
		// unless FILESYSTEM_FIND_FOLDERS is set), which also means it cannot double-count the
		// games under Documents/iso.
		si->AddToStringList(kSection, kFlatKey, DefaultFlatDirectory(dataRoot).c_str());
	}
} // namespace iPSX2::GameListPaths
