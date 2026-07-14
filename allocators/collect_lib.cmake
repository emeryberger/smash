# collect_lib.cmake — copy built allocator shared libraries into one directory.
#
# Invoked from allocators/CMakeLists.txt at ExternalProject install time:
#   cmake -DSRC_DIR=... -DDEST_DIR=... -DPATTERN=libfoo*.so* -P collect_lib.cmake
#
# Each allocator drops its shared library somewhere different (mimalloc under
# lib/mimalloc-<ver>/, gperftools straight in the build dir, jemalloc under
# lib/), so we glob rather than hard-code a path.  FOLLOW_SYMLINK_CHAIN brings
# the versioned real file along with the libfoo.so symlink that points at it,
# which is what LD_PRELOAD needs.

if(NOT SRC_DIR OR NOT DEST_DIR OR NOT PATTERN)
  message(FATAL_ERROR "collect_lib.cmake requires -DSRC_DIR, -DDEST_DIR and -DPATTERN")
endif()

file(GLOB_RECURSE _libs LIST_DIRECTORIES FALSE "${SRC_DIR}/${PATTERN}")

if(NOT _libs)
  message(FATAL_ERROR
    "collect_lib.cmake: no library matching '${PATTERN}' found under ${SRC_DIR}")
endif()

file(MAKE_DIRECTORY "${DEST_DIR}")
file(COPY ${_libs} DESTINATION "${DEST_DIR}" FOLLOW_SYMLINK_CHAIN)

foreach(_lib IN LISTS _libs)
  get_filename_component(_name "${_lib}" NAME)
  message(STATUS "collected ${_name} -> ${DEST_DIR}")
endforeach()
