// free_probe.c - log who actually owns the `free` symbol that _isl.so calls,
// after both libsmash and _isl.so have been mapped.
//
// Build: gcc -shared -fPIC -O0 -o free_probe.so free_probe.c -ldl
// Use:   LD_PRELOAD=libsmash.so:free_probe.so python3 ...
//
// Strategy: at constructor time, walk the process's loaded modules
// (dl_iterate_phdr), find _isl.so, locate its .plt.got entry for `free`,
// dereference it, and log where it points.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>
#include <unistd.h>

static int report_for(const char* path, struct dl_phdr_info* info, size_t /*size*/)
{
    // Find PT_DYNAMIC.
    const ElfW(Dyn)* dyn = NULL;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = (const ElfW(Dyn)*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return 0;

    const ElfW(Sym)*  symtab = NULL;
    const char*       strtab = NULL;
    const ElfW(Rela)* rela_plt = NULL;
    size_t            relasz_plt = 0;

    for (; dyn->d_tag != DT_NULL; ++dyn) {
        switch (dyn->d_tag) {
            case DT_SYMTAB:    symtab = (const ElfW(Sym)*)dyn->d_un.d_ptr; break;
            case DT_STRTAB:    strtab = (const char*)dyn->d_un.d_ptr; break;
            case DT_JMPREL:    rela_plt = (const ElfW(Rela)*)dyn->d_un.d_ptr; break;
            case DT_PLTRELSZ:  relasz_plt = dyn->d_un.d_val; break;
        }
    }
    if (!symtab || !strtab || !rela_plt || !relasz_plt) return 0;

    size_t n = relasz_plt / sizeof(ElfW(Rela));
    for (size_t i = 0; i < n; ++i) {
        const ElfW(Rela)* r = &rela_plt[i];
        unsigned long sym_idx = ELF64_R_SYM(r->r_info);
        const char* name = strtab + symtab[sym_idx].st_name;
        if (strcmp(name, "free") != 0) continue;

        // Read what the PLT GOT slot currently points at.
        void** got_slot = (void**)(info->dlpi_addr + r->r_offset);
        void* target = *got_slot;
        Dl_info info2;
        const char* tgt_dso = "?";
        const char* tgt_sym = "?";
        if (dladdr(target, &info2)) {
            if (info2.dli_fname) tgt_dso = info2.dli_fname;
            if (info2.dli_sname) tgt_sym = info2.dli_sname;
        }
        fprintf(stderr,
            "[free_probe] %s: free@plt slot=%p -> target=%p (%s :: %s)\n",
            path, (void*)got_slot, target, tgt_dso, tgt_sym);
        fflush(stderr);
        return 0;
    }
    return 0;
}

static int callback(struct dl_phdr_info* info, size_t size, void* /*data*/)
{
    const char* name = info->dlpi_name;
    if (!name || !*name) return 0;
    // Filter to libraries we care about.
    if (strstr(name, "_isl.cpython") || strstr(name, "libwalrus") ||
        strstr(name, "libsmash"))
    {
        report_for(name, info, size);
    }
    return 0;
}

__attribute__((constructor(65535)))  // run as late as possible
static void probe_init(void)
{
    fprintf(stderr, "[free_probe] startup probe\n");
    dl_iterate_phdr(callback, NULL);
}

// Re-probe whenever someone dlopens a new library (Python imports an
// extension). The simplest way: replace the `dlopen` symbol and re-probe
// after each successful load.
typedef void* (*real_dlopen_t)(const char*, int);

void* dlopen(const char* file, int mode)
{
    static real_dlopen_t real_dlopen = NULL;
    if (!real_dlopen)
        real_dlopen = (real_dlopen_t)dlsym(RTLD_NEXT, "dlopen");
    if (!real_dlopen) return NULL;
    void* h = real_dlopen(file, mode);
    if (h && file && (strstr(file, "_isl.cpython") || strstr(file, "libwalrus"))) {
        fprintf(stderr, "[free_probe] post-dlopen %s\n", file);
        dl_iterate_phdr(callback, NULL);
    }
    return h;
}
