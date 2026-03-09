# smash/bench/CMakeLists.txt - Standalone benchmark executables

set(SMASH_BENCH_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# Helper to add a benchmark executable (linked against smash internals)
function(smash_add_bench name source)
  add_executable(${name} ${source})
  target_include_directories(${name} PRIVATE ${SMASH_BENCH_INCLUDE_DIRS})
  target_link_libraries(${name} PRIVATE alloc8::headers lz4_static libzstd_static)
  if(APPLE)
    target_compile_options(${name} PRIVATE -ftls-model=initial-exec)
  endif()
endfunction()

# Helper: filter out -fno-builtin-* flags from directory compile options.
# The root CMakeLists.txt sets these globally for smash internals, but standalone
# benchmarks use system malloc/free and must not have them.
function(_get_filtered_dir_compile_options out_var)
  get_property(dir_opts DIRECTORY PROPERTY COMPILE_OPTIONS)
  set(filtered "")
  foreach(opt IN LISTS dir_opts)
    if(NOT opt MATCHES "^-fno-builtin-")
      list(APPEND filtered "${opt}")
    endif()
  endforeach()
  set(${out_var} "${filtered}" PARENT_SCOPE)
endfunction()

# Helper to add a standalone benchmark executable (uses system malloc/free).
# These are designed to run directly (baseline) or via DYLD_INSERT_LIBRARIES/LD_PRELOAD
# with libsmash for A/B comparison.
function(smash_add_standalone_bench name source)
  add_executable(${name} ${source})
  # Override compile options to exclude -fno-builtin-malloc etc.
  _get_filtered_dir_compile_options(clean_opts)
  set_target_properties(${name} PROPERTIES COMPILE_OPTIONS "${clean_opts}")
endfunction()

# ── cJSON library (for bench_json) ──────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
  cjson
  GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG v1.7.18
  GIT_SHALLOW TRUE
)
FetchContent_GetProperties(cjson)
if(NOT cjson_POPULATED)
  FetchContent_Populate(cjson)
  add_library(cjson_static STATIC ${cjson_SOURCE_DIR}/cJSON.c)
  target_include_directories(cjson_static PUBLIC ${cjson_SOURCE_DIR})
  set_target_properties(cjson_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
  # cJSON uses builtin malloc — strip the global -fno-builtin-* flags
  _get_filtered_dir_compile_options(cjson_opts)
  set_target_properties(cjson_static PROPERTIES COMPILE_OPTIONS "${cjson_opts}")
endif()

# ── Existing internal benchmarks ────────────────────────────────────────────

# Malloc/free throughput across size classes
add_executable(bench_throughput
  bench_throughput.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/smash_heap.cpp
)
target_include_directories(bench_throughput PRIVATE ${SMASH_BENCH_INCLUDE_DIRS})
target_link_libraries(bench_throughput PRIVATE alloc8::headers lz4_static libzstd_static)
if(APPLE)
  target_compile_options(bench_throughput PRIVATE -ftls-model=initial-exec)
endif()

# Compression ratio comparison: LZ4 vs zstd vs zstd+dict
smash_add_bench(bench_compression bench_compression.cpp)

# Dictionary compression experiments
smash_add_bench(bench_dict_experiment bench_dict_experiment.cpp)

# Algorithm comparison: WKdm vs LZ4 vs zstd, multi-page compression
smash_add_bench(bench_algo_compare bench_algo_compare.cpp)

# RSS reduction over time
add_executable(bench_rss
  bench_rss.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/smash_heap.cpp
)
target_include_directories(bench_rss PRIVATE ${SMASH_BENCH_INCLUDE_DIRS})
target_link_libraries(bench_rss PRIVATE alloc8::headers lz4_static libzstd_static)
if(APPLE)
  target_compile_options(bench_rss PRIVATE -ftls-model=initial-exec)
endif()

# Alloc/free latency percentiles
add_executable(bench_latency
  bench_latency.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/smash_heap.cpp
)
target_include_directories(bench_latency PRIVATE ${SMASH_BENCH_INCLUDE_DIRS})
target_link_libraries(bench_latency PRIVATE alloc8::headers lz4_static libzstd_static)
if(APPLE)
  target_compile_options(bench_latency PRIVATE -ftls-model=initial-exec)
endif()

# ── Standalone real-world benchmarks ────────────────────────────────────────

# JSON processing workload (cJSON DOM tree with hot/cold access)
smash_add_standalone_bench(bench_json bench_json.cpp)
target_link_libraries(bench_json PRIVATE cjson_static)

# Key-value store simulation (unordered_map with Zipfian access)
smash_add_standalone_bench(bench_kv_store bench_kv_store.cpp)

# ── SQLite amalgamation (for bench_sqlite) ─────────────────────────────────
FetchContent_Declare(
  sqlite3
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3510200.zip
)
FetchContent_GetProperties(sqlite3)
if(NOT sqlite3_POPULATED)
  FetchContent_Populate(sqlite3)
  add_library(sqlite3_static STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
  target_include_directories(sqlite3_static PUBLIC ${sqlite3_SOURCE_DIR})
  target_compile_definitions(sqlite3_static PRIVATE
    SQLITE_THREADSAFE=0
    SQLITE_OMIT_LOAD_EXTENSION
  )
  set_target_properties(sqlite3_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
  _get_filtered_dir_compile_options(sqlite3_opts)
  set_target_properties(sqlite3_static PROPERTIES COMPILE_OPTIONS "${sqlite3_opts}")
endif()

# SQLite in-memory database workload
smash_add_standalone_bench(bench_sqlite bench_sqlite.cpp)
target_link_libraries(bench_sqlite PRIVATE sqlite3_static)

# ── Allocator substrate comparison benchmarks ───────────────────────────────

# Helper to add allocator comparison benchmark variant
function(smash_add_alloc_bench name alloc_define)
  smash_add_standalone_bench(${name} bench_allocator_compare.cpp)
  target_compile_definitions(${name} PRIVATE ALLOC_NAME="${alloc_define}")
  target_link_libraries(${name} PRIVATE lz4_static libzstd_static)
endfunction()

# Helper: allocator bench with ZERO_ON_FREE enabled (benchmark zeros before free)
function(smash_add_alloc_bench_zero name alloc_define)
  smash_add_alloc_bench(${name} "${alloc_define}")
  target_compile_definitions(${name} PRIVATE ZERO_ON_FREE)
endfunction()

# System malloc (also used as Smash target via DYLD_INSERT_LIBRARIES)
smash_add_alloc_bench(bench_alloc_system "system")
smash_add_alloc_bench_zero(bench_alloc_system_zero "system+zero")

# mimalloc
FetchContent_Declare(mimalloc
  GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
  GIT_TAG v2.1.2
  GIT_SHALLOW TRUE
)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(mimalloc)

smash_add_alloc_bench(bench_alloc_mimalloc "mimalloc")
target_link_libraries(bench_alloc_mimalloc PRIVATE mimalloc-static)
smash_add_alloc_bench_zero(bench_alloc_mimalloc_zero "mimalloc+zero")
target_link_libraries(bench_alloc_mimalloc_zero PRIVATE mimalloc-static)

# jemalloc (system install, optional)
find_library(JEMALLOC_LIB jemalloc HINTS /opt/homebrew/lib)
find_path(JEMALLOC_INCLUDE jemalloc/jemalloc.h HINTS /opt/homebrew/include)
if(JEMALLOC_LIB AND JEMALLOC_INCLUDE)
  smash_add_alloc_bench(bench_alloc_jemalloc "jemalloc")
  target_link_libraries(bench_alloc_jemalloc PRIVATE ${JEMALLOC_LIB})
  target_include_directories(bench_alloc_jemalloc PRIVATE ${JEMALLOC_INCLUDE})
  smash_add_alloc_bench_zero(bench_alloc_jemalloc_zero "jemalloc+zero")
  target_link_libraries(bench_alloc_jemalloc_zero PRIVATE ${JEMALLOC_LIB})
  target_include_directories(bench_alloc_jemalloc_zero PRIVATE ${JEMALLOC_INCLUDE})
  message(STATUS "jemalloc found: ${JEMALLOC_LIB}")
else()
  message(STATUS "jemalloc not found (optional)")
endif()

# tcmalloc (system install, optional)
find_library(TCMALLOC_LIB tcmalloc_minimal)
if(TCMALLOC_LIB)
  smash_add_alloc_bench(bench_alloc_tcmalloc "tcmalloc")
  target_link_libraries(bench_alloc_tcmalloc PRIVATE ${TCMALLOC_LIB})
  smash_add_alloc_bench_zero(bench_alloc_tcmalloc_zero "tcmalloc+zero")
  target_link_libraries(bench_alloc_tcmalloc_zero PRIVATE ${TCMALLOC_LIB})
  message(STATUS "tcmalloc found: ${TCMALLOC_LIB}")
else()
  message(STATUS "tcmalloc not found (optional)")
endif()

# Hoard (system install, optional)
find_library(HOARD_LIB hoard libhoard)
if(HOARD_LIB)
  smash_add_alloc_bench(bench_alloc_hoard "hoard")
  target_link_libraries(bench_alloc_hoard PRIVATE ${HOARD_LIB})
  smash_add_alloc_bench_zero(bench_alloc_hoard_zero "hoard+zero")
  target_link_libraries(bench_alloc_hoard_zero PRIVATE ${HOARD_LIB})
  message(STATUS "Hoard found: ${HOARD_LIB}")
else()
  message(STATUS "Hoard not found (optional)")
endif()

# Mesh (external metadata, virtual remapping, no zeroing) - optional
# Prefer static lib to avoid Bazel rpath issues with the dylib
find_library(MESH_LIB_STATIC NAMES libmesh.a HINTS /usr/local/lib)
find_library(MESH_LIB_SHARED mesh HINTS /usr/local/lib)
find_path(MESH_INCLUDE plasma/mesh.h HINTS /usr/local/include)
if(MESH_LIB_STATIC)
  set(MESH_LIB ${MESH_LIB_STATIC})
elseif(MESH_LIB_SHARED)
  set(MESH_LIB ${MESH_LIB_SHARED})
endif()
if(MESH_LIB)
  smash_add_alloc_bench(bench_alloc_mesh "mesh")
  target_link_libraries(bench_alloc_mesh PRIVATE ${MESH_LIB})
  if(MESH_INCLUDE)
    target_include_directories(bench_alloc_mesh PRIVATE ${MESH_INCLUDE})
  endif()
  # Mesh links against Foundation on macOS
  if(APPLE)
    target_link_libraries(bench_alloc_mesh PRIVATE "-framework Foundation")
  endif()
  smash_add_alloc_bench_zero(bench_alloc_mesh_zero "mesh+zero")
  target_link_libraries(bench_alloc_mesh_zero PRIVATE ${MESH_LIB})
  if(MESH_INCLUDE)
    target_include_directories(bench_alloc_mesh_zero PRIVATE ${MESH_INCLUDE})
  endif()
  if(APPLE)
    target_link_libraries(bench_alloc_mesh_zero PRIVATE "-framework Foundation")
  endif()
  message(STATUS "Mesh found: ${MESH_LIB}")
else()
  message(STATUS "Mesh not found (optional)")
endif()

# DieHard (external metadata, no zeroing) - optional
find_library(DIEHARD_LIB diehard libdiehard)
if(DIEHARD_LIB)
  smash_add_alloc_bench(bench_alloc_diehard "diehard")
  target_link_libraries(bench_alloc_diehard PRIVATE ${DIEHARD_LIB})
  smash_add_alloc_bench_zero(bench_alloc_diehard_zero "diehard+zero")
  target_link_libraries(bench_alloc_diehard_zero PRIVATE ${DIEHARD_LIB})
  message(STATUS "DieHard found: ${DIEHARD_LIB}")
else()
  message(STATUS "DieHard not found (optional)")
endif()

# DieHarder (external metadata + security zeroing) - optional
find_library(DIEHARDER_LIB dieharder libdieharder)
if(DIEHARDER_LIB)
  smash_add_alloc_bench(bench_alloc_dieharder "dieharder")
  target_link_libraries(bench_alloc_dieharder PRIVATE ${DIEHARDER_LIB})
  message(STATUS "DieHarder found: ${DIEHARDER_LIB}")
else()
  message(STATUS "DieHarder not found (optional)")
endif()

# Python runner (configure paths)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_allocator_compare.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_allocator_compare.py
  @ONLY
)

# ── Shell script benchmarks (configure paths) ──────────────────────────────
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_runner.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_runner.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_redis.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_redis.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_python.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_python.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_sort.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_sort.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_memcached.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_memcached.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_memcached.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_memcached.py
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_duckdb.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_duckdb.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_duckdb.py.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_duckdb.py
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_memcached_alloc.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_memcached_alloc.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_duckdb_alloc.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_duckdb_alloc.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_redis_alloc.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_redis_alloc.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_polars.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_polars.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_sklearn.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_sklearn.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_networkx.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_networkx.sh
  @ONLY
)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_rocksdb.sh.in
  ${CMAKE_CURRENT_BINARY_DIR}/bench_rocksdb.sh
  @ONLY
)

# ── RocksDB benchmark (optional) ─────────────────────────────────────────────
find_library(ROCKSDB_LIB rocksdb HINTS /opt/homebrew/lib)
find_path(ROCKSDB_INCLUDE rocksdb/db.h HINTS /opt/homebrew/include)
if(ROCKSDB_LIB AND ROCKSDB_INCLUDE)
  smash_add_standalone_bench(bench_rocksdb bench_rocksdb.cpp)
  target_link_libraries(bench_rocksdb PRIVATE ${ROCKSDB_LIB})
  target_include_directories(bench_rocksdb PRIVATE ${ROCKSDB_INCLUDE})
  message(STATUS "RocksDB found: ${ROCKSDB_LIB}")
else()
  message(STATUS "RocksDB not found (optional)")
endif()

# ── Heap page compression sampling ────────────────────────────────────────────

# SQLite variant
smash_add_standalone_bench(bench_heap_compress_sqlite bench_heap_compress.cpp)
target_compile_definitions(bench_heap_compress_sqlite PRIVATE HAS_SQLITE)
target_link_libraries(bench_heap_compress_sqlite PRIVATE sqlite3_static lz4_static libzstd_static)

# Generic variant (memcached/redis/duckdb heap fill patterns)
smash_add_standalone_bench(bench_heap_compress bench_heap_compress.cpp)
target_link_libraries(bench_heap_compress PRIVATE lz4_static libzstd_static)

# RocksDB variant (optional)
if(ROCKSDB_LIB AND ROCKSDB_INCLUDE)
  smash_add_standalone_bench(bench_heap_compress_rocksdb bench_heap_compress.cpp)
  target_compile_definitions(bench_heap_compress_rocksdb PRIVATE HAS_ROCKSDB)
  target_link_libraries(bench_heap_compress_rocksdb PRIVATE
    ${ROCKSDB_LIB} lz4_static libzstd_static)
  target_include_directories(bench_heap_compress_rocksdb PRIVATE ${ROCKSDB_INCLUDE})
endif()

# Remote process heap page sampler (mach_vm_read)
smash_add_standalone_bench(sample_heap_pages sample_heap_pages.cpp)
target_link_libraries(sample_heap_pages PRIVATE lz4_static libzstd_static)

# DYLD-interposable heap sampling library (SIGUSR1 to trigger)
add_library(sample_heap SHARED libsample_heap.cpp)
target_link_libraries(sample_heap PRIVATE lz4_static libzstd_static)
set_target_properties(sample_heap PROPERTIES OUTPUT_NAME sample_heap)
