# syntax=docker/dockerfile:1
#
# smash distribution image: a FROM-scratch image holding only
#
#   /smash/lib/libsmash.so
#   /smash/bin/smash          (POSIX sh launcher; needs /bin/sh in YOUR image)
#
# It is not meant to be run. Copy it into your own image:
#
#   COPY --from=ghcr.io/emeryberger/smash:VERSION /smash /opt/smash
#   ENV LD_PRELOAD=/opt/smash/lib/libsmash.so
#
# (see packaging/docker/example/Dockerfile). libsmash is built inside
# manylinux_2_28, so it has the same glibc 2.28 import floor as the release
# .so assets and runs in any glibc >= 2.28 base image (Debian 11+, Ubuntu
# 20.04+, UBI/Alma/Rocky 8+, AL2023, distroless/cc). musl (Alpine) is not
# supported.
#
# Build (from the repository root; BuildKit required for the per-arch stage
# selection — `docker buildx build`, or DOCKER_BUILDKIT=1):
#
#   docker buildx build -f packaging/docker/Dockerfile -t smash:dev .
#   docker buildx build -f packaging/docker/Dockerfile \
#     --platform linux/amd64,linux/arm64 -t ghcr.io/emeryberger/smash:VERSION --push .
#
# alloc8, LZ4, zstd and printf are fetched by CMake FetchContent during the
# build (network needed). To build against a local alloc8 checkout instead,
# pass it as a named build context:
#   --build-context alloc8=../alloc8 --build-arg ALLOC8_FROM_CONTEXT=1

ARG TARGETARCH
ARG SMASH_FROM=source

# One manylinux_2_28 build stage per architecture; BuildKit only builds the
# one selected by TARGETARCH below.
FROM quay.io/pypa/manylinux_2_28_x86_64 AS manylinux-amd64
FROM quay.io/pypa/manylinux_2_28_aarch64 AS manylinux-arm64

# Placeholder for the optional named build context (empty unless overridden).
FROM scratch AS alloc8

FROM manylinux-${TARGETARCH} AS out-source
ARG ALLOC8_FROM_CONTEXT=0
RUN /opt/python/cp312-cp312/bin/python -m pip install --no-cache-dir cmake ninja >/dev/null \
 && ln -sf /opt/python/cp312-cp312/bin/cmake /usr/local/bin/cmake \
 && ln -sf /opt/python/cp312-cp312/bin/ninja /usr/local/bin/ninja
COPY --from=alloc8 / /alloc8/
COPY . /src
# Configure/build/install, then the same glibc-floor gate as release.yml
# (fail if an imported symbol needs glibc > 2.28), then stage only the two
# shipped files.
RUN set -eu; \
    if [ "$ALLOC8_FROM_CONTEXT" = 1 ]; then a8="-DALLOC8_DIR=/alloc8"; else a8=""; fi; \
    cmake -S /src -B /build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DSMASH_BUILD_TESTS=OFF \
      -DSMASH_BUILD_BENCH=OFF \
      -DCMAKE_INSTALL_PREFIX=/stage \
      $a8; \
    cmake --build /build --target smash -j"$(nproc)"; \
    cmake --install /build; \
    max=$(objdump -T /stage/lib/libsmash.so | grep '\*UND\*' \
          | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -V | tail -1); \
    top=$(printf '%s\n%s\n' 2.28 "$max" | sort -V | tail -1); \
    [ "$top" = 2.28 ] || { echo "libsmash imports glibc $max > 2.28" >&2; exit 1; }; \
    mkdir -p /out/smash/lib /out/smash/bin; \
    cp /stage/lib/libsmash.so /out/smash/lib/; \
    cp /stage/bin/smash /out/smash/bin/; \
    chmod 0644 /out/smash/lib/libsmash.so; chmod 0755 /out/smash/bin/smash

# SMASH_FROM=prebuilt: package already-built release binaries instead of
# compiling (used by release.yml so the image's libsmash.so is byte-identical
# to the release asset, and so the multi-arch image needs no emulation — this
# stage has no RUN). Expects prebuilt/<amd64|arm64>/{lib/libsmash.so,bin/smash}
# in the build context.
FROM scratch AS out-prebuilt
ARG TARGETARCH
COPY prebuilt/${TARGETARCH}/ /out/smash/

FROM out-${SMASH_FROM} AS out

FROM scratch
LABEL org.opencontainers.image.title="smash" \
      org.opencontainers.image.description="libsmash (compression-aware malloc) for COPY --from into glibc images" \
      org.opencontainers.image.source="https://github.com/emeryberger/smash" \
      org.opencontainers.image.url="https://github.com/emeryberger/smash" \
      org.opencontainers.image.licenses="Apache-2.0"
COPY --from=out /out/smash /smash
