// BiosIntake.h — adopting a BIOS image dropped into Documents/
// SPDX-License-Identifier: GPL-3.0+
//
// The BIOS has to live in Documents/bios/ for the core to resolve it: the config stores only the
// filename and it is looked up under EmuFolders::Bios. But Documents/ is the folder the Files app
// opens, so that is where people actually drop things. Anything BIOS-shaped that lands there is
// moved into bios/ rather than being ignored.
//
// This used to be inline in the startup path only, which meant a file dropped in while the app
// was already running was invisible until a relaunch — the BIOS list's refresh button reads
// bios/ and nothing else. It is shared now so both the startup check and that refresh adopt it.

#pragma once

#include <algorithm>
#include <string>

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Pcsx2Types.h"

namespace iPSX2::BiosIntake
{
	// A PS2 BIOS image is around 4 MB. The bounds keep this away from the two things that share
	// the extension in this app: a tiny stray .bin and a disc image (which is hundreds of MB and
	// belongs to the game scanner instead).
	inline constexpr u64 kMinSize = 1024 * 1024;
	inline constexpr u64 kMaxSize = 50 * 1024 * 1024;

	inline bool LooksLikeBios(const FILESYSTEM_FIND_DATA& fd)
	{
		if (fd.Size < kMinSize || fd.Size > kMaxSize)
			return false;

		std::string name(Path::GetFileName(fd.FileName));
		if (name.size() < 4)
			return false;
		std::string ext = name.substr(name.size() - 4);
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		return ext == ".bin" || ext == ".rom";
	}

	/// Moves BIOS-shaped files out of the data root and into bios/. Returns how many moved.
	/// A name already present in bios/ is left where it is rather than overwritten — the copy
	/// already adopted is the one the config may already name.
	inline int AdoptFromDataRoot(const std::string& dataRoot)
	{
		const std::string biosDir = Path::Combine(dataRoot, "bios");

		FileSystem::FindResultsArray results;
		if (!FileSystem::FindFiles(dataRoot.c_str(), "*", FILESYSTEM_FIND_FILES, &results))
			return 0;

		int moved = 0;
		for (const FILESYSTEM_FIND_DATA& fd : results)
		{
			if (!LooksLikeBios(fd))
				continue;

			const std::string name(Path::GetFileName(fd.FileName));
			const std::string src = Path::Combine(dataRoot, name);
			const std::string dst = Path::Combine(biosDir, name);
			if (FileSystem::FileExists(dst.c_str()))
				continue;

			FileSystem::CreateDirectoryPath(biosDir.c_str(), false);

			Error error;
			if (FileSystem::RenamePath(src.c_str(), dst.c_str(), &error))
			{
				Console.WriteLn("[Files] Moved BIOS into bios/: %s", name.c_str());
				moved++;
			}
			else
			{
				Console.Error("[Files] Could not move BIOS '%s' into bios/: %s",
					name.c_str(), error.GetDescription().c_str());
			}
		}
		return moved;
	}
} // namespace iPSX2::BiosIntake
