#include "img_ce.h"

#include <string.h>

/* ── small helpers ──────────────────────────────────────── */

static inline int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint8_t sat_add_u8(uint8_t a, int delta) {
    int v = (int)a + delta;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

/* ── SmallCanvas lifecycle ──────────────────────────────── */

ImgSmallCanvas* img_small_canvas_create(void) {
    ImgSmallCanvas* sc = (ImgSmallCanvas*)malloc(sizeof(ImgSmallCanvas));
    if (!sc) return NULL;
    sc->cells = (ImgSmallCell*)calloc(IMG_SC_TOTAL, sizeof(ImgSmallCell));
    if (!sc->cells) { free(sc); return NULL; }
    sc->width  = IMG_SC_SIZE;
    sc->height = IMG_SC_SIZE;
    return sc;
}

void img_small_canvas_destroy(ImgSmallCanvas* sc) {
    if (!sc) return;
    if (sc->cells) free(sc->cells);
    free(sc);
}

void img_small_canvas_clear(ImgSmallCanvas* sc) {
    if (!sc || !sc->cells) return;
    memset(sc->cells, 0, IMG_SC_TOTAL * sizeof(ImgSmallCell));
}

/* ── classifiers ────────────────────────────────────────── */

static uint8_t classify_tone(int brightness) {
    if (brightness < 85)  return IMG_TONE_DARK;
    if (brightness < 170) return IMG_TONE_MID;
    return IMG_TONE_BRIGHT;
}

static uint8_t classify_mood(int r_mean, int g_mean, int b_mean) {
    (void)g_mean;
    int warm = r_mean - b_mean;
    if (warm > 20) return IMG_MOOD_WARM;
    if (warm < -20) return IMG_MOOD_COOL;
    if ((r_mean + b_mean) / 2 < 80) return IMG_MOOD_DRAMATIC;
    return IMG_MOOD_NEUTRAL;
}

static uint8_t classify_flow(int gx, int gy) {
    if (gx < 5 && gy < 5) return IMG_FLOW_NONE;
    if (gx > gy * 6 / 5)  return IMG_FLOW_HORIZONTAL;
    if (gy > gx * 6 / 5)  return IMG_FLOW_VERTICAL;
    return IMG_FLOW_DIAGONAL_UP;
}

static uint8_t classify_depth_by_row(uint32_t y, uint32_t size) {
    /* Simple heuristic matching the prototype: lower rows trend
     * toward foreground. Replace with a learned estimator later. */
    if (y < size * 1 / 3) return IMG_DEPTH_BACKGROUND;
    if (y < size * 2 / 3) return IMG_DEPTH_MIDGROUND;
    return IMG_DEPTH_FOREGROUND;
}

/* ── image → SmallCanvas ────────────────────────────────── */

void img_image_to_small_canvas(const uint8_t* image_rgb,
                               uint32_t image_w, uint32_t image_h,
                               ImgSmallCanvas* out) {
    if (!image_rgb || !out || !out->cells) return;
    if (image_w == 0 || image_h == 0) return;

    /* fixed-point block step (>= 1 pixel guaranteed when image dims
     * >= IMG_SC_SIZE; smaller inputs degrade to nearest-neighbour). */
    for (uint32_t y = 0; y < IMG_SC_SIZE; y++) {
        uint32_t y0 = (uint32_t)(((uint64_t)y     * image_h) / IMG_SC_SIZE);
        uint32_t y1 = (uint32_t)(((uint64_t)(y+1) * image_h) / IMG_SC_SIZE);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > image_h) y1 = image_h;

        for (uint32_t x = 0; x < IMG_SC_SIZE; x++) {
            uint32_t x0 = (uint32_t)(((uint64_t)x     * image_w) / IMG_SC_SIZE);
            uint32_t x1 = (uint32_t)(((uint64_t)(x+1) * image_w) / IMG_SC_SIZE);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > image_w) x1 = image_w;

            uint64_t sr = 0, sg = 0, sb = 0;
            uint64_t n  = 0;
            uint64_t gx_sum = 0, gx_n = 0;
            uint64_t gy_sum = 0, gy_n = 0;
            int prev_row_lum = -1;

            for (uint32_t yy = y0; yy < y1; yy++) {
                int row_lum_sum = 0;
                int row_n = 0;
                int prev_col_lum = -1;

                for (uint32_t xx = x0; xx < x1; xx++) {
                    uint32_t p = (yy * image_w + xx) * 3;
                    uint8_t r = image_rgb[p + 0];
                    uint8_t g = image_rgb[p + 1];
                    uint8_t b = image_rgb[p + 2];

                    sr += r; sg += g; sb += b; n++;

                    int lum = (r + g + b) / 3;
                    row_lum_sum += lum;
                    row_n++;

                    if (prev_col_lum >= 0) {
                        int d = lum - prev_col_lum;
                        if (d < 0) d = -d;
                        gx_sum += (uint64_t)d;
                        gx_n++;
                    }
                    prev_col_lum = lum;
                }

                int row_lum_mean = (row_n > 0) ? row_lum_sum / row_n : 0;
                if (prev_row_lum >= 0) {
                    int d = row_lum_mean - prev_row_lum;
                    if (d < 0) d = -d;
                    gy_sum += (uint64_t)d;
                    gy_n++;
                }
                prev_row_lum = row_lum_mean;
            }

            int r_mean = (n > 0) ? (int)(sr / n) : 0;
            int g_mean = (n > 0) ? (int)(sg / n) : 0;
            int b_mean = (n > 0) ? (int)(sb / n) : 0;
            int brightness = (r_mean + g_mean + b_mean) / 3;
            int gx = (gx_n > 0) ? (int)(gx_sum / gx_n) : 0;
            int gy = (gy_n > 0) ? (int)(gy_sum / gy_n) : 0;

            ImgSmallCell* c = &out->cells[img_sc_idx(y, x)];
            c->intensity = (uint8_t)clamp_i(brightness, 0, 255);
            c->flow      = classify_flow(gx, gy);
            c->mood      = classify_mood(r_mean, g_mean, b_mean);
            c->depth     = classify_depth_by_row(y, IMG_SC_SIZE);
        }
    }
}

/* ── CE grid lifecycle ──────────────────────────────────── */

ImgCEGrid* img_ce_grid_create(void) {
    ImgCEGrid* ce = (ImgCEGrid*)malloc(sizeof(ImgCEGrid));
    if (!ce) return NULL;
    ce->cells = (ImgCECell*)calloc(IMG_CE_TOTAL, sizeof(ImgCECell));
    if (!ce->cells) { free(ce); return NULL; }
    ce->width  = IMG_CE_SIZE;
    ce->height = IMG_CE_SIZE;
    /* last_delta_id must not default to 0 — that collides with the
     * first legitimate delta id. Force IMG_DELTA_ID_NONE on every cell. */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        ce->cells[i].last_delta_id = IMG_DELTA_ID_NONE;
    }
    return ce;
}

void img_ce_grid_destroy(ImgCEGrid* ce) {
    if (!ce) return;
    if (ce->cells) free(ce->cells);
    free(ce);
}

void img_ce_grid_clear(ImgCEGrid* ce) {
    if (!ce || !ce->cells) return;
    memset(ce->cells, 0, IMG_CE_TOTAL * sizeof(ImgCECell));
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        ce->cells[i].last_delta_id = IMG_DELTA_ID_NONE;
    }
}

/* Pick the majority class in a small per-block histogram (≤ 8 bins). */
static uint8_t majority_u8(const int* hist, int bins) {
    int best = 0;
    int best_v = hist[0];
    for (int i = 1; i < bins; i++) {
        if (hist[i] > best_v) { best_v = hist[i]; best = i; }
    }
    return (uint8_t)best;
}

static uint8_t guess_semantic_role(uint8_t depth, uint8_t mood, int intensity) {
    if (depth == IMG_DEPTH_BACKGROUND &&
        (mood == IMG_MOOD_COOL || mood == IMG_MOOD_WARM)) {
        return IMG_ROLE_SKY;
    }
    if (depth == IMG_DEPTH_FOREGROUND && intensity < 90) {
        return IMG_ROLE_OBJECT;
    }
    if (depth == IMG_DEPTH_FOREGROUND) {
        return IMG_ROLE_GROUND;
    }
    return IMG_ROLE_UNKNOWN;
}

/* ── SmallCanvas → CE ───────────────────────────────────── */

void img_small_canvas_to_ce(const ImgSmallCanvas* sc, ImgCEGrid* out) {
    if (!sc || !out || !sc->cells || !out->cells) return;

    for (uint32_t cy = 0; cy < IMG_CE_SIZE; cy++) {
        for (uint32_t cx = 0; cx < IMG_CE_SIZE; cx++) {
            int flow_hist[5]  = {0};
            int mood_hist[4]  = {0};
            int depth_hist[3] = {0};
            int sum_intensity = 0;
            int n = 0;

            uint32_t sy0 = cy * IMG_CE_BLOCK;
            uint32_t sx0 = cx * IMG_CE_BLOCK;

            for (uint32_t dy = 0; dy < IMG_CE_BLOCK; dy++) {
                for (uint32_t dx = 0; dx < IMG_CE_BLOCK; dx++) {
                    const ImgSmallCell* s =
                        &sc->cells[img_sc_idx(sy0 + dy, sx0 + dx)];
                    sum_intensity += s->intensity;
                    if (s->flow  < 5) flow_hist [s->flow ]++;
                    if (s->mood  < 4) mood_hist [s->mood ]++;
                    if (s->depth < 3) depth_hist[s->depth]++;
                    n++;
                }
            }

            int intensity = (n > 0) ? sum_intensity / n : 0;
            uint8_t flow  = majority_u8(flow_hist,  5);
            uint8_t mood  = majority_u8(mood_hist,  4);
            uint8_t depth = majority_u8(depth_hist, 3);

            int core     = intensity;
            int link     = 64 + (int)flow  * 40;
            int delta    =      (int)mood  * 50;
            int priority = 80 + (int)depth * 50;

            ImgCECell* c = &out->cells[img_ce_idx(cy, cx)];
            c->core      = (uint8_t)clamp_i(core,     0, 255);
            c->link      = (uint8_t)clamp_i(link,     0, 255);
            c->delta     = (uint8_t)clamp_i(delta,    0, 255);
            c->priority  = (uint8_t)clamp_i(priority, 0, 255);

            c->tone_class      = classify_tone(intensity);
            c->direction_class = flow;
            c->depth_class     = depth;
            c->semantic_role   = guess_semantic_role(depth, mood, intensity);
            c->delta_sign      = IMG_DELTA_NONE;
            c->last_delta_id   = IMG_DELTA_ID_NONE;
        }
    }
}

/* ── delta coating ──────────────────────────────────────── */

static void apply_coating_cell(ImgCECell* c, const ImgDeltaCoating* k) {
    c->core     = sat_add_u8(c->core,     k->add_core);
    c->link     = sat_add_u8(c->link,     k->add_link);
    c->delta    = sat_add_u8(c->delta,    k->add_delta);
    c->priority = sat_add_u8(c->priority, k->add_priority);

    if (k->semantic_override_on)  c->semantic_role   = k->semantic_override;
    if (k->depth_override_on)     c->depth_class     = k->depth_override;
    if (k->direction_override_on) c->direction_class = k->direction_override;
    if (k->delta_sign_override_on) c->delta_sign     = k->delta_sign_override;
}

void img_ce_apply_coating(ImgCEGrid* ce, uint32_t y, uint32_t x,
                          const ImgDeltaCoating* coating) {
    if (!ce || !ce->cells || !coating) return;
    if (y >= IMG_CE_SIZE || x >= IMG_CE_SIZE) return;
    apply_coating_cell(&ce->cells[img_ce_idx(y, x)], coating);
}

void img_ce_apply_coating_region(ImgCEGrid* ce,
                                 uint32_t y0, uint32_t x0,
                                 uint32_t y1, uint32_t x1,
                                 const ImgDeltaCoating* coating) {
    if (!ce || !ce->cells || !coating) return;
    if (y1 > IMG_CE_SIZE) y1 = IMG_CE_SIZE;
    if (x1 > IMG_CE_SIZE) x1 = IMG_CE_SIZE;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = x0; x < x1; x++) {
            apply_coating_cell(&ce->cells[img_ce_idx(y, x)], coating);
        }
    }
}

/* ── resolve: sieve + repair ────────────────────────────── */

void img_ce_resolve(ImgCEGrid* ce, int threshold,
                    uint8_t* outlier_mask_or_null,
                    uint8_t* explained_mask_or_null,
                    ImgResolveResult* out_result) {
    if (!ce || !ce->cells) return;

    ImgResolveResult r = {0, 0, 0, 0};

    if (outlier_mask_or_null)
        memset(outlier_mask_or_null, 0, IMG_CE_TOTAL);
    if (explained_mask_or_null)
        memset(explained_mask_or_null, 0, IMG_CE_TOTAL);

    for (uint32_t y = 0; y < IMG_CE_SIZE; y++) {
        for (uint32_t x = 0; x < IMG_CE_SIZE; x++) {
            uint32_t idx = img_ce_idx(y, x);
            ImgCECell* c = &ce->cells[idx];

            int sum = 0;
            int count = 0;
            const ImgCECell* nbrs[4] = {0};
            int nbr_count = 0;

            if (y > 0) {
                const ImgCECell* n = &ce->cells[img_ce_idx(y - 1, x)];
                sum += n->core; count++; nbrs[nbr_count++] = n;
            }
            if (y + 1 < IMG_CE_SIZE) {
                const ImgCECell* n = &ce->cells[img_ce_idx(y + 1, x)];
                sum += n->core; count++; nbrs[nbr_count++] = n;
            }
            if (x > 0) {
                const ImgCECell* n = &ce->cells[img_ce_idx(y, x - 1)];
                sum += n->core; count++; nbrs[nbr_count++] = n;
            }
            if (x + 1 < IMG_CE_SIZE) {
                const ImgCECell* n = &ce->cells[img_ce_idx(y, x + 1)];
                sum += n->core; count++; nbrs[nbr_count++] = n;
            }

            if (count == 0) continue;

            int avg = sum / count;
            int diff = (int)c->core - avg;
            if (diff < 0) diff = -diff;

            if (diff <= threshold) continue;

            r.outlier_count++;
            if (outlier_mask_or_null) outlier_mask_or_null[idx] = 1;

            int same_role = 0, same_dir = 0;
            for (int i = 0; i < nbr_count; i++) {
                if (nbrs[i]->semantic_role   == c->semantic_role)   same_role = 1;
                if (nbrs[i]->direction_class == c->direction_class) same_dir  = 1;
            }

            if (same_role || same_dir) {
                /* Explained outlier: absorb toward neighborhood mean. */
                r.explained_count++;
                if (explained_mask_or_null) explained_mask_or_null[idx] = 1;
                c->core = (uint8_t)(((int)c->core + avg) / 2);
                r.repaired_count++;
            } else {
                /* Unexplained: promote as a repair / keyframe candidate. */
                c->delta    = sat_add_u8(c->delta,    32);
                c->priority = sat_add_u8(c->priority, 16);
                c->delta_sign = IMG_DELTA_POSITIVE;
                r.promoted_count++;
            }
        }
    }

    if (out_result) *out_result = r;
}
