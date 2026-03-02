# smash/bench/CMakeLists.txt - Standalone benchmark executables

set(SMASH_BENCH_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# Helper to add a benchmark executable
function(smash_add_bench name source)
  add_executable(${name} ${source})
  target_include_directories(${name} PRIVATE ${SMASH_BENCH_INCLUDE_DIRS})
  target_link_libraries(${name} PRIVATE alloc8::headers lz4_static libzstd_static)
  if(APPLE)
    target_compile_options(${name} PRIVATE -ftls-model=initial-exec)
  endif()
endfunction()

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
