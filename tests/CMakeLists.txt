# smash/tests/CMakeLists.txt
# Each test is a separate executable that tests internal components directly.

# Build tests as arm64-only on macOS. The fat arm64+arm64e binary (needed for
# libsmash.dylib to interpose system arm64e processes) causes SIGILL in the
# arm64e slice for tests that exercise the fault handler — the thread_local
# lazy-init guard traps when first run inside a signal handler under pointer
# authentication. Tests don't need arm64e; they test allocator correctness.
if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64")
endif()

set(SMASH_TEST_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
  ${printf_SOURCE_DIR}
)

# Helper to add a test executable. Tests transitively pull in headers that
# depend on the mpaland/printf TU (via util/safe_printf.h), so every test
# must link printf.cpp.
function(smash_add_test name source)
  add_executable(${name} ${source} ${printf_SOURCE_DIR}/printf.cpp)
  target_include_directories(${name} PRIVATE ${SMASH_TEST_INCLUDE_DIRS})
  target_link_libraries(${name} PRIVATE alloc8::headers)
  if(APPLE)
    target_compile_options(${name} PRIVATE -ftls-model=initial-exec)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

smash_add_test(test_size_classes test_size_classes.cpp)
smash_add_test(test_bootstrap test_bootstrap.cpp)
smash_add_test(test_span test_span.cpp)
smash_add_test(test_slab test_slab.cpp)

# VM region test (no LZ4 dependency)
smash_add_test(test_vm_region test_vm_region.cpp)

# Thread-exit alternate-stack release leaves no registered (dangling) stack
smash_add_test(test_altstack_release test_altstack_release.cpp)

# SpillStore test (file-backed very-cold blob storage; no codec dependency)
smash_add_test(test_spill_store test_spill_store.cpp)

# Integration test includes smash_heap.cpp for thread cache methods (needs LZ4 + zstd)
add_executable(test_integration
  test_integration.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/smash_heap.cpp
  ${printf_SOURCE_DIR}/printf.cpp
)
target_include_directories(test_integration PRIVATE ${SMASH_TEST_INCLUDE_DIRS})
target_link_libraries(test_integration PRIVATE alloc8::headers lz4_static libzstd_static ${CMAKE_DL_LIBS})
if(APPLE)
  target_compile_options(test_integration PRIVATE -ftls-model=initial-exec)
endif()
add_test(NAME test_integration COMMAND test_integration)

# Compression test (needs LZ4 + zstd)
add_executable(test_compression test_compression.cpp ${printf_SOURCE_DIR}/printf.cpp)
target_include_directories(test_compression PRIVATE ${SMASH_TEST_INCLUDE_DIRS})
target_link_libraries(test_compression PRIVATE alloc8::headers lz4_static libzstd_static)
if(APPLE)
  target_compile_options(test_compression PRIVATE -ftls-model=initial-exec)
endif()
add_test(NAME test_compression COMMAND test_compression)
# Drive the compressor through ticks deterministically — bypass the
# CPU-pressure hard-skip and floor multiplier with an explicit
# cold-ticks override. Also pin SMASH_SOFTDIRTY=0 because this test
# directly checks Phase-3 PROT_READ states (ACTIVE_MONITORING) which
# don't exist under the soft-dirty path.
set_tests_properties(test_compression PROPERTIES
  ENVIRONMENT "SMASH_COLD_TICKS=2;SMASH_SOFTDIRTY=0;SMASH_FIXAV=0")

# Compression-ratio test (needs LZ4 + zstd): verifies the engine hits the
# paper's claimed ratios (RQ2), not just "compressed > 0". In-process,
# direct-linked, no compressor thread — a deterministic guard on the codec.
add_executable(compression_ratio_test compression_ratio_test.cpp ${printf_SOURCE_DIR}/printf.cpp)
target_include_directories(compression_ratio_test PRIVATE ${SMASH_TEST_INCLUDE_DIRS})
target_link_libraries(compression_ratio_test PRIVATE alloc8::headers lz4_static libzstd_static)
if(APPLE)
  target_compile_options(compression_ratio_test PRIVATE -ftls-model=initial-exec)
endif()
add_test(NAME test_compression_ratio COMMAND compression_ratio_test)

# Dictionary training test (needs LZ4 + zstd)
add_executable(test_dictionary test_dictionary.cpp ${printf_SOURCE_DIR}/printf.cpp)
target_include_directories(test_dictionary PRIVATE ${SMASH_TEST_INCLUDE_DIRS})
target_link_libraries(test_dictionary PRIVATE alloc8::headers lz4_static libzstd_static)
if(APPLE)
  target_compile_options(test_dictionary PRIVATE -ftls-model=initial-exec)
endif()
add_test(NAME test_dictionary COMMAND test_dictionary)

# Profile + dictionary persistence test (needs LZ4 + zstd). Validates the
# v4 SMASH_PROFILE_FILE format roundtrip — train, save, load, verify dict
# is usable — plus corrupt-file rejection.
add_executable(test_profile_dict_persistence test_profile_dict_persistence.cpp ${printf_SOURCE_DIR}/printf.cpp)
target_include_directories(test_profile_dict_persistence PRIVATE ${SMASH_TEST_INCLUDE_DIRS})
target_link_libraries(test_profile_dict_persistence PRIVATE alloc8::headers lz4_static libzstd_static)
if(APPLE)
  target_compile_options(test_profile_dict_persistence PRIVATE -ftls-model=initial-exec)
endif()
add_test(NAME test_profile_dict_persistence COMMAND test_profile_dict_persistence)

# Prefetch test (needs LZ4 + zstd)
add_executable(test_prefetch test_prefetch.cpp ${printf_SOURCE_DIR}/printf.cpp)
target_include_directories(test_prefetch PRIVATE ${SMASH_TEST_INCLUDE_DIRS})
target_link_libraries(test_prefetch PRIVATE alloc8::headers lz4_static libzstd_static)
if(APPLE)
  target_compile_options(test_prefetch PRIVATE -ftls-model=initial-exec)
endif()
add_test(NAME test_prefetch COMMAND test_prefetch)
# Pin SMASH_SOFTDIRTY=0 — test directly drives PROT_READ-based Phase 3.
set_tests_properties(test_prefetch PROPERTIES
  ENVIRONMENT "SMASH_COLD_TICKS=2;SMASH_SOFTDIRTY=0;SMASH_FIXAV=0")

# Contention test (needs LZ4 + zstd + smash_heap.cpp)
add_executable(test_contention
  test_contention.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/smash_heap.cpp
  ${printf_SOURCE_DIR}/printf.cpp
)
target_include_directories(test_contention PRIVATE ${SMASH_TEST_INCLUDE_DIRS})
target_link_libraries(test_contention PRIVATE alloc8::headers lz4_static libzstd_static ${CMAKE_DL_LIBS})
if(APPLE)
  target_compile_options(test_contention PRIVATE -ftls-model=initial-exec)
endif()
add_test(NAME test_contention COMMAND test_contention)

# Exit path: stop() keeps compressed pages compressed and the fault handler
# installed; a read after stop() decompresses on demand (needs LZ4 + zstd).
add_executable(test_stop_no_drain test_stop_no_drain.cpp ${printf_SOURCE_DIR}/printf.cpp)
target_include_directories(test_stop_no_drain PRIVATE ${SMASH_TEST_INCLUDE_DIRS})
target_link_libraries(test_stop_no_drain PRIVATE alloc8::headers lz4_static libzstd_static)
if(APPLE)
  target_compile_options(test_stop_no_drain PRIVATE -ftls-model=initial-exec)
endif()
add_test(NAME test_stop_no_drain COMMAND test_stop_no_drain)
set_tests_properties(test_stop_no_drain PROPERTIES
  ENVIRONMENT "SMASH_COLD_TIMEOUT_SEC=1;SMASH_NO_ATEXIT=1;SMASH_DEFER_PHASES_MS=0"
  TIMEOUT 90)

# Fault cycle test - real SIGSEGV handling (needs LZ4 + zstd)
add_executable(test_fault_cycle test_fault_cycle.cpp ${printf_SOURCE_DIR}/printf.cpp)
target_include_directories(test_fault_cycle PRIVATE ${SMASH_TEST_INCLUDE_DIRS})
target_link_libraries(test_fault_cycle PRIVATE alloc8::headers lz4_static libzstd_static)
if(APPLE)
  target_compile_options(test_fault_cycle PRIVATE -ftls-model=initial-exec)
endif()
add_test(NAME test_fault_cycle COMMAND test_fault_cycle)
# This test exercises the same compress→fault→recompress pattern that the
# recompression-thrash back-off is designed to suppress, and runs on busy CI
# runners where the cold-tick floor multiplier would otherwise keep pages
# from being compressed within the test's tick budget. Disable both for this
# test so it can verify the underlying fault-cycle correctness.
# SMASH_COLD_TIMEOUT_SEC=1 sets cold_ticks_overridden, which bypasses the
# CPU-pressure floor multiplier (see compressor_thread.h::phase2Range).
set_tests_properties(test_fault_cycle PROPERTIES
  ENVIRONMENT "SMASH_RECOMPRESS_BACKOFF=0;SMASH_COLD_TIMEOUT_SEC=1;SMASH_FIXAV=0"
  LABELS "known_issue"
  DISABLED TRUE)  # Signal-delivery timing varies across CI environments

# Interposition test: built without alloc8 linkage, run under DYLD_INSERT
add_executable(interpose_test interpose_test.cpp)
set_target_properties(interpose_test PROPERTIES
  CXX_STANDARD 20
)
add_test(
  NAME test_interpose
  COMMAND ${CMAKE_COMMAND} -E env
    "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>"
    $<TARGET_FILE:interpose_test>
)

# External-mapping test: exercises the mmap / Mach VM interposers under
# a live compressor. Same DYLD/LD_PRELOAD pattern as test_interpose;
# requires SMASH_LARGE_ONLY=0 + a short SMASH_COLD_TIMEOUT_SEC so the
# compressor processes pages within the test's 3 s sleep.
add_executable(external_mapping_test external_mapping_test.cpp)
set_target_properties(external_mapping_test PROPERTIES
  CXX_STANDARD 20
)
if(APPLE)
  add_test(
    NAME test_external_mapping
    COMMAND ${CMAKE_COMMAND} -E env
      "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>"
      "MallocNanoZone=0"
      "SMASH_LARGE_ONLY=0"
      "SMASH_DEFER_PHASES_MS=0"
      "SMASH_COLD_TIMEOUT_SEC=1"
      "SMASH_TRACK_EXTERNAL=1"
      $<TARGET_FILE:external_mapping_test>
  )
else()
  add_test(
    NAME test_external_mapping
    COMMAND ${CMAKE_COMMAND} -E env
      "LD_PRELOAD=$<TARGET_FILE:smash>"
      "SMASH_LARGE_ONLY=0"
      "SMASH_DEFER_PHASES_MS=0"
      "SMASH_COLD_TIMEOUT_SEC=1"
      "SMASH_TRACK_EXTERNAL=1"
      $<TARGET_FILE:external_mapping_test>
  )
endif()
set_tests_properties(test_external_mapping PROPERTIES
  TIMEOUT 60)
# Re-enabled: the OOB-on-external-pages crash class was root-caused and fixed
# in issue #84 (self-interposition registered smash's own direct-mmap fallback
# pages as external; free-path routing then poisoned the arena free list).

# External-tracking allocator-pattern regressions: the mmap/madvise/mprotect/
# mremap/fork/churn patterns bundled allocators (jemalloc, mimalloc), CPython
# arenas and coroutine stacks apply to memory smash has registered and
# compressed. One executable, run in BOTH malloc modes (external tracking is
# independent of SMASH_LARGE_ONLY).
add_executable(external_patterns_test external_patterns_test.cpp)
set_target_properties(external_patterns_test PROPERTIES CXX_STANDARD 20)
function(add_external_patterns_test test_name large_only)
  if(APPLE)
    set(_preload "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>" "MallocNanoZone=0")
  else()
    set(_preload "LD_PRELOAD=$<TARGET_FILE:smash>")
  endif()
  add_test(
    NAME ${test_name}
    COMMAND ${CMAKE_COMMAND} -E env
      ${_preload}
      "SMASH_LARGE_ONLY=${large_only}"
      "SMASH_DEFER_PHASES_MS=0"
      "SMASH_COLD_TIMEOUT_SEC=1"
      "SMASH_TRACK_EXTERNAL=1"
      $<TARGET_FILE:external_patterns_test>
  )
  # ~85 s on an idle 24-core slice (each case waits for real compression).
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 300)
endfunction()
add_external_patterns_test(test_external_patterns 0)             # full mode
add_external_patterns_test(test_external_patterns_large_only 1)  # large-only

# stdio first-fill EFAULT: the lazily malloc'd stdio buffer can land on a
# compressed page; libc's internal read() into it must not fail the stream.
add_executable(stdio_first_fill_test stdio_first_fill_test.cpp)
set_target_properties(stdio_first_fill_test PROPERTIES CXX_STANDARD 20)
foreach(_lo 0 1)
  if(_lo STREQUAL "0")
    set(_name test_stdio_first_fill)
  else()
    set(_name test_stdio_first_fill_large_only)
  endif()
  if(APPLE)
    set(_preload "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>" "MallocNanoZone=0")
  else()
    set(_preload "LD_PRELOAD=$<TARGET_FILE:smash>")
  endif()
  add_test(NAME ${_name}
    COMMAND ${CMAKE_COMMAND} -E env ${_preload}
      "SMASH_LARGE_ONLY=${_lo}" "SMASH_DEFER_PHASES_MS=0" "SMASH_COLD_TIMEOUT_SEC=1"
      $<TARGET_FILE:stdio_first_fill_test>)
  set_tests_properties(${_name} PROPERTIES TIMEOUT 120)
endforeach()

# Realloc / usable-size contract under interposition, both malloc modes. On
# macOS it replays SQLite mem1.c's pattern (malloc_zone_* on
# malloc_default_zone() + default_zone->size()) — the large-only bench_sqlite
# hang, where system-passthrough pointers sized as 0 and growOpArray() spun
# forever. The loop is bounded, so a regression fails instead of hanging.
add_executable(realloc_size_test realloc_size_test.cpp)
set_target_properties(realloc_size_test PROPERTIES CXX_STANDARD 20)
foreach(_lo 0 1)
  if(_lo STREQUAL "0")
    set(_name test_realloc_size)
  else()
    set(_name test_realloc_size_large_only)
  endif()
  if(APPLE)
    set(_preload "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>" "MallocNanoZone=0")
  else()
    set(_preload "LD_PRELOAD=$<TARGET_FILE:smash>")
  endif()
  add_test(NAME ${_name}
    COMMAND ${CMAKE_COMMAND} -E env ${_preload} "SMASH_LARGE_ONLY=${_lo}"
      $<TARGET_FILE:realloc_size_test>)
  set_tests_properties(${_name} PROPERTIES TIMEOUT 30)
endforeach()

# Mode compression test: ONE executable (mode_compression_test.cpp), run twice
# with SMASH_LARGE_ONLY={0,1}, so FULL mode (test_malloc_compression) and
# LARGE-ONLY mode (test_large_only_compression) execute byte-identical
# assertions and cannot drift apart in coverage. The test allocates large
# (>= 1 MiB, smash-managed -> compressible VmRegion in BOTH modes) and small
# (<= 16 KiB, slab in full / system-passthrough in large-only) compressible
# chunks, asserts compressed>0 via SIGUSR2 stats, then verifies a byte-exact
# read-back of BOTH classes. The mode is read from the env at runtime, never a
# compile flag — that's what guarantees the assertions are identical.
add_executable(mode_compression_test mode_compression_test.cpp)
set_target_properties(mode_compression_test PROPERTIES CXX_STANDARD 20)

# add_mode_compression_test(<ctest-name> <SMASH_LARGE_ONLY value>)
function(add_mode_compression_test test_name large_only)
  if(APPLE)
    add_test(
      NAME ${test_name}
      COMMAND ${CMAKE_COMMAND} -E env
        "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>"
        "MallocNanoZone=0"
        "SMASH_LARGE_ONLY=${large_only}"
        "SMASH_DEFER_PHASES_MS=0"
        "SMASH_COLD_TIMEOUT_SEC=1"
        "SMASH_STATS_SIGNAL=1"
        $<TARGET_FILE:mode_compression_test>
    )
  else()
    add_test(
      NAME ${test_name}
      COMMAND ${CMAKE_COMMAND} -E env
        "LD_PRELOAD=$<TARGET_FILE:smash>"
        "SMASH_LARGE_ONLY=${large_only}"
        "SMASH_DEFER_PHASES_MS=0"
        "SMASH_COLD_TIMEOUT_SEC=1"
        "SMASH_STATS_SIGNAL=1"
        $<TARGET_FILE:mode_compression_test>
    )
  endif()
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 60)
endfunction()

add_mode_compression_test(test_malloc_compression 0)      # full mode
add_mode_compression_test(test_large_only_compression 1)  # large-only mode

# Observe (dry-run) mode: SMASH_MODE=observe, ONE executable run in both full
# and large-only mode. Asserts cold regions are projected, a hot region is not,
# rewriting a projected region counts refaults, nothing is really compressed
# (SIGUSR2 stats compressed=0 shadow=0), RSS keeps every page, and all data
# reads back byte-exact. Reads the JSON report SMASH_OBSERVE_REPORT rewrites
# every SMASH_OBSERVE_INTERVAL=1 s.
add_executable(observe_mode_test observe_mode_test.cpp)
set_target_properties(observe_mode_test PROPERTIES CXX_STANDARD 20)
function(add_observe_mode_test test_name large_only)
  if(APPLE)
    set(_preload "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>" "MallocNanoZone=0")
  else()
    set(_preload "LD_PRELOAD=$<TARGET_FILE:smash>")
  endif()
  add_test(
    NAME ${test_name}
    COMMAND ${CMAKE_COMMAND} -E env
      ${_preload}
      "SMASH_MODE=observe"
      "SMASH_LARGE_ONLY=${large_only}"
      "SMASH_DEFER_PHASES_MS=0"
      "SMASH_COLD_TIMEOUT_SEC=1"
      "SMASH_STATS_SIGNAL=1"
      "SMASH_OBSERVE_INTERVAL=1"
      "SMASH_OBSERVE_REPORT=${CMAKE_CURRENT_BINARY_DIR}/${test_name}.json"
      $<TARGET_FILE:observe_mode_test>
  )
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
endfunction()
add_observe_mode_test(test_observe_full 0)
add_observe_mode_test(test_observe_large_only 1)
# Observe mode READ tracking through the idle-page bitmap (Linux; root via
# `sudo -n`, CONFIG_IDLE_PAGE_TRACKING; skipped with 77 otherwise): a read-hot
# region must not be projected and reads of a projected region must count as
# refaults_read. _root: smash opens the bitmap itself; _idled: unprivileged,
# through a smash-idled daemon the test starts for itself.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(observe_read_track_test observe_read_track_test.cpp)
  foreach(_lo 0 1)
    if(_lo)
      set(_sfx large_only)
      set(_arg large_only)
    else()
      set(_sfx full)
      set(_arg "")
    endif()
    add_test(NAME test_observe_read_root_${_sfx}
             COMMAND observe_read_track_test $<TARGET_FILE:smash> root ${_arg})
    add_test(NAME test_observe_read_idled_${_sfx}
             COMMAND observe_read_track_test $<TARGET_FILE:smash> idled $<TARGET_FILE:smash-idled> ${_arg})
    # _thp: run only with THP "always" (else 77): pages in 2 MiB folios.
    add_test(NAME test_observe_read_root_${_sfx}_thp
             COMMAND observe_read_track_test $<TARGET_FILE:smash> root ${_arg} thp)
    add_test(NAME test_observe_read_idled_${_sfx}_thp
             COMMAND observe_read_track_test $<TARGET_FILE:smash> idled $<TARGET_FILE:smash-idled> ${_arg} thp)
    set_tests_properties(test_observe_read_root_${_sfx} test_observe_read_idled_${_sfx}
                         test_observe_read_root_${_sfx}_thp test_observe_read_idled_${_sfx}_thp
                         PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
  endforeach()
endif()

# Observe-mode projection model (agents/estimator.md): the refault-backoff
# hold, the referenced-bits read signal (Linux soft-dirty) and refault
# feedback into the cold-floor backoff. Same executable in both modes.
add_executable(observe_projection_test observe_projection_test.cpp)
set_target_properties(observe_projection_test PROPERTIES CXX_STANDARD 20)
function(add_observe_projection_test test_name large_only)
  if(APPLE)
    set(_preload "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>" "MallocNanoZone=0")
  else()
    set(_preload "LD_PRELOAD=$<TARGET_FILE:smash>")
  endif()
  add_test(
    NAME ${test_name}
    COMMAND ${CMAKE_COMMAND} -E env
      ${_preload}
      "SMASH_MODE=observe"
      "SMASH_LARGE_ONLY=${large_only}"
      "SMASH_DEFER_PHASES_MS=0"
      "SMASH_COLD_TIMEOUT_SEC=6"
      "SMASH_OBSERVE_INTERVAL=1"
      "SMASH_OBSERVE_REPORT=${CMAKE_CURRENT_BINARY_DIR}/${test_name}.json"
      $<TARGET_FILE:observe_projection_test>
  )
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 240)
endfunction()
add_observe_projection_test(test_observe_projection_full 0)
add_observe_projection_test(test_observe_projection_large_only 1)

# SMASH_CGROUP (container memory-limit policy). Unit test: parsers, the pure
# policy decision, and the Monitor walking a fake pod/container cgroup tree.
smash_add_test(test_cgroup_monitor cgroup_monitor_test.cpp)

# End-to-end policy test, both modes: SMASH_CGROUP_ROOT points at a fake cgroup
# (written by the cgroup_fake_setup fixture) with ~1% headroom; the compressor
# must report the critical level in the SIGUSR2 stats line, still compress
# resident idle pages, and read every byte back intact.
add_executable(cgroup_policy_test cgroup_policy_test.cpp)
set_target_properties(cgroup_policy_test PROPERTIES CXX_STANDARD 20)
set(SMASH_FAKE_CGROUP_DIR ${CMAKE_CURRENT_BINARY_DIR}/fake_cgroup)
add_test(NAME cgroup_fake_setup
  COMMAND ${CMAKE_COMMAND} -DDIR=${SMASH_FAKE_CGROUP_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/make_fake_cgroup.cmake)
set_tests_properties(cgroup_fake_setup PROPERTIES FIXTURES_SETUP cgroup_fake)
function(add_cgroup_policy_test test_name large_only)
  if(APPLE)
    set(_preload "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>" "MallocNanoZone=0")
  else()
    set(_preload "LD_PRELOAD=$<TARGET_FILE:smash>")
  endif()
  add_test(
    NAME ${test_name}
    COMMAND ${CMAKE_COMMAND} -E env
      ${_preload}
      "SMASH_LARGE_ONLY=${large_only}"
      "SMASH_CGROUP=1"
      "SMASH_CGROUP_ROOT=${SMASH_FAKE_CGROUP_DIR}"
      "SMASH_DEFER_PHASES_MS=0"
      "SMASH_COLD_TIMEOUT_SEC=2"
      "SMASH_STATS_SIGNAL=1"
      $<TARGET_FILE:cgroup_policy_test>
  )
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED cgroup_fake)
endfunction()
add_cgroup_policy_test(test_cgroup_policy_full 0)
add_cgroup_policy_test(test_cgroup_policy_large_only 1)

# Shadow-reclaim regression guard: slab pages that hold freed slots must
# finish shadow reclaim. compressPage() zeroes free slots in the image it
# compresses, so the shadow content check has to mask the live page the same
# way; comparing the raw page discarded every such page forever.
add_executable(shadow_free_slots_test shadow_free_slots_test.cpp)
set_target_properties(shadow_free_slots_test PROPERTIES CXX_STANDARD 20)
if(APPLE)
  add_test(
    NAME test_shadow_free_slots
    COMMAND ${CMAKE_COMMAND} -E env
      "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>"
      "MallocNanoZone=0"
      "SMASH_DEFER_PHASES_MS=0"
      "SMASH_COLD_TIMEOUT_SEC=1"
      "SMASH_STATS_SIGNAL=1"
      $<TARGET_FILE:shadow_free_slots_test>
  )
else()
  add_test(
    NAME test_shadow_free_slots
    COMMAND ${CMAKE_COMMAND} -E env
      "LD_PRELOAD=$<TARGET_FILE:smash>"
      "SMASH_DEFER_PHASES_MS=0"
      "SMASH_COLD_TIMEOUT_SEC=1"
      "SMASH_STATS_SIGNAL=1"
      $<TARGET_FILE:shadow_free_slots_test>
  )
endif()
set_tests_properties(test_shadow_free_slots PROPERTIES TIMEOUT 60)

# Arena-routing regression guard: runs under interposition and asserts that
# allocations from many distinct call sites do NOT all collapse into one arena
# (the LTO/wrapper return-address bug). Full mode so 256 B allocations take the
# slab path and receive an arena; CPU/thread hashing pinned off so the arena is
# a pure function of the return address. Fast + deterministic (no compressor).
add_executable(arena_routing_test arena_routing_test.cpp)
set_target_properties(arena_routing_test PROPERTIES CXX_STANDARD 20)
target_link_libraries(arena_routing_test PRIVATE ${CMAKE_DL_LIBS})
if(APPLE)
  add_test(
    NAME test_arena_routing
    COMMAND ${CMAKE_COMMAND} -E env
      "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>"
      "MallocNanoZone=0"
      "SMASH_LARGE_ONLY=0"
      "SMASH_CPU_ARENA=0"
      $<TARGET_FILE:arena_routing_test>
  )
else()
  add_test(
    NAME test_arena_routing
    COMMAND ${CMAKE_COMMAND} -E env
      "LD_PRELOAD=$<TARGET_FILE:smash>"
      "SMASH_LARGE_ONLY=0"
      "SMASH_CPU_ARENA=0"
      $<TARGET_FILE:arena_routing_test>
  )
endif()
set_tests_properties(test_arena_routing PROPERTIES TIMEOUT 30)

# SMASH_TRACK_EXTERNAL unset = auto: external mappings are compressed iff
# smash reclaims through userfaultfd. Linux: expectation detected (is smash's
# uffd open), plus SMASH_UFFD=0 -> off; macOS: off. Both modes. --unset keeps
# the TE-forced matrix from overriding the setting under test.
add_executable(te_auto_test te_auto_test.cpp)
set_target_properties(te_auto_test PROPERTIES CXX_STANDARD 20)
target_link_libraries(te_auto_test PRIVATE ${CMAKE_DL_LIBS})
if(APPLE)
  set(_te_pre "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>" "MallocNanoZone=0")
  set(_te_cases "off|")
else()
  set(_te_pre "LD_PRELOAD=$<TARGET_FILE:smash>")
  set(_te_cases "detect|" "off|SMASH_UFFD=0")
endif()
foreach(lo 0 1)
  foreach(c ${_te_cases})
    string(REPLACE "|" ";" _parts "${c}")
    list(GET _parts 0 _expect)
    list(LENGTH _parts _np)
    set(_extra "")
    set(_name test_te_auto)
    if(_np GREATER 1)
      list(GET _parts 1 _extra)
    endif()
    if(_extra)
      set(_name test_te_auto_uffd_off)
    endif()
    if(lo)
      set(_name ${_name}_large_only)
    endif()
    add_test(NAME ${_name}
      COMMAND ${CMAKE_COMMAND} -E env --unset=SMASH_TRACK_EXTERNAL ${_te_pre} "SMASH_LARGE_ONLY=${lo}"
        "SMASH_DEFER_PHASES_MS=0" "SMASH_COLD_TIMEOUT_SEC=1" ${_extra}
        $<TARGET_FILE:te_auto_test> ${_expect})
    set_tests_properties(${_name} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
  endforeach()
endforeach()

# io_uring fixed buffers (pinned pages) under SMASH_TRACK_EXTERNAL=1: a
# pinned page must never be reclaimed out from under the kernel (uffd MOVE
# refuses it), and liburing's io_uring_register_buffers of a compressed
# buffer must succeed (libsmash untracks it first). Linux, both modes.
if(NOT APPLE)
  add_executable(iouring_pin_test iouring_pin_test.cpp)
  set_target_properties(iouring_pin_test PROPERTIES CXX_STANDARD 20)
  find_library(SMASH_LIBURING uring)
  find_path(SMASH_LIBURING_INC liburing.h)
  set(_iouring_cases pin)
  if(SMASH_LIBURING AND SMASH_LIBURING_INC)
    target_compile_definitions(iouring_pin_test PRIVATE SMASH_TEST_LIBURING=1)
    target_include_directories(iouring_pin_test PRIVATE ${SMASH_LIBURING_INC})
    target_link_libraries(iouring_pin_test PRIVATE ${SMASH_LIBURING})
    list(APPEND _iouring_cases liburing)
  endif()
  foreach(lo 0 1)
    foreach(c ${_iouring_cases})
      set(_name test_iouring_${c})
      if(lo)
        set(_name ${_name}_large_only)
      endif()
      add_test(NAME ${_name}
        COMMAND ${CMAKE_COMMAND} -E env "LD_PRELOAD=$<TARGET_FILE:smash>" "SMASH_LARGE_ONLY=${lo}"
          "SMASH_TRACK_EXTERNAL=1" "SMASH_DEFER_PHASES_MS=0" "SMASH_COLD_TIMEOUT_SEC=1"
          $<TARGET_FILE:iouring_pin_test> ${c})
      set_tests_properties(${_name} PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    endforeach()
  endforeach()
endif()

# Linux uffd mode: pages UFFDIO_MOVE refuses (here: shared copy-on-write with
# a fork child) must still be reclaimed, via the classic path. Both modes.
if(NOT APPLE)
  add_executable(move_refused_test move_refused_test.cpp)
  set_target_properties(move_refused_test PROPERTIES CXX_STANDARD 20)
  target_include_directories(move_refused_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
  target_link_libraries(move_refused_test PRIVATE ${CMAKE_DL_LIBS})
  foreach(lo 0 1)
    set(_name test_move_refused)
    if(lo)
      set(_name test_move_refused_large_only)
    endif()
    add_test(NAME ${_name}
      COMMAND ${CMAKE_COMMAND} -E env "LD_PRELOAD=$<TARGET_FILE:smash>" "SMASH_LARGE_ONLY=${lo}"
        "SMASH_UFFD=1" "SMASH_DEFER_PHASES_MS=0" "SMASH_COLD_TIMEOUT_SEC=1"
        $<TARGET_FILE:move_refused_test>)
    set_tests_properties(${_name} PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
  endforeach()
endif()

# smash_hint_cold / smash_hint_hot / smash_get_stats, both modes. A 30 s cold
# timeout means only the hint can make pages compress within the test.
add_executable(public_api_test public_api_test.cpp)
set_target_properties(public_api_test PROPERTIES CXX_STANDARD 20)
target_include_directories(public_api_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(public_api_test PRIVATE ${CMAKE_DL_LIBS})
foreach(lo 0 1)
  set(_name test_public_api)
  if(lo)
    set(_name test_public_api_large_only)
  endif()
  if(APPLE)
    set(_pa_pre "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>" "MallocNanoZone=0")
  else()
    set(_pa_pre "LD_PRELOAD=$<TARGET_FILE:smash>")
  endif()
  add_test(NAME ${_name}
    COMMAND ${CMAKE_COMMAND} -E env ${_pa_pre} "SMASH_LARGE_ONLY=${lo}"
      "SMASH_DEFER_PHASES_MS=0" "SMASH_COLD_TIMEOUT_SEC=30" $<TARGET_FILE:public_api_test>)
  set_tests_properties(${_name} PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
endforeach()

# SMASH_CALIBRATION_FILE save + load under interposition (no stdio during
# heap construction). Reuses the arena-routing program as a payload.
if(APPLE)
  set(_cal_preload DYLD_INSERT_LIBRARIES)
else()
  set(_cal_preload LD_PRELOAD)
endif()
add_test(
  NAME test_calibration_file
  COMMAND ${CMAKE_COMMAND}
    -DPRELOAD_VAR=${_cal_preload}
    -DLIB=$<TARGET_FILE:smash>
    -DPROG=$<TARGET_FILE:arena_routing_test>
    -DCAL=${CMAKE_CURRENT_BINARY_DIR}/calibration_test.json
    -P ${CMAKE_CURRENT_SOURCE_DIR}/calibration_file_test.cmake
)
set_tests_properties(test_calibration_file PROPERTIES TIMEOUT 60)
# SMASH_DEBUG periodic stats from the coordinator thread (small stack).
add_executable(debug_stats_payload debug_stats_payload.cpp)
if(APPLE)
  set(_dbg_preload DYLD_INSERT_LIBRARIES)
else()
  set(_dbg_preload LD_PRELOAD)
endif()
add_test(
  NAME test_debug_stats
  COMMAND ${CMAKE_COMMAND}
    -DPRELOAD_VAR=${_dbg_preload}
    -DLIB=$<TARGET_FILE:smash>
    -DPROG=$<TARGET_FILE:debug_stats_payload>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/debug_stats_test.cmake
)
set_tests_properties(test_debug_stats PROPERTIES TIMEOUT 90)
if(APPLE)
  # Tick cadence at background QoS (timer coalescing); macOS only.
  add_test(NAME test_tick_cadence_background
    COMMAND ${CMAKE_COMMAND} -DLIB=$<TARGET_FILE:smash> -DPROG=$<TARGET_FILE:debug_stats_payload>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tick_cadence_test.cmake)
  set_tests_properties(test_tick_cadence_background PROPERTIES TIMEOUT 150)
endif()

# vm.max_map_count regression guard (#94): with the process held 12K VMAs
# below the cap (the test uses them up itself; no root), a teardown-like
# stride walk over 128 MiB of compressed large blocks must neither hang nor
# return wrong data. Linux only (no VMA cap elsewhere). The test exits 77
# (skipped) if the cap is too large to approach quickly. It tests the
# mprotect reclaim path (compressed = PROT_NONE), so uffd mode, the default
# on Linux 6.8+, is turned off.
if(NOT APPLE)
  add_executable(vma_cap_test vma_cap_test.cpp)
  set_target_properties(vma_cap_test PROPERTIES CXX_STANDARD 20)
  add_test(
    NAME test_vma_cap
    COMMAND ${CMAKE_COMMAND} -E env
      "LD_PRELOAD=$<TARGET_FILE:smash>"
      "SMASH_UFFD=0"
      "SMASH_LARGE_ONLY=1"
      "SMASH_DEFER_PHASES_MS=0"
      "SMASH_COLD_TIMEOUT_SEC=1"
      $<TARGET_FILE:vma_cap_test>
  )
  set_tests_properties(test_vma_cap PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77)
endif()

# Per-thread state in a TSD slot (macOS SMASH_TSD_SLOT): ThreadStates are
# reclaimed and reused across thousands of short-lived threads, GCD workers and
# allocating app destructors work, fork() keeps the free list usable. Skips
# itself when smash_tsd_stats is not exported (TSD slot not compiled in).
if(APPLE)
  add_executable(tsd_thread_test tsd_thread_test.cpp)
  set_target_properties(tsd_thread_test PROPERTIES CXX_STANDARD 20)
  add_test(
    NAME test_tsd_thread
    COMMAND ${CMAKE_COMMAND} -E env
      "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>"
      "MallocNanoZone=0"
      $<TARGET_FILE:tsd_thread_test>
  )
  set_tests_properties(test_tsd_thread PROPERTIES TIMEOUT 120)
endif()

# Large-block ownership regression: blocks smash mmaps directly (16 KiB < size
# < kLargeAllocVmThreshold, and every large block in large-only mode) sit
# outside the contiguous arena; xxowns() must still claim them or alloc8
# forwards realloc/malloc_size/free to the system allocator (malloc_size 0,
# realloc reads past the block -> SIGSEGV; crashed every Rust program).
# Interposed, both modes, no compressor needed.
add_executable(large_realloc_test large_realloc_test.cpp)
set_target_properties(large_realloc_test PROPERTIES CXX_STANDARD 20)
foreach(lo 0 1)
  if(lo)
    set(_name test_large_realloc_large_only)
  else()
    set(_name test_large_realloc)
  endif()
  if(APPLE)
    add_test(
      NAME ${_name}
      COMMAND ${CMAKE_COMMAND} -E env
        "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>"
        "MallocNanoZone=0"
        "SMASH_LARGE_ONLY=${lo}"
        $<TARGET_FILE:large_realloc_test>
    )
  else()
    add_test(
      NAME ${_name}
      COMMAND ${CMAKE_COMMAND} -E env
        "LD_PRELOAD=$<TARGET_FILE:smash>"
        "SMASH_LARGE_ONLY=${lo}"
        $<TARGET_FILE:large_realloc_test>
    )
  endif()
  set_tests_properties(${_name} PROPERTIES TIMEOUT 30)
endforeach()

# Syscall EFAULT-retry test: allocates a buffer, forces compression, then
# issues real syscalls (read/writev/readv) directly on the compressed
# buffer. Verifies that the EFAULT-driven retry path in syscall_compat.h
# actually catches kernel-level buffer protection faults and recovers.
# Without retryWithDecompress, the kernel returns -1/EFAULT and the test
# fails. Also exercises the iovec walk path in the rewarm callback.
add_executable(syscall_efault_test syscall_efault_test.cpp)
set_target_properties(syscall_efault_test PROPERTIES
  CXX_STANDARD 20
)
if(APPLE)
  add_test(
    NAME test_syscall_efault
    COMMAND ${CMAKE_COMMAND} -E env
      "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>"
      "MallocNanoZone=0"
      "SMASH_LARGE_ONLY=0"
      "SMASH_DEFER_PHASES_MS=0"
      "SMASH_COLD_TIMEOUT_SEC=1"
      "SMASH_STATS_SIGNAL=1"
      $<TARGET_FILE:syscall_efault_test>
  )
else()
  add_test(
    NAME test_syscall_efault
    COMMAND ${CMAKE_COMMAND} -E env
      "LD_PRELOAD=$<TARGET_FILE:smash>"
      "SMASH_LARGE_ONLY=0"
      "SMASH_DEFER_PHASES_MS=0"
      "SMASH_COLD_TIMEOUT_SEC=1"
      "SMASH_STATS_SIGNAL=1"
      # SMASH_SOFTDIRTY=0: this test relies on the deterministic
      # PROT_READ Phase-3 timing to drive the buffer to COMPRESSED
      # before the syscall. Soft-dirty's pagemap-read sees the
      # initial-fault writes for one extra tick, missing the test's
      # 3 s window.
      "SMASH_SOFTDIRTY=0"
      $<TARGET_FILE:syscall_efault_test>
  )
endif()
set_tests_properties(test_syscall_efault PROPERTIES TIMEOUT 180)

# SMASH_PROFILE resolution: one binary, run per environment; argv carries the
# expected cold floor (ticks), whether it counts as an explicit override (which
# bypasses the ROI gate), the ROI threshold, SMASH_PROMPT_TRIM_MB,
# SMASH_COMPRESSOR_IDLE and SMASH_EMPTY_RESIDENT_MB ("-" = not set). balanced
# must equal unset.
add_executable(test_profile test_profile.cpp ${printf_SOURCE_DIR}/printf.cpp)
target_include_directories(test_profile PRIVATE ${SMASH_TEST_INCLUDE_DIRS})
target_link_libraries(test_profile PRIVATE alloc8::headers lz4_static libzstd_static)
function(add_profile_test name env)
  add_test(NAME ${name} COMMAND test_profile ${ARGN})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "SMASH_CALIBRATE=never;${env}")
endfunction()
add_profile_test(test_profile_unset "SMASH_PROFILE_UNUSED=1" 10 0 1024 - - -)
add_profile_test(test_profile_balanced "SMASH_PROFILE=balanced" 10 0 1024 - - -)
add_profile_test(test_profile_latency "SMASH_PROFILE=latency" 30 0 1024 - 1 -)
add_profile_test(test_profile_memory "SMASH_PROFILE=memory" 2 0 1024 8 - 64)
add_profile_test(test_profile_explicit_wins
  "SMASH_PROFILE=memory;SMASH_COLD_TIMEOUT_SEC=5;SMASH_ROI_THRESHOLD=777;SMASH_PROMPT_TRIM_MB=0"
  5 1 777 0 - 64)
add_profile_test(test_profile_cold_ticks_wins "SMASH_PROFILE=latency;SMASH_COLD_TICKS=3" 3 1 1024 - 1 -)

# SMASH_REPORT + SMASH_METRICS_FILE end to end, both modes: report_test
# compresses, faults back and exits; check_report.sh asserts on the report
# (compressions, faults, latency quantiles) and on the Prometheus file
# (well-formed, histogram consistent, written atomically).
add_executable(report_test report_test.cpp)
set_target_properties(report_test PROPERTIES CXX_STANDARD 20)
foreach(lo 0 1)
  set(_name test_report)
  if(lo)
    set(_name test_report_large_only)
  endif()
  add_test(NAME ${_name}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_report.sh
      $<TARGET_FILE:smash> $<TARGET_FILE:report_test> ${lo}
      ${CMAKE_CURRENT_BINARY_DIR}/${_name}.d)
  set_tests_properties(${_name} PROPERTIES TIMEOUT 90)
endforeach()
# Same, with the env values overwritten in place after smash initialised
# (what setproctitle does in redis): the report and metrics file must still
# land at the configured paths.
add_test(NAME test_report_clobbered_env
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_report.sh
    $<TARGET_FILE:smash> $<TARGET_FILE:report_test> 0
    ${CMAKE_CURRENT_BINARY_DIR}/test_report_clobbered_env.d)
set_tests_properties(test_report_clobbered_env PROPERTIES TIMEOUT 90
  ENVIRONMENT REPORT_TEST_CLOBBER_ENV=1)
if(APPLE)
  set(_ro_preload DYLD_INSERT_LIBRARIES)
else()
  set(_ro_preload LD_PRELOAD)
endif()
add_test(NAME test_report_observe
  COMMAND ${CMAKE_COMMAND} -DPRELOAD_VAR=${_ro_preload} -DLIB=$<TARGET_FILE:smash>
    -DPROG=$<TARGET_FILE:report_test> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/test_report_observe.d
    -P ${CMAKE_CURRENT_SOURCE_DIR}/report_observe_test.cmake)
set_tests_properties(test_report_observe PROPERTIES TIMEOUT 90)

# bin/smash launcher, both modes: library discovery relative to the script,
# preload, --report/--large-only/--profile, `smash stats` refusal and
# --stats-signal delivery, and the macOS SIP warning.
add_executable(launcher_test launcher_test.cpp)
set_target_properties(launcher_test PROPERTIES CXX_STANDARD 20)
target_link_libraries(launcher_test PRIVATE ${CMAKE_DL_LIBS})
foreach(lo 0 1)
  set(_name test_launcher)
  if(lo)
    set(_name test_launcher_large_only)
  endif()
  add_test(NAME ${_name}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/check_launcher.sh
      ${CMAKE_BINARY_DIR}/bin/smash $<TARGET_FILE:launcher_test> ${lo}
      ${CMAKE_CURRENT_BINARY_DIR}/${_name}.d)
  set_tests_properties(${_name} PROPERTIES TIMEOUT 60)
endforeach()
# smash-estimate (Linux): launch mode against a target with known idle/hot
# regions; runs unprivileged (soft-dirty or the referenced fallback; idle-page
# tracking when run as root). The _referenced variant forces the per-mapping
# fallback, the only method on kernels without soft-dirty or page_idle access.
# The _idle variants force the page_idle bitmap method (root via `sudo -n`,
# CONFIG_IDLE_PAGE_TRACKING; skipped with 77 otherwise); _idle_thp backs the
# target with transparent huge pages, whose tail PFNs the bitmap never tracks.
if(TARGET smash-estimate)
  add_executable(estimate_target estimate_target.cpp)
  add_executable(estimate_test estimate_test.cpp)
  add_test(NAME test_smash_estimate
           COMMAND estimate_test $<TARGET_FILE:smash-estimate> $<TARGET_FILE:estimate_target>)
  add_test(NAME test_smash_estimate_referenced
           COMMAND estimate_test $<TARGET_FILE:smash-estimate> $<TARGET_FILE:estimate_target> referenced)
  add_test(NAME test_smash_estimate_idle
           COMMAND estimate_test $<TARGET_FILE:smash-estimate> $<TARGET_FILE:estimate_target> idle)
  add_test(NAME test_smash_estimate_idle_thp
           COMMAND estimate_test $<TARGET_FILE:smash-estimate> $<TARGET_FILE:estimate_target> idle thp)
  set_tests_properties(test_smash_estimate test_smash_estimate_referenced
                       test_smash_estimate_idle test_smash_estimate_idle_thp PROPERTIES TIMEOUT 60)
  set_tests_properties(test_smash_estimate_idle test_smash_estimate_idle_thp PROPERTIES SKIP_RETURN_CODE 77)
endif()

# ── Deployment safety: kill switch, watchdog, SLO controller (agents/safety.md) ──
# Unit: compress/safety.h pure logic (signal parsing, latency histogram,
# watchdog / controller state machine). Deterministic, no compressor.
smash_add_test(test_safety test_safety.cpp)

# smash_add_preload_test(<name> <target> ARGS ... ENV ... [TIMEOUT n])
# Runs <target> under DYLD_INSERT_LIBRARIES (macOS) / LD_PRELOAD (Linux).
function(smash_add_preload_test name target)
  cmake_parse_arguments(P "" "TIMEOUT" "ARGS;ENV" ${ARGN})
  if(APPLE)
    set(_pre "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>" "MallocNanoZone=0")
  else()
    set(_pre "LD_PRELOAD=$<TARGET_FILE:smash>")
  endif()
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND} -E env ${_pre} ${P_ENV} $<TARGET_FILE:${target}> ${P_ARGS})
  if(NOT P_TIMEOUT)
    set(P_TIMEOUT 120)
  endif()
  set_tests_properties(${name} PROPERTIES TIMEOUT ${P_TIMEOUT})
endfunction()

add_executable(kill_switch_test kill_switch_test.cpp)
set_target_properties(kill_switch_test PROPERTIES CXX_STANDARD 20)
target_link_libraries(kill_switch_test PRIVATE ${CMAKE_DL_LIBS})
add_executable(storm_test storm_test.cpp)
set_target_properties(storm_test PROPERTIES CXX_STANDARD 20)
target_link_libraries(storm_test PRIVATE ${CMAKE_DL_LIBS})

foreach(lo 0 1)
  if(lo)
    set(_sfx _large_only)
  else()
    set(_sfx "")
  endif()
  # One kill-switch test per trigger and mode. SIGUSR1 disables, SIGUSR2
  # enables (the stats signal is not enabled here, so SIGUSR2 is free).
  foreach(mech api signal file)
    smash_add_preload_test(test_kill_switch_${mech}${_sfx} kill_switch_test
      ARGS ${mech}
      ENV "SMASH_LARGE_ONLY=${lo}" "SMASH_DEFER_PHASES_MS=0" "SMASH_COLD_TIMEOUT_SEC=1"
          "SMASH_DISABLE_SIGNAL=SIGUSR1" "SMASH_ENABLE_SIGNAL=SIGUSR2"
          "SMASH_CONTROL_FILE=${CMAKE_CURRENT_BINARY_DIR}/kill_switch_${mech}${_sfx}.ctl")
  endforeach()
  # Rate limit 50/s: the storm refaults every page each cold cycle, which is
  # ~170-190/s on macOS CI (16 KiB pages, a quarter as many as 4 KiB Linux
  # pages for the same bytes); 200/s sat just above that and never tripped.
  smash_add_preload_test(test_watchdog_rate${_sfx} storm_test
    ARGS watchdog-rate
    ENV "SMASH_LARGE_ONLY=${lo}" "SMASH_DEFER_PHASES_MS=0" "SMASH_COLD_TIMEOUT_SEC=1"
        "SMASH_RECOMPRESS_BACKOFF=0" "SMASH_WATCHDOG_FAULT_RATE=50" "SMASH_WATCHDOG_WINDOW=2")
  smash_add_preload_test(test_watchdog_p99${_sfx} storm_test
    ARGS watchdog-p99
    ENV "SMASH_LARGE_ONLY=${lo}" "SMASH_DEFER_PHASES_MS=0" "SMASH_COLD_TIMEOUT_SEC=1"
        "SMASH_RECOMPRESS_BACKOFF=0" "SMASH_WATCHDOG_P99_US=1" "SMASH_WATCHDOG_WINDOW=2")
  # Budget 50/s, under the storm on every platform (macOS CI: 182-196/s with
  # 16 KiB pages; 200/s sat just above it and the floor never rose).
  smash_add_preload_test(test_slo_controller${_sfx} storm_test
    ARGS controller
    ENV "SMASH_LARGE_ONLY=${lo}" "SMASH_DEFER_PHASES_MS=0" "SMASH_COLD_TIMEOUT_SEC=1"
        "SMASH_RECOMPRESS_BACKOFF=0" "SMASH_MAX_FAULT_RATE=50")
endforeach()

# Concurrency soak: threads alloc/free/realloc across size classes with
# randomized idle gaps (so pages compress) and read a reclaimed cold set
# (concurrent decompress faults), with the kill switch flipped underneath.
# Short in ctest; the CI "stress" job runs it for minutes (SMASH_SOAK_SEC).
add_executable(soak_test soak_test.cpp)
set_target_properties(soak_test PROPERTIES CXX_STANDARD 20)
target_link_libraries(soak_test PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
foreach(lo 0 1)
  if(lo)
    set(_sfx _large_only)
  else()
    set(_sfx "")
  endif()
  smash_add_preload_test(test_soak${_sfx} soak_test
    ARGS 20 8
    ENV "SMASH_LARGE_ONLY=${lo}" "SMASH_DEFER_PHASES_MS=0" "SMASH_COLD_TIMEOUT_SEC=1"
        "SMASH_SOAK_TOGGLE=1"
    TIMEOUT 180)
  set_tests_properties(test_soak${_sfx} PROPERTIES LABELS "soak")
endforeach()

# fork() safety under threads (issue #61): Linux only (macOS restricts a
# multithreaded fork child to async-signal-safe calls; smash's fault handler
# there is a Mach exception thread the child does not inherit).
if(NOT APPLE)
  add_executable(fork_stress_test fork_stress_test.cpp)
  set_target_properties(fork_stress_test PROPERTIES CXX_STANDARD 20)
  target_link_libraries(fork_stress_test PRIVATE Threads::Threads)
  foreach(lo 0 1)
    if(lo)
      set(_sfx _large_only)
    else()
      set(_sfx "")
    endif()
    smash_add_preload_test(test_fork_stress${_sfx} fork_stress_test
      ARGS 120 6
      ENV "SMASH_LARGE_ONLY=${lo}" "SMASH_DEFER_PHASES_MS=0" "SMASH_COLD_TIMEOUT_SEC=1"
      TIMEOUT 300)
  endforeach()
endif()

# Eager fill compression bounds the peak of a paced write-once fill (1 MiB
# chunks, so both modes manage them). Default cold timeout on purpose: without
# SMASH_EAGER_FILL nothing compresses before the fill ends and the test fails.
add_executable(peak_fill_test peak_fill_test.cpp)
set_target_properties(peak_fill_test PROPERTIES CXX_STANDARD 20)
function(add_peak_fill_test test_name large_only)
  if(APPLE)
    set(_preload "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:smash>" "MallocNanoZone=0")
  else()
    set(_preload "LD_PRELOAD=$<TARGET_FILE:smash>")
  endif()
  add_test(
    NAME ${test_name}
    COMMAND ${CMAKE_COMMAND} -E env ${_preload}
      "SMASH_LARGE_ONLY=${large_only}"
      "SMASH_EAGER_FILL=1"
      $<TARGET_FILE:peak_fill_test>
  )
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 90)
endfunction()
add_peak_fill_test(test_peak_fill 0)             # full mode
add_peak_fill_test(test_peak_fill_large_only 1)  # large-only mode

# SMASH_TEST_FORCE_TRACK_EXTERNAL=ON: run every test with
# SMASH_TRACK_EXTERNAL=1 in its environment (the "TE-forced" CI matrix).
# Preloaded tests pass it on to the program under test; a test that sets
# SMASH_TRACK_EXTERNAL itself (cmake -E env) keeps its own value.
option(SMASH_TEST_FORCE_TRACK_EXTERNAL "Run every test with SMASH_TRACK_EXTERNAL=1" OFF)
if(SMASH_TEST_FORCE_TRACK_EXTERNAL)
  get_property(_smash_all_tests DIRECTORY PROPERTY TESTS)
  foreach(_t ${_smash_all_tests})
    set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT "SMASH_TRACK_EXTERNAL=1")
  endforeach()
endif()
