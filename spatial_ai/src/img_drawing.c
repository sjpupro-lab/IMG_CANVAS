#include "img_drawing.h"
#include "img_noise_memory.h"

#include <stdlib.h>
#include <string.h>

ImgDrawingOptions img_drawing_default_options(void) {
    ImgDrawingOptions o;
    o.top_g            = 3;
    o.presence_penalty = 0.5;
    o.passes           = 1;
    o.skip_zero_cells  = 0;

    o.region_mask  = NULL;
    o.target_tier  = 0;       /* no tier preference */
    o.tier_bonus   = 0.25;
    o.target_role  = 0;       /* no role preference */
    o.role_bonus   = 0.20;
    return o;
}

void img_brush_mask_rect(uint8_t* mask,
                         uint32_t x0, uint32_t y0,
                         uint32_t x1, uint32_t y1) {
    if (!mask) return;
    memset(mask, 0, IMG_CE_TOTAL);
    if (x1 > IMG_CE_SIZE) x1 = IMG_CE_SIZE;
    if (y1 > IMG_CE_SIZE) y1 = IMG_CE_SIZE;
    if (x0 >= x1 || y0 >= y1) return;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            mask[img_ce_idx(y, x)] = 1;
        }
    }
}

int img_drawing_pass(ImgCEGrid* grid,
                     ImgDeltaMemory* memory,
                     const ImgDrawingOptions* opts_or_null,
                     ImgDrawingStats* out_stats) {
    ImgDrawingStats local = {0, 0, 0, 0, 0, 0};

    if (!grid || !grid->cells || !memory ||
        img_delta_memory_count(memory) == 0) {
        if (out_stats) *out_stats = local;
        return 1;   /* no-op success */
    }

    ImgDrawingOptions opt = opts_or_null ? *opts_or_null
                                         : img_drawing_default_options();
    if (opt.top_g == 0) opt.top_g = 1;
    if (opt.passes == 0) opt.passes = 1;

    const uint32_t mem_count = img_delta_memory_count(memory);
    uint32_t* recent_counts = (uint32_t*)calloc(mem_count, sizeof(uint32_t));
    uint8_t*  picked_any    = (uint8_t*) calloc(mem_count, 1);
    if (!recent_counts || !picked_any) {
        free(recent_counts); free(picked_any);
        return 0;
    }

    /* Candidate buffer sized at the largest G we accept. 16 keeps us
     * within img_delta_memory_topg's 32-candidate scratch cap. */
    enum { MAX_G = 16 };
    const ImgDeltaUnit* candidates[MAX_G];
    double              scores[MAX_G];
    if (opt.top_g > MAX_G) opt.top_g = MAX_G;

    const uint32_t n_cells = grid->width * grid->height;
    const int brush_active = (opt.target_tier != 0) ||
                             (opt.target_role != 0);

    for (uint32_t pass = 0; pass < opt.passes; pass++) {
        for (uint32_t i = 0; i < n_cells; i++) {
            ImgCECell* cell = &grid->cells[i];

            /* Brush region gate — skip entirely, don't count as visited. */
            if (opt.region_mask && !opt.region_mask[i]) {
                local.cells_masked_out++;
                continue;
            }

            if (opt.skip_zero_cells && cell->core == 0) continue;

            local.cells_visited++;

            int level = -1;
            uint32_t n = img_delta_memory_topg(
                memory, cell, opt.top_g,
                recent_counts, opt.presence_penalty,
                candidates, scores, &level);
            if (n == 0) continue;

            /* Brush re-score: bias candidates whose payload tier or
             * pre_key role matches the brush target. If the bonus
             * changes the ranking, remember it (for stats). */
            uint32_t pick_idx = 0;
            if (brush_active) {
                double   best_score = scores[0];
                uint32_t orig_winner = 0;

                for (uint32_t j = 0; j < n; j++) {
                    double s = scores[j];
                    if (opt.target_tier != 0) {
                        uint8_t ut = img_delta_state_tier(
                                       candidates[j]->payload.state);
                        if (ut == opt.target_tier) s += opt.tier_bonus;
                    }
                    if (opt.target_role != 0) {
                        uint8_t ur = img_state_key_semantic_role(
                                       candidates[j]->pre_key);
                        if (ur == opt.target_role) s += opt.role_bonus;
                    }
                    scores[j] = s;
                    if (s > best_score) {
                        best_score = s;
                        pick_idx   = j;
                    }
                    (void)orig_winner;
                }
                if (pick_idx != 0) local.brush_bonus_wins++;
            }

            const ImgDeltaUnit* picked = candidates[pick_idx];
            img_delta_apply(cell, memory, picked);

            if (picked->id < mem_count) {
                recent_counts[picked->id]++;
                if (!picked_any[picked->id]) {
                    picked_any[picked->id] = 1;
                    local.unique_deltas_used++;
                }
                if (recent_counts[picked->id] > local.max_recent_count) {
                    local.max_recent_count = recent_counts[picked->id];
                }
            }

            local.stamps_applied++;
        }
    }

    free(recent_counts);
    free(picked_any);

    if (out_stats) *out_stats = local;
    return 1;
}

int img_drawing_pass_with_prior(ImgCEGrid*                          grid,
                                ImgDeltaMemory*                     memory,
                                const struct ImgNoiseMemory*        noise_memory,
                                const struct ImgNoiseSampleOptions* noise_opts,
                                const ImgDrawingOptions*            draw_opts,
                                ImgDrawingStats*                    out_stats) {
    if (noise_memory && noise_opts) {
        if (!img_noise_memory_sample_grid(
                (const ImgNoiseMemory*)noise_memory,
                grid,
                (const ImgNoiseSampleOptions*)noise_opts)) {
            return 0;
        }
    }
    return img_drawing_pass(grid, memory, draw_opts, out_stats);
}

