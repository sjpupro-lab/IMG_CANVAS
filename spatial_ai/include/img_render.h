#ifndef IMG_RENDER_H
#define IMG_RENDER_H

#include "img_ce.h"

/*
 * img_render — CE grid → raster bridge (SPEC §7–8: SlotShape).
 *
 *   Engine stays unchanged. ImgCEGrid is pure state; this module is
 *   the read-only emission layer that projects each cell's channels
 *   onto a small spatial pattern inside its rendered block.
 *
 *   Slot shape v0 (per cell):
 *     R  core      → center square         (primary mass)
 *     G  link      → cross (row/col mid)   (connectivity)
 *     B  delta     → 4 corners             (mutability)
 *     A  priority  → border / ring         (authority)
 *
 *   Tier system (SPEC §10): each channel's value is classified into
 *   tiers 0..4 against a {t1_max, t2_max, t3_max} spec. Higher tiers
 *   draw brighter AND spill slightly outside their primary mask, so
 *   a saturated channel bleeds into neighbouring zones.
 *
 *   Zero float, saturating uint8 math, no engine writes.
 */

typedef struct {
    uint8_t t1_max;   /* value ≤ t1_max  → tier 1 */
    uint8_t t2_max;   /* value ≤ t2_max  → tier 2 */
    uint8_t t3_max;   /* value ≤ t3_max  → tier 3 */
    /* else → tier 4 (saturated, bleeds outside primary mask) */
} ImgRenderTierSpec;

typedef struct {
    uint8_t           cell_px;        /* how many output pixels per CE cell; ≥ 2 */
    ImgRenderTierSpec tier_core;      /* thresholds for R/core */
    ImgRenderTierSpec tier_link;      /* G/link */
    ImgRenderTierSpec tier_delta;     /* B/delta */
    ImgRenderTierSpec tier_priority;  /* A/priority */
} ImgRenderOptions;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t* rgb;     /* width × height × 3 bytes; free with img_render_free_image */
} ImgRenderImage;

/* Sensible defaults: cell_px = 4 (CE 64×64 → 256×256 image),
 * tier thresholds sourced from IMG_TIER_TABLE (range_max per tier)
 * on every channel. */
ImgRenderOptions img_render_default_options(void);

/* Adapt the per-channel tier specs in `opts` to the value
 * distribution of each CE channel (core / link / delta / priority).
 * For every channel, builds a histogram, runs img_tier_adapt, and
 * writes the resulting {t1_max, t2_max, t3_max} back into the
 * matching ImgRenderTierSpec. scale_factor is not used here — this
 * function only changes classification thresholds used at render
 * time. cell_px is untouched. */
void img_render_options_adapt_to_ce(ImgRenderOptions* opts,
                                    const ImgCEGrid* ce);

/* Optional per-cell resolve masks. Both arrays are IMG_CE_TOTAL bytes
 * with 0/1 flags; either pointer may be NULL (that flag is ignored).
 *
 *   outlier==1 && explained==1 → absorbed / repaired  (cyan tint)
 *   outlier==1 && explained==0 → promoted / unresolved (red tint)
 *   else                       → no overlay
 *
 * Matches the "explained / survivor" visualization from the reference
 * design, driven by img_ce_resolve's outputs. */
typedef struct {
    const uint8_t* outlier;
    const uint8_t* explained;
} ImgRenderMasks;

/* Render a CE grid to RGB. Returns 1 on success, 0 on failure.
 * out_img->rgb is malloc'd — release with img_render_free_image. */
int  img_render_ce_grid(const ImgCEGrid* ce,
                        const ImgRenderOptions* opt_or_null,
                        ImgRenderImage* out_img);

/* Same as img_render_ce_grid but applies a per-cell mask overlay.
 * `masks` may be NULL (equivalent to img_render_ce_grid). */
int  img_render_ce_grid_masked(const ImgCEGrid* ce,
                               const ImgRenderOptions* opt_or_null,
                               const ImgRenderMasks* masks_or_null,
                               ImgRenderImage* out_img);

/* Save the image as binary PPM (P6). Zero-dependency, universally
 * decodable — useful for visual regression fixtures. */
int  img_render_save_ppm(const char* path, const ImgRenderImage* img);

/* Release the malloc'd pixel buffer and zero the struct. */
void img_render_free_image(ImgRenderImage* img);

#endif /* IMG_RENDER_H */
