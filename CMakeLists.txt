cmake_minimum_required(VERSION 3.15)
project(smash
  VERSION 0.1.0
  LANGUAGES C CXX
  DESCRIPTION "Compression-aware memory allocator"
)

# Generate build/compile_commands.json so clangd (see .clangd) and other
# language servers can resolve include paths, defines, and per-file flags
# without needing the user to remember a CMake option.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Allow FetchContent_Populate for older CMake compatibility
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

# ── C++ Standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Threads ───────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── macOS: build fat arm64+arm64e so interposition works with all binaries ───
# System binaries (ls, cat, etc.) are arm64e (pointer authentication).
# DYLD_INSERT_LIBRARIES requires the dylib to match the process architecture.
if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
  if("${CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
    set(CMAKE_OSX_ARCHITECTURES "arm64;arm64e" CACHE STRING
        "Build universal arm64+arm64e for dyld interposition" FORCE)
  endif()
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
if(NOT WIN32)
  add_compile_options(
    -fvisibility=hidden
    -fno-builtin-malloc
    -fno-builtin-free
    -fno-builtin-realloc
    -fno-builtin-calloc
    -Wall
    -Wextra
    -Wpedantic
  )
endif()

# ── Pull in alloc8 ────────────────────────────────────────────────────────────
# Prefer a sibling alloc8 checkout (developer workflow). If none exists and no
# ALLOC8_DIR was given, fetch it from GitHub via FetchContent.
set(ALLOC8_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../alloc8" CACHE PATH "Path to alloc8 source")

# On Linux, use Smash's version script (extends alloc8's with syscall wrappers).
# Generated from src/smash_version_script.map.in with a per-arch GLIBC base
# version so malloc/free etc. are exported at a version that satisfies the
# binaries' requests (aarch64 binaries link against malloc@GLIBC_2.17, so
# emitting only @GLIBC_2.2.5 silently bypasses LD_PRELOAD interception).
# Must be set BEFORE add_subdirectory(alloc8) so alloc8 uses this path.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(SMASH_GLIBC_BASE_VERSION "GLIBC_2.17")
  else()
    set(SMASH_GLIBC_BASE_VERSION "GLIBC_2.2.5")
  endif()
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/smash_version_script.map.in
    ${CMAKE_CURRENT_BINARY_DIR}/smash_version_script.map
    @ONLY
  )
  set(SMASH_VERSION_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/smash_version_script.map")
  set(ALLOC8_VERSION_SCRIPT "${SMASH_VERSION_SCRIPT}" CACHE INTERNAL "Version script" FORCE)
endif()

include(FetchContent)

if(EXISTS "${ALLOC8_DIR}/CMakeLists.txt")
  add_subdirectory("${ALLOC8_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/alloc8")
else()
  message(STATUS "alloc8 not found at ${ALLOC8_DIR}; fetching via FetchContent")
  FetchContent_Declare(
    alloc8
    GIT_REPOSITORY https://github.com/emeryberger/alloc8.git
    GIT_TAG main
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(alloc8)
endif()

# ── emeryberger/printf — allocation-free printf/snprintf ────────────────────
# Standard glibc snprintf may call malloc internally (for large field widths
# or when %g/%a paths grow scratch buffers). Inside the malloc fast path,
# fault handlers, or compressor threads, that recursion can deadlock or
# overflow the stack. We compile this header-then-included library directly
# into libsmash and #include "printf.h" wherever we format diagnostic output
# from those paths.
FetchContent_Declare(
  printf
  GIT_REPOSITORY https://github.com/emeryberger/printf.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(printf)

# ── LZ4 compression library ──────────────────────────────────────────────────
FetchContent_Declare(
  lz4
  GIT_REPOSITORY https://github.com/lz4/lz4.git
  GIT_TAG v1.9.4
  GIT_SHALLOW TRUE
)
FetchContent_GetProperties(lz4)
if(NOT lz4_POPULATED)
  FetchContent_Populate(lz4)
  add_library(lz4_static STATIC ${lz4_SOURCE_DIR}/lib/lz4.c ${lz4_SOURCE_DIR}/lib/lz4hc.c)
  target_include_directories(lz4_static PUBLIC ${lz4_SOURCE_DIR}/lib)
  set_target_properties(lz4_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ── Zstandard compression library ───────────────────────────────────────────
FetchContent_Declare(
  zstd
  GIT_REPOSITORY https://github.com/facebook/zstd.git
  GIT_TAG v1.5.6
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR build/cmake
)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zstd)

# ── LTO ───────────────────────────────────────────────────────────────────────
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# ── libsmash shared library ──────────────────────────────────────────────────
# On Linux, gnu_wrapper.cpp already #includes new_delete.inc, so linking
# ALLOC8_COMMON_SOURCES (which contains new_delete.cpp) causes duplicate
# operator new/delete definitions. Only include common sources on non-Linux.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_SMASH_COMMON_SOURCES ${ALLOC8_COMMON_SOURCES})
endif()

# Option to disable thread hooks (workaround for walrus crash)
option(SMASH_NO_THREAD_HOOKS "Disable alloc8 pthread interposition" OFF)

if(SMASH_NO_THREAD_HOOKS)
  set(_SMASH_THREAD_SOURCES "")
  set(_SMASH_THREAD_DEFINE "SMASH_NO_THREAD_HOOKS_BUILD")
  message(STATUS "Thread hooks DISABLED")
else()
  set(_SMASH_THREAD_SOURCES ${ALLOC8_THREAD_SOURCES})
  set(_SMASH_THREAD_DEFINE "")
  message(STATUS "Thread hooks enabled")
endif()

add_library(smash SHARED
  src/smash_heap.cpp
  src/linux_syscall_wrappers.cpp
  ${printf_SOURCE_DIR}/printf.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
  ${_SMASH_THREAD_SOURCES}
  ${_SMASH_COMMON_SOURCES}
)
target_include_directories(smash PRIVATE ${printf_SOURCE_DIR})

# Linux syscall wrappers need default visibility for LD_PRELOAD interposition.
# The global -fvisibility=hidden flag hides them otherwise.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set_source_files_properties(src/linux_syscall_wrappers.cpp PROPERTIES
    COMPILE_FLAGS "-fvisibility=default -fno-lto"
  )
endif()

target_include_directories(smash PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# On Linux, link against alloc8 components manually instead of alloc8::interpose
# to avoid the INTERFACE version script and use Smash's instead (includes syscall wrappers)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(smash PRIVATE alloc8::headers pthread dl)
  target_link_options(smash PRIVATE
    "LINKER:--version-script=${SMASH_VERSION_SCRIPT}"
    "-Bsymbolic"
  )
else()
  target_link_libraries(smash PRIVATE alloc8::interpose)
endif()
target_link_libraries(smash PRIVATE lz4_static libzstd_static Threads::Threads ${CMAKE_DL_LIBS})

# Add thread hooks define if disabled
if(SMASH_NO_THREAD_HOOKS)
  target_compile_definitions(smash PRIVATE SMASH_NO_THREAD_HOOKS_BUILD)
endif()

set_target_properties(smash PROPERTIES
  OUTPUT_NAME "smash"
  PREFIX "lib"
)

# ── Ablation defines (override config.h defaults via -DSMASH_*=value) ────────
# Numeric overrides (only when non-empty)
foreach(_var NUM_ARENAS COLD_TICKS VERY_COLD_TICKS DICT_TRAIN_SAMPLES
            PREFETCH_WINDOW COMPRESSOR_WORKERS COMPRESS_STORE_SHARDS
            LARGE_ALLOC_VM_THRESHOLD
            MAX_SLOTS_PER_PAGE COLD_ARENA_THRESHOLD
            ADAPTIVE_CAP_TARGET_PCT ADAPTIVE_CAP_MIN ADAPTIVE_CAP_MIN_SAMPLES)
  if(DEFINED SMASH_${_var} AND NOT "${SMASH_${_var}}" STREQUAL "")
    target_compile_definitions(smash PRIVATE "SMASH_${_var}=${SMASH_${_var}}")
  endif()
endforeach()
# Boolean ablation flags (define to enable, must be truthy)
foreach(_flag ABLATION_NO_ZERO_EAGER ABLATION_NO_ZERO_DEFERRED
              ABLATION_NO_SKIP_STATS ABLATION_NO_CHUNK_BITMAP
              ABLATION_NO_CALLSITE_ARENA USE_LZ4
              COLD_ARENA_FEEDBACK PAGE_LOCAL_BATCH THREAD_ARENA_HASH
              ADAPTIVE_CAP MEASURE_COHORTS)
  if(SMASH_${_flag})
    target_compile_definitions(smash PRIVATE "SMASH_${_flag}=1")
  endif()
endforeach()

if(APPLE)
  set_target_properties(smash PROPERTIES
    SUFFIX ".dylib"
  )
endif()

# ── libsmash_noopt: compression without allocation optimizations ─────────────
# Same code, but arenas=1, no zero-on-free (eager or deferred).
# Used to isolate the contribution of Smash's allocation design.
add_library(smash_noopt SHARED
  src/smash_heap.cpp
  src/linux_syscall_wrappers.cpp
  ${printf_SOURCE_DIR}/printf.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
  ${ALLOC8_THREAD_SOURCES}
  ${_SMASH_COMMON_SOURCES}
)

target_include_directories(smash_noopt PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${printf_SOURCE_DIR}
)

# On Linux, use custom version script (same as smash target)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(smash_noopt PRIVATE alloc8::headers pthread dl)
  target_link_options(smash_noopt PRIVATE
    "LINKER:--version-script=${SMASH_VERSION_SCRIPT}"
    "-Bsymbolic"
  )
else()
  target_link_libraries(smash_noopt PRIVATE alloc8::interpose)
endif()
target_link_libraries(smash_noopt PRIVATE lz4_static libzstd_static Threads::Threads ${CMAKE_DL_LIBS})

set_target_properties(smash_noopt PROPERTIES
  OUTPUT_NAME "smash_noopt"
  PREFIX "lib"
)

target_compile_definitions(smash_noopt PRIVATE
  SMASH_ABLATION_NO_CALLSITE_ARENA=1
  SMASH_ABLATION_NO_ZERO_EAGER=1
  SMASH_ABLATION_NO_ZERO_DEFERRED=1
)

if(APPLE)
  set_target_properties(smash_noopt PROPERTIES SUFFIX ".dylib")
endif()

# ── libsmash_compress_only: DEPRECATED - use libsmash with SMASH_MODE=compress_only
# Kept for backwards compatibility. The main libsmash library now supports
# compress-only mode at runtime: SMASH_MODE=compress_only LD_PRELOAD=libsmash.so
#
# This separate library can still be useful when you want a smaller binary
# that doesn't include the full Smash allocator code.
add_library(smash_compress_only SHARED
  src/compress_only/co_interpose_mac.cpp
  src/compress_only/co_interpose_linux.cpp
  ${printf_SOURCE_DIR}/printf.cpp
)

target_include_directories(smash_compress_only PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${printf_SOURCE_DIR}
)

# Only needs alloc8 headers (for platform.h), NOT the interpose machinery
target_link_libraries(smash_compress_only PRIVATE alloc8::headers lz4_static libzstd_static Threads::Threads ${CMAKE_DL_LIBS})

set_target_properties(smash_compress_only PROPERTIES
  OUTPUT_NAME "smash_compress_only"
  PREFIX "lib"
)

target_compile_definitions(smash_compress_only PRIVATE SMASH_COMPRESS_ONLY=1)

# No fno-builtin-malloc since we want to forward to the real system malloc
target_compile_options(smash_compress_only PRIVATE
  -fvisibility=hidden -Wall -Wextra -Wpedantic
)

if(APPLE)
  set_target_properties(smash_compress_only PROPERTIES SUFFIX ".dylib")
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
option(SMASH_BUILD_TESTS "Build smash tests" ON)
if(SMASH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ── Benchmarks ───────────────────────────────────────────────────────────────
option(SMASH_BUILD_BENCH "Build smash benchmarks" OFF)
if(SMASH_BUILD_BENCH)
  add_subdirectory(bench)
endif()
