# SMASH_DEBUG=1 (+ SMASH_BUCKET_STATS=1) must print periodic stats lines from
# the coordinator thread without crashing. Invoked as:
#   cmake -DPRELOAD_VAR=.. -DLIB=.. -DPROG=.. -P debug_stats_test.cmake
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "${PRELOAD_VAR}=${LIB}" "MallocNanoZone=0"
          "SMASH_DEBUG=1" "SMASH_BUCKET_STATS=1" "${PROG}"
  RESULT_VARIABLE rc TIMEOUT 40 OUTPUT_QUIET ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "SMASH_DEBUG run failed (rc=${rc}):\n${err}")
endif()
if(NOT err MATCHES "\\[smash stats\\]")
  message(FATAL_ERROR "no periodic [smash stats] line printed:\n${err}")
endif()
message(STATUS "SMASH_DEBUG periodic stats OK")
