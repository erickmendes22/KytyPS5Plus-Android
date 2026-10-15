# FEXCore guest CPU backend. Guest PS5 code is x86-64; on ARM64 hosts it runs inside FEX's translator.
#
# FEX is built as a separate CMake project (ExternalProject) so its own options, global compile flags,
# bundled fmt/xxHash/jemalloc and sanitizer settings never leak into Kyty targets. Kyty then links
# the resulting static libraries as IMPORTED targets and compiles src/cpu/guestCpuFex*.cpp against
# FEX's internal headers.

include(ExternalProject)

# FEX targets Linux (and Android's Linux kernel); macOS arm64 hosts keep the native-only path.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android)$")
	set(KYTY_GUEST_CPU_FEX_DEFAULT ON)
else()
	set(KYTY_GUEST_CPU_FEX_DEFAULT OFF)
endif()
option(KYTY_GUEST_CPU_FEX "Run guest x86-64 code through FEXCore (ARM64 hosts)" ${KYTY_GUEST_CPU_FEX_DEFAULT})

if(KYTY_GUEST_CPU_FEX AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
	message(FATAL_ERROR "KYTY_GUEST_CPU_FEX requires an ARM64 host; FEXCore translates x86-64 on AArch64 only")
endif()

if(KYTY_GUEST_CPU_FEX)
	# Pinned FEX revision. Bump deliberately; the include layout and flags below track this commit.
	set(KYTY_FEX_GIT_REPOSITORY "https://github.com/FEX-Emu/FEX" CACHE STRING "FEX git URL")
	set(KYTY_FEX_GIT_TAG "5285020de0b30dbdae7b44d0d83541af2d13d002" CACHE STRING "FEX commit to build")
	set(KYTY_FEX_SOURCE_DIR "" CACHE PATH "Existing FEX checkout to build instead of cloning KYTY_FEX_GIT_TAG")

	set(kyty_fex_bin "${PROJECT_BINARY_DIR}/fex/build")
	set(kyty_fex_src "${PROJECT_BINARY_DIR}/fex/src")
	if(KYTY_FEX_SOURCE_DIR)
		set(kyty_fex_src "${KYTY_FEX_SOURCE_DIR}")
		set(kyty_fex_fetch)
	else()
		set(kyty_fex_fetch GIT_REPOSITORY "${KYTY_FEX_GIT_REPOSITORY}" GIT_TAG "${KYTY_FEX_GIT_TAG}" GIT_SHALLOW OFF)
		# The Android port is applied to our own clone only; a KYTY_FEX_SOURCE_DIR checkout is taken as patched.
		# applyFexPatch.cmake is idempotent (it skips the patch when it is already applied).
		if(ANDROID)
			list(APPEND kyty_fex_fetch PATCH_COMMAND ${CMAKE_COMMAND}
				"-DFEX_SOURCE_DIR=${kyty_fex_src}"
				"-DFEX_PATCH=${KYTY_SOURCE_DIR}/cpu/patches/fex-android.patch"
				-P "${KYTY_SOURCE_DIR}/cpu/patches/applyFexPatch.cmake")
		endif()
	endif()

	# Android: FEX is cross-compiled with the same NDK toolchain and ABI as Kyty. Its static archives are linked
	# into the shared libmain.so, so they must be position-independent (ExternalProject does not inherit this).
	set(kyty_fex_android_args)
	if(ANDROID)
		set(kyty_fex_android_args
			"-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
			"-DANDROID_ABI=${ANDROID_ABI}"
			"-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
			-DCMAKE_POSITION_INDEPENDENT_CODE=ON)
	endif()

	# CMake's compiler detection already located clang-scan-deps for the Kyty compiler.
	set(KYTY_CLANG_SCAN_DEPS "${CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS}")
	if(NOT KYTY_CLANG_SCAN_DEPS)
		find_program(KYTY_CLANG_SCAN_DEPS NAMES clang-scan-deps-${CMAKE_CXX_COMPILER_VERSION_MAJOR} clang-scan-deps)
	endif()
	if(NOT KYTY_CLANG_SCAN_DEPS)
		message(FATAL_ERROR "FEX needs clang-scan-deps (install clang-tools)")
	endif()

	# Symbol-prefix header keeps FEX's fmt and xxHash from clashing with Kyty's copies.
	set(kyty_fex_prefix_header "${KYTY_SOURCE_DIR}/cpu/fexSymbolPrefix.h")

	# Libraries FEXCore/LinuxEmulation need. JemallocLibs is FEX's fextl allocator hook (rpmalloc-backed),
	# not the glibc jemalloc: that one replaces malloc process-wide and is disabled (thunks are off).
	set(kyty_fex_lib_files
		FEXCore/Source/libFEXCore.a
		FEXCore/Source/libFEXCore_Base.a
		FEXCore/Source/libJemallocLibs.a
		Source/Tools/LinuxEmulation/libLinuxEmulation.a
		Source/Tools/CommonTools/libCommonTools.a
		Source/Common/libCommon.a
		Source/Common/cpp-optparse/libcpp-optparse.a
		External/fmt/libfmt.a
		External/xxhash/cmake_unofficial/libxxhash.a
		External/rpmalloc/librpmalloc.a
		External/tiny-json/libtiny-json.a
		External/SoftFloat-3e/libsoftfloat_3e.a
		External/cephes/libcephes_128bit.a)
	set(kyty_fex_byproducts)
	foreach(file IN LISTS kyty_fex_lib_files)
		list(APPEND kyty_fex_byproducts "${kyty_fex_bin}/${file}")
	endforeach()

	ExternalProject_Add(kyty_fex
		${kyty_fex_fetch}
		SOURCE_DIR "${kyty_fex_src}"
		BINARY_DIR "${kyty_fex_bin}"
		CMAKE_ARGS
			-DCMAKE_BUILD_TYPE=Release
			-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
			-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
			-DCMAKE_ASM_COMPILER=${CMAKE_C_COMPILER}
			-DCMAKE_C_COMPILER_CLANG_SCAN_DEPS=${KYTY_CLANG_SCAN_DEPS}
			-DCMAKE_CXX_COMPILER_CLANG_SCAN_DEPS=${KYTY_CLANG_SCAN_DEPS}
			-DCMAKE_ASM_COMPILER_CLANG_SCAN_DEPS=${KYTY_CLANG_SCAN_DEPS}
			${kyty_fex_android_args}
			"-DCMAKE_C_FLAGS=-include ${kyty_fex_prefix_header}"
			"-DCMAKE_CXX_FLAGS=-include ${kyty_fex_prefix_header}"
			-DBUILD_THUNKS=FALSE
			-DBUILD_TESTING=FALSE
			-DBUILD_FEXCONFIG=FALSE
			-DBUILD_FEX_LINUX_TESTS=FALSE
			-DENABLE_ASSERTIONS=TRUE
			-DENABLE_LTO=FALSE
			-DENABLE_CCACHE=FALSE
			-DENABLE_JEMALLOC_GLIBC_ALLOC=FALSE
			-DTUNE_ARCH=generic
			-DTUNE_CPU=none
		BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target FEXCore LinuxEmulation Common CommonTools cpp-optparse fmt xxhash rpmalloc tiny-json softfloat_3e cephes_128bit JemallocLibs
		INSTALL_COMMAND ""
		BUILD_BYPRODUCTS ${kyty_fex_byproducts}
	)

	# Static FEX libraries, imported so Kyty's link line refers to them by path.
	set(kyty_fex_imported)
	foreach(file IN LISTS kyty_fex_lib_files)
		get_filename_component(lib_name "${file}" NAME_WE)
		string(REGEX REPLACE "^lib" "" lib_name "${lib_name}")
		set(imported "kyty_fex_${lib_name}")
		add_library(${imported} STATIC IMPORTED GLOBAL)
		set_target_properties(${imported} PROPERTIES IMPORTED_LOCATION "${kyty_fex_bin}/${file}")
		list(APPEND kyty_fex_imported ${imported})
	endforeach()

	# Guest CPU implementation. Built against FEX's private headers, so it sees FEX's include dirs and
	# the compile definitions FEX itself uses for LinuxEmulation/FEXCore on AArch64.
	add_library(kyty_guest_cpu STATIC
		"${KYTY_SOURCE_DIR}/cpu/guestCpuFex.cpp"
		"${KYTY_SOURCE_DIR}/cpu/guestCpuFexHle.cpp")
	add_dependencies(kyty_guest_cpu kyty_fex)
	target_include_directories(kyty_guest_cpu BEFORE PRIVATE
		${kyty_fex_bin}/Source/Tools/LinuxEmulation
		${kyty_fex_src}/Source/Tools/LinuxEmulation
		${kyty_fex_src}/Source
		${kyty_fex_bin}/Source
		${kyty_fex_bin}/generated
		${kyty_fex_src}/External/drm-headers/include
		${kyty_fex_bin}/FEXCore/Source
		${kyty_fex_src}/FEXCore/include
		${kyty_fex_bin}/include
		${kyty_fex_src}/External/fmt/include
		${kyty_fex_src}/External/xxhash/cmake_unofficial/..
		${kyty_fex_src}/FEXHeaderUtils
		${kyty_fex_src}/CodeEmitter
		${kyty_fex_src}/External/unordered_dense/include
		${kyty_fex_src}/External/range-v3/include
		${kyty_fex_src}/External/tiny-json
		${kyty_fex_src}/Source/Common/cpp-optparse
		${kyty_fex_src}/Source/Tools/CommonTools)
	# Configure-time probe results FEX bakes into its headers. The Android build (patched) sees no
	# program_invocation_name and no getcpu() declaration at API 31, so these must match what FEX was built with.
	set(kyty_fex_probe_defs HAS_PROGRAM_INVOCATION_NAME=1 HAS_SYSCALL_GETCPU=1)
	set(kyty_fex_sys_libs pthread dl)
	if(ANDROID)
		set(kyty_fex_probe_defs HAS_PROGRAM_INVOCATION_NAME=0)
		# pthread lives in bionic's libc on Android; there is no libpthread to link.
		set(kyty_fex_sys_libs dl)
	endif()

	target_compile_definitions(kyty_guest_cpu PRIVATE
		ARCHITECTURE_arm64=1
		JIT_ARM64=1
		ASSERTIONS_ENABLED=1
		FEXCORE_HAS_PRESERVE_ALL_ATTR=1
		"FEXCORE_PRESERVE_ALL_ATTR=__attribute__((preserve_all))"
		FEX_HAS_PRESERVE_ALL_ATTR=1
		"FEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))"
		${kyty_fex_probe_defs}
		HAS_SYSCALL_GETTID=1
		HAS_SYSCALL_RENAMEAT2=1
		HAS_SYSCALL_STATX=1
		HAS_SYSCALL_TGKILL=1
		"INLINE=static inline"
		INLINE_LEVEL=4
		RANGES_DISABLE_DEPRECATED_WARNINGS=1
		_LARGEFILE64_SOURCE=1)
	target_compile_options(kyty_guest_cpu PRIVATE -fno-operator-names -fno-strict-aliasing)
	# FEX's archives reference each other in both directions (FEXCore <-> LinuxEmulation), so the
	# whole set is one link group.
	target_link_libraries(kyty_guest_cpu PUBLIC -Wl,--start-group ${kyty_fex_imported} -Wl,--end-group ${kyty_fex_sys_libs})

	list(APPEND kyty_emulator_link_libraries kyty_guest_cpu)
endif()
