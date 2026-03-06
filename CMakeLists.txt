cmake_minimum_required(VERSION 3.15)
project(smash
  VERSION 0.1.0
  LANGUAGES C CXX
  DESCRIPTION "Compression-aware memory allocator"
)

# Allow FetchContent_Populate for older CMake compatibility
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

# ── C++ Standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
# Look for alloc8 as a sibling directory, or allow override via ALLOC8_DIR
set(ALLOC8_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../alloc8" CACHE PATH "Path to alloc8 source")
if(NOT EXISTS "${ALLOC8_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "alloc8 not found at ${ALLOC8_DIR}. Set ALLOC8_DIR to the alloc8 source directory.")
endif()
add_subdirectory("${ALLOC8_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/alloc8")

# ── LZ4 compression library ──────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
  lz4
  GIT_REPOSITORY https://github.com/lz4/lz4.git
  GIT_TAG v1.9.4
  GIT_SHALLOW TRUE
)
FetchContent_GetProperties(lz4)
if(NOT lz4_POPULATED)
  FetchContent_Populate(lz4)
  add_library(lz4_static STATIC ${lz4_SOURCE_DIR}/lib/lz4.c)
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
add_library(smash SHARED
  src/smash_heap.cpp
  ${ALLOC8_INTERPOSE_SOURCES}
  ${ALLOC8_THREAD_SOURCES}
  ${ALLOC8_COMMON_SOURCES}
)

target_include_directories(smash PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(smash PRIVATE alloc8::interpose lz4_static libzstd_static)

set_target_properties(smash PROPERTIES
  OUTPUT_NAME "smash"
  PREFIX "lib"
)

# ── Ablation defines (override config.h defaults via -DSMASH_*=value) ────────
# Numeric overrides (only when non-empty)
foreach(_var NUM_ARENAS COLD_TICKS VERY_COLD_TICKS DICT_TRAIN_SAMPLES
            PREFETCH_WINDOW COMPRESSOR_WORKERS COMPRESS_STORE_SHARDS
            LARGE_ALLOC_VM_THRESHOLD)
  if(DEFINED SMASH_${_var} AND NOT "${SMASH_${_var}}" STREQUAL "")
    target_compile_definitions(smash PRIVATE "SMASH_${_var}=${SMASH_${_var}}")
  endif()
endforeach()
# Boolean ablation flags (define to enable, must be truthy)
foreach(_flag ABLATION_NO_ZERO_EAGER ABLATION_NO_ZERO_DEFERRED
              ABLATION_NO_SKIP_STATS ABLATION_NO_CHUNK_BITMAP)
  if(SMASH_${_flag})
    target_compile_definitions(smash PRIVATE "SMASH_${_flag}=1")
  endif()
endforeach()

if(APPLE)
  set_target_properties(smash PROPERTIES
    SUFFIX ".dylib"
  )
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
