#ifndef SCULPT_DRAW_H
#define SCULPT_DRAW_H

#include <stdint.h>
#include "sculpt_grid.h"
#include "sculpt_library.h"
#include "sculpt_tuning.h"

/* Drawing pipe (DESIGN.md §5.2, §6). For each level 3->0 and each cell, pick
 * the best chisel (coherence + weight), apply per-channel subtract with
 * deterministic PRNG noise. P1 guarantees subtractive-only mutations via
 * sculpt_saturate_subtract.
 */

typedef struct {
    int8_t   level;
    int8_t   iter_idx;
    int16_t  cell_id;
    uint32_t chisel_id;
    int32_t  noise_xor;
} sculpt_edit_log_entry_t;

typedef struct {
    long long score_sum[SCULPT_NUM_LEVELS];
    int       score_count[SCULPT_NUM_LEVELS];
} sculpt_draw_stats_t;

/* Draw onto `grid` (caller provides initialized grid, typically all-zero).
 * `iters[l]` is the number of sweeps for level l; if NULL, defaults are used.
 * `log` / `log_capacity` are optional (NULL to skip logging). On return,
 * `*log_written` holds the number of entries written.
 * `stats` is optional.
 *
 * Determinism guarantee (Phase 4 success criterion): same library, same
 * master_seed, same iters => bitwise-identical grid contents.
 */
void sculpt_draw(const sculpt_library_t *lib,
                 uint64_t master_seed,
                 const int iters[SCULPT_NUM_LEVELS],
                 sculpt_grid_t *grid,
                 sculpt_edit_log_entry_t *log,
                 int log_capacity,
                 int *log_written,
                 sculpt_draw_stats_t *stats);

/* Axis-aligned rect in grid coordinates. Inclusive of (x,y); extends w cells
 * right and h cells down. Cells outside the rect are never touched.
 */
typedef struct {
    int16_t x, y, w, h;
} sculpt_rect_t;

/* Phase 5: re-run the drawing loop but only mutate cells inside `rect`.
 * The library, seed, and iter schedule determine PRNG seeding exactly as in
 * sculpt_draw, so an edit_rect on the full canvas is bitwise equivalent to
 * sculpt_draw on a fresh grid. Out-of-rect cells keep their current depth.
 */
void sculpt_edit_rect(const sculpt_library_t *lib,
                       uint64_t master_seed,
                       const int iters[SCULPT_NUM_LEVELS],
                       sculpt_rect_t rect,
                       sculpt_grid_t *grid,
                       sculpt_edit_log_entry_t *log,
                       int log_capacity,
                       int *log_written,
                       sculpt_draw_stats_t *stats);

/* Replay a previously captured edit log onto `grid`. Each entry names a
 * (level, iter_idx, cell_id, chisel_id); the chisel is looked up in `lib`
 * and applied with the same derive_seed-based PRNG stream as the original
 * draw, so replay is bitwise-identical to the original mutations — assuming
 * the library still contains the chisel_ids referenced.
 *
 * Returns 0 on success. Non-zero if a chisel_id is missing from `lib`.
 */
int sculpt_replay(sculpt_grid_t *grid,
                   const sculpt_library_t *lib,
                   uint64_t master_seed,
                   const sculpt_edit_log_entry_t *log,
                   int log_count);

#endif /* SCULPT_DRAW_H */
