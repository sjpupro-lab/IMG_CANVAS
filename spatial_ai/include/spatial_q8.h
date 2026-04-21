#ifndef SPATIAL_Q8_H
#define SPATIAL_Q8_H

#include <stdint.h>

/*
 * Fixed-point "clockwork tick" representation.
 *
 *   Q16: 65 536 ticks = 1.0
 *   range = [0, 65535] in uint16_t
 *   resolution ≈ 1.5 × 10^-5 (vs Q8's 4 × 10^-3)
 *
 * Used for similarity values, similarity thresholds, and channel
 * weights — anything that lives in [0, 1] in the float code. The
 * intent is to remove float sqrt, double accumulation, and the
 * cumulative rounding error those bring while keeping the same
 * semantics. Comparison becomes a single integer compare; the only
 * floating-point cost on the hot path is the one isqrt per cosine.
 *
 * Q16 was picked over Q8 after a measurement run on wiki5k showed
 * Q8 quantization shifted freq_tag avg from 3.92 to 7.26 — cosine
 * thresholds at 0.01 round up to ~0.0117 in Q8, which is
 * empirically too coarse. Q16 preserves four-decimal-digit
 * precision, matching the "scale-able tick" idea (the user wants
 * freedom to crank the multiplier — Q16 is ×65536 instead of ×256
 * with the same gear-style integer machinery).
 *
 * Conversion happens at API boundaries (CLI parses --threshold 0.30
 * as a float, then q16_from_float clamps and rounds it to 19 661
 * ticks). Inside the engine, only Q16 integers move around.
 */

#define Q16_ONE   65536u   /* canonical "1.0" (saturates to MAX in uint16) */
#define Q16_MAX   65535u   /* saturated max representable in uint16_t */

/* Float (0..1) → Q16 (0..65535). Saturating, round-to-nearest. */
static inline uint16_t q16_from_float(float f) {
    if (f <= 0.0f) return 0;
    if (f >= 1.0f) return Q16_MAX;
    int v = (int)(f * 65536.0f + 0.5f);
    if (v > (int)Q16_MAX) v = Q16_MAX;
    if (v < 0)            v = 0;
    return (uint16_t)v;
}

/* Q16 → float, for reporting only. Hot-path code never calls this. */
static inline float q16_to_float(uint16_t q) {
    return (float)q / 65536.0f;
}

/* Integer sqrt for uint64. Newton's method, converges in ≤6 iterations
 * for the magnitudes we see (cosine denominators ≤ ~10^17). Used once
 * per cosine evaluation; avoids the FPU sqrt entirely. */
static inline uint64_t isqrt_u64(uint64_t n) {
    if (n < 2) return n;
    uint64_t x = n;
    uint64_t y = (x + 1u) >> 1;
    while (y < x) {
        x = y;
        y = (x + n / x) >> 1;
    }
    return x;
}

/* Cosine numerator/denominator → Q16 value.
 *   q16 = (dot * 65536) / sqrt(na * nb)
 * Saturates at 65535. Returns 0 when either norm is zero. dot, na,
 * nb arguments are already squared sums kept in uint64. Overflow
 * margin: the largest hot-path inputs (canvas block-sum cosine, dot
 * up to ~2 × 10^7) yield dot * 65536 ≈ 1.3 × 10^12 — well under the
 * uint64 ceiling (1.8 × 10^19). */
static inline uint16_t q16_cosine(uint64_t dot, uint64_t na, uint64_t nb) {
    if (na == 0 || nb == 0) return 0;
    uint64_t denom = isqrt_u64(na * nb);
    if (denom == 0) return 0;
    uint64_t num = dot * 65536u;
    uint64_t r = num / denom;
    if (r > Q16_MAX) r = Q16_MAX;
    return (uint16_t)r;
}

#endif /* SPATIAL_Q8_H */
