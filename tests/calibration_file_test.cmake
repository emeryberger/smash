# Regression guard: SMASH_CALIBRATION_FILE must neither hang nor crash.
# The file is read/written during heap construction; stdio there (fopen →
# malloc → getHeap()) recursed forever on Linux. Run 1 calibrates and saves
# the file, run 2 loads it. Invoked as:
#   cmake -DPRELOAD_VAR=.. -DLIB=.. -DPROG=.. -DCAL=.. -P calibration_file_test.cmake
file(REMOVE "${CAL}")
foreach(run save load)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "${PRELOAD_VAR}=${LIB}" "MallocNanoZone=0"
            "SMASH_CALIBRATION_FILE=${CAL}" "SMASH_LARGE_ONLY=0" "SMASH_CPU_ARENA=0" "${PROG}"
    RESULT_VARIABLE rc TIMEOUT 20 OUTPUT_QUIET ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "calibration ${run} run failed (rc=${rc}): ${err}")
  endif()
  if(NOT EXISTS "${CAL}")
    message(FATAL_ERROR "calibration ${run} run did not leave ${CAL}")
  endif()
endforeach()
file(READ "${CAL}" body)
if(NOT body MATCHES "\"fast_comp_mbs_hi\": [0-9]+" OR NOT body MATCHES "\"deep_ratio_random\": [0-9.]+")
  message(FATAL_ERROR "calibration file malformed:\n${body}")
endif()
message(STATUS "calibration save+load OK")
