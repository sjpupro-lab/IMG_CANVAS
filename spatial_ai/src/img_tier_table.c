#include "img_tier_table.h"
#include "img_ce.h"

#include <string.h>

/*
 * Canonical tier table. scale_factor numbers match the previously
 * inline TIER_MAGNITUDE array in img_delta_compute.c; range_max
 * numbers match the previously inline {8, 32, 96} thresholds in
 * img_render_default_options. Relocating them here makes them the
 * single source of truth without changing numeric behaviour.
 */
const ImgTierEntry IMG_TIER_TABLE[IMG_TIER_MAX] = {
    /* T0 NONE      */ {  0,   0, 0 },
    /* T1 FINE      */ {  4,   8, 0 },
    /* T2 MID       */ { 12,  32, 0 },
    /* T3 STRUCTURE */ { 24,  96, 0 },
};

uint8_t img_tier_classify(uint8_t value) {
    return img_tier_classify_with(value, IMG_TIER_TABLE);
}

uint8_t img_tier_classify_with(uint8_t value, const ImgTierEntry* tbl) {
    if (!tbl)    return 0;
    if (value == 0) return 0;
    if (value <= tbl[IMG_TIER_T1].range_max) return 1;
    if (value <= tbl[IMG_TIER_T2].range_max) return 2;
    if (value <= tbl[IMG_TIER_T3].range_max) return 3;
    return 4;   /* saturated — above T3 */
}

/* Quantile-based adapt: find thresholds t1, t2, t3 so that
 *   count(value ≤ t1) / total ≈ 0.25
 *   count(value ≤ t2) / total ≈ 0.50
 *   count(value ≤ t3) / total ≈ 0.75
 * where `count` is the histogram mass excluding value 0 (since 0
 * belongs to T0 by construction).
 *
 * If the histogram is empty (all bins zero, or only bin 0 is
 * populated), the canonical default table is written.
 */
void img_tier_adapt(const uint32_t histogram[256],
                    ImgTierEntry out[IMG_TIER_MAX]) {
    if (!histogram || !out) return;

    /* Start from the canonical defaults (preserves scale_factor). */
    memcpy(out, IMG_TIER_TABLE, sizeof(IMG_TIER_TABLE));

    /* Total nonzero-value mass. Bin 0 is excluded — that's T0. */
    uint64_t total = 0;
    for (int v = 1; v < 256; v++) total += histogram[v];
    if (total == 0) return;

    const uint64_t q25 = (total + 2) / 4;
    const uint64_t q50 = (total + 1) / 2;
    const uint64_t q75 = (3 * total + 2) / 4;

    uint64_t running = 0;
    int t1 = -1, t2 = -1, t3 = -1;
    for (int v = 1; v < 256; v++) {
        running += histogram[v];
        if (t1 < 0 && running >= q25) t1 = v;
        if (t2 < 0 && running >= q50) t2 = v;
        if (t3 < 0 && running >= q75) t3 = v;
    }

    /* Guarantee strict monotonicity even on bumpy histograms. */
    if (t1 < 0) t1 = 1;
    if (t2 < 0 || t2 <= t1) t2 = (t1 + 1 < 255) ? t1 + 1 : t1;
    if (t3 < 0 || t3 <= t2) t3 = (t2 + 1 < 255) ? t2 + 1 : t2;
    if (t3 > 255) t3 = 255;

    out[IMG_TIER_T1].range_max = (uint8_t)t1;
    out[IMG_TIER_T2].range_max = (uint8_t)t2;
    out[IMG_TIER_T3].range_max = (uint8_t)t3;
}

/* ── per-channel histogram ──────────────────────────────── */

void img_tier_build_histogram_ce(const ImgCEGrid* ce,
                                 ImgCEChannel channel,
                                 uint32_t out[256]) {
    if (!out) return;
    memset(out, 0, 256 * sizeof(uint32_t));
    if (!ce || !ce->cells) return;

    const uint32_t n = ce->width * ce->height;
    for (uint32_t i = 0; i < n; i++) {
        const ImgCECell* c = &ce->cells[i];
        uint8_t v;
        switch (channel) {
            case IMG_CE_CHANNEL_LINK:     v = c->link;     break;
            case IMG_CE_CHANNEL_DELTA:    v = c->delta;    break;
            case IMG_CE_CHANNEL_PRIORITY: v = c->priority; break;
            case IMG_CE_CHANNEL_CORE:
            default:                       v = c->core;     break;
        }
        out[v]++;
    }
}
