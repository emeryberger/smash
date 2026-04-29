# bench/tcmalloc_patch_build.cmake
#
# Append a Bazel cc_binary target that produces a libtcmalloc_preload.so
# suitable for LD_PRELOAD into the upstream google/tcmalloc package's
# BUILD file. Invoked by bench/CMakeLists.txt's tcmalloc_build
# ExternalProject_Add as:
#
#     cmake -DBUILD_FILE=<SOURCE_DIR>/tcmalloc/BUILD
#           -P bench/tcmalloc_patch_build.cmake
#
# Just before this script runs, an empty tcmalloc_so.cc is written into
# the same directory; the cc_binary rule below uses it as a no-op
# translation unit so Bazel's link picks up :tcmalloc transitively.
#
# Idempotent: re-running on an already-patched BUILD file is a no-op.

if(NOT DEFINED BUILD_FILE)
    message(FATAL_ERROR "BUILD_FILE not set; pass via -DBUILD_FILE=<path>")
endif()

if(NOT EXISTS "${BUILD_FILE}")
    message(FATAL_ERROR "BUILD_FILE does not exist: ${BUILD_FILE}")
endif()

# Idempotency marker: if our cc_binary block is already present, don't
# append it again. Re-running PATCH_COMMAND happens whenever
# ExternalProject_Add re-stamps the patch step (e.g. after a checkout
# refresh), and double-patching would cause Bazel to fail with a
# duplicate-target error.
file(READ "${BUILD_FILE}" _existing)
if(_existing MATCHES "name = \"libtcmalloc_preload.so\"")
    message(STATUS "tcmalloc_patch_build: BUILD already patched, skipping")
    return()
endif()

# Append a cc_binary that:
#   - uses linkshared = True so Bazel emits a .so;
#   - depends on :tcmalloc (the upstream allocator cc_library — same
#     package, so no visibility extension required);
#   - takes tcmalloc_so.cc as a no-op TU so the link succeeds without
#     pulling our own malloc shims into the .so.
#
# Using a multi-line raw-bracket argument (`[=[ ... ]=]`) so the Bazel
# Starlark code is appended verbatim with no CMake variable expansion.
file(APPEND "${BUILD_FILE}" [=[


# ── Added by smash bench/tcmalloc_patch_build.cmake ──────────────────────────
# Self-contained shared library suitable for LD_PRELOAD. Built by
# `bazel build //tcmalloc:libtcmalloc_preload.so`.
cc_binary(
    name = "libtcmalloc_preload.so",
    srcs = ["tcmalloc_so.cc"],
    linkshared = True,
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = [":tcmalloc"],
)
]=])

message(STATUS "tcmalloc_patch_build: appended libtcmalloc_preload.so cc_binary to ${BUILD_FILE}")
