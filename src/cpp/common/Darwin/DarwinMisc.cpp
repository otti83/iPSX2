// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Assertions.h"
#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Darwin/DarwinMisc.h"
#include "common/Error.h"
#include "common/Pcsx2Types.h"
#include "common/Threading.h"
#include "common/WindowInfo.h"
#include "common/HostSys.h"

#include <dlfcn.h>
#include <csignal>
#include <csetjmp>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <optional>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#include <time.h>
#include <pthread.h> // Added for pthread_jit_write_protect_np
#include <fcntl.h>   // For O_CREAT, O_APPEND, O_WRONLY
#include <execinfo.h> // For backtrace()
#include <glob.h>     // For TXM detection
#include <mach/mach.h> // For vm_remap

#if defined(__aarch64__) && defined(__APPLE__)
// Declaration for pthread_jit_write_protect_np if not available in headers
extern "C" void pthread_jit_write_protect_np(int enabled);
#endif



#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/mach_time.h>
#include <mach/task.h>
#include <mach/vm_map.h>
#include <mutex>
#include <TargetConditionals.h>

#if !TARGET_OS_IPHONE
#include <mach/mach_vm.h>
#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

#if TARGET_OS_IPHONE
extern "C" {
    kern_return_t mach_vm_map(
        vm_map_t target_task,
        mach_vm_address_t *address,
        mach_vm_size_t size,
        mach_vm_offset_t mask,
        int flags,
        mem_entry_name_port_t object,
        memory_object_offset_t offset,
        boolean_t copy,
        vm_prot_t cur_protection,
        vm_prot_t max_protection,
        vm_inherit_t inheritance);

    kern_return_t mach_vm_deallocate(
        vm_map_t target,
        mach_vm_address_t address,
        mach_vm_size_t size);

    kern_return_t mach_vm_protect(
        vm_map_t target_task,
        mach_vm_address_t address,
        mach_vm_size_t size,
        boolean_t set_maximum,
        vm_prot_t new_protection);

    kern_return_t mach_make_memory_entry_64(
        vm_map_t target_task,
        mach_vm_size_t *size,
        mach_vm_offset_t offset,
        vm_prot_t permission,
        mach_port_t *object_handle,
        mem_entry_name_port_t parent_entry);

    kern_return_t mach_vm_remap(
        vm_map_t target_task,
        mach_vm_address_t *target_address,
        mach_vm_size_t size,
        mach_vm_offset_t mask,
        int flags,
        vm_map_t src_task,
        mach_vm_address_t src_address,
        boolean_t copy,
        vm_prot_t *cur_protection,
        vm_prot_t *max_protection,
        vm_inherit_t inheritance);
}

// [P47] Base address of first MapSharedMemory result (data memory) for fastmem remap source.
static u8* s_ios_shared_data_base = nullptr;
#endif

// Darwin (OSX) is a bit different from Linux when requesting properties of
// the OS because of its BSD/Mach heritage. Helpfully, most of this code
// should translate pretty well to other *BSD systems. (e.g.: the sysctl(3)
// interface).
//
// For an overview of all of Darwin's sysctls, check:
// https://developer.apple.com/library/mac/documentation/Darwin/Reference/ManPages/man3/sysctl.3.html



// Return the total physical memory on the machine, in bytes. Returns 0 on
// failure (not supported by the operating system).
u64 GetPhysicalMemory()
{

	// Debug log to verify TARGET_OS macros
	Console.WriteLn("DEBUG: DarwinMisc: TARGET_OS_IPHONE=%d, TARGET_OS_SIMULATOR=%d", TARGET_OS_IPHONE, TARGET_OS_SIMULATOR);
	
	u64 getmem = 0;
	size_t len = sizeof(getmem);
	int mib[] = {CTL_HW, HW_MEMSIZE};
	if (sysctl(mib, std::size(mib), &getmem, &len, NULL, 0) < 0)
		perror("sysctl:");
	return getmem;
}

u64 GetAvailablePhysicalMemory()
{
#if !TARGET_OS_IPHONE
	const mach_port_t host_port = mach_host_self();
	vm_size_t page_size;

	// Get the system's page size.
	if (host_page_size(host_port, &page_size) != KERN_SUCCESS)
		return 0;

	vm_statistics64_data_t vm_stat;
	mach_msg_type_number_t host_size = sizeof(vm_statistics64_data_t) / sizeof(integer_t);

	// Get system memory statistics.
	if (host_statistics64(host_port, HOST_VM_INFO, reinterpret_cast<host_info64_t>(&vm_stat), &host_size) != KERN_SUCCESS)
		return 0;

	// Get the number of free and inactive pages.
	const u64 free_pages = static_cast<u64>(vm_stat.free_count);
	const u64 inactive_pages = static_cast<u64>(vm_stat.inactive_count);

	// Calculate available memory.
	const u64 get_available_mem = (free_pages + inactive_pages) * page_size;

	return get_available_mem;
#else
	return GetPhysicalMemory() / 2;
#endif
}

static mach_timebase_info_data_t s_timebase_info;
static const u64 tickfreq = []() {
	if (mach_timebase_info(&s_timebase_info) != KERN_SUCCESS)
		abort();
	return (u64)1e9 * (u64)s_timebase_info.denom / (u64)s_timebase_info.numer;
}();

// returns the performance-counter frequency: ticks per second (Hz)
//
// usage:
//   u64 seconds_passed = GetCPUTicks() / GetTickFrequency();
//   u64 millis_passed = (GetCPUTicks() * 1000) / GetTickFrequency();
//
// NOTE: multiply, subtract, ... your ticks before dividing by
// GetTickFrequency() to maintain good precision.
u64 GetTickFrequency()
{
	return tickfreq;
}

// return the number of "ticks" since some arbitrary, fixed time in the
// past. On OSX x86(-64), this is actually the number of nanoseconds passed,
// because mach_timebase_info.numer == denom == 1. So "ticks" ==
// nanoseconds.
u64 GetCPUTicks()
{
	return mach_absolute_time();
}

static std::string sysctl_str(int category, int name)
{
	char buf[32];
	size_t len = sizeof(buf);
	int mib[] = {category, name};
	sysctl(mib, std::size(mib), buf, &len, nullptr, 0);
	return std::string(buf, len > 0 ? len - 1 : 0);
}

template <typename T>
static std::optional<T> sysctlbyname_T(const char* name)
{
	T output = 0;
	size_t output_size = sizeof(output);
	if (sysctlbyname(name, &output, &output_size, nullptr, 0) != 0)
		return std::nullopt;
	if (output_size != sizeof(output))
	{
		ERROR_LOG("(DarwinMisc) sysctl {} gave unexpected size {}", name, output_size);
		return std::nullopt;
	}

	return output;
}

std::string GetOSVersionString()
{
	std::string type = sysctl_str(CTL_KERN, KERN_OSTYPE);
	std::string release = sysctl_str(CTL_KERN, KERN_OSRELEASE);
	std::string arch = sysctl_str(CTL_HW, HW_MACHINE);
	return type + " " + release + " " + arch;
}

#if !TARGET_OS_IPHONE
static IOPMAssertionID s_pm_assertion;
#endif

bool Common::InhibitScreensaver(bool inhibit)
{
#if !TARGET_OS_IPHONE
	if (s_pm_assertion)
	{
		IOPMAssertionRelease(s_pm_assertion);
		s_pm_assertion = 0;
	}

	if (inhibit)
		IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn, CFSTR("Playing a game"), &s_pm_assertion);
#endif
	return true;
}

void Common::SetMousePosition(int x, int y)
{
#if !TARGET_OS_IPHONE
	// Little bit ugly but;
	// Creating mouse move events and posting them wasn't very reliable.
	// Calling CGWarpMouseCursorPosition without CGAssociateMouseAndMouseCursorPosition(false)
	// ends up with the cursor feeling "sticky".
	CGAssociateMouseAndMouseCursorPosition(false);
	CGWarpMouseCursorPosition(CGPointMake(x, y));
	CGAssociateMouseAndMouseCursorPosition(true); // The default state
#endif
	return;
}

#if !TARGET_OS_IPHONE
CFMachPortRef mouseEventTap = nullptr;
CFRunLoopSourceRef mouseRunLoopSource = nullptr;

static std::function<void(int, int)> fnMouseMoveCb;
CGEventRef mouseMoveCallback(CGEventTapProxy, CGEventType type, CGEventRef event, void* arg)
{
	if (type == kCGEventMouseMoved)
	{
		const CGPoint location = CGEventGetLocation(event);
		fnMouseMoveCb(location.x, location.y);
	}
	return event;
}
#endif

bool Common::AttachMousePositionCb(std::function<void(int, int)> cb)
{
#if !TARGET_OS_IPHONE
	if (!AXIsProcessTrusted())
	{
		Console.Warning("Process isn't trusted with accessibility permissions. Mouse tracking will not work!");
	}

	fnMouseMoveCb = cb;
	mouseEventTap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
		CGEventMaskBit(kCGEventMouseMoved), mouseMoveCallback, nullptr);
	if (!mouseEventTap)
	{
		Console.Warning("Unable to create mouse moved event tap. Mouse tracking will not work!");
		return false;
	}

	mouseRunLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, mouseEventTap, 0);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), mouseRunLoopSource, kCFRunLoopCommonModes);
#endif

	return true;
}

void Common::DetachMousePositionCb()
{
#if !TARGET_OS_IPHONE
	if (mouseRunLoopSource)
	{
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), mouseRunLoopSource, kCFRunLoopCommonModes);
		CFRelease(mouseRunLoopSource);
	}
	if (mouseEventTap)
	{
		CFRelease(mouseEventTap);
	}
	mouseRunLoopSource = nullptr;
	mouseEventTap = nullptr;
#endif
}

void Threading::Sleep(int ms)
{
	usleep(1000 * ms);
}

void Threading::SleepUntil(u64 ticks)
{
	// This is definitely sub-optimal, but apparently clock_nanosleep() doesn't exist.
	const s64 diff = static_cast<s64>(ticks - GetCPUTicks());
	if (diff <= 0)
		return;

	const u64 nanos = (static_cast<u64>(diff) * static_cast<u64>(s_timebase_info.denom)) / static_cast<u64>(s_timebase_info.numer);
	if (nanos == 0)
		return;

	struct timespec ts;
	ts.tv_sec = nanos / 1000000000ULL;
	ts.tv_nsec = nanos % 1000000000ULL;
	nanosleep(&ts, nullptr);
}

std::vector<DarwinMisc::CPUClass> DarwinMisc::GetCPUClasses()
{
	std::vector<CPUClass> out;

	if (std::optional<u32> nperflevels = sysctlbyname_T<u32>("hw.nperflevels"))
	{
		char name[64];
		for (u32 i = 0; i < *nperflevels; i++)
		{
			snprintf(name, sizeof(name), "hw.perflevel%u.physicalcpu", i);
			std::optional<u32> physicalcpu = sysctlbyname_T<u32>(name);
			snprintf(name, sizeof(name), "hw.perflevel%u.logicalcpu", i);
			std::optional<u32> logicalcpu = sysctlbyname_T<u32>(name);

			char levelname[64];
			size_t levelname_size = sizeof(levelname);
			snprintf(name, sizeof(name), "hw.perflevel%u.name", i);
			if (0 != sysctlbyname(name, levelname, &levelname_size, nullptr, 0))
				strcpy(levelname, "???");

			if (!physicalcpu.has_value() || !logicalcpu.has_value())
			{
				Console.Warning("(DarwinMisc) Perf level %u is missing data on %s cpus!",
					i, !physicalcpu.has_value() ? "physical" : "logical");
				continue;
			}

			out.push_back({levelname, *physicalcpu, *logicalcpu});
		}
	}
	else if (std::optional<u32> physcpu = sysctlbyname_T<u32>("hw.physicalcpu"))
	{
		out.push_back({"Default", *physcpu, sysctlbyname_T<u32>("hw.logicalcpu").value_or(0)});
	}
	else
	{
		Console.Warning("(DarwinMisc) Couldn't get cpu core count!");
	}

	return out;
}

size_t HostSys::GetRuntimePageSize()
{
	return sysctlbyname_T<u32>("hw.pagesize").value_or(0);
}

size_t HostSys::GetRuntimeCacheLineSize()
{
	return static_cast<size_t>(std::max<s64>(sysctlbyname_T<s64>("hw.cachelinesize").value_or(0), 0));
}

static __ri vm_prot_t MachProt(const PageProtectionMode& mode)
{
	vm_prot_t machmode = (mode.CanWrite()) ? VM_PROT_WRITE : 0;
	machmode |= (mode.CanRead()) ? VM_PROT_READ : 0;
	machmode |= (mode.CanExecute()) ? (VM_PROT_EXECUTE | VM_PROT_READ) : 0;
	return machmode;
}

// Mmap implementation for iOS (Standard mmap)
void* HostSys::Mmap(void* base, size_t size, const PageProtectionMode& mode)
{
	pxAssertMsg((size & (__pagesize - 1)) == 0, "Size is page aligned");
	if (mode.IsNone())
		return nullptr;

#ifdef __aarch64__
	// [P43] JIT code memory allocation — route through dual-mapping for iOS 26
	if (mode.CanExecute())
	{
		if (base)
			return nullptr;

		return DarwinMisc::MmapCodeDualMap(size);
	}
#endif

#include <TargetConditionals.h>

#if TARGET_OS_SIMULATOR
	Console.WriteLn("HostSys::Mmap (Simulator): Requesting size 0x%zx, base %p, prot %s%s%s",
		size, base, mode.CanRead() ? "R" : "", mode.CanWrite() ? "W" : "", mode.CanExecute() ? "X" : "");

	kern_return_t ret = vm_allocate(mach_task_self(), reinterpret_cast<vm_address_t*>(&base), size,
		base ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE);
	if (ret != KERN_SUCCESS)
	{
		Console.Error("vm_allocate() returned {}", ret);
		return nullptr;
	}

	ret = vm_protect(mach_task_self(), reinterpret_cast<vm_address_t>(base), size, false, static_cast<vm_prot_t>(MachProt(mode)));
	if (ret != KERN_SUCCESS)
	{
		Console.Error("vm_protect() returned {}", ret);
		vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(base), size);
		return nullptr;
	}
	return base;
#elif !TARGET_OS_IPHONE
	kern_return_t ret = mach_vm_allocate(mach_task_self(), reinterpret_cast<mach_vm_address_t*>(&base), size,
		base ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE);
	if (ret != KERN_SUCCESS)
	{
		DEV_LOG("mach_vm_allocate() returned {}", ret);
		return nullptr;
	}

	ret = mach_vm_protect(mach_task_self(), reinterpret_cast<mach_vm_address_t>(base), size, false, MachProt(mode));
	if (ret != KERN_SUCCESS)
	{
		DEV_LOG("mach_vm_protect() returned {}", ret);
		mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(base), size);
		return nullptr;
	}
	return base;
#else
	// iOS: Fallback to standard mmap if not JIT
	// Note: VM_FLAGS_FIXED equivalent is MAP_FIXED
	int flags = MAP_PRIVATE | MAP_ANON;
	if (base) flags |= MAP_FIXED;
	int prot = 0;
	if (mode.CanRead()) prot |= PROT_READ;
	if (mode.CanWrite()) prot |= PROT_WRITE;
	if (mode.CanExecute()) prot |= PROT_EXEC;

	void* res = mmap(base, size, prot, flags, -1, 0);
	if (res == MAP_FAILED) return nullptr;
	return res;
#endif
}

void HostSys::Munmap(void* base, size_t size)
{
	if (!base)
		return;
#if !TARGET_OS_IPHONE
	mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(base), size);
#else
	munmap(base, size);
#endif
}

void HostSys::MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	pxAssertMsg((size & (__pagesize - 1)) == 0, "Size is page aligned");

#if !TARGET_OS_IPHONE
	kern_return_t res = mach_vm_protect(mach_task_self(), reinterpret_cast<mach_vm_address_t>(baseaddr), size, false,
		MachProt(mode));
	if (res != KERN_SUCCESS) [[unlikely]]
	{
		ERROR_LOG("mach_vm_protect() failed: {}", res);
		pxFailRel("mach_vm_protect() failed");
	}
#else
	// [iter74] iOS/Simulator: mprotect only, no Console.WriteLn (not async-signal-safe).
	// [iter74b] @@MPROTECT_PROBE@@ – log first 12 calls with result/errno for diagnosis.
	// Removal condition: SIGBUS loopの有無が確定したらdelete。
	int prot = 0;
	if (mode.CanRead()) prot |= PROT_READ;
	if (mode.CanWrite()) prot |= PROT_WRITE;
	if (mode.CanExecute()) prot |= PROT_EXEC;
	int ret = mprotect(baseaddr, size, prot);
	// [iter74b] @@MPROTECT_RW@@ – log ONLY prot=3 (RW-restore) calls in fastmem range.
	// This is the call that must succeed from the signal handler to break the SIGBUS loop.
	// Removal condition: SIGBUS loopの有無after determined。
	{
		uptr addr_val = (uptr)baseaddr;
		if (prot == (PROT_READ|PROT_WRITE) && addr_val >= 0x300000000ULL && addr_val <= 0x3ffffffffULL)
		{
			static u32 s_rw_n = 0;
			if (s_rw_n < 8)
			{
				int saved_errno = (ret != 0) ? errno : 0;
				char buf[192];
				int nn = snprintf(buf, sizeof(buf),
					"@@MPROTECT_RW@@ n=%u addr=%p prot=%d ret=%d err=%d\n",
					s_rw_n, baseaddr, prot, ret, saved_errno);
				write(STDERR_FILENO, buf, nn);
				s_rw_n++;
			}
		}
	}
#endif
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
	return {};
}

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
#if !TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
	mach_vm_size_t vm_size = size;
	mach_port_t port;
	const kern_return_t res = mach_make_memory_entry_64(
		mach_task_self(), &vm_size, 0, MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &port, MACH_PORT_NULL);
	if (res != KERN_SUCCESS)
	{
		ERROR_LOG("mach_make_memory_entry_64() failed: {}", res);
		return nullptr;
	}

	return reinterpret_cast<void*>(static_cast<uintptr_t>(port));
#else
	return (void*)0xDEADBEEF; // Return dummy handle to bypass check in SysMemory::AllocateMemoryMap
#endif
}

void HostSys::DestroySharedMemory(void* ptr)
{
#if !TARGET_OS_IPHONE
	mach_port_deallocate(mach_task_self(), static_cast<mach_port_t>(reinterpret_cast<uintptr_t>(ptr)));
#endif
}

void* HostSys::MapSharedMemory(void* handle, size_t offset, void* baseaddr, size_t size, const PageProtectionMode& mode)
{
#if !TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
	// If baseaddr is null, use VM_FLAGS_ANYWHERE, otherwise use VM_FLAGS_FIXED/OVERWRITE
	int flags = baseaddr ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE;
	mach_vm_address_t ptr = reinterpret_cast<mach_vm_address_t>(baseaddr);
	
	const kern_return_t res = mach_vm_map(mach_task_self(), &ptr, size, 0, flags,
		static_cast<mach_port_t>(reinterpret_cast<uintptr_t>(handle)), offset, FALSE,
		MachProt(mode), VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (res != KERN_SUCCESS)
	{
		ERROR_LOG("mach_vm_map() failed: {} (base={} size={})", res, baseaddr, size);
		return nullptr;
	}

	return reinterpret_cast<void*>(ptr);
#else
    if (handle == (void*)0xDEADBEEF)
    {
        void* ptr = Mmap(baseaddr, size, mode);
        // [P47] Save base for fastmem remap source (first call = data memory)
        if (ptr && !s_ios_shared_data_base)
            s_ios_shared_data_base = static_cast<u8*>(ptr);
        return ptr;
    }
	return nullptr;
#endif
}

void HostSys::UnmapSharedMemory(void* baseaddr, size_t size)
{
#if !TARGET_OS_IPHONE
	const kern_return_t res = mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(baseaddr), size);
	if (res != KERN_SUCCESS)
		pxFailRel("Failed to unmap shared memory");
#else
	munmap(baseaddr, size);
#endif
}

#ifdef _M_ARM64

#ifdef TARGET_OS_IPHONE
#include <libkern/OSCacheControl.h>
#else
#include <libkern/OSCacheControl.h>
#endif

void HostSys::FlushInstructionCache(void* address, u32 size)
{
    // sys_icache_invalidate is the Apple-supported API (libkern/OSCacheControl.h)
    // Works on both Simulator (Rosetta) and real device (ARM64 native)
    sys_icache_invalidate(address, size);

    // [iter238b] FlushInstructionCache log は env iPSX2_DEBUG_VERBOSE=1 で復活、
    // release default suppress (= cap=20 でも release では noise になるため)。
    // common/ は pcsx2/Common.h を include しないので直接 getenv 経由 cached check。
    static int s_dbg = -1;
    if (s_dbg < 0) s_dbg = (std::getenv("iPSX2_DEBUG_VERBOSE") && std::getenv("iPSX2_DEBUG_VERBOSE")[0] == '1') ? 1 : 0;
    if (s_dbg)
    {
        static int s_ficache_n = 0;
        if (s_ficache_n++ < 20)
            Console.WriteLn("DEBUG: FlushInstructionCache(sys_icache_invalidate) Addr=%p Size=0x%x", address, size);
    }
}

#endif

// [P42] csops — private but stable syscall for code signing status
extern "C" int csops(pid_t pid, unsigned int ops, void* useraddr, size_t usersize);

// [P43] JitMode detection and dual-mapping globals
static DarwinMisc::JitMode s_jit_mode = DarwinMisc::JitMode::Simulator;
static bool s_jit_mode_detected = false;
ptrdiff_t DarwinMisc::g_code_rw_offset = 0;
uintptr_t DarwinMisc::g_code_rw_base = 0;
size_t    DarwinMisc::g_code_rw_size = 0;

// Track Legacy code region for mprotect toggle
static void* s_legacy_code_base = nullptr;
static size_t s_legacy_code_size = 0;
// [P49] Lazy toggle state: avoid mprotect per compile block
static bool s_legacy_is_writable = true;  // starts RW after mmap
static bool s_legacy_code_dirty = false;   // code written since last RX flip

#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
static bool HasTXM()
{
    // TXM devices have this firmware file (A15+ chips)
    // Use glob to find it under any UUID/hash subdirectory
    glob_t g = {};
    int ret = glob("/System/Volumes/Preboot/*/boot/*/usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4",
                    GLOB_NOSORT, nullptr, &g);
    bool found = (ret == 0 && g.gl_pathc > 0);
    globfree(&g);
    return found;
}
#endif

DarwinMisc::JitMode DarwinMisc::DetectJitMode()
{
#if TARGET_OS_SIMULATOR
    s_jit_mode = JitMode::Simulator;
#elif TARGET_OS_IPHONE
    // [P52] Check iOS product version via sysctl (kern.osproductversion).
    // kern.osrelease (Darwin version) cannot distinguish iOS 18.2 (25.x) from iOS 26 (25.x).
    // kern.osproductversion returns "18.5", "26.0" etc.
    {
        char buf[64] = {};
        size_t len = sizeof(buf);
        sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0);
        int major = atoi(buf); // "26.0" -> 26, "18.5" -> 18
        fprintf(stderr, "@@JIT_DETECT@@ kern.osproductversion=%s major=%d\n", buf, major);
        if (major >= 26) {
            s_jit_mode = JitMode::LuckTXM;
        } else {
            s_jit_mode = JitMode::Legacy;
        }
    }
#else
    s_jit_mode = JitMode::Simulator; // macOS
#endif

    // Allow env var override for testing
    if (const char* env = getenv("iPSX2_FORCE_DUAL_MAP")) {
        if (atoi(env) == 1)
            s_jit_mode = JitMode::LuckNoTXM; // Test dual-mapping on Simulator
    }

    s_jit_mode_detected = true;

    const char* names[] = { "Simulator", "LuckTXM", "LuckNoTXM", "Legacy" };
    fprintf(stderr, "@@JIT_MODE@@ mode=%s (%d)\n", names[(int)s_jit_mode], (int)s_jit_mode);

    return s_jit_mode;
}

DarwinMisc::JitMode DarwinMisc::GetJitMode()
{
    if (!s_jit_mode_detected)
        DetectJitMode();
    return s_jit_mode;
}

// [P43] Allocate executable code memory with dual-mapping for iOS 26
void* DarwinMisc::MmapCodeDualMap(size_t size)
{
    JitMode mode = GetJitMode();

    // Note: fastmem is auto-disabled in vtlb_Core_Alloc if 4GB allocation fails.
    // No pre-check needed here.

    if (mode == JitMode::Simulator) {
        // Simulator: use MAP_JIT as before
        void* res = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (res == MAP_FAILED) {
            fprintf(stderr, "@@JIT_ALLOC@@ Simulator MAP_JIT FAIL errno=%d\n", errno);
            return nullptr;
        }
        g_code_rw_offset = 0; // same pointer for read/write/execute
        fprintf(stderr, "@@JIT_ALLOC@@ Simulator MAP_JIT OK rx=%p size=0x%zx offset=0\n", res, size);
        return res;
    }

    if (mode == JitMode::Legacy) {
        // [P49] iOS 18: try MAP_JIT first — works with CS_DEBUGGED on pre-TXM devices.
        // MAP_JIT + pthread_jit_write_protect_np is faster and safer than mprotect toggle.
        void* jit_ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (jit_ptr != MAP_FAILED) {
            g_code_rw_offset = 0;
            s_jit_mode = JitMode::Simulator; // Promote to Simulator mode (pthread_jit_write_protect_np)
            fprintf(stderr, "@@JIT_ALLOC@@ Legacy->Simulator MAP_JIT OK ptr=%p size=0x%zx offset=0\n", jit_ptr, size);
            return jit_ptr;
        }
        fprintf(stderr, "@@JIT_ALLOC@@ MAP_JIT failed errno=%d, falling back to mprotect Legacy\n", errno);

        // Fallback: single mapping with mprotect toggle (PPSSPP-style)
        // Start as RW. EndCodeWrite() will flip to RX before execution.
        void* res = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
        if (res == MAP_FAILED) {
            fprintf(stderr, "@@JIT_ALLOC@@ Legacy mmap(RW) FAIL errno=%d\n", errno);
            return nullptr;
        }

        g_code_rw_offset = 0; // same pointer for read/write/execute
        s_legacy_code_base = res;
        s_legacy_code_size = size;
        fprintf(stderr, "@@JIT_ALLOC@@ Legacy mprotect OK ptr=%p size=0x%zx offset=0\n", res, size);
        return res;
    }

    // --- Try MAP_JIT first (works when CS_DEBUGGED set by LLDB/Xcode) ---
    // On TXM devices with a real debugger attached, MAP_JIT succeeds and
    // no brk #0x69 / StikDebug is needed. Falls back to dual-mapping if MAP_JIT fails.
    if (mode == JitMode::LuckTXM || mode == JitMode::LuckNoTXM) {
        void* jit_ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (jit_ptr != MAP_FAILED) {
            g_code_rw_offset = 0;
            s_jit_mode = JitMode::Simulator; // Same W^X toggle (pthread_jit_write_protect_np)
            fprintf(stderr, "@@JIT_ALLOC@@ MAP_JIT OK (debugger mode) ptr=%p size=0x%zx offset=0\n", jit_ptr, size);
            return jit_ptr;
        }
        fprintf(stderr, "@@JIT_ALLOC@@ MAP_JIT errno=%d, falling back to dual-map+brk\n", errno);
    }

    // --- Dual-mapping path (LuckTXM / LuckNoTXM) ---

    // Step 1: Allocate RX region
    void* rx_ptr = mmap(nullptr, size, PROT_READ | PROT_EXEC,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
    if (rx_ptr == MAP_FAILED) {
        fprintf(stderr, "@@JIT_ALLOC@@ dual-map mmap(RX) FAIL errno=%d\n", errno);
        return nullptr;
    }
    fprintf(stderr, "@@JIT_ALLOC@@ dual-map mmap(RX) OK rx=%p size=0x%zx\n", rx_ptr, size);

    // Step 2: TXM page registration via brk #0x69 (only for LuckTXM)
    // [P45-2] DolphiniOS approach: issue brk #0x69 with x0=addr, x1=size.
    // StikDebug's iPSX2.js script catches this via send_command("c"),
    // calls prepare_memory_region() for TXM page registration, then advances PC.
    // SIGTRAP handler is a safety net — if StikDebug isn't handling brk,
    // we catch SIGTRAP instead of crashing (unlike DolphiniOS which crashes).
    if (mode == JitMode::LuckTXM) {
        static sigjmp_buf s_alloc_brk_jmp;
        struct sigaction sa_brk = {}, sa_brk_old = {};
        sa_brk.sa_handler = +[](int) { siglongjmp(s_alloc_brk_jmp, 1); };
        sa_brk.sa_flags = 0;
        sigemptyset(&sa_brk.sa_mask);
        sigaction(SIGTRAP, &sa_brk, &sa_brk_old);

        bool brk_ok = false;
        fprintf(stderr, "@@JIT_ALLOC@@ issuing brk #0x69 for TXM registration (x0=%p x1=0x%zx)...\n", rx_ptr, size);
        if (sigsetjmp(s_alloc_brk_jmp, 1) == 0) {
            asm volatile("mov x0, %0\n"
                         "mov x1, %1\n"
                         "brk #0x69"
                         :: "r"(rx_ptr), "r"(size) : "x0", "x1");
            fprintf(stderr, "@@JIT_ALLOC@@ brk #0x69 OK (StikDebug handled)\n");
            brk_ok = true;
        } else {
            fprintf(stderr, "@@JIT_ALLOC@@ brk #0x69 SIGTRAP — StikDebug not handling brk\n");
        }
        sigaction(SIGTRAP, &sa_brk_old, NULL);

        if (!brk_ok) {
            fprintf(stderr, "@@JIT_ALLOC@@ FATAL: TXM registration failed — ensure StikDebug launched iPSX2\n");
            munmap(rx_ptr, size);
            return nullptr;
        }
    }

    // Step 3: Create RW alias via vm_remap
    vm_address_t rw_region = 0;
    vm_address_t target = reinterpret_cast<vm_address_t>(rx_ptr);
    vm_prot_t cur_protection = 0;
    vm_prot_t max_protection = 0;

    kern_return_t kr = vm_remap(mach_task_self(), &rw_region, size, 0,
                                VM_FLAGS_ANYWHERE, mach_task_self(), target, false,
                                &cur_protection, &max_protection, VM_INHERIT_DEFAULT);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "@@JIT_ALLOC@@ vm_remap FAIL kr=%d\n", kr);
        munmap(rx_ptr, size);
        return nullptr;
    }

    u8* rw_ptr = reinterpret_cast<u8*>(rw_region);
    fprintf(stderr, "@@JIT_ALLOC@@ vm_remap OK rw=%p\n", rw_ptr);

    // Step 4: Make RW region writable
    if (mprotect(rw_ptr, size, PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "@@JIT_ALLOC@@ mprotect(RW) FAIL errno=%d\n", errno);
        vm_deallocate(mach_task_self(), rw_region, size);
        munmap(rx_ptr, size);
        return nullptr;
    }

    // Step 5: Set global offset
    g_code_rw_offset = rw_ptr - static_cast<u8*>(rx_ptr);
    g_code_rw_base = reinterpret_cast<uintptr_t>(rw_ptr);
    g_code_rw_size = size;

    fprintf(stderr, "@@JIT_ALLOC@@ dual-map COMPLETE rx=%p rw=%p offset=%td size=0x%zx\n",
            rx_ptr, rw_ptr, g_code_rw_offset, size);
    return rx_ptr; // Return RX pointer
}

void DarwinMisc::MunmapCodeDualMap(void* rx_ptr, size_t size)
{
    if (!rx_ptr)
        return;

    // Deallocate RW mapping if it exists (dual-mapping path)
    if (g_code_rw_base) {
        vm_deallocate(mach_task_self(), static_cast<vm_address_t>(g_code_rw_base), g_code_rw_size);
        g_code_rw_base = 0;
        g_code_rw_size = 0;
    }

    // Clear Legacy tracking
    if (s_legacy_code_base) {
        s_legacy_code_base = nullptr;
        s_legacy_code_size = 0;
    }

    munmap(rx_ptr, size);
    g_code_rw_offset = 0;
}

// [P45-2] JIT availability — CS_DEBUGGED only (DolphiniOS approach)
// No P_TRACED polling, no URL scheme, no probe brk.
// StikDebug launches iPSX2 suspended, sets CS_DEBUGGED, runs iPSX2.js script.
// The actual brk #0x69 happens later in MmapCodeDualMap().
bool DarwinMisc::IsJITAvailable()
{
#if TARGET_OS_SIMULATOR
    fprintf(stderr, "@@JIT_DETECT@@ simulator=1 result=AVAILABLE\n");
    return true;
#elif TARGET_OS_IPHONE
    // CS_DEBUGGED (0x10000000) is a sticky flag set when a debugger has ever
    // attached to this process. StikDebug sets this when it launches the app
    // suspended. Unlike P_TRACED, this flag persists after debugger detach.
    uint32_t cs_flags = 0;
    int rv = csops(getpid(), 0, &cs_flags, sizeof(cs_flags));
    bool cs_debugged = (rv == 0) && (cs_flags & 0x10000000u);

    fprintf(stderr, "@@JIT_DETECT@@ csops=%d cs_flags=0x%08x CS_DEBUGGED=%d\n",
            rv, cs_flags, cs_debugged ? 1 : 0);

    if (!cs_debugged) {
        fprintf(stderr, "@@JIT_DETECT@@ result=UNAVAILABLE (no CS_DEBUGGED — launch via StikDebug)\n");
        s_jit_mode = JitMode::Legacy;
        s_jit_mode_detected = true;
        return false;
    }

    fprintf(stderr, "@@JIT_DETECT@@ result=AVAILABLE (CS_DEBUGGED set)\n");
    return true;
#else
    return true;
#endif
}

SharedMemoryMappingArea::SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages)
	: m_base_ptr(base_ptr)
	, m_size(size)
	, m_num_pages(num_pages)
{
}

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
	pxAssertRel(m_num_mappings == 0, "No mappings left");
#if !TARGET_OS_IPHONE
	if (mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(m_base_ptr), m_size) != KERN_SUCCESS)
		pxFailRel("Failed to release shared memory area");
#else
	munmap(m_base_ptr, m_size);
#endif
}


std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size)
{
	pxAssertRel(Common::IsAlignedPow2(size, __pagesize), "Size is page aligned");

#if !TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
	mach_vm_address_t alloc = 0;
	// Use VM_PROT_DEFAULT (Read/Write) to ensure memory is accessible immediately
	const kern_return_t res =
		mach_vm_map(mach_task_self(), &alloc, size, 0, VM_FLAGS_ANYWHERE,
			MEMORY_OBJECT_NULL, 0, false, VM_PROT_DEFAULT, VM_PROT_DEFAULT, VM_INHERIT_NONE);
	if (res != KERN_SUCCESS)
	{
		ERROR_LOG("mach_vm_map() failed: {}", res);
		return {};
	}

	return std::unique_ptr<SharedMemoryMappingArea>(new SharedMemoryMappingArea(reinterpret_cast<u8*>(alloc), size, size / __pagesize));
#else
	// Matches VTLB_VMAP_ITEMS
	// On iOS, we can't use mach_vm_map easily to reserve uncommitted space, so we use mmap with PROT_NONE
	// or PROT_READ|PROT_WRITE depending on need. For Fastmem, we need to be able to map over it.
	void* ptr = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (ptr == MAP_FAILED)
		return {};

	return std::unique_ptr<SharedMemoryMappingArea>(new SharedMemoryMappingArea(static_cast<u8*>(ptr), size, size / __pagesize));
#endif
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode)
{
	pxAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

#if !TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
	const kern_return_t res =
		mach_vm_map(mach_task_self(), reinterpret_cast<mach_vm_address_t*>(&map_base), map_size, 0, VM_FLAGS_OVERWRITE,
			static_cast<mach_port_t>(reinterpret_cast<uintptr_t>(file_handle)), file_offset, false,
			MachProt(mode), VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (res != KERN_SUCCESS) [[unlikely]]
	{
		ERROR_LOG("mach_vm_map() failed in Map: {}", res);
		return nullptr;
	}

	m_num_mappings++;
	return static_cast<u8*>(map_base);
#else
	// [P47] Real iOS device: use mach_vm_remap to share physical pages with eeMem.
	// Previously used mmap(MAP_ANON) which created independent pages — fastmem reads
	// returned zeros instead of actual PS2 memory contents.
	if (s_ios_shared_data_base)
	{
		mach_vm_address_t target = (mach_vm_address_t)map_base;
		mach_vm_address_t source = (mach_vm_address_t)(s_ios_shared_data_base + file_offset);
		vm_prot_t cur_prot, max_prot;
		kern_return_t kr = mach_vm_remap(
			mach_task_self(), &target, map_size, 0,
			VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
			mach_task_self(), source,
			FALSE, // copy=FALSE: share same physical pages
			&cur_prot, &max_prot, VM_INHERIT_NONE);
		if (kr == KERN_SUCCESS)
		{
			m_num_mappings++;
			return static_cast<u8*>(map_base);
		}
		ERROR_LOG("mach_vm_remap() failed: {} (target={} source={} size={})",
			kr, (void*)target, (void*)source, map_size);
	}

	// Fallback: anonymous mapping (fastmem will not work correctly)
	int prot = 0;
	if (mode.CanRead())  prot |= PROT_READ;
	if (mode.CanWrite()) prot |= PROT_WRITE;
	void* res = mmap(map_base, map_size, prot, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	if (res == MAP_FAILED) [[unlikely]]
		return nullptr;

	m_num_mappings++;
	return static_cast<u8*>(res);
#endif
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size)
{
	pxAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

#if !TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
	const kern_return_t res =
		mach_vm_map(mach_task_self(), reinterpret_cast<mach_vm_address_t*>(&map_base), map_size, 0, VM_FLAGS_OVERWRITE,
			MEMORY_OBJECT_NULL, 0, false, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_NONE);
	if (res != KERN_SUCCESS) [[unlikely]]
	{
		ERROR_LOG("mach_vm_map() failed: {}", res);
		return false;
	}

	m_num_mappings--;
	return true;
#else
	// Real iOS device: remap as PROT_NONE to release physical pages but keep reservation
	void* res = mmap(map_base, map_size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	if (res == MAP_FAILED) [[unlikely]]
		return false;

	m_num_mappings--;
	return true;
#endif
}

#ifdef _M_ARM64

static thread_local int s_code_write_depth = 0;

void HostSys::BeginCodeWrite()
{
	if ((s_code_write_depth++) == 0)
	{
        // [iPSX2] Trace BeginCodeWrite
        if (DarwinMisc::iPSX2_WX_TRACE) {
            u32 idx = __atomic_fetch_add(&DarwinMisc::g_wx_idx, 1, __ATOMIC_RELAXED) % 16;
            DarwinMisc::g_wx_events[idx].tid = (u64)pthread_self();
            DarwinMisc::g_wx_events[idx].caller = (u64)__builtin_return_address(0);
            DarwinMisc::g_wx_events[idx].write = 1; // RW
            DarwinMisc::g_wx_events[idx].depth = s_code_write_depth;

            
            // Console check is too heavy? No, Console.WriteLn is usually okayish if filtered.
            // But user asked for one-shot throttled logging. We stick to ring buffer for safety?
            // "All logs must be throttled/one-shot".
            // We'll log it directly via Console (it has internal locks, might deadlock if reentrant but this is user space).
             Console.WriteLn("@@WX_TOGGLE@@ tid=%p write=1 depth=%d caller=%p", 
                pthread_self(), s_code_write_depth, __builtin_return_address(0));
            
            DarwinMisc::g_jit_write_state = 1;
            DarwinMisc::g_rec_stage = 0; // Reset rec stage on new write begin?
        } else {
             DarwinMisc::g_jit_write_state = 1;
        }

		// [P43] Mode-dependent W^X toggle
		if (DarwinMisc::g_code_rw_offset != 0) {
			// Dual-mapping: RW view is permanently writable, no toggle needed
		} else if (DarwinMisc::GetJitMode() == DarwinMisc::JitMode::Legacy) {
			// [P49] Legacy: mprotect(RW) only when not already writable (skip redundant syscalls)
			if (s_legacy_code_base && !s_legacy_is_writable) {
				int rv = mprotect(s_legacy_code_base, s_legacy_code_size, PROT_READ | PROT_WRITE);
				if (rv != 0) {
					static int s_begin_fail = 0;
					if (s_begin_fail++ < 5)
						fprintf(stderr, "@@JIT_ALLOC@@ WARN: mprotect(RW) FAIL errno=%d\n", errno);
				}
				s_legacy_is_writable = true;
			}
		} else {
			// Simulator: use pthread_jit_write_protect_np
			static auto func = reinterpret_cast<void(*)(int)>(dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_np"));
			if (func)
				func(0);
		}
	}
}

void HostSys::EndCodeWrite()
{
	pxAssert(s_code_write_depth > 0);
	if ((--s_code_write_depth) == 0)
	{
        DarwinMisc::g_jit_write_state = 0;

		// [P43] Mode-dependent W^X toggle
		if (DarwinMisc::g_code_rw_offset != 0) {
			// Dual-mapping: no toggle needed
		} else if (DarwinMisc::GetJitMode() == DarwinMisc::JitMode::Legacy) {
			// [P49] Legacy: mprotect(RX) to allow execution.
			// Skip redundant syscall if already executable.
			// icache flush is handled by FlushInstructionCache() per-block, not here.
			if (s_legacy_code_base && s_legacy_is_writable && !DarwinMisc::iPSX2_FORCE_EE_INTERP) {
				int rv = mprotect(s_legacy_code_base, s_legacy_code_size, PROT_READ | PROT_EXEC);
				if (rv != 0) {
					static int s_mprotect_fail_count = 0;
					if (s_mprotect_fail_count++ < 5)
						fprintf(stderr, "@@JIT_ALLOC@@ WARN: mprotect(RX) FAIL errno=%d count=%d\n", errno, s_mprotect_fail_count);
				}
				s_legacy_is_writable = false;
			}
		} else {
			// Simulator: use pthread_jit_write_protect_np
			static auto func = reinterpret_cast<void(*)(int)>(dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_np"));
			if (func)
				func(1);
		}
	}
}

// [P49] Legacy lazy toggle: flip RW→RX + icache flush before JIT dispatch.
void DarwinMisc::LegacyEnsureExecutable()
{
	if (!s_legacy_code_base || !s_legacy_is_writable)
		return;

	int rv = mprotect(s_legacy_code_base, s_legacy_code_size, PROT_READ | PROT_EXEC);
	if (rv != 0) {
		static int s_fail = 0;
		if (s_fail++ < 5)
			fprintf(stderr, "@@JIT_ALLOC@@ WARN: LegacyEnsureExecutable mprotect(RX) FAIL errno=%d\n", errno);
		return;
	}
	s_legacy_is_writable = false;

	if (s_legacy_code_dirty) {
		sys_icache_invalidate(s_legacy_code_base, s_legacy_code_size);
		s_legacy_code_dirty = false;
	}
}

[[maybe_unused]] static bool IsStoreInstruction(const void* ptr)
{
	u32 bits;
	std::memcpy(&bits, ptr, sizeof(bits));

	// Based on vixl's disassembler Instruction::IsStore().
	// if (Mask(LoadStoreAnyFMask) != LoadStoreAnyFixed)
	if ((bits & 0x0a000000) != 0x08000000)
		return false;

	// if (Mask(LoadStorePairAnyFMask) == LoadStorePairAnyFixed)
	if ((bits & 0x3a000000) == 0x28000000)
	{
		// return Mask(LoadStorePairLBit) == 0
		return (bits & (1 << 22)) == 0;
	}

	switch (bits & 0xC4C00000)
	{
		case 0x00000000: // STRB_w
		case 0x40000000: // STRH_w
		case 0x80000000: // STR_w
		case 0xC0000000: // STR_x
		case 0x04000000: // STR_b
		case 0x44000000: // STR_h
		case 0x84000000: // STR_s
		case 0xC4000000: // STR_d
		case 0x04800000: // STR_q
			return true;

		default:
			return false;
	}
}

#endif // _M_ARM64

namespace PageFaultHandler
{
	static void SignalHandler(int sig, siginfo_t* info, void* ctx);

	static std::recursive_mutex s_exception_handler_mutex;
	static bool s_in_exception_handler = false;
	static bool s_installed = false;
	// Crash log file descriptor (opened once, never closed for signal safety)
	static int s_crash_log_fd = -1;
	
	// Quarantine Buffer (Ring Buffer)
	struct QuarantineEntry {
		u32 guest_pc;
		u32 block_id;
	};
	static QuarantineEntry s_block_quarantine[16];
	static std::atomic<u32> s_quarantine_idx{0};

	// Async-signal-safe write to STDERR
	static void SafeWriteStr(const char* str)
	{
		size_t len = 0;
		while (str[len]) len++;
		write(STDERR_FILENO, str, len);
		// Also write to crash log file if available
		if (s_crash_log_fd >= 0) {
			write(s_crash_log_fd, str, len);
		}
	}

	static void SafeWriteHex(const char* prefix, u64 val)
	{
		SafeWriteStr(prefix);
		char buf[19];
		buf[0] = '0'; buf[1] = 'x';
		const char* hex = "0123456789ABCDEF";
		for (int i = 0; i < 16; ++i) {
			buf[17 - i] = hex[val & 0xF];
			val >>= 4;
		}
		buf[18] = 0;
		SafeWriteStr(buf);
		SafeWriteStr("\n");
	}



// [iPSX2] JIT Context State
static std::atomic<uintptr_t> s_jit_base{0};
static std::atomic<uintptr_t> s_jit_end{0};
static std::atomic<u32> s_last_guest_pc{0};
static std::atomic<uintptr_t> s_last_rec_ptr{0};

    // [iPSX2] Legacy wrapper to ensure new path is called
    bool Install(Error* error)
    {
        return Install_Fresh(error);
    }
} // namespace PageFaultHandler

// [iPSX2] DarwinMisc JIT Setters

int DarwinMisc::iPSX2_CRASH_DIAG = [](){
    const char* s = getenv("iPSX2_CRASH_DIAG");
    if (s && s[0] == '1') return 1;
    return 0;
}();

int DarwinMisc::iPSX2_WX_TRACE = [](){
    const char* s = getenv("iPSX2_WX_TRACE");
    if (s && s[0] == '1') return 1;
    return 0;
}();

int DarwinMisc::iPSX2_REC_DIAG = [](){
    const char* s = getenv("iPSX2_REC_DIAG");
    if (s && s[0] == '1') return 1;
    return 0;
}();

// [V34] No longer env-driven. Internal flag only — defaults to 0, set to 1 by
// ios_main.mm checkJITAndStartVM on CS_DEBUGGED fallback. Mode switching is now
// controlled exclusively via PCSX2-iOS.ini [EmuCore/CPU] CoreType + Recompiler.
int DarwinMisc::iPSX2_FORCE_EE_INTERP = 0;

int DarwinMisc::iPSX2_FORCE_JIT_VERIFY = [](){
    const char* gate = getenv("iPSX2_ENABLE_DIAG_FLAGS");
    const bool diag_enabled = (gate && gate[0] == '1' && gate[1] == '\0');
    const char* s = getenv("iPSX2_FORCE_JIT_VERIFY");
    int val = (diag_enabled && s && s[0] == '1') ? 1 : 0;
    fprintf(stderr, "@@CFG@@ iPSX2_FORCE_JIT_VERIFY=%d\n", val);
    return val;
}();

int DarwinMisc::iPSX2_CRASH_PACK = [](){
    const char* s = getenv("iPSX2_CRASH_PACK");
    int val = (s && s[0] == '1') ? 1 : 0;
    fprintf(stderr, "@@CFG@@ iPSX2_CRASH_PACK=%d\n", val);
    return val;
}();

int DarwinMisc::iPSX2_CALL_TGT_X9 = [](){
    const char* s = getenv("iPSX2_CALL_TGT_X9");
    if (!s) s = getenv("CALL_TGT_X9");
    int val = (s && s[0] == '1') ? 1 : 0;
    fprintf(stderr, "@@CFG@@ iPSX2_CALL_TGT_X9=%d\n", val);
    return val;
}();

int DarwinMisc::iPSX2_CALLPROBE = [](){
    const char* s = getenv("iPSX2_CALLPROBE");
    int val = (s && s[0] == '1') ? 1 : 0;
    fprintf(stderr, "@@CFG@@ iPSX2_CALLPROBE=%d\n", val);
    return val;
}();

// [P11] iPSX2_JIT_HLE: JIT modeで HLE をenabledにするか (default=1=enabled)
// env var iPSX2_JIT_HLE=0 で JIT の全 HLE をdisabled化 → interpreter と同conditionでboot試験
int DarwinMisc::iPSX2_JIT_HLE = [](){
    const char* s = getenv("iPSX2_JIT_HLE");
    int val = (!s || s[0] != '0') ? 1 : 0; // default=1
    fprintf(stderr, "@@CFG@@ iPSX2_JIT_HLE=%d\n", val);
    return val;
}();

// [iter37] stdout redirect — now handled in ios_main.mm unified logging (L1006-1022).
// On real device, /tmp is outside sandbox. Redirect is done via dup2 in ios_main.mm.
#if TARGET_OS_SIMULATOR
static int s_stdout_redirect = [](){
    freopen("/tmp/pcsx2_stdout.txt", "w", stdout);
    return 0;
}();
#endif

static int s_user_crash_log_fd = -1;
void DarwinMisc::SetCrashLogFD(int fd) {
    s_user_crash_log_fd = fd;
}

// [iPSX2] Safe Image Cache
static struct { const void* addr; const char* name; } s_dyld_imgs[128];
static int s_dyld_cnt = 0;
static void CacheDyldImages() {
    if (s_dyld_cnt > 0) return;
    u32 count = _dyld_image_count();
    for(u32 i=0; i<count && s_dyld_cnt < 128; ++i) {
        const char* name = _dyld_get_image_name(i);
        const auto* hdr = _dyld_get_image_header(i);
        if (hdr) {
            const char* base = strrchr(name, '/');
            s_dyld_imgs[s_dyld_cnt++] = {hdr, base ? base+1 : name};
        }
    }
}

void DarwinMisc::SetJitRange(void* base, size_t size) {
    CacheDyldImages();
    PageFaultHandler::s_jit_base.store(reinterpret_cast<uintptr_t>(base), std::memory_order_relaxed);
    PageFaultHandler::s_jit_end.store(reinterpret_cast<uintptr_t>(base) + size, std::memory_order_relaxed);

    if (iPSX2_CRASH_DIAG) {
        fprintf(stderr, "@@CFG@@ iPSX2_CRASH_DIAG=1\n");
        fprintf(stderr, "@@JIT_RANGE@@ base=%p end=%p\n", base, (void*)((uintptr_t)base + size));
    }

    // One-shot DYLD log
    const struct mach_header* hdr = _dyld_get_image_header(0);
    const char* path = _dyld_get_image_name(0);
    if(hdr) {
        // Direct fprintf to ensure it hits stderr/log immediately
        fprintf(stderr, "@@DYLD_IMG0@@ hdr=%p path=\"%s\"\n", hdr, path ? path : "null");
    }
}

void DarwinMisc::SetLastGuestPC(u32 pc) {
    PageFaultHandler::s_last_guest_pc.store(pc, std::memory_order_relaxed);
}

void DarwinMisc::SetLastRecPtr(void* ptr) {
    PageFaultHandler::s_last_rec_ptr.store(reinterpret_cast<uintptr_t>(ptr), std::memory_order_relaxed);
}

uintptr_t DarwinMisc::GetJitBase() { return PageFaultHandler::s_jit_base.load(std::memory_order_relaxed); }
uintptr_t DarwinMisc::GetJitEnd() { return PageFaultHandler::s_jit_end.load(std::memory_order_relaxed); }
u32 DarwinMisc::GetLastGuestPC() { return PageFaultHandler::s_last_guest_pc.load(std::memory_order_relaxed); }
uintptr_t DarwinMisc::GetLastRecPtr() { return PageFaultHandler::s_last_rec_ptr.load(std::memory_order_relaxed); }

namespace {
    struct JitBlockEntry {
        u32 guest_pc;
        uintptr_t recptr;
        u32 size;
    };
    static JitBlockEntry s_jit_blocks[64];
    static std::atomic<int> s_jit_block_idx{0};
}

void DarwinMisc::RecordJitBlock(u32 guest_pc, void* recptr, u32 size) {
    int idx = s_jit_block_idx.fetch_add(1, std::memory_order_relaxed);
    int slot = idx % 64;
    s_jit_blocks[slot].guest_pc = guest_pc;
    s_jit_blocks[slot].recptr = (uintptr_t)recptr;
    s_jit_blocks[slot].size = (size == 0) ? 0x4000 : size; 
}

bool DarwinMisc::FindJitBlock(uintptr_t site, u32* out_guest_pc, void** out_recptr) {
    int idx = s_jit_block_idx.load(std::memory_order_relaxed);
    // Scan backwards
    for (int i = 0; i < 64; i++) {
        int slot = (idx - 1 - i) % 64;
        if (slot < 0) slot += 64; 
        
        uintptr_t r = s_jit_blocks[slot].recptr;
        u32 sz = s_jit_blocks[slot].size;
        if (r != 0 && site >= r && site < (r + sz)) {
            if (out_guest_pc) *out_guest_pc = s_jit_blocks[slot].guest_pc;
            if (out_recptr) *out_recptr = (void*)r;
            return true;
        }
    }
    // Fallback: finding nearest preceding block if exact range fails?
    // Let's rely on range for now. If size is 0x4000 it catches most.
    return false;
}

void PageFaultHandler::SignalHandler(int sig, siginfo_t* info, void* ctx)
{
#if defined(_M_X86)
	void* const exception_address =
		reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__faultvaddr);
	void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__rip);
	const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__err & 2) != 0;
    uintptr_t host_sp = static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__rsp;
    uintptr_t host_lr = 0; // No LR on x86
    uintptr_t reg_x27 = 0; // [iter84] No RSTATE_CPU on x86
    uintptr_t reg_x25 = 0; // [iter86] RFASTMEMBASE on x86 (N/A)
    uintptr_t reg_x4  = 0; // [iter86]
    uintptr_t reg_x5  = 0; // [iter86]
#elif defined(_M_ARM64)
	void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__far);
	void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__pc);
	// [P15] ESR WnR ビットで is_write determine。IsStoreInstruction(exception_pc) は
	// exception_pc が未マップの場合に二重フォルトをcauseスレッドがhangする。
	// ESR bit 6 (WnR): 1=write, 0=read。EC=0x24/0x25 (data abort) の場合のみenabled。
	// Removal condition: なし（恒久fix）
	const uint32_t esr_early = static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__esr;
	const uint32_t ec_early = (esr_early >> 26) & 0x3F;
	bool is_write;
	if (ec_early == 0x24 || ec_early == 0x25) {
		// Data abort: WnR bit is reliable
		is_write = (esr_early & (1u << 6)) != 0;
	} else {
		// Instruction fetch fault or other: not a write, and IsStoreInstruction
		// would try to read from potentially unmapped exception_pc
		is_write = false;
	}
    auto* ss = &static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss;
    uintptr_t host_sp = ss->__sp;
    uintptr_t host_lr = ss->__lr;
    // Capture critical registers for indirect call debugging (BLR x16/x17) and args (x0-x3)
    uintptr_t reg_x16 = ss->__x[16];
    uintptr_t reg_x17 = ss->__x[17];
    uintptr_t reg_x0  = ss->__x[0];
    uintptr_t reg_x1  = ss->__x[1];
    uintptr_t reg_x2  = ss->__x[2];
    uintptr_t reg_x3  = ss->__x[3];
    uintptr_t reg_x4  = ss->__x[4];  // [iter86] potential EA register
    uintptr_t reg_x5  = ss->__x[5];  // [iter86] potential EA register
    uintptr_t reg_x25 = ss->__x[25]; // [iter86] RFASTMEMBASE
    uintptr_t reg_x27 = ss->__x[27]; // [iter84] RSTATE_CPU = cpuRegs base
    // Capture ESR for abort classification
    uint32_t esr = static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__esr;
#endif

    uintptr_t pc_val = reinterpret_cast<uintptr_t>(exception_pc);
    uintptr_t fault_val = reinterpret_cast<uintptr_t>(exception_address);
    int si_code = info->si_code;
    
    // [iPSX2] Extract LR/SP early: REMOVED (Duplicate)


    	// --- Crash Logging (async-signal-safe) ---
	// [P52] Suppress verbose telemetry after first few W^X SIGBUS events.
	// Normal W^X flow generates many SIGBUS — only log first 3 in detail.
	static volatile int s_sigbus_log_count = 0;
	bool verbose_signal_log = true;
	if (sig == SIGBUS) {
		int n = __sync_fetch_and_add(&s_sigbus_log_count, 1);
		if (n >= 3) verbose_signal_log = false;
	}

	if (verbose_signal_log)
	SafeWriteStr("\n!!! SIGNAL CAUGHT !!!\n");

	if (verbose_signal_log) {
    // [iter37] SIGABRT backtrace: raw addresses for atos symbolication.
    // Removal condition: SIGABRT root cause (abort() call元) after identified。
    if (sig == SIGABRT) {
        SafeWriteStr("@@SIGABRT_BT_START@@\n");
        void* bt_frames[48];
        int bt_cnt = backtrace(bt_frames, 48);
        SafeWriteStr("@@SIGABRT_BT_CNT=");
        { char nc[4]; int v=bt_cnt; nc[0]='0'+v/10; nc[1]='0'+v%10; nc[2]='\n'; nc[3]=0; SafeWriteStr(nc); }
        for (int i = 0; i < bt_cnt; i++) {
            SafeWriteStr("@@BT@@");
            char b[19]; b[0]='0'; b[1]='x';
            const char* h="0123456789abcdef"; uintptr_t v=(uintptr_t)bt_frames[i];
            for(int j=0;j<16;j++){b[17-j]=h[v&0xF];v>>=4;} b[18]=0;
            SafeWriteStr(b); SafeWriteStr("\n");
        }
        SafeWriteStr("@@SIGABRT_BT_END@@\n");
    }
    
    // OUTPUT FORMAT: @@SIGBUS@@ pc=0x... lr=0x... sp=0x... addr=0x... code=...
    auto write_hex_val = [](const char* p, uintptr_t v) {
        SafeWriteStr(p);
        char b[17];
        const char* h = "0123456789abcdef";
        for(int i=0; i<8; ++i) { // 64-bit
            b[2*i] = h[(v >> ((7-i)*8 + 4)) & 0xF];
            b[2*i+1] = h[(v >> ((7-i)*8)) & 0xF];
        }
        b[16] = 0;
        SafeWriteStr(b);
    };

    // [iPSX2] Safe Read Helper (vm_read_overwrite for Simulator/iOS)
    auto SafeRead32 = [](uintptr_t addr, uint32_t* out) -> bool {
        vm_size_t size = 4;
        vm_address_t data = (vm_address_t)out;
        kern_return_t kr = vm_read_overwrite(mach_task_self(), (vm_address_t)addr, 4, data, &size);
        return (kr == KERN_SUCCESS);
    };
    auto SafeRead64 = [](uintptr_t addr, uint64_t* out) -> bool {
        vm_size_t size = 8;
        vm_address_t data = (vm_address_t)out;
        kern_return_t kr = vm_read_overwrite(mach_task_self(), (vm_address_t)addr, 8, data, &size);
        return (kr == KERN_SUCCESS);
    };

    // [iter84] @@SIGBUS_EE_REGS@@ – EE $ra/$v1/$sp/pc at SIGBUS time via RSTATE_CPU (x27)
    // [iter86] ARM64オペコード + x25(RFASTMEMBASE) + x0-x5 add読み取り
    // Removal condition: SW delay slot EE VA=0 JITバグ root cause after identified
    {
        static u32 s_ser_n = 0;
        if (sig == SIGBUS && s_ser_n < 4 && reg_x27 != 0) {
            u32 ee_ra = 0, ee_v1 = 0, ee_sp = 0, ee_pc_v = 0;
            SafeRead32(reg_x27 + 0x1F0, &ee_ra);   // GPR.r[31] ($ra) lower 32b
            SafeRead32(reg_x27 + 0x030, &ee_v1);   // GPR.r[3]  ($v1) lower 32b
            SafeRead32(reg_x27 + 0x1D0, &ee_sp);   // GPR.r[29] ($sp) lower 32b
            SafeRead32(reg_x27 + 0x2A8, &ee_pc_v); // cpuRegs.pc
            char buf[192];
            int nn = snprintf(buf, sizeof(buf),
                "@@SIGBUS_EE_REGS@@ n=%u fa=%08lx ra=%08x v1=%08x sp=%08x eepc=%08x\n",
                s_ser_n, (unsigned long)fault_val, ee_ra, ee_v1, ee_sp, ee_pc_v);
            SafeWriteStr(buf);
            // [iter86] ARM64 faulting opcode + ARM64 register dump
            u32 arm64_op = 0;
            SafeRead32(pc_val, &arm64_op);  // ARM64 opcode at faulting instruction
            char buf2[256];
            int nn2 = snprintf(buf2, sizeof(buf2),
                "@@SIGBUS_ARM64@@ op=%08x x25=%016lx x0=%08lx x1=%08lx x2=%08lx x3=%08lx x4=%08lx x5=%08lx\n",
                arm64_op, (unsigned long)reg_x25,
                (unsigned long)reg_x0, (unsigned long)reg_x1,
                (unsigned long)reg_x2, (unsigned long)reg_x3,
                (unsigned long)reg_x4, (unsigned long)reg_x5);
            SafeWriteStr(buf2);
            s_ser_n++;
        }
    }

    // [iPSX2] CRASH TELEMETRY (Async-Signal-Safe)
    SafeWriteStr("@@BUILD_TS@@ 2026-01-18 13:40 RUN_SIGBUS_DEBUG\n");
    SafeWriteStr("@@SIGBUS_START@@\n");
    SafeWriteStr("@@SIGBUS@@"); 
    write_hex_val(" host_pc=", pc_val);
    write_hex_val(" host_sp=", (uintptr_t)host_sp);
    write_hex_val(" host_lr=", host_lr);
    write_hex_val(" fault=", fault_val);
    SafeWriteStr("\n");
    SafeWriteStr("@@TELEM_VER@@ v=2\n");
 
    SafeWriteStr("@@CheckJIT@@");
    uintptr_t jbase = s_jit_base.load(std::memory_order_relaxed);
    uintptr_t jend = s_jit_end.load(std::memory_order_relaxed);
    write_hex_val(" jbase=", jbase);
    write_hex_val(" jend=", jend);
    write_hex_val(" lr=", host_lr);
    SafeWriteStr("\n");

    // @@LR_INSN@@ probe
    {
         uint32_t val_m4 = 0;
         uint64_t val_m8 = 0;
         bool read_ok_m4 = false;
         bool read_ok_m8 = false;
         
         if (host_lr > 0x1000) {
             read_ok_m4 = SafeRead32(host_lr - 4, &val_m4);
             read_ok_m8 = SafeRead64(host_lr - 8, &val_m8);
         }

         SafeWriteStr("@@LR_INSN@@");
         write_hex_val(" lr=0x", host_lr);
         if (read_ok_m4 && read_ok_m8) {
             write_hex_val(" w_m8=0x", val_m8);
             write_hex_val(" w_m4=0x", val_m4);
             SafeWriteStr(" unreadable=0\n");
         } else {
             write_hex_val(" w_m8=0x", val_m8);
             write_hex_val(" w_m4=0x", val_m4);
             SafeWriteStr(" unreadable=1\n");
         }
    }
    
    // @@STACK16@@ probe
    {
        SafeWriteStr("@@STACK16@@ sp=0x");
        auto write_hex_raw = [&](uintptr_t v) {
             char b[17];
             const char* h = "0123456789abcdef";
             for(int i=0; i<8; ++i) { 
                 b[2*i] = h[(v >> ((7-i)*8 + 4)) & 0xF];
                 b[2*i+1] = h[(v >> ((7-i)*8)) & 0xF];
             }
             b[16]=0;
             SafeWriteStr(b);
        };
        write_hex_raw((uintptr_t)host_sp);
        
        uintptr_t* sp_ptr = (uintptr_t*)host_sp;
        for(int i=0; i<16; ++i) {
            uint64_t val = 0;
            // Safe read from stack (it might be invalid)
            if (SafeRead64((uintptr_t)(sp_ptr + i), &val)) {
                SafeWriteStr(" w");
                char ib[4]; 
                if(i<10) { ib[0]='0'+i; ib[1]=0; }
                else { ib[0]='1'; ib[1]='0'+(i-10); ib[2]=0; }
                SafeWriteStr(ib);
                SafeWriteStr("=0x");
                write_hex_raw(val);
            } else {
                SafeWriteStr(" w");
                char ib[4];
                if(i<10) { ib[0]='0'+i; ib[1]=0; }
                else { ib[0]='1'; ib[1]='0'+(i-10); ib[2]=0; }
                SafeWriteStr(ib);
                SafeWriteStr("=UNREAD");
            }
        }
        SafeWriteStr("\n");
    }

    // @@SYM_PC@@ / @@SYM_LR@@
    {
        auto SymbolizeStrict = [&](const char* tag, uintptr_t addr) {
            SafeWriteStr(tag);
            SafeWriteStr(" mod=\"");
            const char* best_name = "???";
            uintptr_t best_base = 0;
            for(int i=0; i<s_dyld_cnt; ++i) {
                uintptr_t base = (uintptr_t)s_dyld_imgs[i].addr;
                if (addr >= base && base > best_base) {
                    best_base = base;
                    best_name = s_dyld_imgs[i].name;
                }
            }
            SafeWriteStr(best_name);
            SafeWriteStr("\" off=0x");
            uintptr_t off = addr - best_base;
            char b[17];
            const char* h = "0123456789abcdef";
            for(int i=0; i<8; ++i) { 
                b[2*i] = h[(off >> ((7-i)*8 + 4)) & 0xF];
                b[2*i+1] = h[(off >> ((7-i)*8)) & 0xF];
            }
            b[16]=0;
            SafeWriteStr(b);
            SafeWriteStr(" name=\"?\"\n"); 
        };
        SymbolizeStrict("@@SYM_LR@@", host_lr);
        SymbolizeStrict("@@SYM_PC@@", pc_val);
    }
    
    SafeWriteStr("@@REGS@@");
    write_hex_val(" x0=", reg_x0);
    write_hex_val(" x1=", reg_x1);
    write_hex_val(" x2=", reg_x2);
    write_hex_val(" x3=", reg_x3);
    write_hex_val(" x9=", ss->__x[9]);
    write_hex_val(" x16=", reg_x16);
    write_hex_val(" x17=", reg_x17);
    write_hex_val(" x19=", ss->__x[19]);
    SafeWriteStr("\n");

    // [TEMP_DIAG] @@EE_PC_CRASH@@: EE PC from RSTATE_CPU(x27=&g_cpuRegistersPack)
    // cpuRegs.pc offset in cpuRegistersPack = offsetof(cpuRegisters,pc) = 0x2A8 (verified: GPR=512+HI=16+LO=16+CP0=128+sa=4+IsDelaySlot=4=680=0x2A8)
    // Removal condition: crash時EE PCafter determined、本probenot neededになったらdelete
    {
        uintptr_t rstate_cpu = ss->__x[27];
        u32 ee_pc_probe = 0;
        bool ee_pc_ok = (rstate_cpu > 0x1000) && SafeRead32(rstate_cpu + 0x2A8, &ee_pc_probe);
        SafeWriteStr("@@EE_PC_CRASH@@");
        write_hex_val(" x27=", rstate_cpu);
        write_hex_val(" ee_pc=", (uintptr_t)ee_pc_probe);
        SafeWriteStr(ee_pc_ok ? " ok=1\n" : " ok=0\n");
    }
    
    // Minimal register dumps for debugging content
    // NO dladdr, NO mach_vm_region in this handler to avoid deadlocks.
    
    // [iPSX2] Register Dump (x0-x3, x16, x17, x29, x30, pc)
    
    // Registers
    write_hex_val(" pc=0x", pc_val);
    write_hex_val(" lr=0x", host_lr);
    write_hex_val(" sp=0x", (uintptr_t)host_sp);
    
    // Dump Call targets/scratch (x16/x17 typically used for blr/shims)
#if defined(_M_ARM64)
    u64 x16_val = reg_x16;
    u64 x17_val = reg_x17;
    write_hex_val(" x16=", x16_val);
    write_hex_val(" x17=", x17_val);
    write_hex_val(" x28=", ss->__x[28]); // Context pointer?
#else
    u64 x16_val = 0;
    u64 x17_val = 0;
#endif

    // [iPSX2] Dump Instruction Word (Check if readable)
    if (pc_val && (sig == SIGBUS || sig == SIGSEGV)) {
        // Optimistic read - if this crashes, we double-fault and die, which is fine.
        u32 instr = 0;
        // In JIT range?
        uintptr_t jbase = s_jit_base.load(std::memory_order_relaxed);
        uintptr_t jend = s_jit_end.load(std::memory_order_relaxed);
        if (pc_val >= jbase && pc_val < jend) {
             instr = *(u32*)pc_val;
             write_hex_val(" Instr=0x", instr);
        }
    }

    SafeWriteStr("\n");

    write_hex_val(" sp=0x", host_sp);
    write_hex_val(" addr=0x", fault_val);
    
    // si_code is int, hacky hex print
    SafeWriteStr(" code=0x");
    char code_buf[4];
    const char* h = "0123456789abcdef";
    code_buf[0] = h[(si_code >> 4) & 0xF];
    code_buf[1] = h[si_code & 0xF];
    code_buf[2] = 0;
    SafeWriteStr(code_buf);

#if defined(__aarch64__)
    write_hex_val(" x16=0x", reg_x16);
    write_hex_val(" x17=0x", reg_x17);
    write_hex_val(" x29=0x",  ss->__fp); // x29
    write_hex_val(" x30=0x",  ss->__lr); // x30 is stored as LR in mcontext usually, but verify
    write_hex_val(" x0=0x", reg_x0);
    write_hex_val(" x1=0x", reg_x1);
    write_hex_val(" x2=0x", reg_x2);
    write_hex_val(" x3=0x", reg_x3);
    write_hex_val(" esr=0x", esr);
    
    // Classify
    // EC is bits 31:26
    u32 ec = (esr >> 26) & 0x3F;
    if (ec == 0x20 || ec == 0x21) SafeWriteStr(" FAULT=IFETCH");
    else if (ec == 0x24 || ec == 0x25) SafeWriteStr(" FAULT=DATA");
    else SafeWriteStr(" FAULT=OTHER");
    
    // JIT Range Check
    // Reuse outer jbase/jend
    if (pc_val >= jbase && pc_val < jend) SafeWriteStr(" in_jit_range=1");
    else SafeWriteStr(" in_jit_range=0");
#endif
    SafeWriteStr("\n");

 
 	if (sig == SIGBUS) SafeWriteStr("Signal: SIGBUS\n");
	else if (sig == SIGSEGV) SafeWriteStr("Signal: SIGSEGV\n");
	else if (sig == SIGILL) SafeWriteStr("Signal: SIGILL\n");
	else if (sig == SIGABRT) SafeWriteStr("Signal: SIGABRT\n");
	else SafeWriteStr("Signal: UNKNOWN\n");
	
	// Flush file output
	if (s_crash_log_fd >= 0) {
		fsync(s_crash_log_fd);
	}
	
	// --- Async-Signal-Safe Diagnostics ---
	// Check if this is likely a JIT crash (W^X violation or bad codegen)
	// SIGBUS is common for W^X violations on macOS.
	// SIGILL indicates illegal instruction (bad codegen or jumping to data).
	if (HostSys::g_JitContext.block_id != 0 && (sig == SIGBUS || sig == SIGSEGV || sig == SIGILL))
	{
		SafeWriteStr("\n!!! JIT CRASH DETECTED !!!\n");
		if (sig == SIGBUS) SafeWriteStr("Signal: SIGBUS (W^X Violation?)\n");
		if (sig == SIGSEGV) SafeWriteStr("Signal: SIGSEGV (Bad Access)\n");
		if (sig == SIGILL) SafeWriteStr("Signal: SIGILL (Illegal Instruction)\n");

		SafeWriteHex("Fault Addr: ", reinterpret_cast<uintptr_t>(exception_address));
		SafeWriteHex("Host PC:    ", reinterpret_cast<uintptr_t>(exception_pc));
#if defined(_M_ARM64)
		SafeWriteHex("Host LR:    ", static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__lr);
		SafeWriteHex("Host SP:    ", static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__sp);
		
		// ESR classification for ARM64 (if available via mcontext)
		// On Darwin ARM64, ESR may be in __es.__esr or not exposed directly.
		// We can infer from signal type:
// [iPSX2] IFETCH PROBE
		if (sig == SIGBUS && jbase > 0 && jend > 0) {
			uintptr_t lr_val = static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__lr;
            uintptr_t x16_val = static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__x[16];
            SafeWriteHex("Host X16:   ", x16_val);

			// Check if LR is in JIT range
			if (lr_val >= jbase && lr_val < jend) {
				uintptr_t site = lr_val - 4;
				// Ensure site is readable and in JIT
				if (site >= jbase && site < jend) {
					uint32_t insn = *(uint32_t*)site;
                    
                    // Simple Hex Writer helper to avoid newlines
                    auto write_hex_inline = [](const char* p, uintptr_t v) {
                        SafeWriteStr(p);
                        char b[17];
                        const char* h = "0123456789abcdef";
                        for(int i=0; i<8; ++i) { 
                            b[2*i] = h[(v >> ((7-i)*8 + 4)) & 0xF];
                            b[2*i+1] = h[(v >> ((7-i)*8)) & 0xF];
                        }
                        b[16] = 0;
                        SafeWriteStr(b);
                    };

					int reg_n = -1;
					const char* op_name = "UNK";
					
					// BLR Xn: 1101 0110 0011 1111 0000 0000 000n nnnn -> 0xD63F0000 mask 0xFFFFFC1F
					// BR  Xn: 1101 0110 0001 1111 0000 0000 000n nnnn -> 0xD61F0000 mask 0xFFFFFC1F
					
					if ((insn & 0xFFFFFC1F) == 0xD63F0000) {
						reg_n = (insn >> 5) & 0x1F;
						op_name = "BLR";
					} else if ((insn & 0xFFFFFC1F) == 0xD61F0000) {
						reg_n = (insn >> 5) & 0x1F;
						op_name = "BR";
					}
					
					SafeWriteStr("@@IFETCH_SITE@@ lr=");
                    write_hex_inline("", lr_val);
					write_hex_inline(" site=", site);
					write_hex_inline(" insn=", insn);
					SafeWriteStr(" op="); SafeWriteStr(op_name);
					
					if (reg_n >= 0) {
						SafeWriteStr(" reg=x");
						char rbuf[4];
						if(reg_n < 10) { rbuf[0] = '0'+reg_n; rbuf[1]=0; }
						else { rbuf[0] = '0'+(reg_n/10); rbuf[1]='0'+(reg_n%10); rbuf[2]=0; }
						SafeWriteStr(rbuf);
                        uintptr_t reg_val = static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__x[reg_n];
						write_hex_inline(" val=", reg_val);
					}
                    write_hex_inline(" fault=", reinterpret_cast<uintptr_t>(exception_address));
					SafeWriteStr("\n");
				}
			}
		}

		if (sig == SIGILL) {
			SafeWriteStr("ESR Class:  Illegal Instruction (SIGILL)\n");
			// Dump the instruction at fault PC for analysis
			if (exception_pc) {
				u32 fault_instr = *reinterpret_cast<u32*>(exception_pc);
				SafeWriteHex("Fault Instr:", fault_instr);
			}
		} else if (sig == SIGBUS) {
			SafeWriteStr("ESR Class:  Data/Instruction Abort (SIGBUS - likely W^X)\n");
		} else if (sig == SIGSEGV) {
			SafeWriteStr("ESR Class:  Data Abort (SIGSEGV - unmapped/permission)\n");
		}
        
        if (DarwinMisc::iPSX2_WX_TRACE) {
             SafeWriteStr("\n--- W^X Ring Buffer --- \n");
             
             auto WXHexInline = [](const char* p, uintptr_t v) {
                SafeWriteStr(p);
                char b[17];
                const char* h = "0123456789abcdef";
                for(int i=0; i<8; ++i) { 
                    b[2*i] = h[(v >> ((7-i)*8 + 4)) & 0xF];
                    b[2*i+1] = h[(v >> ((7-i)*8)) & 0xF];
                }
                b[16] = 0;
                SafeWriteStr(b);
            };

             // Dumb dump of all events
             for(int i=0; i<16; ++i) {
                 volatile auto& e = DarwinMisc::g_wx_events[i];
                 if (e.tid == 0) continue;
                 SafeWriteStr("@@WX_LAST@@ idx="); 
                 char b[4]; 
                 if(i<10) { b[0]='0'+i; b[1]=0; } else { b[0]='1'; b[1]='0'+(i-10); b[2]=0; }
                 SafeWriteStr(b);
                 WXHexInline(" tid=", e.tid);
                 WXHexInline(" caller=", e.caller);
                 SafeWriteStr(e.write ? " write=1" : " write=0");
                 SafeWriteStr(e.depth ? " depth=1" : " depth=0"); // Approx
                 SafeWriteStr("\n");
             }

             SafeWriteStr("\n--- JIT Emit Call Ring Buffer --- \n");
             for(int i=0; i<32; ++i) {
                 volatile auto& e = DarwinMisc::g_emit_events[i];
                 if (e.ptr == 0) continue;
                 SafeWriteStr("@@EMIT_LAST@@ idx=");
                 char b[4];
                 if(i<10) { b[0]='0'+i; b[1]=0; } else { b[0]='1'+((i-10)/10); b[1]='0'+((i-10)%10); b[2]=0; }
                 SafeWriteStr(b);
                 WXHexInline(" pc=", e.pc);
                 WXHexInline(" ptr=", e.ptr);
                 WXHexInline(" sym=", e.sym);
                 WXHexInline(" tid=", e.tid);
                 WXHexInline(" caller=", e.caller);
                 SafeWriteStr("\n");
             }
        }
#endif
        // [iPSX2] Symbolize Crash PC/LR
        {
        // [iPSX2] Symbolize Crash PC/LR (Strict Tags) - MOVED TO TOP
        // { ... logic moved to top ... }

        // [iPSX2] JIT Block Mapping
        if (pc_val >= jbase && pc_val < jend) {
             u32 gpc = 0; void* rxptr = nullptr;
             if (DarwinMisc::FindJitBlock(pc_val, &gpc, &rxptr)) {
                  SafeWriteStr("@@JIT_BLOCK@@ source=PC");
                  // Minimal hex writer inline
                  char buf[32]; 
                  // ... laziness, reused write_hex_val if kept context ...
                  // Just trust FindJitBlock works and let's print raw logs if possible or minimal
                  // Assuming FindJitBlock doesn't log itself.
             }
        }
        if (host_lr >= jbase && host_lr < jend) {
             u32 gpc = 0; void* rxptr = nullptr;
             if (DarwinMisc::FindJitBlock(host_lr, &gpc, &rxptr)) {
                  // We found the block
                  SafeWriteStr("@@JIT_BLOCK@@ source=LR found=1\n");
             }
        }
		SafeWriteHex("Guest PC:   ", HostSys::g_JitContext.guest_pc);
		SafeWriteHex("Block ID:   ", HostSys::g_JitContext.block_id);

		// Add to Quarantine
		u32 idx = s_quarantine_idx.fetch_add(1) % 16;
		s_block_quarantine[idx] = {HostSys::g_JitContext.guest_pc, HostSys::g_JitContext.block_id};
		SafeWriteStr("Block quarantined.\n");
        
        // If it's a W^X violation, HandlePageFault might fail or deadlock if we lock mutex.
        // We generally shouldn't try to recover from a JIT crash in the wild unless we have a specific handler.
        // For now, fall through to existing logic but beware of deadlocks.
	}
	// [iter80] Close the if(block_id!=0) telemetry block here so HandlePageFault runs unconditionally.
	}
	// ---------------------------------------

	} // [P52] end verbose_signal_log block

	// [iter79] @@PRE_MUTEX@@ – probe BEFORE mutex lock, using SafeWriteStr
	{
		static u32 s_pm_n = 0;
		if (s_pm_n < 4)
		{
			char buf[64];
			snprintf(buf, sizeof(buf), "@@PRE_MUTEX@@ n=%u\n", s_pm_n);
			SafeWriteStr(buf);
			s_pm_n++;
		}
	}

	// [iter685] Use try_lock to avoid deadlock in signal handlers.
	if (!s_exception_handler_mutex.try_lock())
	{
		SafeWriteStr("@@HPF_TRYLOCK_FAIL@@ concurrent page fault, aborting\n");
		_exit(99);
	}

	// [iter78] @@MUTEX_LOCKED@@ – SafeWriteStr (s_crash_log_fd path) to survive stderr redirect
	{
		static u32 s_ml_n = 0;
		if (s_ml_n < 4)
		{
			char buf[80];
			snprintf(buf, sizeof(buf), "@@MUTEX_LOCKED@@ n=%u in_handler=%d\n",
				s_ml_n, (int)s_in_exception_handler);
			SafeWriteStr(buf);
			s_ml_n++;
		}
	}

	// Prevent recursive exception filtering.
	HandlerResult result = HandlerResult::ExecuteNextHandler;
	if (!s_in_exception_handler)
	{
		s_in_exception_handler = true;
		result = HandlePageFault(exception_pc, exception_address, is_write);
		
		// If page fault handled, we are good.
		// If NOT handled (ExecuteNextHandler), and we are in JIT context, we should probably force exit
		// because standard CrashHandler uses malloc/printf which will deadlock/corrupt.
		if (result == HandlerResult::ExecuteNextHandler && HostSys::g_JitContext.block_id != 0)
		{
             s_exception_handler_mutex.unlock();
             SafeWriteStr("Unresolvable JIT Crash. Improved diagnosis above. Aborting safely.\n");
             abort(); // Better than hanging
		}
		
		s_in_exception_handler = false;
	}

	s_exception_handler_mutex.unlock();

	// Resumes execution right where we left off (re-executes instruction that caused the SIGSEGV).
	if (result == HandlerResult::ContinueExecution)
		return;

	// We couldn't handle it. Pass it off to the crash dumper.
	CrashHandler::CrashSignalHandler(sig, info, ctx);
}

void DarwinMisc::LogDyldMain() {
    static bool logged = false;
    if (!logged) {
        const struct mach_header* mh = _dyld_get_image_header(0);
        if (mh) {
            Console.WriteLn("@@DYLD_MAIN@@ base=%p", mh);
        } else {
            Console.WriteLn("@@DYLD_MAIN@@ base=UNKNOWN");
        }
        logged = true;
    }
}

bool PageFaultHandler::Install_Fresh(Error* error)
{
	std::unique_lock lock(s_exception_handler_mutex);
	pxAssertRel(!s_installed, "Page fault handler has already been installed.");

	struct sigaction sa;

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = SignalHandler;

	// MacOS uses SIGBUS for memory permission violations, as well as SIGSEGV on ARM64.
	if (sigaction(SIGBUS, &sa, nullptr) != 0)
	{
		Error::SetErrno(error, "sigaction() for SIGBUS failed: ", errno);
		return false;
	}

#ifdef _M_ARM64
	if (sigaction(SIGSEGV, &sa, nullptr) != 0)
	{
		Error::SetErrno(error, "sigaction() for SIGSEGV failed: ", errno);
		return false;
	}
	
	// SIGILL for illegal instruction detection (bad JIT codegen or jumping to non-code)
	if (sigaction(SIGILL, &sa, nullptr) != 0)
	{
		Error::SetErrno(error, "sigaction() for SIGILL failed: ", errno);
		return false;
	}
	
	// [iter685] Do NOT install PageFaultHandler for SIGABRT.
	// CrashHandler calls abort() → SIGABRT → our handler → HandlePageFault fails
	// → CrashHandler → abort() = infinite loop. Let default SIGABRT handler terminate.
	// if (sigaction(SIGABRT, &sa, nullptr) != 0)
	// {
	// 	Error::SetErrno(error, "sigaction() for SIGABRT failed: ", errno);
	// 	return false;
	// }
#endif

	// Allow us to ignore faults when running under lldb.
	task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, MACH_PORT_NULL, EXCEPTION_DEFAULT, 0);

	    // Open crash log file for backup output (async-signal-safe)
    // Use the FD set by ios_main.mm (Documents/pcsx2_log.txt) if available,
    // otherwise fall back to /tmp (Simulator only).
    if (s_user_crash_log_fd >= 0)
    {
        s_crash_log_fd = s_user_crash_log_fd;
    }
    else
    {
        const char* log_file = "/tmp/pcsx2_crash_RUN_124500.log";
        s_crash_log_fd = open(log_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    }
    // Redirect stderr → crash log so write(STDERR_FILENO,...) probes appear in log file
    if (s_crash_log_fd >= 0)
        dup2(s_crash_log_fd, STDERR_FILENO);

    // Log confirmation that signal handler is installed
    // Must be one-shot effectively since Install_Fresh is called once
    Console.WriteLn("@@LOG_TRUNC@@ pid=%d", getpid());
    Console.WriteLn("@@BOOT_VER@@ v=2 pid=%d", getpid());
    Console.WriteLn("@@SIG_INSTALLED@@ Signals: SIGBUS SIGSEGV SIGILL. CrashLog: fd=%d",
        s_crash_log_fd);

	s_installed = true;
	return true;
}

// [iPSX2] Indirect Branch Probe Definitions
volatile DarwinMisc::IndirectEvent DarwinMisc::g_ie[8] = {};
volatile u32 DarwinMisc::g_ie_idx = 0;
volatile int DarwinMisc::g_jit_write_state = 0;
volatile int DarwinMisc::g_rec_stage = 0;
volatile DarwinMisc::WXTraceEvent DarwinMisc::g_wx_events[16] = {};
volatile u32 DarwinMisc::g_wx_idx = 0;
volatile DarwinMisc::EmitEvent DarwinMisc::g_emit_events[32] = {};
volatile u32 DarwinMisc::g_emit_idx = 0;
