// releaseThreadAltStack() must leave the thread with NO alternate signal stack
// registered before it unmaps the memory. A registered-but-unmapped alternate
// stack turns the thread's next signal into a failed delivery, which the kernel
// answers by killing the process with SIGILL. On macOS 26, SS_DISABLE with
// ss_size < MINSIGSTKSZ fails with ENOMEM, so a zero-initialised stack_t never
// disabled it (test_soak_large_only died with SIGILL at thread exit, TE=1).
#include "vm/fault_handler.h"

#include <pthread.h>
#include <signal.h>
#include <cstdio>
#include <cstdlib>

namespace {
int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); ++g_failures; } } while (0)

void* body(void*) {
    stack_t cur{};
    CHECK(sigaltstack(nullptr, &cur) == 0 && (cur.ss_flags & SS_DISABLE),
          "new thread already has an alternate stack");
    smash::vm::installThreadAltStack();
    char* const mem = smash::vm::g_altstack_mem;
    CHECK(mem != nullptr, "installThreadAltStack did not install");
    CHECK(sigaltstack(nullptr, &cur) == 0 && !(cur.ss_flags & SS_DISABLE),
          "alternate stack not registered after install");
    smash::vm::releaseThreadAltStack();
    CHECK(sigaltstack(nullptr, &cur) == 0, "sigaltstack query failed");
    CHECK(cur.ss_flags & SS_DISABLE,
          "alternate stack still registered after release (sp=%p flags=%d): "
          "its memory was unmapped under a live registration", cur.ss_sp, cur.ss_flags);
    CHECK(smash::vm::g_altstack_mem == nullptr, "release kept the stack");
    return nullptr;
}
}  // namespace

int main() {
    setenv("SMASH_ALTSTACK", "1", 1);  // read once, on first altStackWanted()
    for (int i = 0; i < 4; ++i) {
        pthread_t t;
        if (pthread_create(&t, nullptr, body, nullptr) != 0) { fprintf(stderr, "FAIL: pthread_create\n"); return 1; }
        pthread_join(t, nullptr);
    }
    if (g_failures) { fprintf(stderr, "test_altstack_release: %d failure(s)\n", g_failures); return 1; }
    printf("test_altstack_release: PASS\n");
    return 0;
}
