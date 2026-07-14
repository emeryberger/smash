#
# ─── EXTERNAL ALLOCATORS (for benchmarking) ────────────────────────────
#
# Builds mimalloc, jemalloc, tcmalloc (gperftools) and Hoard as shared
# libraries so they can be compared against smash via LD_PRELOAD /
# DYLD_INSERT_LIBRARIES.  Mirrors the allocators/ directory in Hoard.
#
# This is orthogonal to SMASH_BUILD_BENCH_ALLOCATORS in bench/, which links
# allocators *into* the bench_alloc_* binaries and relies on find_library for
# jemalloc/hoard.  This directory instead produces the *preloadable* libraries
# that run_paper_experiments.py / bench_allocator_compare.py can swap in for
# libsmash on an unmodified application.
#
# This directory is pulled in by the top-level CMakeLists via
# add_subdirectory(allocators).  Nothing here builds unless a BUILD_* option is
# turned on, so a normal smash build is unaffected.
#
#   cmake .. -DBUILD_MIMALLOC=ON            # one allocator
#   cmake .. -DBUILD_ALL_ALLOCATORS=ON      # all of them
#   make allocators                         # build them all
#
# The resulting shared libraries are collected in:
#   ${CMAKE_BINARY_DIR}/allocators/lib
#
# On Linux all four interpose under LD_PRELOAD (verified on Ubuntu 22.04 /
# glibc 2.35).
#
# macOS caveat (measured, arm64, 2026-07): only mimalloc and Hoard actually take
# over malloc under DYLD_INSERT_LIBRARIES.  mimalloc installs a malloc zone from
# a constructor; Hoard uses alloc8's __interpose section.  jemalloc and
# gperftools export malloc/free but nothing binds to them — under the two-level
# namespace the app still calls libsystem_malloc, and both register their zone
# lazily on their *own* first malloc, which therefore never runs.  They are
# still built on macOS (the bench_alloc_* binaries link them directly, and
# preloading works normally on Linux), but do not expect them to interpose here.
#
# Everything is built with ExternalProject_Add (its own build tree) rather than
# FetchContent.  bench/CMakeLists.txt already FetchContent's mimalloc as a
# *static* lib (mimalloc-static); a second FetchContent of the same name would
# fight over MI_BUILD_SHARED/MI_BUILD_STATIC and the first declaration would
# silently win.  Separate build trees keep the two uses independent.
#

# ─── Options ───────────────────────────────────────────────────────────
option(BUILD_ALL_ALLOCATORS "Build all external comparison allocators" OFF)
option(BUILD_MIMALLOC "Build mimalloc for benchmarking"  ${BUILD_ALL_ALLOCATORS})
option(BUILD_JEMALLOC "Build jemalloc for benchmarking"  ${BUILD_ALL_ALLOCATORS})
option(BUILD_TCMALLOC "Build tcmalloc (gperftools) for benchmarking" ${BUILD_ALL_ALLOCATORS})
option(BUILD_HOARD    "Build Hoard for benchmarking"     ${BUILD_ALL_ALLOCATORS})

if(NOT (BUILD_MIMALLOC OR BUILD_JEMALLOC OR BUILD_TCMALLOC OR BUILD_HOARD))
  # Nothing requested; do not fetch or configure anything.
  return()
endif()

if(WIN32)
  message(WARNING
    "External allocator builds (BUILD_MIMALLOC/JEMALLOC/TCMALLOC/HOARD) are "
    "only wired up for Unix/macOS; skipping on Windows.")
  return()
endif()

include(ExternalProject)
include(ProcessorCount)

ProcessorCount(_ALLOC_NCPU)
if(_ALLOC_NCPU EQUAL 0)
  set(_ALLOC_NCPU 4)
endif()

# Version pins. Bump these to test newer releases.
set(MIMALLOC_TAG   "v2.1.7"    CACHE STRING "mimalloc git tag to build")
set(JEMALLOC_TAG   "5.3.0"     CACHE STRING "jemalloc git tag to build")
set(GPERFTOOLS_TAG "gperftools-2.16" CACHE STRING "gperftools git tag to build")
set(HOARD_TAG      "master"    CACHE STRING "Hoard git tag to build")
# Point at a local Hoard checkout instead of cloning (e.g. -DHOARD_DIR=~/git/Hoard).
set(HOARD_DIR      ""          CACHE PATH   "Local Hoard checkout to build instead of cloning")

# Common output directory for the built shared libraries.
set(ALLOC_LIB_DIR "${CMAKE_BINARY_DIR}/allocators/lib")
file(MAKE_DIRECTORY "${ALLOC_LIB_DIR}")

if(APPLE)
  set(_shlib ".dylib")
else()
  set(_shlib ".so")
endif()

set(_alloc_targets "")

# ─── mimalloc (CMake native) ───────────────────────────────────────────
if(BUILD_MIMALLOC)
  message(STATUS "External allocator: mimalloc ${MIMALLOC_TAG}")

  # MI_OVERRIDE=ON makes the shared library export plain malloc/free, which is
  # what makes it interpose the system allocator under LD_PRELOAD.
  ExternalProject_Add(mimalloc_ext
    GIT_REPOSITORY    https://github.com/microsoft/mimalloc.git
    GIT_TAG           ${MIMALLOC_TAG}
    GIT_SHALLOW       TRUE
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DMI_BUILD_SHARED=ON
      -DMI_BUILD_STATIC=OFF
      -DMI_BUILD_OBJECT=OFF
      -DMI_BUILD_TESTS=OFF
      -DMI_OVERRIDE=ON
    BUILD_COMMAND     ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${_ALLOC_NCPU}
    # mimalloc installs into lib/mimalloc-<ver>/; glob rather than guess.
    INSTALL_COMMAND   ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
    COMMAND           ${CMAKE_COMMAND}
                        -DSRC_DIR=<BINARY_DIR>
                        -DDEST_DIR=${ALLOC_LIB_DIR}
                        -DPATTERN=libmimalloc*${_shlib}*
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/collect_lib.cmake
    LOG_CONFIGURE     TRUE
    LOG_BUILD         TRUE
    LOG_INSTALL       TRUE
  )
  list(APPEND _alloc_targets mimalloc_ext)
endif()

# ─── jemalloc (autotools: configure && make) ───────────────────────────
if(BUILD_JEMALLOC)
  message(STATUS "External allocator: jemalloc ${JEMALLOC_TAG}")

  # --with-jemalloc-prefix= (empty) makes jemalloc export plain malloc/free so
  # it interposes the system allocator under LD_PRELOAD / DYLD_INSERT_LIBRARIES.
  # (On macOS jemalloc otherwise defaults to a "je_" prefix and does NOT
  # intercept; Linux already defaults to no prefix.)
  ExternalProject_Add(jemalloc_ext
    GIT_REPOSITORY    https://github.com/jemalloc/jemalloc.git
    GIT_TAG           ${JEMALLOC_TAG}
    GIT_SHALLOW       TRUE
    BUILD_IN_SOURCE   TRUE
    CONFIGURE_COMMAND ./autogen.sh --prefix=<INSTALL_DIR> --disable-debug
                        --with-jemalloc-prefix=
    BUILD_COMMAND     make build_lib_shared -j${_ALLOC_NCPU}
    INSTALL_COMMAND   make install_lib_shared install_include
    COMMAND           ${CMAKE_COMMAND}
                        -DSRC_DIR=<INSTALL_DIR>/lib
                        -DDEST_DIR=${ALLOC_LIB_DIR}
                        -DPATTERN=libjemalloc*${_shlib}*
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/collect_lib.cmake
    LOG_CONFIGURE     TRUE
    LOG_BUILD         TRUE
    LOG_INSTALL       TRUE
  )
  list(APPEND _alloc_targets jemalloc_ext)
endif()

# ─── tcmalloc / gperftools (CMake native since 2.11) ───────────────────
#
# gperftools, not google/tcmalloc: the Bazel-built google/tcmalloc needs
# glibc >= 2.35 for RSEQ TLS (bench/CMakeLists.txt has that path, with a
# gperftools fallback).  For preload-comparison purposes gperftools is the
# portable choice and builds without Bazel.
if(BUILD_TCMALLOC)
  message(STATUS "External allocator: gperftools/tcmalloc ${GPERFTOOLS_TAG}")

  ExternalProject_Add(tcmalloc_ext
    GIT_REPOSITORY    https://github.com/gperftools/gperftools.git
    GIT_TAG           ${GPERFTOOLS_TAG}
    GIT_SHALLOW       TRUE
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DBUILD_SHARED_LIBS=ON
      -DBUILD_TESTING=OFF
      -DGPERFTOOLS_BUILD_STATIC=OFF
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    BUILD_COMMAND     ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${_ALLOC_NCPU}
    # gperftools' CMake build defines no install() rules; the shared libraries
    # are produced directly in the build dir.  Skip install and copy from there.
    INSTALL_COMMAND   ${CMAKE_COMMAND}
                        -DSRC_DIR=<BINARY_DIR>
                        -DDEST_DIR=${ALLOC_LIB_DIR}
                        -DPATTERN=libtcmalloc*${_shlib}*
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/collect_lib.cmake
    LOG_CONFIGURE     TRUE
    LOG_BUILD         TRUE
    LOG_INSTALL       TRUE
  )
  list(APPEND _alloc_targets tcmalloc_ext)
endif()

# ─── Hoard (CMake native) ──────────────────────────────────────────────
if(BUILD_HOARD)
  if(HOARD_DIR)
    message(STATUS "External allocator: Hoard (local checkout ${HOARD_DIR})")
    set(_hoard_source SOURCE_DIR "${HOARD_DIR}")
  else()
    message(STATUS "External allocator: Hoard ${HOARD_TAG}")
    set(_hoard_source
      GIT_REPOSITORY https://github.com/emeryberger/Hoard.git
      GIT_TAG        ${HOARD_TAG}
      GIT_SHALLOW    TRUE)
  endif()

  ExternalProject_Add(hoard_ext
    ${_hoard_source}
    # Build out of tree even for a local checkout, so we never touch the user's
    # own build directories there.
    BUILD_IN_SOURCE   FALSE
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    BUILD_COMMAND     ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${_ALLOC_NCPU}
    INSTALL_COMMAND   ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
    COMMAND           ${CMAKE_COMMAND}
                        -DSRC_DIR=<BINARY_DIR>
                        -DDEST_DIR=${ALLOC_LIB_DIR}
                        -DPATTERN=libhoard*${_shlib}*
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/collect_lib.cmake
    LOG_CONFIGURE     TRUE
    LOG_BUILD         TRUE
    LOG_INSTALL       TRUE
  )
  list(APPEND _alloc_targets hoard_ext)
endif()

# ─── Aggregate target: `make allocators` ───────────────────────────────
add_custom_target(allocators DEPENDS ${_alloc_targets})

message(STATUS "External allocator libraries will be collected in: ${ALLOC_LIB_DIR}")

if(APPLE AND (BUILD_JEMALLOC OR BUILD_TCMALLOC))
  message(STATUS
    "  note: on macOS only mimalloc and Hoard interpose under "
    "DYLD_INSERT_LIBRARIES; jemalloc/tcmalloc do not (see comment above).")
endif()
