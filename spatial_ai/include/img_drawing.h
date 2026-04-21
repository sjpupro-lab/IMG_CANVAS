#ifndef IMG_DRAWING_H
#define IMG_DRAWING_H

#include "img_ce.h"
#include "img_delta_memory.h"

/* Forward declarations so drawing callers can opt into a learned
 * prior without forcing a compile-time dependency on the noise
 * memory module. Full definitions live in img_noise_memory.h. */
struct ImgNoiseMemory;
struct ImgNoiseSampleOptions;

/*
 * img_drawing — "print-the-image" operating mode.
 *
 *   This is the counterpart to img_pipeline_run (which ingests an
 *   input image and compresses it to CE state). img_drawing_pass
 *   operates the engine in the other direction: given a CE grid
 *   (empty, seeded from a keyframe, or mid-draw) plus a populated
 *   DeltaMemory, walk the cells and stamp learned deltas onto them.
 *
 *   Selection model — top-G sampling with presence penalty, lifted
 *   directly from language-model token sampling:
 *
 *     for each target cell (raster order):
 *         topg = img_delta_memory_topg(
 *                   cell, G,
 *                   recent_counts, penalty_alpha, ...);
 *         pick  = topg[0];                 (greedy; penalty diversifies)
 *         img_delta_apply(cell, memory, pick);
 *         recent_counts[pick->id] += 1;
 *
 *   Presence penalty is analogous to coverage / presence penalties
 *   in LM sampling: a delta picked recently drops in score so under-
 *   used candidates surface. This is what keeps the drawing from
 *   collapsing into "stamp the same thing everywhere".
 *
 *   The engine's core model of "same architecture, drawing mode":
 *     - No noise-denoising loop.
 *     - Delta memory holds bounded, lossless resume codes.
 *     - Per-cell tier / role / depth already encode how much detail
 *       belongs where; the stamp does not need a global mode
 *       switch.
 *     - Multiple passes over the grid produce underdrawing →
 *       detail layering (earlier passes stamp low-tier deltas,
 *       later passes pick up finer-tier ones as cell state changes).
 */

typedef struct {
    uint32_t top_g;              /* candidate pool per cell (default 3) */
    double   presence_penalty;   /* α; subtracted per recent pick; default 0.5 */
    uint32_t passes;             /* drawing iterations over the grid; default 1 */
    int      skip_zero_cells;    /* 1 = skip cells with core==0 (seed-only focus);
                                  * 0 = stamp every cell (full blank-canvas fill) */

    /* ── Brush controls (all optional) ────────────────────
     *
     * Together these turn drawing_pass into a region-aware brush.
     * A single delta memory can produce radically different output
     * depending on the brush: "paint the face region at tier-3 detail"
     * vs. "paint the background at tier-1" — same engine, same
     * memory, different tier / role / mask dials. */

    /* Per-cell gate: cells where region_mask[i] == 0 are skipped
     * entirely (not visited, no stamp). Expected size
     * IMG_CE_TOTAL bytes. NULL = "no mask" = every cell eligible. */
    const uint8_t* region_mask;

    /* Preferred payload tier for this pass. 0 = no preference.
     * Non-zero values add `tier_bonus` to the score of every top-G
     * candidate whose payload.state's tier_idx matches, then the
     * best post-bonus candidate is picked. */
    uint8_t  target_tier;        /* 0 or IMG_TIER_T1 / T2 / T3 */
    double   tier_bonus;         /* default 0.25 when brush active */

    /* Preferred cell semantic_role. 0 = no preference.
     * When non-zero, candidates whose pre_key.semantic_role matches
     * gain `role_bonus`. Useful for painting a face region with
     * deltas learned on face cells. */
    uint8_t  target_role;        /* 0 or IMG_ROLE_* */
    double   role_bonus;         /* default 0.20 when brush active */
} ImgDrawingOptions;

typedef struct {
    uint32_t stamps_applied;     /* # of successful img_delta_apply calls */
    uint32_t cells_visited;      /* cells we considered stamping (post-filter) */
    uint32_t cells_masked_out;   /* cells skipped by region_mask */
    uint32_t brush_bonus_wins;   /* picks where tier/role bonus changed the winner */
    uint32_t unique_deltas_used; /* distinct delta ids picked at least once */
    uint32_t max_recent_count;   /* highest value in the recent_counts table */
} ImgDrawingStats;

ImgDrawingOptions img_drawing_default_options(void);

/* Run a drawing pass on `grid` using `memory`. If either is NULL the
 * call is a no-op that reports zero stats. Internally allocates a
 * recent_counts table the size of img_delta_memory_count(memory) for
 * the duration of the call. Runs `opts.passes` iterations; within a
 * single pass, `recent_counts` accumulates (diversifies selection);
 * it is NOT cleared between passes.
 *
 * Returns 1 on success (including the no-op path), 0 on allocation
 * failure. */
int img_drawing_pass(ImgCEGrid* grid,
                     ImgDeltaMemory* memory,
                     const ImgDrawingOptions* opts_or_null,
                     ImgDrawingStats* out_stats_or_null);

/* Wrapper: optionally sample a learned prior into `grid` first, then
 * run the standard drawing pass. If either `noise_memory` or
 * `noise_opts` is NULL the prior step is skipped and behaviour is
 * bit-identical to img_drawing_pass(grid, memory, draw_opts, ...).
 *
 * Returns 1 on success (including the no-op path), 0 if the prior
 * sampling step reported a failure. */
int img_drawing_pass_with_prior(ImgCEGrid*                          grid,
                                ImgDeltaMemory*                     memory,
                                const struct ImgNoiseMemory*        noise_memory,
                                const struct ImgNoiseSampleOptions* noise_opts,
                                const ImgDrawingOptions*            draw_opts,
                                ImgDrawingStats*                    out_stats);

/* Convenience: fill `mask` (size IMG_CE_TOTAL) with 1s inside the
 * rectangle [x0, x1) × [y0, y1) and 0s elsewhere. Bounds are
 * clamped to the grid. `mask` must be pre-allocated by the caller. */
void img_brush_mask_rect(uint8_t* mask,
                         uint32_t x0, uint32_t y0,
                         uint32_t x1, uint32_t y1);

#endif /* IMG_DRAWING_H */
