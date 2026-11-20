# Syscall & Buffered I/O Compatibility

Smash's mprotect-based monitoring (PROT_READ) and compression (PROT_NONE) can conflict with kernel syscalls that access userspace buffers. **The kernel does not raise a signal for syscall-side faults**: when `copy_from_user`/`copy_to_user` (Linux) or `copyin`/`copyout` (macOS) hits a protected page during a syscall, the kernel uses the page-fault fixup path to convert the fault into `-EFAULT` and discard the address. The SIGSEGV/SIGBUS handler in `fault_handler.h` only fires for direct user-code accesses; for syscalls we have to detect EFAULT ourselves and trigger a userspace touch (which *does* go through the handler) so the page can be decompressed.

## EFAULT-driven decompress-and-retry (`syscall_compat.h::retryWithDecompress`)

Every buffer-taking syscall wrapper follows the same shape: call the real syscall, on `errno == EFAULT` walk the buffer pages (one byte per page; the SIGSEGV handler decompresses), then retry. Bounded to 8 attempts with us-scale exponential backoff (1, 2, 4, ..., 128 us); the compressor tick fires at ~10 ms intervals so 8 attempts at 128 us is well inside one tick. Bounded so unmapped-pointer bugs surface as EFAULT instead of livelocking. `retryMachOnInvalidData` is the same shape but keyed on `MACH_RCV_INVALID_DATA` / `MACH_SEND_INVALID_DATA` for the mach_msg trio.

The PageState CAS + per-page lock are sufficient for correctness — there is no separate pin counter. The fault handler takes the same per-page lock as the compressor's `transition(ACTIVE -> COMPRESSING)`, so syscall-retry -> walk -> fault -> handler -> decompress -> retry is correct without a pin counter.

## DYLD interposition limitations (macOS) — the buffered-I/O carve-out

`__DATA,__interpose` only intercepts **cross-dylib** GOT calls. Intra-libSystem calls are invisible:
- `fread()`/`fgetc()` -> internal `read()`: NOT intercepted (same dylib)
- `getc_unlocked` is a macro (`__sgetc`) inlined into callers, calls `__srget` for refills
- C++ iostream (`std::cin`) -> `getc_unlocked` -> `__srget` -> `read()` (all intra-libSystem)
- Direct `read()` from application code: IS intercepted (cross-dylib)

For these paths the EFAULT-retry model does not help — those syscalls happen inside libc and never enter our wrapper. Two carve-outs exist:

1. **Buffered I/O wrappers** (`fread`/`fgets`/`fgetc`/`getc`/`fwrite`/`fflush` on both platforms): proactively `warmPages` the user buffer + the FILE's internal buffer (macOS `stream->_bf._base`, Linux `stream->_IO_buf_base`) before delegating to libc. There is no retry path because libc's internal `__read` is intra-dylib.
2. **stdio buffer warming** (`compressor_thread.h::warmStdioBuffers`, macOS-only): re-warms `stdin`/`stdout`/`stderr` FILE struct + buffer each compressor tick so the kernel never finds them protected. Without this, intra-libSystem `getc_unlocked` / `__srget` would EFAULT inside libc with no retry surface.

## Interposed functions

- macOS (`smash_heap.cpp`): `read`, `write`, `pread`, `pwrite`, `readv`, `writev`, `recv`, `send`, `recvfrom`, `sendto`, `recvmsg`, `sendmsg`, `poll`, `kevent`, `kevent64`, `mach_msg`, `mach_msg_overwrite`, `mach_msg2_internal`, plus the buffered-I/O carve-out above.
- Linux (`linux_syscall_wrappers.cpp`): everything macOS has minus the Mach trio plus `ppoll`, `select`, `pselect`, `accept`, `accept4`, `recvmmsg`, `sendmmsg`, `getsockopt`, `getsockname`, `getpeername`, `getrandom`, `epoll_wait`, `epoll_pwait`.
- Memory-management wrappers for `SMASH_TRACK_EXTERNAL` (see [external-tracking.md](external-tracking.md)): `mmap`, `mmap64`, `munmap`, `mremap`, `madvise`, `mprotect`, `mlock`, `mlock2`, `mlockall`, `sbrk`, `__sbrk`, `sigaltstack`, `ioctl` (an app's `UFFDIO_REGISTER`; forwarded with a raw `SYS_ioctl`, and smash's own ioctls bypass it), liburing's `io_uring_register_buffers`, `_tags`, `_update_tag` (versioned `LIBURING_2.0`/`2.1`, resolved with `dlvsym`) (Linux); `mmap`, `munmap`, `madvise`, `mprotect`, `mlock`, `mlockall`, `minherit`, `sigaltstack`, `mach_vm_allocate`/`deallocate`, `vm_allocate`/`deallocate` (macOS). smash's own calls use raw syscalls (`vm::rawMmap` / `rawMunmap` / `rawMadvise` / `rawMprotect` / `rawSigaltstack`) so they never re-enter these.
- Path arguments (the kernel's string copy fails with `EFAULT` on a compressed page, no signal): Linux `open`, `open64`, `openat`, `openat64`, `creat`, `rename`, `renameat`, `renameat2`, `unlink`, `unlinkat`, `rmdir`, `mkdir`, `mkdirat`, `access`, `chdir`, `truncate`, `link`, `symlink`, `chmod`, `fopen`, `fopen64`, `freopen`, plus the existing stat/readlink/xattr wrappers; macOS `open`, `openat`, `rename`, `unlink`, `rmdir`, `mkdir`, `access`, `fopen`, `stat`. Each calls `vm::warmString` (one `VmRegion::contains()` unless the path is smash-managed). Linux path wrappers other than `open*`/`fopen*` issue the raw `*at` syscalls (no `dlsym`); the fault path itself opens `/proc/self/mem` with a raw `openat`.
- Buffered-I/O first fill: the stdio buffer is malloc'd inside libc on the first read, after the wrapper's warm. If `fread`/`fgets`/`fgetc`/`getc` fail with `EFAULT` on a stream that had no buffer before the call, nothing was consumed: the wrapper clears the error, warms the new buffer and retries once (`test_stdio_first_fill`).
- Signal masks, both platforms: `pthread_sigmask` (Linux: also the `GLIBC_2.32` version), `sigprocmask` — `SIGSEGV`/`SIGBUS` are removed from any set being blocked, because a synchronous fault on a thread that blocks them is never delivered to the decompression handler (the kernel kills the process).

## Platform-specific interposition patterns

**macOS**: Do NOT use `dlsym(RTLD_NEXT)` — it returns the wrapper itself. Instead read the `.original` field from the interpose struct:
```cpp
extern "C" ssize_t smash_read(int fd, void* buf, size_t count);
SMASH_INTERPOSE(smash_read, read);
extern "C" ssize_t smash_read(int fd, void* buf, size_t count) {
    auto* vm = smash::g_smash_vm_region;
    return smash::vm::retryWithDecompress(
        [&] { return reinterpret_cast<read_fn>(smash_interpose_smash_read.original)(fd, buf, count); },
        [&] { if (vm && buf && count) smash::vm::walkPagesForFault(buf, count, vm); });
}
```

**Linux**: Use `dlsym(RTLD_NEXT)` with lazy resolution in `linux_syscall_wrappers.cpp`. For versioned glibc symbols (e.g., `epoll_wait@GLIBC_2.3.2` used by libevent), create aliased wrapper functions with `.symver` directives and export both versions in `smash_version_script.map`:
```cpp
// Wrapper for GLIBC_2.3.2 version
SMASH_VISIBLE int epoll_wait_232(...) { return epoll_wait(...); }
__asm__(".symver epoll_wait_232,epoll_wait@GLIBC_2.3.2");
```
