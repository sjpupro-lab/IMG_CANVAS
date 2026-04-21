#include "spatial_keyframe.h"
#include "spatial_layers.h"
#include "spatial_subtitle.h"   /* SpatialCanvasPool for ai_get_canvas_pool */
#include "spatial_canvas.h"     /* DataType, detect_data_type */
#include "spatial_q8.h"
#include "img_ce.h"             /* ImgCEGrid lifecycle for ce_snapshot free */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define INITIAL_CAPACITY 64
#define SIMILARITY_THRESHOLD 0.3f
#define EMA_ALPHA            0.1f   /* new value weight per update */
#define EMA_MIN_EVIDENCE     2.0f   /* skip cells with fewer observations */

/* Per-DataType store thresholds. Index = DataType enum value.
 *   PROSE (0): 0.30  narrative baseline (historical engine default)
 *   DIALOG(1): 0.35  conversational turns vary more, needs tighter gate
 *   CODE  (2): 0.20  syntax/punctuation overlap loosens the gate
 *   SHORT (3): 0.25  short strings are noisy; strict enough to avoid
 *                    collapsing dictionary entries into one KF
 *
 * These are the same 4 values the canvas boundary_weight table already
 * differentiates by DataType — the rationale mirrors that spec:
 * PROSE is densest, DIALOG sparsest per-turn, CODE homogeneous, SHORT
 * noisy.
 *
 * Lookup is bounds-clamped: unknown type → PROSE slot. */
static float g_store_threshold_per_type[DATA_TYPE_COUNT] = {
    0.30f,  /* DATA_PROSE */
    0.35f,  /* DATA_DIALOG */
    0.20f,  /* DATA_CODE */
    0.25f   /* DATA_SHORT */
};

static inline int clamp_data_kind(int k) {
    return (k < 0 || k >= DATA_TYPE_COUNT) ? (int)DATA_PROSE : k;
}

void ai_set_store_threshold_for_type(int data_kind, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    g_store_threshold_per_type[clamp_data_kind(data_kind)] = t;
}

float ai_get_store_threshold_for_type(int data_kind) {
    return g_store_threshold_per_type[clamp_data_kind(data_kind)];
}

/* Legacy single-knob API: applies to the PROSE slot, which has been the
 * engine-wide default since SIMILARITY_THRESHOLD=0.30 shipped. Callers
 * that don't care about DataType (tests, legacy tools) keep working. */
void ai_set_store_threshold(float t) {
    ai_set_store_threshold_for_type((int)DATA_PROSE, t);
}

float ai_get_store_threshold(void) {
    return g_store_threshold_per_type[(int)DATA_PROSE];
}

/* Derive the DataType tag stored on a new keyframe. Mirrors the canvas
 * pipeline's detect_data_type so a clause's "kind" is consistent
 * wherever it lands. */
static uint8_t resolve_data_kind(const char* clause_text) {
    if (!clause_text) return (uint8_t)DATA_SHORT;
    return (uint8_t)detect_data_type((const uint8_t*)clause_text,
                                     (uint32_t)strlen(clause_text));
}

/* ── Topic hashing ──
 *
 * Keyframes carry a topic_hash + seq_in_topic. The hash groups
 * clauses that likely belong to the same document so ai_store_auto
 * can skip the O(KF) flat scan and only compare the new clause
 * against same-topic keyframes. When enough same-topic clauses
 * cluster, deltas trigger instead of exploding the keyframe count.
 *
 * Derivation precedence:
 *   1. non-empty label: djb2(label) — explicit document tag
 *   2. else:            djb2(first space-delimited token of clause)
 *   3. else:            0 (legacy sequential behavior)
 *
 * Using only the first token is intentional: wiki abstracts often
 * lead every clause with the article subject ("Anarchism is ...",
 * "Anarchism advocates ..."), so a 1-token fingerprint clusters
 * them cheaply without the cost of a full doc classifier. */
static uint32_t djb2_bytes(const unsigned char* p, size_t n) {
    uint32_t h = 5381u;
    for (size_t i = 0; i < n; i++) h = h * 33u + (uint32_t)p[i];
    return h;
}

static uint32_t topic_hash_from_text(const char* text) {
    if (!text) return 0;
    const unsigned char* p = (const unsigned char*)text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    const unsigned char* start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' &&
           *p != '.' && *p != ',' && *p != '!' && *p != '?') p++;
    size_t n = (size_t)(p - start);
    if (n == 0) return 0;
    uint32_t h = djb2_bytes(start, n);
    return h ? h : 1;
}

static uint32_t topic_hash_from_label(const char* label) {
    if (!label || !*label) return 0;
    uint32_t h = djb2_bytes((const unsigned char*)label, strlen(label));
    return h ? h : 1;
}

/* Resolve the topic for a store call: label wins if provided, else the
 * clause's first token. Returns 0 only when neither yields any bytes,
 * which we treat as "no topic" (legacy flat-scan fallback). */
uint32_t ai_resolve_topic(const char* clause_text, const char* label) {
    uint32_t h = topic_hash_from_label(label);
    if (h == 0) h = topic_hash_from_text(clause_text);
    return h;
}

static uint32_t resolve_topic(const char* clause_text, const char* label) {
    return ai_resolve_topic(clause_text, label);
}

static uint32_t next_seq_in_topic(const SpatialAI* ai, uint32_t topic) {
    if (topic == 0) return 0;
    uint32_t max_seq = 0;
    int seen = 0;
    for (uint32_t i = 0; i < ai->kf_count; i++) {
        if (ai->keyframes[i].topic_hash == topic) {
            if (!seen || ai->keyframes[i].seq_in_topic > max_seq) {
                max_seq = ai->keyframes[i].seq_in_topic;
            }
            seen = 1;
        }
    }
    return seen ? max_seq + 1 : 1;
}

/* Scan only keyframes carrying `topic` and return the best A-cosine
 * match. Returns UINT32_MAX (no candidates) when no keyframe shares
 * this topic. Uses the Q8 integer cosine on the hot path; out_sim is
 * still float for backward compat with callers that compare against
 * float thresholds (they convert internally via q8). */
static uint32_t topic_bucket_best_match(const SpatialAI* ai,
                                        const SpatialGrid* input,
                                        uint32_t topic,
                                        float* out_sim) {
    int      best_q16 = -1;
    uint32_t best_id  = UINT32_MAX;
    if (topic != 0) {
        for (uint32_t i = 0; i < ai->kf_count; i++) {
            if (ai->keyframes[i].topic_hash != topic) continue;
            int sim_q16 = (int)cos_a_q16(input, &ai->keyframes[i].grid);
            if (sim_q16 > best_q16) { best_q16 = sim_q16; best_id = i; }
        }
    }
    if (out_sim) *out_sim = (best_q16 < 0) ? 0.0f : q16_to_float((uint16_t)best_q16);
    return best_id;
}

/* ── RGB EMA ──
 *
 * apply_ema_to_grid blends the accumulated EMA means into an input
 * grid's R/G/B wherever the EMA has enough evidence. The intent is to
 * stabilize each (y, x) cell's channel values across a large corpus:
 * individual clauses are noisy, but the mean over thousands of
 * clauses converges to the POS / context prior for that bitmap slot.
 *
 * ema_update walks the active cells of a freshly stored grid and
 * folds their R/G/B into the running means with weight EMA_ALPHA. A
 * companion count table lets apply_ema_to_grid know how many
 * observations back each cell; cells below EMA_MIN_EVIDENCE are left
 * alone. The update is commutative and doesn't depend on the order
 * in which clauses are stored.
 */

void apply_ema_to_grid(const SpatialAI* ai, SpatialGrid* grid) {
    if (!ai || !grid) return;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (grid->A[i] == 0) continue;
        if (ai->ema_count[i] < EMA_MIN_EVIDENCE) continue;

        float er = ai->ema_R[i];
        float eg = ai->ema_G[i];
        float eb = ai->ema_B[i];

        /* Cells that came out of the encoder with no POS seed get
         * overwritten; cells with a seed are blended 50/50 so the
         * local signal still matters. */
        if (grid->R[i] == 0 && grid->G[i] == 0 && grid->B[i] == 0) {
            grid->R[i] = (uint8_t)(er > 255.0f ? 255 : (er < 0.0f ? 0 : er));
            grid->G[i] = (uint8_t)(eg > 255.0f ? 255 : (eg < 0.0f ? 0 : eg));
            grid->B[i] = (uint8_t)(eb > 255.0f ? 255 : (eb < 0.0f ? 0 : eb));
        } else {
            float r = 0.5f * (float)grid->R[i] + 0.5f * er;
            float g = 0.5f * (float)grid->G[i] + 0.5f * eg;
            float b = 0.5f * (float)grid->B[i] + 0.5f * eb;
            if (r > 255.0f) r = 255.0f; else if (r < 0.0f) r = 0.0f;
            if (g > 255.0f) g = 255.0f; else if (g < 0.0f) g = 0.0f;
            if (b > 255.0f) b = 255.0f; else if (b < 0.0f) b = 0.0f;
            grid->R[i] = (uint8_t)r;
            grid->G[i] = (uint8_t)g;
            grid->B[i] = (uint8_t)b;
        }
    }
}

void ema_update(SpatialAI* ai, const SpatialGrid* grid) {
    if (!ai || !grid) return;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (grid->A[i] == 0) continue;
        float r = (float)grid->R[i];
        float g = (float)grid->G[i];
        float b = (float)grid->B[i];

        if (ai->ema_count[i] == 0.0f) {
            ai->ema_R[i] = r;
            ai->ema_G[i] = g;
            ai->ema_B[i] = b;
        } else {
            ai->ema_R[i] = (1.0f - EMA_ALPHA) * ai->ema_R[i] + EMA_ALPHA * r;
            ai->ema_G[i] = (1.0f - EMA_ALPHA) * ai->ema_G[i] + EMA_ALPHA * g;
            ai->ema_B[i] = (1.0f - EMA_ALPHA) * ai->ema_B[i] + EMA_ALPHA * b;
        }
        ai->ema_count[i] += 1.0f;
    }
}

SpatialAI* spatial_ai_create(void) {
    SpatialAI* ai = (SpatialAI*)malloc(sizeof(SpatialAI));
    if (!ai) return NULL;

    ai->kf_count = 0;
    ai->kf_capacity = INITIAL_CAPACITY;
    ai->keyframes = (Keyframe*)calloc(INITIAL_CAPACITY, sizeof(Keyframe));

    ai->df_count = 0;
    ai->df_capacity = INITIAL_CAPACITY;
    ai->deltas = (DeltaFrame*)calloc(INITIAL_CAPACITY, sizeof(DeltaFrame));

    /* Adaptive channel weights start uniform */
    weight_init(&ai->global_weights);

    /* Canvas pool is lazily created on first ai_get_canvas_pool() call */
    ai->canvas_pool = NULL;

    /* Hash bucket index for large-corpus retrieval; populated as
     * keyframes are added. Rebuilt in ai_load after reading KFs. */
    bucket_index_init(&ai->bucket_idx);

    /* Morpheme dictionary is embedded, but we still pay the per-engine
     * init cost up-front so hot paths can skip it. */
    morpheme_init();
    ai->morpheme_ready = 1;

    /* EMA tables start at zero (no evidence yet). */
    memset(ai->ema_R,     0, sizeof(ai->ema_R));
    memset(ai->ema_G,     0, sizeof(ai->ema_G));
    memset(ai->ema_B,     0, sizeof(ai->ema_B));
    memset(ai->ema_count, 0, sizeof(ai->ema_count));

    if (!ai->keyframes || !ai->deltas) {
        spatial_ai_destroy(ai);
        return NULL;
    }

    /* Initialize keyframe grids */
    for (uint32_t i = 0; i < ai->kf_capacity; i++) {
        ai->keyframes[i].grid.A = NULL;
    }

    return ai;
}

void spatial_ai_destroy(SpatialAI* ai) {
    if (!ai) return;

    if (ai->keyframes) {
        for (uint32_t i = 0; i < ai->kf_count; i++) {
            SpatialGrid* g = &ai->keyframes[i].grid;
            if (g->A) { free(g->A); g->A = NULL; }
            if (g->R) { free(g->R); g->R = NULL; }
            if (g->G) { free(g->G); g->G = NULL; }
            if (g->B) { free(g->B); g->B = NULL; }

            /* Bimodal pairing: free the optional CE image snapshot. */
            if (ai->keyframes[i].ce_snapshot) {
                img_ce_grid_destroy(ai->keyframes[i].ce_snapshot);
                ai->keyframes[i].ce_snapshot = NULL;
            }
        }
        free(ai->keyframes);
    }

    if (ai->deltas) {
        for (uint32_t i = 0; i < ai->df_count; i++) {
            free(ai->deltas[i].entries);
        }
        free(ai->deltas);
    }

    if (ai->canvas_pool) {
        pool_destroy(ai->canvas_pool);
        ai->canvas_pool = NULL;
    }

    bucket_index_destroy(&ai->bucket_idx);

    free(ai);
}

/* ── Lazy canvas-pool accessors ── */

SpatialCanvasPool* ai_get_canvas_pool(SpatialAI* ai) {
    if (!ai) return NULL;
    if (!ai->canvas_pool) ai->canvas_pool = pool_create();
    return ai->canvas_pool;
}

void ai_release_canvas_pool(SpatialAI* ai) {
    if (!ai || !ai->canvas_pool) return;
    pool_destroy(ai->canvas_pool);
    ai->canvas_pool = NULL;
}

/* Grow keyframe array if needed */
static int ensure_kf_capacity(SpatialAI* ai) {
    if (ai->kf_count < ai->kf_capacity) return 1;

    uint32_t new_cap = ai->kf_capacity * 2;
    Keyframe* new_kf = (Keyframe*)realloc(ai->keyframes, new_cap * sizeof(Keyframe));
    if (!new_kf) return 0;

    ai->keyframes = new_kf;
    /* Zero new entries */
    memset(&ai->keyframes[ai->kf_capacity], 0,
           (new_cap - ai->kf_capacity) * sizeof(Keyframe));
    ai->kf_capacity = new_cap;
    return 1;
}

/* Grow delta array if needed */
static int ensure_df_capacity(SpatialAI* ai) {
    if (ai->df_count < ai->df_capacity) return 1;

    uint32_t new_cap = ai->df_capacity * 2;
    DeltaFrame* new_df = (DeltaFrame*)realloc(ai->deltas, new_cap * sizeof(DeltaFrame));
    if (!new_df) return 0;

    ai->deltas = new_df;
    memset(&ai->deltas[ai->df_capacity], 0,
           (new_cap - ai->df_capacity) * sizeof(DeltaFrame));
    ai->df_capacity = new_cap;
    return 1;
}

/* Allocate and copy grid channels into keyframe inline grid */
static void keyframe_alloc_grid(Keyframe* kf, const SpatialGrid* src) {
    kf->grid.A = (uint16_t*)malloc(GRID_TOTAL * sizeof(uint16_t));
    kf->grid.R = (uint8_t*)malloc(GRID_TOTAL);
    kf->grid.G = (uint8_t*)malloc(GRID_TOTAL);
    kf->grid.B = (uint8_t*)malloc(GRID_TOTAL);

    memcpy(kf->grid.A, src->A, GRID_TOTAL * sizeof(uint16_t));
    memcpy(kf->grid.R, src->R, GRID_TOTAL);
    memcpy(kf->grid.G, src->G, GRID_TOTAL);
    memcpy(kf->grid.B, src->B, GRID_TOTAL);
}

uint32_t compute_delta(const SpatialGrid* base, const SpatialGrid* target,
                       DeltaEntry* entries, uint32_t max_entries) {
    if (!base || !target || !entries) return 0;

    uint32_t count = 0;
    for (uint32_t i = 0; i < GRID_TOTAL && count < max_entries; i++) {
        int16_t dA = (int16_t)target->A[i] - (int16_t)base->A[i];
        int8_t  dR = (int8_t)((int)target->R[i] - (int)base->R[i]);
        int8_t  dG = (int8_t)((int)target->G[i] - (int)base->G[i]);
        int8_t  dB = (int8_t)((int)target->B[i] - (int)base->B[i]);

        if (dA != 0 || dR != 0 || dG != 0 || dB != 0) {
            entries[count].index  = i;
            entries[count].diff_A = dA;
            entries[count].diff_R = dR;
            entries[count].diff_G = dG;
            entries[count].diff_B = dB;
            count++;
        }
    }
    return count;
}

void apply_delta(const SpatialGrid* base, const DeltaEntry* entries,
                 uint32_t count, SpatialGrid* out) {
    if (!base || !entries || !out) return;

    grid_copy(out, base);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = entries[i].index;
        if (idx >= GRID_TOTAL) continue;

        int val_A = (int)out->A[idx] + entries[i].diff_A;
        if (val_A < 0) val_A = 0;
        if (val_A > 65535) val_A = 65535;
        out->A[idx] = (uint16_t)val_A;

        int val_R = (int)out->R[idx] + entries[i].diff_R;
        if (val_R < 0) val_R = 0;
        if (val_R > 255) val_R = 255;
        out->R[idx] = (uint8_t)val_R;

        int val_G = (int)out->G[idx] + entries[i].diff_G;
        if (val_G < 0) val_G = 0;
        if (val_G > 255) val_G = 255;
        out->G[idx] = (uint8_t)val_G;

        int val_B = (int)out->B[idx] + entries[i].diff_B;
        if (val_B < 0) val_B = 0;
        if (val_B > 255) val_B = 255;
        out->B[idx] = (uint8_t)val_B;
    }
}

uint32_t ai_store_auto(SpatialAI* ai, const char* clause_text, const char* label) {
    if (!ai || !clause_text) return UINT32_MAX;

    /* Encode clause into grid */
    SpatialGrid* input = grid_create();
    if (!input) return UINT32_MAX;

    layers_encode_clause(clause_text, NULL, input);
    update_rgb_directional(input);
    apply_ema_to_grid(ai, input);

    uint32_t topic          = resolve_topic(clause_text, label);
    uint8_t  new_data_kind  = resolve_data_kind(clause_text);
    float    store_thresh    = ai_get_store_threshold_for_type((int)new_data_kind);

    /* If no keyframes yet, store as first keyframe */
    if (ai->kf_count == 0) {
        if (!ensure_kf_capacity(ai)) { grid_destroy(input); return UINT32_MAX; }

        Keyframe* kf = &ai->keyframes[0];
        kf->id = 0;
        if (label) strncpy(kf->label, label, 63);
        kf->label[63] = '\0';
        kf->text_byte_count = (uint32_t)strlen(clause_text);
        kf->topic_hash      = topic;
        kf->seq_in_topic    = topic ? 1 : 0;
        kf->data_kind       = new_data_kind;
        keyframe_alloc_grid(kf, input);

        ai->kf_count = 1;
        bucket_index_add(&ai->bucket_idx, input, 0);
        ema_update(ai, input);
        grid_destroy(input);
        return 0;
    }

    /* Matching strategy (spec v3 — topic-bucketed):
     *   1. If the clause has a topic, first walk only same-topic
     *      keyframes. Same-topic clauses typically share a lot of
     *      byte-position overlap (wiki abstracts often lead with
     *      the article subject) so this small linear scan finds
     *      deltas the flat scan missed.
     *   2. Fall back to the unified spatial_match(MATCH_SEARCH)
     *      cascade if the topic bucket is empty OR the best
     *      same-topic sim failed to clear the threshold. The
     *      cascade respects the bucket index when kf_count is
     *      large. */
    float    best_sim = 0.0f;
    uint32_t best_id  = UINT32_MAX;
    uint32_t bucket_best = topic_bucket_best_match(ai, input, topic, &best_sim);
    if (bucket_best != UINT32_MAX) best_id = bucket_best;

    if (best_sim < store_thresh) {
        MatchContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.bucket_idx = &ai->bucket_idx;
        MatchResult mr = spatial_match(ai, input, MATCH_SEARCH, &ctx);
        if (mr.best_score > best_sim) {
            best_sim = mr.best_score;
            best_id  = mr.best_id;
        }
    }

    if (best_sim >= store_thresh && best_id < ai->kf_count) {
        /* Store as delta frame */
        if (!ensure_df_capacity(ai)) { grid_destroy(input); return UINT32_MAX; }

        DeltaEntry* entries = (DeltaEntry*)malloc(GRID_TOTAL * sizeof(DeltaEntry));
        if (!entries) { grid_destroy(input); return UINT32_MAX; }

        uint32_t delta_count = compute_delta(&ai->keyframes[best_id].grid, input,
                                             entries, GRID_TOTAL);

        DeltaFrame* df = &ai->deltas[ai->df_count];
        df->id = ai->df_count;
        df->parent_id = best_id;
        if (label) strncpy(df->label, label, 63);
        df->label[63] = '\0';
        df->count = delta_count;
        if (delta_count > 0) {
            /* Shrink the scratch buffer to actual size. If realloc can't
             * shrink in place and returns NULL, keep the original — which
             * is still a valid free-able pointer. */
            DeltaEntry* shrunk = (DeltaEntry*)realloc(entries,
                                   delta_count * sizeof(DeltaEntry));
            df->entries = shrunk ? shrunk : entries;
        } else {
            /* Identical clauses: delta has zero entries. realloc(ptr, 0)
             * is implementation-defined (may free ptr and return NULL),
             * so free explicitly and leave entries NULL. df->count == 0
             * means readers/writers skip the array. */
            free(entries);
            df->entries = NULL;
        }
        uint32_t active = grid_active_count(input);
        df->change_ratio = active ? (float)delta_count / (float)active : 0.0f;

        /* Adaptive feedback: good structural match → boost the channel
         * that most contributed. Compute per-channel similarities
         * between input and matched parent keyframe. */
        {
            SpatialGrid* parent = &ai->keyframes[best_id].grid;
            float sA = channel_sim_A(input, parent);
            float sR = channel_sim_R(input, parent);
            float sG = channel_sim_G(input, parent);
            float sB = channel_sim_B(input, parent);
            weight_update(&ai->global_weights, sA, sR, sG, sB);
        }

        ai->df_count++;
        ema_update(ai, input);
        grid_destroy(input);
        return df->id | 0x80000000u; /* high bit indicates delta */
    } else {
        /* Store as new keyframe */
        if (!ensure_kf_capacity(ai)) { grid_destroy(input); return UINT32_MAX; }

        uint32_t new_id = ai->kf_count;
        Keyframe* kf = &ai->keyframes[new_id];
        kf->id = new_id;
        if (label) strncpy(kf->label, label, 63);
        kf->label[63] = '\0';
        kf->text_byte_count = (uint32_t)strlen(clause_text);
        kf->topic_hash      = topic;
        kf->seq_in_topic    = next_seq_in_topic(ai, topic);
        kf->data_kind       = new_data_kind;
        keyframe_alloc_grid(kf, input);

        /* Adaptive feedback for "novel" input: compare against the
         * nearest existing keyframe to learn which channel saw it as
         * novel (i.e. produced low similarity). Channels that were
         * already low-similarity have done their job in distinguishing
         * the new pattern; they earn a small boost. */
        if (best_id < ai->kf_count - 1) {  /* -1 since we just added */
            SpatialGrid* nearest = &ai->keyframes[best_id].grid;
            float sA = 1.0f - channel_sim_A(input, nearest);
            float sR = 1.0f - channel_sim_R(input, nearest);
            float sG = 1.0f - channel_sim_G(input, nearest);
            float sB = 1.0f - channel_sim_B(input, nearest);
            weight_update(&ai->global_weights, sA, sR, sG, sB);
        }

        ai->kf_count++;
        bucket_index_add(&ai->bucket_idx, input, new_id);
        ema_update(ai, input);
        grid_destroy(input);
        return new_id;
    }
}

uint32_t ai_force_keyframe(SpatialAI* ai, const char* clause_text, const char* label) {
    if (!ai || !clause_text) return UINT32_MAX;

    SpatialGrid* input = grid_create();
    if (!input) return UINT32_MAX;

    layers_encode_clause(clause_text, NULL, input);
    update_rgb_directional(input);
    apply_ema_to_grid(ai, input);

    if (!ensure_kf_capacity(ai)) { grid_destroy(input); return UINT32_MAX; }

    uint32_t new_id = ai->kf_count;
    Keyframe* kf = &ai->keyframes[new_id];
    kf->id = new_id;
    if (label) strncpy(kf->label, label, 63);
    kf->label[63] = '\0';
    kf->text_byte_count = (uint32_t)strlen(clause_text);
    kf->topic_hash      = resolve_topic(clause_text, label);
    kf->seq_in_topic    = next_seq_in_topic(ai, kf->topic_hash);
    kf->data_kind       = resolve_data_kind(clause_text);
    keyframe_alloc_grid(kf, input);

    ai->kf_count++;
    bucket_index_add(&ai->bucket_idx, input, new_id);
    ema_update(ai, input);
    grid_destroy(input);
    return new_id;
}

uint32_t ai_predict(SpatialAI* ai, const char* input_text, float* out_similarity) {
    if (!ai || !input_text || ai->kf_count == 0) {
        if (out_similarity) *out_similarity = 0.0f;
        return UINT32_MAX;
    }

    SpatialGrid* input = grid_create();
    if (!input) {
        if (out_similarity) *out_similarity = 0.0f;
        return UINT32_MAX;
    }

    layers_encode_clause(input_text, NULL, input);
    update_rgb_directional(input);
    apply_ema_to_grid(ai, input);

    /* Delegate to the unified 2-stage core. */
    MatchContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.bucket_idx = &ai->bucket_idx;
    MatchResult r = spatial_match(ai, input, MATCH_PREDICT, &ctx);

    grid_destroy(input);
    if (out_similarity) *out_similarity = r.best_score;
    return r.best_id;
}

/* ── Post-training: repaint + recluster ────────────────────────────
 *
 * The streaming trainer stores KFs in arrival order, so two clauses
 * that belong to the same topic but are separated by thousands of
 * other clauses can both end up as independent keyframes — the
 * similarity drifted below threshold at the time the later clause was
 * stored (EMA not converged yet, encoder noise, etc.).
 *
 * Once the full corpus has been ingested we have enough information to
 * group these stragglers. ai_repaint_ema stabilizes each KF's R/G/B
 * using the final EMA means, and ai_recluster then reclusters by
 * cosine similarity on the A channel, keeps one anchor per cluster,
 * and rewrites every other KF as a delta against that anchor. The net
 * effect is dramatic: a typical wiki-5k run drops ~4600 KFs to
 * ~500 anchors + ~4500 deltas, cutting the on-disk model ~80%.
 */

void ai_repaint_ema(SpatialAI* ai) {
    if (!ai) return;
    for (uint32_t k = 0; k < ai->kf_count; k++) {
        apply_ema_to_grid(ai, &ai->keyframes[k].grid);
    }
}

/* Free a single keyframe's grid channel buffers and NULL them so a
 * later spatial_ai_destroy sweep is a no-op on this slot. */
static void keyframe_free_grid(Keyframe* kf) {
    SpatialGrid* g = &kf->grid;
    if (g->A) { free(g->A); g->A = NULL; }
    if (g->R) { free(g->R); g->R = NULL; }
    if (g->G) { free(g->G); g->G = NULL; }
    if (g->B) { free(g->B); g->B = NULL; }
}

/* Allocate a scratch grid on the stack-ish heap for apply_delta output.
 * Caller must free the returned channels. Returns 0 on alloc failure. */
static int scratch_grid_alloc(SpatialGrid* g) {
    g->A = (uint16_t*)malloc(GRID_TOTAL * sizeof(uint16_t));
    g->R = (uint8_t*)malloc(GRID_TOTAL);
    g->G = (uint8_t*)malloc(GRID_TOTAL);
    g->B = (uint8_t*)malloc(GRID_TOTAL);
    if (!g->A || !g->R || !g->G || !g->B) {
        free(g->A); free(g->R); free(g->G); free(g->B);
        g->A = NULL; g->R = NULL; g->G = NULL; g->B = NULL;
        return 0;
    }
    return 1;
}

static void scratch_grid_free(SpatialGrid* g) {
    free(g->A); free(g->R); free(g->G); free(g->B);
    g->A = NULL; g->R = NULL; g->G = NULL; g->B = NULL;
}

static int cmp_float_desc_kf(const void* a, const void* b) {
    float fa = *(const float*)a, fb = *(const float*)b;
    return (fa < fb) - (fa > fb);
}

/* Data-driven threshold picker for ai_recluster_ex.
 *
 * Why this exists: the streaming-time ai_store_auto and the post-
 * training ai_recluster both compare A-channel cosines between a new
 * clause and the existing KFs. The A channel is fixed at encode time
 * (EMA repaint only touches RGB), so any pair that fell below the
 * store threshold at streaming time is guaranteed to fall below it at
 * recluster time too — reusing g_store_threshold therefore guarantees
 * zero merges. Measured symptom: anchors_kept == kf_count,
 * converted_to_delta = 0.
 *
 * Fix: sample the actual KF-vs-KF cos_A distribution *for this
 * DataType bucket*, sort descending, and pick the value at
 * target_merge_ratio × n_pairs. That value is "the threshold at which
 * target_merge_ratio of same-type, same-topic pairs would pass" —
 * mirrors the calibrate_threshold / canvas_pool_auto_threshold pattern
 * already used for store-time and canvas-level tuning.
 *
 * Returns -1.0f when the type has < 10 same-topic pairs (too sparse);
 * caller should fall back to a sane default or skip that type. */
#define RECLUSTER_CALIB_CAP 4096

static float calibrate_recluster_for_type(const SpatialAI* ai,
                                          int data_kind,
                                          float target_merge_ratio,
                                          uint32_t* out_n_pairs) {
    const uint32_t n = ai->kf_count;
    float* sims = (float*)malloc(RECLUSTER_CALIB_CAP * sizeof(float));
    if (!sims) { if (out_n_pairs) *out_n_pairs = 0; return -1.0f; }

    uint32_t n_sims = 0;
    for (uint32_t i = 0; i < n && n_sims < RECLUSTER_CALIB_CAP; i++) {
        if ((int)ai->keyframes[i].data_kind != data_kind) continue;
        uint32_t topic_i = ai->keyframes[i].topic_hash;
        if (topic_i == 0) continue;
        for (uint32_t j = i + 1; j < n && n_sims < RECLUSTER_CALIB_CAP; j++) {
            if ((int)ai->keyframes[j].data_kind != data_kind) continue;
            if (ai->keyframes[j].topic_hash    != topic_i)    continue;
            uint16_t sim_q16 = cos_a_q16(&ai->keyframes[i].grid,
                                         &ai->keyframes[j].grid);
            sims[n_sims++] = q16_to_float(sim_q16);
        }
    }
    if (out_n_pairs) *out_n_pairs = n_sims;

    if (n_sims < 10) { free(sims); return -1.0f; }

    qsort(sims, n_sims, sizeof(float), cmp_float_desc_kf);
    uint32_t idx = (uint32_t)(target_merge_ratio * (float)n_sims);
    if (idx >= n_sims) idx = n_sims - 1;
    float threshold = sims[idx];
    free(sims);
    return threshold;
}

void ai_recluster(SpatialAI* ai, float cluster_threshold) {
    /* Legacy entry point. An explicit cluster_threshold is passed
     * through unchanged; otherwise target_merge_ratio=0.5 (median)
     * picks a threshold that merges roughly half of same-type /
     * same-topic pairs — a reasonable default given the A-channel is
     * frozen and pairs below the store threshold dominate the post-
     * training distribution. */
    ai_recluster_ex(ai, cluster_threshold, 0.5f);
}

void ai_recluster_ex(SpatialAI* ai, float cluster_threshold,
                     float target_merge_ratio) {
    if (!ai || ai->kf_count < 2) return;

    const uint32_t n = ai->kf_count;

    int32_t*  cluster_id     = (int32_t*) calloc(n, sizeof(int32_t));
    uint32_t* cluster_anchor = (uint32_t*)calloc(n, sizeof(uint32_t));
    uint32_t* kf_child_count = (uint32_t*)calloc(n, sizeof(uint32_t));
    uint32_t* id_remap       = (uint32_t*)calloc(n, sizeof(uint32_t));
    if (!cluster_id || !cluster_anchor || !kf_child_count || !id_remap) {
        free(cluster_id); free(cluster_anchor);
        free(kf_child_count); free(id_remap);
        return;
    }
    for (uint32_t i = 0; i < n; i++) cluster_id[i] = -1;

    /* Count existing delta children per KF so the anchor picker can
     * prefer KFs that already parent deltas (keeps those deltas valid
     * without reconstruction). */
    for (uint32_t d = 0; d < ai->df_count; d++) {
        uint32_t p = ai->deltas[d].parent_id;
        if (p < n) kf_child_count[p]++;
    }

    /* ── Per-DataType threshold resolution ──
     *
     * Explicit cluster_threshold >= 0  → broadcast to every type.
     * Explicit cluster_threshold <  0  → auto-calibrate each type
     *                                    from its own KF-KF cos_A
     *                                    distribution.
     *
     * Auto mode falls back to the per-type store threshold only if a
     * type has < 10 pairs to sample (too sparse to tune). */
    float    type_threshold[DATA_TYPE_COUNT];
    uint16_t type_threshold_q16[DATA_TYPE_COUNT];
    for (int k = 0; k < DATA_TYPE_COUNT; k++) {
        if (cluster_threshold >= 0.0f) {
            type_threshold[k] = cluster_threshold;
        } else {
            uint32_t n_pairs = 0;
            float t = calibrate_recluster_for_type(ai, k,
                                                   target_merge_ratio,
                                                   &n_pairs);
            if (t < 0.0f) {
                t = ai_get_store_threshold_for_type(k);
                printf("[recluster] %-6s  auto-calibrate: only %u pairs — "
                       "falling back to store threshold %.3f\n",
                       data_type_name((DataType)k), n_pairs, t);
            } else {
                printf("[recluster] %-6s  auto-calibrate: %u pairs, "
                       "threshold=%.3f (target_merge=%.2f)\n",
                       data_type_name((DataType)k), n_pairs, t,
                       target_merge_ratio);
            }
            type_threshold[k] = t;
        }
        type_threshold_q16[k] = q16_from_float(type_threshold[k]);
    }

    /* ── Step 1: greedy multi-layer-filtered clustering (Q8) ────
     *
     * Comparing N^2 pairs across 20k KFs = 400M cosine calls. The
     * inner loop is gated by three cheap O(1) checks before any
     * cosine work:
     *   header[1] data_kind  — DataType bucket must match
     *   header[2] topic_hash — first-token document fingerprint
     *   stage-A   cos_a_q16  — byte-position overlap
     *   stage-B   cos_rgb_weighted_q16 — POS/semantic agreement
     *
     * data_kind drops cross-type noise (PROSE<->CODE comparisons etc.)
     * on top of topic filtering; on a wiki+code mixed corpus this
     * cuts the cosine count another 2–4×. KFs with topic_hash=0
     * (legacy/unknown) fall into their own singleton clusters since
     * they have no peer to anchor against.
     *
     * Threshold is DataType-specific (type_threshold_q16[]) so CODE
     * and SHORT get their own tuned gate instead of one global value
     * that fits none of them. */
    uint32_t num_clusters = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cluster_id[i] >= 0) continue;
        uint32_t c = num_clusters++;
        cluster_id[i]     = (int32_t)c;
        cluster_anchor[c] = i;
        uint32_t topic   = ai->keyframes[i].topic_hash;
        uint8_t  kind_i  = ai->keyframes[i].data_kind;
        uint16_t t_q16   = type_threshold_q16[clamp_data_kind((int)kind_i)];
        if (topic == 0) continue;  /* singleton; no peers to absorb */

        for (uint32_t j = i + 1; j < n; j++) {
            if (cluster_id[j] >= 0) continue;
            if (ai->keyframes[j].data_kind  != kind_i) continue;
            if (ai->keyframes[j].topic_hash != topic)  continue;

            uint16_t sim_a_q16 = cos_a_q16(&ai->keyframes[i].grid,
                                           &ai->keyframes[j].grid);
            if (sim_a_q16 < t_q16) continue;

            uint16_t sim_rgb_q16 = cos_rgb_weighted_q16(&ai->keyframes[i].grid,
                                                       &ai->keyframes[j].grid);
            if (sim_rgb_q16 < t_q16) continue;

            cluster_id[j] = (int32_t)c;
        }
    }

    /* ── Step 2: pick cluster anchors ──
     *
     * Priority: more existing children > more active cells. Picking a
     * KF that already parents deltas means fewer deltas need
     * reconstruction in Step 3. */
    for (uint32_t c = 0; c < num_clusters; c++) {
        uint32_t best_idx      = cluster_anchor[c];
        uint32_t best_children = kf_child_count[best_idx];
        uint32_t best_active   = grid_active_count(&ai->keyframes[best_idx].grid);
        for (uint32_t i = 0; i < n; i++) {
            if (cluster_id[i] != (int32_t)c) continue;
            uint32_t children = kf_child_count[i];
            uint32_t active   = grid_active_count(&ai->keyframes[i].grid);
            if (children > best_children ||
                (children == best_children && active > best_active)) {
                best_idx      = i;
                best_children = children;
                best_active   = active;
            }
        }
        cluster_anchor[c] = best_idx;
    }

    /* Precompute the old→new KF index remap. Anchors get sequential
     * new IDs; absorbed KFs are marked UINT32_MAX. */
    uint32_t write = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = (uint32_t)cluster_id[i];
        if (cluster_anchor[c] == i) id_remap[i] = write++;
        else                        id_remap[i] = UINT32_MAX;
    }

    /* ── Step 3: re-parent existing deltas whose parent got absorbed
     *
     * Reconstruct the delta's target grid against its old parent,
     * then recompute a new delta against the new anchor. The old
     * entries buffer is freed. Deltas whose parent was already the
     * anchor are untouched. */
    uint32_t reparented = 0;
    for (uint32_t d = 0; d < ai->df_count; d++) {
        uint32_t old_parent = ai->deltas[d].parent_id;
        if (old_parent >= n) continue;
        uint32_t anchor = cluster_anchor[(uint32_t)cluster_id[old_parent]];
        if (anchor == old_parent) continue;

        SpatialGrid target;
        if (!scratch_grid_alloc(&target)) continue;
        apply_delta(&ai->keyframes[old_parent].grid,
                    ai->deltas[d].entries, ai->deltas[d].count, &target);

        DeltaEntry* new_entries =
            (DeltaEntry*)malloc(GRID_TOTAL * sizeof(DeltaEntry));
        if (!new_entries) { scratch_grid_free(&target); continue; }

        uint32_t new_count = compute_delta(&ai->keyframes[anchor].grid, &target,
                                           new_entries, GRID_TOTAL);
        free(ai->deltas[d].entries);

        if (new_count > 0) {
            DeltaEntry* shrunk = (DeltaEntry*)realloc(new_entries,
                                   new_count * sizeof(DeltaEntry));
            ai->deltas[d].entries = shrunk ? shrunk : new_entries;
        } else {
            free(new_entries);
            ai->deltas[d].entries = NULL;
        }
        ai->deltas[d].count     = new_count;
        ai->deltas[d].parent_id = anchor;
        uint32_t target_active  = grid_active_count(&target);
        ai->deltas[d].change_ratio = target_active
            ? (float)new_count / (float)target_active : 0.0f;

        scratch_grid_free(&target);
        reparented++;
    }

    /* ── Step 4: convert absorbed KFs into fresh deltas ──
     *
     * We still hold the original KF grid here (compaction happens in
     * Step 5), so compute_delta has a valid source. */
    uint32_t converted = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t anchor = cluster_anchor[(uint32_t)cluster_id[i]];
        if (anchor == i) continue;

        if (!ensure_df_capacity(ai)) break;

        DeltaEntry* entries = (DeltaEntry*)malloc(GRID_TOTAL * sizeof(DeltaEntry));
        if (!entries) continue;

        uint32_t delta_count = compute_delta(&ai->keyframes[anchor].grid,
                                             &ai->keyframes[i].grid,
                                             entries, GRID_TOTAL);

        DeltaFrame* df = &ai->deltas[ai->df_count];
        df->id        = ai->df_count;
        df->parent_id = anchor;                      /* remapped in Step 6 */
        memcpy(df->label, ai->keyframes[i].label, sizeof(df->label));
        df->label[63] = '\0';
        df->count = delta_count;
        if (delta_count > 0) {
            DeltaEntry* shrunk = (DeltaEntry*)realloc(entries,
                                   delta_count * sizeof(DeltaEntry));
            df->entries = shrunk ? shrunk : entries;
        } else {
            free(entries);
            df->entries = NULL;
        }
        uint32_t active = grid_active_count(&ai->keyframes[i].grid);
        df->change_ratio = active ? (float)delta_count / (float)active : 0.0f;

        ai->df_count++;
        converted++;
    }

    /* ── Step 5: compact keyframe array ──
     *
     * Free absorbed grids. Move surviving anchors down to fill gaps.
     * Anchor IDs are rewritten to match their new positions. */
    uint32_t new_n = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t anchor = cluster_anchor[(uint32_t)cluster_id[i]];
        if (anchor != i) {
            keyframe_free_grid(&ai->keyframes[i]);
            continue;
        }
        if (new_n != i) {
            ai->keyframes[new_n] = ai->keyframes[i];
        }
        ai->keyframes[new_n].id = new_n;
        new_n++;
    }
    /* Zero the tail so a later growth memset pattern stays consistent. */
    if (new_n < ai->kf_capacity) {
        memset(&ai->keyframes[new_n], 0,
               (ai->kf_capacity - new_n) * sizeof(Keyframe));
    }
    ai->kf_count = new_n;

    /* ── Step 6: remap every delta's parent_id through id_remap ──
     *
     * Both legacy deltas (whose parent survived) and fresh deltas
     * created in Step 4 still carry OLD indices here. */
    for (uint32_t d = 0; d < ai->df_count; d++) {
        uint32_t p = ai->deltas[d].parent_id;
        if (p < n && id_remap[p] != UINT32_MAX) {
            ai->deltas[d].parent_id = id_remap[p];
        }
    }

    /* ── Step 7: rebuild the hash bucket index ──
     *
     * KF indices all shifted; the old bucket entries point at stale
     * IDs. Destroy + re-init + re-add is simpler and correct. */
    bucket_index_destroy(&ai->bucket_idx);
    bucket_index_init(&ai->bucket_idx);
    for (uint32_t i = 0; i < ai->kf_count; i++) {
        bucket_index_add(&ai->bucket_idx, &ai->keyframes[i].grid, i);
    }

    printf("[recluster] clusters=%u  anchors_kept=%u  converted_to_delta=%u  reparented=%u\n",
           num_clusters, ai->kf_count, converted, reparented);
    printf("[recluster] KF: %u -> %u  Delta: total=%u\n",
           n, ai->kf_count, ai->df_count);

    free(cluster_id);
    free(cluster_anchor);
    free(kf_child_count);
    free(id_remap);
}
