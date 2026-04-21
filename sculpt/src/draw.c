#include "sculpt_draw.h"
#include "sculpt_cell.h"
#include "sculpt_chisel.h"
#include "sculpt_prng.h"

#include <string.h>

static int abs_i(int v) { return v < 0 ? -v : v; }

static int neighborhood_coherence(const sculpt_chisel_t *c,
                                    const sculpt_cell_t *cell,
                                    const sculpt_cell_t *neighbors[8])
{
    uint8_t new_r = sculpt_saturate_subtract(cell->depth_r, c->subtract_r);
    uint8_t new_g = sculpt_saturate_subtract(cell->depth_g, c->subtract_g);
    uint8_t new_b = sculpt_saturate_subtract(cell->depth_b, c->subtract_b);
    uint8_t new_a = sculpt_saturate_subtract(cell->depth_a, c->subtract_a);

    int threshold_sum = SCULPT_COHERENCE_THRESHOLD * 4;
    int score = 0;
    for (int i = 0; i < 8; ++i) {
        const sculpt_cell_t *n = neighbors[i];
        int diff = abs_i((int)new_r - n->depth_r)
                 + abs_i((int)new_g - n->depth_g)
                 + abs_i((int)new_b - n->depth_b)
                 + abs_i((int)new_a - n->depth_a);
        if (diff < threshold_sum) score += SCULPT_COHERENCE_BONUS;
        else                      score -= SCULPT_COHERENCE_PENALTY;
    }
    return score;
}

static uint8_t apply_noise_channel(uint8_t cell_depth, uint8_t ideal, int margin,
                                    sculpt_prng_t *rng, int *out_noise)
{
    int noise = 0;
    int actual = ideal;
    if (margin > 0) {
        noise = sculpt_prng_next_in_range(rng, -margin, margin);
        int v = ideal + noise;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        actual = v;
    }
    *out_noise = noise;
    return sculpt_saturate_subtract(cell_depth, actual);
}

static void write_margin(sculpt_cell_t *cell, int level, int margin)
{
    uint8_t m = (uint8_t)(margin > 255 ? 255 : (margin < 0 ? 0 : margin));
    switch (level) {
        case 0: cell->margin_l0 = m; break;
        case 1: cell->margin_l1 = m; break;
        case 2: cell->margin_l2 = m; break;
        case 3: cell->margin_l3 = m; break;
    }
}

/* Shared body of sculpt_draw and sculpt_edit_rect. When rect is NULL, every
 * cell is visited; otherwise only cells inside the rect are mutated. PRNG
 * seeding is independent of rect (derive_seed uses cell_id), so a full-canvas
 * edit_rect is bitwise-equivalent to draw on a fresh grid.
 */
static void draw_loop(const sculpt_library_t *lib,
                       uint64_t master_seed,
                       const int *iter_counts,
                       const sculpt_rect_t *rect,
                       sculpt_grid_t *grid,
                       sculpt_edit_log_entry_t *log,
                       int log_capacity,
                       int *log_written,
                       sculpt_draw_stats_t *stats)
{
    int log_n = 0;
    if (stats) memset(stats, 0, sizeof(*stats));

    int rx0 = 0, ry0 = 0, rx1 = grid->size, ry1 = grid->size;
    if (rect) {
        rx0 = rect->x; ry0 = rect->y;
        rx1 = rect->x + rect->w; ry1 = rect->y + rect->h;
        if (rx0 < 0) rx0 = 0;
        if (ry0 < 0) ry0 = 0;
        if (rx1 > grid->size) rx1 = grid->size;
        if (ry1 > grid->size) ry1 = grid->size;
    }

    const int order[SCULPT_NUM_LEVELS] = { 3, 2, 1, 0 };

    for (int oi = 0; oi < SCULPT_NUM_LEVELS; ++oi) {
        int level = order[oi];
        int margin = SCULPT_LEVEL_MARGIN[level];
        int top_g = SCULPT_TOP_G[level];
        if (top_g > 8) top_g = 8;

        for (int iter_idx = 0; iter_idx < iter_counts[level]; ++iter_idx) {
            for (int y = ry0; y < ry1; ++y) {
                for (int x = rx0; x < rx1; ++x) {
                    sculpt_cell_t *cell = sculpt_grid_at(grid, x, y);
                    const sculpt_cell_t *neighbors[8];
                    sculpt_grid_neighbor_8(grid, x, y, neighbors);

                    sculpt_neighbor_key_t key;
                    sculpt_neighbor_key_build(cell, neighbors, &key);

                    const sculpt_chisel_t *cands[8] = {0};
                    int found = sculpt_library_lookup(lib, level, &key, top_g, cands);
                    if (found == 0) continue;

                    const sculpt_chisel_t *best = NULL;
                    int best_score = -1000000000;
                    for (int i = 0; i < found; ++i) {
                        int s = neighborhood_coherence(cands[i], cell, neighbors)
                              + (int)cands[i]->weight;
                        if (s > best_score) {
                            best_score = s;
                            best = cands[i];
                        }
                    }
                    if (!best) continue;

                    int cell_id = y * grid->size + x;
                    sculpt_prng_t rng;
                    sculpt_prng_seed(&rng, sculpt_derive_seed(master_seed, level, iter_idx, cell_id));

                    int n_r, n_g, n_b, n_a;
                    cell->depth_r = apply_noise_channel(cell->depth_r, best->subtract_r, margin, &rng, &n_r);
                    cell->depth_g = apply_noise_channel(cell->depth_g, best->subtract_g, margin, &rng, &n_g);
                    cell->depth_b = apply_noise_channel(cell->depth_b, best->subtract_b, margin, &rng, &n_b);
                    cell->depth_a = apply_noise_channel(cell->depth_a, best->subtract_a, margin, &rng, &n_a);

                    cell->last_chisel_id = best->chisel_id;
                    write_margin(cell, level, margin);

                    if (log && log_n < log_capacity) {
                        log[log_n].level = (int8_t)level;
                        log[log_n].iter_idx = (int8_t)iter_idx;
                        log[log_n].cell_id = (int16_t)cell_id;
                        log[log_n].chisel_id = best->chisel_id;
                        log[log_n].noise_xor = (n_r & 0xFFFF) ^ (n_g & 0xFFFF)
                                            ^ (n_b & 0xFFFF) ^ (n_a & 0xFFFF);
                        ++log_n;
                    }
                    if (stats) {
                        stats->score_sum[level] += best_score;
                        stats->score_count[level] += 1;
                    }
                }
            }
        }
    }

    if (log_written) *log_written = log_n;
}

void sculpt_draw(const sculpt_library_t *lib,
                 uint64_t master_seed,
                 const int iters[SCULPT_NUM_LEVELS],
                 sculpt_grid_t *grid,
                 sculpt_edit_log_entry_t *log,
                 int log_capacity,
                 int *log_written,
                 sculpt_draw_stats_t *stats)
{
    const int *iter_counts = iters ? iters : SCULPT_DEFAULT_ITERS;
    draw_loop(lib, master_seed, iter_counts, NULL, grid, log, log_capacity, log_written, stats);
}

void sculpt_edit_rect(const sculpt_library_t *lib,
                       uint64_t master_seed,
                       const int iters[SCULPT_NUM_LEVELS],
                       sculpt_rect_t rect,
                       sculpt_grid_t *grid,
                       sculpt_edit_log_entry_t *log,
                       int log_capacity,
                       int *log_written,
                       sculpt_draw_stats_t *stats)
{
    const int *iter_counts = iters ? iters : SCULPT_DEFAULT_ITERS;
    draw_loop(lib, master_seed, iter_counts, &rect, grid, log, log_capacity, log_written, stats);
}

int sculpt_replay(sculpt_grid_t *grid,
                   const sculpt_library_t *lib,
                   uint64_t master_seed,
                   const sculpt_edit_log_entry_t *log,
                   int log_count)
{
    for (int i = 0; i < log_count; ++i) {
        const sculpt_edit_log_entry_t *e = &log[i];
        const sculpt_chisel_t *c = sculpt_library_get_by_id(lib, e->chisel_id);
        if (!c) return 1;

        int cell_id = e->cell_id;
        int x = cell_id % grid->size;
        int y = cell_id / grid->size;
        sculpt_cell_t *cell = sculpt_grid_at(grid, x, y);

        int margin = SCULPT_LEVEL_MARGIN[e->level];
        sculpt_prng_t rng;
        sculpt_prng_seed(&rng, sculpt_derive_seed(master_seed, e->level, e->iter_idx, cell_id));

        int n_r, n_g, n_b, n_a;
        cell->depth_r = apply_noise_channel(cell->depth_r, c->subtract_r, margin, &rng, &n_r);
        cell->depth_g = apply_noise_channel(cell->depth_g, c->subtract_g, margin, &rng, &n_g);
        cell->depth_b = apply_noise_channel(cell->depth_b, c->subtract_b, margin, &rng, &n_b);
        cell->depth_a = apply_noise_channel(cell->depth_a, c->subtract_a, margin, &rng, &n_a);

        cell->last_chisel_id = c->chisel_id;
        write_margin(cell, e->level, margin);
    }
    return 0;
}
