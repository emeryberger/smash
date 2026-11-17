# macOS: the compressor's 1 s tick must hold at background QoS (taskpolicy -b),
# where the kernel coalesces short timers. Counting 100 x usleep(10 ms) made a
# tick take ~10 s there (5 ticks in ~52 s); the tick is now bounded by the
# monotonic clock. debug_stats_payload exits at the first SMASH_DEBUG stats
# line (every 5 ticks), so the run time measures 5 ticks. Invoked as:
#   cmake -DLIB=.. -DPROG=.. -P tick_cadence_test.cmake
string(TIMESTAMP t0 "%s")
execute_process(
  COMMAND taskpolicy -b ${CMAKE_COMMAND} -E env "DYLD_INSERT_LIBRARIES=${LIB}" "MallocNanoZone=0"
          "SMASH_DEBUG=1" "SMASH_CALIBRATE=never" "${PROG}"
  RESULT_VARIABLE rc TIMEOUT 120 OUTPUT_QUIET ERROR_VARIABLE err)
string(TIMESTAMP t1 "%s")
math(EXPR dt "${t1} - ${t0}")
if(NOT rc EQUAL 0 OR NOT err MATCHES "\\[smash stats\\]")
  message(FATAL_ERROR "no stats line at background QoS (rc=${rc}, ${dt} s):\n${err}")
endif()
if(dt GREATER 15)
  message(FATAL_ERROR "5 compressor ticks took ${dt} s at background QoS (want <= 15 s)")
endif()
message(STATUS "5 ticks at background QoS in ${dt} s")
