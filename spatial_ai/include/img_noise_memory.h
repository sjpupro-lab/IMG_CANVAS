#ifndef IMG_NOISE_MEMORY_H
#define IMG_NOISE_MEMORY_H

#include <stdint.h>
#include <stddef.h>

#include "img_ce.h"

/*
 * img_noise_memory — per-cell learned prior for deterministic
 * noise/seed synthesis.
 *
 *   The delta memory (img_delta_memory) answers "which stamp goes
 *   here?". The noise memory answers "what was most often already
 *   here before any stamps were applied?" — i.e. a spatial prior
 *   that replaces random Gaussian noise as the drawing-pass seed.
 *
 *   observe(grid, label)   → accumulate top-K sample frequencies
 *                            per CE cell (and tier / global fallbacks)
 *   sample_grid(out, opts) → deterministic per-cell sampling from
 *                            those learned priors, controlled by a
 *                            seed / tier / role / mask.
 *
 *   Design invariants:
 *     - Zero floating point in the grid sampling path (temperature
 *       is converted to 8.8 fixed point before any use).
 *     - PRNG is xorshift64 keyed off a caller-supplied u64 seed.
 *     - Output of sample_grid is a pure function of (memory, opts).
 *     - Safe to call without a memory file: callers pass NULL and
 *       the wrapper in img_drawing skips the prior step entirely.
 */

#define IMG_NOISE_TOPK     8
#define IMG_NOISE_MAGIC    "NMEM"
#define IMG_NOISE_VERSION  1u

/* Number of 8-direction buckets and delta-sign buckets that fit in
 * the `direction` byte (3 bits + 2 bits + 3 bits reserved). */
#define IMG_NOISE_DIR_BUCKETS   8
#define IMG_NOISE_DSIGN_BUCKETS 4

/* Exactly 8 bytes, no padding. Packs the CE RGBA signal plus a
 * compressed slice of the interpretation tags. */
typedef struct ImgNoiseSample {
    uint8_t ce_r;        /* CE core     */
    uint8_t ce_g;        /* CE link     */
    uint8_t ce_b;        /* CE delta    */
    uint8_t ce_a;        /* CE priority */
    uint8_t tags0;       /* tone(2) | flow(3) | mood(2) | reserved(1) */
    uint8_t tags1;       /* depth(2) | role(3) | tier(2) | reserved(1) */
    uint8_t direction;   /* dir(3) | delta_sign(2) | reserved(3)      */
    uint8_t weight;      /* saturating frequency accumulator          */
} ImgNoiseSample;

/* 64 bytes = IMG_NOISE_TOPK × 8. */
typedef struct ImgNoiseCellProfile {
    ImgNoiseSample top_k[IMG_NOISE_TOPK];
} ImgNoiseCellProfile;

/* Retrieval index entry; 16 bytes packed. */
typedef struct ImgNoiseLabelEntry {
    uint64_t label_hash;       /* FNV-1a 64-bit of the utf-8 label  */
    uint32_t profile_offset;   /* reserved (retrieval profiles: v2) */
    uint32_t keyframe_ref;     /* SpatialAI kf id or UINT32_MAX     */
} ImgNoiseLabelEntry;

typedef struct ImgNoiseMemory {
    ImgNoiseCellProfile  cell_priors[IMG_CE_TOTAL];
    ImgNoiseCellProfile  tier_priors[3];     /* T1, T2, T3           */
    ImgNoiseCellProfile  global_prior;

    uint32_t             label_count;
    uint32_t             label_capacity;
    ImgNoiseLabelEntry*  label_index;

    uint32_t             observe_count;      /* drives periodic decay */
    uint32_t             flags;              /* reserved              */
} ImgNoiseMemory;

/* ── lifecycle ─────────────────────────────────────────────── */

/* Zero-initialise an in-place ImgNoiseMemory. Returns 1 on success. */
int  img_noise_memory_init(ImgNoiseMemory* nmem);

/* Release any heap owned by nmem (label index). Safe on zero-init. */
void img_noise_memory_free(ImgNoiseMemory* nmem);

/* ── learning ──────────────────────────────────────────────── */

/* Fold one CE grid observation into `nmem`. Per cell:
 *   - compress the CE cell to an ImgNoiseSample
 *   - accumulate into cell_priors[y*64+x]
 *   - accumulate into tier_priors[sample.tier-1]
 *   - accumulate into global_prior
 *
 * Every 1024 observations all weights are multiplied by ~0.95 via
 * integer arithmetic so stale samples fade. Returns 1 on success.
 *
 * `label_or_null` is currently reserved (registered in the label
 * index for v2 retrieval; profile_offset stays zero until that
 * feature lands). */
int  img_noise_memory_observe(ImgNoiseMemory* nmem,
                              const ImgCEGrid* grid,
                              const char* label_or_null);

/* ── sampling ──────────────────────────────────────────────── */

typedef struct ImgNoiseSampleOptions {
    uint64_t        seed;            /* PRNG seed; 0 → fixed internal      */
    uint8_t         tier_hint;       /* 0 = any, else IMG_TIER_T1..T3      */
    uint8_t         role_hint;       /* 0 = any, else IMG_ROLE_*           */
    uint8_t         k_mix;           /* 1 = weighted pick, >1 = blend k    */
    uint8_t         flags;           /* reserved                           */
    const uint8_t*  region_mask;     /* IMG_CE_TOTAL bytes or NULL         */
    uint32_t        temperature_q8;  /* 8.8 fixed: 256 = 1.0, 0 → greedy   */
    uint64_t        label_hash;      /* 0 = no label conditioning          */
} ImgNoiseSampleOptions;

ImgNoiseSampleOptions img_noise_sample_default_options(void);

/* Fill `out_grid` from `nmem` according to `opts`. Cells whose
 * region_mask byte is zero are left untouched.
 *
 * Returns 1 on success, 0 on argument error. Deterministic: same
 * inputs produce byte-identical output. */
int img_noise_memory_sample_grid(const ImgNoiseMemory*         nmem,
                                 ImgCEGrid*                    out_grid,
                                 const ImgNoiseSampleOptions*  opts);

/* ── persistence ───────────────────────────────────────────── */

/* Save `nmem` to `path` using the little-endian NMEM v1 format.
 * Returns 1 on success. */
int  img_noise_memory_save(const ImgNoiseMemory* nmem, const char* path);

/* Load `nmem` (must be zero-init or freshly _init'd) from `path`.
 * Returns 1 on success, 0 on any error (open / magic / version /
 * short read). On failure `nmem` is left in an _init'd empty state
 * if it was one; callers should treat it as a soft failure. */
int  img_noise_memory_load(ImgNoiseMemory* nmem, const char* path);

/* ── helpers / FNV-1a 64 ──────────────────────────────────── */

/* Hash the NUL-terminated string `s`. Returns 0 for NULL input. */
uint64_t img_noise_fnv1a64(const char* s);

#endif /* IMG_NOISE_MEMORY_H */
