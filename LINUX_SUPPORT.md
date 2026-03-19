# Linux Support Plan

## Current Status

Smash works on Linux with the following results:
- **SQLite**: 65.9% RSS reduction ✓
- **RocksDB**: 78.5% RSS reduction ✓
- **JSON/KV benchmarks**: 55-69% RSS reduction ✓
- **Memcached**: Works ✓ (fixed with bufferInHeap check)
- **Redis**: Works but low RSS reduction (~0.4%) - uses jemalloc
- **DuckDB**: Works ✓ (fixed with bufferInHeap check)

## Known Issues

### 1. ~~Memcached Crash: `epoll_wait: Bad address`~~ FIXED

**Status**: Fixed by adding `bufferInHeap()` check to all syscall wrappers.

**Root Cause**: Syscall wrappers were calling warmPages/pinPages on buffers allocated by libevent (mmap'd memory), not by Smash. This caused EFAULT when the kernel tried to access those pages.

**Solution**: Added `bufferInHeap()` function that checks if a buffer is within Smash's VmRegion before warming/pinning. Only operate on pages we actually manage.

### 2. Redis Low RSS Reduction

**Symptom**: Redis shows only 0.4% RSS reduction vs 78% for RocksDB.

**Likely Cause**: Redis uses jemalloc by default (compiled in), so malloc interposition does not capture Redis allocations.

**Next Steps**:
- [ ] Build Redis with system malloc (`make MALLOC=libc`)
- [ ] Test with explicit LD_PRELOAD precedence

### 3. ~~DuckDB Intermittent Crashes~~ FIXED

**Status**: Fixed by the same `bufferInHeap()` check that fixed memcached.

## Architecture Differences: Linux vs macOS

| Aspect | macOS | Linux |
|--------|-------|-------|
| Page size | 16KB (ARM64) | 4KB |
| Interposition | DYLD_INSERT_LIBRARIES | LD_PRELOAD |
| Syscall wrapping | `__interpose` section | dlsym(RTLD_NEXT) |
| Mach VM APIs | Available | N/A |
| RSS reading | task_info() | /proc/self/status |

## Implemented Linux Fixes

1. **RSS reading**: Use direct `syscall(SYS_read)` to read /proc/self/status instead of std::ifstream (avoids Smash's read() wrapper on internal buffers)

2. **Syscall wrappers** (linux_syscall_wrappers.cpp):
   - read/write/pread/pwrite: warm+pin buffer pages
   - recv/send/recvfrom/sendto: warm+pin buffer pages
   - recvmsg/sendmsg: warm+pin iovec buffers
   - Removed: poll/epoll_wait (caused libevent issues)

3. **Benchmark script fixes**:
   - Use `ss` instead of `netstat` on Linux
   - Handle ConnectionResetError in memcached benchmark
   - Use `fuser` instead of `lsof` for port checks

## Proposed Fix for libevent Apps

Add heap range check before warming buffers:

```cpp
ssize_t recv(int fd, void* buf, size_t count) {
    auto* vm = smash::g_smash_vm_region;
    // Only warm if buffer is in Smash-managed heap
    bool in_heap = vm && vm->contains(reinterpret_cast<uintptr_t>(buf));
    if (in_heap) {
        smash::vm::warmPages(buf, count, vm);
        smash::vm::pinPages(buf, count, vm);
    }
    ssize_t ret = real_recv(fd, buf, count);
    if (in_heap) {
        smash::vm::unpinPages(buf, count, vm);
    }
    return ret;
}
```

This ensures we only operate on pages we actually manage.

## Compress-Only Mode

For applications that use mmap for large allocations (like DuckDB), use `libsmash_compress_only.so`:

```bash
LD_PRELOAD=./libsmash_compress_only.so duckdb ...
```

This mode:
- Interposes on malloc/mmap but forwards to the system allocator
- Tracks pages allocated through malloc/mmap
- Compresses cold pages using the same LZ4/zstd pipeline
- Achieves ~80% RSS reduction on compressible data

Key difference from full Smash: compress-only works with ANY allocator since it tracks
pages after allocation, not during. It's ideal for applications where you can't replace
the allocator but still want compression benefits.

## Test Matrix

| App | System Malloc | Smash (no compress) | Smash (full) |
|-----|--------------|---------------------|--------------|
| SQLite | ✓ | ✓ | ✓ 66.0% |
| RocksDB | ✓ | ✓ | ✓ 73.1% |
| Redis | ✓ | ✓ | ✓ 0.4%* |
| Memcached | ✓ | ✓ | ✓ (working) |
| DuckDB | ✓ | ✓ | ✓ 0%** |

*Redis low reduction due to built-in jemalloc
**DuckDB uses mmap for buffer pool - use compress-only mode (libsmash_compress_only.so) instead

## Priority

1. ~~**High**: Fix memcached crash~~ DONE
2. **Medium**: Improve Redis RSS reduction (build with system malloc)
3. ~~**Low**: Fix DuckDB~~ DONE
