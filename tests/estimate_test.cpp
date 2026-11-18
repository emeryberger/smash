// test_smash_estimate: run smash-estimate in launch mode against
// estimate_target (it is the target's parent, so process_vm_readv works with
// the default yama ptrace_scope=1 and no root) and check the JSON against the
// target's known layout. argv: <smash-estimate> <estimate_target> [method] [thp]
//
// method "idle" needs root and CONFIG_IDLE_PAGE_TRACKING: the test runs
// smash-estimate via `sudo -n` when it is not root, and exits 77 (skip) when
// neither works or the kernel lacks /sys/kernel/mm/page_idle. "thp" backs the
// target's regions with transparent huge pages (skip if THP is disabled) and
// requires the estimate to say it tracked large folios.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

static double num(const std::string& js, const char* key) {
    std::string k = std::string("\"") + key + "\": ";
    size_t p = js.find(k);
    if (p == std::string::npos) return -1;
    return atof(js.c_str() + p + k.size());
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: estimate_test <smash-estimate> <target> [method] [thp]\n"); return 2; }
    const std::string method = argc > 3 ? argv[3] : "";
    const bool thp = argc > 4 && strcmp(argv[4], "thp") == 0;
    std::string prefix;
    if (method == "idle") {
        if (access("/sys/kernel/mm/page_idle/bitmap", F_OK) != 0) {
            printf("SKIP: kernel lacks CONFIG_IDLE_PAGE_TRACKING\n");
            return 77;
        }
        if (geteuid() != 0) {
            if (system("sudo -n true >/dev/null 2>&1") != 0) {
                printf("SKIP: idle method needs root (no passwordless sudo)\n");
                return 77;
            }
            prefix = "sudo -n ";
        }
    }
    if (thp) {
        FILE* t = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
        char buf[128] = {};
        if (!t || !fgets(buf, sizeof(buf), t) || strstr(buf, "[never]")) {
            if (t) fclose(t);
            printf("SKIP: transparent huge pages unavailable\n");
            return 77;
        }
        fclose(t);
    }
    std::string cmd = prefix + argv[1] + " --json --warmup 2 --window 4 --points 2 --kill";
    if (!method.empty()) cmd += " --method " + method;
    cmd += std::string(" -- ") + argv[2];
    if (thp) cmd += " thp";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) { perror("popen"); return 1; }
    std::string js;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) js.append(buf, n);
    int rc = pclose(f);
    printf("%s", js.c_str());
    if (rc != 0) { fprintf(stderr, "FAIL: smash-estimate exited with %d\n", rc); return 1; }
    if (!method.empty() && js.find("\"method\": \"" + method + "\"") == std::string::npos) {
        fprintf(stderr, "FAIL: method is not %s\n", method.c_str());
        return 1;
    }
    const double MiB = 1 << 20;
    const double idle = num(js, "idle_bytes"), saving = num(js, "projected_saving_bytes");
    const double ci = num(js, "ci95_bytes");
    printf("idle=%.1f MiB saving=%.1f MiB +/- %.1f MiB\n", idle / MiB, saving / MiB, ci / MiB);
    // Idle: cold_comp + cold_rand = 80 MiB; the two hot 16 MiB regions are out.
    if (idle < 72 * MiB || idle > 92 * MiB) { fprintf(stderr, "FAIL: idle %.1f MiB not in [72, 92]\n", idle / MiB); return 1; }
    // Savings: the 64 MiB compressible region; random pages fail the ratio gate.
    if (saving < 54 * MiB || saving > 72 * MiB) { fprintf(stderr, "FAIL: saving %.1f MiB not in [54, 72]\n", saving / MiB); return 1; }
    if (js.find("\"savings_available\": true") == std::string::npos) { fprintf(stderr, "FAIL: no savings\n"); return 1; }
    // zero_read's 16 MiB of zero-page PTEs are present in pagemap but not in
    // RSS; with PFNs visible (idle method) they must not count as present.
    const double present = num(js, "anon_present_bytes"), rss = num(js, "rss_bytes");
    if (method == "idle" && present > rss + 4 * MiB) {
        fprintf(stderr, "FAIL: anon present %.1f MiB > RSS %.1f MiB (zero page counted?)\n", present / MiB, rss / MiB);
        return 1;
    }
    // The idle bitmap tracks a THP by its head page only; the estimate must
    // have seen (and resolved) large folios for this case to mean anything.
    if (thp && method == "idle" && js.find("in large folios") == std::string::npos) {
        fprintf(stderr, "FAIL: no large folios tracked (target not THP-backed?)\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
