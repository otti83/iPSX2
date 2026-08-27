// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Console.h"
#include "common/Assertions.h"
#include "common/FileSystem.h"
#include "common/SmallString.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <mutex>
#include <vector>
#include <utility>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#else
#include <unistd.h>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#endif

using namespace std::string_view_literals;

// Dummy objects, need to get rid of them...
ConsoleLogWriter<LOGLEVEL_INFO> Console;
ConsoleLogWriter<LOGLEVEL_DEV> DevCon;

#ifdef _DEBUG
ConsoleLogWriter<LOGLEVEL_DEBUG> DbgConWriter;
#else
NullLogWriter DbgConWriter;
#endif

#define TIMESTAMP_FORMAT_STRING "[{:10.4f}] "
#define TIMESTAMP_PRINTF_STRING "[%10.4f] "

namespace Log
{
	static void WriteToConsole(LOGLEVEL level, ConsoleColors color, std::string_view message);
	static void WriteToDebug(LOGLEVEL level, ConsoleColors color, std::string_view message);
	static void WriteToFile(LOGLEVEL level, ConsoleColors color, std::string_view message);
#if defined(__ANDROID__)
	static void WriteToAndroidFile(LOGLEVEL level, ConsoleColors color, std::string_view message);
#endif

	static void UpdateMaxLevel();

	static void ExecuteCallbacks(LOGLEVEL level, ConsoleColors color, std::string_view message);

	static Common::Timer::Value s_start_timestamp = Common::Timer::GetCurrentValue();

	static LOGLEVEL s_max_level = LOGLEVEL_NONE;
	static LOGLEVEL s_console_level = LOGLEVEL_NONE;
	static LOGLEVEL s_debug_level = LOGLEVEL_NONE;
	static LOGLEVEL s_file_level = LOGLEVEL_NONE;
	static LOGLEVEL s_host_level = LOGLEVEL_NONE;
	static bool s_log_timestamps = true;

	static FileSystem::ManagedCFilePtr s_file_handle;
	static std::string s_file_path;
	static std::mutex s_file_mutex;

	static HostCallbackType s_host_callback;

#ifdef _WIN32
	static HANDLE s_hConsoleStdIn = NULL;
	static HANDLE s_hConsoleStdOut = NULL;
	static HANDLE s_hConsoleStdErr = NULL;
#endif
#if defined(__ANDROID__)
	static FileSystem::ManagedCFilePtr s_android_file_handle;
	static std::string s_android_file_path;
	static std::mutex s_android_file_mutex;
	static LOGLEVEL s_android_file_level = LOGLEVEL_NONE;
#endif
} // namespace Log

float Log::GetCurrentMessageTime()
{
	return static_cast<float>(Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_start_timestamp));
}

__ri void Log::WriteToConsole(LOGLEVEL level, ConsoleColors color, std::string_view message)
{
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_DEBUG, "NDK_LOG", "%s", std::string(message).c_str());
#else
	static constexpr std::string_view s_ansi_color_codes[ConsoleColors_Count] = {
		"\033[0m"sv, // default
		"\033[30m\033[1m"sv, // black
		"\033[32m"sv, // green
		"\033[31m"sv, // red
		"\033[34m"sv, // blue
		"\033[35m"sv, // magenta
		"\033[35m"sv, // orange (FIXME)
		"\033[37m"sv, // gray
		"\033[36m"sv, // cyan
		"\033[33m"sv, // yellow
		"\033[37m"sv, // white
		"\033[30m\033[1m"sv, // strong black
		"\033[31m\033[1m"sv, // strong red
		"\033[32m\033[1m"sv, // strong green
		"\033[34m\033[1m"sv, // strong blue
		"\033[35m\033[1m"sv, // strong magenta
		"\033[35m\033[1m"sv, // strong orange (FIXME)
		"\033[37m\033[1m"sv, // strong gray
		"\033[36m\033[1m"sv, // strong cyan
		"\033[33m\033[1m"sv, // strong yellow
		"\033[37m\033[1m"sv, // strong white
	};

	static constexpr size_t BUFFER_SIZE = 512;

	SmallStackString<BUFFER_SIZE> buffer;
	buffer.reserve(32 + static_cast<u32>(message.length()));
	buffer.append(s_ansi_color_codes[color]);

	if (s_log_timestamps)
		buffer.append_format(TIMESTAMP_FORMAT_STRING, Log::GetCurrentMessageTime());

	buffer.append(message);
	buffer.append('\n');

#ifdef _WIN32
	const HANDLE hOutput = (level <= LOGLEVEL_WARNING) ? s_hConsoleStdErr : s_hConsoleStdOut;

	// Convert to UTF-16 first so Unicode characters display correctly. NT is going to do it anyway...
	wchar_t wbuf[BUFFER_SIZE];
	wchar_t* wmessage_buf = wbuf;
	int wmessage_buflen = static_cast<int>(std::size(wbuf) - 1);
	if (buffer.length() >= std::size(wbuf))
	{
		wmessage_buflen = static_cast<int>(buffer.length());
		wmessage_buf = static_cast<wchar_t*>(std::malloc(static_cast<size_t>(wmessage_buflen) * sizeof(wchar_t)));
		if (!wmessage_buf)
			return;
	}

	const int wmessage_size = MultiByteToWideChar(CP_UTF8, 0, buffer.data(), static_cast<int>(buffer.length()), wmessage_buf, wmessage_buflen);
	if (wmessage_size <= 0)
		goto cleanup;

	DWORD chars_written;
	WriteConsoleW(hOutput, wmessage_buf, static_cast<DWORD>(wmessage_size), &chars_written, nullptr);

cleanup:
	if (wmessage_buf != wbuf)
		std::free(wmessage_buf);
#else
	const int fd = (level <= LOGLEVEL_WARNING) ? STDERR_FILENO : STDOUT_FILENO;
	write(fd, buffer.data(), buffer.length());
#endif

#endif
}

bool Log::IsConsoleOutputEnabled()
{
	return (s_console_level > LOGLEVEL_NONE);
}

void Log::SetConsoleOutputLevel(LOGLEVEL level)
{
	if (s_console_level == level)
		return;

	const bool was_enabled = (s_console_level > LOGLEVEL_NONE);
	const bool now_enabled = (level > LOGLEVEL_NONE);
	s_console_level = level;
	UpdateMaxLevel();

	if (was_enabled == now_enabled)
		return;

		// Worst that happens here is we write to a bad handle..

#if defined(_WIN32)
	static constexpr auto enable_virtual_terminal_processing = [](HANDLE hConsole) {
		DWORD old_mode;
		if (!GetConsoleMode(hConsole, &old_mode))
			return;

		// already enabled?
		if (old_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
			return;

		SetConsoleMode(hConsole, old_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	};

	// On windows, no console is allocated by default on a windows based application
	static bool console_was_allocated = false;
	static HANDLE old_stdin = NULL;
	static HANDLE old_stdout = NULL;
	static HANDLE old_stderr = NULL;

	if (now_enabled)
	{
		old_stdin = GetStdHandle(STD_INPUT_HANDLE);
		old_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
		old_stderr = GetStdHandle(STD_ERROR_HANDLE);

		if (!old_stdout)
		{
			// Attach to the parent console if we're running from a command window
			if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole())
				return;

			s_hConsoleStdIn = GetStdHandle(STD_INPUT_HANDLE);
			s_hConsoleStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
			s_hConsoleStdErr = GetStdHandle(STD_ERROR_HANDLE);

			enable_virtual_terminal_processing(s_hConsoleStdOut);
			enable_virtual_terminal_processing(s_hConsoleStdErr);

			std::FILE* fp;
			freopen_s(&fp, "CONIN$", "r", stdin);
			freopen_s(&fp, "CONOUT$", "w", stdout);
			freopen_s(&fp, "CONOUT$", "w", stderr);

			console_was_allocated = true;
		}
		else
		{
			s_hConsoleStdIn = old_stdin;
			s_hConsoleStdOut = old_stdout;
			s_hConsoleStdErr = old_stderr;
		}
	}
	else
	{
		if (console_was_allocated)
		{
			console_was_allocated = false;

			std::FILE* fp;
			freopen_s(&fp, "NUL:", "w", stderr);
			freopen_s(&fp, "NUL:", "w", stdout);
			freopen_s(&fp, "NUL:", "w", stdin);

			SetStdHandle(STD_ERROR_HANDLE, old_stderr);
			SetStdHandle(STD_OUTPUT_HANDLE, old_stdout);
			SetStdHandle(STD_INPUT_HANDLE, old_stdin);

			s_hConsoleStdIn = NULL;
			s_hConsoleStdOut = NULL;
			s_hConsoleStdErr = NULL;

			FreeConsole();
		}
	}
#endif
}

__ri void Log::WriteToDebug(LOGLEVEL level, ConsoleColors color, std::string_view message)
{
#ifdef _WIN32
	static constexpr size_t BUFFER_SIZE = 512;

	// Convert to UTF-16 first so Unicode characters display correctly. NT is going to do it anyway...
	wchar_t wbuf[BUFFER_SIZE];
	wchar_t* wmessage_buf = wbuf;
	int wmessage_buflen = static_cast<int>(std::size(wbuf) - 1);
	if (message.length() >= std::size(wbuf))
	{
		wmessage_buflen = static_cast<int>(message.length());
		wmessage_buf = static_cast<wchar_t*>(std::malloc((static_cast<size_t>(wmessage_buflen) + 2) * sizeof(wchar_t)));
		if (!wmessage_buf)
			return;
	}

	int wmessage_size = 0;
	if (!message.empty()) [[likely]]
	{
		wmessage_size = MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.length()), wmessage_buf, wmessage_buflen);
		if (wmessage_size <= 0)
			goto cleanup;
	}

	wmessage_buf[wmessage_size++] = L'\n';
	wmessage_buf[wmessage_size++] = 0;
	OutputDebugStringW(wmessage_buf);

cleanup:
	if (wmessage_buf != wbuf)
		std::free(wmessage_buf);
#endif
}

bool Log::IsDebugOutputAvailable()
{
#ifdef _WIN32
	return IsDebuggerPresent();
#else
	return false;
#endif
}

bool Log::IsDebugOutputEnabled()
{
	return (s_console_level > LOGLEVEL_NONE);
}

void Log::SetDebugOutputLevel(LOGLEVEL level)
{
	s_debug_level = level;
	UpdateMaxLevel();
}

__ri void Log::WriteToFile(LOGLEVEL level, ConsoleColors color, std::string_view message)
{
	std::unique_lock lock(s_file_mutex);
	if (!s_file_handle) [[unlikely]]
		return;

	if (!message.empty()) [[likely]]
	{
		if (s_log_timestamps)
		{
			std::fprintf(s_file_handle.get(), TIMESTAMP_PRINTF_STRING "%.*s\n", GetCurrentMessageTime(),
				static_cast<int>(message.size()), message.data());
		}
		else
		{
			std::fprintf(s_file_handle.get(), "%.*s\n", static_cast<int>(message.size()), message.data());
		}
	}
	else
	{
		if (s_log_timestamps)
		{
			std::fprintf(s_file_handle.get(), TIMESTAMP_PRINTF_STRING "\n", GetCurrentMessageTime());
		}
		else
		{
			std::fputc('\n', s_file_handle.get());
		}
	}

	std::fflush(s_file_handle.get());
}

bool Log::IsFileOutputEnabled()
{
	return (s_file_level > LOGLEVEL_NONE);
}

bool Log::SetFileOutputLevel(LOGLEVEL level, std::string path)
{
	std::unique_lock lock(s_file_mutex);

	const bool was_enabled = (s_file_level > LOGLEVEL_NONE);
	const bool new_enabled = (level > LOGLEVEL_NONE && !path.empty());
	if (was_enabled != new_enabled || (new_enabled && path == s_file_path))
	{
		if (new_enabled)
		{
			if (!s_file_handle || s_file_path != path)
			{
				s_file_handle.reset();
				s_file_handle = FileSystem::OpenManagedSharedCFile(path.c_str(), "wb", FileSystem::FileShareMode::DenyWrite);
				if (s_file_handle)
				{
					s_file_path = std::move(path);
				}
				else
				{
					s_file_path = {};

					if (IsConsoleOutputEnabled())
						WriteToConsole(LOGLEVEL_ERROR, Color_StrongRed, TinyString::from_format("Failed to open log file '{}'", path));
				}
			}
		}
		else
		{
			s_file_handle.reset();
			s_file_path = {};
		}
	}

	s_file_level = s_file_handle ? level : LOGLEVEL_NONE;
	UpdateMaxLevel();
	return IsFileOutputEnabled();
}

std::FILE* Log::GetFileLogHandle()
{
	std::unique_lock lock(s_file_mutex);
	return s_file_handle.get();
}

#if defined(__ANDROID__)
bool Log::SetAndroidFileOutputLevel(LOGLEVEL level, std::string path)
{
	std::unique_lock lock(s_android_file_mutex);

	const bool enable = (level > LOGLEVEL_NONE) && !path.empty();
	if (!enable)
	{
		s_android_file_handle.reset();
		s_android_file_path.clear();
		s_android_file_level = LOGLEVEL_NONE;
		UpdateMaxLevel();
		return false;
	}

	if (!s_android_file_handle || s_android_file_path != path)
	{
		s_android_file_handle.reset();

		const size_t slash_pos = path.find_last_of(FS_OSPATH_SEPARATOR_CHARACTER);
		if (slash_pos != std::string::npos)
		{
			std::string directory = path.substr(0, slash_pos);
			FileSystem::EnsureDirectoryExists(directory.c_str(), true);
		}

		s_android_file_handle = FileSystem::OpenManagedSharedCFile(path.c_str(), "wb", FileSystem::FileShareMode::DenyWrite);
		if (!s_android_file_handle)
		{
			s_android_file_path.clear();
			s_android_file_level = LOGLEVEL_NONE;
			UpdateMaxLevel();
			__android_log_print(ANDROID_LOG_ERROR, "NDK_LOG", "Failed to open Android log file '%s'", path.c_str());
			return false;
		}

		s_android_file_path = std::move(path);
	}

	s_android_file_level = level;
	UpdateMaxLevel();
	return true;
}
#endif

bool Log::IsHostOutputEnabled()
{
	return (s_host_level > LOGLEVEL_NONE);
}

void Log::SetHostOutputLevel(LOGLEVEL level, HostCallbackType callback)
{
	s_host_callback = callback;
	s_host_level = callback ? level : LOGLEVEL_NONE;
	UpdateMaxLevel();
}

bool Log::AreTimestampsEnabled()
{
	return s_log_timestamps;
}

void Log::SetTimestampsEnabled(bool enabled)
{
	s_log_timestamps = enabled;
}

LOGLEVEL Log::GetMaxLevel()
{
	return s_max_level;
}

__ri void Log::UpdateMaxLevel()
{
#if defined(__ANDROID__)
	s_max_level = std::max(s_console_level, std::max(s_debug_level, std::max(s_file_level, std::max(s_host_level, s_android_file_level))));
#else
	s_max_level = std::max(s_console_level, std::max(s_debug_level, std::max(s_file_level, s_host_level)));
#endif
}

// [iPSX2] Log Limiting Logic (moved to Global scope/Log Namespace)
	namespace LogLimiter
	{
		struct LimiterEntry {
			std::string_view tag;
			std::atomic<int> count{0};
			std::atomic<bool> notified{false};
		};

		static LimiterEntry s_entries[] = {
			{"@@RECOMPILE@@", {}, {}},
			{"@@IOP_RECOMPILE@@", {}, {}},
			{"@@EE_TICK@@", {}, {}},
			{"@@RECSTUB_CALL@@", {}, {}},
			{"@@VMODE_OK@@", {}, {}},
			{"@@VTLB_DUMMY@@", {}, {}},
			{"@@REC_BLOCK@@", {}, {}},
			{"@@SITE_MAP@@", {}, {}},
			{"@@DEBUG@@", {}, {}},
			{"[DEBUG-VERIFY]", {}, {}}
		};

		static bool ShouldSuppress(std::string_view message)
		{
            // [P52] Suppress all @@ probe tags except essential startup/fatal.
            // Debug builds can enable verbose probes via iPSX2_VERBOSE_LOG=1.
            static bool s_verbose = [](){
#if defined(DEBUG) || defined(_DEBUG)
                const char* e = std::getenv("iPSX2_VERBOSE_LOG");
                return e && std::atoi(e);
#else
                return false;
#endif
            }();
            if (!s_verbose && message.find("@@") != std::string_view::npos) {
                if (message.find("@@LOG_") != std::string_view::npos ||
                    message.find("@@BUNDLE_ID") != std::string_view::npos ||
                    message.find("@@BUILD_ID") != std::string_view::npos ||
                    message.find("@@BIOS_PICK") != std::string_view::npos ||
                    message.find("@@BIOS_DIR") != std::string_view::npos ||
                    message.find("@@BIOS_ENV") != std::string_view::npos ||
                    message.find("@@BIOS_SCAN") != std::string_view::npos ||
                    message.find("@@ISO_BOOT") != std::string_view::npos ||
                    message.find("@@CFG") != std::string_view::npos ||
                    message.find("@@JIT_GATE") != std::string_view::npos ||
                    message.find("@@AUTO_BOOT") != std::string_view::npos ||
                    message.find("@@SIG") != std::string_view::npos ||
                    message.find("@@EE_SEL") != std::string_view::npos ||
                    message.find("@@IOP_SEL") != std::string_view::npos ||
                    message.find("@@TEST") != std::string_view::npos ||
                    message.find("@@ELF") != std::string_view::npos ||
                    message.find("@@BOOT") != std::string_view::npos ||
                    // [V10 Phase 3.2.1] backport: 新 tag for Step 5 / SW fastmem / auto-press / GIF dump 等
                    message.find("@@VU_SEL") != std::string_view::npos ||
                    message.find("@@AUTO_PRESS") != std::string_view::npos ||
                    message.find("@@SW_FASTMEM_FIX") != std::string_view::npos ||
                    message.find("@@FASTMEM_DBG") != std::string_view::npos ||
                    message.find("@@DIE_BIT_CHECK") != std::string_view::npos ||
                    message.find("@@GIF_DUMP") != std::string_view::npos ||
                    message.find("@@GIF_PKT") != std::string_view::npos ||
                    message.find("@@BLOCKCYCLE_MASK") != std::string_view::npos ||
                    message.find("@@REC_NULL") != std::string_view::npos ||
                    // [V34] QA probe tags (eval_bench.py / ss_compare.py / gif_diff.py consume these)
                    message.find("@@BL_FRAME") != std::string_view::npos ||
                    message.find("@@QA_") != std::string_view::npos ||
                    // [port 2026-05-30] iv2 VU1 measurement tags: bit-exact diff harness,
                    // per-stage ORCH profiler, GS_TICK (VU1 kick counter + vsync for vps).
                    message.find("@@VU_IV2") != std::string_view::npos ||
                    message.find("@@VU1_ORCH") != std::string_view::npos ||
                    message.find("@@GS_TICK") != std::string_view::npos ||
                    // [P101-7 Stage2-b] kernel-HLE diagnostic tags (all env-gated, default-off):
                    // probe / self-check / kernel verdict. eval consumes @@HLE_SELFCHECK@@.
                    message.find("@@HLE_") != std::string_view::npos ||
                    // [P101-14 2026-08-06] iv2 EE ブロックキャッシュの計測タグ
                    // (@@IV2_INVAL@@ = 無効化内訳 / @@IV2_BLOCK_POOL@@ = 実効 cap /
                    //  @@IV2_PROFILE@@ = ブロック統計)。いずれも `iPSX2_IV2_PROFILE=1` でのみ出る。
                    // ★ stderr への fprintf はログへリダイレクトされると block-buffered になり
                    //   pkill -9 で失われるので、計測タグは必ず Console 経由にすること。
                    message.find("@@IV2_") != std::string_view::npos ||
                    // [P115 2026-08-08] GS 出力の回帰ゲート (scripts/gs_verify.sh が消費する)。
                    // iPSX2_GS_SHOT_AT でのみ出る。上流 GS 合流のクローズ時に除去可。
                    message.find("@@GSSHOT@@") != std::string_view::npos ||
                    message.find("FATAL") != std::string_view::npos)
                    return false;
                return true;
            }
            // Lazy initialization of limits
			static int s_max_per_tag = [](){
				const char* env = std::getenv("iPSX2_LOG_MAX_PER_TAG");
                int val = env ? std::atoi(env) : 50;
                fprintf(stderr, "@@CFG@@ iPSX2_LOG_MAX_PER_TAG=%d (env=%s)\n", val, env ? env : "null");
				return val;
			}();

            static int s_max_total_lines = [](){
                const char* env = std::getenv("iPSX2_LOG_MAX_LINES");
                int val = env ? std::atoi(env) : 0;
                fprintf(stderr, "@@CFG@@ iPSX2_LOG_MAX_LINES=%d (env=%s)\n", val, env ? env : "null");
                return val;
            }();
            
            static std::atomic<int> s_total_lines{0};
            static std::atomic<bool> s_global_notified{false};

            if (message.find("@@SIG") != std::string_view::npos ||
                message.find("FATAL") != std::string_view::npos ||
                message.find("@@CT_MIN") != std::string_view::npos ||
                message.find("Aborting application") != std::string_view::npos ||
                message.find("assert") != std::string_view::npos ||
                message.find("Assertion") != std::string_view::npos ||
                message.find("@@BUILD_ID") != std::string_view::npos ||
                message.find("@@LOG_TAG_CUTOFF@@") != std::string_view::npos ||
                message.find("@@LOG_GLOBAL_CUTOFF@@") != std::string_view::npos) 
            {
                return false;
            }

            if (s_max_total_lines > 0) {
                int lines = s_total_lines.fetch_add(1, std::memory_order_relaxed);
                if (lines >= s_max_total_lines) {
                    bool expected = false;
                    if (s_global_notified.compare_exchange_strong(expected, true)) {
                         TinyString msg = TinyString::from_format("@@LOG_GLOBAL_CUTOFF@@ max_lines={} suppressed=1", s_max_total_lines);
                         Log::Write(LOGLEVEL_INFO, Color_Red, msg.c_str());
                    }
                    return true;
                }
            }

			for (auto& entry : s_entries)
			{
				if (message.find(entry.tag) != std::string_view::npos)
				{
					int current = entry.count.fetch_add(1, std::memory_order_relaxed) + 1;
					if (current <= s_max_per_tag) {
						return false;
					} else {
						bool expected = false;
						if (entry.notified.compare_exchange_strong(expected, true)) {
                            TinyString msg = TinyString::from_format("@@LOG_TAG_CUTOFF@@ tag=\"{}\" max={} suppressed=1", 
                                entry.tag, s_max_per_tag);
                            Log::Write(LOGLEVEL_INFO, Color_Red, msg.c_str());
						}
						return true;
					}
				}
			}
			return false;
		}
	}

    // Move definition INSIDE namespace Log to match static declaration
    namespace Log {
        static void ExecuteCallbacks(LOGLEVEL level, ConsoleColors color, std::string_view message)
        {
            if (LogLimiter::ShouldSuppress(message)) {
                return;
            }

            // TODO: Cache the message time.

        #if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_DEBUG, "NDK_LOG", "%s", std::string(message).c_str());

            if (level <= s_android_file_level)
                WriteToAndroidFile(level, color, message);

            if (level <= s_file_level)
                WriteToFile(level, color, message);

            if (level <= s_host_level)
            {
                const HostCallbackType callback = s_host_callback;
                if (callback)
                    s_host_callback(level, color, message);
            }

            return;
        #else
            // Split newlines into separate messages.
            std::string_view::size_type start_pos = 0;
            if (std::string_view::size_type end_pos = message.find('\n'); end_pos != std::string::npos) [[unlikely]]
            {
                for (;;)
                {
                    std::string_view message_line;
                    if (start_pos != end_pos)
                        message_line = message.substr(start_pos, (end_pos == std::string_view::npos) ? end_pos : end_pos - start_pos);

                    ExecuteCallbacks(level, color, message_line);

                    if (end_pos == std::string_view::npos)
                        return;

                    start_pos = end_pos + 1;
                    end_pos = message.find('\n', start_pos);
                }
                return;
            }

            pxAssert(level > LOGLEVEL_NONE);
            if (level <= s_console_level)
                WriteToConsole(level, color, message);

            if (level <= s_debug_level)
                WriteToDebug(level, color, message);

            if (level <= s_file_level)
                WriteToFile(level, color, message);

            if (level <= s_host_level)
            {
                // double check in case of race here
                const HostCallbackType callback = s_host_callback;
                if (callback)
                    s_host_callback(level, color, message);
            }
        #endif
        }
    } // namespace Log

void Log::Write(LOGLEVEL level, ConsoleColors color, std::string_view message)
{
	if (level > s_max_level)
		return;

	ExecuteCallbacks(level, color, message);
}

void Log::Writef(LOGLEVEL level, ConsoleColors color, const char* format, ...)
{
	std::va_list ap;
	va_start(ap, format);
	Writev(level, color, format, ap);
	va_end(ap);
}

void Log::Writev(LOGLEVEL level, ConsoleColors color, const char* format, va_list ap)
{
	if (level > s_max_level)
		return;

	std::va_list ap_copy;
	va_copy(ap_copy, ap);

#ifdef _WIN32
	const u32 required_size = static_cast<u32>(_vscprintf(format, ap_copy));
#else
	const u32 required_size = std::vsnprintf(nullptr, 0, format, ap_copy);
#endif
	va_end(ap_copy);

	if (required_size < 512)
	{
		char buffer[512];
		const int len = std::vsnprintf(buffer, std::size(buffer), format, ap);
		if (len > 0)
			ExecuteCallbacks(level, color, std::string_view(buffer, static_cast<size_t>(len)));
	}
	else
	{
		char* buffer = new char[required_size + 1];
		const int len = std::vsnprintf(buffer, required_size + 1, format, ap);
		if (len > 0)
			ExecuteCallbacks(level, color, std::string_view(buffer, static_cast<size_t>(len)));
		delete[] buffer;
	}
}

void Log::WriteFmtArgs(LOGLEVEL level, ConsoleColors color, fmt::string_view fmt, fmt::format_args args)
{
	if (level > s_max_level)
		return;

	fmt::memory_buffer buffer;
	fmt::vformat_to(std::back_inserter(buffer), fmt, args);

	ExecuteCallbacks(level, color, std::string_view(buffer.data(), buffer.size()));
}
