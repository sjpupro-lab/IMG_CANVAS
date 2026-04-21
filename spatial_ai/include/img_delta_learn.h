#ifndef IMG_DELTA_LEARN_H
#define IMG_DELTA_LEARN_H

#include "img_ce.h"
#include "img_delta_memory.h"

/*
 * img_delta_learn — observe a before/after pair and populate a
 * DeltaMemory with the transitions that explain the change.
 *
 *   before_ce, after_ce  (same dimensions)
 *      │
 *      ▼  for each cell where state_key_before ≠ state_key_after
 *   pre_key  = state_key_from_cell(before)
 *   payload  = derive_delta(before, after)       // one dominant axis
 *   post_hint = state_key_from_cell(after)
 *      │
 *      ▼
 *   memory.add(pre_key, payload, post_hint)
 *
 * derive_delta picks ONE axis to describe the transition, in this
 * priority order:
 *
 *   1. semantic_role change   → MODE_ROLE with role_target_on
 *   2. direction_class change → MODE_DIRECTION (±1 per apply)
 *   3. depth_class change     → MODE_DEPTH     (±1 per apply)
 *   4. largest |Δ| among core/link/delta/priority → matching MODE
 *
 * Numeric deltas below a small noise floor are skipped so image
 * compression jitter doesn't pollute the memory. Tag-level changes
 * are always captured.
 *
 * Result: memory.count grows, and subsequent img_pipeline_run calls
 * will have non-zero `expansions` when the pipeline encounters
 * cells whose state_key matches a stored pre_key (or falls back to
 * a wider key via the existing fallback chain).
 */

/* Walk both grids cell-by-cell. Returns the number of DeltaUnits
 * inserted into `memory`. Requires before and after to have
 * matching dimensions. */
uint32_t img_delta_memory_learn_from_pair(ImgDeltaMemory* memory,
                                          const ImgCEGrid* before,
                                          const ImgCEGrid* after);

/* Convenience: compress two RGB images through SmallCanvas → CE
 * and delegate to learn_from_pair. Image dimensions may differ
 * between before and after (both collapse to the same CE size),
 * but each image must be non-empty. Returns the number of
 * DeltaUnits inserted. */
uint32_t img_delta_memory_learn_from_images(ImgDeltaMemory* memory,
                                            const uint8_t* before_rgb,
                                            uint32_t before_w,
                                            uint32_t before_h,
                                            const uint8_t* after_rgb,
                                            uint32_t after_w,
                                            uint32_t after_h);

/* Multi-scale learn from a single image.
 *
 *   Blur cascade: for each radius in `blur_radii` (sorted largest
 *   first = coarsest first), produce a box-blurred copy. Consecutive
 *   pairs are used as (before=coarser, after=finer):
 *
 *     blur_radii = {16, 4, 0}
 *       → pair A: before = blur(16), after = blur(4)   // coarse→mid
 *       → pair B: before = blur(4),  after = blur(0)   // mid→fine
 *
 *   Each pair runs learn_from_images, so deltas capture the detail
 *   added at that step of the cascade. The rarity sieve in
 *   img_delta_memory_add naturally places first-seen patterns at
 *   high weight, so coarse-only signals (present early, absent
 *   later) and fine-only signals (late-only) end up in different
 *   weight buckets — the engine's "detail-per-region" axis is
 *   derived for free from the blur cascade.
 *
 *   `blur_radii` must have ≥ 2 entries and be sorted descending
 *   (largest radius first). Returns total DeltaUnits inserted.
 *   Zero-sized images or < 2 radii return 0 with no side effects.
 */
uint32_t img_delta_memory_learn_multiscale(ImgDeltaMemory* memory,
                                           const uint8_t* image_rgb,
                                           uint32_t image_w,
                                           uint32_t image_h,
                                           const uint32_t* blur_radii,
                                           uint32_t n_radii);

#endif /* IMG_DELTA_LEARN_H */
