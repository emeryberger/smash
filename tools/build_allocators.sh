#!/usr/bin/env bash
# Build jemalloc, mimalloc, and hoard for LD_PRELOAD benchmarking
set -euo pipefail

ALLOC_DIR="${1:-/tmp/allocators}"
mkdir -p "$ALLOC_DIR"
cd "$ALLOC_DIR"

NPROC=$(nproc)

echo "=== Building jemalloc ==="
if [ ! -f "$ALLOC_DIR/lib/libjemalloc.so" ]; then
  rm -rf jemalloc
  git clone --depth 1 --branch 5.3.0 https://github.com/jemalloc/jemalloc.git
  cd jemalloc
  ./autogen.sh
  ./configure --prefix="$ALLOC_DIR" --enable-shared --disable-static
  make -j$NPROC
  make install
  cd ..
else
  echo "  (already built)"
fi

echo ""
echo "=== Building mimalloc ==="
if [ ! -f "$ALLOC_DIR/lib/libmimalloc.so" ]; then
  rm -rf mimalloc
  git clone --depth 1 --branch v2.1.2 https://github.com/microsoft/mimalloc.git
  cd mimalloc
  mkdir -p build && cd build
  cmake .. -DCMAKE_INSTALL_PREFIX="$ALLOC_DIR" -DMI_BUILD_SHARED=ON -DMI_BUILD_STATIC=OFF -DMI_BUILD_TESTS=OFF
  make -j$NPROC
  make install
  cd ../..
else
  echo "  (already built)"
fi

echo ""
echo "=== Building Hoard ==="
if [ ! -f "$ALLOC_DIR/lib/libhoard.so" ]; then
  rm -rf Hoard
  git clone --depth 1 https://github.com/emeryberger/Hoard.git
  cd Hoard/src
  make -j$NPROC
  mkdir -p "$ALLOC_DIR/lib"
  cp libhoard.so "$ALLOC_DIR/lib/"
  cd ../..
else
  echo "  (already built)"
fi

echo ""
echo "=== Built allocator libraries ==="
ls -la "$ALLOC_DIR/lib"/lib{jemalloc,mimalloc,hoard}*.so* 2>/dev/null || echo "(none)"
