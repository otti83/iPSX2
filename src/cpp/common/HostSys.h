// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

class Error;

// --------------------------------------------------------------------------------------
//  PageProtectionMode
// --------------------------------------------------------------------------------------
class PageProtectionMode
{
protected:
	bool m_read = false;
	bool m_write = false;
	bool m_exec = false;

public:
	__fi constexpr PageProtectionMode() = default;

	__fi constexpr PageProtectionMode& Read(bool allow = true)
	{
		m_read = allow;
		return *this;
	}

	__fi constexpr PageProtectionMode& Write(bool allow = true)
	{
		m_write = allow;
		return *this;
	}

	__fi constexpr PageProtectionMode& Execute(bool allow = true)
	{
		m_exec = allow;
		return *this;
	}

	__fi constexpr PageProtectionMode& All(bool allow = true)
	{
		m_read = m_write = m_exec = allow;
		return *this;
	}

	__fi constexpr bool CanRead() const { return m_read; }
	__fi constexpr bool CanWrite() const { return m_write; }
	__fi constexpr bool CanExecute() const { return m_exec && m_read; }
	__fi constexpr bool IsNone() const { return !m_read && !m_write; }
};

static __fi PageProtectionMode PageAccess_None()
{
	return PageProtectionMode();
}

static __fi PageProtectionMode PageAccess_ReadOnly()
{
	return PageProtectionMode().Read();
}

static __fi PageProtectionMode PageAccess_WriteOnly()
{
	return PageProtectionMode().Write();
}

static __fi PageProtectionMode PageAccess_ReadWrite()
{
	return PageAccess_ReadOnly().Write();
}

static __fi PageProtectionMode PageAccess_ExecOnly()
{
	return PageAccess_ReadOnly().Execute();
}

static __fi PageProtectionMode PageAccess_Any()
{
	return PageProtectionMode().All();
}

// --------------------------------------------------------------------------------------
//  HostSys
// --------------------------------------------------------------------------------------
namespace HostSys
{
	// Maps a block of memory for use as a recompiled code buffer.
	// Returns NULL on allocation failure.
	extern void* Mmap(void* base, size_t size, const PageProtectionMode& mode);

	// Unmaps a block allocated by SysMmap
	extern void Munmap(void* base, size_t size);

	extern void MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode);

	extern std::string GetFileMappingName(const char* prefix);
	extern void* CreateSharedMemory(const char* name, size_t size);
	extern void DestroySharedMemory(void* ptr);
	extern void* MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, const PageProtectionMode& mode);
	extern void UnmapSharedMemory(void* baseaddr, size_t size);

	/// JIT write protect for Apple Silicon. Needs to be called prior to writing to any RWX pages.
#if !defined(__APPLE__) || !defined(_M_ARM64)
	// clang-format -off
	[[maybe_unused]] __fi static void BeginCodeWrite() {}
	[[maybe_unused]] __fi static void EndCodeWrite() {}
	// clang-format on
#else
	void BeginCodeWrite();
	void EndCodeWrite();
#endif

	/// Flushes the instruction cache on the host for the specified range.
	/// Only needed on ARM64, X86 has coherent D/I cache.
#ifdef _M_X86
	[[maybe_unused]] __fi static void FlushInstructionCache(void* address, u32 size) {}
#else
	void FlushInstructionCache(void* address, u32 size);
#endif

	/// Returns the size of pages for the current host.
	size_t GetRuntimePageSize();

	/// Returns the size of a cache line for the current host.
	size_t GetRuntimeCacheLineSize();

	// JIT A64 Shim
	static inline void* CommitCodeSpace(size_t size)
	{
		// R/W/X; 
		return Mmap(nullptr, size, PageAccess_Any());
	}

	static inline void DecommitCodeSpace(void* ptr, size_t size)
	{
		Munmap(ptr, size);
	}

    // --- Safety Extensions ---

    // Block Exit Reason (for tracing)
    enum class BlockExitReason : u8 {
        Unknown = 0,
        Link,           // Direct block-to-block link
        Helper,         // Called a C++ helper function
        Exception,      // Exception/interrupt occurred
        Fallback,       // Fell back to interpreter
        DispatcherExit, // Normal exit to dispatcher
        BranchTaken,    // Branch was taken
        BranchNotTaken, // Branch fell through
    };

    // Single trace entry for ring buffer
    struct BlockTraceEntry {
        u32 guest_pc;
        u32 next_guest_pc;  // Where execution will go next
        u32 block_id;
        u32 cycle;
        BlockExitReason exit_reason;
        // Exception info (only valid when exit_reason == Exception)
        u8 exception_code;
        u32 epc;
        u8 bd_bit;  // Branch Delay bit
    };

    // Ring buffer size (must be power of 2)
    static constexpr size_t BLOCK_TRACE_SIZE = 256;

    struct JitRuntimeContext {
        u32 guest_pc;
        u32 next_guest_pc;  // Set by JIT code before exit
        u32 block_id;
        
        // Exception info (set by exception handlers)
        u8 pending_exception_code;
        u32 pending_epc;
        u8 pending_bd_bit;
        
        // Block Trace Ring Buffer
        BlockTraceEntry trace_buffer[BLOCK_TRACE_SIZE];
        u32 trace_index;
        
        // Watchdog: last update timestamp (milliseconds since epoch, or steady_clock)
        u64 last_trace_update_ms;
        static constexpr u64 HANG_THRESHOLD_MS = 500;
        
        // Link Loop Detection
        u32 last_block_id;
        u32 repeat_count;
        static constexpr u32 LOOP_THRESHOLD = 100000;
        
        void RecordBlockExit(u32 pc, u32 next_pc, u32 id, u32 cyc, BlockExitReason reason,
                             u8 exc_code = 0, u32 exc_epc = 0, u8 exc_bd = 0) {
            BlockTraceEntry& entry = trace_buffer[trace_index & (BLOCK_TRACE_SIZE - 1)];
            entry.guest_pc = pc;
            entry.next_guest_pc = next_pc;
            entry.block_id = id;
            entry.cycle = cyc;
            entry.exit_reason = reason;
            entry.exception_code = exc_code;
            entry.epc = exc_epc;
            entry.bd_bit = exc_bd;
            trace_index++;
            
            // Update watchdog timestamp (simple counter approximation without chrono)
            // In real use, this would be: std::chrono::steady_clock::now()
            last_trace_update_ms++; // placeholder increment
            
            // Link loop detection
            if (id == last_block_id) {
                repeat_count++;
            } else {
                last_block_id = id;
                repeat_count = 1;
            }
        }
        
        bool IsLinkLoopSuspected() const {
            return repeat_count >= LOOP_THRESHOLD;
        }
        
        void ResetLoopDetection() {
            repeat_count = 0;
        }
        
        // Check if trace hasn't been updated (for external watchdog)
        bool IsHangSuspected(u64 current_ms) const {
            return (current_ms - last_trace_update_ms) > HANG_THRESHOLD_MS;
        }
    };
    extern thread_local JitRuntimeContext g_JitContext;

    // RAII handling for W^X and Cache Flushing
    class AutoCodeWrite
    {
    public:
        AutoCodeWrite(void* ptr = nullptr, size_t size = 0) 
            : m_ptr(ptr), m_size(size) 
        {
            BeginCodeWrite();
        }
        
        ~AutoCodeWrite()
        {
            EndCodeWrite();
            if (m_ptr && m_size > 0) {
                FlushInstructionCache(m_ptr, static_cast<u32>(m_size));
            }
        }
        
        // Disable copy
        AutoCodeWrite(const AutoCodeWrite&) = delete;
        AutoCodeWrite& operator=(const AutoCodeWrite&) = delete;

    private:
        void* m_ptr;
        size_t m_size;
    };

} // namespace HostSys

namespace PageFaultHandler
{
	enum class HandlerResult
	{
		ContinueExecution,
		ExecuteNextHandler,
	};

	HandlerResult HandlePageFault(void* exception_pc, void* fault_address, bool is_write);
	bool Install_Fresh(Error* error = nullptr);
} // namespace PageFaultHandler

class SharedMemoryMappingArea
{
public:
	static std::unique_ptr<SharedMemoryMappingArea> Create(size_t size);

	~SharedMemoryMappingArea();

	__fi size_t GetSize() const { return m_size; }
	__fi size_t GetNumPages() const { return m_num_pages; }

	__fi u8* BasePointer() const { return m_base_ptr; }
	__fi u8* OffsetPointer(size_t offset) const { return m_base_ptr + offset; }
	__fi u8* PagePointer(size_t page) const { return m_base_ptr + __pagesize * page; }

	u8* Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode);
	bool Unmap(void* map_base, size_t map_size);

private:
	SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages);

	u8* m_base_ptr;
	size_t m_size;
	size_t m_num_pages;
	size_t m_num_mappings = 0;

#ifdef _WIN32
	using PlaceholderMap = std::map<size_t, size_t>;

	PlaceholderMap::iterator FindPlaceholder(size_t page);

	PlaceholderMap m_placeholder_ranges;
#endif
};

extern u64 GetTickFrequency();
extern u64 GetCPUTicks();
extern u64 GetPhysicalMemory();
extern u64 GetAvailablePhysicalMemory();
/// Spin for a short period of time (call while spinning waiting for a lock)
/// Returns the approximate number of ns that passed
extern u32 ShortSpin();
/// Number of ns to spin for before sleeping a thread
extern const u32 SPIN_TIME_NS;
/// Like C abort() but adds the given message to the crashlog
[[noreturn]] void AbortWithMessage(const char* msg);

extern std::string GetOSVersionString();

namespace Common
{
	/// Enables or disables the screen saver from starting.
	bool InhibitScreensaver(bool inhibit);

	/// Abstracts platform-specific code for asynchronously playing a sound.
	/// On Windows, this will use PlaySound(). On Linux, it will shell out to aplay. On MacOS, it uses NSSound.
	bool PlaySoundAsync(const char* path);

	void SetMousePosition(int x, int y);
	bool AttachMousePositionCb(std::function<void(int,int)> cb);
	void DetachMousePositionCb();
} // namespace Common
