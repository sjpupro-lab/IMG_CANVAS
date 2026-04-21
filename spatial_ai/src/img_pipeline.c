#include "img_pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── defaults ───────────────────────────────────────────── */

ImgPipelineOptions img_pipeline_default_options(void) {
    ImgPipelineOptions o;
    o.seed_fraction     = 0.03f;    /* mid of SPEC's 1..5% */
    o.expansion_steps   = 4;
    o.frontier_max      = 1024;
    o.resolve_threshold = 40;
    o.feedback          = 1;        /* auto-ingest resolve outcomes */
    return o;
}

/* Defensive dim bounds for the ingest path.
 *   IMG_PIPELINE_MIN_DIM guards against degenerate 1×1 inputs that
 *     produce a uniform CE grid and waste a keyframe slot.
 *   IMG_PIPELINE_MAX_DIM guards against arbitrarily large uploads
 *     (satellite imagery, 40k×40k dumps) that would make the
 *     block-averaging loop dominate ingest time. 16384 covers 4K /
 *     8K photos with headroom; oversize inputs are rejected so the
 *     caller can downscale explicitly.
 * `img_image_to_small_canvas` is safe for any dim — these bounds are
 * policy at the pipeline boundary, not correctness. */
#define IMG_PIPELINE_MIN_DIM  16u
#define IMG_PIPELINE_MAX_DIM  16384u

/* ── seed selection: top-K by priority, raster-order tiebreak ── */

typedef struct {
    uint8_t  pri;
    uint16_t idx;
} SeedEntry;

static int cmp_seed_desc(const void* a, const void* b) {
    const SeedEntry* aa = (const SeedEntry*)a;
    const SeedEntry* bb = (const SeedEntry*)b;
    /* priority descending, idx ascending (deterministic tiebreak) */
    if (aa->pri != bb->pri) return (int)bb->pri - (int)aa->pri;
    return (int)aa->idx - (int)bb->idx;
}

/* ── main run ───────────────────────────────────────────── */

int img_pipeline_run(const uint8_t* image_rgb,
                     uint32_t image_w, uint32_t image_h,
                     ImgDeltaMemory* memory,
                     const ImgPipelineOptions* opt_or_null,
                     ImgPipelineResult* out) {
    if (!image_rgb || !out)         return 0;
    if (image_w == 0 || image_h == 0) return 0;

    if (image_w < IMG_PIPELINE_MIN_DIM || image_h < IMG_PIPELINE_MIN_DIM ||
        image_w > IMG_PIPELINE_MAX_DIM || image_h > IMG_PIPELINE_MAX_DIM) {
        fprintf(stderr,
                "[img_pipeline] reject %ux%u: outside [%u..%u] per side\n",
                image_w, image_h,
                IMG_PIPELINE_MIN_DIM, IMG_PIPELINE_MAX_DIM);
        return 0;
    }

    memset(out, 0, sizeof(*out));

    ImgPipelineOptions opt = opt_or_null ? *opt_or_null
                                         : img_pipeline_default_options();
    if (opt.seed_fraction < 0.0f) opt.seed_fraction = 0.0f;
    if (opt.seed_fraction > 1.0f) opt.seed_fraction = 1.0f;

    /* Stage 1: image → SmallCanvas. */
    ImgSmallCanvas* sc = img_small_canvas_create();
    if (!sc) return 0;
    img_image_to_small_canvas(image_rgb, image_w, image_h, sc);

    /* Stage 2: SmallCanvas → CE grid. */
    ImgCEGrid* ce = img_ce_grid_create();
    if (!ce) { img_small_canvas_destroy(sc); return 0; }
    img_small_canvas_to_ce(sc, ce);

    /* Stage 3: seed selection by priority. */
    SeedEntry*  entries       = (SeedEntry*)malloc(sizeof(SeedEntry) * IMG_CE_TOTAL);
    uint8_t*    visited       = (uint8_t*  )calloc(IMG_CE_TOTAL, 1);
    uint16_t*   frontier      = (uint16_t*)malloc(sizeof(uint16_t)  * IMG_CE_TOTAL);
    uint16_t*   next_frontier = (uint16_t*)malloc(sizeof(uint16_t)  * IMG_CE_TOTAL);
    if (!entries || !visited || !frontier || !next_frontier) {
        free(entries); free(visited); free(frontier); free(next_frontier);
        img_small_canvas_destroy(sc); img_ce_grid_destroy(ce);
        return 0;
    }

    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        entries[i].pri = ce->cells[i].priority;
        entries[i].idx = (uint16_t)i;
    }
    qsort(entries, IMG_CE_TOTAL, sizeof(SeedEntry), cmp_seed_desc);

    uint32_t target = (uint32_t)(opt.seed_fraction * (float)IMG_CE_TOTAL);
    if (target > IMG_CE_TOTAL) target = IMG_CE_TOTAL;

    uint32_t frontier_size = 0;
    for (uint32_t i = 0; i < target; i++) {
        const uint16_t idx = entries[i].idx;
        frontier[frontier_size++] = idx;
        visited[idx]              = 1;
    }
    const uint32_t seed_count = frontier_size;
    uint32_t       expansions = 0;

    /* Stage 4: BFS expansion with delta apply. */
    for (uint32_t step = 0;
         step < opt.expansion_steps && frontier_size > 0;
         step++) {

        uint32_t next_size = 0;
        for (uint32_t i = 0; i < frontier_size; i++) {
            const uint16_t cidx = frontier[i];
            ImgCECell*     cell = &ce->cells[cidx];

            if (memory) {
                double score = 0.0;
                int    level = -1;
                const ImgDeltaUnit* best =
                    img_delta_memory_best(memory, cell, &score, &level);
                if (best) {
                    img_delta_apply(cell, memory, best);
                    expansions++;
                }
            }

            const uint32_t y = cidx / IMG_CE_SIZE;
            const uint32_t x = cidx % IMG_CE_SIZE;

            /* 4-neighbor enqueue, guarded by grid bounds, visited, and
             * the frontier_max cap per round. */
            #define TRY_ENQ(ny, nx) do {                                      \
                if ((ny) < IMG_CE_SIZE && (nx) < IMG_CE_SIZE) {               \
                    uint32_t _nidx = (uint32_t)((ny) * IMG_CE_SIZE + (nx));   \
                    if (!visited[_nidx] && next_size < opt.frontier_max       \
                        && next_size < IMG_CE_TOTAL) {                        \
                        next_frontier[next_size++] = (uint16_t)_nidx;         \
                        visited[_nidx] = 1;                                   \
                    }                                                          \
                }                                                              \
            } while (0)

            if (y > 0)                    TRY_ENQ(y - 1, x);
            if (y + 1 < IMG_CE_SIZE)      TRY_ENQ(y + 1, x);
            if (x > 0)                    TRY_ENQ(y, x - 1);
            if (x + 1 < IMG_CE_SIZE)      TRY_ENQ(y, x + 1);

            #undef TRY_ENQ
        }

        /* swap */
        uint16_t* tmp = frontier;
        frontier      = next_frontier;
        next_frontier = tmp;
        frontier_size = next_size;
    }

    uint32_t visited_count = 0;
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        if (visited[i]) visited_count++;
    }

    free(entries);
    free(visited);
    free(frontier);
    free(next_frontier);

    /* Stage 5: resolve with per-cell masks retained in the result. */
    uint8_t* outlier_mask   = (uint8_t*)calloc(IMG_CE_TOTAL, 1);
    uint8_t* explained_mask = (uint8_t*)calloc(IMG_CE_TOTAL, 1);
    if (!outlier_mask || !explained_mask) {
        free(outlier_mask); free(explained_mask);
        img_small_canvas_destroy(sc); img_ce_grid_destroy(ce);
        return 0;
    }

    ImgResolveResult rr = {0, 0, 0, 0};
    img_ce_resolve(ce, opt.resolve_threshold,
                   outlier_mask, explained_mask, &rr);

    /* Stage 6: auto-feedback — credit each applied delta's outcome. */
    ImgDeltaFeedbackStats fb = {0, 0, 0};
    if (memory && opt.feedback) {
        img_delta_memory_ingest_resolve(memory, ce,
                                        outlier_mask, explained_mask,
                                        &fb);
    }

    out->small_canvas             = sc;
    out->ce_grid                  = ce;
    out->outlier_mask             = outlier_mask;
    out->explained_mask           = explained_mask;
    out->stats.seed_count         = seed_count;
    out->stats.expansions         = expansions;
    out->stats.visited            = visited_count;
    out->stats.resolve_outliers   = rr.outlier_count;
    out->stats.resolve_explained  = rr.explained_count;
    out->stats.resolve_promoted   = rr.promoted_count;
    out->stats.feedback_success   = fb.credited_success;
    out->stats.feedback_failure   = fb.credited_failure;

    return 1;
}

void img_pipeline_result_destroy(ImgPipelineResult* r) {
    if (!r) return;
    if (r->small_canvas)   img_small_canvas_destroy(r->small_canvas);
    if (r->ce_grid)        img_ce_grid_destroy(r->ce_grid);
    if (r->outlier_mask)   free(r->outlier_mask);
    if (r->explained_mask) free(r->explained_mask);
    r->small_canvas   = NULL;
    r->ce_grid        = NULL;
    r->outlier_mask   = NULL;
    r->explained_mask = NULL;
    memset(&r->stats, 0, sizeof(r->stats));
}
