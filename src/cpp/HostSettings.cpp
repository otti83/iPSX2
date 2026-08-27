// HostSettings.cpp — settings persistence for both Apple hosts
// SPDX-License-Identifier: GPL-3.0+
//
// [F2] Core code changes a setting by writing it into the base layer and then calling
// Host::CommitBaseSettingChanges to persist it. Both Apple hosts left that function empty
// (ios_main.mm and MacOSStubs.cpp), so every such write was kept in memory and thrown away
// at exit — silently, since the write itself succeeded. Achievements, SDLInputSource and
// FullscreenUI all persist through this path.
//
// The implementation is platform-independent: SettingsInterface::Save is pure virtual and
// Host::Internal::GetBaseSettingsLayer already returns whichever interface the host installed
// (s_macos_settings on macOS, s_settings_interface on iOS). One definition for both targets,
// which also keeps it from drifting the way the duplicated host globals in this frontend did.

#include "pcsx2/Host.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/SettingsInterface.h"

#include <mutex>

void Host::CommitBaseSettingChanges()
{
	auto lock = Host::GetSettingsLock();

	SettingsInterface* si = Host::Internal::GetBaseSettingsLayer();
	if (!si)
	{
		Console.Warning("[F2] CommitBaseSettingChanges with no base settings layer; change dropped.");
		return;
	}

	Error error;
	if (!si->Save(&error))
		Console.Error("[F2] Failed to save settings: %s", error.GetDescription().c_str());
}
