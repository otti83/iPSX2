// iPSX2Bridge.h — ObjC bridge for C++ emulator control
// SPDX-License-Identifier: GPL-3.0+

#import <Foundation/Foundation.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

typedef NS_ENUM(NSInteger, iPSX2EmulatorState) {
    iPSX2EmulatorStateStopped = 0,
    iPSX2EmulatorStateRunning,
    iPSX2EmulatorStatePaused,
    iPSX2EmulatorStateSaving,
    iPSX2EmulatorStateSuspended,
};

typedef NS_ENUM(NSInteger, iPSX2CoreType) {
    iPSX2CoreTypeJIT = 0,
    iPSX2CoreTypeInterpreter = 1,
};

/// [SPEED] Mirrors LimiterModeType (Config.h:263).
typedef NS_ENUM(NSInteger, iPSX2LimiterMode) {
    iPSX2LimiterModeNormal = 0,
    iPSX2LimiterModeTurbo,
    iPSX2LimiterModeSlowMotion,
    iPSX2LimiterModeUnlimited,
};

typedef NS_ENUM(NSInteger, iPSX2PadButton) {
    iPSX2PadButtonUp = 0,
    iPSX2PadButtonDown,
    iPSX2PadButtonLeft,
    iPSX2PadButtonRight,
    iPSX2PadButtonCross,
    iPSX2PadButtonCircle,
    iPSX2PadButtonSquare,
    iPSX2PadButtonTriangle,
    iPSX2PadButtonL1,
    iPSX2PadButtonR1,
    iPSX2PadButtonL2,
    iPSX2PadButtonR2,
    iPSX2PadButtonStart,
    iPSX2PadButtonSelect,
    iPSX2PadButtonL3,
    iPSX2PadButtonR3,
};

/// [COVER] Posted on the main queue when a cover download run finishes.
extern NSString * _Nonnull const iPSX2CoverDownloadDidFinishNotification;

/// [F8] Posted on the main queue when a game-list refresh finishes.
extern NSString * _Nonnull const iPSX2GameListDidRefreshNotification;

/// [S2] Keys in the dictionaries returned by +saveStateSlots.
extern NSString * _Nonnull const iPSX2SlotKeyIndex;      // NSNumber, 1-based
extern NSString * _Nonnull const iPSX2SlotKeyExists;     // NSNumber BOOL
extern NSString * _Nonnull const iPSX2SlotKeyDate;       // NSDate, absent when empty
extern NSString * _Nonnull const iPSX2SlotKeyScreenshot; // NSData (PNG), absent when none
extern NSString * _Nonnull const iPSX2SlotKeyHasBackup;  // NSNumber BOOL

/// [F8] Keys in the dictionaries returned by +gameListEntries.
extern NSString * _Nonnull const iPSX2GameKeyPath;
extern NSString * _Nonnull const iPSX2GameKeySerial;
extern NSString * _Nonnull const iPSX2GameKeyTitle;
extern NSString * _Nonnull const iPSX2GameKeyTitleEn;
extern NSString * _Nonnull const iPSX2GameKeyRegion;
extern NSString * _Nonnull const iPSX2GameKeyType;
extern NSString * _Nonnull const iPSX2GameKeyCRC;
extern NSString * _Nonnull const iPSX2GameKeySize;
extern NSString * _Nonnull const iPSX2GameKeyCompatibility;
extern NSString * _Nonnull const iPSX2GameKeyLastPlayed;
extern NSString * _Nonnull const iPSX2GameKeyTotalPlayed;
extern NSString * _Nonnull const iPSX2GameKeyCoverPath;

/// [SCAN] Keys in the dictionaries returned by +gameDirectoryEntries.
extern NSString * _Nonnull const iPSX2GameDirKeyPath;      // NSString, absolute
extern NSString * _Nonnull const iPSX2GameDirKeyRecursive; // NSNumber BOOL, scans subfolders
extern NSString * _Nonnull const iPSX2GameDirKeyBuiltIn;   // NSNumber BOOL, cannot be removed
extern NSString * _Nonnull const iPSX2GameDirKeyExists;    // NSNumber BOOL, present on disk now

/// [TWEAK] Keys in the dictionaries returned by +cheatsForEntryPath:.
extern NSString * _Nonnull const iPSX2PatchKeyName;               // NSString, full name = the INI token
extern NSString * _Nonnull const iPSX2PatchKeyDisplayName;        // NSString, leaf of a "parent\\child" name
extern NSString * _Nonnull const iPSX2PatchKeyGroup;              // NSString, parent part, "" at top level
extern NSString * _Nonnull const iPSX2PatchKeyDescription;        // NSString
extern NSString * _Nonnull const iPSX2PatchKeyAuthor;             // NSString
extern NSString * _Nonnull const iPSX2PatchKeyEnabled;            // NSNumber BOOL
extern NSString * _Nonnull const iPSX2PatchKeyGloballyToggleable; // NSNumber BOOL, owned by a global switch

@interface iPSX2Bridge : NSObject

// Game render view
#if TARGET_OS_IPHONE
+ (nonnull UIView *)gameRenderView;
#else
+ (nonnull NSView *)gameRenderView;
#endif

// Lifecycle
+ (void)saveNVRAM;
+ (void)saveMemoryCards;
+ (void)saveAllState;  // NVM + MC
+ (BOOL)isRunning;

// NVM status
+ (nullable NSDate *)lastNVMSaveDate;
+ (nullable NSString *)nvmFilePath;
+ (BOOL)nvmFileExists;

// Pad input
+ (void)setPadButton:(iPSX2PadButton)button pressed:(BOOL)pressed;
+ (void)setLeftStickX:(float)x Y:(float)y;
+ (void)setRightStickX:(float)x Y:(float)y;

// VM control
+ (void)requestVMStop;
+ (void)setFullScreen:(BOOL)enabled;

// Info
+ (nonnull NSString *)biosName;
+ (nonnull NSString *)buildVersion;

// ISO management
+ (nullable NSString *)currentISOPath;
+ (nonnull NSString *)isoDirectory;
+ (nonnull NSString *)documentsDirectory;
+ (nonnull NSArray<NSString *> *)availableISOs;

// [P44] ISO boot
+ (void)bootISO:(nonnull NSString *)isoName;

// [P44] BIOS management
+ (nonnull NSString *)biosDirectory;
+ (nonnull NSArray<NSString *> *)availableBIOSes;
+ (nonnull NSString *)defaultBIOSName;
+ (void)setDefaultBIOS:(nonnull NSString *)biosName;

// [P44] Favorites
+ (BOOL)isFavorite:(nonnull NSString *)isoName;
+ (void)setFavorite:(nonnull NSString *)isoName favorite:(BOOL)favorite;

// [P44] INI generic getter/setter
+ (int)getINIInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def;
+ (BOOL)getINIBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def;
+ (float)getINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def;
+ (nonnull NSString *)getINIString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def;
+ (void)setINIInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value;
+ (void)setINIBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value;
+ (void)setINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value;
+ (void)setINIString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value;

/// Re-reads the whole configuration from the INI on the CPU thread, so a value just written
/// with the setters above takes effect without rebooting the VM. The setters only persist —
/// nothing re-reads on its own. Safe to call with no VM running.
+ (void)applySettingsNow;

// [P44] VM lifecycle for menu flow
+ (BOOL)isVMRunning;
+ (BOOL)hasBIOS;
+ (void)requestVMBoot;
+ (void)requestVMShutdown;

// [F8] Game library — backed by upstream GameList (serial/CRC/region/title/compatibility/
// play time/cover), replacing the raw-filename listing above. Entry dictionaries use the
// keys declared above this interface.
+ (void)refreshGameList:(BOOL)invalidateCache;
+ (BOOL)isGameListRefreshing;
+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)gameListEntries;
/// [SCAN] The directories GameList walks. Two of them are built in and re-derived from the
/// app's own data root on every launch — Documents itself (one level deep) and Documents/iso
/// (including subfolders) — so removing one would only last until the next launch; they are
/// reported with iPSX2GameDirKeyBuiltIn set. Everything else is the user's own list.
+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)gameDirectoryEntries;
+ (void)addGameDirectory:(nonnull NSString *)path recursive:(BOOL)recursive;
+ (void)removeGameDirectory:(nonnull NSString *)path;
/// Boots by absolute path. Prefer this over bootISO: — a game list entry is not necessarily
/// a bare filename inside Documents/iso.
+ (void)bootGameAtPath:(nonnull NSString *)path;
/// Where a cover for this entry would live, whether or not one is present.
+ (nullable NSString *)coverPathForEntryPath:(nonnull NSString *)path;
/// Copies an image into the covers directory for this entry, replacing any existing one.
+ (BOOL)setCoverForEntryPath:(nonnull NSString *)path fromFile:(nonnull NSString *)sourceFile;
+ (void)setCustomTitle:(nullable NSString *)title forEntryPath:(nonnull NSString *)path;
+ (void)clearPlayTimeForSerial:(nonnull NSString *)serial;

// [SPEED] Emulation speed. The three multipliers live in [Framerate]; the limiter mode is
// runtime-only state, so it is not persisted and resets to Normal on each boot.
+ (iPSX2LimiterMode)limiterMode;
+ (void)setLimiterMode:(iPSX2LimiterMode)mode;
+ (BOOL)isPaused;
+ (void)setPaused:(BOOL)paused;
+ (void)frameAdvance;
/// Multipliers as percentages, matching how PCSX2 presents them (100 / 200 / 50).
+ (int)turboSpeedPercent;
+ (void)setTurboSpeedPercent:(int)percent;
+ (int)slowMotionSpeedPercent;
+ (void)setSlowMotionSpeedPercent:(int)percent;
+ (int)normalSpeedPercent;
+ (void)setNormalSpeedPercent:(int)percent;

// [COVER] Cover art acquisition. Display already works from Documents/covers; these are the
// two ways to get files there.
//
// ★ No default URL is shipped, deliberately. Upstream PCSX2 does not ship one either — the
//   cover downloader takes a template from the user. Placeholders: ${serial}, ${title},
//   ${titleen}, ${title_safe}, ${titleen_safe}, ${filetitle}, ${region}. Runs asynchronously;
//   observe iPSX2CoverDownloadDidFinishNotification, whose userInfo carries success (BOOL),
//   saved (int, covers actually written), missing (int, entries that had no cover when the run
//   started) and cancelled (BOOL). saved/missing are what separates "nothing to do" from
//   "nothing matched" — the core reports both as failures.
+ (void)downloadCoversWithTemplate:(nonnull NSString *)urlTemplate useSerial:(BOOL)useSerial;
+ (BOOL)isDownloadingCovers;
+ (void)cancelCoverDownload;
+ (int)coverDownloadProgress;   // 0..100
+ (nonnull NSString *)coversDirectory;

// [AUDIO] Volume is applied live through SPU2 so a slider can be dragged while a game runs;
// it is persisted to [SPU2/Output] as well so it survives a restart.
+ (int)audioVolume;              // 0..100
+ (void)setAudioVolume:(int)volume;
+ (BOOL)isAudioMuted;
+ (void)setAudioMuted:(BOOL)muted;

// [S2] Save states. Slots are 1..kSaveStateSlotCount; slot 0 is reserved for the resume
// state. Every mutating call goes through the CPU thread, so they return immediately and the
// caller should refresh from saveStateSlots afterwards.
+ (int)saveStateSlotCount;
/// One dictionary per slot, always slotCount entries, empty slots included. Keys below.
+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)saveStateSlots;
+ (void)saveStateToSlot:(int)slot;
+ (void)loadStateFromSlot:(int)slot backup:(BOOL)backup;
+ (BOOL)deleteStateInSlot:(int)slot;
/// True when a game is running and has a serial, i.e. slots are addressable at all.
+ (BOOL)canUseSaveStates;

// [TWEAK] Per-game settings and cheats.
//
// A per-game override lives in gamesettings/<serial>_<CRC>.ini, which the core installs as a
// layer above the global INI on every boot (VMManager::UpdateGameSettingsLayer). That layer is
// read-only from here on purpose — the layered interface aborts on any write — so editing goes
// through a standalone INI opened over the same path, exactly as the core's own settings UI
// does. Open it, edit, then close: closing removes empty sections, deletes the file outright if
// nothing is left (which is what makes "use the global value" actually inherit), and reloads
// the layer when the edited game is the one currently running.
//
// Everything is keyed on a game-list entry path rather than on serial+crc, because the rule for
// which of the two names the file is not obvious — an ELF entry uses the CRC alone — and
// getting it wrong writes a file that saves cleanly and is never read.
+ (BOOL)openGameSettingsForEntryPath:(nonnull NSString *)path;
+ (void)closeGameSettings;
/// True when the game already has an override file, without opening it.
+ (BOOL)gameSettingsExistForEntryPath:(nonnull NSString *)path;
/// True when this key is overridden for the open game, i.e. not inherited from the global value.
+ (BOOL)gameSettingsHasOverride:(nonnull NSString *)section key:(nonnull NSString *)key;
+ (int)gameSettingsInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def;
+ (BOOL)gameSettingsBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def;
+ (float)gameSettingsFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def;
+ (nonnull NSString *)gameSettingsString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def;
+ (void)setGameSettingsInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value;
+ (void)setGameSettingsBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value;
+ (void)setGameSettingsFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value;
+ (void)setGameSettingsString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value;
/// Drops the override so the key inherits the global value again. Not the same as writing the
/// current global value back, which would freeze it against later global changes.
+ (void)clearGameSettings:(nonnull NSString *)section key:(nonnull NSString *)key;
/// Removes every override for the open game. The file is deleted on close.
+ (void)clearAllGameSettings;

/// Cheats found for this entry, sorted by name. Restricted to cheats (.pnach files under
/// Documents/cheats); the shipped patch archive is a separate, zip-backed path that is not
/// safe to enumerate off the CPU thread.
+ (nonnull NSArray<NSDictionary<NSString *, id> *> *)cheatsForEntryPath:(nonnull NSString *)path;
/// Codes with no name in the .pnach. They cannot be listed or ticked individually — the core
/// activates them whenever cheats are on — so the UI can only say how many there are.
+ (NSUInteger)unlabelledCheatCountForEntryPath:(nonnull NSString *)path;
/// Requires an open game-settings edit; the tick is stored in the per-game INI.
+ (void)setCheat:(nonnull NSString *)name enabled:(BOOL)enabled;
+ (nonnull NSString *)cheatsDirectory;
/// Copies a .pnach into the cheats folder, keeping its filename. Returns NO on a failed copy.
+ (BOOL)importPnachAtPath:(nonnull NSString *)path;

// [P53] Gamepad button mapping
+ (void)startButtonCapture;
+ (void)stopButtonCapture;
+ (void)pollGamepadForCapture;  // call from main thread when VM is not running
+ (int)capturedButton;  // returns SDL_GamepadButton or -1
+ (void)setButtonMapping:(int)ps2Index toSDLButton:(int)sdlButton;
+ (int)getButtonMapping:(int)ps2Index;
+ (void)resetButtonMappings;

@end
