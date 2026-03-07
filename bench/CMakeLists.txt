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
