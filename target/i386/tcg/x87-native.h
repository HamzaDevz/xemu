/*
 * x87 arithmetic on the host FPU, where it is bit-exact
 *
 * Xbox games run the x87 with precision control set to double (53-bit
 * significand), some phases at single (24-bit, see x87n_round_single()), and
 * round-to-nearest. At double precision an x87 add, subtract, multiply,
 * divide or square root rounds the exact result once, to 53 bits, keeping the
 * 15-bit extended exponent. A host IEEE double operation rounds the same
 * exact result once, to the same 53 bits. The two agree bit for bit whenever
 *
 *   - both operands are exactly representable as doubles (normal, at most 53
 *     significant bits; a register loaded or computed at 64-bit precision may
 *     carry more), and
 *   - the result lies well inside the double normal range, so neither the
 *     double's narrower exponent nor its gradual underflow comes into play.
 *
 * Under those conditions the only exception the operation can raise is
 * inexact: invalid needs a NaN or infinity (excluded), divide-by-zero gives
 * an infinity (excluded), overflow and underflow need an out-of-range result
 * (excluded). Inexact is recovered exactly from the rounding error: TwoSum
 * for add/subtract, an FMA residual for multiply, divide and square root.
 * Those residuals are exact because the guard keeps every intermediate far
 * from both ends of the exponent range.
 *
 * Anything else returns false and the caller falls back to softfloat. The
 * registers keep their 80-bit format throughout; only the arithmetic moves.
 * (This is unlike USE_HARD_FPU, which stored registers as doubles and lost
 * extended-precision equality, see fpu_helper_hard.c.)
 *
 * Self-contained, so tests can check it against softfloat directly.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#ifndef TARGET_I386_TCG_X87_NATIVE_H
#define TARGET_I386_TCG_X87_NATIVE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* No contraction: the error terms below rely on separately rounded steps. */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

enum {
    X87N_ADD,
    X87N_SUB,
    X87N_MUL,
    X87N_DIV,
};

/*
 * Operands and results must have an unbiased exponent within +-X87N_EXP_LIMIT.
 * The double normal range is -1022..1023; the margin keeps the residuals
 * (down to about 2^(e - 106)) normal and products/quotients finite or caught.
 */
#define X87N_EXP_LIMIT 900

#define X87N_FRAC_MASK 0x000FFFFFFFFFFFFFull

/*
 * Whether round-to-nearest-even bumps kept bits ending in lsb, given the
 * discarded bits left-aligned in rem: above a half, or a half with lsb odd.
 * Computed as a carry, since a branch on rem mispredicts on real data.
 */
static inline bool x87n_round_nearest(uint64_t rem, uint64_t lsb)
{
    uint64_t sum;

    return __builtin_add_overflow(rem, (1ull << 63) - 1 + (lsb & 1), &sum);
}

/*
 * Under single precision control the x87 rounds the exact result to 24 bits,
 * still with the extended exponent. A double result rounded again to 24 bits
 * agrees with that except at a tie on the double, which the exact error
 * resolves: dir is the sign of (exact - r), and when it is not zero the
 * exact value lies off the tie, on dir's side. The double's exponent stays
 * as it is (the range checks around it still apply), and a carry out of the
 * significand moves into it as it should.
 */
static inline double x87n_round_single(double r, int dir, bool *inexact)
{
    uint64_t bits;
    memcpy(&bits, &r, sizeof(bits));
    uint64_t rem = bits << 35;          /* the 29 bits dropped, left-aligned */
    int away = (bits >> 63) ? -dir : dir;    /* exact magnitude above |r| */
    bool tie_up = away > 0 || (away == 0 && ((bits >> 29) & 1));

    bits = ((bits >> 29) + x87n_round_nearest(rem, tie_up)) << 29;
    *inexact |= rem != 0;
    memcpy(&r, &bits, sizeof(bits));
    return r;
}

/* An extended value as a double, if it is exactly one (and in range). */
static inline bool x87n_to_double(uint64_t low, uint16_t high, double *out)
{
    uint32_t exp = high & 0x7fff;
    uint64_t sign = (uint64_t)(high >> 15) << 63;
    uint64_t bits;

    if (exp == 0) {
        if (low) {
            return false;               /* denormal or pseudo-denormal */
        }
        bits = sign;                    /* +-0 */
    } else {
        int e = (int)exp - 16383;
        if (!(low >> 63) ||             /* unnormal, or infinity/NaN encoding */
            (low & 0x7ff) ||            /* more than 53 significant bits */
            e < -X87N_EXP_LIMIT || e > X87N_EXP_LIMIT) {
            return false;
        }
        bits = sign | ((uint64_t)(e + 1023) << 52) | ((low >> 11) & X87N_FRAC_MASK);
    }
    memcpy(out, &bits, sizeof(bits));
    return true;
}

/* A double result as an extended value, if it is zero or in range. */
static inline bool x87n_from_double(double r, uint64_t *low, uint16_t *high)
{
    uint64_t bits;
    memcpy(&bits, &r, sizeof(bits));
    uint16_t sign = (uint16_t)((bits >> 63) << 15);
    int biased = (int)((bits >> 52) & 0x7ff);

    if (biased == 0) {
        if (bits & X87N_FRAC_MASK) {
            return false;               /* subnormal */
        }
        *low = 0;
        *high = sign;                   /* +-0 */
        return true;
    }
    int e = biased - 1023;
    if (e < -X87N_EXP_LIMIT || e > X87N_EXP_LIMIT) {
        return false;                   /* also rejects infinity and NaN */
    }
    *low = (1ull << 63) | ((bits & X87N_FRAC_MASK) << 11);
    *high = sign | (uint16_t)(e + 16383);
    return true;
}

/*
 * Store a double result r (inexact: err != 0, dir: the sign of exact - r),
 * rounded to single first if asked. The range is checked on r itself too, as
 * rounding a subnormal could make it look like an exact zero.
 */
static inline bool x87n_result(double r, bool inexact_r, bool single, int dir,
                               uint64_t *r_low, uint16_t *r_high, bool *inexact)
{
    uint64_t low;
    uint16_t high;

    if (!x87n_from_double(r, &low, &high)) {
        return false;
    }
    if (single && !x87n_from_double(x87n_round_single(r, dir, &inexact_r),
                                    &low, &high)) {
        return false;
    }
    *r_low = low;
    *r_high = high;
    *inexact = inexact_r;
    return true;
}

/*
 * a op b at double precision (single: at single precision), round-to-nearest.
 * Returns false (and writes nothing) when the result might differ from the
 * x87's; *inexact is the PE flag otherwise.
 */
static inline bool x87n_binop(int op, bool single,
                              uint64_t a_low, uint16_t a_high,
                              uint64_t b_low, uint16_t b_high,
                              uint64_t *r_low, uint16_t *r_high, bool *inexact)
{
    double a, b, r, err;
    int dir;

    if (!x87n_to_double(a_low, a_high, &a) ||
        !x87n_to_double(b_low, b_high, &b)) {
        return false;
    }

    switch (op) {
    case X87N_ADD:
    case X87N_SUB: {
        if (op == X87N_SUB) {
            b = -b;
        }
        r = a + b;
        /* TwoSum: err is exactly (a + b) - r */
        double bb = r - a;
        err = (a - (r - bb)) + (b - bb);
        dir = (err > 0) - (err < 0);
        break;
    }
    case X87N_MUL:
        r = a * b;
        err = fma(a, b, -r);
        dir = (err > 0) - (err < 0);
        break;
    case X87N_DIV:
        if (b == 0.0) {
            return false;               /* divide-by-zero or invalid */
        }
        r = a / b;
        err = fma(-r, b, a);            /* a - r * b, exact */
        dir = ((err > 0) - (err < 0)) * (b < 0 ? -1 : 1);
        break;
    default:
        return false;
    }

    /*
     * A zero is exact from add/subtract (exact cancellation; the operands are
     * far above the underflow threshold) or from a zero operand. From two
     * non-zero factors, or a non-zero dividend, it is a double underflow
     * where the x87's wider exponent still holds a tiny value.
     */
    if (r == 0.0 && (op == X87N_MUL ? (a != 0.0 && b != 0.0) :
                     op == X87N_DIV ? a != 0.0 : false)) {
        return false;
    }

    return x87n_result(r, err != 0.0, single, dir, r_low, r_high, inexact);
}

/* sqrt(a) at double (or single) precision, round-to-nearest; as x87n_binop. */
static inline bool x87n_sqrt(bool single, uint64_t a_low, uint16_t a_high,
                             uint64_t *r_low, uint16_t *r_high, bool *inexact)
{
    double a, r;

    if (!x87n_to_double(a_low, a_high, &a) || a < 0.0) {
        return false;                   /* negative: invalid */
    }
    r = sqrt(a);
    double err = fma(-r, r, a);         /* a - r * r: sqrt(a) - r has its sign */
    return x87n_result(r, err != 0.0, single, (err > 0) - (err < 0),
                       r_low, r_high, inexact);
}

/*
 * Compares and integer conversions need no host FPU at all: for finite
 * operands they are exact integer work on the 80-bit encoding, valid at any
 * precision (a register may hold all 64 significand bits). Only zeros and
 * normal numbers are taken; denormals (which raise DE), unnormals,
 * infinities and NaNs go to softfloat.
 */

/* A zero or a normal finite number. */
static inline bool x87n_ordinary(uint64_t low, uint16_t high, uint32_t *exp)
{
    *exp = high & 0x7fff;
    if (*exp == 0) {
        return low == 0;                /* +-0, not a denormal */
    }
    return *exp != 0x7fff && (low >> 63);
}

/*
 * a compared with b: -1 less, 0 equal, 1 greater (softfloat's FloatRelation).
 * No exception is possible for these operands, signalling or quiet.
 */
static inline bool x87n_compare(uint64_t a_low, uint16_t a_high,
                                uint64_t b_low, uint16_t b_high, int *rel)
{
    uint32_t ea, eb;

    if (!x87n_ordinary(a_low, a_high, &ea) ||
        !x87n_ordinary(b_low, b_high, &eb)) {
        return false;
    }

    bool za = ea == 0, zb = eb == 0;
    if (za && zb) {
        *rel = 0;                       /* +0 == -0 */
        return true;
    }
    bool na = !za && (a_high >> 15), nb = !zb && (b_high >> 15);
    if (na != nb) {
        *rel = na ? -1 : 1;
        return true;
    }
    /* Same sign (a zero counts as positive here): order the magnitudes. */
    int mag = (ea != eb) ? (ea < eb ? -1 : 1) :
              (a_low != b_low) ? (a_low < b_low ? -1 : 1) : 0;
    *rel = na ? -mag : mag;
    return true;
}

enum {                                  /* x87 RC, FPUC bits 11:10 */
    X87N_RC_NEAREST,
    X87N_RC_DOWN,
    X87N_RC_UP,
    X87N_RC_CHOP,
};

/*
 * a rounded to an integer by rc, if |a| < 2^62 (so any int64 result fits
 * after rounding); *inexact if a had a fractional part.
 */
static inline bool x87n_to_int64(uint64_t low, uint16_t high, int rc,
                                 int64_t *out, bool *inexact)
{
    uint32_t exp;
    uint64_t mag, frac;

    if (!x87n_ordinary(low, high, &exp)) {
        return false;
    }
    if (exp == 0) {
        *out = 0;
        *inexact = false;
        return true;
    }

    int e = (int)exp - 16383;
    if (e > 61) {
        return false;                   /* may not fit; leave the overflow */
    }
    int shift = 63 - e;                 /* >= 2 */
    if (shift < 64) {
        mag = low >> shift;
        frac = low << (64 - shift);     /* fraction, left-aligned */
    } else if (shift == 64) {
        mag = 0;
        frac = low;                     /* [0.5, 1) */
    } else {
        mag = 0;
        frac = 1;                       /* (0, 0.5): non-zero, below a half */
    }

    bool neg = high >> 15;
    bool inc;
    switch (rc) {
    case X87N_RC_NEAREST:
        inc = x87n_round_nearest(frac, mag);
        break;
    case X87N_RC_DOWN:
        inc = neg && frac;
        break;
    case X87N_RC_UP:
        inc = !neg && frac;
        break;
    default:
        inc = false;
        break;
    }
    mag += inc;

    *out = neg ? -(int64_t)mag : (int64_t)mag;
    *inexact = frac != 0;
    return true;
}

/*
 * Loads and stores are integer work on the encodings too. A load widens
 * exactly, but softfloat then rounds it to the precision control (prec_bits:
 * 24, 53 or 64 significand bits), so a value with more significant bits than
 * that is left to softfloat. A store rounds once, by rc, to the memory
 * format. Zeros and normal numbers only, as above; no exception but inexact.
 */

/* Whether a significand fits in prec_bits, so precision rounding is exact */
static inline bool x87n_fits(uint64_t low, int prec_bits)
{
    return prec_bits >= 64 || !(low << prec_bits);
}

/*
 * An IEEE single (frac_bits 23, bias 127) or double (52, 1023) as an
 * extended value.
 */
static inline bool x87n_from_ieee(uint64_t f, int frac_bits, int bias,
                                  int prec_bits, uint64_t *low, uint16_t *high)
{
    int exp_bits = frac_bits == 23 ? 8 : 11;
    uint32_t exp = (f >> frac_bits) & ((1u << exp_bits) - 1);
    uint64_t frac = f & ((1ull << frac_bits) - 1);
    uint16_t sign = (uint16_t)(((f >> (frac_bits + exp_bits)) & 1) << 15);

    if (exp == 0) {
        if (frac) {
            return false;               /* denormal: raises DE */
        }
        *low = 0;
        *high = sign;
        return true;
    }
    if (exp == (1u << exp_bits) - 1) {
        return false;                   /* infinity or NaN */
    }
    *low = (1ull << 63) | (frac << (63 - frac_bits));
    *high = sign | (uint16_t)((int)exp - bias + 16383);
    return x87n_fits(*low, prec_bits);
}

/* An integer as an extended value (exact at 64 bits) */
static inline bool x87n_from_int64(int64_t v, int prec_bits,
                                   uint64_t *low, uint16_t *high)
{
    uint64_t mag = v < 0 ? -(uint64_t)v : (uint64_t)v;

    if (!mag) {
        *low = 0;
        *high = 0;                      /* +0 */
        return true;
    }
    int lz = __builtin_clzll(mag);
    *low = mag << lz;
    *high = (uint16_t)((v < 0 ? 0x8000 : 0) | (16383 + 63 - lz));
    return x87n_fits(*low, prec_bits);
}

/*
 * An extended value rounded by rc to an IEEE single or double; false when
 * the result would be subnormal, tiny or overflow.
 */
static inline bool x87n_to_ieee(uint64_t low, uint16_t high, int rc,
                                int frac_bits, int bias,
                                uint64_t *out, bool *inexact)
{
    int exp_bits = frac_bits == 23 ? 8 : 11;
    uint64_t sign = (uint64_t)(high >> 15) << (frac_bits + exp_bits);
    uint32_t exp;

    if (!x87n_ordinary(low, high, &exp)) {
        return false;
    }
    if (exp == 0) {
        *out = sign;
        *inexact = false;
        return true;
    }

    int e = (int)exp - 16383;
    if (e < 1 - bias || e > bias) {
        return false;
    }
    int shift = 63 - frac_bits;         /* 40 or 11 */
    uint64_t keep = low >> shift;       /* with the integer bit */
    uint64_t rem = low << (64 - shift); /* discarded bits, left-aligned */
    bool neg = high >> 15;
    bool inc;
    switch (rc) {
    case X87N_RC_NEAREST:
        inc = x87n_round_nearest(rem, keep);
        break;
    case X87N_RC_DOWN:
        inc = neg && rem;
        break;
    case X87N_RC_UP:
        inc = !neg && rem;
        break;
    default:
        inc = false;
        break;
    }
    keep += inc;
    if (keep >> (frac_bits + 1)) {      /* rounded up to the next binade */
        keep >>= 1;
        if (++e > bias) {
            return false;               /* overflow */
        }
    }

    *out = sign | ((uint64_t)(e + bias) << frac_bits) |
           (keep & ((1ull << frac_bits) - 1));
    *inexact = rem != 0;
    return true;
}

#endif
