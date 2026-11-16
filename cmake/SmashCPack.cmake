# CPack configuration: .deb, .rpm and .tar.gz of the install tree.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSMASH_BUILD_TESTS=OFF
#   cmake --build build --target smash
#   (cd build && cpack)                       # DEB + RPM + TGZ on Linux, TGZ on macOS
#   (cd build && cpack -G DEB)                # just one
#
# Release packages are built inside the manylinux_2_28 containers in
# .github/workflows/release.yml, so they inherit the same glibc 2.28 import
# floor as the raw .so assets (see agents/releases.md). That container has no
# dpkg, so nothing here may depend on dpkg tools (architecture and Depends are
# set explicitly instead of via dpkg --print-architecture / dpkg-shlibdeps).
#
# Version: defaults to project(VERSION). Release automation passes
# -DSMASH_PACKAGE_VERSION=<tag without the leading v, '-' -> '~'>, e.g.
# v0.0.1-alpha -> 0.0.1~alpha. '~' sorts *before* the bare version in both
# dpkg and rpm (so 0.0.1~alpha < 0.0.1), and rpm forbids '-' in Version.

set(SMASH_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Version string stamped into the .deb/.rpm/.tar.gz packages")

set(CPACK_PACKAGE_NAME "smash")
set(CPACK_PACKAGE_VENDOR "Emery Berger")
set(CPACK_PACKAGE_CONTACT "Emery Berger <emery.berger@gmail.com>")
set(CPACK_PACKAGE_VERSION "${SMASH_PACKAGE_VERSION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/emeryberger/smash")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Compression-aware malloc that transparently compresses cold heap pages")
set(CPACK_PACKAGE_DESCRIPTION
"smash is a drop-in malloc replacement that finds the parts of the heap a
program is not using and compresses them, so long-running services hold less
physical memory while idle. Load it with LD_PRELOAD (or the bundled `smash`
launcher); no code changes, root or kernel configuration are needed.")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
set(CPACK_STRIP_FILES ON)
set(CPACK_SOURCE_GENERATOR "")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(_smash_deb_arch arm64)
  set(_smash_rpm_arch aarch64)
  set(_smash_tgz_arch aarch64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  set(_smash_deb_arch amd64)
  set(_smash_rpm_arch x86_64)
  set(_smash_tgz_arch x86_64)
else()
  set(_smash_deb_arch ${CMAKE_SYSTEM_PROCESSOR})
  set(_smash_rpm_arch ${CMAKE_SYSTEM_PROCESSOR})
  set(_smash_tgz_arch ${CMAKE_SYSTEM_PROCESSOR})
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CPACK_GENERATOR "DEB;RPM;TGZ")
  set(_smash_os linux)
elseif(APPLE)
  set(CPACK_GENERATOR "TGZ")
  set(_smash_os macos)
  set(_smash_tgz_arch ${CMAKE_SYSTEM_PROCESSOR})
else()
  set(CPACK_GENERATOR "TGZ")
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" _smash_os)
endif()

# Tarball: smash-<ver>-<os>-<arch>.tar.gz containing smash-<ver>-<os>-<arch>/{bin,lib,include}.
set(CPACK_PACKAGE_FILE_NAME
    "smash-${CPACK_PACKAGE_VERSION}-${_smash_os}-${_smash_tgz_arch}")

# deb/rpm install under /usr; the tarball stays prefix-relative (the per-
# generator override lives in SmashCPackOptions.cmake, read at cpack time).
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
configure_file(${CMAKE_CURRENT_LIST_DIR}/SmashCPackOptions.cmake.in
               ${CMAKE_BINARY_DIR}/SmashCPackOptions.cmake @ONLY)
set(CPACK_PROJECT_CONFIG_FILE ${CMAKE_BINARY_DIR}/SmashCPackOptions.cmake)

# ── Debian ──
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE ${_smash_deb_arch})
set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
# lz4/zstd are static; the only runtime deps are libc, libstdc++ and libgcc_s. The
# glibc floor matches the manylinux_2_28 build (enforced in release.yml).
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.28), libstdc++6, libgcc-s1 | libgcc1")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
# Refresh the ld.so cache on install/remove (lintian lacks-ldconfig-trigger).
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${PROJECT_SOURCE_DIR}/packaging/debian/triggers")

# ── RPM ──
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_ARCHITECTURE ${_smash_rpm_arch})
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
set(CPACK_RPM_PACKAGE_GROUP "System Environment/Libraries")
set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_RPM_PACKAGE_RELEASE 1)
# Without this the RPM generator ignores CPACK_PACKAGE_DESCRIPTION and emits
# CPack's boilerplate "This is an installer created using CPack" text.
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
# rpmbuild's automatic Requires (glibc symbol versions, libstdc++.so.6) are
# kept; don't claim ownership of the shared /usr/{bin,lib,include} dirs.
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_PACKAGE_AUTOPROV OFF)
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /usr/include /usr/lib /usr/share/doc)

include(CPack)
