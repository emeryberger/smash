// death_trace.c - LD_PRELOAD shim that catches every fatal signal AND every
// _exit() / _Exit() / exit() with a non-zero status, then writes a stack +
// PID + signal/exit info to /tmp/death.<pid>.log AND stderr.
//
// Used to chase silent worker deaths in concurrent.futures.ProcessPoolExecutor:
// when a worker dies and the parent reports "process pool was terminated
// abruptly", this gives us the actual cause + stack.
//
// Build: gcc -O0 -fPIC -shared -o death_trace.so death_trace.c -ldl
// Use:   LD_PRELOAD=libsmash.so:death_trace.so python3 ...
//
// Notes:
// - SA_SIGINFO so we get si_addr / si_code (helpful for SIGSEGV).
// - Re-raise via SIG_DFL so the kernel still reports the right exit code.
// - dlsym(RTLD_NEXT, "_exit") so we can hook it without going through
//   libc's TLS-using stdio paths.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

static void write_trace(int sig, const char *kind, void *fault_addr) {
    char path[64];
    int pid = (int)getpid();
    int n = snprintf(path, sizeof(path), "/tmp/death.%d.log", pid);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) fd = 2;

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "=== %s pid=%d tid=%ld sig=%d fault_addr=%p ===\n",
        kind, pid, (long)syscall(SYS_gettid), sig, fault_addr);
    if (hlen > 0) {
        write(fd, hdr, (size_t)hlen);
        write(2,  hdr, (size_t)hlen);
    }

    void *bt[96];
    int nframes = backtrace(bt, 96);
    backtrace_symbols_fd(bt, nframes, fd);
    backtrace_symbols_fd(bt, nframes, 2);

    const char *trail = "=== end ===\n";
    write(fd, trail, strlen(trail));
    write(2,  trail, strlen(trail));
    if (fd != 2) close(fd);
}

static void on_signal(int sig, siginfo_t *info, void *ctx) {
    (void)ctx;
    const char *kind = "SIGNAL";
    switch (sig) {
        case SIGABRT: kind = "SIGABRT"; break;
        case SIGSEGV: kind = "SIGSEGV"; break;
        case SIGBUS:  kind = "SIGBUS";  break;
        case SIGFPE:  kind = "SIGFPE";  break;
        case SIGILL:  kind = "SIGILL";  break;
        case SIGSYS:  kind = "SIGSYS";  break;
    }
    write_trace(sig, kind, info ? info->si_addr : NULL);
    // Restore default handler and re-raise so the kernel reports the
    // correct exit status.
    signal(sig, SIG_DFL);
    raise(sig);
}

// Hook _exit / _Exit so we know if the process is leaving via that path.
typedef void (*exit_fn)(int);

static void log_exit(const char *fn, int status) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "[death] pid=%d %s(%d) called\n", (int)getpid(), fn, status);
    if (n > 0) write(2, buf, (size_t)n);
    if (status != 0) write_trace(0, "EXIT_NONZERO", NULL);
}

void _exit(int status) {
    log_exit("_exit", status);
    static exit_fn real;
    if (!real) real = (exit_fn)dlsym(RTLD_NEXT, "_exit");
    real(status);
    syscall(SYS_exit_group, status);
    while (1) {}
}

void _Exit(int status) {
    log_exit("_Exit", status);
    static exit_fn real;
    if (!real) real = (exit_fn)dlsym(RTLD_NEXT, "_Exit");
    real(status);
    syscall(SYS_exit_group, status);
    while (1) {}
}

__attribute__((constructor(50000)))
static void install_handlers(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGSYS };
    for (size_t i = 0; i < sizeof(sigs)/sizeof(sigs[0]); ++i) {
        sigaction(sigs[i], &sa, NULL);
    }
    char banner[64];
    int n = snprintf(banner, sizeof(banner),
        "[death] installed pid=%d\n", (int)getpid());
    if (n > 0) write(2, banner, (size_t)n);
}
