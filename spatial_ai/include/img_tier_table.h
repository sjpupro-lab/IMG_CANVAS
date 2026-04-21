#ifndef IMG_TIER_TABLE_H
#define IMG_TIER_TABLE_H

#include "img_delta_memory.h"   /* IMG_TIER_MAX, ImgTier */

#include <stdint.h>

/*
 * img_tier_table — canonical SPEC §10 / §13.4 tier descriptor.
 *
 *   Each tier carries two invariants:
 *     scale_factor — multiplicative weight used by img_delta_compute
 *                    when turning a delta resume code into a concrete
 *                    channel contribution.
 *     range_max    — inclusive upper bound of channel values that
 *                    belong to this tier. Rendering uses it to place
 *                    a cell's channel value into a tier 0..4.
 *
 *   Tier indices align with ImgTier (IMG_TIER_NONE / T1 / T2 / T3).
 *   IMG_TIER_MAX is 4. A fifth rendering tier ("saturated", value >
 *   T3.range_max) is implicit and only meaningful to the renderer.
 */

typedef struct {
    uint16_t scale_factor;  /* base magnitude of this tier */
    uint8_t  range_max;     /* ≤ this → belongs to this tier */
    uint8_t  reserved;      /* padding — keep sizeof = 4 */
} ImgTierEntry;

/* Canonical const table. Shared by img_delta_compute (scale_factor)
 * and img_render (range_max). Baked delta tables are regenerated
 * against this same data — see tools/gen_delta_tables.c. */
extern const ImgTierEntry IMG_TIER_TABLE[IMG_TIER_MAX];

/* Classify a channel value into one of 5 visual tiers 0..4:
 *   0 — quiescent  (value == 0)
 *   1 — fine       (value ≤ T1.range_max)
 *   2 — mid        (value ≤ T2.range_max)
 *   3 — structure  (value ≤ T3.range_max)
 *   4 — saturated  (value  > T3.range_max, bleeds in rendering)
 */
uint8_t img_tier_classify(uint8_t value);

/* Same but against a caller-supplied table (for per-channel
 * overrides or adapted tables). `tbl` must be IMG_TIER_MAX entries. */
uint8_t img_tier_classify_with(uint8_t value, const ImgTierEntry* tbl);

/* Adaptive tier: adjust the `range_max` entries so that the
 * observed value histogram splits roughly evenly into 4 tiers by
 * cumulative mass (quantile-based). `scale_factor` is left at the
 * canonical defaults — callers who want adaptive scales should
 * write them in after this call.
 *
 * `histogram` is 256 uint32 bins keyed by channel value.
 * `out` receives the adapted table. Both are required. */
void img_tier_adapt(const uint32_t histogram[256],
                    ImgTierEntry out[IMG_TIER_MAX]);

/* ── Per-channel histograms off a CE grid ────────────────── */

typedef enum {
    IMG_CE_CHANNEL_CORE     = 0,
    IMG_CE_CHANNEL_LINK     = 1,
    IMG_CE_CHANNEL_DELTA    = 2,
    IMG_CE_CHANNEL_PRIORITY = 3
} ImgCEChannel;

/* Build a 256-bin histogram of one CE channel's values across the
 * entire grid. `out` is fully rewritten (no accumulation). Useful
 * as an input to img_tier_adapt. */
void img_tier_build_histogram_ce(const ImgCEGrid* ce,
                                 ImgCEChannel channel,
                                 uint32_t out[256]);

#endif /* IMG_TIER_TABLE_H */

