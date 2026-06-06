// uffd_bench.c — microbenchmark comparing two ways to implement smash's
// "compress a cold page, then decompress-on-fault when accessed" cycle, with a
// focus on the cross-core TLB-shootdown IPI cost.
//
// METHOD A "mprotect": models smash's current restore path. Pages are
//   "compressed" via madvise(MADV_DONTNEED)+mprotect(PROT_NONE) and "faulted
//   back" via mprotect(PROT_READ|PROT_WRITE)+memcpy. Changing PTE protection on
//   a mapped VMA forces a TLB shootdown IPI to every CPU running the process.
//
// METHOD B "uffd": models a proposed userfaultfd restore path. The region is
//   registered with userfaultfd in MISSING mode. "Compress" is just
//   madvise(MADV_DONTNEED) — the page becomes not-present. Accessing it raises
//   a uffd MISSING fault that a handler thread resolves with UFFDIO_COPY,
//   filling the not-present page. Filling a not-present page needs no TLB
//   invalidation, so there should be no shootdown IPI.
//
// Both methods run with N reader threads that keep live TLB entries on OTHER
// cores by touching a handful of distinct pages of the same region and then
// sleeping (NOT hot-spinning — a hot spin saturates every CPU and drowns the
// IPI signal in scheduler/cache noise). Method A's mprotect must therefore shoot
// down the TLBs of those cores.
//
// PRIMARY METRIC: direct TLB-shootdown IPI count read from /proc/interrupts
// (the "TLB:" row). We snapshot the system-wide TLB-shootdown IPI total
// immediately before and after each method's timed section and print
// "TLB_IPI_delta=N". This directly counts the cross-core shootdowns each method
// triggers, where wall-clock alone is too noisy. /proc/interrupts is
// system-wide so there is background noise; run on an otherwise-idle machine and
// compare the per-method deltas over the fixed workload. We also print
// wall-clock ns/page and METHOD_A_DONE / METHOD_B_DONE markers.
//
// Build:  gcc -O2 -pthread -o uffd_bench uffd_bench.c
//
// Usage:  uffd_bench [threads] [region_MiB] [pages_touched] [rounds] [method]
//   method is "A", "B", or "both" (default "both"). Single-method mode lets a
//   caller wrap one method with external measurement and avoid cross-method
//   interference.
//
// Defaults: 16 reader threads, 256 MiB region, 20000 pages touched, 5 rounds.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>

// UFFD_USER_MODE_ONLY: allows unprivileged userfaultfd that only handles
// user-mode faults. Defined in recent linux/userfaultfd.h; value is 1.
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

// O_CLOEXEC / O_NONBLOCK come from fcntl.h.

// ----------------------------------------------------------------------------
// Tunables (overridable via argv).
// ----------------------------------------------------------------------------
static size_t g_page_size = 4096;       // resolved at runtime
static int    g_num_readers = 16;       // reader threads on other pages
static size_t g_region_bytes = (size_t)256 * 1024 * 1024;  // 256 MiB
static size_t g_pages_touched = 20000;  // pages compressed+faulted per round
static int    g_rounds = 5;             // rounds over the touched pages

static unsigned char *g_region = NULL;  // base of the mmap'd region
static size_t g_region_pages = 0;       // total pages in the region

// Known byte pattern written into each page so we can verify integrity.
// Pattern for page index i is the byte (i & 0xFF) repeated, with a small header
// holding the page index so a swapped page would be caught.
static inline unsigned char page_pattern_byte(size_t page_idx) {
  return (unsigned char)((page_idx * 131u + 7u) & 0xFFu);
}

static void fill_known_pattern(unsigned char *dst, size_t page_idx) {
  unsigned char b = page_pattern_byte(page_idx);
  memset(dst, b, g_page_size);
  // Stamp the page index in the first 8 bytes so reordering is detectable.
  uint64_t idx = (uint64_t)page_idx;
  memcpy(dst, &idx, sizeof(idx));
}

static void verify_known_pattern(const unsigned char *src, size_t page_idx) {
  uint64_t idx = 0;
  memcpy(&idx, src, sizeof(idx));
  if (idx != (uint64_t)page_idx) {
    fprintf(stderr,
            "VERIFY FAILED: page %zu has stale index header %llu\n",
            page_idx, (unsigned long long)idx);
    abort();
  }
  unsigned char b = page_pattern_byte(page_idx);
  for (size_t off = sizeof(uint64_t); off < g_page_size; off++) {
    if (src[off] != b) {
      fprintf(stderr,
              "VERIFY FAILED: page %zu byte %zu = 0x%02x expected 0x%02x\n",
              page_idx, off, src[off], b);
      abort();
    }
  }
}

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

// ----------------------------------------------------------------------------
// Direct TLB-shootdown IPI counter (primary metric).
//
// /proc/interrupts has one row labelled "TLB:" (the "TLB shootdowns" row on
// x86; on some arches it appears under a different label, but "TLB:" is the
// common one). The row holds one integer column per CPU. We sum all per-CPU
// columns and return the system-wide total. Snapshot before/after a timed
// section and subtract to get the IPIs that section triggered.
//
// Note: this is system-wide, so there is background noise from other processes.
// Over a fixed 100k-page workload the per-method delta still separates mprotect
// (~1 shootdown IPI per mprotect of a mapped page) from uffd (UFFDIO_COPY into a
// not-present page → ideally ~0 shootdowns). Returns 0 if the row can't be read.
// ----------------------------------------------------------------------------
static unsigned long long read_tlb_ipis(void) {
  FILE *f = fopen("/proc/interrupts", "r");
  if (!f) return 0ULL;
  unsigned long long total = 0ULL;
  char line[8192];
  while (fgets(line, sizeof(line), f)) {
    // Find the first non-space character and check for a "TLB:" label.
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "TLB:", 4) != 0) continue;
    // Sum every integer column that follows the label. Trailing columns are a
    // textual description ("TLB shootdowns"); strtoull simply yields 0 / stops
    // there, so summing only the leading numeric columns is automatic.
    char *q = p + 4;
    for (;;) {
      // Skip non-digit separators.
      while (*q == ' ' || *q == '\t') q++;
      if (*q < '0' || *q > '9') break;  // hit the description text → done
      char *end = NULL;
      unsigned long long v = strtoull(q, &end, 10);
      if (end == q) break;
      total += v;
      q = end;
    }
    break;  // only one TLB: row
  }
  fclose(f);
  return total;
}

// ----------------------------------------------------------------------------
// Reader threads: keep live TLB entries on OTHER cores so method A's mprotect
// must shoot them down — but WITHOUT saturating CPUs. A hot spin burns 100% on
// every core and drowns the IPI signal in scheduler/cache noise. Instead each
// reader touches a small handful of distinct pages (enough to keep TLB entries
// resident) and then sleeps ~50 µs before the next batch. The threads are alive
// on other cores with live mappings, not pegged.
// ----------------------------------------------------------------------------
static atomic_int g_readers_run = 0;          // 1 while readers should run
static atomic_long g_reader_checksum = 0;     // sink so reads aren't elided
static size_t g_reader_lo_page = 0;           // [lo, hi) read range (other pages)
static size_t g_reader_hi_page = 0;

// Pages each reader touches per batch before sleeping. Small enough to stay
// cheap, large enough to keep several TLB entries warm on the core.
#define READER_BATCH_PAGES 8
// Sleep between batches (~50 µs). Keeps threads runnable/scheduled across cores
// without hot-spinning.
#define READER_SLEEP_NS 50000L

static void *reader_main(void *arg) {
  long id = (long)arg;
  // Each reader walks a strided slice of the reader range.
  long local = 0;
  size_t lo = g_reader_lo_page;
  size_t hi = g_reader_hi_page;
  if (hi <= lo) hi = lo + 1;
  size_t span = hi - lo;
  size_t cursor = lo + ((size_t)id % span);
  struct timespec nap = {0, READER_SLEEP_NS};
  while (atomic_load_explicit(&g_readers_run, memory_order_relaxed)) {
    // Touch a handful of distinct pages to keep their TLB entries resident.
    for (int k = 0; k < READER_BATCH_PAGES; k++) {
      const volatile unsigned char *p = g_region + cursor * g_page_size;
      local += p[0];
      local += p[g_page_size / 2];
      cursor += 1;
      if (cursor >= hi) cursor = lo;
    }
    // Back off so we don't saturate the CPU (drowns the IPI signal in noise).
    nanosleep(&nap, NULL);
  }
  atomic_fetch_add_explicit(&g_reader_checksum, local, memory_order_relaxed);
  return NULL;
}

static pthread_t *g_reader_threads = NULL;

static void start_readers(void) {
  // Readers occupy the upper half of the region; compress/fault uses the lower
  // portion (first g_pages_touched pages). Ensure they don't overlap.
  size_t lo = g_pages_touched;
  if (lo >= g_region_pages) lo = g_region_pages / 2;
  g_reader_lo_page = lo;
  g_reader_hi_page = g_region_pages;
  if (g_reader_hi_page <= g_reader_lo_page) {
    g_reader_hi_page = g_reader_lo_page + 1;
  }
  // Pre-touch reader pages so they are present (live TLB entries) and not part
  // of the compress/fault workload.
  for (size_t i = g_reader_lo_page; i < g_reader_hi_page; i++) {
    g_region[i * g_page_size] = (unsigned char)i;
  }
  atomic_store(&g_readers_run, 1);
  g_reader_threads = calloc((size_t)g_num_readers, sizeof(pthread_t));
  for (int i = 0; i < g_num_readers; i++) {
    if (pthread_create(&g_reader_threads[i], NULL, reader_main,
                       (void *)(long)i) != 0) {
      perror("pthread_create(reader)");
      exit(1);
    }
  }
}

static void stop_readers(void) {
  atomic_store(&g_readers_run, 0);
  for (int i = 0; i < g_num_readers; i++) {
    pthread_join(g_reader_threads[i], NULL);
  }
  free(g_reader_threads);
  g_reader_threads = NULL;
}

// ----------------------------------------------------------------------------
// METHOD A: mprotect compress + restore.
// ----------------------------------------------------------------------------
static double run_method_a(unsigned long long *out_tlb_ipi_delta) {
  // Initialize the workload pages with the known pattern (present + mapped).
  for (size_t i = 0; i < g_pages_touched; i++) {
    fill_known_pattern(g_region + i * g_page_size, i);
  }

  unsigned long long ipi0 = read_tlb_ipis();
  double t0 = now_ns();
  for (int r = 0; r < g_rounds; r++) {
    for (size_t i = 0; i < g_pages_touched; i++) {
      unsigned char *page = g_region + i * g_page_size;
      // "Compress": drop physical page + make inaccessible.
      if (madvise(page, g_page_size, MADV_DONTNEED) != 0) {
        perror("madvise(MADV_DONTNEED) [A]");
        exit(1);
      }
      if (mprotect(page, g_page_size, PROT_NONE) != 0) {
        perror("mprotect(PROT_NONE) [A]");
        exit(1);
      }
      // "Decompress on fault": restore protection (TLB shootdown IPI here) and
      // write the known data back.
      if (mprotect(page, g_page_size, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect(PROT_RW) [A]");
        exit(1);
      }
      fill_known_pattern(page, i);
      verify_known_pattern(page, i);
    }
  }
  double t1 = now_ns();
  unsigned long long ipi1 = read_tlb_ipis();
  if (out_tlb_ipi_delta) {
    *out_tlb_ipi_delta = (ipi1 >= ipi0) ? (ipi1 - ipi0) : 0ULL;
  }
  return t1 - t0;
}

// ----------------------------------------------------------------------------
// METHOD B: userfaultfd MISSING-mode compress + restore.
// ----------------------------------------------------------------------------
static int g_uffd = -1;
static atomic_int g_uffd_run = 0;
static atomic_long g_uffd_faults = 0;
static unsigned char *g_uffd_fill_buf = NULL;  // one page of scratch

static long uffd_syscall(int flags) {
  return syscall(SYS_userfaultfd, flags);
}

// Resolve a single fault at fault_addr by copying the correct known pattern in.
static int uffd_resolve(unsigned long fault_addr) {
  unsigned long base = (unsigned long)g_region;
  unsigned long aligned = fault_addr & ~(unsigned long)(g_page_size - 1);
  size_t page_idx = (size_t)((aligned - base) / g_page_size);

  fill_known_pattern(g_uffd_fill_buf, page_idx);

  struct uffdio_copy copy;
  memset(&copy, 0, sizeof(copy));
  copy.dst = aligned;
  copy.src = (unsigned long)g_uffd_fill_buf;
  copy.len = g_page_size;
  copy.mode = 0;
  copy.copy = 0;
  if (ioctl(g_uffd, UFFDIO_COPY, &copy) != 0) {
    // EEXIST means the page was already filled (lost race) — benign.
    if (errno != EEXIST) {
      perror("ioctl(UFFDIO_COPY)");
      return -1;
    }
  }
  atomic_fetch_add_explicit(&g_uffd_faults, 1, memory_order_relaxed);
  return 0;
}

static void *uffd_handler_main(void *arg) {
  (void)arg;
  struct pollfd pfd;
  pfd.fd = g_uffd;
  pfd.events = POLLIN;
  while (atomic_load_explicit(&g_uffd_run, memory_order_relaxed)) {
    int pr = poll(&pfd, 1, 10 /* ms */);
    if (pr < 0) {
      if (errno == EINTR) continue;
      perror("poll(uffd)");
      break;
    }
    if (pr == 0) continue;  // timeout — re-check run flag
    if (!(pfd.revents & POLLIN)) continue;

    struct uffd_msg msg;
    ssize_t n = read(g_uffd, &msg, sizeof(msg));
    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
      break;
    }
    if (n != (ssize_t)sizeof(msg)) continue;
    if (msg.event == UFFD_EVENT_PAGEFAULT) {
      if (uffd_resolve((unsigned long)msg.arg.pagefault.address) != 0) {
        // A failed copy other than EEXIST is fatal for correctness.
        exit(1);
      }
    }
  }
  return NULL;
}

// Returns 0 on success (method B ran), -1 if uffd is unavailable.
// On success, *out_elapsed_ns holds the measured time and
// *out_tlb_ipi_delta holds the TLB-shootdown IPI delta over the timed section.
static int run_method_b(double *out_elapsed_ns,
                        unsigned long long *out_tlb_ipi_delta) {
  long fd = uffd_syscall(O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
  if (fd < 0) {
    // Retry without UFFD_USER_MODE_ONLY in case the running kernel predates it.
    int saved = errno;
    fd = uffd_syscall(O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
      fprintf(stderr,
              "userfaultfd() unavailable: %s (also without USER_MODE_ONLY: %s)"
              " — skipping METHOD B\n",
              strerror(saved), strerror(errno));
      return -1;
    }
  }
  g_uffd = (int)fd;

  // API handshake.
  struct uffdio_api api;
  memset(&api, 0, sizeof(api));
  api.api = UFFD_API;
  api.features = 0;
  if (ioctl(g_uffd, UFFDIO_API, &api) != 0) {
    fprintf(stderr, "ioctl(UFFDIO_API) failed: %s — skipping METHOD B\n",
            strerror(errno));
    close(g_uffd);
    g_uffd = -1;
    return -1;
  }

  // Register the workload page range in MISSING mode.
  struct uffdio_register reg;
  memset(&reg, 0, sizeof(reg));
  reg.range.start = (unsigned long)g_region;
  reg.range.len = (unsigned long)(g_pages_touched * g_page_size);
  reg.mode = UFFDIO_REGISTER_MODE_MISSING;
  if (ioctl(g_uffd, UFFDIO_REGISTER, &reg) != 0) {
    fprintf(stderr, "ioctl(UFFDIO_REGISTER) failed: %s — skipping METHOD B\n",
            strerror(errno));
    close(g_uffd);
    g_uffd = -1;
    return -1;
  }

  g_uffd_fill_buf = mmap(NULL, g_page_size, PROT_READ | PROT_WRITE,
                         MAP_ANON | MAP_PRIVATE, -1, 0);
  if (g_uffd_fill_buf == MAP_FAILED) {
    perror("mmap(uffd_fill_buf)");
    exit(1);
  }

  // Start the handler thread.
  atomic_store(&g_uffd_run, 1);
  pthread_t handler;
  if (pthread_create(&handler, NULL, uffd_handler_main, NULL) != 0) {
    perror("pthread_create(uffd_handler)");
    exit(1);
  }

  // Prime: the pages are currently not-present (freshly registered, never
  // written). First access of each will fault through uffd, establishing the
  // initial contents.
  unsigned long long ipi0 = read_tlb_ipis();
  double t0 = now_ns();
  for (int r = 0; r < g_rounds; r++) {
    for (size_t i = 0; i < g_pages_touched; i++) {
      unsigned char *page = g_region + i * g_page_size;
      // "Decompress on fault": touch triggers MISSING fault -> handler
      // UFFDIO_COPY fills it. Read forces the fault.
      verify_known_pattern(page, i);
      // "Compress": drop the page. It returns to not-present, so the next
      // access re-faults through uffd. No mprotect, no TLB shootdown.
      if (madvise(page, g_page_size, MADV_DONTNEED) != 0) {
        perror("madvise(MADV_DONTNEED) [B]");
        exit(1);
      }
    }
  }
  double t1 = now_ns();
  unsigned long long ipi1 = read_tlb_ipis();
  if (out_tlb_ipi_delta) {
    *out_tlb_ipi_delta = (ipi1 >= ipi0) ? (ipi1 - ipi0) : 0ULL;
  }

  // Tear down.
  atomic_store(&g_uffd_run, 0);
  pthread_join(handler, NULL);

  struct uffdio_range unreg;
  memset(&unreg, 0, sizeof(unreg));
  unreg.start = (unsigned long)g_region;
  unreg.len = (unsigned long)(g_pages_touched * g_page_size);
  if (ioctl(g_uffd, UFFDIO_UNREGISTER, &unreg) != 0) {
    perror("ioctl(UFFDIO_UNREGISTER)");
    // not fatal for the measurement
  }
  munmap(g_uffd_fill_buf, g_page_size);
  g_uffd_fill_buf = NULL;
  close(g_uffd);
  g_uffd = -1;

  *out_elapsed_ns = t1 - t0;
  return 0;
}

// ----------------------------------------------------------------------------
static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [threads] [region_MiB] [pages_touched] [rounds] [method]\n"
          "  threads        reader threads on other pages (default 16)\n"
          "  region_MiB     region size in MiB (default 256)\n"
          "  pages_touched  pages compressed+faulted per round (default 20000)\n"
          "  rounds         rounds over the touched pages (default 5)\n"
          "  method         A | B | both (default both)\n",
          prog);
}

int main(int argc, char **argv) {
  long ps = sysconf(_SC_PAGESIZE);
  if (ps > 0) g_page_size = (size_t)ps;

  if (argc > 1) {
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
      usage(argv[0]);
      return 0;
    }
    g_num_readers = atoi(argv[1]);
    if (g_num_readers < 0) g_num_readers = 0;
  }
  if (argc > 2) {
    long mib = atol(argv[2]);
    if (mib > 0) g_region_bytes = (size_t)mib * 1024 * 1024;
  }
  if (argc > 3) {
    long p = atol(argv[3]);
    if (p > 0) g_pages_touched = (size_t)p;
  }
  if (argc > 4) {
    int r = atoi(argv[4]);
    if (r > 0) g_rounds = r;
  }
  // method: A, B, or both (default). run_a / run_b gate which methods execute.
  int run_a = 1, run_b = 1;
  if (argc > 5) {
    const char *m = argv[5];
    if (strcmp(m, "A") == 0 || strcmp(m, "a") == 0) {
      run_a = 1; run_b = 0;
    } else if (strcmp(m, "B") == 0 || strcmp(m, "b") == 0) {
      run_a = 0; run_b = 1;
    } else if (strcmp(m, "both") == 0 || strcmp(m, "BOTH") == 0) {
      run_a = 1; run_b = 1;
    } else {
      fprintf(stderr, "unknown method '%s' (expected A, B, or both)\n", m);
      usage(argv[0]);
      return 1;
    }
  }

  // Round region up to a multiple of page size.
  g_region_bytes = (g_region_bytes + g_page_size - 1) & ~(g_page_size - 1);
  g_region_pages = g_region_bytes / g_page_size;

  // Need room for the touched pages plus at least some reader pages.
  if (g_pages_touched >= g_region_pages) {
    fprintf(stderr,
            "pages_touched (%zu) >= region pages (%zu); enlarge region or "
            "reduce pages_touched\n",
            g_pages_touched, g_region_pages);
    return 1;
  }

  printf("config: readers=%d region=%zu MiB (%zu pages) "
         "pages_touched=%zu rounds=%d page_size=%zu method=%s\n",
         g_num_readers, g_region_bytes / (1024 * 1024), g_region_pages,
         g_pages_touched, g_rounds, g_page_size,
         (run_a && run_b) ? "both" : (run_a ? "A" : "B"));
  fflush(stdout);

  g_region = mmap(NULL, g_region_bytes, PROT_READ | PROT_WRITE,
                  MAP_ANON | MAP_PRIVATE, -1, 0);
  if (g_region == MAP_FAILED) {
    perror("mmap(region)");
    return 1;
  }

  start_readers();

  size_t total_pages = g_pages_touched * (size_t)g_rounds;

  // ---- METHOD A ----
  double a_ns = 0.0;
  unsigned long long a_ipi = 0ULL;
  int a_ran = 0;
  if (run_a) {
    a_ns = run_method_a(&a_ipi);
    a_ran = 1;
    printf("METHOD_A_DONE mprotect: total=%.3f ms  %.1f ns/page  (%zu pages)  "
           "TLB_IPI_delta=%llu\n",
           a_ns / 1e6, a_ns / (double)total_pages, total_pages, a_ipi);
    fflush(stdout);
  }

  // ---- METHOD B ----
  double b_ns = 0.0;
  unsigned long long b_ipi = 0ULL;
  int b_ok = -1;
  if (run_b) {
    b_ok = run_method_b(&b_ns, &b_ipi);
    if (b_ok == 0) {
      printf("METHOD_B_DONE uffd: total=%.3f ms  %.1f ns/page  (%zu pages, "
             "%ld faults resolved)  TLB_IPI_delta=%llu\n",
             b_ns / 1e6, b_ns / (double)total_pages, total_pages,
             atomic_load(&g_uffd_faults), b_ipi);
      fflush(stdout);
    } else {
      printf("METHOD_B_DONE uffd: SKIPPED (userfaultfd unavailable)\n");
      fflush(stdout);
    }
  }

  stop_readers();

  if (a_ran && b_ok == 0) {
    double speedup = a_ns / (b_ns > 0 ? b_ns : 1.0);
    printf("summary: mprotect %.1f ns/page (TLB_IPI_delta=%llu), "
           "uffd %.1f ns/page (TLB_IPI_delta=%llu), uffd is %.2fx %s\n",
           a_ns / (double)total_pages, a_ipi,
           b_ns / (double)total_pages, b_ipi,
           speedup >= 1.0 ? speedup : 1.0 / speedup,
           speedup >= 1.0 ? "faster" : "slower");
  }

  // Keep the reader checksum live so the compiler can't elide reader work.
  printf("reader_checksum=%ld\n", atomic_load(&g_reader_checksum));

  munmap(g_region, g_region_bytes);
  return 0;
}
