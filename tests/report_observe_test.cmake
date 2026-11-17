# SMASH_REPORT under SMASH_MODE=observe: the exit report must label the mode
# and must not print a "nothing was compressed because ..." hint (observe
# mode compresses nothing by design). Invoked as:
#   cmake -DPRELOAD_VAR=.. -DLIB=.. -DPROG=.. -DOUT=.. -P report_observe_test.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "${PRELOAD_VAR}=${LIB}" "MallocNanoZone=0"
          "SMASH_MODE=observe" "SMASH_COLD_TIMEOUT_SEC=1" "SMASH_DEFER_PHASES_MS=0"
          "SMASH_REPORT=${OUT}/report.txt" "${PROG}"
  RESULT_VARIABLE rc TIMEOUT 60 OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "observe workload failed (rc=${rc})")
endif()
file(READ "${OUT}/report.txt" body)
if(NOT body MATCHES "mode=observe")
  message(FATAL_ERROR "report does not label observe mode:\n${body}")
endif()
if(body MATCHES "hint: nothing was compressed")
  message(FATAL_ERROR "misleading zero-compression hint in observe mode:\n${body}")
endif()
message(STATUS "observe-mode report OK")
