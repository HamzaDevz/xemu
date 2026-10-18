/*
 * Xbox fastmem: guest virtual RAM mirrored into a host address window.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TCG_XBOX_FASTMEM_H
#define TCG_XBOX_FASTMEM_H

/*
 * The MMU index every Xbox data access uses: i386 MMU_KNOSMAP32_IDX
 * (ring 0, 32-bit, no SMAP).  Only loads in this index take the fast path.
 */
#define XBOX_FASTMEM_MMU_IDX 5

/* See cputlb.c: changes whenever cached host pointers may be stale. */
extern uint32_t xbox_host_map_gen;

#if defined(XBOX) && defined(__aarch64__) && defined(CONFIG_LINUX)
#define XBOX_FASTMEM_SUPPORTED 1

struct MemoryRegion;
struct CPUState;

/*
 * Host base of the 4 GiB window, reserving it on first call.  Returns 0
 * when the window could not be reserved.  Called from the TCG prologue.
 */
uintptr_t xbox_fastmem_window_base(void);

/* Back guest RAM with @fd so pages can be mirrored; enables the fast path. */
void xbox_fastmem_set_ram(struct MemoryRegion *mr, int fd);

/* True while the backend may emit fastmem loads (and stores). */
extern bool xbox_fastmem_codegen;
extern bool xbox_fastmem_stores;

/*
 * Code generation: the backend emitted a window load at @load_rx whose
 * fallback stub (full TLB probe, then the slow-path helper) is at
 * @stub_rx.  Returns false if the site could not be recorded, in which
 * case the backend must not have emitted the window load -- so it calls
 * xbox_fastmem_can_record() first.
 */
bool xbox_fastmem_can_record(void);
void xbox_fastmem_record(const void *load_rx, const void *stub_rx);

/* The code buffer was reset: forget every recorded site. */
void xbox_fastmem_code_flushed(void);

/* TLB invalidation hooks, called with the TLB lock held on the vCPU. */
void xbox_fastmem_unmap_all(void);
void xbox_fastmem_unmap_range(uint64_t addr, uint64_t len);

/* Dirty tracking re-armed for host RAM [@host, @host + @len). */
void xbox_fastmem_reset_dirty(uintptr_t host, uintptr_t len);

/* Periodic stats line; cheap when not due. */
void xbox_fastmem_tick(void);

#else
#define XBOX_FASTMEM_SUPPORTED 0
static inline void xbox_fastmem_code_flushed(void) { }
static inline void xbox_fastmem_unmap_all(void) { }
static inline void xbox_fastmem_unmap_range(uint64_t addr, uint64_t len) { }
#endif

#endif
