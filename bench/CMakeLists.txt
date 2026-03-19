# smash/bench/CMakeLists.txt - Standalone benchmark executables

find_package(Threads REQUIRED)

# ── Option to build external benchmark dependencies ──────────────────────────
# Set -DSMASH_BUILD_BENCH_DEPS=ON to build Redis, memcached, DuckDB, RocksDB
option(SMASH_BUILD_BENCH_DEPS "Build external benchmark dependencies (Redis, memcached, etc.)" OFF)

set(SMASH_BENCH_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# Helper to add a benchmark executable (linked against smash internals)
function(smash_add_bench name source)
  add_executable(${name} ${source})
  target_include_directories(${name} PRIVATE ${SMASH_BENCH_INCLUDE_DIRS})
  target_link_libraries(${name} PRIVATE alloc8::headers lz4_static libzstd_static ${CMAKE_DL_LIBS})
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
  target_link_libraries(${name} PRIVATE Threads::Threads)
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
target_link_libraries(bench_throughput PRIVATE alloc8::headers lz4_static libzstd_static ${CMAKE_DL_LIBS})
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
target_link_libraries(bench_rss PRIVATE alloc8::headers lz4_static libzstd_static ${CMAKE_DL_LIBS})
if(APPLE)
  target_compile_options(bench_rss PRIVATE -ftls-model=initial-exec)
endif()

# Alloc/free latency percentiles
add_executable(bench_latency
  bench_latency.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/smash_heap.cpp
)
target_include_directories(bench_latency PRIVATE ${SMASH_BENCH_INCLUDE_DIRS})
target_link_libraries(bench_latency PRIVATE alloc8::headers lz4_static libzstd_static ${CMAKE_DL_LIBS})
if(APPLE)
  target_compile_options(bench_latency PRIVATE -ftls-model=initial-exec)
endif()

# ── Standalone real-world benchmarks ────────────────────────────────────────

# JSON processing workload (cJSON DOM tree with hot/cold access)
smash_add_standalone_bench(bench_json bench_json.cpp)
target_link_libraries(bench_json PRIVATE cjson_static)

# Key-value store simulation (unordered_map with Zipfian access)
smash_add_standalone_bench(bench_kv_store bench_kv_store.cpp)

# Configurable cold/hot mix microbenchmark (raw malloc/free)
smash_add_standalone_bench(bench_coldhotmix bench_coldhotmix.cpp)

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
  target_compile_definitions(${name} PRIVATE ALLOC_NAME="${alloc_define}" HAS_SQLITE)
  target_link_libraries(${name} PRIVATE lz4_static libzstd_static sqlite3_static)
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

# Remote process heap page sampler (macOS only — uses mach_vm_read)
if(APPLE)
  smash_add_standalone_bench(sample_heap_pages sample_heap_pages.cpp)
  target_link_libraries(sample_heap_pages PRIVATE lz4_static libzstd_static)
endif()

# Preloadable heap sampling library (SIGUSR2 to trigger)
# Works on macOS (DYLD_INSERT_LIBRARIES) and Linux (LD_PRELOAD)
add_library(sample_heap SHARED libsample_heap.cpp)
target_link_libraries(sample_heap PRIVATE lz4_static libzstd_static)
set_target_properties(sample_heap PROPERTIES OUTPUT_NAME sample_heap)

# ══════════════════════════════════════════════════════════════════════════════
# External Benchmark Dependencies (Redis, memcached, DuckDB, RocksDB)
# ══════════════════════════════════════════════════════════════════════════════
# Build with: cmake .. -DSMASH_BUILD_BENCH=ON -DSMASH_BUILD_BENCH_DEPS=ON
#
# These are built from source to ensure they use system malloc (libc) instead of
# their default allocators (jemalloc, etc.), which is required for Smash to
# effectively compress their memory.

if(SMASH_BUILD_BENCH_DEPS)
  include(ExternalProject)

  # Output directory for built binaries
  set(BENCH_DEPS_DIR ${CMAKE_CURRENT_BINARY_DIR}/deps)
  file(MAKE_DIRECTORY ${BENCH_DEPS_DIR}/bin)

  # ── Redis (built with libc malloc instead of jemalloc) ─────────────────────
  ExternalProject_Add(redis_build
    GIT_REPOSITORY https://github.com/redis/redis.git
    GIT_TAG 7.2.4
    GIT_SHALLOW TRUE
    PREFIX ${BENCH_DEPS_DIR}/redis
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE TRUE
    BUILD_COMMAND make MALLOC=libc -j4
    INSTALL_COMMAND
      ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/src/redis-server ${BENCH_DEPS_DIR}/bin/redis-server-libc
      COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/src/redis-cli ${BENCH_DEPS_DIR}/bin/redis-cli
      COMMAND ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/src/redis-benchmark ${BENCH_DEPS_DIR}/bin/redis-benchmark
    BUILD_BYPRODUCTS
      ${BENCH_DEPS_DIR}/bin/redis-server-libc
      ${BENCH_DEPS_DIR}/bin/redis-cli
      ${BENCH_DEPS_DIR}/bin/redis-benchmark
  )

  # ── Memcached (built with system malloc) ───────────────────────────────────
  # Memcached uses its own slab allocator on top of malloc, so system malloc works
  # Requires: autoconf, libevent-devel
  find_program(AUTORECONF autoreconf)
  find_path(LIBEVENT_INCLUDE event.h HINTS /usr/include /usr/local/include)
  if(AUTORECONF AND LIBEVENT_INCLUDE)
    ExternalProject_Add(memcached_build
      GIT_REPOSITORY https://github.com/memcached/memcached.git
      GIT_TAG 1.6.23
      GIT_SHALLOW TRUE
      PREFIX ${BENCH_DEPS_DIR}/memcached
      CONFIGURE_COMMAND
        cd <SOURCE_DIR> && ./autogen.sh &&
        <SOURCE_DIR>/configure --prefix=${BENCH_DEPS_DIR}
      BUILD_IN_SOURCE TRUE
      BUILD_COMMAND make -j4
      INSTALL_COMMAND
        ${CMAKE_COMMAND} -E copy <SOURCE_DIR>/memcached ${BENCH_DEPS_DIR}/bin/memcached
      BUILD_BYPRODUCTS ${BENCH_DEPS_DIR}/bin/memcached
    )
    message(STATUS "Will build memcached from source")
  elseif(NOT AUTORECONF)
    message(STATUS "autoreconf not found - skipping memcached build (install autoconf)")
  else()
    message(STATUS "libevent-devel not found - skipping memcached build (install libevent-devel)")
  endif()

  # ── DuckDB (CLI with system malloc) ────────────────────────────────────────
  ExternalProject_Add(duckdb_build
    GIT_REPOSITORY https://github.com/duckdb/duckdb.git
    GIT_TAG v1.0.0
    GIT_SHALLOW TRUE
    PREFIX ${BENCH_DEPS_DIR}/duckdb
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_SHELL=ON
      -DBUILD_UNITTESTS=OFF
      -DENABLE_SANITIZER=OFF
      -DENABLE_UBSAN=OFF
      -DCMAKE_INSTALL_PREFIX=${BENCH_DEPS_DIR}
    BUILD_COMMAND ${CMAKE_COMMAND} --build . --target shell -- -j4
    INSTALL_COMMAND
      ${CMAKE_COMMAND} -E copy <BINARY_DIR>/duckdb ${BENCH_DEPS_DIR}/bin/duckdb
    BUILD_BYPRODUCTS ${BENCH_DEPS_DIR}/bin/duckdb
  )

  # ── RocksDB (static library for benchmarks) ────────────────────────────────
  ExternalProject_Add(rocksdb_build
    GIT_REPOSITORY https://github.com/facebook/rocksdb.git
    GIT_TAG v9.0.0
    GIT_SHALLOW TRUE
    PREFIX ${BENCH_DEPS_DIR}/rocksdb
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DWITH_TESTS=OFF
      -DWITH_BENCHMARK_TOOLS=OFF
      -DWITH_TOOLS=OFF
      -DWITH_GFLAGS=OFF
      -DROCKSDB_BUILD_SHARED=OFF
      -DCMAKE_INSTALL_PREFIX=${BENCH_DEPS_DIR}
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    BUILD_COMMAND ${CMAKE_COMMAND} --build . -- -j4
    INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install
    BUILD_BYPRODUCTS
      ${BENCH_DEPS_DIR}/lib/librocksdb.a
      ${BENCH_DEPS_DIR}/include/rocksdb/db.h
  )

  # Export paths for benchmark scripts
  set(BENCH_REDIS_SERVER ${BENCH_DEPS_DIR}/bin/redis-server-libc CACHE PATH "Redis server with libc malloc")
  set(BENCH_REDIS_CLI ${BENCH_DEPS_DIR}/bin/redis-cli CACHE PATH "Redis CLI")
  set(BENCH_REDIS_BENCHMARK ${BENCH_DEPS_DIR}/bin/redis-benchmark CACHE PATH "Redis benchmark")
  set(BENCH_MEMCACHED ${BENCH_DEPS_DIR}/bin/memcached CACHE PATH "Memcached server")
  set(BENCH_DUCKDB ${BENCH_DEPS_DIR}/bin/duckdb CACHE PATH "DuckDB CLI")
  set(BENCH_ROCKSDB_LIB ${BENCH_DEPS_DIR}/lib/librocksdb.a CACHE PATH "RocksDB library")
  set(BENCH_ROCKSDB_INCLUDE ${BENCH_DEPS_DIR}/include CACHE PATH "RocksDB headers")

  # Create a convenience target
  add_custom_target(bench_deps)
  add_dependencies(bench_deps redis_build duckdb_build rocksdb_build)
  if(TARGET memcached_build)
    add_dependencies(bench_deps memcached_build)
  endif()

  message(STATUS "External benchmark dependencies will be built in: ${BENCH_DEPS_DIR}")
  message(STATUS "  Redis (libc malloc): ${BENCH_DEPS_DIR}/bin/redis-server-libc")
  message(STATUS "  DuckDB: ${BENCH_DEPS_DIR}/bin/duckdb")
  message(STATUS "  RocksDB: ${BENCH_DEPS_DIR}/lib/librocksdb.a")

  # ── RocksDB benchmark linked against built RocksDB ─────────────────────────
  # This version uses our freshly built RocksDB instead of system install
  add_executable(bench_rocksdb_builtin bench_rocksdb.cpp)
  _get_filtered_dir_compile_options(rocksdb_builtin_opts)
  set_target_properties(bench_rocksdb_builtin PROPERTIES COMPILE_OPTIONS "${rocksdb_builtin_opts}")
  target_include_directories(bench_rocksdb_builtin PRIVATE ${BENCH_DEPS_DIR}/include)
  target_link_libraries(bench_rocksdb_builtin PRIVATE
    ${BENCH_DEPS_DIR}/lib/librocksdb.a
    Threads::Threads
    ${CMAKE_DL_LIBS}
  )
  # Link against compression libs that RocksDB needs
  find_package(ZLIB)
  find_library(BZ2_LIB bz2)
  find_library(LZ4_SYSTEM_LIB lz4)
  find_library(ZSTD_SYSTEM_LIB zstd)
  find_library(SNAPPY_LIB snappy)
  if(ZLIB_FOUND)
    target_link_libraries(bench_rocksdb_builtin PRIVATE ZLIB::ZLIB)
  endif()
  if(BZ2_LIB)
    target_link_libraries(bench_rocksdb_builtin PRIVATE ${BZ2_LIB})
  endif()
  if(LZ4_SYSTEM_LIB)
    target_link_libraries(bench_rocksdb_builtin PRIVATE ${LZ4_SYSTEM_LIB})
  endif()
  if(ZSTD_SYSTEM_LIB)
    target_link_libraries(bench_rocksdb_builtin PRIVATE ${ZSTD_SYSTEM_LIB})
  endif()
  if(SNAPPY_LIB)
    target_link_libraries(bench_rocksdb_builtin PRIVATE ${SNAPPY_LIB})
  endif()
  add_dependencies(bench_rocksdb_builtin rocksdb_build)

endif() # SMASH_BUILD_BENCH_DEPS

# ── Configure benchmark scripts with dependency paths ────────────────────────
# These variables are used by the .sh.in templates
if(DEFINED BENCH_REDIS_SERVER)
  set(REDIS_SERVER_PATH ${BENCH_REDIS_SERVER})
else()
  set(REDIS_SERVER_PATH "redis-server")
endif()
if(DEFINED BENCH_MEMCACHED)
  set(MEMCACHED_PATH ${BENCH_MEMCACHED})
else()
  set(MEMCACHED_PATH "memcached")
endif()
if(DEFINED BENCH_DUCKDB)
  set(DUCKDB_PATH ${BENCH_DUCKDB})
else()
  set(DUCKDB_PATH "duckdb")
endif()
