#ifndef IMG_PIPELINE_H
#define IMG_PIPELINE_H

#include "img_ce.h"
#include "img_delta_memory.h"

/*
 * img_pipeline — SPEC §14 execution pipeline.
 *
 *   Input (RGB bytes)
 *     → SmallCanvas              (§14 / interpretation)
 *     → CE grid                  (§14 / state compression)
 *     → Seed selection           (§14.1 — top-priority cells)
 *     → CE Expansion (Delta)     (§14.2 — frontier-driven delta apply)
 *     → Resolve                  (§14.4 — sieve + repair)
 *     → (caller renders via img_render)
 *
 *   Seeds: top-K cells by priority, K = seed_fraction × IMG_CE_TOTAL.
 *   Expansion: BFS — for each frontier cell, apply the best delta
 *   from memory (if provided), then enqueue its 4 unvisited neighbours.
 *   Expansion runs for at most `expansion_steps` BFS rounds, or until
 *   the frontier empties.
 *
 *   If `memory` is NULL the expansion walk still happens (seeds and
 *   frontier still expand) but no deltas are applied — useful for
 *   measuring baseline behaviour without any learned rules.
 */

typedef struct {
    float    seed_fraction;      /* SPEC §14.1: 0.01..0.05 recommended */
    uint32_t expansion_steps;    /* max BFS rounds (0 disables expand) */
    uint32_t frontier_max;       /* cap per BFS round */
    int      resolve_threshold;  /* core-diff threshold for §14.4     */
    int      feedback;           /* 1 → after resolve, auto-ingest each
                                  *     applied delta's outcome into
                                  *     memory usage/success (default 1)
                                  * 0 → skip the ingest step */
} ImgPipelineOptions;

typedef struct {
    uint32_t seed_count;         /* how many cells seeded the frontier */
    uint32_t expansions;         /* # of delta apply events */
    uint32_t visited;            /* # of cells touched by the BFS */
    uint32_t resolve_outliers;
    uint32_t resolve_explained;
    uint32_t resolve_promoted;
    uint32_t feedback_success;   /* deltas credited as success */
    uint32_t feedback_failure;   /* deltas credited as failure */
} ImgPipelineStats;

typedef struct {
    ImgSmallCanvas*  small_canvas;
    ImgCEGrid*       ce_grid;

    /* Per-cell resolve masks, each IMG_CE_TOTAL bytes. Populated by
     * img_pipeline_run and owned by the result. Can be fed directly
     * into img_render_ce_grid_masked via an ImgRenderMasks struct. */
    uint8_t*         outlier_mask;
    uint8_t*         explained_mask;

    ImgPipelineStats stats;
} ImgPipelineResult;

/* Defaults:
 *   seed_fraction    = 0.03   (3% of IMG_CE_TOTAL)
 *   expansion_steps  = 4
 *   frontier_max     = 1024
 *   resolve_threshold = 40
 */
ImgPipelineOptions img_pipeline_default_options(void);

/* Run the full pipeline. Returns 1 on success, 0 on allocation /
 * argument failure. On success the caller owns the pointers inside
 * `out` and must call img_pipeline_result_destroy.
 *
 * `memory` may be NULL. */
int  img_pipeline_run(const uint8_t* image_rgb,
                      uint32_t image_w, uint32_t image_h,
                      ImgDeltaMemory* memory,
                      const ImgPipelineOptions* opt_or_null,
                      ImgPipelineResult* out);

/* Releases SmallCanvas and CE grid and zeroes the struct. Safe to
 * call on a zero-initialised result. */
void img_pipeline_result_destroy(ImgPipelineResult* r);

#endif /* IMG_PIPELINE_H */
