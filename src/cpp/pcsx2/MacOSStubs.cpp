// [P63] Stubs for macOS native build — symbols not available without full UI
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE

#include "Host.h"
#include "VMManager.h"
#include "Input/InputManager.h"
#include "DEV9/pcap_io.h"
#include "Achievements.h"
#include "common/HTTPDownloader.h"
#include "common/ProgressCallback.h"
#include "common/FileSystem.h"
#include "Host/AudioStream.h"
#include "INISettingsInterface.h"
#include "common/Console.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "../HostCpuQueue.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <string>

// --- PNG (already in common/PNGStub.cpp) ---

// --- FileSystem ---
int FileSystem::OpenFDFileContent(const char* path) { return -1; }

// --- x86 emitter symbols (never called on ARM64, but referenced by JIT stubs) ---
// Non-const → external linkage; Itanium ABI mangles by name only, type doesn't matter
namespace x86Emitter {
    char xmm0[16]={}, xmm1[16]={}, xmm2[16]={}, xmm3[16]={}, xmm4[16]={}, xmm5[16]={};
    char xmm6[16]={}, xmm7[16]={}, xmm8[16]={}, xmm9[16]={}, xmm10[16]={};
    char xmm11[16]={}, xmm12[16]={}, xmm13[16]={}, xmm14[16]={}, xmm15[16]={};
}

// --- g_xmmtypes (thread-local, referenced by iCore.o) ---
enum { _XMMT_INT = 0 };
thread_local int g_xmmtypes[16] = {_XMMT_INT};

// --- Audio: Oboe is Android-only ---
std::unique_ptr<AudioStream> AudioStream::CreateOboeAudioStream(u32 sr, const AudioStreamParameters& p, bool s, Error* e) { return nullptr; }

// --- Network: PCAP not available ---
PCAPAdapter::PCAPAdapter() : NetAdapter() {}

// --- HTTP: curl not linked ---
// [F5] HTTPDownloader::Create now lives in common/HTTPDownloaderNSURL.mm.

// --- Input: keyboard mapping ---
std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 c) { return std::nullopt; }
const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 c) { return nullptr; }
std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(std::string_view s) { return std::nullopt; }

// --- Host functions (declared in Host.h) ---
bool Host::InNoGUIMode() { return true; }
// [F1] Host::RunOnCPUThread now lives in HostCpuQueue.cpp (shared by both Apple targets).
void Host::RequestVMShutdown(bool a, bool b, bool c) {}
bool Host::RequestResetSettings(bool a, bool b, bool c, bool d, bool e) { return false; }
// [F8] Host::RefreshGameListAsync / CancelGameListRefresh now live in HostGameList.mm.
// [F2] Host::CommitBaseSettingChanges now lives in HostSettings.cpp (shared by both Apple targets).
bool Host::ConfirmMessage(std::string_view t, std::string_view m) { return true; }
void Host::ReportErrorAsync(std::string_view t, std::string_view m) { fprintf(stderr, "[Error] %.*s: %.*s\n", (int)t.size(), t.data(), (int)m.size(), m.data()); }
void Host::ReportInfoAsync(std::string_view t, std::string_view m) {}
void Host::OpenURL(std::string_view u) {}
bool Host::CopyTextToClipboard(std::string_view t) { return false; }
std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback() { return nullptr; }
std::string Host::TranslatePluralToString(const char* ctx, const char* msg, const char* mpl, int n) { return msg; }
s32 Host::Internal::GetTranslatedStringImpl(std::string_view ctx, std::string_view msg, char* buf, size_t sz) {
    s32 len = std::min((s32)msg.size(), (s32)(sz - 1));
    std::memcpy(buf, msg.data(), len);
    buf[len] = 0;
    return len;
}

// --- [P115] GS 出力の回帰ゲート (既定 OFF、摂動ゼロ) ---------------------------
// なぜ要るか: Scripts/t1_verify.sh の GIF パケット CRC は **GS に渡る手前**を見ている。
// GS / Metal を触る変更はそのゲートを素通りするので、GS の出力そのものを比較する手段が
// 別に必要になる。savestate をロードして固定 vsync まで進め、GS の表示フレームを
// 取り出してハッシュする = 同じ入力に対する GS 出力の同一性を機械判定できる。
//
//   iPSX2_GS_SHOT_AT=<N[,M,...]>  この vsync 数で撮る (PumpMessagesOnCPUThread の呼び出し回数)
//   iPSX2_GS_SHOT_DIR=<dir>       指定時は BMP も書く (libpng は macOS では未リンク)
//   iPSX2_GS_SHOT_QUIT=1          最後の 1 枚を撮ったら VM を止める
//
// 出力 = `@@GSSHOT@@ vsync=N w=.. h=.. rgb=0x.. rgba=0x.. luma=.. nonblack=..%`
//   rgb  … R,G,B だけの FNV-1a 64 (α の初期化差に左右されない主指標)
//   rgba … α も含めた版 (rgb と乖離したら α 側だけが動いた証拠)
//   luma / nonblack … 「真っ黒 / 暗すぎ」を数値で見るための補助 (P114 の DIM/BLACK 判定)
//
// 除去条件: 上流 GS 合流がクローズし、GS を触る作業が終わったら削除してよい。
// 判定に使っている間は残す。既定 OFF なので通常経路には一切効かない。
#include "MTGS.h"
#include <cstdio>
#include <cinttypes>
#include <vector>
namespace {

    bool P115_WriteBMP24(const char* path, u32 w, u32 h, const std::vector<u32>& px)
    {
        // 24bpp BMP。libpng が macOS ビルドでは未リンク (common/PNGStub.cpp) なので、
        // ライブラリ不要でプレビューできる形式にする。行は下から上、4 byte 境界で padding。
        FILE* fp = std::fopen(path, "wb");
        if (!fp)
            return false;
        const u32 row = ((w * 3u) + 3u) & ~3u;
        const u32 img = row * h;
        const u32 off = 54u;
        u8 hdr[54] = {};
        auto put32 = [&](u32 at, u32 v) { hdr[at] = u8(v); hdr[at + 1] = u8(v >> 8); hdr[at + 2] = u8(v >> 16); hdr[at + 3] = u8(v >> 24); };
        hdr[0] = 'B'; hdr[1] = 'M';
        put32(2, off + img);
        put32(10, off);
        put32(14, 40);
        put32(18, w);
        put32(22, h);
        hdr[26] = 1; hdr[28] = 24;
        put32(34, img);
        std::fwrite(hdr, 1, sizeof(hdr), fp);
        std::vector<u8> line(row, 0);
        for (u32 y = 0; y < h; y++)
        {
            const u32* src = px.data() + (size_t)(h - 1 - y) * w;
            for (u32 x = 0; x < w; x++)
            {
                const u32 c = src[x];
                line[x * 3 + 0] = u8((c >> 16) & 0xFF); // B <- BMP は BGR 順
                line[x * 3 + 1] = u8((c >> 8) & 0xFF);
                line[x * 3 + 2] = u8(c & 0xFF);
            }
            std::fwrite(line.data(), 1, row, fp);
        }
        std::fclose(fp);
        return true;
    }

    // vsync ごとに呼ばれる。撮る予定が無ければ即 return する (env 未設定なら 1 回目で確定)。
    void P115_MaybeShoot(long vsync)
    {
        static const std::vector<long> s_at = []() {
            std::vector<long> v;
            if (const char* e = std::getenv("iPSX2_GS_SHOT_AT"))
            {
                const char* p = e;
                while (*p)
                {
                    char* end = nullptr;
                    const long n = std::strtol(p, &end, 10);
                    if (end == p)
                        break;
                    v.push_back(n);
                    p = (*end == ',') ? end + 1 : end;
                }
            }
            return v;
        }();
        if (s_at.empty())
            return;

        bool hit = false, last = true;
        for (const long n : s_at)
        {
            if (n == vsync)
                hit = true;
            if (n > vsync)
                last = false;
        }
        if (!hit)
            return;

        u32 w = 0, h = 0;
        std::vector<u32> px;
        if (!MTGS::SaveMemorySnapshot(640, 480, true, false, &w, &h, &px) || px.empty())
        {
            Console.WriteLn("@@GSSHOT@@ vsync=%ld FAILED (snapshot returned nothing)", vsync);
            return;
        }

        // FNV-1a 64。RGB だけの版を主指標にする (α の初期化差で判定が揺れないように)。
        u64 hrgb = 1469598103934665603ull, hrgba = 1469598103934665603ull;
        u64 luma_sum = 0, nonblack = 0;
        for (const u32 c : px)
        {
            const u32 rgb = c & 0x00FFFFFFu;
            for (int b = 0; b < 3; b++)
            {
                hrgb ^= (rgb >> (b * 8)) & 0xFFu;
                hrgb *= 1099511628211ull;
            }
            for (int b = 0; b < 4; b++)
            {
                hrgba ^= (c >> (b * 8)) & 0xFFu;
                hrgba *= 1099511628211ull;
            }
            const u32 r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
            const u32 y = (r * 54 + g * 183 + bl * 19) >> 8; // 近似輝度
            luma_sum += y;
            if (y > 8)
                nonblack++;
        }
        const double luma = double(luma_sum) / double(px.size());
        const double nb = 100.0 * double(nonblack) / double(px.size());
        Console.WriteLn("@@GSSHOT@@ vsync=%ld w=%u h=%u rgb=0x%016" PRIx64 " rgba=0x%016" PRIx64 " luma=%.3f nonblack=%.2f%%",
            vsync, w, h, hrgb, hrgba, luma, nb);

        if (const char* dir = std::getenv("iPSX2_GS_SHOT_DIR"))
        {
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/gsshot_%05ld.bmp", dir, vsync);
            Console.WriteLn("@@GSSHOT@@ wrote %s (%s)", path,
                P115_WriteBMP24(path, w, h, px) ? "ok" : "FAILED");
        }

        if (last)
        {
            if (const char* q = std::getenv("iPSX2_GS_SHOT_QUIT"); q && q[0] == '1')
            {
                Console.WriteLn("@@GSSHOT@@ done (iPSX2_GS_SHOT_QUIT=1) — requesting shutdown");
                VMManager::SetState(VMState::Stopping);
            }
        }
    }

} // namespace

// --- Host functions (declared in VMManager.h / other headers) ---
namespace Host {
    void BeginPresentFrame() {}
    // AcquireRenderWindow is in ios_main.mm (macOS section) with correct return type
    void ReleaseRenderWindow() {}
    void BeginTextInput() {}
    void EndTextInput() {}
    bool IsFullscreen() { return false; }
    void SetFullscreen(bool f) {}
    void SetMouseMode(bool r, bool h) {}
    void OnVMStarting() {}
    void OnVMStarted() {}
    // [F1] Drop anything still queued for a CPU thread that no longer exists.
    void OnVMDestroyed() { iPSX2::ResetCPUThreadQueue(); }
    void OnVMPaused() {}
    void OnVMResumed() {}
    void OnGameChanged(const std::string& d, const std::string& e, const std::string& t, const std::string& s, u32 c, u32 r) {}
    void OnPerformanceMetricsUpdated() {}
    void OnAchievementsHardcoreModeChanged(bool e) {}
    void OnAchievementsLoginRequested(Achievements::LoginRequestReason r) {}
    void OnAchievementsLoginSuccess(const char* u, u32 p, u32 sc, u32 us) {}
    void OnAchievementsRefreshed() {}
    void OnCoverDownloaderOpenRequested() {}
    void OnCreateMemoryCardOpenRequested() {}
    void OnInputDeviceConnected(std::string_view i, std::string_view d) {}
    void OnInputDeviceDisconnected(InputBindingKey k, std::string_view i) {}
    // [P101-9 dev] macOS keyboard control + save/load-state. Runs on the CPU thread per vsync
    // (VSyncOnCPUThread -> PumpMessagesOnCPUThread) = the safe point for SaveState. Lets the user
    // navigate the window to ANY scene and F5=save / F7=load, so HLE capture/coverage can target
    // arbitrary scenes (auto-button can't traverse the map). ON BY DEFAULT (keyboard is the primary
    // dev input now); kill-switch iPSX2_KBD=0 disables it (e.g. GTA VC auto-button debug runs, which
    // are themselves opt-in via iPSX2_AUTO_*_VS). The pad is driven ONLY while a mapped key is held
    // (+1 release frame), so when the keyboard is idle other input sources / automated capture +
    // benchmark + determinism runs are untouched (byte-identical to the old default-off). state path = iPSX2_STATE_PATH or
    // ~/Documents/iPSX2/savestates/ipsx2_quick.p2s. Load-on-boot: iPSX2_LOAD_STATE=<file> (BuildBootParams).
    void PumpMessagesOnCPUThread() {
        // [F1] Must stay first: iPSX2_KBD=0 and a null keyboard state both return early below.
        iPSX2::DrainCPUThreadTasks();

        auto statePath0 = []() -> std::string {
            if (const char* e = std::getenv("iPSX2_STATE_PATH")) return e;
            const char* home = std::getenv("HOME");
            return std::string(home ? home : "/tmp") + "/Documents/iPSX2/savestates/ipsx2_quick.p2s";
        };
        // [P101-9 dev] deterministic auto-save at vsync N (env iPSX2_SAVE_STATE_AT) — saves once without a
        // keypress, for automated capture + roundtrip self-test. Runs on CPU thread = safe.
        static const long s_save_at = []{ const char* e=std::getenv("iPSX2_SAVE_STATE_AT"); return e?atol(e):-1; }();
        if (s_save_at >= 0) {
            static long s_vsync = 0; static bool s_done = false;
            if (!s_done && ++s_vsync >= s_save_at) { s_done = true; const std::string p = statePath0();
                Console.WriteLn(VMManager::SaveState(p.c_str(), false, false) ? "[STATE] auto-saved -> %s" : "[STATE] auto-save FAILED -> %s", p.c_str()); }
        }
        // [P115] GS 出力の回帰ゲート。iPSX2_GS_SHOT_AT 未設定なら即 return する。
        // iPSX2_KBD=0 でも撮れるよう、キーボード処理の前に置く。
        { static long s_gs_vsync = 0; P115_MaybeShoot(++s_gs_vsync); }
        static const bool s_kbd = []{ const char* e = std::getenv("iPSX2_KBD"); return !(e && e[0] == '0'); }();
        if (!s_kbd) return;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (!keys) return;
        using I = PadDualshock2::Inputs;
        static const struct { SDL_Scancode sc; u32 idx; } btn[] = {
            {SDL_SCANCODE_Z, I::PAD_CIRCLE}, {SDL_SCANCODE_X, I::PAD_CROSS},
            {SDL_SCANCODE_A, I::PAD_SQUARE}, {SDL_SCANCODE_S, I::PAD_TRIANGLE},
            {SDL_SCANCODE_Q, I::PAD_L1},     {SDL_SCANCODE_W, I::PAD_R1},
            {SDL_SCANCODE_1, I::PAD_L2},     {SDL_SCANCODE_2, I::PAD_R2},
            {SDL_SCANCODE_RETURN, I::PAD_START}, {SDL_SCANCODE_SPACE, I::PAD_SELECT},
            // dpad (TGFH), left stick (arrows), right stick (IJKL)
            {SDL_SCANCODE_T, I::PAD_UP},    {SDL_SCANCODE_G, I::PAD_DOWN},
            {SDL_SCANCODE_F, I::PAD_LEFT},  {SDL_SCANCODE_H, I::PAD_RIGHT},
            {SDL_SCANCODE_UP, I::PAD_L_UP},     {SDL_SCANCODE_DOWN, I::PAD_L_DOWN},
            {SDL_SCANCODE_LEFT, I::PAD_L_LEFT}, {SDL_SCANCODE_RIGHT, I::PAD_L_RIGHT},
            {SDL_SCANCODE_I, I::PAD_R_UP},    {SDL_SCANCODE_K, I::PAD_R_DOWN},
            {SDL_SCANCODE_J, I::PAD_R_LEFT},  {SDL_SCANCODE_L, I::PAD_R_RIGHT},
        };
        // Drive the pad ONLY while a mapped key is held, plus the single frame after the last release
        // (to push the neutral state). When idle, leave the pad untouched — so this default-on handler
        // never clobbers other input sources and adds zero behavioral change to automated runs.
        bool anyDown = false;
        for (const auto& b : btn) if (keys[b.sc]) { anyDown = true; break; }
        static bool s_drove = false;
        if (anyDown || s_drove) {
            if (PadBase* pad = Pad::GetPad(0, 0))
                for (const auto& b : btn) pad->Set(b.idx, keys[b.sc] ? 1.0f : 0.0f);
        }
        s_drove = anyDown;
        auto statePath = []() -> std::string {
            if (const char* e = std::getenv("iPSX2_STATE_PATH")) return e;
            const char* home = std::getenv("HOME");
            return std::string(home ? home : "/tmp") + "/Documents/iPSX2/savestates/ipsx2_quick.p2s";
        };
        static bool s_f5 = false, s_f7 = false;
        const bool f5 = keys[SDL_SCANCODE_F5], f7 = keys[SDL_SCANCODE_F7];
        if (f5 && !s_f5) { const std::string p = statePath();
            Console.WriteLn(VMManager::SaveState(p.c_str(), false, false) ? "[STATE] saved -> %s" : "[STATE] save FAILED -> %s", p.c_str()); }
        if (f7 && !s_f7) { const std::string p = statePath();
            Console.WriteLn(VMManager::LoadState(p.c_str()) ? "[STATE] loaded <- %s" : "[STATE] load FAILED <- %s", p.c_str()); }
        s_f5 = f5; s_f7 = f7;
    }
    void RequestExitApplication(bool a) {}
    void RequestExitBigPicture() {}
    void CheckForSettingsChanges(const Pcsx2Config& c) {}
    void LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock) {}
    bool ShouldPreferHostFileSelector() { return false; }
    void OpenHostFileSelectorAsync(std::string_view t, bool d, std::function<void(const std::string&)> cb, std::vector<std::string> f, std::string_view i) {}
    bool LocaleCircleConfirm() { return false; }
}

// iOS-specific globals (referenced by iPSX2Bridge.mm)
#include <map>
#include <string>
int g_touchPadState = 0;
bool s_captureMode = false;
int s_capturedButton = -1;
bool s_requestVMStop = false;
extern "C" void iPSX2_SetSDLFullscreen(bool) {}
std::map<std::string, int> s_buttonMap;

// PCAPAdapter vtable
PCAPAdapter::~PCAPAdapter() {}
bool PCAPAdapter::blocks() { return false; }
bool PCAPAdapter::isInitialised() { return false; }
bool PCAPAdapter::recv(NetPacket* p) { return false; }
bool PCAPAdapter::send(NetPacket* p) { return false; }
void PCAPAdapter::reloadSettings() {}

#endif // !TARGET_OS_IPHONE

