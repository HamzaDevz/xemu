/*
 * xboxrun — runs an Xbox title in the native core from the command line
 * (adb shell, /data/local/tmp), without the app. Used for the frontier loop
 * while the core has no display.
 *
 *   xboxrun <disc image>
 */
#include "native/core/core.h"
#include "native/core/mmio.h"
#include "native/hle/hle_native.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>

static NcCore g_core;

/* capture.c: NC_CAPTURE_EVERY=<s> writes the presented frame to frames/. */
void nc_capture_start(void);
static void dump_ranges(const char *spec);

/* Hex dump of mapped guest bytes (unmapped pages show as ..). */
static void dump_guest(const NcVm *vm, const char *what, uint32_t va, unsigned len)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   %s %08x:", what, va);
    for (unsigned i = 0; i < len && n < (int)sizeof(buf) - 4; i++) {
        if (nc_vm_phys(vm, va + i) == UINT32_MAX) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " ..");
        } else {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %02x", *(const uint8_t *)nc_vm_host(vm, va + i));
        }
    }
    buf[n++] = '\n';
    write(2, buf, (size_t)n);
}

/* Guest state at a signal: registers live in host registers inside JIT code
 * (FEX's 32-bit static mapping: eax x4, ecx x7, edx x5, ebx x6, esp x8, ebp x9,
 * esi x10, edi x11). */
static void report_guest(const mcontext_t *mc, uint32_t eip)
{
    if (eip) {
        static const char *const names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
        static const int regs[8] = {4, 7, 5, 6, 8, 9, 10, 11};
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:  ");
        for (int i = 0; i < 8; i++) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s=%08x", names[i], (uint32_t)mc->regs[regs[i]]);
        }
        buf[n++] = '\n';
        write(2, buf, (size_t)n);
        dump_guest(&g_core.vm, "code", eip - 32, 64);
        dump_guest(&g_core.vm, "stack", (uint32_t)mc->regs[8], 64);
    }
    nc_mmio_dump_stats(2);
    nc_kernel_dump_recent(2);
    nc_kernel_dump_threads(&g_core.kernel, 2);
}

/* The faulting host thread and the mappings around it (a crash path: not
 * async-signal-safe, but the process is going down). */
static void report_host_thread(uintptr_t pc, uintptr_t sp, uintptr_t addr)
{
    char buf[512], comm[64] = "?";
    const int tid = (int)gettid();
    snprintf(buf, sizeof(buf), "/proc/self/task/%d/comm", tid);
    FILE *f = fopen(buf, "r");
    if (f) {
        if (fgets(comm, sizeof(comm), f)) {
            comm[strcspn(comm, "\n")] = 0;
        }
        fclose(f);
    }
    int n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   thread %d (%s) sp=%llx (main %d)\n", tid, comm,
                     (unsigned long long)sp, (int)getpid());
    write(2, buf, (size_t)n);
    f = fopen("/proc/self/maps", "r");
    if (!f) {
        return;
    }
    char line[400];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) != 2) {
            continue;
        }
        const char *what = pc >= lo && pc < hi ? "pc" : sp >= lo && sp < hi ? "sp" : addr >= lo && addr < hi ? "addr"
                         : addr >= hi && addr - hi < 0x10000 ? "below addr" : addr < lo && lo - addr < 0x10000 ? "above addr"
                                                                                                                   : NULL;
        if (what) {
            n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   map %-10s %s", what, line);
            write(2, buf, (size_t)n);
        }
    }
    fclose(f);
}

static void fault(int sig, siginfo_t *info, void *ctx)
{
    if (nc_core_fault(&g_core, sig, info, ctx)) {
        return;
    }
    const mcontext_t *mc = &((ucontext_t *)ctx)->uc_mcontext;
    uint64_t guest = nc_vm_guest_of(&g_core.vm, info->si_addr);
    uint32_t eip = nc_cpu_guest_eip_at(g_core.cpu, (uintptr_t)mc->pc);
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "hakuX-native-frontier: fault sig=%d host_pc=%llx addr=%p guest_addr=%s%llx "
                     "guest_eip=%08x\n",
                     sig, (unsigned long long)mc->pc, info->si_addr,
                     guest == UINT64_MAX ? "host:" : "", (unsigned long long)(guest == UINT64_MAX ? 0 : guest),
                     eip);
    write(2, buf, (size_t)n);
    report_host_thread((uintptr_t)mc->pc, (uintptr_t)mc->sp, (uintptr_t)info->si_addr);
    if (eip) {
        n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   host insn %08x\n", *(const uint32_t *)(uintptr_t)mc->pc);
        write(2, buf, (size_t)n);
    }
    /* Host code: where in which image (llvm-addr2line on the unstripped build). */
    Dl_info dl;
    if (!eip && dladdr((void *)(uintptr_t)mc->pc, &dl) && dl.dli_fbase) {
        n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   host %s+0x%llx (%s)\n", dl.dli_fname,
                     (unsigned long long)((uintptr_t)mc->pc - (uintptr_t)dl.dli_fbase),
                     dl.dli_sname ? dl.dli_sname : "?");
        write(2, buf, (size_t)n);
        n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   host lr +0x%llx\n",
                     (unsigned long long)((uintptr_t)mc->regs[30] - (uintptr_t)dl.dli_fbase));
        write(2, buf, (size_t)n);
    }
    if (g_core.kernel.in_call && g_core.kernel.frame) {
        /* Inside a kernel function: the guest state is the call's frame. */
        NcFrame *f = g_core.kernel.frame;
        static const char *const names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
        n = snprintf(buf, sizeof(buf), "hakuX-native-frontier:   in a kernel call; guest");
        for (int i = 0; i < 8; i++) {
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %s=%08x", names[i], nc_frame_gpr(f, i));
        }
        buf[n++] = '\n';
        write(2, buf, (size_t)n);
        const uint32_t esp = nc_frame_gpr(f, NC_ESP);
        dump_guest(&g_core.vm, "stack", esp, 64);
        dump_guest(&g_core.vm, "stack", esp + 64, 64);
        const uint32_t ret = nc_vm_phys(&g_core.vm, esp) != UINT32_MAX ? nc_vm_rd32(&g_core.vm, esp) : 0;
        dump_guest(&g_core.vm, "caller", ret - 48, 64);
        dump_ranges(getenv("NC_FAULT_DUMP"));
        report_guest(mc, 0);
    } else {
        report_guest(mc, eip);
        dump_ranges(getenv("NC_FAULT_DUMP"));
    }
    _exit(3);
}

/* NC_TIMEOUT (seconds, default 30): a run that neither stops nor faults by
 * then is reported where it is (a guest spin shows its EIP and MMIO sites). */
static unsigned g_timeout;

static void timeout(int sig, siginfo_t *info, void *ctx)
{
    const mcontext_t *mc = &((ucontext_t *)ctx)->uc_mcontext;
    uint32_t eip = nc_cpu_guest_eip_at(g_core.cpu, (uintptr_t)mc->pc);
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "hakuX-native-frontier: timeout after %u s: host_pc=%llx guest_eip=%08x%s\n",
                     g_timeout, (unsigned long long)mc->pc, eip, eip ? "" : " (not in guest code)");
    write(2, buf, (size_t)n);
    report_guest(mc, eip);
    dump_ranges(getenv("NC_FAULT_DUMP"));
    _exit(5);
}

/* NC_DUMP="va,len;va,len": after the title loads, writes guest bytes to
 * dump_<va>.bin in the working directory (for disassembly on the host). */
static void dump_ranges(const char *spec)
{
    while (spec && *spec) {
        char *end;
        uint32_t va = (uint32_t)strtoul(spec, &end, 16);
        uint32_t len = *end == ',' ? (uint32_t)strtoul(end + 1, &end, 16) : 0;
        char name[64];
        snprintf(name, sizeof(name), "dump_%08x.bin", va);
        FILE *out = fopen(name, "wb");
        for (uint32_t i = 0; out && i < len; i++) {
            const uint8_t b = nc_vm_phys(&g_core.vm, va + i) == UINT32_MAX ? 0 : *(const uint8_t *)nc_vm_host(&g_core.vm, va + i);
            fputc(b, out);
        }
        if (out) {
            fclose(out);
            nc_log("hakuX-native", "dumped %08x+%x to %s", va, len, name);
        }
        /* The physical page behind each page of the range (u32 each; ~0: none). */
        snprintf(name, sizeof(name), "dump_%08x.pa", va);
        if ((out = fopen(name, "wb")) != NULL) {
            for (uint32_t p = va & ~(NC_PAGE - 1); p < va + len; p += NC_PAGE) {
                const uint32_t pa = nc_vm_phys(&g_core.vm, p);
                fwrite(&pa, sizeof(pa), 1, out);
            }
            fclose(out);
        }
        spec = *end == ';' ? end + 1 : "";
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: xboxrun <disc image>\n");
        return 2;
    }
    int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror(argv[1]);
        return 2;
    }

    struct sigaction sa = {0};
    sa.sa_sigaction = fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sa.sa_sigaction = timeout;
    sigaction(SIGALRM, &sa, NULL);
    g_timeout = getenv("NC_TIMEOUT") ? (unsigned)atoi(getenv("NC_TIMEOUT")) : 30;
    nc_log("hakuX-native", "xboxrun pid %d", (int)getpid()); /* the HLE layer logs to logcat under this pid */
    /* The console's video settings, as the app's video= and avpack= (NC_AVPACK:
     * the NC_AV_PACK_* number). */
    if (getenv("NC_VIDEO")) {
        nc_kernel_set_video((int64_t)strtoul(getenv("NC_VIDEO"), NULL, 16),
                            getenv("NC_AVPACK") ? atoi(getenv("NC_AVPACK")) : NC_AV_PACK_HDTV);
    }
    if (getenv("NC_LANGUAGE")) { /* as the app's lang= */
        nc_kernel_set_language(atoi(getenv("NC_LANGUAGE")));
    }

    if (getenv("NC_HDD_ROOT")) { /* off-device runs (qemu on a desktop): no /data/local/tmp */
        nc_io_set_hdd_root(getenv("NC_HDD_ROOT"));
    }

    if (!nc_core_boot(&g_core, fd)) {
        nc_log("hakuX-native-frontier", "boot failed");
        return 1;
    }
    dump_ranges(getenv("NC_DUMP"));
    if (getenv("NC_DUMP_ONLY")) {
        return 0;
    }
    nc_capture_start();
    alarm(g_timeout);
    nc_core_run(&g_core);
    return 0;
}
