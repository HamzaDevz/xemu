/*
 * Native equivalents of hot Bink 1 video routines statically linked into
 * Xbox titles. Pure functions over host pointers, with no QEMU dependencies,
 * so the same code is differential-tested against the original x86 bytes.
 *
 * Each routine reproduces the guest code bit for bit, including 32-bit
 * wraparound and the absence of output clamping in the IDCT.
 */

#ifndef BINK_HLE_CORE_H
#define BINK_HLE_CORE_H

#include <stdint.h>
#include <string.h>

static inline int32_t bink_rd16s(const uint8_t *p)
{
    int16_t v;
    memcpy(&v, p, 2);
    return v;
}

static inline uint32_t bink_rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* imul r32 then sar 11: 32-bit wrapping product, arithmetic shift. */
static inline int32_t bink_mul11(uint32_t x, uint32_t c)
{
    return (int32_t)(x * c) >> 11;
}

/* One 8-point Bink IDCT pass, in the guest's exact operation order. */
static inline void bink_idct_1d(const int32_t in[8], int32_t out[8])
{
    uint32_t a0 = in[0], a1 = in[1], a2 = in[2], a3 = in[3];
    uint32_t a4 = in[4], a5 = in[5], a6 = in[6], a7 = in[7];

    uint32_t A0 = a0 + a4, A1 = a0 - a4, A2 = a2 + a6;
    uint32_t A3 = (uint32_t)bink_mul11(a2 - a6, 2896) - A2;
    uint32_t e0 = A0 + A2, e3 = A0 - A2, e1 = A1 + A3, e2 = A1 - A3;

    uint32_t B0 = a3 + a5, B1 = a5 - a3, B2 = a1 + a7, B3 = a1 - a7;
    uint32_t C0 = B0 + B2, C1 = B2 - B0, C2 = B3 + B1;
    uint32_t D = bink_mul11(C2, 3784);
    uint32_t O1 = (uint32_t)bink_mul11(B1, (uint32_t)-5352) - C0 + D;
    uint32_t O2 = (uint32_t)bink_mul11(C1, 2896) - O1;
    uint32_t O3 = (uint32_t)bink_mul11(B3, 2217) - D + O2;

    out[0] = e0 + C0;
    out[7] = e0 - C0;
    out[1] = e1 + O1;
    out[6] = e1 - O1;
    out[2] = e2 + O2;
    out[5] = e2 - O2;
    out[4] = e3 + O3;
    out[3] = e3 - O3;
}

/*
 * Dequantise (int16 coefficient * int32 quant >> 11) and run the column
 * pass, then the row pass into tmp-shaped rows.
 * coef: 64 int16, quant: 64 int32, both little-endian guest layout.
 */
static inline void bink_idct_2d(const uint8_t *coef, const uint8_t *quant,
                                int32_t rows[64])
{
    int32_t tmp[64];

    for (int c = 0; c < 8; c++) {
        int32_t in[8], out[8];
        for (int k = 0; k < 8; k++) {
            in[k] = bink_mul11((uint32_t)bink_rd16s(coef + 2 * (8 * k + c)),
                               bink_rd32(quant + 4 * (8 * k + c)));
        }
        bink_idct_1d(in, out);
        for (int k = 0; k < 8; k++) {
            tmp[8 * k + c] = out[k];
        }
    }
    for (int r = 0; r < 8; r++) {
        bink_idct_1d(&tmp[8 * r], &rows[8 * r]);
    }
}

static inline uint8_t bink_px(int32_t v)
{
    return (uint8_t)((int32_t)((uint32_t)v + 0x7f) >> 8);
}

/* dst[r] points at row r of the 8x8 destination block. */
static inline void bink_idct_put(uint8_t *const dst[8], const uint8_t *coef,
                                 const uint8_t *quant)
{
    int32_t v[64];

    bink_idct_2d(coef, quant, v);
    for (int r = 0; r < 8; r++) {
        for (int i = 0; i < 8; i++) {
            dst[r][i] = bink_px(v[8 * r + i]);
        }
    }
}

/*
 * pred is a contiguous 8x8 byte block. Each output byte is written right
 * after its own prediction byte is read, as in the guest, so in-place use
 * (dst row == pred row) behaves identically.
 */
static inline void bink_idct_add(uint8_t *const dst[8], const uint8_t *coef,
                                 const uint8_t *quant, const uint8_t *pred)
{
    int32_t v[64];

    bink_idct_2d(coef, quant, v);
    for (int r = 0; r < 8; r++) {
        for (int i = 0; i < 8; i++) {
            dst[r][i] = (uint8_t)(pred[8 * r + i] + bink_px(v[8 * r + i]));
        }
    }
}

/* MMX lane helpers, 4 x 16-bit words in a uint64_t. */
static inline uint16_t bink_w(uint64_t q, int i)
{
    return (uint16_t)(q >> (16 * i));
}

static inline uint64_t bink_mkw(const uint16_t w[4])
{
    return (uint64_t)w[0] | (uint64_t)w[1] << 16 | (uint64_t)w[2] << 32 |
           (uint64_t)w[3] << 48;
}

static inline uint64_t bink_paddw(uint64_t a, uint64_t b)
{
    uint16_t w[4];
    for (int i = 0; i < 4; i++) {
        w[i] = (uint16_t)(bink_w(a, i) + bink_w(b, i));
    }
    return bink_mkw(w);
}

static inline uint64_t bink_paddsw(uint64_t a, uint64_t b)
{
    uint16_t w[4];
    for (int i = 0; i < 4; i++) {
        int32_t s = (int16_t)bink_w(a, i) + (int16_t)bink_w(b, i);
        s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        w[i] = (uint16_t)s;
    }
    return bink_mkw(w);
}

static inline uint64_t bink_psubusw(uint64_t a, uint64_t b)
{
    uint16_t w[4];
    for (int i = 0; i < 4; i++) {
        int32_t s = (int32_t)bink_w(a, i) - (int32_t)bink_w(b, i);
        w[i] = (uint16_t)(s < 0 ? 0 : s);
    }
    return bink_mkw(w);
}

static inline uint64_t bink_pmulhw(uint64_t a, uint64_t b)
{
    uint16_t w[4];
    for (int i = 0; i < 4; i++) {
        int32_t p = (int16_t)bink_w(a, i) * (int16_t)bink_w(b, i);
        w[i] = (uint16_t)(p >> 16);
    }
    return bink_mkw(w);
}

static inline uint64_t bink_psllw(uint64_t a, int n)
{
    uint16_t w[4];
    for (int i = 0; i < 4; i++) {
        w[i] = (uint16_t)(bink_w(a, i) << n);
    }
    return bink_mkw(w);
}

/* punpcklwd/punpckhwd x, 0 then pslld n: two dwords from words lo/hi. */
static inline uint64_t bink_unpack_shl(uint64_t a, int hi, int n)
{
    uint32_t d0 = (uint32_t)bink_w(a, 2 * hi) << n;
    uint32_t d1 = (uint32_t)bink_w(a, 2 * hi + 1) << n;
    return (uint64_t)d0 | (uint64_t)d1 << 32;
}

typedef struct BinkYuvConsts {
    uint64_t ysub;  /* psubusw operand */
    uint64_t ymul;  /* pmulhw operand */
    uint64_t clamp; /* paddsw/psubusw clamp, also left in mm7 */
} BinkYuvConsts;

/* Lookup tables, 256 dwords each. */
typedef struct BinkYuvTables {
    const uint8_t *u_b;  /* U -> mm1 */
    const uint8_t *v_g;  /* V -> added to mm2 */
    const uint8_t *u_g;  /* U -> mm2 */
    const uint8_t *v_r;  /* V -> mm3 */
} BinkYuvTables;

/*
 * One iteration of the 4-pixel YUV -> 32-bit RGB loop: 4 Y bytes, 2 U and
 * 2 V bytes in, 16 bytes out. mm[] receives the MMX registers as the guest
 * leaves them after the iteration.
 */
static inline void bink_yuv_step(const uint8_t *y, const uint8_t *u,
                                 const uint8_t *v, uint8_t *out,
                                 const BinkYuvConsts *k,
                                 const BinkYuvTables *t, uint64_t mm[8])
{
    uint64_t mm1, mm2, mm3, mm4, mm5, mm6;
    uint16_t yw[4] = { y[0], y[1], y[2], y[3] };

    mm4 = bink_mkw(yw);
    mm4 = bink_psubusw(mm4, k->ysub);
    mm4 = bink_psllw(mm4, 2);
    mm4 = bink_pmulhw(mm4, k->ymul);

    mm1 = bink_rd32(t->u_b + 4 * u[0]) |
          (uint64_t)bink_rd32(t->u_b + 4 * u[1]) << 32;
    mm2 = bink_paddw(bink_rd32(t->u_g + 4 * u[0]),
                     bink_rd32(t->v_g + 4 * v[0]));
    mm6 = bink_paddw(bink_rd32(t->u_g + 4 * u[1]),
                     bink_rd32(t->v_g + 4 * v[1]));
    mm2 = (mm2 & 0xffffffffu) | (mm6 << 32);
    mm3 = bink_rd32(t->v_r + 4 * v[0]) |
          (uint64_t)bink_rd32(t->v_r + 4 * v[1]) << 32;

    mm1 = bink_psubusw(bink_paddsw(bink_paddw(mm1, mm4), k->clamp), k->clamp);
    mm2 = bink_psubusw(bink_paddsw(bink_paddw(mm2, mm4), k->clamp), k->clamp);
    mm3 = bink_psubusw(bink_paddsw(bink_paddw(mm3, mm4), k->clamp), k->clamp);

    mm4 = bink_unpack_shl(mm1, 1, 0);
    mm5 = bink_unpack_shl(mm2, 1, 8);
    mm6 = bink_unpack_shl(mm3, 1, 16);
    mm1 = bink_unpack_shl(mm1, 0, 0);
    mm2 = bink_unpack_shl(mm2, 0, 8);
    mm3 = bink_unpack_shl(mm3, 0, 16);
    mm1 |= mm2 | mm3;
    mm4 |= mm5 | mm6;
    memcpy(out, &mm1, 8);
    memcpy(out + 8, &mm4, 8);

    mm[0] = 0;
    mm[1] = mm1;
    mm[2] = mm2;
    mm[3] = mm3;
    mm[4] = mm4;
    mm[5] = mm5;
    mm[6] = mm6;
    mm[7] = k->clamp;
}

/*
 * Vector versions, written with clang/GCC vector extensions so the same
 * code compiles to NEON on the phone and to SSE/AVX in the x86 harness that
 * checks it against the original guest code. Arithmetic is done on unsigned
 * lanes so 32-bit wraparound is defined; only shifts use signed lanes.
 */
#if defined(__clang__)
#define BINK_HLE_VECTOR 1

typedef int32_t bink_v8si __attribute__((vector_size(32)));
typedef uint32_t bink_v8su __attribute__((vector_size(32)));
typedef uint8_t bink_v8qu __attribute__((vector_size(8)));
typedef int16_t bink_v8hi __attribute__((vector_size(16)));

static inline bink_v8su bink_vmul11(bink_v8su x, uint32_t c)
{
    return (bink_v8su)((bink_v8si)(x * c) >> 11);
}

/* bink_idct_1d on 8 independent lanes. */
static inline void bink_idct_1d_v(const bink_v8su a[8], bink_v8su o[8])
{
    bink_v8su A0 = a[0] + a[4], A1 = a[0] - a[4], A2 = a[2] + a[6];
    bink_v8su A3 = bink_vmul11(a[2] - a[6], 2896) - A2;
    bink_v8su e0 = A0 + A2, e3 = A0 - A2, e1 = A1 + A3, e2 = A1 - A3;

    bink_v8su B0 = a[3] + a[5], B1 = a[5] - a[3];
    bink_v8su B2 = a[1] + a[7], B3 = a[1] - a[7];
    bink_v8su C0 = B0 + B2, C1 = B2 - B0, C2 = B3 + B1;
    bink_v8su D = bink_vmul11(C2, 3784);
    bink_v8su O1 = bink_vmul11(B1, (uint32_t)-5352) - C0 + D;
    bink_v8su O2 = bink_vmul11(C1, 2896) - O1;
    bink_v8su O3 = bink_vmul11(B3, 2217) - D + O2;

    o[0] = e0 + C0;
    o[7] = e0 - C0;
    o[1] = e1 + O1;
    o[6] = e1 - O1;
    o[2] = e2 + O2;
    o[5] = e2 - O2;
    o[4] = e3 + O3;
    o[3] = e3 - O3;
}

static inline void bink_transpose8(bink_v8su m[8])
{
    bink_v8su t[8], u[8];

    for (int i = 0; i < 8; i += 2) {
        t[i] = __builtin_shufflevector(m[i], m[i + 1], 0, 8, 2, 10, 4, 12, 6, 14);
        t[i + 1] = __builtin_shufflevector(m[i], m[i + 1], 1, 9, 3, 11, 5, 13, 7, 15);
    }
    for (int i = 0; i < 8; i += 4) {
        for (int j = 0; j < 2; j++) {
            u[i + j] = __builtin_shufflevector(t[i + j], t[i + j + 2],
                                               0, 1, 8, 9, 4, 5, 12, 13);
            u[i + j + 2] = __builtin_shufflevector(t[i + j], t[i + j + 2],
                                                   2, 3, 10, 11, 6, 7, 14, 15);
        }
    }
    for (int j = 0; j < 4; j++) {
        m[j] = __builtin_shufflevector(u[j], u[j + 4], 0, 1, 2, 3, 8, 9, 10, 11);
        m[j + 4] = __builtin_shufflevector(u[j], u[j + 4], 4, 5, 6, 7, 12, 13, 14, 15);
    }
}

/* Returns the 8 output rows as int32 lanes (row r = result[r], lane = column). */
static inline void bink_idct_2d_v(const uint8_t *coef, const uint8_t *quant,
                                  bink_v8su m[8])
{
    bink_v8su a[8];

    for (int k = 0; k < 8; k++) {
        bink_v8hi c;
        bink_v8su q;
        memcpy(&c, coef + 16 * k, 16);
        memcpy(&q, quant + 32 * k, 32);
        a[k] = (bink_v8su)((bink_v8si)((bink_v8su)__builtin_convertvector(
                                           c, bink_v8si) * q) >> 11);
    }
    bink_idct_1d_v(a, m);   /* column pass: m[k] = tmp row k */
    bink_transpose8(m);     /* m[i] = tmp column i */
    bink_idct_1d_v(m, a);   /* row pass: a[i] lane r = out[r][i] */
    for (int i = 0; i < 8; i++) {
        m[i] = (bink_v8su)((bink_v8si)(a[i] + 0x7f) >> 8);
    }
    bink_transpose8(m);     /* m[r] lane i = pixel delta of row r */
}

static inline void bink_idct_put_v(uint8_t *const dst[8], const uint8_t *coef,
                                   const uint8_t *quant)
{
    bink_v8su m[8];

    bink_idct_2d_v(coef, quant, m);
    for (int r = 0; r < 8; r++) {
        bink_v8qu b = __builtin_convertvector(m[r], bink_v8qu);
        memcpy(dst[r], &b, 8);
    }
}

static inline void bink_idct_add_v(uint8_t *const dst[8], const uint8_t *coef,
                                   const uint8_t *quant, const uint8_t *pred)
{
    bink_v8su m[8];

    bink_idct_2d_v(coef, quant, m);
    for (int r = 0; r < 8; r++) {
        bink_v8qu p, b;
        memcpy(&p, pred + 8 * r, 8);
        b = __builtin_convertvector(m[r], bink_v8qu) + p;
        memcpy(dst[r], &b, 8);
    }
}

#define BINK_YUV_VLANES 16 /* pixels per vector step: 4 loop iterations */

typedef uint16_t bink_v16hu __attribute__((vector_size(32)));
typedef int16_t bink_v16hi __attribute__((vector_size(32)));
typedef int32_t bink_v16si __attribute__((vector_size(64)));
typedef uint32_t bink_v16su __attribute__((vector_size(64)));
typedef uint8_t bink_v16qu __attribute__((vector_size(16)));
typedef uint32_t bink_v8u32 __attribute__((vector_size(32)));

static inline bink_v16hu bink_rep16(uint64_t q)
{
    bink_v16hu v;
    for (int i = 0; i < 16; i++) {
        v[i] = (uint16_t)(q >> (16 * (i & 3)));
    }
    return v;
}

/* 4 iterations of bink_yuv_step at once (no MMX state). */
static inline void bink_yuv_step4(const uint8_t *y, const uint8_t *u,
                                  const uint8_t *v, uint8_t *out,
                                  bink_v16hu ysub, bink_v16hi ymul,
                                  bink_v16hu clamp, const BinkYuvTables *t)
{
    bink_v16qu yb;
    bink_v8u32 c1, c2a, c2b, c3;

    memcpy(&yb, y, 16);
    bink_v16hu yw = __builtin_convertvector(yb, bink_v16hu);
    yw = __builtin_elementwise_sub_sat(yw, ysub);
    yw = yw << 2;
    yw = (bink_v16hu)__builtin_convertvector(
        (__builtin_convertvector((bink_v16hi)yw, bink_v16si) *
         __builtin_convertvector(ymul, bink_v16si)) >> 16, bink_v16hi);

    for (int m = 0; m < 8; m++) {
        c1[m] = bink_rd32(t->u_b + 4 * u[m]);
        c2a[m] = bink_rd32(t->u_g + 4 * u[m]);
        c2b[m] = bink_rd32(t->v_g + 4 * v[m]);
        c3[m] = bink_rd32(t->v_r + 4 * v[m]);
    }
    bink_v16hu w1 = (bink_v16hu)c1 + yw;
    bink_v16hu w2 = (bink_v16hu)c2a + (bink_v16hu)c2b + yw;
    bink_v16hu w3 = (bink_v16hu)c3 + yw;

    w1 = __builtin_elementwise_sub_sat(
        (bink_v16hu)__builtin_elementwise_add_sat((bink_v16hi)w1,
                                                  (bink_v16hi)clamp), clamp);
    w2 = __builtin_elementwise_sub_sat(
        (bink_v16hu)__builtin_elementwise_add_sat((bink_v16hi)w2,
                                                  (bink_v16hi)clamp), clamp);
    w3 = __builtin_elementwise_sub_sat(
        (bink_v16hu)__builtin_elementwise_add_sat((bink_v16hi)w3,
                                                  (bink_v16hi)clamp), clamp);

    bink_v16su px = __builtin_convertvector(w1, bink_v16su) |
                    __builtin_convertvector(w2, bink_v16su) << 8 |
                    __builtin_convertvector(w3, bink_v16su) << 16;
    memcpy(out, &px, 64);
}

/*
 * @count iterations from y/u/v into out. The last iteration goes through the
 * scalar step so mm[] gets the guest's final MMX registers.
 */
static inline void bink_yuv_run(const uint8_t *y, const uint8_t *u,
                                const uint8_t *v, uint8_t *out, uint32_t count,
                                const BinkYuvConsts *k, const BinkYuvTables *t,
                                uint64_t mm[8])
{
    bink_v16hu ysub = bink_rep16(k->ysub), clamp = bink_rep16(k->clamp);
    bink_v16hi ymul = (bink_v16hi)bink_rep16(k->ymul);
    uint32_t i = 0;

    for (; i + 4 < count; i += 4) {
        bink_yuv_step4(y + 4 * i, u + 2 * i, v + 2 * i, out + 16 * i,
                       ysub, ymul, clamp, t);
    }
    for (; i < count; i++) {
        bink_yuv_step(y + 4 * i, u + 2 * i, v + 2 * i, out + 16 * i, k, t, mm);
    }
}
#endif

#endif
