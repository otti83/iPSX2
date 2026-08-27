// Set SDL_MAIN_HANDLED to prevent SDL from redefining main()
#define SDL_MAIN_HANDLED

#if defined(iPSX2_MACOS)
// ============================================================================
// [P63] macOS native build — full PCSX2 initialization + SDL Metal window
// ============================================================================
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#import <Cocoa/Cocoa.h>
#include <sys/stat.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <algorithm>

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/WindowInfo.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/CDVD/CDVDcommon.h"
#include "HostCpuQueue.h"
#include "HostGameList.h"
#include "GameListPaths.h"
#include "OsdDefaults.h"

// --- Global state ---
static SDL_Window* s_macos_window = nullptr;
static INISettingsInterface* s_macos_settings = nullptr;
static std::atomic<bool> s_vm_running{false};
static std::atomic<bool> s_vm_quit{false};

// Host::AcquireRenderWindow — real implementation for macOS Metal
namespace Host {
    SDL_Window* g_sdl_window = nullptr;
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window) {
    if (!g_sdl_window) {
        Console.Error("[P63] AcquireRenderWindow: g_sdl_window is NULL");
        return std::nullopt;
    }
    WindowInfo wi = {};
    wi.type = WindowInfo::Type::MacOS;

    SDL_PropertiesID props = SDL_GetWindowProperties(g_sdl_window);
    NSWindow* nswin = (__bridge NSWindow*)SDL_GetPointerProperty(props,
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (!nswin) {
        Console.Error("[P63] AcquireRenderWindow: no NSWindow from SDL");
        return std::nullopt;
    }
    wi.window_handle = (__bridge void*)[nswin contentView];

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_sdl_window, &w, &h);
    wi.surface_width = (u32)w;
    wi.surface_height = (u32)h;
    wi.surface_scale = SDL_GetWindowDisplayScale(g_sdl_window);

    SDL_DisplayID display = SDL_GetDisplayForWindow(g_sdl_window);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    wi.surface_refresh_rate = mode ? mode->refresh_rate : 60.0f;

    Console.WriteLn("[P63] AcquireRenderWindow: %ux%u scale=%.2f",
        wi.surface_width, wi.surface_height, wi.surface_scale);
    return wi;
}

// --- Helper: setup directories ---
static void SetupDirectories(const std::string& dataRoot) {
    const char* dirs[] = {"bios", "iso", "logs", "memcards", "savestates",
                          "snaps", "cheats", "patches", "cache", "covers",
                          "gamesettings", "textures", "inputprofiles", "videos",
                          "inis"};
    mkdir(dataRoot.c_str(), 0755);
    for (auto d : dirs)
        mkdir((dataRoot + "/" + d).c_str(), 0755);

    EmuFolders::DataRoot = dataRoot;
    EmuFolders::AppRoot = dataRoot;
    // [F6] Upstream keeps this at DataRoot/inis, and nothing here ever assigned it. An empty
    // Settings makes every path derived from it resolve against the filesystem root — the log
    // showed "Debugger Layouts Directory: /debuggerlayouts" — and it silently broke the game
    // list's playtime.dat and custom_properties.ini plus relative DEV9 HDD paths.
    EmuFolders::Settings = dataRoot + "/inis";
    // [F3] Read-only runtime resources live in the .app bundle, not the user's data root.
    // This used to point at dataRoot, so the bundled GameIndex.yaml was never read and GameDB
    // only worked on machines where a copy had been placed in ~/Documents/iPSX2 by hand — a
    // fresh checkout had an empty database. iOS already resolved this correctly; now both do.
    if (NSString* rp = [[NSBundle mainBundle] resourcePath])
        EmuFolders::Resources = [rp UTF8String];
    else
        EmuFolders::Resources = dataRoot;
    EmuFolders::Bios = dataRoot + "/bios";
    EmuFolders::Logs = dataRoot + "/logs";
    EmuFolders::Savestates = dataRoot + "/savestates";
    EmuFolders::MemoryCards = dataRoot + "/memcards";
    EmuFolders::Snapshots = dataRoot + "/snaps";
    EmuFolders::Cheats = dataRoot + "/cheats";
    EmuFolders::Patches = dataRoot + "/patches";
    EmuFolders::Cache = dataRoot + "/cache";
    EmuFolders::Covers = dataRoot + "/covers";
    EmuFolders::GameSettings = dataRoot + "/gamesettings";
    EmuFolders::Textures = dataRoot + "/textures";
    EmuFolders::InputProfiles = dataRoot + "/inputprofiles";
    EmuFolders::Videos = dataRoot + "/videos";
}

// --- Helper: find BIOS in bios/ directory ---
static bool FindAndConfigureBIOS(const std::string& biosDir) {
    // Check existing config first
    if (!EmuConfig.BaseFilenames.Bios.empty()) {
        std::string path = Path::Combine(biosDir, EmuConfig.BaseFilenames.Bios);
        if (FileSystem::FileExists(path.c_str())) {
            Console.WriteLn("[P63] BIOS from config: %s", EmuConfig.BaseFilenames.Bios.c_str());
            return true;
        }
    }
    // Auto-scan for .bin files
    FileSystem::FindResultsArray results;
    if (FileSystem::FindFiles(biosDir.c_str(), "*", FILESYSTEM_FIND_FILES, &results)) {
        for (const auto& fd : results) {
            if (fd.Size >= 1024*1024 && fd.Size <= 50*1024*1024) {
                std::string fn(Path::GetFileName(fd.FileName));
                std::string ext = fn.size() >= 4 ? fn.substr(fn.size() - 4) : "";
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".bin" || ext == ".rom") {
                    EmuConfig.BaseFilenames.Bios = fn;
                    if (s_macos_settings) {
                        s_macos_settings->SetStringValue("Filenames", "BIOS", fn.c_str());
                        s_macos_settings->Save();
                    }
                    Console.WriteLn("[P63] BIOS auto-detected: %s", fn.c_str());
                    return true;
                }
            }
        }
    }
    return false;
}

// --- Helper: initialize settings ---
static void InitSettings(const std::string& dataRoot) {
    std::string iniPath = dataRoot + "/PCSX2-macOS.ini";
    s_macos_settings = new INISettingsInterface(iniPath);
    if (!s_macos_settings->Load()) {
        Console.WriteLn("[P63] Creating new config: %s", iniPath.c_str());
        s_macos_settings->SetIntValue("EmuCore/CPU", "CoreType", 0);
        s_macos_settings->SetBoolValue("EmuCore/CPU", "UseArm64Dynarec", false);
        s_macos_settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", true);
        s_macos_settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", true);
        s_macos_settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", true);
        s_macos_settings->SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", true);
        s_macos_settings->SetBoolValue("EmuCore/CPU", "EnableSparseMemory", true);
        // [JIT-OFF 2026-08-25] モードを明示的に書く。VMManager の fallback も iv2 になったので
        // 書かなくても iv2 に落ちるが、それだと **INI が自分の構成を語らない**。計測ホストでは
        // 「何で走っているか」がファイルから読めることが重要なので明示する
        // (Hard Rule 5c = INI が唯一の source)。
        // ★ macOS には iOS 側の jit→iv2 移行を入れていない。ここは計測ホストで、
        //   jit アームを手で組めることに意味があるため。
        s_macos_settings->SetStringValue("EmuCore/CPU", "EEMode", "iv2");
        s_macos_settings->SetStringValue("EmuCore/CPU", "IOPMode", "iv2");
        s_macos_settings->SetStringValue("EmuCore/CPU", "VU0Mode", "iv2");
        s_macos_settings->SetStringValue("EmuCore/CPU", "VU1Mode", "iv2");
        s_macos_settings->SetStringValue("SPU2/Output", "Backend", "SDL");
        s_macos_settings->SetIntValue("EmuCore/GS", "VsyncQueueSize", 8);
        s_macos_settings->SetBoolValue("EmuCore/Speedhacks", "MTVU", false);
        s_macos_settings->Save();
    }
    // [F8] Seed the directories GameList scans. Without an entry here GameList::Refresh walks
    // nothing and s_entries stays empty however often it is called.
    iPSX2::GameListPaths::Seed(s_macos_settings, dataRoot);
    iPSX2::OsdDefaults::Seed(s_macos_settings);
    s_macos_settings->Save();
    Host::Internal::SetBaseSettingsLayer(s_macos_settings);
}

// --- Graphics メニュー: レンダラー選択 -------------------------------------------
// なぜ要るか: GS のレンダラーは INI (`[EmuCore/GS] Renderer`) で決まるのに、macOS には
// それを切り替える手段が無かった (メニューの "Renderer: Metal" は action:nil の飾り)。
// iOS / Simulator は SwiftUI の Graphics 設定に picker があり
// SettingsStore が同じ INI キーを書くので、これで 3 プラットフォームが揃う。
//
// 反映は iOS 側と同じく **再起動時**。実行中の GSreopen は CPU スレッドで行う必要があり、
// メニュー (main スレッド) から直接叩くのは別作業として切る。
@interface iPSX2RendererMenuTarget : NSObject
- (void)selectRenderer:(NSMenuItem*)sender;
@end

@implementation iPSX2RendererMenuTarget
- (void)selectRenderer:(NSMenuItem*)sender
{
	if (!s_macos_settings)
		return;
	const int value = static_cast<int>([sender tag]);
	s_macos_settings->SetIntValue("EmuCore/GS", "Renderer", value);
	s_macos_settings->Save();
	// 同じ submenu 内の checkmark を張り替える
	for (NSMenuItem* item in [[sender menu] itemArray])
		[item setState:([item tag] == [sender tag]) ? NSControlStateValueOn : NSControlStateValueOff];
	Console.WriteLn("@@CFG@@ GS Renderer -> %d (INI に保存。再起動で反映)", value);
}
@end

static iPSX2RendererMenuTarget* s_renderer_menu_target = nil;

// --- OSD title bar update ---
#include "pcsx2/PerformanceMetrics.h"
static std::string s_titlebar_game;

static void UpdateWindowTitle() {
    if (!s_macos_window) return;
    char buf[256];
    int fps = (int)PerformanceMetrics::GetFPS();
    int speed = (int)PerformanceMetrics::GetSpeed();
    if (!s_titlebar_game.empty())
        snprintf(buf, sizeof(buf), "iPSX2 — %s | %d FPS | %d%%", s_titlebar_game.c_str(), fps, speed);
    else
        snprintf(buf, sizeof(buf), "iPSX2 — BIOS | %d FPS | %d%%", fps, speed);
    SDL_SetWindowTitle(s_macos_window, buf);
}

// --- Build boot parameters from INI ---
static VMBootParameters BuildBootParams() {
    VMBootParameters bp;
    bp.fast_boot = false;
    std::string isoDir = EmuFolders::DataRoot + "/iso";
    std::string isoFilename = s_macos_settings->GetStringValue("GameISO", "BootISO", "");
    bool fastBoot = s_macos_settings->GetBoolValue("GameISO", "FastBoot", false);
    if (!isoFilename.empty()) {
        // [F8] See the iOS branch: game-list entries carry an absolute path.
        std::string isoPath = Path::IsAbsolute(isoFilename) ? isoFilename : (isoDir + "/" + isoFilename);
        if (!FileSystem::FileExists(isoPath.c_str())) {
            std::string rootPath = EmuFolders::DataRoot + "/" + isoFilename;
            if (FileSystem::FileExists(rootPath.c_str()))
                isoPath = rootPath;
        }
        if (FileSystem::FileExists(isoPath.c_str())) {
            bp.filename = isoPath;
            bp.source_type = CDVD_SourceType::Iso;
            bp.fast_boot = fastBoot;
            Console.WriteLn("[P63] ISO: %s fast_boot=%d", isoPath.c_str(), fastBoot);
        }
    }
    // [P101-9 dev] load a save-state on boot (resume the user's captured scene for HLE capture/coverage).
    if (const char* st = std::getenv("iPSX2_LOAD_STATE")) {
        if (FileSystem::FileExists(st)) { bp.save_state = st; Console.WriteLn("[P63] LOAD_STATE: %s", st); }
        else Console.Error("[P63] LOAD_STATE not found: %s", st);
    }
    return bp;
}

// --- VM thread (persistent, with restart support) ---
static void VMThreadFunc() {
    Console.WriteLn("[P63] VM thread: CPUThreadInitialize...");
    if (!VMManager::Internal::CPUThreadInitialize()) {
        Console.Error("[P63] CPUThreadInitialize failed!");
        s_vm_running.store(false);
        return;
    }

    // Set ImGui font path
    std::string fontPath = Path::Combine(EmuFolders::Resources, "fonts/Roboto-Regular.ttf");
    if (!FileSystem::FileExists(fontPath.c_str()))
        fontPath = Path::Combine(EmuFolders::DataRoot, "fonts/Roboto-Regular.ttf");
    ImGuiManager::SetFontPathAndRange(std::move(fontPath), {});

    // Persistent boot loop — handles VM restart after PS2LOGO→game transitions
    while (!s_vm_quit.load()) {
        if (EmuConfig.BaseFilenames.Bios.empty() ||
            !FileSystem::FileExists(Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios).c_str())) {
            Console.Error("[P63] BIOS not found!");
            break;
        }

        VMBootParameters boot_params = BuildBootParams();
        Console.WriteLn("[P63] Booting with BIOS=%s", EmuConfig.BaseFilenames.Bios.c_str());

        if (VMManager::Initialize(boot_params)) {
            Console.WriteLn("[P63] VM initialized, entering run loop...");
            VMManager::SetState(VMState::Running);

            // Update title with game name
            s_titlebar_game = s_macos_settings->GetStringValue("GameISO", "BootISO", "");

            while (!s_vm_quit.load()) {
                VMState state = VMManager::GetState();
                if (state == VMState::Stopping || state == VMState::Shutdown)
                    break;
                if (state == VMState::Running)
                    VMManager::Execute();
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            Console.WriteLn("[P63] VM run loop exited (state=%d, quit=%d)",
                (int)VMManager::GetState(), s_vm_quit.load() ? 1 : 0);

            if (!s_vm_quit.load()) {
                // VM stopped but app still running — restart (PS2LOGO→game transition)
                Console.WriteLn("[P63] VM restarting...");
                VMManager::Shutdown(false);
                continue;
            }

            VMManager::Shutdown(false);
        } else {
            Console.Error("[P63] VMManager::Initialize failed!");
        }
        break;
    }

    VMManager::Internal::CPUThreadShutdown();
    s_vm_running.store(false);
    Console.WriteLn("[P63] VM thread exited.");
}

// --- macOS main ---
int main(int argc, char* argv[]) {
    @autoreleasepool {
        // 1. Data root
        const char* home = getenv("HOME");
        std::string dataRoot = std::string(home ? home : "/tmp") + "/Documents/iPSX2";
        SetupDirectories(dataRoot);

        // 2. Logging
        std::string logPath = dataRoot + "/pcsx2_log.txt";
        freopen(logPath.c_str(), "w", stderr);
        dup2(fileno(stderr), fileno(stdout));
        setvbuf(stderr, NULL, _IONBF, 0);
        setvbuf(stdout, NULL, _IONBF, 0);

        fprintf(stderr, "[P63] iPSX2 macOS native build starting...\n");
        fprintf(stderr, "[P63] DataRoot: %s\n", dataRoot.c_str());
        fprintf(stderr, "[P63] Log: %s\n", logPath.c_str());

        // 3. Settings
        InitSettings(dataRoot);
        VMManager::Internal::LoadStartupSettings();
        // [F8] Default OFF; see HostGameList.mm. Runs before any VM exists, which is required
        // because scanning reassigns the global CDVD reader.
        iPSX2::MaybeRunDiagnosticGameListScan();
        VMManager::ApplySettings();

        // 4. BIOS
        if (!FindAndConfigureBIOS(EmuFolders::Bios)) {
            fprintf(stderr, "[P63] ERROR: No BIOS found in %s/bios/\n", dataRoot.c_str());
            fprintf(stderr, "[P63] Copy a PS2 BIOS .bin file there and restart.\n");
            // Still open window to show the user something
        }

        // 5. SDL + Window
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD) < 0) {
            fprintf(stderr, "[P63] SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        // [HEADLESS] iPSX2_HEADLESS=1 creates the window HIDDEN (no on-screen window) for
        // autonomous/background runs (e.g. HLE kernel dev: self-check compares GIF packet
        // bytes and does not need on-screen rendering). Pair with INI Renderer=11 (Null) to
        // also skip the GPU. Default (unset) = normal visible window.
        Uint32 s_win_flags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE;
        const bool s_headless = (std::getenv("iPSX2_HEADLESS") != nullptr);
        if (s_headless) s_win_flags |= SDL_WINDOW_HIDDEN;
        s_macos_window = SDL_CreateWindow("iPSX2 macOS", 1280, 720, s_win_flags);
        if (!s_macos_window) {
            fprintf(stderr, "[P63] SDL_CreateWindow failed: %s\n", SDL_GetError());
            return 1;
        }
        Host::g_sdl_window = s_macos_window;
        // [HEADLESS] drop Dock icon / focus-stealing so background runs are unobtrusive.
        if (s_headless)
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        Console.WriteLn(s_headless ? "[P63] SDL window created (HEADLESS/hidden)." : "[P63] SDL window created.");

        // 6. Setup macOS native menu bar
        @autoreleasepool {
            NSMenu* menuBar = [[NSMenu alloc] init];

            // App menu
            NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
            NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"iPSX2"];
            [appMenu addItemWithTitle:@"About iPSX2" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
            [appMenu addItem:[NSMenuItem separatorItem]];
            [appMenu addItemWithTitle:@"Quit iPSX2" action:@selector(terminate:) keyEquivalent:@"q"];
            [appMenuItem setSubmenu:appMenu];
            [menuBar addItem:appMenuItem];

            // Emulation menu
            NSMenuItem* emuMenuItem = [[NSMenuItem alloc] init];
            NSMenu* emuMenu = [[NSMenu alloc] initWithTitle:@"Emulation"];
            [emuMenu addItemWithTitle:@"Pause / Resume" action:nil keyEquivalent:@"p"];
            [emuMenu addItemWithTitle:@"Reset" action:nil keyEquivalent:@"r"];
            [emuMenu addItem:[NSMenuItem separatorItem]];
            // CPU settings submenu
            NSMenuItem* cpuItem = [[NSMenuItem alloc] init];
            NSMenu* cpuMenu = [[NSMenu alloc] initWithTitle:@"CPU"];
            [cpuMenu addItemWithTitle:@"EE: JIT Recompiler" action:nil keyEquivalent:@""];
            [cpuMenu addItemWithTitle:@"EE: Interpreter" action:nil keyEquivalent:@""];
            [cpuMenu addItem:[NSMenuItem separatorItem]];
            [cpuMenu addItemWithTitle:@"IOP Recompiler" action:nil keyEquivalent:@""];
            [cpuMenu addItemWithTitle:@"VU0 Recompiler" action:nil keyEquivalent:@""];
            [cpuMenu addItemWithTitle:@"VU1 Recompiler" action:nil keyEquivalent:@""];
            [cpuItem setSubmenu:cpuMenu];
            [emuMenu addItem:cpuItem];
            // Speedhacks submenu
            NSMenuItem* hackItem = [[NSMenuItem alloc] init];
            NSMenu* hackMenu = [[NSMenu alloc] initWithTitle:@"Speed Hacks"];
            [hackMenu addItemWithTitle:@"MTVU (Multi-Threaded VU1)" action:nil keyEquivalent:@""];
            [hackMenu addItemWithTitle:@"Instant VU1" action:nil keyEquivalent:@""];
            [hackMenu addItemWithTitle:@"Fastmem" action:nil keyEquivalent:@""];
            [hackItem setSubmenu:hackMenu];
            [emuMenu addItem:hackItem];
            [emuMenuItem setSubmenu:emuMenu];
            [menuBar addItem:emuMenuItem];

            // GS menu
            NSMenuItem* gsMenuItem = [[NSMenuItem alloc] init];
            NSMenu* gsMenu = [[NSMenu alloc] initWithTitle:@"Graphics"];
            // Renderer 選択 (INI `[EmuCore/GS] Renderer`。反映は再起動時)
            if (!s_renderer_menu_target)
                s_renderer_menu_target = [[iPSX2RendererMenuTarget alloc] init];
            const int cur_renderer = s_macos_settings ?
                s_macos_settings->GetIntValue("EmuCore/GS", "Renderer", 17) : 17;
            NSMenuItem* rendItem = [[NSMenuItem alloc] init];
            [rendItem setTitle:@"Renderer (restart to apply)"];
            NSMenu* rendMenu = [[NSMenu alloc] initWithTitle:@"Renderer"];
            const struct { const char* title; int tag; } renderers[] = {
                {"Metal (Hardware)", 17}, {"Software", 13}, {"Null (No Output)", 11},
            };
            for (const auto& r : renderers)
            {
                NSMenuItem* it = [rendMenu addItemWithTitle:[NSString stringWithUTF8String:r.title]
                                                     action:@selector(selectRenderer:)
                                              keyEquivalent:@""];
                [it setTag:r.tag];
                [it setTarget:s_renderer_menu_target];
                [it setState:(r.tag == cur_renderer) ? NSControlStateValueOn : NSControlStateValueOff];
            }
            [rendItem setSubmenu:rendMenu];
            [gsMenu addItem:rendItem];
            [gsMenu addItem:[NSMenuItem separatorItem]];
            [gsMenu addItemWithTitle:@"Show FPS" action:nil keyEquivalent:@""];
            [gsMenu addItemWithTitle:@"Show CPU Usage" action:nil keyEquivalent:@""];
            [gsMenuItem setSubmenu:gsMenu];
            [menuBar addItem:gsMenuItem];

            // Window menu
            NSMenuItem* winMenuItem = [[NSMenuItem alloc] init];
            NSMenu* winMenu = [[NSMenu alloc] initWithTitle:@"Window"];
            [winMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
            [winMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
            [winMenu addItem:[NSMenuItem separatorItem]];
            [winMenu addItemWithTitle:@"Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
            [winMenuItem setSubmenu:winMenu];
            [menuBar addItem:winMenuItem];

            [NSApp setMainMenu:menuBar];
        }

        // 7. Launch VM thread (if BIOS found)
        std::thread vmThread;
        if (!EmuConfig.BaseFilenames.Bios.empty()) {
            s_vm_running.store(true);
            vmThread = std::thread(VMThreadFunc);
        }

        // 8. Main event loop (must run on main thread for Cocoa/Metal)
        bool running = true;
        uint32_t titleUpdateCounter = 0;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                    case SDL_EVENT_QUIT:
                        running = false;
                        break;
                    case SDL_EVENT_WINDOW_RESIZED: {
                        int w = event.window.data1, h = event.window.data2;
                        Console.WriteLn("[P63] Window resized: %dx%d", w, h);
                        break;
                    }
                    default:
                        break;
                }
            }

            // Update window title bar with OSD info (~1Hz)
            if (++titleUpdateCounter >= 60) {
                titleUpdateCounter = 0;
                UpdateWindowTitle();
            }

            // Check if VM thread has exited
            if (!s_vm_running.load() && vmThread.joinable()) {
                // VM finished — don't quit the app, let user close window
            }

            SDL_Delay(16);
        }

        // 9. Shutdown
        Console.WriteLn("[P63] Quit requested, shutting down...");
        s_vm_quit.store(true);
        if (vmThread.joinable())
            vmThread.join();

        SDL_DestroyWindow(s_macos_window);
        SDL_Quit();
        Console.WriteLn("[P63] Clean exit.");
        return 0;
    }
}
#else // TARGET_OS_IPHONE — original iOS code below

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_metal.h>

// SwiftUI integration — import auto-generated Swift header
#if __has_include("iPSX2-Swift.h")
#import "iPSX2-Swift.h"
#define iPSX2_HAS_SWIFTUI 1
#else
#define iPSX2_HAS_SWIFTUI 0
#endif
#import <SwiftUI/SwiftUI.h>

// ... other includes ...
#include <unistd.h>
#include <cstdlib>
#include <string>
#include <iostream>

#include "common/ProgressCallback.h"
#include "GameListPaths.h"
#include "OsdDefaults.h"
#include "BiosIntake.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/Counters.h" // g_FrameCount
#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVDdiscReader.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Config.h"
#include "HostCpuQueue.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/CDVD/CDVDcommon.h"

#include "pcsx2/DEV9/pcap_io.h"
#include "pcsx2/DEV9/net.h"

#include "pcsx2/Host.h"
#include "pcsx2/Host/AudioStreamTypes.h"

#include "common/WindowInfo.h"
#include "common/HTTPDownloader.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <optional>
#include <sys/stat.h> // For mkdir

struct rc_client_event_t;
struct rc_client_t;

// iOS specific headers
#import <UIKit/UIKit.h>
#include <mach-o/dyld.h>
#include "common/Darwin/DarwinMisc.h" // For iPSX2_CRASH_DIAG

// Global Log View
static UITextView* g_logView = nil;

// Game render view — frame-based layout, portrait=50% / landscape=full safe area
// Game render view with CAMetalLayer as backing layer (like MTKView)
// Game render view — backing layer (CAMetalLayer), manual landscape toggle
#include "pcsx2/MTGS.h"
// [iPSX2] 宣言をここで手書きすると上流の署名変更に追随できない
// (実際に上流が `int width, int height` → `u32` に変えてリンクエラーになった)。
// 本来の宣言元を include する。
#include "pcsx2/GS/GS.h"

@interface iPSX2GameView : UIView
@end
@implementation iPSX2GameView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (void)layoutSubviews {
    [super layoutSubviews];
    // Frame is set by SwiftUI — only update Metal drawable size
    CGFloat scale = self.contentScaleFactor;
    CAMetalLayer *mtl = (CAMetalLayer *)self.layer;
    mtl.drawableSize = CGSizeMake(self.bounds.size.width * scale,
                                   self.bounds.size.height * scale);
    int w = (int)(self.bounds.size.width * scale);
    int h = (int)(self.bounds.size.height * scale);
    float s = (float)scale;
    MTGS::RunOnGSThread([w, h, s]() {
        GSResizeDisplayWindow(w, h, s);
    });
}
@end
iPSX2GameView* g_gameRenderView = nil;  // non-static: accessed from iPSX2Bridge.mm

// Touch pad state
bool g_touchPadState[64] = {};

// Persistent VM thread lifecycle
static std::atomic<bool> s_vmThreadActive{false};   // true while VM is executing
std::atomic<bool> s_requestVMStop{false};     // signal VM to stop from UI (extern for iPSX2Bridge)
static std::atomic<bool> s_requestVMBoot{false};     // signal VM thread to boot
static std::mutex s_vmMutex;
static std::condition_variable s_vmCV;
static bool s_vmThreadCreated = false;               // guarded by s_vmMutex

// Gamepad button mapping — 16 PS2 buttons → SDL_GamepadButton
std::atomic<bool> s_captureMode{false};
std::atomic<int>  s_capturedButton{-1};

// Default mapping: PS2 index → SDL_GamepadButton
int s_buttonMap[16] = {
    SDL_GAMEPAD_BUTTON_DPAD_UP,        // 0  PAD_UP
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,      // 1  PAD_DOWN
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,      // 2  PAD_LEFT
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     // 3  PAD_RIGHT
    SDL_GAMEPAD_BUTTON_SOUTH,          // 4  PAD_CROSS
    SDL_GAMEPAD_BUTTON_EAST,           // 5  PAD_CIRCLE
    SDL_GAMEPAD_BUTTON_WEST,           // 6  PAD_SQUARE
    SDL_GAMEPAD_BUTTON_NORTH,          // 7  PAD_TRIANGLE
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  // 8  PAD_L1
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, // 9  PAD_R1
    -1,                                // 10 PAD_L2 (analog trigger)
    -1,                                // 11 PAD_R2 (analog trigger)
    SDL_GAMEPAD_BUTTON_START,          // 12 PAD_START
    SDL_GAMEPAD_BUTTON_BACK,           // 13 PAD_SELECT
    SDL_GAMEPAD_BUTTON_LEFT_STICK,     // 14 PAD_L3
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,    // 15 PAD_R3
};
const int s_defaultMap[16] = {
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    -1, -1,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
};

// View controller references for background color switching
static UIViewController* __unsafe_unretained s_menuVC = nil;
static UIViewController* __unsafe_unretained s_rootVC = nil;

// Helper to log to screen (thread safe)
void LogToScreen(const char* str) {
    if (!str) return;
    NSString *msg = [NSString stringWithUTF8String:str];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_logView) {
            g_logView.text = [g_logView.text stringByAppendingString:msg];
            if (g_logView.text.length > 20000) {
                 g_logView.text = [g_logView.text substringFromIndex:g_logView.text.length - 20000];
            }
            [g_logView scrollRangeToVisible:NSMakeRange(g_logView.text.length, 0)];
        }
    });
}

// ... Host Stubs (Keep existing ones) ...

// We need to forward declare the Host stubs here or ensure they are present.
// For brevity, I will include the critical parts and stubs.

// -- Host Implementation Start --

namespace Host
{
    SDL_Window* g_sdl_window = nullptr;

    void RequestShutdown() {
        SDL_Event event;
        event.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&event);
    }
    
    // ... [Include all other Host stubs from previous ios_main.mm] ...
    // Note: I will paste the stubs in the actual write call to ensure compilation.
    

    
    void RunOnMainThread(std::function<void()> func, bool wait) {
        if (wait) {
            dispatch_sync(dispatch_get_main_queue(), ^{ func(); });
        } else {
            dispatch_async(dispatch_get_main_queue(), ^{ func(); });
        }
    }

    // Only needed stubs for linking
    // GetHTTPUserAgent removed (duplicate)
    bool CopyTextToClipboard(const std::string_view text) { return false; }
    void OnOSDMessage(const std::string&, float, u32) {}
    void ReportError(const char*, const char*) {}
    bool ConfirmAction(const char*, const char*, const char*) { return true; }
    std::optional<std::string> OpenFileSelectionDialog(const char*, const char*, const char*, const char*) { return std::nullopt; }
    std::optional<std::string> OpenDirectorySelectionDialog(const char*, const char*) { return std::nullopt; }
    void SysLog(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
    void LoadSettings(SettingsInterface&, std::unique_lock<std::mutex>&) {} 
    void RequestResetSettings(bool) {} 
    const char* GetTranslatedStringImpl(const char* key) { return key; }
    u32 GetDisplayRefreshRate() { return 60; }
    std::optional<WindowInfo> AcquireRenderWindow(bool recreate_window) {
        Console.WriteLn("Host::AcquireRenderWindow(recreate=%d) called.", recreate_window);
        if (!g_sdl_window) {
            Console.Error("Host::AcquireRenderWindow: g_sdl_window is NULL");
            return std::nullopt;
        }
        
        __block WindowInfo wi = {};
        wi.type = WindowInfo::Type::iOS;
        
        // SDL calls that interact with UIKit must run on the main thread
        dispatch_sync(dispatch_get_main_queue(), ^{
            // SDL3 properties for UIKit
            SDL_PropertiesID props = SDL_GetWindowProperties(g_sdl_window);
            UIWindow* window = (__bridge UIWindow*)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
            
            if (window) {
// Use dedicated game render view if available (sized for portrait)
                 if (g_gameRenderView) {
                     wi.window_handle = (__bridge void*)g_gameRenderView;
                 } else {
                     wi.window_handle = (__bridge void*)[window rootViewController].view;
                 }
            }

            if (!wi.window_handle) {
                 Console.Error("Host::AcquireRenderWindow: Failed to get UIKit View (UIWindow=%p)", window);
                 // Last resort: some older SDL versions might put the view in the window property or vice versa
                 if (!wi.window_handle) wi.window_handle = (__bridge void*)window;
            }

// Get render size from the actual render view
            UIView* renderView = (__bridge UIView*)wi.window_handle;
            CGFloat scale = renderView.contentScaleFactor;
            wi.surface_width = static_cast<u32>(renderView.bounds.size.width * scale);
            wi.surface_height = static_cast<u32>(renderView.bounds.size.height * scale);
            wi.surface_scale = SDL_GetWindowDisplayScale(g_sdl_window);
            
            SDL_DisplayID display = SDL_GetDisplayForWindow(g_sdl_window);
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
            if (mode)
                wi.surface_refresh_rate = mode->refresh_rate;
            else
                wi.surface_refresh_rate = 60.0f;
        });
            
        Console.WriteLn("Host::AcquireRenderWindow: Returning WindowInfo (Type=%d, View=%p, Size=%ux%u, Scale=%.2f)", 
            (int)wi.type, wi.window_handle, wi.surface_width, wi.surface_height, wi.surface_scale);

        return wi;
    }
    void ReleaseRenderWindow() {}
    bool InNoGUIMode() { return false; }
    void OnVMPaused() {}
    void OnVMResumed() {}
    void OnVMStarted() {}
    void OnVMStarting() {}
    void EndTextInput() {}
    bool IsFullscreen() { return true; }
    void SetMouseMode(bool, bool) {}
    void OnGameChanged(const std::string&, const std::string&, const std::string&, const std::string&, unsigned int, unsigned int) {}
    // [F1] Drop anything still queued for a CPU thread that no longer exists.
    void OnVMDestroyed() { iPSX2::ResetCPUThreadQueue(); }
    void SetFullscreen(bool) {}
    void BeginTextInput() {}
    bool ConfirmMessage(std::string_view, std::string_view) { return true; }
    // [F1] Host::RunOnCPUThread now lives in HostCpuQueue.cpp (shared by both Apple targets).
    void ReportInfoAsync(std::string_view, std::string_view) {}
    void ReportErrorAsync(std::string_view title, std::string_view msg) {
        Console.Error("Host::ReportErrorAsync: %s - %s", std::string(title).c_str(), std::string(msg).c_str());
    }
    void BeginPresentFrame() {}
    bool LocaleCircleConfirm() { return false; }

    bool RequestResetSettings(bool, bool, bool, bool, bool) { return true; }
// [F8] Host::RefreshGameListAsync / CancelGameListRefresh now live in HostGameList.mm.
    void RequestVMShutdown(bool, bool, bool) {}
    void RequestExitBigPicture() {}
    void OnInputDeviceConnected(std::string_view, std::string_view) {}
    void RequestExitApplication(bool) {}
    void CheckForSettingsChanges(const Pcsx2Config&) {}
    void OnAchievementsRefreshed() {}
    void PumpMessagesOnCPUThread()
    {
        // [F1] Must stay first: every path below can return early, and a drain placed after one
        // of them would never run again for the rest of the session.
        iPSX2::DrainCPUThreadTasks();

// Check for VM shutdown request (safe: runs on CPU thread)
        if (s_requestVMStop.load()) {
            Console.WriteLn("[UI] PumpMessages: setting VM state to Stopping");
            VMManager::SetState(VMState::Stopping);
            return;
        }

        PadBase* pad = Pad::GetPad(0, 0);
        if (!pad) return;

#if TARGET_OS_SIMULATOR
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (!keys) return;

        static const struct { SDL_Scancode sc; u32 idx; } mapping[] = {
            { SDL_SCANCODE_UP,     PadDualshock2::Inputs::PAD_UP },
            { SDL_SCANCODE_DOWN,   PadDualshock2::Inputs::PAD_DOWN },
            { SDL_SCANCODE_LEFT,   PadDualshock2::Inputs::PAD_LEFT },
            { SDL_SCANCODE_RIGHT,  PadDualshock2::Inputs::PAD_RIGHT },
            { SDL_SCANCODE_Z,      PadDualshock2::Inputs::PAD_CIRCLE },
            { SDL_SCANCODE_X,      PadDualshock2::Inputs::PAD_CROSS },
            { SDL_SCANCODE_A,      PadDualshock2::Inputs::PAD_SQUARE },
            { SDL_SCANCODE_S,      PadDualshock2::Inputs::PAD_TRIANGLE },
            { SDL_SCANCODE_Q,      PadDualshock2::Inputs::PAD_L1 },
            { SDL_SCANCODE_W,      PadDualshock2::Inputs::PAD_R1 },
            { SDL_SCANCODE_1,      PadDualshock2::Inputs::PAD_L2 },
            { SDL_SCANCODE_2,      PadDualshock2::Inputs::PAD_R2 },
            { SDL_SCANCODE_RETURN, PadDualshock2::Inputs::PAD_START },
            { SDL_SCANCODE_SPACE,  PadDualshock2::Inputs::PAD_SELECT },
        };

        // Merge keyboard + touch input: only override with keyboard if
        // touch is not currently pressing the same button.
        for (const auto& m : mapping) {
            if (keys[m.sc])
                pad->Set(m.idx, 1.0f);
            else if (!g_touchPadState[m.idx])
                pad->Set(m.idx, 0.0f);
            // If touch is holding the button (g_touchPadState), don't reset it
        }
#endif // TARGET_OS_SIMULATOR — keyboard mapping

        // MFi / External gamepad support via SDL3
        {
            static SDL_Gamepad* s_gamepad = nullptr;
            // Auto-detect: open first available gamepad if not already open
            if (!s_gamepad) {
                int count = 0;
                SDL_JoystickID* ids = SDL_GetGamepads(&count);
                if (ids && count > 0) {
                    s_gamepad = SDL_OpenGamepad(ids[0]);
                    if (s_gamepad)
                        Console.WriteLn("[Files] MFi gamepad connected: %s", SDL_GetGamepadName(s_gamepad));
                }
                SDL_free(ids);
            }
            // Handle disconnect
            if (s_gamepad && !SDL_GamepadConnected(s_gamepad)) {
                Console.WriteLn("[Files] MFi gamepad disconnected");
                SDL_CloseGamepad(s_gamepad);
                s_gamepad = nullptr;
            }
            if (s_gamepad) {
                SDL_UpdateGamepads();
                // Capture mode: detect any pressed button for remapping UI
                if (s_captureMode.load()) {
                    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
                        if (SDL_GetGamepadButton(s_gamepad, (SDL_GamepadButton)b)) {
                            s_capturedButton.store(b);
                            break;
                        }
                    }
                }
// Use configurable mapping table
                static const u32 ps2Buttons[] = {
                    PadDualshock2::Inputs::PAD_UP, PadDualshock2::Inputs::PAD_DOWN,
                    PadDualshock2::Inputs::PAD_LEFT, PadDualshock2::Inputs::PAD_RIGHT,
                    PadDualshock2::Inputs::PAD_CROSS, PadDualshock2::Inputs::PAD_CIRCLE,
                    PadDualshock2::Inputs::PAD_SQUARE, PadDualshock2::Inputs::PAD_TRIANGLE,
                    PadDualshock2::Inputs::PAD_L1, PadDualshock2::Inputs::PAD_R1,
                    0, 0, // L2/R2 handled as analog
                    PadDualshock2::Inputs::PAD_START, PadDualshock2::Inputs::PAD_SELECT,
                    PadDualshock2::Inputs::PAD_L3, PadDualshock2::Inputs::PAD_R3,
                };
                for (int i = 0; i < 16; i++) {
                    int sdlBtn = s_buttonMap[i];
                    if (sdlBtn < 0) continue; // analog trigger
                    bool pressed = SDL_GetGamepadButton(s_gamepad, (SDL_GamepadButton)sdlBtn);
                    if (pressed)
                        pad->Set(ps2Buttons[i], 1.0f);
                    else if (!g_touchPadState[ps2Buttons[i]])
                        pad->Set(ps2Buttons[i], 0.0f);
                }
                // L2/R2 triggers (analog)
                float l2 = SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
                float r2 = SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
                if (l2 > 0.1f || !g_touchPadState[PadDualshock2::Inputs::PAD_L2])
                    pad->Set(PadDualshock2::Inputs::PAD_L2, l2 > 0.1f ? l2 : 0.0f);
                if (r2 > 0.1f || !g_touchPadState[PadDualshock2::Inputs::PAD_R2])
                    pad->Set(PadDualshock2::Inputs::PAD_R2, r2 > 0.1f ? r2 : 0.0f);
                // Analog sticks
                auto axis = [&](SDL_GamepadAxis a) -> float {
                    float v = SDL_GetGamepadAxis(s_gamepad, a) / 32767.0f;
                    return (v > 0.15f || v < -0.15f) ? v : 0.0f; // deadzone
                };
                float lx = axis(SDL_GAMEPAD_AXIS_LEFTX);
                float ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
                float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX);
                float ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
                pad->Set(PadDualshock2::Inputs::PAD_L_RIGHT, lx > 0 ? lx : 0.0f);
                pad->Set(PadDualshock2::Inputs::PAD_L_LEFT,  lx < 0 ? -lx : 0.0f);
                pad->Set(PadDualshock2::Inputs::PAD_L_DOWN,  ly > 0 ? ly : 0.0f);
                pad->Set(PadDualshock2::Inputs::PAD_L_UP,    ly < 0 ? -ly : 0.0f);
                pad->Set(PadDualshock2::Inputs::PAD_R_RIGHT, rx > 0 ? rx : 0.0f);
                pad->Set(PadDualshock2::Inputs::PAD_R_LEFT,  rx < 0 ? -rx : 0.0f);
                pad->Set(PadDualshock2::Inputs::PAD_R_DOWN,  ry > 0 ? ry : 0.0f);
                pad->Set(PadDualshock2::Inputs::PAD_R_UP,    ry < 0 ? -ry : 0.0f);
            }
        }

        // [BIOS_NAV] Auto-navigate BIOS — debug only
#if DEBUG
        if (const char* nav = getenv("iPSX2_BIOS_NAV"); nav && atoi(nav))
        {
            unsigned int fc = ::g_FrameCount;
            auto press = [&](u32 btn, unsigned int at) {
                if (fc >= at && fc <= at + 1) pad->Set(btn, 1.0f);
                else if (fc == at + 2) pad->Set(btn, 0.0f);
            };
            // BIOS nav: ↓ → ○ → ○ → ○ → ← → ○ → ○...
            // The exact screen order varies. Try multiple ←+○ combos.
            press(PadDualshock2::Inputs::PAD_DOWN, 600);
            press(PadDualshock2::Inputs::PAD_CIRCLE, 750);
            // After entering System Configuration, each screen needs ○ to advance.
            // The "initialization" dialog needs ← first to select "Yes".
            // Try ← before each ○ to handle wherever the dialog appears.
            unsigned int seq[] = {
                950,  0,  // ○ language
                1150, 0,  // ○ clock
                1350, 1,  // ← then ○ (init dialog attempt 1)
                1550, 1,  // ← then ○ (init dialog attempt 2)
                1750, 0,  // ○
                1950, 0,  // ○
                2150, 1,  // ← then ○ (attempt 3)
                2350, 0,  // ○
                2550, 0, 2750, 0, 2950, 0, 3150, 0, 3350, 0, 3550, 0,
            };
            for (int i = 0; i < (int)(sizeof(seq)/sizeof(seq[0])); i += 2) {
                unsigned int t = seq[i];
                if (seq[i+1]) // needs LEFT first
                    press(PadDualshock2::Inputs::PAD_LEFT, t);
                press(PadDualshock2::Inputs::PAD_CIRCLE, t + (seq[i+1] ? 100 : 0));
            }

            // Log after each step
            static const unsigned int cps[] = {650, 770, 950, 1130, 1300, 1500, 1800, 2100, 2400, 2700, 3000};
            for (auto cp : cps) {
                if (fc == cp) {
                    Console.WriteLn(Color_Yellow, "[BIOS_NAV] checkpoint f=%u", fc);
                }
            }
        }
#endif // DEBUG — BIOS_NAV
    }
    std::string TranslatePluralToString(const char*, const char*, const char*, int) { return ""; }
// [F2] Host::CommitBaseSettingChanges now lives in HostSettings.cpp (shared by both Apple targets).
    void OnInputDeviceDisconnected(InputBindingKey, std::string_view) {}
    void OpenHostFileSelectorAsync(std::string_view, bool, std::function<void(const std::string&)>, std::vector<std::string>, std::string_view) {}
    std::unique_ptr<ProgressCallback> CreateHostProgressCallback() { return nullptr; }
    void OnAchievementsLoginSuccess(char const*, u32, u32, u32) {}
    void OnPerformanceMetricsUpdated() {}
    void OnAchievementsLoginRequested(Achievements::LoginRequestReason) {}
    bool ShouldPreferHostFileSelector() { return false; }
    void OnCoverDownloaderOpenRequested() {}
    void OnCreateMemoryCardOpenRequested() {}
    void OnAchievementsHardcoreModeChanged(bool) {}
    void OpenURL(std::string_view) {}
}

namespace Host::Internal
{
    s32 GetTranslatedStringImpl(const std::string_view, const std::string_view, char*, size_t) { return 0; }
}

// Called from iPSX2Bridge to toggle SDL fullscreen (controls status bar visibility)
extern "C" void iPSX2_SetSDLFullscreen(bool enabled) {
    if (Host::g_sdl_window)
        SDL_SetWindowFullscreen(Host::g_sdl_window, enabled);
}

namespace Common {
    bool PlaySoundAsync(const char*) { return false; }
}

// IOCtlSrc Stubs
IOCtlSrc::IOCtlSrc(std::string filename) : m_filename(std::move(filename)) {}
IOCtlSrc::~IOCtlSrc() {}
bool IOCtlSrc::Reopen(Error*) { return false; }
u32 IOCtlSrc::GetSectorCount() const { return 0; }
const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const { static std::vector<toc_entry> empty; return empty; }
bool IOCtlSrc::ReadSectors2048(u32, u32, u8*) const { return false; }
bool IOCtlSrc::ReadSectors2352(u32, u32, u8*) const { return false; }
bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ*) const { return false; }
u32 IOCtlSrc::GetLayerBreakAddress() const { return 0; }
s32 IOCtlSrc::GetMediaType() const { return 0; }
void IOCtlSrc::SetSpindleSpeed(bool) const {}
bool IOCtlSrc::DiscReady() { return false; }

// ... InputManager Stubs ...
namespace InputManager {
    void Initialize() {}
    void Shutdown() {}
    void Update() {}
    void SetRumble(int, u8, u8) {}
    const char* ConvertHostKeyboardCodeToIcon(unsigned int) { return ""; }
    std::optional<std::string> ConvertHostKeyboardCodeToString(unsigned int) { return std::nullopt; }
    std::optional<unsigned int> ConvertHostKeyboardStringToCode(std::string_view) { return std::nullopt; }
}

// ... HTTP Stubs ...
// [F5] HTTPDownloader::Create now lives in common/HTTPDownloaderNSURL.mm.

// Global stubs for DISCopen
void GetValidDrive(std::string&) {  }
std::vector<std::string> GetOpticalDriveList() { return {}; }

namespace FileSystem {
    int OpenFDFileContent(const char*) { return -1; } // Added overload
    bool OpenFDFileContent(const std::string&, int, s64, s64) { return false; }
    std::string GetValidDrive(const std::string&) { return ""; }
    std::vector<std::string> GetOpticalDriveList() { return {}; }
}


// ... IOCtlSrc Stub Removed ...

// ... CocoaTools Stub ...
namespace CocoaTools {
    void InhibitAppNap(const std::string&) {}
    void UninhibitAppNap() {}
    std::string GetBundlePath() { return [[NSBundle mainBundle].bundlePath UTF8String]; }
    
    void* CreateMetalLayer(WindowInfo* wi) {
        if (!Host::g_sdl_window) return nullptr;
        
        // Return existing layer if we already have it
        if (wi->surface_handle) {
            return SDL_Metal_GetLayer((SDL_MetalView)wi->surface_handle);
        }
        
        // Create the Metal view
        SDL_MetalView view = SDL_Metal_CreateView(Host::g_sdl_window);
        if (!view) {
            Console.Error("SDL_Metal_CreateView failed: %s", SDL_GetError());
            return nullptr;
        }
        
        void* layer = SDL_Metal_GetLayer(view);
        wi->surface_handle = view; // Store view handle to destroy later
        Console.WriteLn("Created Metal Layer: %p from View: %p", layer, view);
        return layer;
    }
    
    void DestroyMetalLayer(WindowInfo* wi) {
        if (wi->surface_handle) {
            Console.WriteLn("Destroying Metal View: %p", wi->surface_handle);
            SDL_Metal_DestroyView((SDL_MetalView)wi->surface_handle);
            wi->surface_handle = nullptr;
        }
    }
}

// ... AudioStream Stub ...
#include "pcsx2/Host/AudioStream.h"
std::unique_ptr<AudioStream> AudioStream::CreateOboeAudioStream(unsigned int, AudioStreamParameters const&, bool, Error*) { return nullptr; }

// ... PCAP Stub ...
PCAPAdapter::PCAPAdapter() {}
PCAPAdapter::~PCAPAdapter() {}
bool PCAPAdapter::blocks() { return false; }
bool PCAPAdapter::isInitialised() { return false; }
bool PCAPAdapter::recv(NetPacket*) { return false; }
bool PCAPAdapter::send(NetPacket*) { return false; }
void PCAPAdapter::reloadSettings() {}
std::vector<AdapterEntry> PCAPAdapter::GetAdapters() { return {}; }
AdapterOptions PCAPAdapter::GetAdapterOptions() { return {}; }
bool PCAPAdapter::InitPCAP(const std::string&, bool) { return false; }
bool PCAPAdapter::SetMACSwitchedFilter(PacketReader::MAC_Address) { return false; }
void PCAPAdapter::SetMACBridgedRecv(NetPacket*) {}
void PCAPAdapter::SetMACBridgedSend(NetPacket*) {}
void PCAPAdapter::HandleFrameCheckSequence(NetPacket*) {}
bool PCAPAdapter::ValidateEtherFrame(NetPacket*) { return false; }


// ... FileSystem Stub ...
// Duplicate FileSystem block removed

// -- End Host Stubs --

// Settings Interface
#include "3rdparty/simpleini/include/SimpleIni.h"
#include "pcsx2/INISettingsInterface.h"

static INISettingsInterface* s_settings_interface = nullptr;
// Expose to iPSX2Bridge.mm via extern

//
// -- IOS AppDelegate & SceneDelegate --
//

@interface PCSX2SceneDelegate : UIResponder <UIWindowSceneDelegate, UIDocumentPickerDelegate>
@property (strong, nonatomic) UIWindow *window;
@property (strong, nonatomic) UIButton *startBiosButton;
@end

@implementation PCSX2SceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    if (![scene isKindOfClass:[UIWindowScene class]]) return;
    
    UIWindowScene *windowScene = (UIWindowScene *)scene;
    
    // --- SDL Initialization ---
    static bool s_initialized = false;
    if (!s_initialized) {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD) < 0) {
            NSLog(@"SDL_Init failed: %s", SDL_GetError());
            return;
        }
        s_initialized = true;
    }
    
    // --- Setup PCSX2 Environment ---
    // (Moved to AppDelegate)
    // We still need local variables if used below, but EmuFolders are global.
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *documentsDirectory = [paths objectAtIndex:0];
    std::string dataRoot = [documentsDirectory UTF8String];
    
    // Re-ensure EmuFolders (idempotent)
    EmuFolders::DataRoot = dataRoot;
    EmuFolders::Bios = dataRoot + "/bios";
    // ...
    
    Console.WriteLn("PCSX2 iOS: Initializing logic in SceneDelegate...");
    
    // Settings Initialization
    if (!s_settings_interface) {
        std::string iniPath = dataRoot + "/PCSX2-iOS.ini";
        s_settings_interface = new INISettingsInterface(iniPath);
        if (!static_cast<INISettingsInterface*>(s_settings_interface)->Load()) {
            Console.WriteLn("Creating new config at %s", iniPath.c_str());
            
            // [JIT-OFF 2026-08-25] 全コンポーネント iv2 が既定。JIT 版の出荷は行わない。
            // App Store では codegen が禁止で、jit を書いても起動時に必ず iv2 へ降格されていた
            // (= 設定に "JIT" と表示されるのに中身は IV2、という嘘の状態だった)。
            // Per-component engine selector (source of truth, see VMManager parse_mode):
            //   "interp" | "iv2" | "jit"。UI は interp / iv2 のみ提示する。
            s_settings_interface->SetStringValue("EmuCore/CPU", "EEMode", "iv2");
            s_settings_interface->SetStringValue("EmuCore/CPU", "IOPMode", "iv2");
            s_settings_interface->SetStringValue("EmuCore/CPU", "VU0Mode", "iv2");
            s_settings_interface->SetStringValue("EmuCore/CPU", "VU1Mode", "iv2");
            s_settings_interface->SetIntValue("EmuCore/CPU", "CoreType", 0); // Recompiler
            s_settings_interface->SetBoolValue("EmuCore/CPU", "UseArm64Dynarec", false);
            s_settings_interface->SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU", "EnableSparseMemory", true);
            s_settings_interface->SetBoolValue("EmuCore/CPU", "ExtraMemory", false);

            // Audio
            s_settings_interface->SetStringValue("SPU2/Output", "Backend", "SDL");

            // GS
            s_settings_interface->SetIntValue("EmuCore/GS", "VsyncQueueSize", 8);

            // Speedhacks
            s_settings_interface->SetBoolValue("EmuCore/Speedhacks", "MTVU", false);
            
            Console.WriteLn("@@CFG_DEFAULTS@@ created=1 CoreType=0 UseArm64Dynarec=false EnableEE=1");
            s_settings_interface->Save();
        }
        Host::Internal::SetBaseSettingsLayer(s_settings_interface);
// Load gamepad button mapping from INI
        for (int i = 0; i < 16; i++) {
            char key[32]; snprintf(key, sizeof(key), "Button%d", i);
            int val = s_settings_interface->GetIntValue("iPSX2/GamepadMapping", key, s_defaultMap[i]);
            s_buttonMap[i] = val;
        }
    }
    // One-time migration for existing INI (runs once, then conditions are false)
    if (!s_settings_interface->ContainsValue("SPU2/Output", "Backend")) {
        s_settings_interface->SetStringValue("SPU2/Output", "Backend", "SDL");
    }
    if (!s_settings_interface->ContainsValue("EmuCore/CPU", "ExtraMemory")) {
        s_settings_interface->SetBoolValue("EmuCore/CPU", "ExtraMemory", false);
    }
    // [JIT-OFF 2026-08-25] 既存 INI の "jit" を iv2 へ書き換える。App Store では codegen が
    // 禁止で、jit は起動時に必ず iv2 へ降格されていた = 設定画面が実態と食い違っていた。
    // ★ これは**毎起動**走る。したがって iOS/シミュレータでは INI を手で jit に戻しても
    //   VMManager が読む前に消される = iOS 側に jit の開発経路は無い。
    //   jit アームで計測したい場合は **macOS ビルドを使う** (macOS にはこの移行を入れていない)。
    //   interp は尊重する (書き換えるのは jit のときだけ)。
    for (const char* key : {"EEMode", "IOPMode", "VU0Mode", "VU1Mode"}) {
        if (s_settings_interface->GetStringValue("EmuCore/CPU", key, "") == "jit") {
            s_settings_interface->SetStringValue("EmuCore/CPU", key, "iv2");
            Console.WriteLn("[JIT-OFF] %s: jit -> iv2 (codegen is not permitted here)", key);
        }
    }

    // [F8] Seed the directories GameList scans; the rule is shared with the macOS branch and
    // lives in GameListPaths.h, including the container-rebase that keeps entries alive across
    // a reinstall. Documents/iso is scanned recursively and Documents itself one level deep —
    // games sitting directly in Documents used to belong to neither key and were never
    // enumerated, however often a refresh was asked for.
    iPSX2::GameListPaths::Seed(s_settings_interface, dataRoot);
    iPSX2::OsdDefaults::Seed(s_settings_interface);
    s_settings_interface->Save();
    [self checkAndConfigureBIOS];

    // [iPSX2] ここ (iOS / Simulator の経路) には「GS Renderer: Metal fixed on iOS」と称して
    //   EmuConfig.GS.Renderer = GSRendererType::Metal;
    // を書いていたが、**直後の LoadStartupSettings() が INI から読み直して上書きする**ため
    // 何の効果も無い死んだコードだった。コメントも実態と食い違っていた
    // (SwiftUI の Graphics 設定には以前からレンダラー picker があり、INI の
    //  `[EmuCore/GS] Renderer` を書いている = Metal 固定ではない)。
    // レンダラーは INI が single source (Hard Rule 5c)。既定は INI 未設定時の `Auto`
    // → GSUtil::GetPreferredRenderer() で Apple では Metal。
    // DEBUG 限定の `iPSX2_NULL_GS` env も Null が UI から選べるので併せて撤去。
    s_settings_interface->Save();

    VMManager::Internal::LoadStartupSettings();
    VMManager::ApplySettings();
    
    // --- Create SDL Window ---
    Host::g_sdl_window = SDL_CreateWindow("PCSX2 iOS", 1280, 720, SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE);
    if (!Host::g_sdl_window) {
        Console.Error("Failed to create SDL window: %s", SDL_GetError());
        return;
    }
    
    // --- Attach UIWindow ---
    UIWindow *uiWindow = (__bridge UIWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(Host::g_sdl_window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    if (uiWindow) {
        Console.WriteLn("Attaching UIWindow to Scene...");
        uiWindow.windowScene = windowScene;
        self.window = uiWindow;
        self.window.backgroundColor = [UIColor systemGroupedBackgroundColor];
        [self.window makeKeyAndVisible];

// Create game render view — SwiftUI MetalGameView (UIViewRepresentable) manages placement
        g_gameRenderView = [[iPSX2GameView alloc] initWithFrame:CGRectZero];
        g_gameRenderView.backgroundColor = [UIColor blackColor];
        g_gameRenderView.clipsToBounds = YES;
        // Do NOT addSubview here — SwiftUI's MetalGameView handles view hierarchy
        Console.WriteLn("[Layout] Game render view created (SwiftUI-managed)");
        
// Debug-only UI elements
#if DEBUG
        if (rootVC) {
            g_logView = [[UITextView alloc] initWithFrame:CGRectMake(10, 50, 600, 300)];
            g_logView.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.5];
            g_logView.textColor = [UIColor whiteColor];
            g_logView.font = [UIFont fontWithName:@"Courier" size:10];
            g_logView.editable = NO;
            g_logView.hidden = YES;
            [rootVC.view addSubview:g_logView];
        }
#endif
    }
    
    // --- [UI] Startup Logic: Show menu first, boot on user action ---
#if iPSX2_HAS_SWIFTUI
    {
        UIViewController *rootVC = self.window.rootViewController;
        if (rootVC) {
            rootVC.view.backgroundColor = [UIColor systemGroupedBackgroundColor];

            UIViewController *menuVC = [SwiftUIHost createMenuController];
            menuVC.view.translatesAutoresizingMaskIntoConstraints = NO;
            menuVC.view.userInteractionEnabled = YES;
// Keep hosting controller always clear — SwiftUI RootView handles its own background
            menuVC.view.backgroundColor = [UIColor clearColor];
            [rootVC.view addSubview:menuVC.view];
            [NSLayoutConstraint activateConstraints:@[
                [menuVC.view.topAnchor constraintEqualToAnchor:rootVC.view.topAnchor],
                [menuVC.view.bottomAnchor constraintEqualToAnchor:rootVC.view.bottomAnchor],
                [menuVC.view.leadingAnchor constraintEqualToAnchor:rootVC.view.leadingAnchor],
                [menuVC.view.trailingAnchor constraintEqualToAnchor:rootVC.view.trailingAnchor],
            ]];
            [rootVC addChildViewController:menuVC];
            [menuVC didMoveToParentViewController:rootVC];
            s_menuVC = menuVC;
            s_rootVC = rootVC;

            if (g_logView) {
                g_logView.hidden = YES;
                g_logView.userInteractionEnabled = NO;
            }
            Console.WriteLn("[UI] SwiftUI menu attached (screen: %.0fx%.0f)",
                rootVC.view.bounds.size.width, rootVC.view.bounds.size.height);

}
    }

    // Listen for VM boot request from SwiftUI
    // queue:nil = synchronous delivery, so background colors are set BEFORE
    // SwiftUI re-renders the game overlay (avoids gray flash)
    [[NSNotificationCenter defaultCenter] addObserverForName:@"iPSX2RequestVMBoot"
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
        Console.WriteLn("[UI] VM boot requested from UI (rootVC=%p)", s_rootVC);
        if (s_rootVC) s_rootVC.view.backgroundColor = [UIColor blackColor];
#if TARGET_OS_SIMULATOR
        [self startVMThread];
#else
        [self checkJITAndStartVM];
#endif
    }];

    [[NSNotificationCenter defaultCenter] addObserverForName:@"iPSX2RequestVMShutdown"
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
        Console.WriteLn("[UI] VM shutdown requested from UI");
        s_requestVMStop.store(true);
    }];

    // iPSX2VMDidShutdown / iPSX2ReturnToMenu: no rootVC background change needed.
    // SwiftUI RootView handles menu background via Color(systemGroupedBackground).ignoresSafeArea().
    // rootVC stays black after first boot — eliminates white flash during VM restart.

    [[NSNotificationCenter defaultCenter] addObserverForName:@"iPSX2VMDidShutdown"
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
        // No rootVC background change — SwiftUI handles menu bg
    }];

    [[NSNotificationCenter defaultCenter] addObserverForName:@"iPSX2ReturnToMenu"
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
        // No rootVC background change — SwiftUI handles menu bg
    }];

    [[NSNotificationCenter defaultCenter] addObserverForName:@"iPSX2EnterGameScreen"
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification * _Nonnull note) {
        if (s_rootVC) s_rootVC.view.backgroundColor = [UIColor blackColor];
    }];

// Auto-boot — debug/simulator only
#if DEBUG || TARGET_OS_SIMULATOR
    if (getenv("iPSX2_AUTO_BOOT") && atoi(getenv("iPSX2_AUTO_BOOT")) == 1) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            Console.WriteLn("[AutoBoot] @@AUTO_BOOT@@ posting iPSX2RequestVMBoot + AutoBootDidStart");
            [[NSNotificationCenter defaultCenter] postNotificationName:@"iPSX2RequestVMBoot" object:nil];
            [[NSNotificationCenter defaultCenter] postNotificationName:@"iPSX2AutoBootDidStart" object:nil];
        });
    }
#endif // DEBUG || TARGET_OS_SIMULATOR — AUTO_BOOT
    // ps2autotests: auto-boot VM when ELF env var is set (always enabled)
    if (getenv("iPSX2_BOOT_ELF")) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [[NSNotificationCenter defaultCenter] postNotificationName:@"iPSX2RequestVMBoot" object:nil];
        });
    }
#else
    // Fallback: no SwiftUI — auto-boot like before
    if (!EmuConfig.BaseFilenames.Bios.empty() && FileSystem::FileExists(Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios).c_str())) {
#if TARGET_OS_SIMULATOR
        [self startVMThread];
#else
        [self checkJITAndStartVM];
#endif
    } else {
        Console.Warning("No valid BIOS found. Showing selection UI.");
        if (self.startBiosButton) {
            self.startBiosButton.hidden = NO;
            [self.window bringSubviewToFront:self.startBiosButton];
        }
    }
#endif
}

- (void)checkAndConfigureBIOS {
    std::string dataRoot = EmuFolders::DataRoot;
    std::string biosDir = dataRoot + "/bios";
    
    // 0. [iPSX2] Check Env Var Override (iPSX2_BIOS_PATH)
    const char* envBios = getenv("iPSX2_BIOS_PATH");
    // Simulator: use iPSX2_BIOS_PATH env var or auto-scan Documents/bios/
    // Real device: BIOS must be placed in Documents/bios/ via Files app
    Console.WriteLn("@@BIOS_DIR@@ path=\"%s\"", biosDir.c_str());
    
    if (envBios) {
        bool exists = FileSystem::FileExists(envBios);
        Console.WriteLn("@@BIOS_ENV@@ exists=%d", exists ? 1 : 0);
        
        if (exists) {
            // Copy to EmuFolders::Bios to ensure sandbox compliance
            struct stat st = {0};
            if (stat(biosDir.c_str(), &st) == -1) mkdir(biosDir.c_str(), 0755);
            
            std::string fileName(Path::GetFileName(envBios));
            std::string destPath = Path::Combine(biosDir, fileName);
            
            // Only copy if source != dest
            if (std::string(envBios) != destPath) {
                FILE *src = fopen(envBios, "rb");
                FILE *dst = fopen(destPath.c_str(), "wb");
                if (src && dst) {
                     char buffer[4096];
                     size_t bytes;
                     while ((bytes = fread(buffer, 1, 4096, src)) > 0) fwrite(buffer, 1, bytes, dst);
                     fclose(src); fclose(dst);
                     Console.WriteLn("Copied env-var BIOS to: %s", destPath.c_str());
                } else {
                     Console.Error("Failed to copy env-var BIOS. src=%p dst=%p", src, dst);
                     if (src) fclose(src);
                     if (dst) fclose(dst);
                }
            }
            
            EmuConfig.BaseFilenames.Bios = fileName;
            if (s_settings_interface) {
                s_settings_interface->SetStringValue("Filenames", "BIOS", EmuConfig.BaseFilenames.Bios.c_str());
                s_settings_interface->Save();
            }
            Console.WriteLn("@@BIOS_PICK@@ result=\"%s\" source=env", EmuConfig.BaseFilenames.Bios.c_str());
            return;
        }
    } else {
        Console.WriteLn("@@BIOS_ENV@@ exists=0");
    }

    // 1. Check existing config
    if (!EmuConfig.BaseFilenames.Bios.empty() && FileSystem::FileExists(Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios).c_str())) {
        Console.WriteLn("@@BIOS_PICK@@ result=\"%s\" source=config", EmuConfig.BaseFilenames.Bios.c_str());
        return;
    }

    // 1b. Adopt anything BIOS-shaped left in Documents/ root. Shared with the BIOS list's
    //     refresh (iPSX2Bridge availableBIOSes) so a file dropped in while the app is running
    //     does not have to wait for a relaunch.
    iPSX2::BiosIntake::AdoptFromDataRoot(dataRoot);

    // 2. Scan Documents/bios
    FileSystem::FindResultsArray results;
    int foundCount = 0;
    if (FileSystem::FindFiles(biosDir.c_str(), "*", FILESYSTEM_FIND_FILES, &results)) {
        for (const auto& fd : results) {
            foundCount++;
            if (fd.Size >= 1024*1024 && (fd.FileName.find(".bin") != std::string::npos || fd.FileName.find(".BIN") != std::string::npos)) {
                // Found a candidate
                std::string currentName = std::string(Path::GetFileName(fd.FileName));
                EmuConfig.BaseFilenames.Bios = currentName;
                Console.WriteLn("Auto-detected BIOS (name only): %s", EmuConfig.BaseFilenames.Bios.c_str());
                if (s_settings_interface) {
                    s_settings_interface->SetStringValue("Filenames", "BIOS", EmuConfig.BaseFilenames.Bios.c_str());
                    s_settings_interface->Save();
                }
                Console.WriteLn("@@BIOS_PICK@@ result=\"%s\" source=scan", EmuConfig.BaseFilenames.Bios.c_str());
                return;
            }
        }
    }
    Console.WriteLn("@@BIOS_SCAN@@ found=%d", foundCount);
    Console.WriteLn("@@BIOS_PICK@@ result=\"(none)\" source=none");

    // 3. Check Bundle Resources (Fallback)
    NSString *resourcePath = [[NSBundle mainBundle] resourcePath];
    std::string bundleDir = [resourcePath UTF8String];
    FileSystem::FindResultsArray bundleResults;

    // [iPSX2] Support "BiosFiles" folder reference
    std::string bfDir = bundleDir + "/BiosFiles";
    if (FileSystem::FindFiles(bfDir.c_str(), "*", FILESYSTEM_FIND_FILES, &bundleResults)) {
        for (const auto& fd : bundleResults) {
             if (fd.Size >= 1024*1024 && (fd.FileName.find(".bin") != std::string::npos || fd.FileName.find(".BIN") != std::string::npos)) {
                 Console.WriteLn("Found BIOS in BiosFiles: %s", fd.FileName.c_str());
                 struct stat st = {0};
                 if (stat(biosDir.c_str(), &st) == -1) mkdir(biosDir.c_str(), 0755);

                 std::string src = bfDir + "/" + fd.FileName;
                 std::string dst = biosDir + "/" + fd.FileName;
                 FILE *s=fopen(src.c_str(),"rb"), *d=fopen(dst.c_str(),"wb");
                 if(s && d) { char b[4096]; size_t n; while((n=fread(b,1,4096,s))>0) fwrite(b,1,n,d); }
                 if(s) fclose(s); if(d) fclose(d);
                 EmuConfig.BaseFilenames.Bios = fd.FileName;
                 return;
             }
        }
    }
    if (FileSystem::FindFiles(bundleDir.c_str(), "*", FILESYSTEM_FIND_FILES, &bundleResults)) {
        for (const auto& fd : bundleResults) {
             if (fd.Size >= 1024*1024 && (fd.FileName.find(".bin") != std::string::npos || fd.FileName.find(".BIN") != std::string::npos)) {
                 Console.WriteLn("Found BIOS in Bundle: %s. Copying...", fd.FileName.c_str());
                 std::string srcPath = bundleDir + "/" + fd.FileName;
                 std::string destPath = biosDir + "/" + fd.FileName;
                 
                 struct stat st = {0};
                 if (stat(biosDir.c_str(), &st) == -1) mkdir(biosDir.c_str(), 0755);

                 FILE *src = fopen(srcPath.c_str(), "rb");
                 FILE *dst = fopen(destPath.c_str(), "wb");
                 if (src && dst) {
                     char buffer[4096];
                     size_t bytes;
                     while ((bytes = fread(buffer, 1, 4096, src)) > 0) fwrite(buffer, 1, bytes, dst);
                     fclose(src); fclose(dst);
                     EmuConfig.BaseFilenames.Bios = fd.FileName;
                     Console.WriteLn("Copy and set successful.");
                     return;
                 }
                 if(src) fclose(src);
                 if(dst) fclose(dst);
             }
        }
    }
    
    Console.Warning("No BIOS found automatically.");
    EmuConfig.BaseFilenames.Bios.clear();
}

- (void)showBiosPicker {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    UIDocumentPickerViewController *documentPicker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[@"public.data"] inMode:UIDocumentPickerModeImport];
#pragma clang diagnostic pop
    documentPicker.delegate = self;
    documentPicker.allowsMultipleSelection = NO;
    [self.window.rootViewController presentViewController:documentPicker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (urls.count == 0) return;
    
    NSURL *url = urls.firstObject;
    Console.WriteLn("User picked file: %s", [[url path] UTF8String]);
    
    // Copy to Documents/bios
    std::string biosDir = EmuFolders::DataRoot + "/bios";
    struct stat st = {0};
    if (stat(biosDir.c_str(), &st) == -1) mkdir(biosDir.c_str(), 0755);
    
    NSString *destPath = [NSString stringWithFormat:@"%s/%@", biosDir.c_str(), [url lastPathComponent]];
    NSError *error = nil;
    
    // Remove if exists
    [[NSFileManager defaultManager] removeItemAtPath:destPath error:nil];
    
    if ([[NSFileManager defaultManager] copyItemAtURL:url toURL:[NSURL fileURLWithPath:destPath] error:&error]) {
        Console.WriteLn("Imported BIOS to: %s", [destPath UTF8String]);
        
        std::string fileName = [[destPath lastPathComponent] UTF8String];
        EmuConfig.BaseFilenames.Bios = fileName;
        
        // Hide button and start VM
        dispatch_async(dispatch_get_main_queue(), ^{
            self.startBiosButton.hidden = YES;
        });
        
#if TARGET_OS_SIMULATOR
        [self startVMThread];
#else
        [self checkJITAndStartVM];
#endif

    } else {
        Console.Error("Failed to import BIOS: %s", [[error localizedDescription] UTF8String]);
        Host::ReportErrorAsync("Import Failed", [[error localizedDescription] UTF8String]);
    }
}

// JIT availability check for real device — fallback to Interpreter if JIT unavailable
// CS_DEBUGGED check only (DolphiniOS approach) — no blocking, runs on main thread
- (void)checkJITAndStartVM {
#if !TARGET_OS_SIMULATOR
    if (DarwinMisc::IsJITAvailable()) {
        Console.WriteLn("@@JIT_GATE@@ JIT available — starting VM in JIT mode");
        [self startVMThread];
        return;
    }

    Console.Warning("@@JIT_GATE@@ JIT NOT available — falling back to Interpreter mode");
    DarwinMisc::iPSX2_FORCE_EE_INTERP = 1;
    Console.WriteLn("@@JIT_GATE@@ iPSX2_FORCE_EE_INTERP forced to 1");
    [self startVMThread];
#else
    [self startVMThread];
#endif
}

- (void)startVMThread {
    {
        std::lock_guard<std::mutex> lk(s_vmMutex);
        if (s_vmThreadActive.load()) {
            Console.WriteLn("[VM] startVMThread: VM already active, ignoring");
            return;
        }

        // Signal the persistent thread to boot
        s_requestVMBoot.store(true);
        s_requestVMStop.store(false);

        if (s_vmThreadCreated) {
            Console.WriteLn("[VM] startVMThread: signaling existing VM thread");
            s_vmCV.notify_one();
            return;
        }

        // First call: create the persistent thread
        s_vmThreadCreated = true;
    }

    Console.WriteLn("[VM] Creating persistent VM thread...");

    std::thread vmThread([]() {
        // === ONE-TIME INIT (runs once per app lifetime) ===
        Console.WriteLn("[VM] VM Thread: CPUThreadInitialize (once)...");
        if (!VMManager::Internal::CPUThreadInitialize()) {
            Console.Error("VM Thread: CPUThreadInitialize failed.");
            std::lock_guard<std::mutex> lk(s_vmMutex);
            s_vmThreadCreated = false;
            return;
        }

        // Set ImGui font path (once)
        std::string fontPath = Path::Combine(EmuFolders::Resources, "fonts/Roboto-Regular.ttf");
        ImGuiManager::SetFontPathAndRange(std::move(fontPath), {});

        // === PERSISTENT BOOT LOOP ===
        bool auto_boot_first = (getenv("iPSX2_AUTO_BOOT") && atoi(getenv("iPSX2_AUTO_BOOT")) == 1)
                            || (getenv("iPSX2_BOOT_ELF") != nullptr);
        while (true) {
            // Wait for boot signal (or auto-boot on first iteration)
            {
                std::unique_lock<std::mutex> lk(s_vmMutex);
                if (auto_boot_first) {
                    Console.WriteLn("[AutoBoot] @@AUTO_BOOT@@ skipping UI wait, auto-boot enabled");
                    auto_boot_first = false;
                } else {
                    Console.WriteLn("[VM] VM Thread: waiting for boot request...");
                    s_vmCV.wait(lk, [] { return s_requestVMBoot.load(); });
                }
                s_requestVMBoot.store(false);
            }

            Console.WriteLn("[VM] VM Thread: boot signal received, preparing boot params...");
            s_vmThreadActive.store(true);

            // --- Build boot parameters from INI ---
            VMBootParameters boot_params;
            boot_params.fast_boot = false;
            {
                std::string isoDir = EmuFolders::DataRoot + "/iso";
                std::string defaultISO = "";
                std::string isoFilename = s_settings_interface->GetStringValue("GameISO", "BootISO", defaultISO.c_str());
                bool fastBoot = s_settings_interface->GetBoolValue("GameISO", "FastBoot", false);
                s_settings_interface->SetStringValue("GameISO", "BootISO", isoFilename.c_str());
                s_settings_interface->SetBoolValue("GameISO", "FastBoot", fastBoot);
                s_settings_interface->Save();
                // [F8] A game-list entry is an absolute path, not a bare filename inside
                // Documents/iso, so honour one when given. Bare names still resolve as before.
                std::string isoPath = Path::IsAbsolute(isoFilename) ? isoFilename : (isoDir + "/" + isoFilename);
                // Fallback: check Documents/ root if not found in iso/
                if (!isoFilename.empty() && !FileSystem::FileExists(isoPath.c_str())) {
                    std::string rootPath = EmuFolders::DataRoot + "/" + isoFilename;
                    if (FileSystem::FileExists(rootPath.c_str())) {
                        isoPath = rootPath;
                        Console.WriteLn("ISO found in Documents/ root: %s", isoPath.c_str());
                    }
                }
                if (!isoFilename.empty() && FileSystem::FileExists(isoPath.c_str())) {
                    std::string suffix = isoFilename.size() >= 4 ? isoFilename.substr(isoFilename.size() - 4) : "";
                    std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
                    bool isElf = (suffix == ".elf");
                    if (isElf) {
                        boot_params.elf_override = isoPath;
                        boot_params.source_type = CDVD_SourceType::NoDisc;
                        boot_params.fast_boot = true;
                        Console.WriteLn("@@ISO_BOOT@@ path=%s fast_boot=1 mode=ELF (INI: %s)", isoPath.c_str(), isoFilename.c_str());
                    } else {
                        boot_params.filename = isoPath;
                        boot_params.source_type = CDVD_SourceType::Iso;
                        boot_params.fast_boot = fastBoot;
                        Console.WriteLn("@@ISO_BOOT@@ path=%s fast_boot=%d (INI: %s)", isoPath.c_str(), fastBoot ? 1 : 0, isoFilename.c_str());
                    }
                } else {
                    Console.WriteLn("@@ISO_BOOT@@ no ISO='%s', falling back to BIOS only", isoFilename.c_str());
                }
            }

            if (getenv("iPSX2_AUTO_BOOT_BIOS")) {
                Console.WriteLn("@@AUTO_BOOT_BIOS@@ enabled=1 action=triggered");
                boot_params.fast_boot = false;
            }
            // ps2autotests: boot ELF directly via env var
            if (const char* testElf = getenv("iPSX2_BOOT_ELF")) {
                boot_params.elf_override = testElf;
                boot_params.source_type = CDVD_SourceType::NoDisc;
                boot_params.fast_boot = true;
                Console.WriteLn("@@BOOT_ELF@@ elf=%s", testElf);
            }

            // BIOS sanity check
            if (EmuConfig.BaseFilenames.Bios.empty() || !FileSystem::FileExists(Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios).c_str())) {
                Console.Error("CRITICAL: BIOS verification failed inside VM thread.");
                Host::ReportErrorAsync("BIOS Error", "Validation failed.");
                s_vmThreadActive.store(false);
                dispatch_async(dispatch_get_main_queue(), ^{
                    [[NSNotificationCenter defaultCenter] postNotificationName:@"iPSX2VMDidShutdown" object:nil];
                });
                continue; // back to wait loop
            }

            // --- Initialize & Execute VM ---
            if (VMManager::Initialize(boot_params)) {
                Console.WriteLn("[VM] VM initialized successfully");
                VMManager::SetState(VMState::Running);

                while (true) {
                    if (s_requestVMStop.load()) {
                        Console.WriteLn("[VM] VM Thread: stop requested from UI.");
                        break;
                    }
                    VMState state = VMManager::GetState();
                    if (state == VMState::Stopping || state == VMState::Shutdown) {
                        Console.WriteLn("[VM] VM Thread: shutdown signal received.");
                        break;
                    } else if (state == VMState::Running) {
                        VMManager::Execute();
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }

                Console.WriteLn("[VM] VM Thread: shutting down VM...");
                VMManager::Shutdown(false);
            } else {
                Console.Error("VM Thread: VMManager::Initialize failed!");
                Host::ReportErrorAsync("Startup Error", "VM Initialization Failed.");
            }

            // --- Post-shutdown: reset state, notify UI ---
            s_vmThreadActive.store(false);
            s_requestVMStop.store(false);
            Console.WriteLn("[VM] VM Thread: shutdown complete, posting notification");
            dispatch_async(dispatch_get_main_queue(), ^{
                [[NSNotificationCenter defaultCenter] postNotificationName:@"iPSX2VMDidShutdown" object:nil];
            });
        } // end while(true) boot loop

        // Note: CPUThreadShutdown() is never reached because the thread persists.
        // It would only be needed if we added an app-termination signal.
    });
    vmThread.detach();
}

- (void)sceneDidDisconnect:(UIScene *)scene {
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
}

- (void)sceneWillResignActive:(UIScene *)scene {
// NVM save when app loses focus
    extern void cdvdSaveNVRAM();
    cdvdSaveNVRAM();
}

- (void)sceneWillEnterForeground:(UIScene *)scene {
}

- (void)sceneDidEnterBackground:(UIScene *)scene {
// Save NVM + memory cards when app goes to background.
    // Without this, BIOS settings (language/date) are lost on every restart
    // because cdvdSaveNVRAM() is only called at VM shutdown, which never
    // happens when iOS terminates the app via SIGTERM.
    extern void cdvdSaveNVRAM();
    cdvdSaveNVRAM();
    Console.WriteLn("[NVM] NVM saved on sceneDidEnterBackground");
}

@end


@interface PCSX2AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation PCSX2AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    // Override point for customization after application launch.
    
    // --- Setup PCSX2 Environment (Moved from SceneDelegate) ---
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *documentsDirectory = [paths objectAtIndex:0];
    NSString *resourcePath = [[NSBundle mainBundle] resourcePath];
    
    std::string dataRoot = [documentsDirectory UTF8String];
    EmuFolders::DataRoot = dataRoot;
    EmuFolders::AppRoot = [resourcePath UTF8String];
    EmuFolders::Resources = [resourcePath UTF8String];
    // [F6] See the macOS branch: an unassigned Settings resolves derived paths against the
    // filesystem root, which is where "/debuggerlayouts" in the startup log came from.
    EmuFolders::Settings = dataRoot + "/inis";
    mkdir(EmuFolders::Settings.c_str(), 0755);
    EmuFolders::Bios = dataRoot + "/bios";
    // ... [Init other folders if needed, but DataRoot is key] ...
    EmuFolders::Logs = dataRoot + "/logs";

    // --- Unified Logging Redirection ---
    // Force stderr and stdout to pcsx2_log.txt
    std::string logPath = dataRoot + "/pcsx2_log.txt";
    
    // Redirect stderr to file
    if (freopen(logPath.c_str(), "w", stderr) == NULL) { // "w" clears old logs
        printf("Reopen stderr failed\n");
    }
    
    // Redirect stdout to stderr
    if (dup2(fileno(stderr), fileno(stdout)) == -1) {
        fprintf(stderr, "Redirection of stdout failed\n");
    }
    
    // Disable buffering
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    
    // [iPSX2] Register File Descriptor for Signal Handler
    // We use the raw file descriptor of stderr (which is now our log file)
    DarwinMisc::SetCrashLogFD(fileno(stderr));
    
    // Log Proof Tag
    fprintf(stderr, "@@LOG_SINK@@ unified=1 path=%s pid=%d\n", logPath.c_str(), getpid());
    NSString* bundleID = [[NSBundle mainBundle] bundleIdentifier];
    fprintf(stderr, "@@BUNDLE_ID@@ %s\n", bundleID ? [bundleID UTF8String] : "(null)");
#ifndef IPSX2_BUILD_TIME
#define IPSX2_BUILD_TIME "unknown"
#endif
#ifndef IPSX2_VERSION_STR
#define IPSX2_VERSION_STR "dev"
#endif
    fprintf(stderr, "@@BUILD_ID@@ v%s %s %s\n", IPSX2_VERSION_STR, iPSX2_GIT_HASH, IPSX2_BUILD_TIME);
    
    // [iPSX2] Unification Validation
    // @@BIOS_GATE@@ build_id=2026-01-14_13-30-00 bundle=(from_nsbundle)
    NSString* bID = [[NSBundle mainBundle] bundleIdentifier];
    const char* cBundle = bID ? [bID UTF8String] : "(null)";
    fprintf(stderr, "@@BIOS_GATE@@ build_id=2026-01-17_PROBE bundle=%s\n", cBundle);
    fprintf(stderr, "@@LOG_UNIFIED@@ pcsx2_log.txt includes emulog output; emulog.txt disabled=1\n");
    
// DYLD Map — debug builds only
#if DEBUG
    {
        fprintf(stderr, "@@CFG@@ iPSX2_CRASH_DIAG=1 (DYLD Dump Enabled)\n");
        uint32_t count = _dyld_image_count();
        for (uint32_t i = 0; i < count; i++) {
            const char* name = _dyld_get_image_name(i);
            intptr_t slide = _dyld_get_image_vmaddr_slide(i);
            const struct mach_header* hdr = _dyld_get_image_header(i);
            fprintf(stderr, "@@DYLD_MAP@@ idx=%u addr=%p slide=%p path=%s\n", i, hdr, (void*)slide, name);
        }
    }
#endif
    fflush(stderr);

    // Enable PCSX2 Console Output only (std::cout/cerr will now go to file)
    Log::SetConsoleOutputLevel(LOGLEVEL::LOGLEVEL_INFO);
    
    Console.WriteLn("PCSX2 iOS: AppDelegate didFinishLaunching.");
    
    return YES;
}

- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession options:(UISceneConnectionOptions *)options {
    // Called when a new scene session is being created.
    // Use this method to select a configuration to create the new scene with.
    UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"Default Configuration" sessionRole:connectingSceneSession.role];
    config.delegateClass = [PCSX2SceneDelegate class];
    return config;
}

- (void)application:(UIApplication *)application didDiscardSceneSessions:(NSSet<UISceneSession *> *)sceneSessions {
}

@end


//
// -- Main Entry Point --
//

// Signal Handler removed to allow PCSX2's internal handlers to work
#import <UIKit/UIKit.h>
#include <mach-o/dyld.h>
#include <cstdio>

int main(int argc, char * argv[]) {
    // InstallSignalHandler(); // Removed
    
    @autoreleasepool {
        // SDL_MAIN_HANDLED is set, so we use standard main()
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([PCSX2AppDelegate class]));
    }
}
#endif // iPSX2_MACOS
