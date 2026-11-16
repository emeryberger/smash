#!/usr/bin/env bash
# Build libsmash + its .deb/.rpm/.tar.gz packages inside a manylinux_2_28
# container, so everything inherits the glibc 2.28 import floor documented in
# agents/releases.md. Used by .github/workflows/release.yml (build-linux) and
# for local verification:
#
#   docker run --rm -v "$PWD/..:/work" -w /work \
#     quay.io/pypa/manylinux_2_28_$(uname -m) \
#     bash smash/packaging/linux/build-manylinux.sh /work/smash /work/alloc8 0.0.1~alpha
#
# Args: <smash src dir> <alloc8 src dir> [package version]
# Env:  SMASH_BUILD_DIR (default <smash>/build)
# Outputs: $SMASH_BUILD_DIR/libsmash.so and $SMASH_BUILD_DIR/pkg/smash-*.{deb,rpm,tar.gz}
set -euo pipefail
SRC=${1:?smash source dir}
ALLOC8=${2:?alloc8 source dir}
VERSION=${3:-}
B=${SMASH_BUILD_DIR:-$SRC/build}

# manylinux ships gcc-toolset (C++20-capable) on PATH; cmake/ninja via the
# bundled CPython pip. rpmbuild for CPack's RPM generator (the DEB generator
# is self-contained and needs no dpkg).
/opt/python/cp312-cp312/bin/python -m pip install --upgrade cmake ninja >/dev/null
ln -sf /opt/python/cp312-cp312/bin/cmake /usr/local/bin/cmake
ln -sf /opt/python/cp312-cp312/bin/cpack /usr/local/bin/cpack
ln -sf /opt/python/cp312-cp312/bin/ninja /usr/local/bin/ninja
command -v rpmbuild >/dev/null || dnf install -y -q rpm-build >/dev/null
cmake --version | head -1; gcc --version | head -1

# Point at the sibling alloc8 checkout (no network fetch of alloc8; lz4/zstd
# are still FetchContent). Library only.
cmake -S "$SRC" -B "$B" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DALLOC8_DIR="$ALLOC8" \
  -DSMASH_BUILD_TESTS=OFF \
  -DSMASH_BUILD_BENCH=OFF \
  ${VERSION:+-DSMASH_PACKAGE_VERSION="$VERSION"}
cmake --build "$B" --target smash -j

(cd "$B" && cpack -G "DEB;RPM;TGZ" -B "$B/pkg")
rm -rf "$B/pkg/_CPack_Packages"
ls -l "$B/pkg"

# Build runs as container root; make outputs world-readable so the host
# steps (objdump, upload-artifact) can read them.
chmod -R a+rX "$B"
