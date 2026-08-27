// HostSaveState.mm — save state notifications for both Apple hosts
// SPDX-License-Identifier: GPL-3.0+
//
// [S2] Saving and loading run on the emulation thread, so the UI that triggered them has no
// synchronous result to react to. Both Apple hosts left these callbacks empty, which meant a
// save/load UI could only guess when to refresh. They now post on the main queue, matching
// the iPSX2VMDidShutdown pattern the Swift layer already uses.

#include "pcsx2/Host.h"
#include "pcsx2/VMManager.h"

#include "common/Console.h"

#import <Foundation/Foundation.h>

#include <string>

NSString* const iPSX2SaveStateSavedNotification  = @"iPSX2SaveStateSaved";
NSString* const iPSX2SaveStateLoadedNotification = @"iPSX2SaveStateLoaded";

namespace
{
	void PostOnMain(NSString* name, const std::string& path, bool success)
	{
		NSString* p = [NSString stringWithUTF8String:path.c_str()] ?: @"";
		dispatch_async(dispatch_get_main_queue(), ^{
			[[NSNotificationCenter defaultCenter] postNotificationName:name
															   object:nil
															 userInfo:@{@"path" : p, @"success" : @(success)}];
		});
	}
} // namespace

void Host::OnSaveStateLoading(std::string_view filename)
{
	// Nothing to publish yet — the outcome is what the UI needs, and that is OnSaveStateLoaded.
	(void)filename;
}

void Host::OnSaveStateLoaded(std::string_view filename, bool was_successful)
{
	if (!was_successful)
		Console.Error("Failed to load save state '%.*s'", static_cast<int>(filename.size()), filename.data());

	PostOnMain(iPSX2SaveStateLoadedNotification, std::string(filename), was_successful);
}

void Host::OnSaveStateSaved(std::string_view filename)
{
	PostOnMain(iPSX2SaveStateSavedNotification, std::string(filename), true);
}
