/*
 * Xbox fastmem: guest virtual RAM mirrored into a host address window.
 *
 * A guest load normally costs an inline softmmu TLB probe of ~10 host
 * instructions.  With fastmem the aarch64 backend instead emits a single
 * "ldr data, [x26, addr, uxtw]" where x26 is the base of a 4 GiB host
 * window that mirrors the guest's virtual address space.
 *
 * The window starts out PROT_NONE.  The first access to a page faults;
 * the SIGSEGV handler looks the page up in the softmmu TLB and, when the
 * entry is plain RAM with no flags, maps the matching page of guest RAM
 * (which is memfd-backed for exactly this purpose) into the window and
 * retries.  A page is mapped writable only while its TLB write entry is
 * plain too, i.e. the page is dirty for every dirty-memory client; when
 * tlb_reset_dirty() re-arms tracking it is demoted back to read-only, so
 * the next store faults and takes the TLB_NOTDIRTY slow path exactly as
 * it would without fastmem.  Anything else -- a TLB miss, MMIO, a watched page
 * -- is sent to an out-of-line stub holding the ordinary TLB probe and
 * slow path.  A site that touches a page which can never be mapped is
 * patched to branch to that stub for good, so it costs what it did
 * before fastmem plus one branch.
 *
 * The window only ever holds pages the TLB itself held, and is cleared by
 * every TLB flush of the data MMU index (full, page, range), so it never
 * holds a translation the guest could not also have hit in a large TLB.
 * Mapping decisions are made under the TLB lock, which tlb_reset_dirty()
 * also holds when another thread re-arms dirty tracking.
 *
 * Kill switches: setprop debug.xemu.tcg.fastmem 0 (all of it), or
 * setprop debug.xemu.tcg.fastmem_st 0 (stores keep the TLB probe).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/bitmap.h"
#include "qemu/bitops.h"
#include "hw/core/cpu.h"
#include "system/memory.h"
#include "tcg/tcg.h"
#include "tcg/xbox-fastmem.h"

#if XBOX_FASTMEM_SUPPORTED

#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

/* 4 GiB of guest virtual space plus a guard page for the last access. */
#define WINDOW_SIZE ((uint64_t)1 << 32)
#define WINDOW_RESERVE (WINDOW_SIZE + 0x10000)
#define PAGE_SZ 4096u

/*
 * Each mapped page is its own VMA unless the kernel merges neighbours.
 * Stay well clear of vm.max_map_count (65530), which ART also draws on.
 */
#define MAX_MAPPED_PAGES 16384

/* Site table: code-buffer offsets of window loads -> their stubs. */
#define SITE_BITS 21
#define SITE_COUNT (1u << SITE_BITS)

bool xbox_fastmem_codegen;
bool xbox_fastmem_stores;

static uintptr_t window_base;
static int ram_fd = -1;
static uint8_t *ram_host;
static uint64_t ram_size;
static unsigned mapped_pages;

/*
 * Window pages currently mapped writable, per guest RAM page: up to two
 * guest virtual pages (stored +1, 0 = free).  A page with no free slot is
 * mapped read-only instead, so this list is always complete.
 */
static uint32_t (*rw_alias)[2];

/*
 * Per guest RAM page: how often its writable mapping was demoted.  Pages
 * whose dirty tracking is re-armed constantly (nv2a re-arms every frame for
 * buffers the CPU rewrites) would cost two faults and two syscalls per
 * re-arm through the window, against one helper call on the TLB path, so
 * once a page crosses DEMOTE_LIMIT its store sites go back to the TLB path.
 */
#define DEMOTE_LIMIT 4
static uint8_t *demote_count;

/*
 * MAP_ARMED redirects per site, by site hash (collisions only patch a site
 * sooner, which is always safe).  Stores to a page that also holds
 * translated code keep hitting TLB_NOTDIRTY for as long as the code lives,
 * so without this such a site would take a signal on every store; after
 * REDIRECT_LIMIT it is patched to its stub.
 */
#define REDIRECT_LIMIT 32
static uint8_t redirect_count[1u << 16];

/* One bit per guest virtual page: currently mapped in the window. */
static unsigned long *mapped_bits;
static uint64_t *sites;
static unsigned site_used;
static struct sigaction prev_segv;

/* Stats, read by the periodic log line. */
static unsigned long stat_maps, stat_rw_maps, stat_demotes, stat_redirects,
    stat_patches, stat_resets;

int xbox_fastmem_tlb_probe(CPUState *cpu, int mmu_idx, uint64_t page,
                           bool is_write, uintptr_t *host);
void xbox_fastmem_tlb_lock(CPUState *cpu);
void xbox_fastmem_tlb_unlock(CPUState *cpu);

static bool prop_is_off(const char *prop, const char *env_name)
{
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get(prop, value) > 0 && value[0] == '0';
#else
    const char *env = getenv(env_name);
    return env && env[0] == '0';
#endif
}

static bool fastmem_disabled_by_prop(void)
{
    return prop_is_off("debug.xemu.tcg.fastmem", "XEMU_TCG_FASTMEM");
}

static void fastmem_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void fastmem_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "xemu-fastmem", fmt, ap);
#else
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
    va_end(ap);
}

uintptr_t xbox_fastmem_window_base(void)
{
    void *p;

    if (window_base || fastmem_disabled_by_prop()) {
        return window_base;
    }
    p = mmap(NULL, WINDOW_RESERVE, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        fastmem_log("window reservation failed: %s", strerror(errno));
        return 0;
    }
    window_base = (uintptr_t)p;
    return window_base;
}

/* Drop every mapping in the window with one syscall. */
static void window_clear(void)
{
    if (!mapped_pages) {
        return;
    }
    mmap((void *)window_base, WINDOW_SIZE, PROT_NONE,
         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    mapped_pages = 0;
    memset(rw_alias, 0, (ram_size / PAGE_SZ) * sizeof(*rw_alias));
    bitmap_zero(mapped_bits, WINDOW_SIZE / PAGE_SZ);
    stat_resets++;
}

void xbox_fastmem_unmap_all(void)
{
    if (window_base) {
        window_clear();
    }
}

void xbox_fastmem_unmap_range(uint64_t addr, uint64_t len)
{
    uint64_t start, end;

    if (!window_base || !mapped_pages) {
        return;
    }
    start = addr & ~(uint64_t)(PAGE_SZ - 1);
    end = addr + len;
    if (end > WINDOW_SIZE || end < addr) {
        end = WINDOW_SIZE;
    }
    if (start >= end) {
        return;
    }
    if (end - start > 64 * PAGE_SZ) {
        window_clear();
        return;
    }
    /*
     * rw_alias may keep naming these pages; demoting a page that is no
     * longer writable later only costs a spurious fault, never a miss.
     */
    mmap((void *)(window_base + start), end - start, PROT_NONE,
         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    for (uint64_t pg = start / PAGE_SZ; pg < end / PAGE_SZ; pg++) {
        if (test_and_clear_bit(pg, mapped_bits)) {
            mapped_pages--;
        }
    }
}

void xbox_fastmem_reset_dirty(uintptr_t host, uintptr_t len)
{
    uint64_t first, last;

    if (!rw_alias || !len || host + len <= (uintptr_t)ram_host ||
        host >= (uintptr_t)ram_host + ram_size) {
        return;
    }
    first = host > (uintptr_t)ram_host ? (host - (uintptr_t)ram_host) / PAGE_SZ
                                       : 0;
    last = MIN((host + len - 1 - (uintptr_t)ram_host) / PAGE_SZ,
               ram_size / PAGE_SZ - 1);
    for (uint64_t pg = first; pg <= last; pg++) {
        for (int k = 0; k < 2; k++) {
            uint32_t va = rw_alias[pg][k];
            if (va) {
                mprotect((void *)(window_base + (uint64_t)(va - 1) * PAGE_SZ),
                         PAGE_SZ, PROT_READ);
                rw_alias[pg][k] = 0;
                if (demote_count[pg] < UINT8_MAX) {
                    demote_count[pg]++;
                }
                stat_demotes++;
            }
        }
    }
}

/* --- site table ------------------------------------------------------ */

uintptr_t tcg_code_gen_buffer_rx_base(size_t *size);

static uintptr_t code_rx_base;
static size_t code_size;

static inline uint32_t site_key(const void *rx)
{
    return (uint32_t)(((uintptr_t)rx - code_rx_base) >> 2);
}

static inline uint32_t site_hash(uint32_t key)
{
    return (key * 0x9E3779B1u) >> (32 - SITE_BITS);
}

bool xbox_fastmem_can_record(void)
{
    return sites && site_used < SITE_COUNT / 2;
}

void xbox_fastmem_record(const void *load_rx, const void *stub_rx)
{
    uint32_t key = site_key(load_rx);
    uint32_t val = site_key(stub_rx);
    uint64_t ent = ((uint64_t)(key + 1) << 32) | val;

    for (uint32_t i = site_hash(key);; i = (i + 1) & (SITE_COUNT - 1)) {
        uint64_t cur = qatomic_read(&sites[i]);
        if (cur == 0) {
            site_used++;
        } else if ((uint32_t)(cur >> 32) != key + 1) {
            continue;
        }
        qatomic_set(&sites[i], ent);
        return;
    }
}

static const void *site_lookup(uintptr_t pc)
{
    uintptr_t buf = code_rx_base;
    uint32_t key;

    if (pc < buf || pc - buf >= code_size) {
        return NULL;
    }
    key = (uint32_t)((pc - buf) >> 2);
    for (uint32_t i = site_hash(key);; i = (i + 1) & (SITE_COUNT - 1)) {
        uint64_t cur = qatomic_read(&sites[i]);
        if (cur == 0) {
            return NULL;
        }
        if ((uint32_t)(cur >> 32) == key + 1) {
            return (const void *)(buf + ((uintptr_t)(uint32_t)cur << 2));
        }
    }
}

void xbox_fastmem_code_flushed(void)
{
    if (sites) {
        /* Anonymous memory: dropping the pages zeroes them cheaply. */
        madvise(sites, SITE_COUNT * sizeof(uint64_t), MADV_DONTNEED);
        site_used = 0;
        memset(redirect_count, 0, sizeof(redirect_count));
    }
}

/* --- fault handling -------------------------------------------------- */

/*
 * Make one guest page accessible in the window for an access of the given
 * kind.  Called with the TLB lock held.
 */
enum {
    MAP_OK = 1,         /* mapped: retry the access */
    MAP_MISS = 0,       /* TLB miss: the slow path fills it, retry later */
    MAP_NEVER = -1,     /* MMIO, ROM, watched, outside RAM: patch the site */
    MAP_ARMED = -2,     /* store to a page with dirty tracking armed */
};

static int map_page(CPUState *cpu, uint32_t va_page, bool is_write)
{
    uintptr_t host, whost;
    int r = xbox_fastmem_tlb_probe(cpu, XBOX_FASTMEM_MMU_IDX, va_page,
                                   false, &host);
    int w;
    uint64_t pg;
    uint32_t *slot = NULL;
    void *p;

    if (r <= 0) {
        return r == 0 ? MAP_MISS : MAP_NEVER;
    }
    if (host < (uintptr_t)ram_host || host - (uintptr_t)ram_host >= ram_size) {
        return MAP_NEVER;
    }
    pg = (host - (uintptr_t)ram_host) / PAGE_SZ;

    w = xbox_fastmem_tlb_probe(cpu, XBOX_FASTMEM_MMU_IDX, va_page,
                               true, &whost);
    if (w > 0 && whost == host && demote_count[pg] < DEMOTE_LIMIT) {
        for (int k = 0; k < 2; k++) {
            if (rw_alias[pg][k] == va_page / PAGE_SZ + 1) {
                slot = &rw_alias[pg][k];
                break;
            }
            if (!slot && !rw_alias[pg][k]) {
                slot = &rw_alias[pg][k];
            }
        }
    }
    if (is_write && !slot) {
        /*
         * Not writable now.  Armed dirty tracking (-1), a miss, or no free
         * alias slot pass; a permanently slow page (-2), or one whose
         * tracking keeps being re-armed, sends the site back for good.
         */
        if (w == -2 || demote_count[pg] >= DEMOTE_LIMIT) {
            return MAP_NEVER;
        }
        return w == 0 ? MAP_MISS : MAP_ARMED;
    }

    if (mapped_pages >= MAX_MAPPED_PAGES &&
        !test_bit(va_page / PAGE_SZ, mapped_bits)) {
        window_clear();
        slot = NULL;
        if (w > 0 && whost == host && demote_count[pg] < DEMOTE_LIMIT) {
            slot = &rw_alias[pg][0];
        }
        if (is_write && !slot) {
            return MAP_ARMED;
        }
    }
    p = mmap((void *)(window_base + va_page), PAGE_SZ,
             slot ? PROT_READ | PROT_WRITE : PROT_READ,
             MAP_SHARED | MAP_FIXED, ram_fd, host - (uintptr_t)ram_host);
    if (p == MAP_FAILED) {
        return MAP_NEVER;
    }
    if (slot) {
        *slot = va_page / PAGE_SZ + 1;
        stat_rw_maps++;
    }
    if (!test_and_set_bit(va_page / PAGE_SZ, mapped_bits)) {
        mapped_pages++;
    }
    stat_maps++;
    return MAP_OK;
}

static void patch_branch(uintptr_t pc_rx, uintptr_t dest_rx)
{
    uint32_t *rw = tcg_splitwx_to_rw((const void *)pc_rx);
    int64_t disp = ((int64_t)dest_rx - (int64_t)pc_rx) >> 2;

    qatomic_set(rw, 0x14000000u | (uint32_t)(disp & 0x3ffffff));
    __builtin___clear_cache((char *)pc_rx, (char *)pc_rx + 4);
    if ((uintptr_t)rw != pc_rx) {
        __builtin___clear_cache((char *)rw, (char *)rw + 4);
    }
}

static bool handle_fault(ucontext_t *uc, uintptr_t fault)
{
    mcontext_t *mc = &uc->uc_mcontext;
    uintptr_t pc = mc->pc;
    const void *stub;
    uint32_t insn, addr, size, first, last;
    CPUState *cpu = current_cpu;
    bool is_write;
    int r;

    if (fault - window_base >= WINDOW_RESERVE || !cpu) {
        return false;
    }
    stub = site_lookup(pc);
    if (!stub) {
        return false;
    }

    /*
     * LDR or STR (register, extended): size in [31:30], opc in [23:22]
     * (00 = store), Rm in [20:16].
     */
    insn = *(const uint32_t *)pc;
    addr = (uint32_t)mc->regs[(insn >> 16) & 31];
    size = 1u << (insn >> 30);
    is_write = ((insn >> 22) & 3) == 0;
    first = addr & ~(PAGE_SZ - 1);
    last = (addr + size - 1) & ~(PAGE_SZ - 1);

    xbox_fastmem_tlb_lock(cpu);
    r = map_page(cpu, first, is_write);
    if (r == MAP_OK && last != first) {
        r = map_page(cpu, last, is_write);
    }
    xbox_fastmem_tlb_unlock(cpu);
    if (r == MAP_OK) {
        return true;                   /* retry the access */
    }
    if (r == MAP_NEVER ||
        (r == MAP_ARMED &&
         ++redirect_count[site_hash(site_key((const void *)pc)) >>
                          (SITE_BITS - 16)] >= REDIRECT_LIMIT)) {
        patch_branch(pc, (uintptr_t)stub);
        stat_patches++;
    }
    stat_redirects++;
    mc->pc = (uintptr_t)stub;
    return true;
}

static void fastmem_sigsegv(int sig, siginfo_t *si, void *ctx)
{
    struct sigaction *prev = &prev_segv;

    if (handle_fault(ctx, (uintptr_t)si->si_addr)) {
        return;
    }
    if (prev->sa_flags & SA_SIGINFO) {
        prev->sa_sigaction(sig, si, ctx);
    } else if (prev->sa_handler == SIG_DFL || prev->sa_handler == SIG_IGN) {
        /* Re-raise with the default action on return. */
        signal(sig, SIG_DFL);
    } else {
        prev->sa_handler(sig);
    }
}

static void fastmem_stats(void)
{
    static int64_t last;
    int64_t now = g_get_monotonic_time();

    if (now - last < 5 * G_USEC_PER_SEC) {
        return;
    }
    last = now;
    fastmem_log("maps=%lu rw=%lu demotes=%lu redirects=%lu patches=%lu "
                "resets=%lu mapped=%u sites=%u",
                stat_maps, stat_rw_maps, stat_demotes, stat_redirects,
                stat_patches, stat_resets, mapped_pages, site_used);
}

void xbox_fastmem_set_ram(MemoryRegion *mr, int fd)
{
    struct sigaction sa;

    if (!xbox_fastmem_window_base()) {
        return;
    }
    sites = mmap(NULL, SITE_COUNT * sizeof(uint64_t), PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (sites == MAP_FAILED) {
        sites = NULL;
        return;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fastmem_sigsegv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &prev_segv)) {
        fastmem_log("sigaction failed: %s", strerror(errno));
        return;
    }

    code_rx_base = tcg_code_gen_buffer_rx_base(&code_size);
    if (code_size >> 2 >= UINT32_MAX) {
        return;
    }
    ram_fd = fd;
    ram_host = memory_region_get_ram_ptr(mr);
    ram_size = memory_region_size(mr);
    rw_alias = g_malloc0((ram_size / PAGE_SZ) * sizeof(*rw_alias));
    mapped_bits = bitmap_new(WINDOW_SIZE / PAGE_SZ);
    demote_count = g_malloc0(ram_size / PAGE_SZ);
    xbox_fastmem_stores = !prop_is_off("debug.xemu.tcg.fastmem_st",
                                       "XEMU_TCG_FASTMEM_ST");
    xbox_fastmem_codegen = true;
    fastmem_log("enabled: window %p, ram %p (+%" PRIu64 " MiB), stores %s",
                (void *)window_base, ram_host, ram_size >> 20,
                xbox_fastmem_stores ? "on" : "off");
}

void xbox_fastmem_tick(void)
{
    if (xbox_fastmem_codegen) {
        fastmem_stats();
    }
}

#endif
