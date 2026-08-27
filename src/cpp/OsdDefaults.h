// OsdDefaults.h — what the performance overlay does before anyone configures it
// SPDX-License-Identifier: GPL-3.0+
//
// The core's own defaults turn the overlay ON: OsdPerformancePos defaults to TopRight and
// OsdShowSpeed/FPS/VPS/Resolution/CPU/GPU/FrameTimes/HardwareInfo/Version/Indicators all default
// to true (Pcsx2Config.cpp). A fresh install writes none of those keys, so the overlay appeared
// over the game while the settings screen — which reads the same keys with its own "off"
// defaults — said OFF. Two different answers to the same question.
//
// This app's answer is that an overlay nobody asked for should not be there. Seeding the
// position to None on a config that does not mention it makes the INI say so out loud, which is
// what the rest of the settings system expects (the INI is the single source of truth).

#pragma once

#include "pcsx2/INISettingsInterface.h"

namespace iPSX2::OsdDefaults
{
	inline constexpr const char* kSection = "EmuCore/GS";
	inline constexpr const char* kPositionKey = "OsdPerformancePos";

	/// Hides the overlay unless the config already has an opinion. Written only when the key is
	/// absent, so a user who positioned or enabled the overlay is never overridden — and once
	/// written, this never fires again. The caller is responsible for Save().
	inline void Seed(INISettingsInterface* si)
	{
		if (si->ContainsValue(kSection, kPositionKey))
			return;

		// 0 == OsdOverlayPos::None. Spelled as a literal to keep this header free of the GS
		// config headers; the enum's None is first and the value is part of the INI format.
		si->SetIntValue(kSection, kPositionKey, 0);
	}
} // namespace iPSX2::OsdDefaults
