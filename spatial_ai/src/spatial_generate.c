#include "spatial_generate.h"
#include "spatial_layers.h"
#include "spatial_morpheme.h"
#include "spatial_match.h"
#include "spatial_context.h"
#include "spatial_canvas.h"
#include "spatial_subtitle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── Aggregated tables ─────────────────────────────────── */

AggTables* agg_build(const SpatialAI* ai) {
    if (!ai) return NULL;
    AggTables* t = (AggTables*)calloc(1, sizeof(AggTables));
    if (!t) return NULL;

    /* Sum A; accumulate A-weighted sums of R, G, B per (y, x) */
    for (uint32_t k = 0; k < ai->kf_count; k++) {
        const SpatialGrid* g = &ai->keyframes[k].grid;
        for (uint32_t i = 0; i < GRID_SIZE * GRID_SIZE; i++) {
            uint16_t a = g->A[i];
            if (a == 0) continue;
            double da = (double)a;
            t->A_sum [i] += da;
            t->R_mean[i] += da * (double)g->R[i];
            t->G_mean[i] += da * (double)g->G[i];
            t->B_mean[i] += da * (double)g->B[i];
        }
    }

    /* Finalize: divide weighted sums by A_sum to get means;
       compute per-row activation totals. */
    for (uint32_t y = 0; y < GRID_SIZE; y++) {
        double row = 0.0;
        for (uint32_t x = 0; x < GRID_SIZE; x++) {
            uint32_t i = y * GRID_SIZE + x;
            if (t->A_sum[i] > 0.0) {
                t->R_mean[i] /= t->A_sum[i];
                t->G_mean[i] /= t->A_sum[i];
                t->B_mean[i] /= t->A_sum[i];
            }
            row += t->A_sum[i];
        }
        t->row_total_A[y] = row;
    }
    return t;
}

AggTables* agg_build_from_pool(const struct SpatialCanvasPool_* pool) {
    if (!pool) return NULL;
    AggTables* t = (AggTables*)calloc(1, sizeof(AggTables));
    if (!t) return NULL;

    /* Iterate every populated slot in every canvas, aggregating into
     * tile-local (y, x) coordinates. This mirrors agg_build but with
     * pool as the source of training patterns. */
    for (uint32_t ei = 0; ei < pool->track.count; ei++) {
        const SubtitleEntry* e = &pool->track.entries[ei];
        const SpatialCanvas* c = pool->canvases[e->canvas_id];
        uint32_t x0, y0;
        canvas_slot_byte_offset(e->slot_id, &x0, &y0);

        for (uint32_t dy = 0; dy < GRID_SIZE; dy++) {
            for (uint32_t dx = 0; dx < GRID_SIZE; dx++) {
                uint32_t ti = dy * GRID_SIZE + dx;
                uint32_t ci = (y0 + dy) * CV_WIDTH + (x0 + dx);
                uint16_t a = c->A[ci];
                if (a == 0) continue;
                double da = (double)a;
                t->A_sum [ti] += da;
                t->R_mean[ti] += da * (double)c->R[ci];
                t->G_mean[ti] += da * (double)c->G[ci];
                t->B_mean[ti] += da * (double)c->B[ci];
            }
        }
    }

    /* Finalise means */
    for (uint32_t y = 0; y < GRID_SIZE; y++) {
        double row = 0.0;
        for (uint32_t x = 0; x < GRID_SIZE; x++) {
            uint32_t i = y * GRID_SIZE + x;
            if (t->A_sum[i] > 0.0) {
                t->R_mean[i] /= t->A_sum[i];
                t->G_mean[i] /= t->A_sum[i];
                t->B_mean[i] /= t->A_sum[i];
            }
            row += t->A_sum[i];
        }
        t->row_total_A[y] = row;
    }
    return t;
}

void agg_destroy(AggTables* t) { free(t); }

/* ── Input signature ────────────────────────────────────── */

void input_signature_compute(InputSignature* sig, const SpatialGrid* input) {
    if (!sig || !input) return;
    memset(sig, 0, sizeof(*sig));

    double global_aw = 0.0, global_rw = 0.0, global_gw = 0.0, global_bw = 0.0;

    for (uint32_t y = 0; y < GRID_SIZE; y++) {
        double aw = 0.0, rw = 0.0, gw = 0.0, bw = 0.0;
        for (uint32_t x = 0; x < GRID_SIZE; x++) {
            uint32_t i = y * GRID_SIZE + x;
            if (input->A[i] == 0) continue;
            double da = (double)input->A[i];
            aw += da;
            rw += da * (double)input->R[i];
            gw += da * (double)input->G[i];
            bw += da * (double)input->B[i];
        }
        if (aw > 0.0) {
            sig->R_row[y] = rw / aw;
            sig->G_row[y] = gw / aw;
            sig->B_row[y] = bw / aw;
            sig->has_activity[y] = 1;
        }
        global_aw += aw;
        global_rw += rw;
        global_gw += gw;
        global_bw += bw;
    }

    if (global_aw > 0.0) {
        sig->R_global = global_rw / global_aw;
        sig->G_global = global_gw / global_aw;
        sig->B_global = global_bw / global_aw;
    }
}

void input_signature_get(const InputSignature* sig, uint32_t y,
                         double* out_R, double* out_G, double* out_B) {
    if (!sig || !out_R || !out_G || !out_B) return;

    /* Fast path: this row has activity */
    if (sig->has_activity[y]) {
        *out_R = sig->R_row[y];
        *out_G = sig->G_row[y];
        *out_B = sig->B_row[y];
        return;
    }

    /* Fallback: nearest active neighbor row within a window */
    for (int d = 1; d < 32; d++) {
        int yu = (int)y - d;
        int yd = (int)y + d;
        if (yu >= 0 && sig->has_activity[yu]) {
            *out_R = sig->R_row[yu];
            *out_G = sig->G_row[yu];
            *out_B = sig->B_row[yu];
            return;
        }
        if (yd < (int)GRID_SIZE && sig->has_activity[yd]) {
            *out_R = sig->R_row[yd];
            *out_G = sig->G_row[yd];
            *out_B = sig->B_row[yd];
            return;
        }
    }

    /* Last resort: global clause signature */
    *out_R = sig->R_global;
    *out_G = sig->G_global;
    *out_B = sig->B_global;
}

/* ── Byte scoring: A × G_sim × R_sim ──────────────────── */

double agg_score_byte(const AggTables* t, uint32_t y, uint8_t v,
                      double in_R, double in_G, double in_B) {
    if (!t) return 0.0;
    uint32_t i = y * GRID_SIZE + (uint32_t)v;
    double A = t->A_sum[i];
    if (A <= 0.0) return 0.0;

    double R = t->R_mean[i];
    double G = t->G_mean[i];
    double B = t->B_mean[i];

    double R_sim = 1.0 - fabs(R - in_R) / 255.0;
    double G_sim = 1.0 - fabs(G - in_G) / 255.0;
    double B_sim = 1.0 - fabs(B - in_B) / 255.0;
    if (R_sim < 0.0) R_sim = 0.0;
    if (G_sim < 0.0) G_sim = 0.0;
    if (B_sim < 0.0) B_sim = 0.0;

    /* Full A × R × G × B product — SPEC §5.1 §9.4 */
    return A * R_sim * G_sim * B_sim;
}

/* ── Grid → text decoding ─────────────────────────────── */

uint32_t grid_decode_text(const SpatialGrid* g, char* out, uint32_t max_out) {
    if (!g || !out || max_out == 0) return 0;

    uint32_t written = 0;
    for (uint32_t y = 0; y < GRID_SIZE && written + 1 < max_out; y++) {
        /* argmax A on this row */
        uint32_t best_x = 0;
        uint16_t best_a = 0;
        for (uint32_t x = 0; x < GRID_SIZE; x++) {
            uint32_t i = y * GRID_SIZE + x;
            if (g->A[i] > best_a) {
                best_a = g->A[i];
                best_x = x;
            }
        }
        if (best_a == 0) {
            /* empty row → clause end */
            break;
        }
        out[written++] = (char)(uint8_t)best_x;
    }
    out[written] = '\0';
    return written;
}

/* ── Full-clause generation ────────────────────────────── */

uint32_t ai_generate_next(SpatialAI* ai, const char* input_text,
                          char* out, uint32_t max_out,
                          float* out_match_similarity) {
    if (!ai || !input_text || !out || max_out == 0 || ai->kf_count == 0) {
        if (out && max_out > 0) out[0] = '\0';
        if (out_match_similarity) *out_match_similarity = 0.0f;
        return 0;
    }

    /* 1. Encode input through full pipeline */
    morpheme_init();
    SpatialGrid* in_grid = grid_create();
    layers_encode_clause(input_text, NULL, in_grid);
    update_rgb_directional(in_grid);

    /* 2. Match via SPEC §9 pipeline (overlap → RGB-weighted cosine) */
    float sim = 0.0f;
    uint32_t best_id = match_engine(ai, in_grid, NULL, NULL, NULL, &sim);
    if (best_id >= ai->kf_count) {
        /* fallback to simple predict */
        grid_destroy(in_grid);
        uint32_t fid = ai_predict(ai, input_text, &sim);
        if (fid >= ai->kf_count) {
            out[0] = '\0';
            if (out_match_similarity) *out_match_similarity = 0.0f;
            return 0;
        }
        best_id = fid;
    } else {
        grid_destroy(in_grid);
    }

    if (out_match_similarity) *out_match_similarity = sim;

    /* 3. Next frame = best_id + 1 (sequential keyframe order) */
    uint32_t target_id = (best_id + 1 < ai->kf_count) ? (best_id + 1) : best_id;

    /* 4. Decode target frame's grid → text */
    return grid_decode_text(&ai->keyframes[target_id].grid, out, max_out);
}

/* Forwarding shim — see comment in spatial_generate.h. */
uint32_t grid_decode_text_utf8(const SpatialGrid* g, char* out,
                               uint32_t max_out) {
    return grid_decode_text(g, out, max_out);
}
