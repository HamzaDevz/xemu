/*
 * Native replacement of hot Bink 1 video routines.
 *
 * Many Xbox titles statically link RAD's Bink 1 decoder, and on a phone its
 * IDCT and YUV->RGB loops dominate FMV playback (Halo's attract videos spend
 * ~36% of their translated guest time in the three routines below). When the
 * translator reaches the entry of one of them, it emits a single helper call
 * that performs the whole routine natively and returns to the caller.
 *
 * A routine is recognised by its exact code bytes, with absolute address
 * operands masked out. Those operands are checked for consistency within
 * their group (globals, constants, tables) and passed to the helper, so the
 * same Bink build linked at other addresses also matches.
 *
 * The native code reproduces the guest bit for bit, including the registers,
 * flags and MMX state the routine leaves behind; bink-hle-core.h was
 * differential-tested against the original x86 code. Turn it off with
 * XEMU_TCG_BINK_HLE=0, or on Android `setprop debug.xemu.tcg.bink_hle 0`.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "accel/tcg/probe.h"
#include "exec/helper-proto.h"
#include "exec/target_page.h"
#include "exec/translator.h"
#include "helper-tcg.h"
#include "bink-hle.h"
#include "bink-hle-core.h"
#include "tcg/xbox-fastmem.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

enum {
    BINK_IDCT_PUT,
    BINK_IDCT_ADD,
    BINK_YUV32,
};

typedef struct BinkAbsOperand {
    uint16_t off;   /* byte offset of the imm32/disp32 in the routine */
    uint8_t group;  /* index into bases[] */
    uint32_t value; /* value in the reference build */
} BinkAbsOperand;

typedef struct BinkRoutine {
    int id;
    uint16_t len;
    uint8_t prefix[8];
    uint64_t hash;  /* FNV-1a of the bytes, absolute operands zeroed */
    uint32_t ref_base[3];
    const BinkAbsOperand *abs;
    int n_abs;
} BinkRoutine;

/* Halo: Combat Evolved (USA), BINK section, build with "RAD Game Tools" */
static const BinkAbsOperand halo_idct_add_abs[] = {
    { 0x01d, 0, 0x1d9878 },
};

static const BinkAbsOperand halo_yuv32_abs[] = {
    { 0x006, 0, 0x26e578 }, { 0x00c, 0, 0x26e580 }, { 0x012, 0, 0x26e588 },
    { 0x018, 0, 0x26e58c }, { 0x026, 0, 0x26e578 }, { 0x02d, 1, 0x1f2588 },
    { 0x046, 2, 0x26fdc0 }, { 0x04e, 2, 0x26f5c0 }, { 0x05b, 1, 0x1f2578 },
    { 0x063, 2, 0x26fdc0 }, { 0x06b, 2, 0x26f5c0 }, { 0x078, 1, 0x1f2580 },
    { 0x080, 2, 0x26f9c0 }, { 0x088, 2, 0x2701c0 }, { 0x099, 2, 0x26f9c0 },
    { 0x0a1, 2, 0x2701c0 }, { 0x104, 0, 0x26e578 }, { 0x116, 0, 0x26e580 },
    { 0x11c, 0, 0x26e57c }, { 0x122, 0, 0x26e584 }, { 0x128, 0, 0x26e588 },
    { 0x12e, 0, 0x26e58c }, { 0x13c, 0, 0x26e57c }, { 0x143, 1, 0x1f2588 },
    { 0x15c, 2, 0x26fdc0 }, { 0x164, 2, 0x26f5c0 }, { 0x171, 1, 0x1f2578 },
    { 0x179, 2, 0x26fdc0 }, { 0x181, 2, 0x26f5c0 }, { 0x18e, 1, 0x1f2580 },
    { 0x196, 2, 0x26f9c0 }, { 0x19e, 2, 0x2701c0 }, { 0x1af, 2, 0x26f9c0 },
    { 0x1b7, 2, 0x2701c0 }, { 0x21a, 0, 0x26e57c }, { 0x22c, 0, 0x26e584 },
    { 0x232, 0, 0x26e588 }, { 0x238, 0, 0x26e58c },
};

static const BinkRoutine bink_routines[] = {
    {
        .id = BINK_IDCT_PUT, .len = 1707,
        .prefix = { 0x81, 0xec, 0x2c, 0x01, 0x00, 0x00, 0x53, 0x55 },
        .hash = 0x8d479d50c56b0742ull,
    },
    {
        .id = BINK_IDCT_ADD, .len = 1956,
        .prefix = { 0x81, 0xec, 0x2c, 0x01, 0x00, 0x00, 0x8b, 0x94 },
        .hash = 0x3617d907600395f9ull,
        .ref_base = { 0x1d9878 },
        .abs = halo_idct_add_abs, .n_abs = ARRAY_SIZE(halo_idct_add_abs),
    },
    {
        .id = BINK_YUV32, .len = 579,
        .prefix = { 0x53, 0x55, 0x56, 0x57, 0x8b, 0x3d, 0x00, 0x00 },
        .hash = 0xe70840bdb6d2c393ull,
        /* globals, MMX constants, lookup tables */
        .ref_base = { 0x26e578, 0x1f2578, 0x26f5c0 },
        .abs = halo_yuv32_abs, .n_abs = ARRAY_SIZE(halo_yuv32_abs),
    },
};

#define BINK_MAX_LEN 2048

bool bink_hle_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        enabled = 1;
#ifdef __ANDROID__
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.xemu.tcg.bink_hle", value) > 0 &&
            value[0] == '0') {
            enabled = 0;
        }
#else
        const char *env = getenv("XEMU_TCG_BINK_HLE");
        if (env && env[0] == '0') {
            enabled = 0;
        }
#endif
    }
    return enabled;
}

static uint64_t bink_fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;

    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

int bink_hle_match(CPUX86State *env, DisasContextBase *db, vaddr pc,
                   uint32_t bases[3])
{
    uint8_t head[8];
    const BinkRoutine *r = NULL;

    /*
     * Cheap prefilter on bytes from the TB's own page only: reading past it
     * could fault on an unmapped page during translation.
     */
    if (-(pc | TARGET_PAGE_MASK) < sizeof(head)) {
        return BINK_HLE_NONE;
    }
    for (int i = 0; i < sizeof(head); i++) {
        head[i] = translator_ldub(env, db, pc + i);
    }
    for (int i = 0; i < ARRAY_SIZE(bink_routines); i++) {
        const BinkRoutine *c = &bink_routines[i];
        int n = c->id == BINK_YUV32 ? 6 : 8; /* yuv32 has an abs at 6 */
        if (!memcmp(head, c->prefix, n)) {
            r = c;
            break;
        }
    }
    if (!r) {
        return BINK_HLE_NONE;
    }

    /* The whole routine must lie within the TB's first two pages. */
    if ((pc & TARGET_PAGE_MASK) + 2 * TARGET_PAGE_SIZE < pc + r->len) {
        return BINK_HLE_NONE;
    }

    uint8_t code[BINK_MAX_LEN];
    for (int i = 0; i < r->len; i++) {
        code[i] = translator_ldub(env, db, pc + i);
    }

    bool have[3] = { false, false, false };
    for (int i = 0; i < r->n_abs; i++) {
        const BinkAbsOperand *a = &r->abs[i];
        uint32_t v = ldl_le_p(code + a->off);
        uint32_t base = v - (a->value - r->ref_base[a->group]);
        if (!have[a->group]) {
            bases[a->group] = base;
            have[a->group] = true;
        } else if (bases[a->group] != base) {
            return BINK_HLE_NONE;
        }
        memset(code + a->off, 0, 4);
    }
    if (bink_fnv1a(code, r->len) != r->hash) {
        return BINK_HLE_NONE;
    }

    static bool logged[ARRAY_SIZE(bink_routines)];
    if (!qatomic_xchg(&logged[r->id], true)) {
        static const char *const names[] = { "idct_put", "idct_add", "yuv32" };
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "xemu-bink",
                            "native %s at 0x%08x", names[r->id], (uint32_t)pc);
#else
        qemu_log_mask(LOG_UNIMP, "bink-hle: native %s at 0x%08x\n",
                      names[r->id], (uint32_t)pc);
#endif
    }
    return r->id;
}

/* ---- guest memory access ---- */

typedef struct BinkMem {
    CPUX86State *env;
    int mmu_idx;
    uintptr_t ra;
    bool cacheable;
} BinkMem;

/*
 * Host pointers of plain guest RAM pages, kept across calls on this vCPU
 * thread. An entry is only made when the TLB entry has no flags at all (no
 * MMIO, watchpoint or pending dirty tracking), so a cached access behaves
 * exactly like a TCG fast-path access. xbox_host_map_gen drops the cache
 * whenever that may have stopped being true.
 */
#define BINK_PAGE_CACHE 64

typedef struct BinkPageCache {
    uint32_t gen;
    uint32_t tag[2][BINK_PAGE_CACHE]; /* page | 1, 0 = empty */
    uint8_t *host[2][BINK_PAGE_CACHE];
} BinkPageCache;

static __thread BinkPageCache bink_pages;

static void bink_cache_check(void)
{
    uint32_t gen = qatomic_read(&xbox_host_map_gen);

    if (unlikely(gen != bink_pages.gen)) {
        memset(bink_pages.tag, 0, sizeof(bink_pages.tag));
        bink_pages.gen = gen;
    }
}

/*
 * Host address for [addr, addr+len) within one page, raising any fault the
 * access would. Returns NULL for MMIO, where the caller uses byte accessors.
 */
static uint8_t *bink_host(BinkMem *m, uint32_t addr, uint32_t len,
                          MMUAccessType type)
{
    int w = type == MMU_DATA_STORE;
    uint32_t page = addr & TARGET_PAGE_MASK;
    uint32_t slot = (addr >> TARGET_PAGE_BITS) % BINK_PAGE_CACHE;
    uint8_t *host;

    if (m->cacheable) {
        if (w) {
            /* another thread may have re-armed dirty tracking */
            bink_cache_check();
        }
        if (bink_pages.tag[w][slot] == (page | 1)) {
            return bink_pages.host[w][slot] + (addr - page);
        }
    }
    host = probe_access(m->env, addr, len, type, m->mmu_idx, m->ra);
    if (host && m->cacheable &&
        tlb_vaddr_to_host(m->env, addr, type, m->mmu_idx) == host) {
        bink_pages.tag[w][slot] = page | 1;
        bink_pages.host[w][slot] = host - (addr - page);
    }
    return host;
}

static void bink_read(BinkMem *m, uint32_t addr, void *dest, uint32_t len)
{
    uint8_t *d = dest;

    while (len) {
        uint32_t chunk = MIN(len, -(addr | TARGET_PAGE_MASK));
        uint8_t *host = bink_host(m, addr, chunk, MMU_DATA_LOAD);
        if (host) {
            memcpy(d, host, chunk);
        } else {
            for (uint32_t i = 0; i < chunk; i++) {
                d[i] = cpu_ldub_mmuidx_ra(m->env, addr + i, m->mmu_idx, m->ra);
            }
        }
        addr += chunk;
        d += chunk;
        len -= chunk;
    }
}

static uint32_t bink_read32(BinkMem *m, uint32_t addr)
{
    uint8_t b[4];

    bink_read(m, addr, b, 4);
    return ldl_le_p(b);
}

/* Raise any fault a store to [addr, addr+len) would, before writing. */
static void bink_probe_write(BinkMem *m, uint32_t addr, uint32_t len)
{
    while (len) {
        uint32_t chunk = MIN(len, -(addr | TARGET_PAGE_MASK));
        bink_host(m, addr, chunk, MMU_DATA_STORE);
        addr += chunk;
        len -= chunk;
    }
}

static void bink_write(BinkMem *m, uint32_t addr, const void *src,
                       uint32_t len)
{
    const uint8_t *s = src;

    while (len) {
        uint32_t chunk = MIN(len, -(addr | TARGET_PAGE_MASK));
        uint8_t *host = bink_host(m, addr, chunk, MMU_DATA_STORE);
        if (host) {
            memcpy(host, s, chunk);
        } else {
            for (uint32_t i = 0; i < chunk; i++) {
                cpu_stb_mmuidx_ra(m->env, addr + i, s[i], m->mmu_idx, m->ra);
            }
        }
        addr += chunk;
        s += chunk;
        len -= chunk;
    }
}

static void bink_write32(BinkMem *m, uint32_t addr, uint32_t v)
{
    uint8_t b[4];

    stl_le_p(b, v);
    bink_write(m, addr, b, 4);
}

/*
 * Leave flags as the routine's last flag-setting instruction did.
 * Both IDCTs end with `add esp, 0x12c` restoring esp to its entry value.
 */
static void bink_set_add_flags(CPUX86State *env, uint32_t res, uint32_t src)
{
    env->cc_dst = res;
    env->cc_src = src;
    env->cc_op = CC_OP_ADDL;
}

/* Return to the caller: pop the return address and @args bytes. */
static void bink_ret(CPUX86State *env, BinkMem *m, uint32_t esp,
                     uint32_t args)
{
    env->eip = bink_read32(m, esp);
    env->regs[R_ESP] = esp + 4 + args;
}

static void bink_idct(CPUX86State *env, BinkMem *m, bool add,
                      uint32_t qbase)
{
    uint32_t esp = env->regs[R_ESP];
    uint32_t dst, stride, coef_addr, quant_addr, pred_addr = 0;
    uint8_t coef[128], quant[256], pred[64];
    uint8_t out[8][8];
    uint8_t *rows[8];

    if (!add) {
        /* cdecl (dst, stride); eax = coefficients, edx = quant table */
        dst = bink_read32(m, esp + 4);
        stride = bink_read32(m, esp + 8);
        coef_addr = env->regs[R_EAX];
        quant_addr = env->regs[R_EDX];
    } else {
        /* stdcall (dst, stride, coefficients, quant index, prediction) */
        dst = bink_read32(m, esp + 4);
        stride = bink_read32(m, esp + 8);
        coef_addr = bink_read32(m, esp + 12);
        quant_addr = (bink_read32(m, esp + 16) << 8) + qbase;
        pred_addr = bink_read32(m, esp + 20);
        bink_read(m, pred_addr, pred, sizeof(pred));
    }
    bink_read(m, coef_addr, coef, sizeof(coef));
    bink_read(m, quant_addr, quant, sizeof(quant));
    for (int r = 0; r < 8; r++) {
        bink_probe_write(m, dst + r * stride, 8);
        rows[r] = out[r];
    }

#ifdef BINK_HLE_VECTOR
    if (add) {
        bink_idct_add_v(rows, coef, quant, pred);
    } else {
        bink_idct_put_v(rows, coef, quant);
    }
#else
    if (add) {
        bink_idct_add(rows, coef, quant, pred);
    } else {
        bink_idct_put(rows, coef, quant);
    }
#endif
    for (int r = 0; r < 8; r++) {
        bink_write(m, dst + r * stride, out[r], 8);
    }

    env->regs[R_EAX] = dst + 8 * stride;
    env->regs[R_EDX] = 0;
    if (add) {
        env->regs[R_ECX] = pred_addr + 1 + 0x40;
    } else {
        env->regs[R_ECX] = esp + 0x18;
    }
    bink_set_add_flags(env, esp, 0x12c);
    bink_ret(env, m, esp, add ? 0x14 : 0);
}

#define BINK_YUV_CHUNK 512 /* 4-pixel groups converted per batch */

/*
 * Iterations of the guest's `do { ...; d += 16; } while (d < end)`,
 * computed with its unsigned 32-bit arithmetic: n == 0 still runs once.
 */
static uint32_t bink_yuv_count(uint32_t d, uint32_t end)
{
    uint32_t count = 1;

    for (d += 16; d < end; d += 16) {
        count++;
    }
    return count;
}

static void bink_yuv32(CPUX86State *env, BinkMem *m, const uint32_t bases[3])
{
    static __thread uint8_t tables[0x1000];
    uint32_t esp = env->regs[R_ESP];
    uint32_t g = bases[0];
    uint32_t n = bink_read32(m, esp + 4);
    uint8_t ptrs[24], kbuf[24];
    BinkYuvConsts k;
    BinkYuvTables t = { tables, tables + 0x400, tables + 0x800,
                        tables + 0xc00 };
    uint64_t mm[8];
    uint32_t dst[2], ysrc[2], end[2], count[2];

    bink_read(m, g, ptrs, sizeof(ptrs));
    bink_read(m, bases[1], kbuf, sizeof(kbuf));
    bink_read(m, bases[2], tables, sizeof(tables));
    memcpy(&k.ysub, kbuf, 8);
    memcpy(&k.ymul, kbuf + 8, 8);
    memcpy(&k.clamp, kbuf + 16, 8);

    for (int row = 0; row < 2; row++) {
        dst[row] = ldl_le_p(ptrs + 4 * row);
        ysrc[row] = ldl_le_p(ptrs + 8 + 4 * row);
        end[row] = dst[row] + (n << 4);
        count[row] = bink_yuv_count(dst[row], end[row]);
        bink_probe_write(m, dst[row], count[row] * 16);
    }

    uint32_t u = 0, v = 0, y = 0, d = 0;
    for (int row = 0; row < 2; row++) {
        /* both rows convert from the same chroma row */
        d = dst[row];
        y = ysrc[row];
        u = ldl_le_p(ptrs + 16);
        v = ldl_le_p(ptrs + 20);
        bink_write32(m, g + 4 * row, end[row]);
        for (uint32_t done = 0; done < count[row]; done += BINK_YUV_CHUNK) {
            uint32_t c = MIN(BINK_YUV_CHUNK, count[row] - done);
            uint8_t yb[BINK_YUV_CHUNK * 4], ub[BINK_YUV_CHUNK * 2];
            uint8_t vb[BINK_YUV_CHUNK * 2], ob[BINK_YUV_CHUNK * 16];

            bink_read(m, y, yb, c * 4);
            bink_read(m, u, ub, c * 2);
            bink_read(m, v, vb, c * 2);
#ifdef BINK_HLE_VECTOR
            bink_yuv_run(yb, ub, vb, ob, c, &k, &t, mm);
#else
            for (uint32_t i = 0; i < c; i++) {
                bink_yuv_step(yb + 4 * i, ub + 2 * i, vb + 2 * i,
                              ob + 16 * i, &k, &t, mm);
            }
#endif
            bink_write(m, d, ob, c * 16);
            d += c * 16;
            y += c * 4;
            u += c * 2;
            v += c * 2;
        }
        bink_write32(m, g + 8 + 4 * row, y);
    }
    bink_write32(m, g + 16, u);
    bink_write32(m, g + 20, v);

    env->regs[R_EAX] = end[1];
    env->regs[R_EDX] = y;
    /* the last flag-setting instruction is `cmp edi, eax` */
    env->cc_dst = d - end[1];
    env->cc_src = end[1];
    env->cc_op = CC_OP_SUBL;

    for (int i = 0; i < 8; i++) {
        env->fpregs[i].mmx.MMX_Q(0) = mm[i];
    }
    env->fpstt = 0;
    memset(env->fptags, 0, sizeof(env->fptags));

    bink_ret(env, m, esp, 4);
}

void helper_bink_hle(CPUX86State *env, uint32_t id, uint32_t base0,
                     uint32_t base1, uint32_t base2)
{
    BinkMem m = {
        .env = env,
        .mmu_idx = cpu_mmu_index(env_cpu(env), false),
        .ra = GETPC(),
    };
    m.cacheable = m.mmu_idx == XBOX_FASTMEM_MMU_IDX;
    if (m.cacheable) {
        bink_cache_check();
    }
    uint32_t bases[3] = { base0, base1, base2 };

    switch (id) {
    case BINK_IDCT_PUT:
        bink_idct(env, &m, false, 0);
        break;
    case BINK_IDCT_ADD:
        bink_idct(env, &m, true, base0);
        break;
    case BINK_YUV32:
        bink_yuv32(env, &m, bases);
        break;
    default:
        g_assert_not_reached();
    }
}
