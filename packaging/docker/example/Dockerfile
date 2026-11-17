# Example: run an application under smash by copying the smash image in.
#
#   docker build -t smash-example packaging/docker/example
#   docker run --rm smash-example
#
# Any glibc >= 2.28 base works (Debian 11+, Ubuntu 20.04+, UBI 8+, AL2023,
# distroless/cc); Alpine/musl does not.
ARG SMASH_IMAGE=ghcr.io/emeryberger/smash:latest
FROM ${SMASH_IMAGE} AS smash

FROM python:3.12-slim
COPY --from=smash /smash /opt/smash

# Preload for every process in the container. Alternatively leave LD_PRELOAD
# unset and start the app through the launcher:
#   ENTRYPOINT ["/opt/smash/bin/smash", "run", "--"]
# PYTHONMALLOC=malloc: CPython's own small-object allocator otherwise hides
# allocations from malloc. SMASH_LARGE_ONLY=1 is the production-supported mode
# for runtimes with their own small-object allocator. SMASH_STATS=1 prints a
# one-line summary at exit (drop it in production).
ENV LD_PRELOAD=/opt/smash/lib/libsmash.so \
    PYTHONMALLOC=malloc \
    SMASH_LARGE_ONLY=1 \
    SMASH_STATS=1

CMD ["python3", "-c", "b = bytearray(64 << 20); b[::4096] = b'x' * len(b[::4096]); print('example ok', len(b))"]
