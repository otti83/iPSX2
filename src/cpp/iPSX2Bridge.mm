// iPSX2Bridge.mm — ObjC bridge implementation
// SPDX-License-Identifier: GPL-3.0+

#import "iPSX2Bridge.h"
#include <SDL3/SDL.h>

extern "C" void iPSX2_SetSDLFullscreen(bool enabled);
#include "Common.h"
#include "CDVD/CDVD.h"
#include "VMManager.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "Counters.h"
#include "GS/GSState.h"
#include "pcsx2/Host.h"
#include "pcsx2/GameList.h"
#include "pcsx2/SaveState.h"
#include "pcsx2/SPU2/spu2.h"
#include <thread>
#include <atomic>
#include "common/ProgressCallback.h"
#include "common/Image.h"
#include "HostGameList.h"
#include "GameListPaths.h"
#include "BiosIntake.h"
#include "pcsx2/Patch.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/FileSystem.h"
#include "common/Error.h"
#include "common/Path.h"
#include <cmath>
#include <algorithm>

static NSDate* s_lastNVMSaveDate = nil;

@implementation iPSX2Bridge

#if TARGET_OS_IPHONE
+ (UIView *)gameRenderView {
    extern UIView* g_gameRenderView;
    return g_gameRenderView;
}
#else
+ (NSView *)gameRenderView {
    return nil; // [P63] macOS: SDL window handles rendering
}
#endif

+ (void)saveNVRAM {
    cdvdSaveNVRAM();
    s_lastNVMSaveDate = [NSDate date];
    NSLog(@"[iPSX2Bridge] NVM saved at %@", s_lastNVMSaveDate);
}

+ (void)saveMemoryCards {
    // FileMcd_EmuClose triggers save on all open memory cards
    // For now, MC saves happen automatically via the existing PCSX2 MC system
    NSLog(@"[iPSX2Bridge] Memory card save requested");
}

+ (void)saveAllState {
    [self saveNVRAM];
    [self saveMemoryCards];
}

+ (BOOL)isRunning {
    return VMManager::GetState() == VMState::Running;
}

+ (nullable NSDate *)lastNVMSaveDate {
    return s_lastNVMSaveDate;
}

+ (nullable NSString *)nvmFilePath {
    // NVM path is BIOS path with .nvm extension
    // We can't easily access BiosPath from here, so return nil for now
    return nil;
}

+ (BOOL)nvmFileExists {
    NSString* path = [self nvmFilePath];
    if (!path) return NO;
    return [[NSFileManager defaultManager] fileExistsAtPath:path];
}

+ (void)setPadButton:(iPSX2PadButton)button pressed:(BOOL)pressed {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;

    static const u32 buttonMap[] = {
        PadDualshock2::Inputs::PAD_UP,       // Up
        PadDualshock2::Inputs::PAD_DOWN,     // Down
        PadDualshock2::Inputs::PAD_LEFT,     // Left
        PadDualshock2::Inputs::PAD_RIGHT,    // Right
        PadDualshock2::Inputs::PAD_CROSS,    // Cross
        PadDualshock2::Inputs::PAD_CIRCLE,   // Circle
        PadDualshock2::Inputs::PAD_SQUARE,   // Square
        PadDualshock2::Inputs::PAD_TRIANGLE, // Triangle
        PadDualshock2::Inputs::PAD_L1,       // L1
        PadDualshock2::Inputs::PAD_R1,       // R1
        PadDualshock2::Inputs::PAD_L2,       // L2
        PadDualshock2::Inputs::PAD_R2,       // R2
        PadDualshock2::Inputs::PAD_START,    // Start
        PadDualshock2::Inputs::PAD_SELECT,   // Select
        PadDualshock2::Inputs::PAD_L3,       // L3
        PadDualshock2::Inputs::PAD_R3,       // R3
    };

    if ((int)button < (int)(sizeof(buttonMap)/sizeof(buttonMap[0]))) {
        u32 idx = buttonMap[(int)button];
        pad->Set(idx, pressed ? 1.0f : 0.0f);
        // Update touch state so PumpMessagesOnCPUThread doesn't override
        extern bool g_touchPadState[64];
        if (idx < 64) g_touchPadState[idx] = pressed;
    }
}

+ (void)setLeftStickX:(float)x Y:(float)y {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;
    // Convert axis (-1..+1) to individual direction values (0..1)
    pad->Set(PadDualshock2::Inputs::PAD_L_RIGHT, x > 0 ? x : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_L_LEFT, x < 0 ? -x : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_L_DOWN, y > 0 ? y : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_L_UP, y < 0 ? -y : 0.0f);
}

+ (void)setRightStickX:(float)x Y:(float)y {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;
    pad->Set(PadDualshock2::Inputs::PAD_R_RIGHT, x > 0 ? x : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_R_LEFT, x < 0 ? -x : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_R_DOWN, y > 0 ? y : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_R_UP, y < 0 ? -y : 0.0f);
}

+ (nonnull NSString *)biosName {
    return @"PS2";
}

+ (void)requestVMStop {
    extern std::atomic<bool> s_requestVMStop;
    s_requestVMStop.store(true);
    [[NSNotificationCenter defaultCenter] postNotificationName:@"iPSX2VMDidShutdown" object:nil];
}

+ (void)setFullScreen:(BOOL)enabled {
    iPSX2_SetSDLFullscreen(enabled ? true : false);
}

+ (nonnull NSString *)buildVersion {
    NSString *ver = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    return [NSString stringWithFormat:@"iPSX2 v%@", ver];
}

+ (nullable NSString *)currentISOPath {
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString *iniPath = [docsPath stringByAppendingPathComponent:@"PCSX2-iOS.ini"];
    // Read BootISO from INI
    FILE *f = fopen(iniPath.UTF8String, "r");
    if (!f) return nil;
    char line[512];
    bool inSection = false;
    NSString *result = nil;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "[GameISO]")) { inSection = true; continue; }
        if (line[0] == '[') { inSection = false; continue; }
        if (inSection && strstr(line, "BootISO")) {
            char *eq = strchr(line, '=');
            if (eq) {
                eq++;
                while (*eq == ' ') eq++;
                // Remove trailing newline
                char *nl = strchr(eq, '\n'); if (nl) *nl = 0;
                char *cr = strchr(eq, '\r'); if (cr) *cr = 0;
                if (strlen(eq) > 0) result = [NSString stringWithUTF8String:eq];
            }
        }
    }
    fclose(f);
    return result;
}

+ (nonnull NSString *)isoDirectory {
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString *isoDir = [docsPath stringByAppendingPathComponent:@"iso"];
    [[NSFileManager defaultManager] createDirectoryAtPath:isoDir withIntermediateDirectories:YES attributes:nil error:nil];
    return isoDir;
}

+ (nonnull NSString *)documentsDirectory {
    return [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
}

+ (nonnull NSArray<NSString *> *)availableISOs {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableSet *seen = [NSMutableSet set];
    NSMutableArray *isos = [NSMutableArray array];

    // Helper block: scan a directory for ISO files
    void (^scanDir)(NSString *) = ^(NSString *dir) {
        NSArray *files = [fm contentsOfDirectoryAtPath:dir error:nil];
        for (NSString *file in files) {
            if ([seen containsObject:file]) continue;
            NSString *ext = file.pathExtension.lowercaseString;
            if ([ext isEqualToString:@"iso"] || [ext isEqualToString:@"img"] || [ext isEqualToString:@"chd"] || [ext isEqualToString:@"elf"]) {
                [isos addObject:file];
                [seen addObject:file];
            } else if ([ext isEqualToString:@"bin"]) {
// .bin > 50MB treated as game image
                NSString *fullPath = [dir stringByAppendingPathComponent:file];
                NSDictionary *attrs = [fm attributesOfItemAtPath:fullPath error:nil];
                if ([attrs fileSize] > 50 * 1024 * 1024) {
                    [isos addObject:file];
                    [seen addObject:file];
                }
            }
        }
    };

    // Scan Documents/iso/ first, then Documents/ root
    scanDir([self isoDirectory]);
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    scanDir(docsPath);

    return isos;
}

// [OSD] The three methods that used to live here — setPerformanceOverlayVisible:,
// isPerformanceOverlayVisible and applyOsdPreset: — wrote straight into the live GS config and
// never touched the INI. That made the overlay two different kinds of wrong at once: the state
// did not survive a restart, and anything that reloaded settings (which is most of the settings
// screen) silently put it back. Worse, hiding it parked the position at None and no preset ever
// wrote the position again, so once hidden in-game it could not be brought back from Settings.
// The overlay is now driven entirely from the INI plus +applySettingsNow, like every other
// setting; see SettingsStore's OSD block.

// ============================================================
// ISO / BIOS / Settings management
// ============================================================

#pragma mark - ISO boot

+ (void)bootISO:(nonnull NSString *)isoName {
    Host::SetBaseStringSettingValue("GameISO", "BootISO", isoName.UTF8String);
    Host::CommitBaseSettingChanges();
    NSLog(@"bootISO: set BootISO=%@", isoName);
}

#pragma mark - BIOS management

+ (nonnull NSString *)biosDirectory {
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString *biosDir = [docsPath stringByAppendingPathComponent:@"bios"];
    [[NSFileManager defaultManager] createDirectoryAtPath:biosDir withIntermediateDirectories:YES attributes:nil error:nil];
    return biosDir;
}

+ (nonnull NSArray<NSString *> *)availableBIOSes {
    // Documents/ is where the Files app drops things, but the core resolves the BIOS by
    // filename under EmuFolders::Bios — so a file left in the root is unusable where it sits.
    // Adopt it first, then list. Without this the refresh button read bios/ and nothing else,
    // and a BIOS dropped into Documents/ stayed invisible until the app was relaunched.
    [self biosDirectory];   // creates bios/ if this is a fresh install
    iPSX2::BiosIntake::AdoptFromDataRoot(EmuFolders::DataRoot);

    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableSet *seen = [NSMutableSet set];
    NSMutableArray *bioses = [NSMutableArray array];

    // Helper block: scan directory for BIOS files (>= 1MB .bin/.rom)
    void (^scanDir)(NSString *) = ^(NSString *dir) {
        NSArray *files = [fm contentsOfDirectoryAtPath:dir error:nil];
        for (NSString *file in files) {
            if ([seen containsObject:file]) continue;
            NSString *ext = file.pathExtension.lowercaseString;
            if ([ext isEqualToString:@"bin"] || [ext isEqualToString:@"rom"]) {
                NSString *fullPath = [dir stringByAppendingPathComponent:file];
                NSDictionary *attrs = [fm attributesOfItemAtPath:fullPath error:nil];
// BIOS files are >= 1MB and <= 50MB
                unsigned long long sz = [attrs fileSize];
                if (sz >= 1024 * 1024 && sz <= 50 * 1024 * 1024) {
                    [bioses addObject:file];
                    [seen addObject:file];
                }
            }
        }
    };

    scanDir([self biosDirectory]);
    return bioses;
}

+ (nonnull NSString *)defaultBIOSName {
    const std::string val = Host::GetBaseStringSettingValue("Filenames", "BIOS", "");
    return [NSString stringWithUTF8String:val.c_str()] ?: @"";
}

+ (void)setDefaultBIOS:(nonnull NSString *)biosName {
    Host::SetBaseStringSettingValue("Filenames", "BIOS", biosName.UTF8String);
    Host::CommitBaseSettingChanges();
    EmuConfig.BaseFilenames.Bios = biosName.UTF8String;
    NSLog(@"setDefaultBIOS: %@", biosName);
}

#pragma mark - Favorites

+ (BOOL)isFavorite:(nonnull NSString *)isoName {
    return Host::GetBaseBoolSettingValue("Favorites", isoName.UTF8String, false) ? YES : NO;
}

+ (void)setFavorite:(nonnull NSString *)isoName favorite:(BOOL)favorite {
    Host::SetBaseBoolSettingValue("Favorites", isoName.UTF8String, favorite ? true : false);
    Host::CommitBaseSettingChanges();
}

#pragma mark - INI generic getter/setter

// [F2] These go through the Host settings API rather than poking an INISettingsInterface
// pointer directly. The pointer these used to use (g_p44_settings_interface) was assigned only
// on iOS — MacOSStubs.cpp defined it as nullptr and nothing ever set it — so on the macOS build
// this project develops and measures against, every accessor below silently did nothing. It is
// now gone entirely. Writing through Host:: also takes the settings lock and persists via
// Host::CommitBaseSettingChanges, which is the same path core code uses.
//
// These are deliberately the *base* (global) layer. Per-game overrides live in a layer above
// it; a reader for the effective value belongs with the per-game settings work, not here.

+ (int)getINIInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def {
    return Host::GetBaseIntSettingValue(section.UTF8String, key.UTF8String, def);
}

+ (BOOL)getINIBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def {
    return Host::GetBaseBoolSettingValue(section.UTF8String, key.UTF8String, def) ? YES : NO;
}

+ (float)getINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def {
    return Host::GetBaseFloatSettingValue(section.UTF8String, key.UTF8String, def);
}

+ (nonnull NSString *)getINIString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def {
    const std::string val = Host::GetBaseStringSettingValue(section.UTF8String, key.UTF8String, def.UTF8String);
    NSString* out = [NSString stringWithUTF8String:val.c_str()];
    return out ?: def;
}

+ (void)setINIInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value {
    Host::SetBaseIntSettingValue(section.UTF8String, key.UTF8String, value);
    Host::CommitBaseSettingChanges();
}

+ (void)setINIBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value {
    Host::SetBaseBoolSettingValue(section.UTF8String, key.UTF8String, value ? true : false);
    Host::CommitBaseSettingChanges();
}

+ (void)setINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value {
    Host::SetBaseFloatSettingValue(section.UTF8String, key.UTF8String, value);
    Host::CommitBaseSettingChanges();
}

+ (void)setINIString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value {
    Host::SetBaseStringSettingValue(section.UTF8String, key.UTF8String, value.UTF8String);
    Host::CommitBaseSettingChanges();
}

// The setters above only persist. Nothing re-reads the INI on its own, so a control that wrote
// a value and stopped there did not take effect until the next boot — and, worse, a control
// that instead poked the live config directly was silently reverted the next time anything
// called ApplySettings. Writing the INI and then calling this is the one path that is both
// persistent and immediate. Same shape as the speed setters above.
+ (void)applySettingsNow {
    Host::RunOnCPUThread([]() { VMManager::ApplySettings(); });
}

#pragma mark - Cover art (COVER)

NSString * const iPSX2CoverDownloadDidFinishNotification = @"iPSX2CoverDownloadDidFinish";

namespace
{
	std::atomic<bool> s_cover_running{false};
	std::atomic<bool> s_cover_cancel{false};
	std::atomic<int> s_cover_percent{0};

	// GameList::DownloadCovers wants a ProgressCallback; this one only carries what a progress
	// bar and a cancel button need.
	class CoverProgress final : public ProgressCallback
	{
	public:
		void PushState() override {}
		void PopState() override {}
		bool IsCancelled() const override { return s_cover_cancel.load(std::memory_order_relaxed); }
		bool IsCancellable() const override { return true; }
		void SetCancellable(bool) override {}
		void SetTitle(const char*) override {}
		void SetStatusText(const char*) override {}
		void SetProgressRange(u32 range) override { m_range = range ? range : 1; Publish(); }
		void SetProgressValue(u32 value) override { m_value = value; Publish(); }
		void IncrementProgressValue() override { m_value++; Publish(); }
		void SetProgressState(ProgressState) override {}
		void DisplayError(const char* m) override { Console.Error("[Covers] %s", m); }
		void DisplayWarning(const char* m) override { Console.Warning("[Covers] %s", m); }
		void DisplayInformation(const char* m) override { Console.WriteLn("[Covers] %s", m); }
		void DisplayDebugMessage(const char* m) override { DevCon.WriteLn("[Covers] %s", m); }
		void ModalError(const char* m) override { Console.Error("[Covers] %s", m); }
		bool ModalConfirmation(const char*) override { return false; }
		void ModalInformation(const char* m) override { Console.WriteLn("[Covers] %s", m); }

	private:
		void Publish() { s_cover_percent.store(static_cast<int>((m_value * 100) / m_range)); }
		u32 m_range = 1;
		u32 m_value = 0;
	};
} // namespace

+ (nonnull NSString *)coversDirectory {
    return [NSString stringWithUTF8String:EmuFolders::Covers.c_str()] ?: @"";
}

+ (BOOL)isDownloadingCovers { return s_cover_running.load() ? YES : NO; }
+ (int)coverDownloadProgress { return s_cover_percent.load(); }
+ (void)cancelCoverDownload { s_cover_cancel.store(true); }

+ (void)downloadCoversWithTemplate:(nonnull NSString *)urlTemplate useSerial:(BOOL)useSerial {
    bool expected = false;
    if (!s_cover_running.compare_exchange_strong(expected, true))
        return;

    s_cover_cancel.store(false);
    s_cover_percent.store(0);
    const std::string tmpl = urlTemplate.UTF8String;
    const bool serial = useSerial ? true : false;

    std::thread([tmpl, serial]() {
        // Count what could possibly gain a cover before starting. DownloadCovers works this out
        // for itself and then reports "no URLs to download enumerated" as a failure when the
        // answer is zero — but that is the ordinary case of a library that is already covered,
        // not an error, and it is why a second run used to end in "Finished with errors". The
        // count is also the denominator: without it a run where every request 404s and a run
        // where everything succeeded both end in a bare green "Finished".
        int missing = 0;
        {
            auto lock = GameList::GetLock();
            const u32 count = GameList::GetEntryCount();
            for (u32 i = 0; i < count; i++) {
                const GameList::Entry* e = GameList::GetEntryByIndex(i);
                if (e && GameList::GetCoverImagePathForEntry(e).empty())
                    missing++;
            }
        }

        // The save callback runs inside HTTPDownloader::LockedPollRequests on this thread, but
        // an atomic costs nothing and removes the need to rely on that.
        std::atomic<int> saved{0};
        bool ok = true;
        if (missing > 0) {
            CoverProgress progress;
            // Network + disk work; nothing here touches VM state, so it stays off the CPU thread.
            ok = GameList::DownloadCovers({tmpl}, serial, &progress,
                [&saved](const GameList::Entry*, std::string) { saved.fetch_add(1); });
        }

        const bool cancelled = s_cover_cancel.load();
        Console.WriteLn("[Covers] download finished: %s (%d of %d saved%s)", ok ? "ok" : "failed",
            saved.load(), missing, cancelled ? ", cancelled" : "");
        s_cover_running.store(false);
        NSDictionary* info = @{
            @"success" : @(ok),
            @"saved" : @(saved.load()),
            @"missing" : @(missing),
            @"cancelled" : @(cancelled),
        };
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSNotificationCenter defaultCenter] postNotificationName:iPSX2CoverDownloadDidFinishNotification
                                                               object:nil
                                                             userInfo:info];
        });
    }).detach();
}

#pragma mark - Audio (AUDIO)

+ (int)audioVolume {
    return static_cast<int>(EmuConfig.SPU2.OutputVolume);
}

+ (void)setAudioVolume:(int)volume {
    const u32 v = static_cast<u32>(std::clamp(volume, 0, 100));
    EmuConfig.SPU2.OutputVolume = v;
    Host::SetBaseIntSettingValue("SPU2/Output", "OutputVolume", static_cast<int>(v));
    Host::CommitBaseSettingChanges();
    // Applied immediately rather than on next boot: a volume slider that does nothing until
    // you restart is worse than no slider. SPU2::SetOutputVolume is a no-op when no stream.
    Host::RunOnCPUThread([v]() { SPU2::SetOutputVolume(v); });
}

+ (BOOL)isAudioMuted {
    return EmuConfig.SPU2.OutputMuted ? YES : NO;
}

+ (void)setAudioMuted:(BOOL)muted {
    const bool m = muted ? true : false;
    EmuConfig.SPU2.OutputMuted = m;
    Host::SetBaseBoolSettingValue("SPU2/Output", "OutputMuted", m);
    Host::CommitBaseSettingChanges();
    const u32 v = m ? 0u : EmuConfig.SPU2.OutputVolume;
    Host::RunOnCPUThread([v]() { SPU2::SetOutputVolume(v); });
}

#pragma mark - Emulation speed (SPEED)

// PCSX2 stores the multipliers as floats; the UI works in whole percent because that is how
// PCSX2 presents them and it avoids float rounding wobble in a stepper. Sanity-checked to the
// same 5%..1000% band Pcsx2Config::SanityCheck enforces (Pcsx2Config.cpp:1744).
static float PercentToScalar(int percent) {
    return std::clamp(percent, 5, 1000) / 100.0f;
}
static int ScalarToPercent(float scalar) {
    return static_cast<int>(std::lround(scalar * 100.0f));
}

+ (iPSX2LimiterMode)limiterMode {
    switch (VMManager::GetLimiterMode()) {
        case LimiterModeType::Turbo:     return iPSX2LimiterModeTurbo;
        case LimiterModeType::Slomo:     return iPSX2LimiterModeSlowMotion;
        case LimiterModeType::Unlimited: return iPSX2LimiterModeUnlimited;
        default:                         return iPSX2LimiterModeNormal;
    }
}

+ (void)setLimiterMode:(iPSX2LimiterMode)mode {
    LimiterModeType t = LimiterModeType::Nominal;
    switch (mode) {
        case iPSX2LimiterModeTurbo:      t = LimiterModeType::Turbo; break;
        case iPSX2LimiterModeSlowMotion: t = LimiterModeType::Slomo; break;
        case iPSX2LimiterModeUnlimited:  t = LimiterModeType::Unlimited; break;
        default: break;
    }
    // Touches the throttle state the CPU thread reads every vsync.
    Host::RunOnCPUThread([t]() { VMManager::SetLimiterMode(t); });
}

+ (BOOL)isPaused {
    return VMManager::GetState() == VMState::Paused ? YES : NO;
}

+ (void)setPaused:(BOOL)paused {
    const bool p = paused ? true : false;
    Host::RunOnCPUThread([p]() { VMManager::SetPaused(p); });
}

+ (void)frameAdvance {
    Host::RunOnCPUThread([]() { VMManager::FrameAdvance(1); });
}

+ (int)normalSpeedPercent { return ScalarToPercent(EmuConfig.EmulationSpeed.NominalScalar); }
+ (int)turboSpeedPercent  { return ScalarToPercent(EmuConfig.EmulationSpeed.TurboScalar); }
+ (int)slowMotionSpeedPercent { return ScalarToPercent(EmuConfig.EmulationSpeed.SlomoScalar); }

+ (void)setNormalSpeedPercent:(int)percent {
    Host::SetBaseFloatSettingValue("Framerate", "NominalScalar", PercentToScalar(percent));
    Host::CommitBaseSettingChanges();
    Host::RunOnCPUThread([]() { VMManager::ApplySettings(); });
}

+ (void)setTurboSpeedPercent:(int)percent {
    Host::SetBaseFloatSettingValue("Framerate", "TurboScalar", PercentToScalar(percent));
    Host::CommitBaseSettingChanges();
    Host::RunOnCPUThread([]() { VMManager::ApplySettings(); });
}

+ (void)setSlowMotionSpeedPercent:(int)percent {
    Host::SetBaseFloatSettingValue("Framerate", "SlomoScalar", PercentToScalar(percent));
    Host::CommitBaseSettingChanges();
    Host::RunOnCPUThread([]() { VMManager::ApplySettings(); });
}

#pragma mark - Save states (S2)

NSString * const iPSX2SlotKeyIndex      = @"index";
NSString * const iPSX2SlotKeyExists     = @"exists";
NSString * const iPSX2SlotKeyDate       = @"date";
NSString * const iPSX2SlotKeyScreenshot = @"screenshot";
NSString * const iPSX2SlotKeyHasBackup  = @"hasBackup";

// Slot filenames are keyed on the running disc's serial and CRC, so slots only exist while a
// game is running. Returns false when there is nothing to address.
static bool CurrentGameKey(std::string* serial, u32* crc)
{
    if (!VMManager::HasValidVM())
        return false;
    *serial = VMManager::GetDiscSerial();
    *crc = VMManager::GetDiscCRC();
    return !serial->empty();
}

+ (int)saveStateSlotCount {
    return VMManager::NUM_SAVE_STATE_SLOTS;
}

+ (BOOL)canUseSaveStates {
    std::string serial; u32 crc = 0;
    return CurrentGameKey(&serial, &crc) ? YES : NO;
}

+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)saveStateSlots {
    NSMutableArray* out = [NSMutableArray array];
    std::string serial; u32 crc = 0;
    if (!CurrentGameKey(&serial, &crc))
        return out;

    for (s32 slot = 1; slot <= VMManager::NUM_SAVE_STATE_SLOTS; slot++) {
        const std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot, false);
        const std::string backup = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot, true);

        NSMutableDictionary* d = [NSMutableDictionary dictionary];
        d[iPSX2SlotKeyIndex] = @(slot);
        d[iPSX2SlotKeyHasBackup] = @(!backup.empty() && FileSystem::FileExists(backup.c_str()));

        FILESYSTEM_STAT_DATA sd = {};
        const bool exists = !path.empty() && FileSystem::StatFile(path.c_str(), &sd);
        d[iPSX2SlotKeyExists] = @(exists);
        if (exists) {
            d[iPSX2SlotKeyDate] = [NSDate dateWithTimeIntervalSince1970:static_cast<double>(sd.ModificationTime)];

            // The thumbnail is stored as raw pixels; re-encode to PNG so Swift can take it as
            // NSData without the bridge knowing about UIImage or NSImage.
            u32 w = 0, h = 0;
            std::vector<u32> pixels;
            if (SaveState_ReadScreenshot(path, &w, &h, &pixels) && w > 0 && h > 0) {
                const RGBA8Image image(w, h, pixels.data());
                if (auto png = image.SaveToBuffer("screenshot.png"); png.has_value() && !png->empty())
                    d[iPSX2SlotKeyScreenshot] = [NSData dataWithBytes:png->data() length:png->size()];
            }
        }
        [out addObject:d];
    }
    return out;
}

+ (void)saveStateToSlot:(int)slot {
    // Must run on the emulation thread; F1 makes that real rather than a silent drop.
    Host::RunOnCPUThread([slot]() { VMManager::SaveStateToSlot(slot); });
}

+ (void)loadStateFromSlot:(int)slot backup:(BOOL)backup {
    const bool use_backup = backup ? true : false;
    Host::RunOnCPUThread([slot, use_backup]() { VMManager::LoadStateFromSlot(slot, use_backup); });
}

+ (BOOL)deleteStateInSlot:(int)slot {
    std::string serial; u32 crc = 0;
    if (!CurrentGameKey(&serial, &crc))
        return NO;

    bool removed = false;
    for (bool backup : {false, true}) {
        const std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot, backup);
        if (!path.empty() && FileSystem::FileExists(path.c_str()))
            removed |= FileSystem::DeleteFilePath(path.c_str());
    }
    return removed ? YES : NO;
}

#pragma mark - Game library (F8)

NSString * const iPSX2GameKeyPath          = @"path";
NSString * const iPSX2GameKeySerial        = @"serial";
NSString * const iPSX2GameKeyTitle         = @"title";
NSString * const iPSX2GameKeyTitleEn       = @"titleEn";
NSString * const iPSX2GameKeyRegion        = @"region";
NSString * const iPSX2GameKeyType          = @"type";
NSString * const iPSX2GameKeyCRC           = @"crc";
NSString * const iPSX2GameKeySize          = @"size";
NSString * const iPSX2GameKeyCompatibility = @"compatibility";
NSString * const iPSX2GameKeyLastPlayed    = @"lastPlayed";
NSString * const iPSX2GameKeyTotalPlayed   = @"totalPlayed";
NSString * const iPSX2GameKeyCoverPath     = @"coverPath";

static NSString* NSStr(const std::string& v) {
    NSString* s = [NSString stringWithUTF8String:v.c_str()];
    return s ?: @"";
}

+ (void)refreshGameList:(BOOL)invalidateCache {
    Host::RefreshGameListAsync(invalidateCache ? true : false);
}

+ (BOOL)isGameListRefreshing {
    return iPSX2::IsGameListRefreshing() ? YES : NO;
}

+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)gameListEntries {
    // GetLock is held for the whole snapshot: Refresh swaps s_entries wholesale, so reading
    // entry pointers without it can hand back memory a concurrent refresh has replaced.
    auto lock = GameList::GetLock();
    const u32 count = GameList::GetEntryCount();
    NSMutableArray* out = [NSMutableArray arrayWithCapacity:count];
    for (u32 i = 0; i < count; i++) {
        const GameList::Entry* e = GameList::GetEntryByIndex(i);
        if (!e) continue;
        const std::string cover = GameList::GetCoverImagePathForEntry(e);
        [out addObject:@{
            iPSX2GameKeyPath          : NSStr(e->path),
            iPSX2GameKeySerial        : NSStr(e->serial),
            iPSX2GameKeyTitle         : NSStr(e->GetTitle()),
            iPSX2GameKeyTitleEn       : NSStr(e->title_en),
            iPSX2GameKeyRegion        : @(GameList::RegionToString(e->region, false)),
            iPSX2GameKeyType          : @(GameList::EntryTypeToString(e->type, false)),
            iPSX2GameKeyCRC           : @(e->crc),
            iPSX2GameKeySize          : @(e->total_size),
            iPSX2GameKeyCompatibility : @(static_cast<int>(e->compatibility_rating)),
            iPSX2GameKeyLastPlayed    : @(static_cast<long long>(e->last_played_time)),
            iPSX2GameKeyTotalPlayed   : @(static_cast<long long>(e->total_played_time)),
            iPSX2GameKeyCoverPath     : NSStr(cover),
        }];
    }
    return out;
}

NSString * const iPSX2GameDirKeyPath      = @"path";
NSString * const iPSX2GameDirKeyRecursive = @"recursive";
NSString * const iPSX2GameDirKeyBuiltIn   = @"builtIn";
NSString * const iPSX2GameDirKeyExists    = @"exists";

+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)gameDirectoryEntries {
    namespace GLP = iPSX2::GameListPaths;
    const std::string flat_default = GLP::DefaultFlatDirectory(EmuFolders::DataRoot);
    const std::string recursive_default = GLP::DefaultRecursiveDirectory(EmuFolders::DataRoot);

    NSMutableArray* out = [NSMutableArray array];
    auto append = [&](const char* key, bool recursive, const std::string& built_in) {
        for (const std::string& d : Host::GetBaseStringListSetting(GLP::kSection, key)) {
            [out addObject:@{
                iPSX2GameDirKeyPath      : NSStr(d),
                iPSX2GameDirKeyRecursive : @(recursive),
                iPSX2GameDirKeyBuiltIn   : @(d == built_in),
                iPSX2GameDirKeyExists    : @(FileSystem::DirectoryExists(d.c_str())),
            }];
        }
    };
    append(GLP::kRecursiveKey, true, recursive_default);
    append(GLP::kFlatKey, false, flat_default);
    return out;
}

+ (void)addGameDirectory:(nonnull NSString *)path recursive:(BOOL)recursive {
    namespace GLP = iPSX2::GameListPaths;
    // A directory listed under both keys would be walked twice, so drop it from the other one
    // rather than letting the two entries fight.
    Host::RemoveBaseValueFromStringList(GLP::kSection, recursive ? GLP::kFlatKey : GLP::kRecursiveKey,
                                        path.UTF8String);
    Host::AddBaseValueToStringList(GLP::kSection, recursive ? GLP::kRecursiveKey : GLP::kFlatKey,
                                   path.UTF8String);
    Host::CommitBaseSettingChanges();
}

+ (void)removeGameDirectory:(nonnull NSString *)path {
    Host::RemoveBaseValueFromStringList("GameList", "RecursivePaths", path.UTF8String);
    Host::RemoveBaseValueFromStringList("GameList", "Paths", path.UTF8String);
    Host::CommitBaseSettingChanges();
}

+ (void)bootGameAtPath:(nonnull NSString *)path {
    // The boot resolver accepts an absolute path here; see ios_main.mm. Keeping the same INI
    // key means the existing boot flow needs no other change.
    Host::SetBaseStringSettingValue("GameISO", "BootISO", path.UTF8String);
    Host::CommitBaseSettingChanges();
}

+ (nullable NSString *)coverPathForEntryPath:(nonnull NSString *)path {
    auto lock = GameList::GetLock();
    const GameList::Entry* e = GameList::GetEntryForPath(path.UTF8String);
    if (!e) return nil;
    const std::string cover = GameList::GetCoverImagePathForEntry(e);
    return cover.empty() ? nil : NSStr(cover);
}

+ (BOOL)setCoverForEntryPath:(nonnull NSString *)path fromFile:(nonnull NSString *)sourceFile {
    std::string dest;
    {
        auto lock = GameList::GetLock();
        const GameList::Entry* e = GameList::GetEntryForPath(path.UTF8String);
        if (!e) return NO;
        dest = GameList::GetNewCoverImagePathForEntry(e, sourceFile.UTF8String, false);
    }
    if (dest.empty()) return NO;

    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* destStr = NSStr(dest);
    [fm removeItemAtPath:destStr error:nil];
    NSError* err = nil;
    if (![fm copyItemAtPath:sourceFile toPath:destStr error:&err]) {
        Console.Error("[GameList] cover copy failed: %s", [[err localizedDescription] UTF8String]);
        return NO;
    }
    return YES;
}

+ (void)setCustomTitle:(nullable NSString *)title forEntryPath:(nonnull NSString *)path {
    GameList::SaveCustomTitleForPath(path.UTF8String, title ? std::string(title.UTF8String) : std::string());
}

+ (void)clearPlayTimeForSerial:(nonnull NSString *)serial {
    GameList::ClearPlayedTimeForSerial(serial.UTF8String);
}

#pragma mark - VM lifecycle

+ (BOOL)isVMRunning {
    VMState st = VMManager::GetState();
    return st == VMState::Running || st == VMState::Paused;
}

+ (BOOL)hasBIOS {
    if (EmuConfig.BaseFilenames.Bios.empty()) return NO;
    std::string fullPath = Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios);
    return FileSystem::FileExists(fullPath.c_str());
}

+ (void)requestVMBoot {
    // Post notification that ios_main.mm will observe to start VM
    [[NSNotificationCenter defaultCenter] postNotificationName:@"iPSX2RequestVMBoot" object:nil];
}

+ (void)requestVMShutdown {
    [[NSNotificationCenter defaultCenter] postNotificationName:@"iPSX2RequestVMShutdown" object:nil];
}

// Gamepad button mapping
extern std::atomic<bool> s_captureMode;
extern std::atomic<int>  s_capturedButton;
extern int s_buttonMap[16];

+ (void)startButtonCapture {
    s_capturedButton.store(-1);
    s_captureMode.store(true);
}

+ (void)stopButtonCapture {
    s_captureMode.store(false);
}

// Poll SDL gamepad from main thread (for settings screen when VM is not running)
+ (void)pollGamepadForCapture {
    if (!s_captureMode.load()) return;
    SDL_UpdateGamepads();
    // Keep gamepad open across polls to avoid open/close overhead
    static SDL_Gamepad* s_settingsGP = nullptr;
    if (!s_settingsGP) {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids && count > 0) s_settingsGP = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    if (!s_settingsGP) return;
    if (!SDL_GamepadConnected(s_settingsGP)) {
        SDL_CloseGamepad(s_settingsGP);
        s_settingsGP = nullptr;
        return;
    }
    // SDL_PumpEvents required for GCController input to be processed
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        if (SDL_GetGamepadButton(s_settingsGP, (SDL_GamepadButton)b)) {
            s_capturedButton.store(b);
            break;
        }
    }
}

+ (int)capturedButton {
    return s_capturedButton.exchange(-1);
}

+ (void)setButtonMapping:(int)ps2Index toSDLButton:(int)sdlButton {
    if (ps2Index >= 0 && ps2Index < 16) {
        s_buttonMap[ps2Index] = sdlButton;
        // Persist to INI
        char key[32];
        snprintf(key, sizeof(key), "Button%d", ps2Index);
        Host::SetBaseIntSettingValue("iPSX2/GamepadMapping", key, sdlButton);
        Host::CommitBaseSettingChanges();
    }
}

+ (int)getButtonMapping:(int)ps2Index {
    if (ps2Index >= 0 && ps2Index < 16) return s_buttonMap[ps2Index];
    return -1;
}

+ (void)resetButtonMappings {
    static const int defMap[16] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        -1, -1,
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    };
    for (int i = 0; i < 16; i++) s_buttonMap[i] = defMap[i];
    // No Host:: wrapper removes a whole section, so drop the keys we own individually.
    for (int i = 0; i < 16; i++) {
        char key[32];
        snprintf(key, sizeof(key), "Button%d", i);
        Host::RemoveBaseSettingValue("iPSX2/GamepadMapping", key);
    }
    Host::CommitBaseSettingChanges();
}

#pragma mark - Per-game settings and cheats (TWEAK)

// The core installs gamesettings/<serial>_<CRC>.ini as a layer above the global INI on every
// boot, and reads through the stack so an override wins. That layer cannot be written to — every
// setter on the layered interface aborts by design — so editing means opening a standalone INI
// over the same path, which is what the core's own settings UI does.
//
// One edit is open at a time, held here with the identity of the game it belongs to. Closing is
// what commits: empty sections are removed, a file left with nothing in it is deleted (that is
// what makes "use the global value" inherit rather than freeze), and the live layer is reloaded
// only when the edited game is the one actually running.
NSString * const iPSX2PatchKeyName               = @"name";
NSString * const iPSX2PatchKeyDisplayName        = @"displayName";
NSString * const iPSX2PatchKeyGroup              = @"group";
NSString * const iPSX2PatchKeyDescription        = @"description";
NSString * const iPSX2PatchKeyAuthor             = @"author";
NSString * const iPSX2PatchKeyEnabled            = @"enabled";
NSString * const iPSX2PatchKeyGloballyToggleable = @"globallyToggleable";

static std::unique_ptr<INISettingsInterface> s_game_settings_edit;
static std::string s_game_settings_serial;
static u32 s_game_settings_crc = 0;
// How many screens are currently inside this session. A pushed sub-screen (the cheat list)
// edits the same file as the screen that pushed it, and SwiftUI's ordering of the parent's
// disappear against the child's appear is not something to bet on: whichever way round they
// fire, a depth count keeps the session alive for exactly as long as someone is using it.
static int s_game_settings_depth = 0;

/// The (serial, crc) pair a game-list entry is filed under. An ELF entry deliberately gets an
/// empty serial: the core keys its settings on the CRC alone for those, because the serial is
/// itself something a per-game setting can override.
static bool GameKeyForEntryPath(NSString* path, std::string* serial, u32* crc)
{
    auto lock = GameList::GetLock();
    const GameList::Entry* e = GameList::GetEntryForPath(path.UTF8String);
    if (!e)
        return false;
    *serial = (e->type == GameList::EntryType::ELF) ? std::string() : e->serial;
    *crc = e->crc;
    return true;
}

+ (BOOL)openGameSettingsForEntryPath:(nonnull NSString *)path {
    std::string serial;
    u32 crc = 0;
    if (!GameKeyForEntryPath(path, &serial, &crc))
        return NO;

    if (s_game_settings_edit) {
        if (s_game_settings_serial == serial && s_game_settings_crc == crc) {
            s_game_settings_depth++;
            return YES;
        }
        // A different game — commit what is open before switching to it.
        s_game_settings_depth = 1;
        [self closeGameSettings];
    }

    s_game_settings_serial = serial;
    s_game_settings_crc = crc;
    s_game_settings_edit = std::make_unique<INISettingsInterface>(VMManager::GetGameSettingsPath(serial, crc));
    // Load fails when the file does not exist yet, which is the normal case for a game with no
    // overrides — an empty interface is exactly what we want to start editing.
    s_game_settings_edit->Load();
    s_game_settings_depth = 1;
    return YES;
}

/// Writes the open edit to disk. Called after every change rather than only on close: iOS can
/// kill a backgrounded app without running any teardown, and an override the user set and then
/// lost is worse than one that never appeared.
///
/// Deleting the file when nothing is left is what makes "use the global setting" inherit rather
/// than freeze — but the deletion cannot happen here. INISettingsInterface saves itself from its
/// destructor whenever it is dirty, and every mutator (including DeleteValue and
/// RemoveEmptySections) marks it dirty, so unlinking a file the object still intends to write
/// just gets it recreated. The empty case is therefore left to closeGameSettings, which unlinks
/// *after* the object is gone.
static void SaveGameSettingsEdit()
{
    if (!s_game_settings_edit)
        return;
    s_game_settings_edit->RemoveEmptySections();
    if (s_game_settings_edit->IsEmpty())
        return; // handled on close, once the destructor can no longer write it back

    Error error;
    if (!s_game_settings_edit->Save(&error)) {
        Console.Error("Failed to save game settings '%s': %s",
                      s_game_settings_edit->GetFileName().c_str(), error.GetDescription().c_str());
    }
}

+ (void)closeGameSettings {
    if (!s_game_settings_edit)
        return;
    if (--s_game_settings_depth > 0)
        return; // a sub-screen is still editing the same file

    s_game_settings_edit->RemoveEmptySections();
    const std::string filename = s_game_settings_edit->GetFileName();
    const bool is_empty = s_game_settings_edit->IsEmpty();
    if (!is_empty)
        SaveGameSettingsEdit();

    // Only disturb a running VM when the edit was for the game it is running.
    const bool same_game = VMManager::HasValidVM() &&
                           VMManager::GetDiscCRC() == s_game_settings_crc &&
                           (s_game_settings_serial.empty() || VMManager::GetDiscSerial() == s_game_settings_serial);

    // The destructor writes the file back if the object is still dirty, so the unlink has to
    // come after it — otherwise "no overrides left" leaves an empty file behind and the game
    // keeps reporting itself as customised.
    s_game_settings_edit.reset();

    if (is_empty && FileSystem::FileExists(filename.c_str())) {
        Error error;
        if (!FileSystem::DeleteFilePath(filename.c_str(), &error))
            Console.Error("Failed to delete empty game settings '%s': %s", filename.c_str(), error.GetDescription().c_str());
    }

    s_game_settings_serial.clear();
    s_game_settings_crc = 0;
    s_game_settings_depth = 0;

    if (same_game)
        Host::RunOnCPUThread([]() { VMManager::ReloadGameSettings(); });
}

+ (BOOL)gameSettingsExistForEntryPath:(nonnull NSString *)path {
    std::string serial;
    u32 crc = 0;
    if (!GameKeyForEntryPath(path, &serial, &crc))
        return NO;
    return FileSystem::FileExists(VMManager::GetGameSettingsPath(serial, crc).c_str()) ? YES : NO;
}

+ (BOOL)gameSettingsHasOverride:(nonnull NSString *)section key:(nonnull NSString *)key {
    if (!s_game_settings_edit)
        return NO;
    return s_game_settings_edit->ContainsValue(section.UTF8String, key.UTF8String) ? YES : NO;
}

+ (int)gameSettingsInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def {
    if (!s_game_settings_edit)
        return def;
    return s_game_settings_edit->GetIntValue(section.UTF8String, key.UTF8String, def);
}

+ (BOOL)gameSettingsBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def {
    if (!s_game_settings_edit)
        return def;
    return s_game_settings_edit->GetBoolValue(section.UTF8String, key.UTF8String, def ? true : false) ? YES : NO;
}

+ (float)gameSettingsFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def {
    if (!s_game_settings_edit)
        return def;
    return s_game_settings_edit->GetFloatValue(section.UTF8String, key.UTF8String, def);
}

+ (nonnull NSString *)gameSettingsString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def {
    if (!s_game_settings_edit)
        return def;
    const std::string val = s_game_settings_edit->GetStringValue(section.UTF8String, key.UTF8String, def.UTF8String);
    NSString* out = [NSString stringWithUTF8String:val.c_str()];
    return out ?: def;
}

+ (void)setGameSettingsInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value {
    if (!s_game_settings_edit)
        return;
    s_game_settings_edit->SetIntValue(section.UTF8String, key.UTF8String, value);
    SaveGameSettingsEdit();
}

+ (void)setGameSettingsBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value {
    if (!s_game_settings_edit)
        return;
    s_game_settings_edit->SetBoolValue(section.UTF8String, key.UTF8String, value ? true : false);
    SaveGameSettingsEdit();
}

+ (void)setGameSettingsFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value {
    if (!s_game_settings_edit)
        return;
    s_game_settings_edit->SetFloatValue(section.UTF8String, key.UTF8String, value);
    SaveGameSettingsEdit();
}

+ (void)setGameSettingsString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value {
    if (!s_game_settings_edit)
        return;
    s_game_settings_edit->SetStringValue(section.UTF8String, key.UTF8String, value.UTF8String);
    SaveGameSettingsEdit();
}

+ (void)clearGameSettings:(nonnull NSString *)section key:(nonnull NSString *)key {
    if (!s_game_settings_edit)
        return;
    s_game_settings_edit->DeleteValue(section.UTF8String, key.UTF8String);
    SaveGameSettingsEdit();
}

+ (void)clearAllGameSettings {
    if (!s_game_settings_edit)
        return;
    s_game_settings_edit->Clear();
    // Nothing to save — an empty edit is committed by deleting the file on close.
}

// --- Cheats -------------------------------------------------------------------------------
//
// showAllCRCS is true on purpose: it lists every CRC variant filed under the serial, which is
// what a user who dropped in a .pnach for a different revision of the same game expects to see.
// Cheats only — the shipped patch archive is zip-backed and lazily opened without a lock, so
// enumerating it from the UI thread could race a reload on the CPU thread.

+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)cheatsForEntryPath:(nonnull NSString *)path {
    std::string serial;
    u32 crc = 0;
    if (!GameKeyForEntryPath(path, &serial, &crc))
        return @[];

    u32 unlabelled = 0;
    Patch::PatchInfoList list = Patch::GetPatchInfo(serial, crc, true, true, &unlabelled);
    std::sort(list.begin(), list.end(),
              [](const Patch::PatchInfo& a, const Patch::PatchInfo& b) { return a.name < b.name; });

    // Which ones are ticked comes from the per-game INI being edited when one is open, and from
    // the file on disk otherwise, so the list is correct whether or not an edit session started.
    std::vector<std::string> enabled;
    if (s_game_settings_edit && s_game_settings_crc == crc) {
        enabled = s_game_settings_edit->GetStringList(Patch::CHEATS_CONFIG_SECTION, Patch::PATCH_ENABLE_CONFIG_KEY);
    } else {
        INISettingsInterface si(VMManager::GetGameSettingsPath(serial, crc));
        if (si.Load())
            enabled = si.GetStringList(Patch::CHEATS_CONFIG_SECTION, Patch::PATCH_ENABLE_CONFIG_KEY);
    }

    NSMutableArray* out = [NSMutableArray arrayWithCapacity:list.size()];
    for (const Patch::PatchInfo& p : list) {
        const bool is_enabled = std::find(enabled.begin(), enabled.end(), p.name) != enabled.end();
        [out addObject:@{
            iPSX2PatchKeyName               : NSStr(p.name),
            iPSX2PatchKeyDisplayName        : NSStr(std::string(p.GetNamePart())),
            iPSX2PatchKeyGroup              : NSStr(std::string(p.GetNameParentPart())),
            iPSX2PatchKeyDescription        : NSStr(p.description),
            iPSX2PatchKeyAuthor             : NSStr(p.author),
            iPSX2PatchKeyEnabled            : @(is_enabled),
            iPSX2PatchKeyGloballyToggleable : @(Patch::IsGloballyToggleablePatch(p)),
        }];
    }
    return out;
}

+ (NSUInteger)unlabelledCheatCountForEntryPath:(nonnull NSString *)path {
    std::string serial;
    u32 crc = 0;
    if (!GameKeyForEntryPath(path, &serial, &crc))
        return 0;
    u32 unlabelled = 0;
    Patch::GetPatchInfo(serial, crc, true, true, &unlabelled);
    return unlabelled;
}

+ (void)setCheat:(nonnull NSString *)name enabled:(BOOL)enabled {
    if (!s_game_settings_edit)
        return;
    if (enabled)
        s_game_settings_edit->AddToStringList(Patch::CHEATS_CONFIG_SECTION, Patch::PATCH_ENABLE_CONFIG_KEY, name.UTF8String);
    else
        s_game_settings_edit->RemoveFromStringList(Patch::CHEATS_CONFIG_SECTION, Patch::PATCH_ENABLE_CONFIG_KEY, name.UTF8String);
    SaveGameSettingsEdit();
}

+ (nonnull NSString *)cheatsDirectory {
    return NSStr(EmuFolders::Cheats);
}

+ (BOOL)importPnachAtPath:(nonnull NSString *)path {
    const std::string dest = Path::Combine(EmuFolders::Cheats, Path::GetFileName(path.UTF8String));
    if (dest.empty())
        return NO;
    if (!FileSystem::CopyFilePath(path.UTF8String, dest.c_str(), true)) {
        Console.Error("Failed to import cheat file: %s", path.UTF8String);
        return NO;
    }
    Console.WriteLn("Imported cheat file: %s", dest.c_str());
    return YES;
}

@end

