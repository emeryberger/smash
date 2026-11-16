// Target program for tests/check_launcher.sh: run via `smash run`, it checks
// that libsmash was actually preloaded (the exported smash_arena_for hook is
// visible to dlsym) and allocates a little so the exit report has content.
// `launcher_test wait N` additionally sleeps N seconds (for `smash stats`).
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

int main(int argc, char** argv) {
    void* sym = dlsym(RTLD_DEFAULT, "smash_arena_for");
    if (!sym) {
        printf("launcher_test: smash NOT loaded\n");
        return 1;
    }
    void* p = malloc(256);
    memset(p, 1, 256);
    using ArenaFor = int (*)(const void*);
    printf("launcher_test: smash loaded (arena_for=%d)\n", reinterpret_cast<ArenaFor>(sym)(p));
    fflush(stdout);
    if (argc == 3 && strcmp(argv[1], "wait") == 0) sleep(static_cast<unsigned>(atoi(argv[2])));
    free(p);
    return 0;
}
