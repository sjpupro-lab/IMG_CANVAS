#include "spatial_subtitle.h"
#include "spatial_match.h"
#include "spatial_q8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── SubtitleTrack ─────────────────────────────────────── */

void subtitle_track_init(SubtitleTrack* t) {
    if (!t) return;
    t->entries = NULL;
    t->count = 0;
    t->capacity = 0;
    for (uint32_t i = 0; i < DATA_TYPE_COUNT; i++) {
        t->by_type[i] = NULL;
        t->by_type_count[i] = 0;
        t->by_type_cap[i] = 0;
    }
}

void subtitle_track_destroy(SubtitleTrack* t) {
    if (!t) return;
    free(t->entries);
    for (uint32_t i = 0; i < DATA_TYPE_COUNT; i++) {
        free(t->by_type[i]);
    }
    /* Reset to a clean state so calling again is safe */
    subtitle_track_init(t);
}

static int ensure_entries_cap(SubtitleTrack* t, uint32_t need) {
    if (t->capacity >= need) return 1;
    uint32_t c = t->capacity ? t->capacity : 32;
    while (c < need) c *= 2;
    SubtitleEntry* n = (SubtitleEntry*)realloc(t->entries, c * sizeof(*t->entries));
    if (!n) return 0;
    t->entries = n;
    t->capacity = c;
    return 1;
}

static int ensure_by_type_cap(SubtitleTrack* t, DataType type, uint32_t need) {
    uint32_t idx = (uint32_t)type;
    if (t->by_type_cap[idx] >= need) return 1;
    uint32_t c = t->by_type_cap[idx] ? t->by_type_cap[idx] : 16;
    while (c < need) c *= 2;
    uint32_t* n = (uint32_t*)realloc(t->by_type[idx], c * sizeof(uint32_t));
    if (!n) return 0;
    t->by_type[idx] = n;
    t->by_type_cap[idx] = c;
    return 1;
}

uint32_t subtitle_track_add(SubtitleTrack* t,
                            DataType type, uint32_t topic_hash,
                            uint32_t canvas_id, uint32_t slot_id,
                            uint32_t byte_length) {
    if (!t) return UINT32_MAX;
    if (!ensure_entries_cap(t, t->count + 1)) return UINT32_MAX;

    uint32_t new_id = t->count;
    SubtitleEntry* e = &t->entries[new_id];
    e->type        = type;
    e->topic_hash  = topic_hash;
    e->canvas_id   = canvas_id;
    e->slot_id     = slot_id;
    e->byte_length = byte_length;
    t->count++;

    if (type >= 0 && type < DATA_TYPE_COUNT) {
        if (!ensure_by_type_cap(t, type, t->by_type_count[type] + 1)) {
            return new_id;  /* entry is in, by-type index is best-effort */
        }
        t->by_type[type][t->by_type_count[type]++] = new_id;
    }
    return new_id;
}

const uint32_t* subtitle_track_ids_of_type(const SubtitleTrack* t,
                                           DataType type, uint32_t* out_count) {
    if (!t || type < 0 || type >= DATA_TYPE_COUNT) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    if (out_count) *out_count = t->by_type_count[(uint32_t)type];
    return t->by_type[(uint32_t)type];
}

int32_t subtitle_track_find(const SubtitleTrack* t,
                            uint32_t canvas_id, uint32_t slot_id) {
    if (!t) return -1;
    for (uint32_t i = 0; i < t->count; i++) {
        if (t->entries[i].canvas_id == canvas_id &&
            t->entries[i].slot_id == slot_id) {
            return (int32_t)i;
        }
    }
    return -1;
}

/* ── Slot-level scoring primitives ─────────────────────── */

float canvas_slot_cosine_a(const SpatialCanvas* c, uint32_t slot,
                           const SpatialGrid* q) {
    if (!c || !q || slot >= CV_SLOTS) return 0.0f;
    uint32_t x0, y0;
    canvas_slot_byte_offset(slot, &x0, &y0);

    double dot = 0.0, na = 0.0, nb = 0.0;
    for (uint32_t dy = 0; dy < CV_TILE; dy++) {
        for (uint32_t dx = 0; dx < CV_TILE; dx++) {
            uint32_t qi = dy * CV_TILE + dx;
            uint32_t ci = (y0 + dy) * CV_WIDTH + (x0 + dx);
            double qa = (double)q->A[qi];
            double ca = (double)c->A[ci];
            dot += qa * ca;
            na += qa * qa;
            nb += ca * ca;
        }
    }
    if (na == 0.0 || nb == 0.0) return 0.0f;
    return (float)(dot / (sqrt(na) * sqrt(nb)));
}

float canvas_slot_rg_score(const SpatialCanvas* c, uint32_t slot,
                           const SpatialGrid* q) {
    if (!c || !q || slot >= CV_SLOTS) return 0.0f;
    uint32_t x0, y0;
    canvas_slot_byte_offset(slot, &x0, &y0);

    double s = 0.0;
    for (uint32_t dy = 0; dy < CV_TILE; dy++) {
        for (uint32_t dx = 0; dx < CV_TILE; dx++) {
            uint32_t qi = dy * CV_TILE + dx;
            uint32_t ci = (y0 + dy) * CV_WIDTH + (x0 + dx);
            if (q->A[qi] == 0 || c->A[ci] == 0) continue;
            double rs = 1.0 - fabs((double)q->R[qi] - c->R[ci]) / 255.0;
            double gs = 1.0 - fabs((double)q->G[qi] - c->G[ci]) / 255.0;
            if (rs < 0) rs = 0;
            if (gs < 0) gs = 0;
            s += rs * gs;
        }
    }
    return (float)s;
}

float canvas_slot_ba_score(const SpatialCanvas* c, uint32_t slot,
                           const SpatialGrid* q) {
    if (!c || !q || slot >= CV_SLOTS) return 0.0f;
    uint32_t x0, y0;
    canvas_slot_byte_offset(slot, &x0, &y0);

    double s = 0.0;
    for (uint32_t dy = 0; dy < CV_TILE; dy++) {
        for (uint32_t dx = 0; dx < CV_TILE; dx++) {
            uint32_t qi = dy * CV_TILE + dx;
            uint32_t ci = (y0 + dy) * CV_WIDTH + (x0 + dx);
            if (q->A[qi] == 0 || c->A[ci] == 0) continue;
            double bs = 1.0 - fabs((double)q->B[qi] - c->B[ci]) / 255.0;
            uint16_t mn = (q->A[qi] < c->A[ci]) ? q->A[qi] : c->A[ci];
            if (bs < 0) bs = 0;
            s += bs * (double)mn;
        }
    }
    return (float)s;
}

/* ── SpatialCanvasPool ─────────────────────────────────── */

/* ── H.264-style scene change classification ────────────── */

void scene_change_init(SceneChangeState* s) {
    if (!s) return;
    s->threshold_ema = 0.0f;
    s->n_samples = 0;
}

CanvasFrameType scene_change_classify(
    const SpatialCanvas* candidate,
    SpatialCanvas* const* refs, uint32_t n_refs,
    SceneChangeState* state,
    uint32_t* out_best_ref_id, float* out_changed_ratio)
{
    if (!candidate) return CANVAS_IFRAME;
    if (n_refs == 0) {
        /* First canvas of this type — always IFRAME */
        if (out_best_ref_id) *out_best_ref_id = UINT32_MAX;
        if (out_changed_ratio) *out_changed_ratio = 1.0f;
        return CANVAS_IFRAME;
    }

    CanvasBlockSummary cand;
    canvas_compute_block_sums(candidate, &cand);

    /* Find the closest reference by total absolute block-sum difference
     * (Sum of Absolute Differences). */
    uint32_t best_ref = 0;
    uint64_t min_sad = UINT64_MAX;
    for (uint32_t r = 0; r < n_refs; r++) {
        CanvasBlockSummary ref;
        canvas_compute_block_sums(refs[r], &ref);
        uint64_t sad = 0;
        for (uint32_t b = 0; b < CV_BLOCKS_TOTAL; b++) {
            uint32_t d = (cand.sums[b] > ref.sums[b])
                       ? (cand.sums[b] - ref.sums[b])
                       : (ref.sums[b] - cand.sums[b]);
            sad += d;
        }
        if (sad < min_sad) { min_sad = sad; best_ref = r; }
    }

    /* Count blocks that changed significantly vs the closest reference.
     * "Significantly" = above the EMA-tracked typical diff. */
    CanvasBlockSummary ref;
    canvas_compute_block_sums(refs[best_ref], &ref);

    double mean_diff = 0.0;
    uint32_t changed = 0;
    for (uint32_t b = 0; b < CV_BLOCKS_TOTAL; b++) {
        uint32_t d = (cand.sums[b] > ref.sums[b])
                   ? (cand.sums[b] - ref.sums[b])
                   : (ref.sums[b] - cand.sums[b]);
        mean_diff += (double)d;
        if ((float)d > state->threshold_ema) changed++;
    }
    mean_diff /= (double)CV_BLOCKS_TOTAL;

    /* Update EMA with this canvas's mean block diff */
    if (state->n_samples == 0) {
        state->threshold_ema = (float)mean_diff;
    } else {
        state->threshold_ema =
            SCENE_CHANGE_ALPHA * (float)mean_diff +
            (1.0f - SCENE_CHANGE_ALPHA) * state->threshold_ema;
    }
    state->n_samples++;

    float ratio = (float)changed / (float)CV_BLOCKS_TOTAL;
    if (out_best_ref_id) *out_best_ref_id = best_ref;
    if (out_changed_ratio) *out_changed_ratio = ratio;

    return (ratio > SCENE_CHANGE_IFRAME_RATIO) ? CANVAS_IFRAME : CANVAS_PFRAME;
}

/* ── Pool lifecycle ───────────────────────────────────── */

SpatialCanvasPool* pool_create(void) {
    SpatialCanvasPool* p = (SpatialCanvasPool*)calloc(1, sizeof(SpatialCanvasPool));
    if (!p) return NULL;
    p->canvases = NULL;
    p->count = 0;
    p->capacity = 0;
    /* Clock-engine thresholds; tuned on wiki5k to land avg 3-5 chapters
     * per 32-slot canvas. G drives chapter transitions (hz_diff
     * accumulation over a chapter). R+B catches context/structure
     * breaks G alone misses. Raise both for stricter (fewer) chapters;
     * lower for finer splits. */
    p->clock_g_threshold  = 250u;
    p->clock_rb_threshold = 150u;
    subtitle_track_init(&p->track);
    scene_change_init(&p->scene);
    return p;
}

void pool_destroy(SpatialCanvasPool* p) {
    if (!p) return;
    if (p->canvases) {
        for (uint32_t i = 0; i < p->count; i++) {
            if (p->canvases[i]) canvas_destroy(p->canvases[i]);
        }
        free(p->canvases);
    }
    subtitle_track_destroy(&p->track);
    free(p);
}

static int ensure_pool_cap(SpatialCanvasPool* p, uint32_t need) {
    if (p->capacity >= need) return 1;
    uint32_t c = p->capacity ? p->capacity : 4;
    while (c < need) c *= 2;
    SpatialCanvas** n = (SpatialCanvas**)realloc(p->canvases, c * sizeof(*p->canvases));
    if (!n) return 0;
    p->canvases = n;
    p->capacity = c;
    return 1;
}

/* Find a canvas of matching type with a free slot; -1 if none */
static int32_t find_open_canvas(const SpatialCanvasPool* p, DataType type) {
    for (uint32_t i = 0; i < p->count; i++) {
        SpatialCanvas* c = p->canvases[i];
        if (!c) continue;
        if (c->slot_count == 0) continue;   /* unused canvas — wait until type is known */
        if (c->canvas_type != type) continue;
        if (c->slot_count < CV_SLOTS) return (int32_t)i;
    }
    return -1;
}

int pool_add_clause(SpatialCanvasPool* p, const char* text) {
    if (!p || !text) return -1;
    uint32_t len = (uint32_t)strlen(text);
    DataType type = detect_data_type((const uint8_t*)text, len);

    int32_t cvi = find_open_canvas(p, type);
    if (cvi < 0) {
        /* Create a new canvas for this type */
        if (!ensure_pool_cap(p, p->count + 1)) return -1;
        SpatialCanvas* nc = canvas_create();
        if (!nc) return -1;
        p->canvases[p->count] = nc;
        cvi = (int32_t)p->count;
        p->count++;
    }

    SpatialCanvas* c = p->canvases[cvi];
    int slot = canvas_add_clause(c, text);
    if (slot < 0) return -1;

    /* Append to subtitle track */
    uint32_t th = c->meta[slot].topic_hash;
    uint32_t entry_id = subtitle_track_add(&p->track, type, th,
                                           (uint32_t)cvi, (uint32_t)slot, len);

    /* If this placement filled the canvas, run scene-change classification
     * so future matching / compression can rely on I/P labels. */
    if (c->slot_count == CV_SLOTS && !c->classified) {
        /* RGB diffusion has to run before freq_tag assignment: the
         * tag heuristic looks at B-channel values across the slot
         * edge, and B only exists at active cells (byte-value
         * positions). Without diffusion the boundary band is mostly
         * empty and every slot ends up tagged differently. Diffusing
         * spreads B between active cells across the boundary so the
         * mean-B metric is meaningful. Cost: ~2M cells × constant
         * work, ~50 ms per canvas — paid once per fill. */
        canvas_update_rgb(c);

        /* Carve the 32 slots into chapter groups *before* classify so
         * boundary_multiplier (which scene_change_classify will end up
         * driving via canvas_compute_block_sums) sees the correct
         * cross-chapter dampening.
         *
         * NB: the metric flipped in canvas_b_edge_value v2 from
         * "B-channel mean at the literal pixel edge" to "A-channel
         * cosine between the two slot regions". Threshold (set by
         * trainer via pool->freq_tag_sad_threshold; default 30)
         * means a chapter break is declared when adjacent slots' A
         * patterns agree at less than that cosine — for wiki-style
         * text the noise floor sits around 0.10, so 0.05 keeps most
         * neighbours together while still cutting at sharp shifts.
         *
         * use_topic_hash=0: wiki-style clauses start with a different
         * first token almost every line, so topic_hash flips on every
         * slot transition. Re-enable once a coarser topic_hash
         * (per-paragraph, not per-clause) exists. */
        canvas_compute_all_summaries(c);

        /* Diagnostic: dump the summary distribution for the first
         * canvas to fill. Single static flag so we only print once
         * per process. Kept for side-by-side comparison with the
         * clock-engine output during tuning. */
        static int diag_done = 0;
        if (!diag_done) {
            diag_done = 1;
            printf("[diag] === first canvas summary ===\n");
            printf("[diag] slot  b_mean  hz_hist(b3 b2 b1 b0)  byte_len  topic_hash\n");
            for (uint32_t s = 0; s < c->slot_count; s++) {
                const SlotComputeSummary* sm = &c->compute[s];
                uint8_t h3 = (sm->hz_hist >> 12) & 0xF;
                uint8_t h2 = (sm->hz_hist >> 8)  & 0xF;
                uint8_t h1 = (sm->hz_hist >> 4)  & 0xF;
                uint8_t h0 = (sm->hz_hist     )  & 0xF;
                printf("[diag] %3u   %3u    %2u %2u %2u %2u            %5u    %u\n",
                       s, sm->b_mean, h3, h2, h1, h0,
                       c->meta[s].byte_length, c->meta[s].topic_hash);
            }
            printf("[diag] --- adjacent SAD (i-1 -> i, same row only) ---\n");
            printf("[diag] pair    b_diff  hz_diff  total_sad\n");
            for (uint32_t i = 1; i < c->slot_count; i++) {
                uint32_t row_p = (i - 1) / CV_COLS;
                uint32_t row_c = i       / CV_COLS;
                if (row_p != row_c) {
                    printf("[diag] %2u->%2u  (row crossing, skipped)\n", i - 1, i);
                    continue;
                }
                const SlotComputeSummary* sa = &c->compute[i - 1];
                const SlotComputeSummary* sb = &c->compute[i];
                int b_diff = (sa->b_mean > sb->b_mean)
                           ? sa->b_mean - sb->b_mean
                           : sb->b_mean - sa->b_mean;
                int hz_diff = 0;
                for (uint32_t k = 0; k < 4; k++) {
                    int va = (sa->hz_hist >> (k * 4)) & 0xF;
                    int vb = (sb->hz_hist >> (k * 4)) & 0xF;
                    hz_diff += (va > vb) ? va - vb : vb - va;
                }
                printf("[diag] %2u->%2u  %4d    %4d     %4d\n",
                       i - 1, i, b_diff, hz_diff, b_diff + hz_diff);
            }
            printf("[diag] === end of first-canvas dump ===\n");
        }

        canvas_assign_freq_tags_clock(c,
                                      p->clock_g_threshold,
                                      p->clock_rb_threshold,
                                      0);

        /* Gather same-type IFRAME canvases (excluding the candidate). */
        SpatialCanvas** refs = (SpatialCanvas**)malloc(p->count * sizeof(SpatialCanvas*));
        uint32_t n_refs = 0;
        for (uint32_t i = 0; i < p->count; i++) {
            if ((int32_t)i == cvi) continue;
            SpatialCanvas* other = p->canvases[i];
            if (!other) continue;
            if (!other->classified) continue;
            if (other->frame_type != CANVAS_IFRAME) continue;
            if (other->canvas_type != c->canvas_type) continue;
            refs[n_refs++] = other;
        }
        uint32_t best_ref_local = 0;
        float ratio = 0.0f;
        CanvasFrameType t = scene_change_classify(c, refs, n_refs, &p->scene,
                                                  &best_ref_local, &ratio);
        c->frame_type = t;
        c->changed_ratio = ratio;
        if (t == CANVAS_PFRAME && n_refs > 0) {
            /* Map local ref index back to pool canvas index */
            uint32_t local = 0;
            for (uint32_t i = 0; i < p->count; i++) {
                if ((int32_t)i == cvi) continue;
                SpatialCanvas* other = p->canvases[i];
                if (!other || !other->classified) continue;
                if (other->frame_type != CANVAS_IFRAME) continue;
                if (other->canvas_type != c->canvas_type) continue;
                if (local == best_ref_local) {
                    c->parent_canvas_id = i;
                    break;
                }
                local++;
            }
        }
        c->classified = 1;
        free(refs);
    }

    return (int)entry_id;
}

uint32_t pool_total_slots(const SpatialCanvasPool* p) {
    return p ? p->track.count : 0;
}

/* ── Canvas-level recluster ──────────────────────────────
 *
 * Equivalent of ai_recluster, one tier up. Rationale: streaming ingest
 * calls scene_change_classify the moment a canvas fills up, so I/P
 * decisions are made against whatever IFRAMEs happen to exist at that
 * instant. A canvas that fills late in training might have been a
 * perfect PFRAME child of a canvas filled early, but the classifier
 * didn't have the information to spot it.
 *
 * Once ingest is done we have the full set. We recompute A-channel
 * block-sum cosine between every same-type canvas pair, greedy-cluster
 * them, and relabel: one anchor per cluster stays IFRAME, the rest
 * become PFRAMEs pointing at that anchor. Pixels stay put; only the
 * I/P graph changes.
 *
 * Cost: O(N_canvas^2 * 8192) on block-sum dot products. With ~150
 * canvases per 5000-clause run that's ~184M fmul — sub-second.
 */

static int cmp_u16_desc(const void* a, const void* b) {
    uint16_t fa = *(const uint16_t*)a, fb = *(const uint16_t*)b;
    if (fa > fb) return -1;
    if (fa < fb) return  1;
    return 0;
}

/* Q8 cosine over CanvasBlockSummary (8192 uint32 sums per canvas).
 * In practice block sums are tiny — wiki5k canvases see per-block A
 * sums in the 0..50 range (active density × A value × 256 cells per
 * block, with most blocks empty), so na, nb ≤ 8192 × 50² ≈ 20M and
 * dot stays well under uint64. No pre-scale needed; an earlier
 * version shifted by 8 to be "safe" but that flattened everything to
 * 0 because the real magnitudes were already small. */
static uint16_t block_sum_cosine_q16(const CanvasBlockSummary* a,
                                     const CanvasBlockSummary* b) {
    uint64_t dot = 0, na = 0, nb = 0;
    for (uint32_t i = 0; i < CV_BLOCKS_TOTAL; i++) {
        uint64_t ai = (uint64_t)a->sums[i];
        uint64_t bi = (uint64_t)b->sums[i];
        dot += ai * bi;
        na  += ai * ai;
        nb  += bi * bi;
    }
    return q16_cosine(dot, na, nb);
}

float canvas_pool_auto_threshold(const SpatialCanvasPool* pool,
                                 float target_merge_ratio) {
    if (!pool || pool->count < 2) return -1.0f;
    if (target_merge_ratio < 0.0f) target_merge_ratio = 0.0f;
    if (target_merge_ratio > 1.0f) target_merge_ratio = 1.0f;

    const uint32_t n = pool->count;

    CanvasBlockSummary* sums = (CanvasBlockSummary*)malloc(n * sizeof(CanvasBlockSummary));
    if (!sums) return -1.0f;
    for (uint32_t i = 0; i < n; i++) {
        if (pool->canvases[i]) {
            canvas_compute_block_sums(pool->canvases[i], &sums[i]);
        } else {
            memset(&sums[i], 0, sizeof(CanvasBlockSummary));
        }
    }

    /* Upper bound on same-type pair count; allocate accordingly. */
    uint64_t max_pairs = (uint64_t)n * (uint64_t)(n - 1) / 2ull;
    if (max_pairs == 0) { free(sums); return -1.0f; }
    uint16_t* pairs_q16 = (uint16_t*)malloc((size_t)max_pairs * sizeof(uint16_t));
    if (!pairs_q16) { free(sums); return -1.0f; }

    uint32_t np = 0;
    for (uint32_t i = 0; i < n; i++) {
        SpatialCanvas* ci = pool->canvases[i];
        if (!ci) continue;
        for (uint32_t j = i + 1; j < n; j++) {
            SpatialCanvas* cj = pool->canvases[j];
            if (!cj) continue;
            if (cj->canvas_type != ci->canvas_type) continue;
            pairs_q16[np++] = block_sum_cosine_q16(&sums[i], &sums[j]);
        }
    }

    float threshold = -1.0f;
    if (np >= 4) {
        qsort(pairs_q16, np, sizeof(uint16_t), cmp_u16_desc);
        uint32_t idx = (uint32_t)(target_merge_ratio * (float)np);
        if (idx >= np) idx = np - 1;
        uint16_t pick_q16 = pairs_q16[idx];
        threshold = q16_to_float(pick_q16);
        printf("[canvas-calibrate] pairs=%u  target_merge=%.2f  "
               "min=%.4f  p%02d=%.4f  median=%.4f  p10=%.4f  max=%.4f  (Q16 pick=%u)\n",
               np, target_merge_ratio,
               q16_to_float(pairs_q16[np - 1]),
               (int)(target_merge_ratio * 100), threshold,
               q16_to_float(pairs_q16[np / 2]), q16_to_float(pairs_q16[np / 10]),
               q16_to_float(pairs_q16[0]),
               (unsigned)pick_q16);
    } else {
        printf("[canvas-calibrate] only %u same-type pairs — "
               "not enough to auto-tune a threshold\n", np);
    }

    free(pairs_q16);
    free(sums);
    return threshold;
}

void canvas_pool_recluster(SpatialCanvasPool* pool, float cluster_threshold) {
    if (!pool || pool->count < 2) return;

    const uint32_t n = pool->count;

    CanvasBlockSummary* sums           = (CanvasBlockSummary*)malloc(n * sizeof(CanvasBlockSummary));
    uint32_t*           active         = (uint32_t*)calloc(n, sizeof(uint32_t));
    int32_t*            cluster_id     = (int32_t*) calloc(n, sizeof(int32_t));
    uint32_t*           cluster_anchor = (uint32_t*)calloc(n, sizeof(uint32_t));
    if (!sums || !active || !cluster_id || !cluster_anchor) {
        free(sums); free(active); free(cluster_id); free(cluster_anchor);
        return;
    }

    for (uint32_t i = 0; i < n; i++) {
        cluster_id[i] = -1;
        if (pool->canvases[i]) {
            canvas_compute_block_sums(pool->canvases[i], &sums[i]);
            active[i] = canvas_active_count(pool->canvases[i]);
        }
    }

    /* Step 1: greedy type-filtered clustering on Q16 block-sum cosine.
     * Convert the float threshold to Q16 once; per-pair work is then
     * one isqrt + integer compare. */
    uint16_t threshold_q16 = q16_from_float(cluster_threshold);
    uint32_t num_clusters = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cluster_id[i] >= 0) continue;
        SpatialCanvas* ci = pool->canvases[i];
        if (!ci) continue;

        uint32_t c = num_clusters++;
        cluster_id[i]     = (int32_t)c;
        cluster_anchor[c] = i;

        for (uint32_t j = i + 1; j < n; j++) {
            if (cluster_id[j] >= 0) continue;
            SpatialCanvas* cj = pool->canvases[j];
            if (!cj) continue;
            if (cj->canvas_type != ci->canvas_type) continue;

            uint16_t sim_q16 = block_sum_cosine_q16(&sums[i], &sums[j]);
            if (sim_q16 >= threshold_q16) cluster_id[j] = (int32_t)c;
        }
    }

    /* Step 2: anchor selection.
     *   Priority: pre-existing IFRAME > active cell count.
     * Keeping an existing IFRAME as anchor avoids re-labelling its
     * already-parent-linked children mid-flight. */
    for (uint32_t c = 0; c < num_clusters; c++) {
        uint32_t best_idx       = cluster_anchor[c];
        SpatialCanvas* bc       = pool->canvases[best_idx];
        int      best_was_iframe= bc && bc->classified &&
                                  bc->frame_type == CANVAS_IFRAME;
        uint32_t best_active    = active[best_idx];

        for (uint32_t i = 0; i < n; i++) {
            if (cluster_id[i] != (int32_t)c) continue;
            SpatialCanvas* ci = pool->canvases[i];
            if (!ci) continue;
            int is_iframe = (ci->classified &&
                             ci->frame_type == CANVAS_IFRAME);

            int take = 0;
            if (is_iframe && !best_was_iframe) take = 1;
            else if (is_iframe == best_was_iframe &&
                     active[i] > best_active)   take = 1;

            if (take) {
                best_idx        = i;
                best_was_iframe = is_iframe;
                best_active     = active[i];
            }
        }
        cluster_anchor[c] = best_idx;
    }

    /* Step 3: relabel. Pixel data untouched. */
    uint32_t iframes = 0, pframes = 0, flipped = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cluster_id[i] < 0) continue;
        SpatialCanvas* ci = pool->canvases[i];
        if (!ci) continue;
        uint32_t anchor = cluster_anchor[(uint32_t)cluster_id[i]];

        CanvasFrameType old = ci->frame_type;
        if (anchor == i) {
            ci->frame_type        = CANVAS_IFRAME;
            ci->parent_canvas_id  = UINT32_MAX;
            iframes++;
        } else {
            ci->frame_type        = CANVAS_PFRAME;
            ci->parent_canvas_id  = anchor;
            pframes++;
        }
        ci->classified = 1;
        if (old != ci->frame_type) flipped++;
    }

    printf("[canvas-recluster] canvases=%u  clusters=%u  I=%u  P=%u  flipped=%u\n",
           n, num_clusters, iframes, pframes, flipped);

    free(sums);
    free(active);
    free(cluster_id);
    free(cluster_anchor);
}

/* ── 4-step pool_match ─────────────────────────────────── */

/* Evaluate A → RG → BA over a list of (canvas_id, slot_id) and return
 * the best by A-cosine (normalized metric for cross-step comparison). */
static PoolMatchResult match_within_ids(SpatialCanvasPool* p,
                                        const SpatialGrid* q,
                                        const uint32_t* ids, uint32_t n) {
    PoolMatchResult r;
    r.canvas_id = 0;
    r.slot_id = 0;
    r.similarity = -1.0f;
    r.query_type = DATA_SHORT;
    r.fallback = 0;
    r.step_taken = 0;

    if (n == 0) { r.similarity = 0.0f; return r; }

    /* Step 1: A-only argmax */
    float best_a = -1.0f;
    uint32_t best_a_canvas = 0, best_a_slot = 0;
    for (uint32_t i = 0; i < n; i++) {
        SubtitleEntry* e = &p->track.entries[ids[i]];
        float s = canvas_slot_cosine_a(p->canvases[e->canvas_id], e->slot_id, q);
        if (s > best_a) { best_a = s; best_a_canvas = e->canvas_id; best_a_slot = e->slot_id; }
    }
    if (best_a >= 0.5f) {
        r.canvas_id = best_a_canvas; r.slot_id = best_a_slot;
        r.similarity = best_a; r.step_taken = 1;
        return r;
    }

    /* Step 2: R×G argmax (ordinal), then evaluate A for reporting */
    float best_rg = -1.0f;
    uint32_t best_rg_canvas = 0, best_rg_slot = 0;
    for (uint32_t i = 0; i < n; i++) {
        SubtitleEntry* e = &p->track.entries[ids[i]];
        float s = canvas_slot_rg_score(p->canvases[e->canvas_id], e->slot_id, q);
        if (s > best_rg) { best_rg = s; best_rg_canvas = e->canvas_id; best_rg_slot = e->slot_id; }
    }

    /* Step 3: B×A argmax */
    float best_ba = -1.0f;
    uint32_t best_ba_canvas = 0, best_ba_slot = 0;
    for (uint32_t i = 0; i < n; i++) {
        SubtitleEntry* e = &p->track.entries[ids[i]];
        float s = canvas_slot_ba_score(p->canvases[e->canvas_id], e->slot_id, q);
        if (s > best_ba) { best_ba = s; best_ba_canvas = e->canvas_id; best_ba_slot = e->slot_id; }
    }

    /* Among the three candidates, pick the one with the highest A-cosine
       to report a comparable similarity score. */
    typedef struct { uint32_t cv, sl; int step; } Cand;
    Cand cands[3] = {
        { best_a_canvas,  best_a_slot,  1 },
        { best_rg_canvas, best_rg_slot, 2 },
        { best_ba_canvas, best_ba_slot, 3 }
    };

    float best_final = -1.0f;
    int   best_step  = 1;
    uint32_t best_cv = 0, best_sl = 0;
    for (int i = 0; i < 3; i++) {
        float s = canvas_slot_cosine_a(p->canvases[cands[i].cv], cands[i].sl, q);
        if (s > best_final) {
            best_final = s;
            best_step = cands[i].step;
            best_cv = cands[i].cv;
            best_sl = cands[i].sl;
        }
    }
    r.canvas_id = best_cv;
    r.slot_id = best_sl;
    r.similarity = best_final;
    r.step_taken = best_step;
    return r;
}

PoolMatchResult pool_match(SpatialCanvasPool* p,
                           const SpatialGrid* q,
                           const char* query_text) {
    PoolMatchResult r;
    memset(&r, 0, sizeof(r));
    r.similarity = -1.0f;

    if (!p || !q || pool_total_slots(p) == 0) {
        r.similarity = 0.0f;
        return r;
    }

    DataType q_type = (query_text)
        ? detect_data_type((const uint8_t*)query_text, (uint32_t)strlen(query_text))
        : DATA_DIALOG;
    r.query_type = q_type;

    /* Step 0: jump to same-type slots */
    uint32_t n_same;
    const uint32_t* same_ids = subtitle_track_ids_of_type(&p->track, q_type, &n_same);

    /* Steps 1-3 within same type */
    PoolMatchResult within = match_within_ids(p, q, same_ids, n_same);
    within.query_type = q_type;

    /* Success criterion: non-trivial similarity */
    if (within.similarity >= 0.1f) {
        return within;
    }

    /* Step 4: fall back to other types (excluding q_type) */
    PoolMatchResult best_fb;
    memset(&best_fb, 0, sizeof(best_fb));
    best_fb.similarity = -1.0f;
    best_fb.query_type = q_type;

    for (uint32_t t = 0; t < DATA_TYPE_COUNT; t++) {
        if ((DataType)t == q_type) continue;
        uint32_t n;
        const uint32_t* ids = subtitle_track_ids_of_type(&p->track, (DataType)t, &n);
        if (n == 0) continue;
        PoolMatchResult fb = match_within_ids(p, q, ids, n);
        if (fb.similarity > best_fb.similarity) {
            best_fb = fb;
            best_fb.fallback = 1;
            best_fb.step_taken = 4;
            best_fb.query_type = q_type;
        }
    }

    /* If fallback found something, return it; otherwise return
       within-type result (even if weak) so caller has an answer. */
    PoolMatchResult chosen = (best_fb.similarity > within.similarity) ? best_fb : within;
    /* Resolve subtitle entry id for the chosen slot */
    int32_t eid = subtitle_track_find(&p->track, chosen.canvas_id, chosen.slot_id);
    chosen.subtitle_entry_id = (eid >= 0) ? (uint32_t)eid : 0;
    return chosen;
}

/* ── Top-K across all entries ─────────────────────────── */

uint32_t pool_match_topk(SpatialCanvasPool* p,
                         const SpatialGrid* query,
                         uint32_t k,
                         uint32_t* out_entry_ids,
                         float* out_scores) {
    if (!p || !query || !out_entry_ids || !out_scores || k == 0) return 0;
    uint32_t n = p->track.count;
    if (n == 0) return 0;
    if (k > n) k = n;

    typedef struct { uint32_t id; float sim; } Pair;
    Pair* arr = (Pair*)malloc(n * sizeof(Pair));
    if (!arr) return 0;

    for (uint32_t i = 0; i < n; i++) {
        const SubtitleEntry* e = &p->track.entries[i];
        arr[i].id = i;
        arr[i].sim = canvas_slot_cosine_a(p->canvases[e->canvas_id],
                                          e->slot_id, query);
    }
    /* Selection sort top-K in place */
    for (uint32_t i = 0; i < k; i++) {
        uint32_t max_i = i;
        for (uint32_t j = i + 1; j < n; j++) {
            if (arr[j].sim > arr[max_i].sim) max_i = j;
        }
        if (max_i != i) { Pair tmp = arr[i]; arr[i] = arr[max_i]; arr[max_i] = tmp; }
    }
    for (uint32_t i = 0; i < k; i++) {
        out_entry_ids[i] = arr[i].id;
        out_scores[i]    = arr[i].sim;
    }
    free(arr);
    return k;
}
