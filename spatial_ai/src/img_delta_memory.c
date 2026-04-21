#include "img_delta_memory.h"
#include "img_delta_compute.h"

#include <stdlib.h>
#include <string.h>

/* ── Baked SoA tables (src/img_delta_tables_data.c) ───────
 *
 * Generated offline by tools/gen_delta_tables. Linked in as plain
 * compile-time const data — no runtime build, no init guard, no
 * initialization cost. Regenerate with `make -C spatial_ai gen-tables`.
 */
extern const int16_t g_core_table    [IMG_DELTA_TABLE_N];
extern const int16_t g_link_table    [IMG_DELTA_TABLE_N];
extern const int16_t g_delta_table   [IMG_DELTA_TABLE_N];
extern const int16_t g_priority_table[IMG_DELTA_TABLE_N];
extern const uint8_t g_pattern_table [IMG_DELTA_TABLE_N];
extern const uint8_t g_direction_step[IMG_FLOW_BUCKETS][IMG_SIGN_MAX];
extern const uint8_t g_depth_step    [IMG_DEPTH_BUCKETS][IMG_SIGN_MAX];

/* ── small helpers ──────────────────────────────────────── */

static inline uint8_t sat_add_u8(uint8_t a, int delta) {
    int v = (int)a + delta;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

/* ── DeltaState packing (SPEC §3.1, §3.3) ───────────────── */

#define DS_TIER_SHIFT            0
#define DS_SCALE_SHIFT           2
#define DS_PRECISION_SHIFT       5
#define DS_SIGN_SHIFT            7
#define DS_TICK_SHIFT            9
#define DS_MODE_SHIFT           13
#define DS_CHANNEL_LAYOUT_SHIFT 16
#define DS_SLOT_SHAPE_SHIFT     19

#define DS_TIER_MASK            (0x3u   << DS_TIER_SHIFT)            /* 2 bits */
#define DS_SCALE_MASK           (0x7u   << DS_SCALE_SHIFT)           /* 3 bits */
#define DS_PRECISION_MASK       (0x3u   << DS_PRECISION_SHIFT)       /* 2 bits */
#define DS_SIGN_MASK            (0x3u   << DS_SIGN_SHIFT)            /* 2 bits */
#define DS_TICK_MASK            (0xFu   << DS_TICK_SHIFT)            /* 4 bits */
#define DS_MODE_MASK            (0x7u   << DS_MODE_SHIFT)            /* 3 bits */
#define DS_CHANNEL_LAYOUT_MASK  (0x7u   << DS_CHANNEL_LAYOUT_SHIFT)  /* 3 bits */
#define DS_SLOT_SHAPE_MASK      (0xFu   << DS_SLOT_SHAPE_SHIFT)      /* 4 bits */

static inline uint32_t ds_clamp(uint32_t v, uint32_t max) {
    return (v < max) ? v : (max - 1);
}

ImgDeltaState img_delta_state_make(uint8_t tier, uint8_t scale,
                                   uint8_t precision, uint8_t sign,
                                   uint8_t tick, uint8_t mode,
                                   uint8_t channel_layout,
                                   uint8_t slot_shape) {
    return ((uint32_t)ds_clamp(tier,           IMG_TIER_MAX)           << DS_TIER_SHIFT)
         | ((uint32_t)ds_clamp(scale,          IMG_SCALE_MAX)          << DS_SCALE_SHIFT)
         | ((uint32_t)ds_clamp(precision,      IMG_PRECISION_MAX)      << DS_PRECISION_SHIFT)
         | ((uint32_t)ds_clamp(sign,           IMG_SIGN_MAX)           << DS_SIGN_SHIFT)
         | ((uint32_t)ds_clamp(tick,           IMG_TICK_MAX)           << DS_TICK_SHIFT)
         | ((uint32_t)ds_clamp(mode,           IMG_MODE_MAX)           << DS_MODE_SHIFT)
         | ((uint32_t)ds_clamp(channel_layout, IMG_CHANNEL_LAYOUT_MAX) << DS_CHANNEL_LAYOUT_SHIFT)
         | ((uint32_t)ds_clamp(slot_shape,     IMG_SLOT_SHAPE_MAX)     << DS_SLOT_SHAPE_SHIFT);
}

ImgDeltaState img_delta_state_simple(uint8_t tier, uint8_t scale,
                                     uint8_t sign, uint8_t mode) {
    return img_delta_state_make(tier, scale, /*precision=*/0, sign,
                                /*tick=*/0, mode,
                                /*channel_layout=*/0,
                                /*slot_shape=*/0);
}

uint8_t img_delta_state_tier          (ImgDeltaState s) { return (uint8_t)((s & DS_TIER_MASK)           >> DS_TIER_SHIFT); }
uint8_t img_delta_state_scale         (ImgDeltaState s) { return (uint8_t)((s & DS_SCALE_MASK)          >> DS_SCALE_SHIFT); }
uint8_t img_delta_state_precision     (ImgDeltaState s) { return (uint8_t)((s & DS_PRECISION_MASK)      >> DS_PRECISION_SHIFT); }
uint8_t img_delta_state_sign          (ImgDeltaState s) { return (uint8_t)((s & DS_SIGN_MASK)           >> DS_SIGN_SHIFT); }
uint8_t img_delta_state_tick          (ImgDeltaState s) { return (uint8_t)((s & DS_TICK_MASK)           >> DS_TICK_SHIFT); }
uint8_t img_delta_state_mode          (ImgDeltaState s) { return (uint8_t)((s & DS_MODE_MASK)           >> DS_MODE_SHIFT); }
uint8_t img_delta_state_channel_layout(ImgDeltaState s) { return (uint8_t)((s & DS_CHANNEL_LAYOUT_MASK) >> DS_CHANNEL_LAYOUT_SHIFT); }
uint8_t img_delta_state_slot_shape    (ImgDeltaState s) { return (uint8_t)((s & DS_SLOT_SHAPE_MASK)     >> DS_SLOT_SHAPE_SHIFT); }

int img_delta_state_is_valid(ImgDeltaState s) {
    if (img_delta_state_tier          (s) >= IMG_TIER_MAX)           return 0;
    if (img_delta_state_scale         (s) >= IMG_SCALE_MAX)          return 0;
    if (img_delta_state_precision     (s) >= IMG_PRECISION_MAX)      return 0;
    if (img_delta_state_sign          (s) >= IMG_SIGN_MAX)           return 0;
    if (img_delta_state_tick          (s) >= IMG_TICK_MAX)           return 0;
    if (img_delta_state_mode          (s) >= IMG_MODE_MAX)           return 0;
    if (img_delta_state_channel_layout(s) >= IMG_CHANNEL_LAYOUT_MAX) return 0;
    if (img_delta_state_slot_shape    (s) >= IMG_SLOT_SHAPE_MAX)     return 0;
    return 1;
}

/* ── StateKey packing ───────────────────────────────────── */

#define KEY_DELTA_SIGN_SHIFT       0
#define KEY_LINK_BUCKET_SHIFT      8
#define KEY_DEPTH_CLASS_SHIFT     16
#define KEY_DIRECTION_CLASS_SHIFT 24
#define KEY_TONE_CLASS_SHIFT      32
#define KEY_SEMANTIC_ROLE_SHIFT   40

#define KEY_BYTE_MASK             0xFFULL

#define KEY_DELTA_SIGN_MASK       (KEY_BYTE_MASK << KEY_DELTA_SIGN_SHIFT)
#define KEY_LINK_BUCKET_MASK      (KEY_BYTE_MASK << KEY_LINK_BUCKET_SHIFT)
#define KEY_DEPTH_CLASS_MASK      (KEY_BYTE_MASK << KEY_DEPTH_CLASS_SHIFT)
#define KEY_DIRECTION_CLASS_MASK  (KEY_BYTE_MASK << KEY_DIRECTION_CLASS_SHIFT)
#define KEY_TONE_CLASS_MASK       (KEY_BYTE_MASK << KEY_TONE_CLASS_SHIFT)
#define KEY_SEMANTIC_ROLE_MASK    (KEY_BYTE_MASK << KEY_SEMANTIC_ROLE_SHIFT)

uint8_t img_link_bucket(uint8_t link) {
    return (uint8_t)(link >> 5);
}

ImgStateKey img_state_key_make(uint8_t role, uint8_t tone, uint8_t dir,
                               uint8_t depth, uint8_t link_bucket,
                               uint8_t sign) {
    return ((ImgStateKey)role  << KEY_SEMANTIC_ROLE_SHIFT)
         | ((ImgStateKey)tone  << KEY_TONE_CLASS_SHIFT)
         | ((ImgStateKey)dir   << KEY_DIRECTION_CLASS_SHIFT)
         | ((ImgStateKey)depth << KEY_DEPTH_CLASS_SHIFT)
         | ((ImgStateKey)link_bucket << KEY_LINK_BUCKET_SHIFT)
         | ((ImgStateKey)sign  << KEY_DELTA_SIGN_SHIFT);
}

ImgStateKey img_state_key_from_cell(const ImgCECell* cell) {
    if (!cell) return 0;
    return img_state_key_make(cell->semantic_role,
                              cell->tone_class,
                              cell->direction_class,
                              cell->depth_class,
                              img_link_bucket(cell->link),
                              cell->delta_sign);
}

uint8_t img_state_key_semantic_role  (ImgStateKey k) { return (uint8_t)((k >> KEY_SEMANTIC_ROLE_SHIFT)   & KEY_BYTE_MASK); }
uint8_t img_state_key_tone_class     (ImgStateKey k) { return (uint8_t)((k >> KEY_TONE_CLASS_SHIFT)      & KEY_BYTE_MASK); }
uint8_t img_state_key_direction_class(ImgStateKey k) { return (uint8_t)((k >> KEY_DIRECTION_CLASS_SHIFT) & KEY_BYTE_MASK); }
uint8_t img_state_key_depth_class    (ImgStateKey k) { return (uint8_t)((k >> KEY_DEPTH_CLASS_SHIFT)     & KEY_BYTE_MASK); }
uint8_t img_state_key_link_bucket    (ImgStateKey k) { return (uint8_t)((k >> KEY_LINK_BUCKET_SHIFT)     & KEY_BYTE_MASK); }
uint8_t img_state_key_delta_sign     (ImgStateKey k) { return (uint8_t)((k >> KEY_DELTA_SIGN_SHIFT)      & KEY_BYTE_MASK); }

/* Fallback chain (SPEC-aligned widening strategy). L0..L6. */
#define FALLBACK_LEVELS 7
static const ImgStateKey FALLBACK_MASKS[FALLBACK_LEVELS] = {
    /* L0 */ KEY_SEMANTIC_ROLE_MASK | KEY_TONE_CLASS_MASK | KEY_DIRECTION_CLASS_MASK |
             KEY_DEPTH_CLASS_MASK   | KEY_LINK_BUCKET_MASK | KEY_DELTA_SIGN_MASK,
    /* L1 */ KEY_SEMANTIC_ROLE_MASK | KEY_TONE_CLASS_MASK | KEY_DIRECTION_CLASS_MASK |
             KEY_DEPTH_CLASS_MASK   | KEY_DELTA_SIGN_MASK,
    /* L2 */ KEY_SEMANTIC_ROLE_MASK | KEY_TONE_CLASS_MASK | KEY_DIRECTION_CLASS_MASK |
             KEY_DEPTH_CLASS_MASK,
    /* L3 */ KEY_SEMANTIC_ROLE_MASK | KEY_DIRECTION_CLASS_MASK | KEY_DEPTH_CLASS_MASK,
    /* L4 */ KEY_SEMANTIC_ROLE_MASK | KEY_DEPTH_CLASS_MASK,
    /* L5 */ KEY_SEMANTIC_ROLE_MASK,
    /* L6 */ 0
};

/* ── Interpretation: pure SoA lookup (SPEC §13.2) ──────────
 *
 * Tables are *baked* — see src/img_delta_tables_data.c. Lookup is
 * one indexed load per channel + a small flag dispatch. No runtime
 * arithmetic over delta values, no init guard, no first-call cost.
 *
 * Pattern byte layout (per entry):
 *   bits 0..1  direction_sign   (0 none, 1 POS, 2 NEG)
 *   bits 2..3  depth_sign       (same)
 *   bits 4..5  mood_sign_fire   (0 none, 1 POS, 2 NEG)
 *   bits 6..7  role_flag        (0 none, 1 promote UNKNOWN→OBJECT,
 *                                2 demote → UNKNOWN)
 *
 * Index layout (LSB → MSB):
 *   mode | tier | scale | sign | tone | depth
 */

void img_delta_tables_init(void) {
    /* No-op: tables are compile-time const, always ready. Kept as a
     * stable entry-point so callers and tests don't need to change. */
}

uint32_t img_delta_tables_entry_count(void) {
    return (uint32_t)IMG_DELTA_TABLE_N;
}

uint32_t img_delta_tables_memory_bytes(void) {
    return (uint32_t)(sizeof(g_core_table)     + sizeof(g_link_table)
                    + sizeof(g_delta_table)    + sizeof(g_priority_table)
                    + sizeof(g_pattern_table)  + sizeof(g_direction_step)
                    + sizeof(g_depth_step));
}

void img_delta_interpret(const ImgCECell* cell,
                         const ImgDeltaPayload* payload,
                         ImgConcreteDelta* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!cell || !payload) return;

    const ImgDeltaState s = payload->state;
    const uint8_t mode  = img_delta_state_mode(s);
    const uint8_t tier  = img_delta_state_tier(s);
    const uint8_t scale = img_delta_state_scale(s);
    const uint8_t sign  = img_delta_state_sign(s);

    uint8_t tone  = (cell->tone_class  < IMG_TONE_BUCKETS)  ? cell->tone_class  : IMG_TONE_MID;
    uint8_t depth = (cell->depth_class < IMG_DEPTH_BUCKETS) ? cell->depth_class : IMG_DEPTH_MIDGROUND;

    const size_t idx = img_delta_table_idx(mode, tier, scale, sign, tone, depth);

    out->add_core     = g_core_table    [idx];
    out->add_link     = g_link_table    [idx];
    out->add_delta    = g_delta_table   [idx];
    out->add_priority = g_priority_table[idx];

    const uint8_t pat = g_pattern_table[idx];
    const uint8_t dir_sign  = (uint8_t)( pat       & 0x3);
    const uint8_t dep_sign  = (uint8_t)((pat >> 2) & 0x3);
    const uint8_t mood_sign = (uint8_t)((pat >> 4) & 0x3);
    const uint8_t role_flag = (uint8_t)((pat >> 6) & 0x3);

    if (dir_sign) {
        uint8_t cur_dir = (cell->direction_class < IMG_FLOW_BUCKETS)
                        ? cell->direction_class : 0;
        out->direction_override    = g_direction_step[cur_dir][dir_sign];
        out->direction_override_on = 1;
    }

    if (dep_sign) {
        out->depth_override    = g_depth_step[depth][dep_sign];
        out->depth_override_on = 1;
    }

    if (mood_sign) {
        out->delta_sign_override    = (mood_sign == 1) ? IMG_DELTA_POSITIVE
                                                       : IMG_DELTA_NEGATIVE;
        out->delta_sign_override_on = 1;
    }

    /* role_flag=1 promotes UNKNOWN→OBJECT; role_flag=2 demotes any→UNKNOWN.
     * The apply step additionally gates this unless role_target_on is set. */
    if (role_flag == 1 && cell->semantic_role == IMG_ROLE_UNKNOWN) {
        out->semantic_override    = IMG_ROLE_OBJECT;
        out->semantic_override_on = 1;
    } else if (role_flag == 2) {
        out->semantic_override    = IMG_ROLE_UNKNOWN;
        out->semantic_override_on = 1;
    }

    /* Explicit role target bypasses the table's role_flag logic and
     * lets the apply step honour it even on non-UNKNOWN cells. */
    if (payload->role_target_on) {
        out->semantic_override    = payload->role_target;
        out->semantic_override_on = 1;
    }
}

/* ── DeltaUnit success rate ─────────────────────────────── */

double img_delta_unit_success_rate(const ImgDeltaUnit* u) {
    if (!u) return 0.0;
    return (double)(u->success_count + 1) / (double)(u->usage_count + 2);
}

/* ── DeltaMemory storage ────────────────────────────────── */

struct ImgDeltaMemory {
    ImgDeltaUnit* units;
    uint32_t      count;
    uint32_t      capacity;
};

ImgDeltaMemory* img_delta_memory_create(void) {
    ImgDeltaMemory* m = (ImgDeltaMemory*)calloc(1, sizeof(ImgDeltaMemory));
    if (!m) return NULL;
    m->capacity = 16;
    m->units = (ImgDeltaUnit*)calloc(m->capacity, sizeof(ImgDeltaUnit));
    if (!m->units) { free(m); return NULL; }
    return m;
}

void img_delta_memory_destroy(ImgDeltaMemory* m) {
    if (!m) return;
    if (m->units) free(m->units);
    free(m);
}

uint32_t img_delta_memory_count(const ImgDeltaMemory* m) {
    return m ? m->count : 0;
}

static int memory_grow(ImgDeltaMemory* m) {
    uint32_t new_cap = m->capacity * 2;
    ImgDeltaUnit* p = (ImgDeltaUnit*)realloc(m->units,
                                             new_cap * sizeof(ImgDeltaUnit));
    if (!p) return 0;
    memset(p + m->capacity, 0,
           (new_cap - m->capacity) * sizeof(ImgDeltaUnit));
    m->units = p;
    m->capacity = new_cap;
    return 1;
}

uint32_t img_delta_memory_add_with_hint(ImgDeltaMemory* m,
                                         ImgStateKey pre_key,
                                         ImgDeltaPayload payload,
                                         ImgStateKey post_hint) {
    if (!m) return IMG_DELTA_ID_NONE;
    if (m->count >= m->capacity && !memory_grow(m)) {
        return IMG_DELTA_ID_NONE;
    }
    uint32_t id = m->count++;
    ImgDeltaUnit* u = &m->units[id];
    u->id            = id;
    u->pre_key       = pre_key;
    u->payload       = payload;
    u->post_hint     = post_hint;
    u->has_post_hint = (post_hint != 0) ? 1 : 0;
    u->usage_count   = 0;
    u->success_count = 0;
    u->weight        = (uint16_t)IMG_DELTA_WEIGHT_DEFAULT;
    u->_pad          = 0;
    return id;
}

uint32_t img_delta_memory_add(ImgDeltaMemory* m,
                              ImgStateKey pre_key,
                              ImgDeltaPayload payload) {
    return img_delta_memory_add_with_hint(m, pre_key, payload, 0);
}

uint32_t img_delta_memory_add_weighted(ImgDeltaMemory* m,
                                       ImgStateKey pre_key,
                                       ImgDeltaPayload payload,
                                       uint16_t weight) {
    uint32_t id = img_delta_memory_add_with_hint(m, pre_key, payload, 0);
    if (id == IMG_DELTA_ID_NONE) return id;
    /* A weight of 0 would zero out the weight nudge AND divide-by-zero
     * anyone computing a normalised factor — clamp up to 1. */
    m->units[id].weight = weight ? weight : 1u;
    return id;
}

const ImgDeltaUnit* img_delta_memory_get(const ImgDeltaMemory* m,
                                          uint32_t id) {
    if (!m || id >= m->count) return NULL;
    return &m->units[id];
}

uint32_t img_delta_memory_candidates(const ImgDeltaMemory* m,
                                     ImgStateKey key,
                                     const ImgDeltaUnit** out,
                                     uint32_t max_out,
                                     int* out_level) {
    if (out_level) *out_level = -1;
    if (!m || m->count == 0 || !out || max_out == 0) return 0;

    for (int level = 0; level < FALLBACK_LEVELS; level++) {
        ImgStateKey mask   = FALLBACK_MASKS[level];
        ImgStateKey target = key & mask;
        uint32_t found = 0;
        for (uint32_t i = 0; i < m->count && found < max_out; i++) {
            if ((m->units[i].pre_key & mask) == target) {
                out[found++] = &m->units[i];
            }
        }
        if (found > 0) {
            if (out_level) *out_level = level;
            return found;
        }
    }
    return 0;
}

double img_delta_score(const ImgDeltaUnit* unit,
                       const ImgCECell* current,
                       int fallback_level) {
    if (!unit || !current) return 0.0;
    double s = 0.0;
    if (img_state_key_semantic_role  (unit->pre_key) == current->semantic_role)   s += 0.35;
    if (img_state_key_direction_class(unit->pre_key) == current->direction_class) s += 0.20;
    if (img_state_key_depth_class    (unit->pre_key) == current->depth_class)     s += 0.20;
    s += 0.25 * img_delta_unit_success_rate(unit);
    if (fallback_level > 0) s -= 0.05 * (double)fallback_level;

    /* Weight nudge: rare deltas (weight above baseline) get a small
     * push; baseline weight contributes 0; below-baseline (which
     * learning never produces, only manual inserts) is capped so it
     * can't dominate — weak signals still survive. */
    const double delta_w = ((double)unit->weight - (double)IMG_DELTA_WEIGHT_DEFAULT)
                         / (double)IMG_DELTA_WEIGHT_DEFAULT;
    double nudge = 0.10 * delta_w;
    if (nudge >  0.30) nudge =  0.30;
    if (nudge < -0.10) nudge = -0.10;
    s += nudge;

    return s;
}

const ImgDeltaUnit* img_delta_memory_best(const ImgDeltaMemory* m,
                                           const ImgCECell* current,
                                           double* out_score,
                                           int* out_level) {
    if (out_score) *out_score = 0.0;
    if (out_level) *out_level = -1;
    if (!m || !current) return NULL;

    ImgStateKey key = img_state_key_from_cell(current);
    enum { MAX_CAND = 16 };
    const ImgDeltaUnit* cand[MAX_CAND];
    int level = -1;
    uint32_t n = img_delta_memory_candidates(m, key, cand, MAX_CAND, &level);
    if (n == 0) return NULL;

    const ImgDeltaUnit* best = NULL;
    double best_s = -1.0;
    for (uint32_t i = 0; i < n; i++) {
        double s = img_delta_score(cand[i], current, level);
        if (s > best_s) { best_s = s; best = cand[i]; }
    }
    if (out_score) *out_score = best_s;
    if (out_level) *out_level = level;
    return best;
}

uint32_t img_delta_memory_topg(const ImgDeltaMemory* m,
                               const ImgCECell* current,
                               uint32_t G,
                               const uint32_t* recent_counts,
                               double penalty_alpha,
                               const ImgDeltaUnit** out_units,
                               double* out_scores,
                               int* out_level) {
    if (out_level) *out_level = -1;
    if (!m || !current || !out_units || G == 0) return 0;

    ImgStateKey key = img_state_key_from_cell(current);
    enum { MAX_CAND = 32 };
    const ImgDeltaUnit* cand[MAX_CAND];
    int level = -1;
    uint32_t n = img_delta_memory_candidates(m, key, cand, MAX_CAND, &level);
    if (n == 0) return 0;

    double scores[MAX_CAND];
    for (uint32_t i = 0; i < n; i++) {
        double s = img_delta_score(cand[i], current, level);
        if (recent_counts) {
            s -= penalty_alpha * (double)recent_counts[cand[i]->id];
        }
        scores[i] = s;
    }

    /* Partial insertion sort descending — up to G entries. n ≤ 32 so
     * full sort is cheap. */
    uint32_t write = (G < n) ? G : n;
    for (uint32_t slot = 0; slot < write; slot++) {
        uint32_t best_i = slot;
        for (uint32_t j = slot + 1; j < n; j++) {
            if (scores[j] > scores[best_i]) best_i = j;
        }
        if (best_i != slot) {
            const ImgDeltaUnit* tu = cand[slot];
            double ts = scores[slot];
            cand[slot] = cand[best_i];
            scores[slot] = scores[best_i];
            cand[best_i] = tu;
            scores[best_i] = ts;
        }
        out_units[slot] = cand[slot];
        if (out_scores) out_scores[slot] = scores[slot];
    }

    if (out_level) *out_level = level;
    return write;
}

void img_delta_memory_record_usage(ImgDeltaMemory* m,
                                   uint32_t delta_id, int success) {
    if (!m || delta_id >= m->count) return;
    ImgDeltaUnit* u = &m->units[delta_id];
    u->usage_count++;
    if (success) u->success_count++;
}

void img_delta_memory_ingest_resolve(ImgDeltaMemory* memory,
                                     const ImgCEGrid* ce,
                                     const uint8_t* outlier_mask,
                                     const uint8_t* explained_mask,
                                     ImgDeltaFeedbackStats* out) {
    ImgDeltaFeedbackStats local = {0, 0, 0};

    if (memory && ce && ce->cells) {
        const uint32_t n = ce->width * ce->height;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t id = ce->cells[i].last_delta_id;
            if (id == IMG_DELTA_ID_NONE) { local.skipped_untouched++; continue; }
            if (id >= memory->count)     { local.skipped_untouched++; continue; }

            int success = 1;
            const uint8_t o = outlier_mask   ? outlier_mask[i]   : 0;
            const uint8_t e = explained_mask ? explained_mask[i] : 0;
            if (o && !e) success = 0;   /* promoted (unexplained) → failure */

            /* img_delta_apply has already bumped usage_count for every
             * delta that landed on a cell. Ingest only records the
             * outcome — success bumps success_count, failure leaves
             * it alone. */
            if (success) {
                memory->units[id].success_count++;
                local.credited_success++;
            } else {
                local.credited_failure++;
            }
        }
    }

    if (out) *out = local;
}

/* ── Constrained apply ──────────────────────────────────── */

void img_delta_apply(ImgCECell* cell,
                     ImgDeltaMemory* m,
                     const ImgDeltaUnit* unit) {
    if (!cell || !unit) return;

    ImgConcreteDelta cd;
    img_delta_interpret(cell, &unit->payload, &cd);

    /* Constraint: direction may only rotate ±1 step from current. */
    if (cd.direction_override_on) {
        int diff = (int)cd.direction_override - (int)cell->direction_class;
        if (diff > 1)  cd.direction_override = (uint8_t)((int)cell->direction_class + 1);
        if (diff < -1) cd.direction_override = (uint8_t)((int)cell->direction_class - 1);
    }
    /* Constraint: depth may only change by ±1 step. */
    if (cd.depth_override_on) {
        int diff = (int)cd.depth_override - (int)cell->depth_class;
        if (diff > 1)  cd.depth_override = (uint8_t)((int)cell->depth_class + 1);
        if (diff < -1) cd.depth_override = (uint8_t)((int)cell->depth_class - 1);
    }
    /* Constraint: role override honored only when role_target_on or
     * the current role is UNKNOWN. */
    if (cd.semantic_override_on
        && cell->semantic_role != IMG_ROLE_UNKNOWN
        && !unit->payload.role_target_on) {
        cd.semantic_override_on = 0;
    }

    cell->core     = sat_add_u8(cell->core,     cd.add_core);
    cell->link     = sat_add_u8(cell->link,     cd.add_link);
    cell->delta    = sat_add_u8(cell->delta,    cd.add_delta);
    cell->priority = sat_add_u8(cell->priority, cd.add_priority);

    if (cd.semantic_override_on)   cell->semantic_role   = cd.semantic_override;
    if (cd.depth_override_on)      cell->depth_class     = cd.depth_override;
    if (cd.direction_override_on)  cell->direction_class = cd.direction_override;
    if (cd.delta_sign_override_on) cell->delta_sign      = cd.delta_sign_override;

    cell->last_delta_id = unit->id;

    if (m && unit->id < m->count) {
        m->units[unit->id].usage_count++;
    }
}

/* ── Persistence ──────────────────────────────────────── */

#include <stdio.h>

#define IMEM_MAGIC    "IMEM"
#define IMEM_VERSION  1u

const char* img_delta_memory_status_str(ImemStatus s) {
    switch (s) {
        case IMEM_OK:          return "ok";
        case IMEM_ERR_OPEN:    return "open";
        case IMEM_ERR_READ:    return "short read";
        case IMEM_ERR_WRITE:   return "short write";
        case IMEM_ERR_MAGIC:   return "bad magic";
        case IMEM_ERR_VERSION: return "unsupported version";
        case IMEM_ERR_ALLOC:   return "allocation failed";
    }
    return "unknown";
}

/* 40 bytes on disk — fields packed explicitly, not struct-layout
 * dependent. */
static int imem_write_unit(FILE* fp, const ImgDeltaUnit* u) {
    uint32_t state = u->payload.state;
    uint8_t  hh    = u->has_post_hint;
    uint8_t  rt    = u->payload.role_target;
    uint8_t  rto   = u->payload.role_target_on;
    uint8_t  pad1  = 0;
    uint16_t pad2  = 0;

    if (fwrite(&u->id,            4, 1, fp) != 1) return 0;
    if (fwrite(&u->pre_key,       8, 1, fp) != 1) return 0;
    if (fwrite(&u->post_hint,     8, 1, fp) != 1) return 0;
    if (fwrite(&hh,               1, 1, fp) != 1) return 0;
    if (fwrite(&rt,               1, 1, fp) != 1) return 0;
    if (fwrite(&rto,              1, 1, fp) != 1) return 0;
    if (fwrite(&pad1,             1, 1, fp) != 1) return 0;
    if (fwrite(&state,            4, 1, fp) != 1) return 0;
    if (fwrite(&u->usage_count,   4, 1, fp) != 1) return 0;
    if (fwrite(&u->success_count, 4, 1, fp) != 1) return 0;
    if (fwrite(&u->weight,        2, 1, fp) != 1) return 0;
    if (fwrite(&pad2,             2, 1, fp) != 1) return 0;
    return 1;
}

static int imem_read_unit(FILE* fp, ImgDeltaUnit* u) {
    uint32_t state;
    uint8_t  hh, rt, rto, pad1;
    uint16_t pad2;

    if (fread(&u->id,            4, 1, fp) != 1) return 0;
    if (fread(&u->pre_key,       8, 1, fp) != 1) return 0;
    if (fread(&u->post_hint,     8, 1, fp) != 1) return 0;
    if (fread(&hh,               1, 1, fp) != 1) return 0;
    if (fread(&rt,               1, 1, fp) != 1) return 0;
    if (fread(&rto,              1, 1, fp) != 1) return 0;
    if (fread(&pad1,             1, 1, fp) != 1) return 0;
    if (fread(&state,            4, 1, fp) != 1) return 0;
    if (fread(&u->usage_count,   4, 1, fp) != 1) return 0;
    if (fread(&u->success_count, 4, 1, fp) != 1) return 0;
    if (fread(&u->weight,        2, 1, fp) != 1) return 0;
    if (fread(&pad2,             2, 1, fp) != 1) return 0;

    memset(&u->payload, 0, sizeof(u->payload));
    u->payload.state           = state;
    u->payload.role_target     = rt;
    u->payload.role_target_on  = rto;
    u->has_post_hint           = hh;
    u->_pad                    = 0;
    return 1;
}

ImemStatus img_delta_memory_save(const ImgDeltaMemory* m, const char* path) {
    if (!m || !path) return IMEM_ERR_OPEN;
    FILE* fp = fopen(path, "wb");
    if (!fp) return IMEM_ERR_OPEN;

    uint32_t version = IMEM_VERSION;
    uint32_t count   = m->count;
    uint32_t reserved = 0;
    if (fwrite(IMEM_MAGIC, 1, 4, fp)  != 4) { fclose(fp); return IMEM_ERR_WRITE; }
    if (fwrite(&version,   4, 1, fp)  != 1) { fclose(fp); return IMEM_ERR_WRITE; }
    if (fwrite(&count,     4, 1, fp)  != 1) { fclose(fp); return IMEM_ERR_WRITE; }
    if (fwrite(&reserved,  4, 1, fp)  != 1) { fclose(fp); return IMEM_ERR_WRITE; }

    for (uint32_t i = 0; i < m->count; i++) {
        if (!imem_write_unit(fp, &m->units[i])) {
            fclose(fp);
            return IMEM_ERR_WRITE;
        }
    }

    if (fclose(fp) != 0) return IMEM_ERR_WRITE;
    return IMEM_OK;
}

ImgDeltaMemory* img_delta_memory_load(const char* path,
                                      ImemStatus* out_status) {
    if (out_status) *out_status = IMEM_OK;
    if (!path) { if (out_status) *out_status = IMEM_ERR_OPEN; return NULL; }

    FILE* fp = fopen(path, "rb");
    if (!fp) { if (out_status) *out_status = IMEM_ERR_OPEN; return NULL; }

    char     magic[4];
    uint32_t version, count, reserved;
    if (fread(magic,    1, 4, fp) != 4 ||
        fread(&version, 4, 1, fp) != 1 ||
        fread(&count,   4, 1, fp) != 1 ||
        fread(&reserved,4, 1, fp) != 1) {
        fclose(fp); if (out_status) *out_status = IMEM_ERR_READ; return NULL;
    }
    if (memcmp(magic, IMEM_MAGIC, 4) != 0) {
        fclose(fp); if (out_status) *out_status = IMEM_ERR_MAGIC; return NULL;
    }
    if (version != IMEM_VERSION) {
        fclose(fp); if (out_status) *out_status = IMEM_ERR_VERSION; return NULL;
    }

    ImgDeltaMemory* m = img_delta_memory_create();
    if (!m) {
        fclose(fp); if (out_status) *out_status = IMEM_ERR_ALLOC; return NULL;
    }

    /* Ensure capacity for the whole batch up front. */
    while (m->capacity < count) {
        if (!memory_grow(m)) {
            img_delta_memory_destroy(m); fclose(fp);
            if (out_status) *out_status = IMEM_ERR_ALLOC;
            return NULL;
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        ImgDeltaUnit u;
        memset(&u, 0, sizeof(u));
        if (!imem_read_unit(fp, &u)) {
            img_delta_memory_destroy(m); fclose(fp);
            if (out_status) *out_status = IMEM_ERR_READ;
            return NULL;
        }
        m->units[i] = u;
    }
    m->count = count;

    fclose(fp);
    return m;
}
