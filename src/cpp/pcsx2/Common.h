// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "arm64/VixlHelpers.h"
#include "common/Pcsx2Defs.h"

// [iPSX2] P0 Cleanup: Disable behavior-changing patches by default.
// This gates BiosTools (CopyBIOS patches) and Memory (memReset ROM patches).
#ifndef iPSX2_ENABLE_P0_BEHAVIOR_PATCHES
#define iPSX2_ENABLE_P0_BEHAVIOR_PATCHES 0
#endif

static const u32 BIAS = 2;				// Bus is half of the actual ps2 speed
static const u32 PS2CLK = 294912000;	//hz	/* 294.912 mhz */
extern s64 PSXCLK;	/* 36.864 Mhz */


#include "Memory.h"
#include "R5900.h"
#include "Hw.h"
#include "Dmac.h"

#include "SaveState.h"
#include "DebugTools/Debug.h"

#include <cstdlib>
#include <cstring>
#include <string>

// iOS Simulator often injects process env via SIMCTL_CHILD_ prefix.
// Use this for iPSX2_* runtime flags so launch-time flags are reflected reliably.
inline const char* iPSX2_GetRuntimeEnv(const char* name)
{
	if (!name || !name[0])
		return nullptr;

	const char* value = std::getenv(name);
	if (value && value[0])
		return value;

	if (std::strncmp(name, "iPSX2_", 7) != 0)
		return nullptr;

	std::string child_name = "SIMCTL_CHILD_";
	child_name += name;
	value = std::getenv(child_name.c_str());
	return (value && value[0]) ? value : nullptr;
}

inline bool iPSX2_GetRuntimeEnvBool(const char* name, bool default_value = false)
{
	const char* value = iPSX2_GetRuntimeEnv(name);
	if (!value)
		return default_value;
	return (value[0] == '1' && value[1] == '\0');
}

// SAFE_ONLY=1:
// - disables semantics-changing diagnostics/patches
// - disables hot-path callout-heavy probes
// - keeps observation-only probes (store-only + one-shot dump)
inline bool iPSX2_IsSafeOnlyEnabled()
{
	static int s_cached = -1;
	if (s_cached < 0)
		s_cached = iPSX2_GetRuntimeEnvBool("iPSX2_SAFE_ONLY", true) ? 1 : 0;
	return (s_cached == 1);
}

// DEBUG_VERBOSE=1 で legacy "DEBUG: ..." prefix log + R103 block dump 等の hot-path
// diagnostic を有効化。 release ship default は false (= 全 suppress)。
// 用途: counter rcnt* trace、 JIT block compile trace、 icache flush trace 等を
// 必要時 (= 退行調査) に env で復活可能にする。
inline bool iPSX2_IsDebugVerbose()
{
	static int s_cached = -1;
	if (s_cached < 0)
		s_cached = iPSX2_GetRuntimeEnvBool("iPSX2_DEBUG_VERBOSE", false) ? 1 : 0;
	return (s_cached == 1);
}

extern std::string ShiftJIS_ConvertString( const char* src );
extern std::string ShiftJIS_ConvertString( const char* src, int maxlen );
