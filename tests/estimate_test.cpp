// test_smash_estimate: run smash-estimate in launch mode against
// estimate_target (it is the target's parent, so process_vm_readv works with
// the default yama ptrace_scope=1 and no root) and check the JSON against the
// target's known layout. argv: <smash-estimate> <estimate_target> [method]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static double num(const std::string& js, const char* key) {
    std::string k = std::string("\"") + key + "\": ";
    size_t p = js.find(k);
    if (p == std::string::npos) return -1;
    return atof(js.c_str() + p + k.size());
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: estimate_test <smash-estimate> <target> [method]\n"); return 2; }
    std::string cmd = std::string(argv[1]) + " --json --warmup 2 --window 4 --points 2 --kill";
    if (argc > 3) cmd += std::string(" --method ") + argv[3];
    cmd += std::string(" -- ") + argv[2];
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) { perror("popen"); return 1; }
    std::string js;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) js.append(buf, n);
    int rc = pclose(f);
    printf("%s", js.c_str());
    if (rc != 0) { fprintf(stderr, "FAIL: smash-estimate exited with %d\n", rc); return 1; }
    const double MiB = 1 << 20;
    const double idle = num(js, "idle_bytes"), saving = num(js, "projected_saving_bytes");
    const double ci = num(js, "ci95_bytes");
    printf("idle=%.1f MiB saving=%.1f MiB +/- %.1f MiB\n", idle / MiB, saving / MiB, ci / MiB);
    // Idle: cold_comp + cold_rand = 80 MiB; the two hot 16 MiB regions are out.
    if (idle < 72 * MiB || idle > 92 * MiB) { fprintf(stderr, "FAIL: idle %.1f MiB not in [72, 92]\n", idle / MiB); return 1; }
    // Savings: the 64 MiB compressible region; random pages fail the ratio gate.
    if (saving < 54 * MiB || saving > 72 * MiB) { fprintf(stderr, "FAIL: saving %.1f MiB not in [54, 72]\n", saving / MiB); return 1; }
    if (js.find("\"savings_available\": true") == std::string::npos) { fprintf(stderr, "FAIL: no savings\n"); return 1; }
    printf("PASS\n");
    return 0;
}
