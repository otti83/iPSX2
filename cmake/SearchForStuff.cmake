#-------------------------------------------------------------------------------
#                       Search all libraries on the system
#-------------------------------------------------------------------------------
find_package(Git)

# Require threads on all OSes.
find_package(Threads REQUIRED)

# Dependency libraries.
# On macOS, Mono.framework contains an ancient version of libpng.  We don't want that.
# Avoid it by telling cmake to avoid finding frameworks while we search for libpng.
set(FIND_FRAMEWORK_BACKUP ${CMAKE_FIND_FRAMEWORK})
set(CMAKE_FIND_FRAMEWORK NEVER)
#find_package(PNG 1.6.40 REQUIRED)
#find_package(JPEG REQUIRED) # No version because flatpak uses libjpeg-turbo.
#find_package(ZLIB REQUIRED) # v1.3, but Mac uses the SDK version.
#find_package(Zstd 1.5.5 REQUIRED)
#find_package(LZ4 REQUIRED)
#find_package(WebP REQUIRED) # v1.3.2, spews an error on Linux because no pkg-config.
#find_package(SDL3 3.2.6 REQUIRED)
#find_package(Freetype 2.11.1 REQUIRED)
#find_package(plutovg REQUIRED) # v0.0.13 is needed for building plutosvg, but we can support v1.0.0
#find_package(plutosvg 0.0.6 REQUIRED)

#if(USE_VULKAN)
#	find_package(Shaderc REQUIRED)
#endif()

# Platform-specific dependencies.
if (WIN32)
	add_subdirectory(3rdparty/D3D12MemAlloc EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/winpixeventruntime EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/winwil EXCLUDE_FROM_ALL)
	set(FFMPEG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/include")
	find_package(Vtune)
elseif (ANDROID)
	add_subdirectory(3rdparty/zlib EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/zstd EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/lz4 EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/libwebp EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/SDL3 EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/harfbuzz EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/freetype EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/oboe EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/plutosvg1 EXCLUDE_FROM_ALL)
	find_package(EGL REQUIRED)
#	include(CheckLib)
#	check_lib(EGL EGL EGL/egl.h)
	set(CUBEB_API ON)
elseif(iPSX2_MACOS_BUILD)
	# [P63] macOS native build: use same 3rdparty as iOS but with macOS SDK
	set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
	set(ZLIB_LIBRARIES "z" CACHE FILEPATH "" FORCE)
	set(ZLIB_LIBRARY "z" CACHE FILEPATH "" FORCE)
	set(ZLIB_INCLUDE_DIR "" CACHE PATH "" FORCE)
	set(BZIP2_FOUND TRUE CACHE BOOL "" FORCE)
	set(BZIP2_LIBRARIES "bz2" CACHE FILEPATH "" FORCE)
	set(BZIP2_LIBRARY "bz2" CACHE FILEPATH "" FORCE)
	set(MATH_LIBRARY "m" CACHE FILEPATH "" FORCE)
	set(PNG_FOUND FALSE CACHE BOOL "" FORCE)
	set(PNG_LIBRARIES "" CACHE FILEPATH "" FORCE)
	set(PNG_LIBRARY "" CACHE FILEPATH "" FORCE)
	add_subdirectory(3rdparty/zlib EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/zstd EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/lz4 EXCLUDE_FROM_ALL)
	set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
	add_subdirectory(3rdparty/libwebp EXCLUDE_FROM_ALL)
	if(TARGET webp AND NOT TARGET WebP::libwebp)
		add_library(WebP::libwebp ALIAS webp)
	endif()
	set(SDL_SHARED OFF CACHE BOOL "" FORCE)
	set(SDL_STATIC ON CACHE BOOL "" FORCE)
	add_subdirectory(3rdparty/SDL3 EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/harfbuzz EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/freetype EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/plutosvg1 EXCLUDE_FROM_ALL)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
	# Force system library names to avoid SDK path mismatch (Device vs Simulator)
	set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
	set(ZLIB_LIBRARIES "z" CACHE FILEPATH "" FORCE)
	set(ZLIB_LIBRARY "z" CACHE FILEPATH "" FORCE)
	set(ZLIB_INCLUDE_DIR "" CACHE PATH "" FORCE)
	
	set(BZIP2_FOUND TRUE CACHE BOOL "" FORCE)
	set(BZIP2_LIBRARIES "bz2" CACHE FILEPATH "" FORCE)
	set(BZIP2_LIBRARY "bz2" CACHE FILEPATH "" FORCE)

	set(MATH_LIBRARY "m" CACHE FILEPATH "" FORCE)
	
	# iOS does not have a system libpng, and we don't have it in 3rdparty.
	# Disable it to avoid linking errors or mismatching paths from macOS SDK.
	set(PNG_FOUND FALSE CACHE BOOL "" FORCE)
	set(PNG_LIBRARIES "" CACHE FILEPATH "" FORCE)
	set(PNG_LIBRARY "" CACHE FILEPATH "" FORCE)

	add_subdirectory(3rdparty/zlib EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/zstd EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/lz4 EXCLUDE_FROM_ALL)
	
	# Disable WebP utilities for iOS to avoid install errors
	set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
	set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
	
	add_subdirectory(3rdparty/libwebp EXCLUDE_FROM_ALL)
	if(TARGET webp AND NOT TARGET WebP::libwebp)
		add_library(WebP::libwebp ALIAS webp)
	endif()

	# Force SDL3 to be static to avoid signing issues with dynamic libraries on iOS
	set(SDL_SHARED OFF CACHE BOOL "Build a shared version of the library" FORCE)
	set(SDL_STATIC ON CACHE BOOL "Build a static version of the library" FORCE)

	# Disable cubeb AudioUnit backend on iOS as it depends on macOS-only HAL
	set(USE_AUDIOUNIT OFF CACHE BOOL "Disable AudioUnit backend" FORCE)
	
	add_subdirectory(3rdparty/SDL3 EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/harfbuzz EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/freetype EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/plutosvg1 EXCLUDE_FROM_ALL)
else()
	if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
		find_package(CURL REQUIRED)
		find_package(PCAP REQUIRED)
		find_package(Vtune)
	endif()

	# Use bundled ffmpeg v4.x.x headers if we can't locate it in the system.
	# We'll try to load it dynamically at runtime.
	find_package(FFMPEG COMPONENTS avcodec avformat avutil swresample swscale)
	if(NOT FFMPEG_FOUND)
		message(WARNING "FFmpeg not found, using bundled headers.")
		set(FFMPEG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/include")
	endif()

	## Use CheckLib package to find module
	include(CheckLib)

	if(UNIX AND NOT APPLE)
		if(LINUX)
			check_lib(LIBUDEV libudev libudev.h)
		endif()

		if(X11_API)
			find_package(X11 REQUIRED)
			if (NOT X11_Xrandr_FOUND)
				message(FATAL_ERROR "XRandR extension is required")
			endif()
		endif()

		if(WAYLAND_API)
			find_package(ECM REQUIRED NO_MODULE)
			list(APPEND CMAKE_MODULE_PATH "${ECM_MODULE_PATH}")
			find_package(Wayland REQUIRED Egl)
		endif()

		if(USE_BACKTRACE)
			find_package(Libbacktrace REQUIRED)
		endif()

		find_package(PkgConfig REQUIRED)
		pkg_check_modules(DBUS REQUIRED dbus-1)
	endif()
endif()

set(CMAKE_FIND_FRAMEWORK ${FIND_FRAMEWORK_BACKUP})

add_subdirectory(3rdparty/fast_float EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/rapidyaml EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/lzma EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/libchdr EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(libchdr)
add_subdirectory(3rdparty/soundtouch EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/simpleini EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/imgui EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/cpuinfo EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(cpuinfo)
add_subdirectory(3rdparty/libzip EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/rcheevos EXCLUDE_FROM_ALL)
add_subdirectory(3rdparty/rapidjson EXCLUDE_FROM_ALL)
if(NOT ANDROID)
	add_subdirectory(3rdparty/discord-rpc EXCLUDE_FROM_ALL)
endif()
add_subdirectory(3rdparty/freesurround EXCLUDE_FROM_ALL)

if(USE_OPENGL)
	add_subdirectory(3rdparty/glad EXCLUDE_FROM_ALL)
endif()

if(USE_VULKAN)
	add_subdirectory(3rdparty/vulkan EXCLUDE_FROM_ALL)
endif()

if(ANDROID AND USE_VULKAN)
	add_subdirectory(3rdparty/libadrenotools EXCLUDE_FROM_ALL)
endif()

add_subdirectory(3rdparty/cubeb EXCLUDE_FROM_ALL)
disable_compiler_warnings_for_target(cubeb)
disable_compiler_warnings_for_target(speex)

# Find the Qt components that we need.
#find_package(Qt6 6.7.3 COMPONENTS CoreTools Core GuiTools Gui WidgetsTools Widgets LinguistTools REQUIRED)

if(WIN32)
  add_subdirectory(3rdparty/rainterface EXCLUDE_FROM_ALL)
endif()

# Demangler for the debugger.
add_subdirectory(3rdparty/demangler EXCLUDE_FROM_ALL)

# Symbol table parser.
add_subdirectory(3rdparty/ccc EXCLUDE_FROM_ALL)

## The docking system for the debugger.
#find_package(KDDockWidgets-qt6 2.0.0 REQUIRED)
## Add an extra include path to work around a broken include directive.
## TODO: Remove this the next time we update KDDockWidgets.
#get_target_property(KDDOCKWIDGETS_INCLUDE_DIRECTORY KDAB::kddockwidgets INTERFACE_INCLUDE_DIRECTORIES)
#target_include_directories(KDAB::kddockwidgets INTERFACE
#	${KDDOCKWIDGETS_INCLUDE_DIRECTORY}/kddockwidgets
#)

# Architecture-specific.
if(_M_X86)
	add_subdirectory(3rdparty/zydis EXCLUDE_FROM_ALL)
elseif(_M_ARM64)
	add_subdirectory(3rdparty/glslang EXCLUDE_FROM_ALL)
	add_subdirectory(3rdparty/vixl EXCLUDE_FROM_ALL)
endif()

# Prevent fmt from being built with exceptions, or being thrown at call sites.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DFMT_USE_EXCEPTIONS=0 -DFMT_USE_RTTI=0")
add_subdirectory(3rdparty/fmt EXCLUDE_FROM_ALL)

# Deliberately at the end. We don't want to set the flag on third-party projects.
if(MSVC)
	# Don't warn about "deprecated" POSIX functions.
	add_definitions("-D_CRT_NONSTDC_NO_WARNINGS" "-D_CRT_SECURE_NO_WARNINGS" "-DCRT_SECURE_NO_DEPRECATE")
endif()
