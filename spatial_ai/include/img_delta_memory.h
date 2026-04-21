#ifndef IMG_DELTA_MEMORY_H
#define IMG_DELTA_MEMORY_H

#include "img_ce.h"

/*
 * img_delta_memory — incremental-rule store that drives CE generation.
 * See SPEC-CE.md for the full engine contract.
 *
 *   "CE는 작은 정수 기반의 유한 상태를 저장하고, precomputed lookup을
 *    통해 큰 값과 구조를 즉시 재구성하는 SIMD 친화적 상태 재개 엔진이다."
 *
 * A Delta is NOT a scalar diff. It is a bounded-integer resume code
 * whose axes (tier, scale, precision, sign, tick, mode, channel_layout,
 * slot_shape) parameterise a single point in a finite state space.
 * Every axis carries a MAX_* constant so the total state volume is
 * known at compile time — that is what enables O(1) lookup tables
 * and SIMD-friendly processing.
 *
 *   Phase A (this file): bounded index tuple + u32 packing + getters.
 *   Phase B (TODO):      replace runtime arithmetic in img_delta_interpret
 *                        with precomputed SoA lookup tables per §13.
 */

/* ── DeltaState axes (SPEC §3.1) ─────────────────────────── */

/* Tier (SPEC §10) */
typedef enum {
    IMG_TIER_NONE = 0,
    IMG_TIER_T1   = 1,   /* 미세 */
    IMG_TIER_T2   = 2,   /* 중간 */
    IMG_TIER_T3   = 3,   /* 구조 */
    IMG_TIER_MAX  = 4    /* bound */
} ImgTier;

/* Sign (resume code polarity) */
typedef enum {
    IMG_SIGN_ZERO = 0,
    IMG_SIGN_POS  = 1,
    IMG_SIGN_NEG  = 2,
    IMG_SIGN_MAX  = 4    /* bound — 2 bits */
} ImgSign;

/* Mode (which channel / tag family this delta activates) */
typedef enum {
    IMG_MODE_NONE      = 0,
    IMG_MODE_INTENSITY = 1,   /* core, tone-scaled */
    IMG_MODE_LINK      = 2,   /* link */
    IMG_MODE_DIRECTION = 3,   /* direction_class ±1 */
    IMG_MODE_DEPTH     = 4,   /* depth_class ±1 */
    IMG_MODE_MOOD      = 5,   /* delta channel + delta_sign */
    IMG_MODE_ROLE      = 6,   /* semantic_role (gated) */
    IMG_MODE_PRIORITY  = 7,   /* priority, depth-scaled */
    IMG_MODE_MAX       = 8    /* bound — 3 bits */
} ImgMode;

/* Bounds for the remaining axes. Keep these tight — the cartesian
 * product is what lookup tables will be sized against in Phase B. */
#define IMG_SCALE_MAX           8   /* 3 bits */
#define IMG_PRECISION_MAX       4   /* 2 bits */
#define IMG_TICK_MAX           16   /* 4 bits */
#define IMG_CHANNEL_LAYOUT_MAX  8   /* 3 bits */
#define IMG_SLOT_SHAPE_MAX     16   /* 4 bits */

/* Total bits used: 2+3+2+2+4+3+3+4 = 23 → fits in uint32_t with room
 * for future axes (§17 확장). */

typedef uint32_t ImgDeltaState;

/* Bit layout (LSB → MSB)
 *   [0..1]   tier_idx            (IMG_TIER_MAX)
 *   [2..4]   scale_idx           (IMG_SCALE_MAX)
 *   [5..6]   precision_idx       (IMG_PRECISION_MAX)
 *   [7..8]   sign_idx            (IMG_SIGN_MAX)
 *   [9..12]  tick_idx            (IMG_TICK_MAX)
 *   [13..15] mode_idx            (IMG_MODE_MAX)
 *   [16..18] channel_layout_idx  (IMG_CHANNEL_LAYOUT_MAX)
 *   [19..22] slot_shape_idx      (IMG_SLOT_SHAPE_MAX)
 *   [23..31] reserved
 */

ImgDeltaState img_delta_state_make(uint8_t tier,
                                   uint8_t scale,
                                   uint8_t precision,
                                   uint8_t sign,
                                   uint8_t tick,
                                   uint8_t mode,
                                   uint8_t channel_layout,
                                   uint8_t slot_shape);

/* Minimal-ceremony constructor: only tier/scale/sign/mode specified. */
ImgDeltaState img_delta_state_simple(uint8_t tier,
                                     uint8_t scale,
                                     uint8_t sign,
                                     uint8_t mode);

uint8_t img_delta_state_tier          (ImgDeltaState s);
uint8_t img_delta_state_scale         (ImgDeltaState s);
uint8_t img_delta_state_precision     (ImgDeltaState s);
uint8_t img_delta_state_sign          (ImgDeltaState s);
uint8_t img_delta_state_tick          (ImgDeltaState s);
uint8_t img_delta_state_mode          (ImgDeltaState s);
uint8_t img_delta_state_channel_layout(ImgDeltaState s);
uint8_t img_delta_state_slot_shape    (ImgDeltaState s);

/* Returns 1 if every axis is within its declared MAX, else 0. */
int     img_delta_state_is_valid      (ImgDeltaState s);

/* Payload = bounded DeltaState + optional explicit role target.
 * `role_target_on` only meaningful when mode == IMG_MODE_ROLE; when
 * set, apply can override semantic_role even on non-UNKNOWN cells. */
typedef struct {
    ImgDeltaState state;
    uint8_t       role_target;     /* an ImgSemanticRole value */
    uint8_t       role_target_on;
} ImgDeltaPayload;

/* ── StateKey (unchanged — SPEC §3.3) ────────────────────── */

typedef uint64_t ImgStateKey;

uint8_t      img_link_bucket(uint8_t link);

ImgStateKey  img_state_key_make(uint8_t semantic_role,
                                uint8_t tone_class,
                                uint8_t direction_class,
                                uint8_t depth_class,
                                uint8_t link_bucket,
                                uint8_t delta_sign);

ImgStateKey  img_state_key_from_cell(const ImgCECell* cell);

uint8_t      img_state_key_semantic_role  (ImgStateKey k);
uint8_t      img_state_key_tone_class     (ImgStateKey k);
uint8_t      img_state_key_direction_class(ImgStateKey k);
uint8_t      img_state_key_depth_class    (ImgStateKey k);
uint8_t      img_state_key_link_bucket    (ImgStateKey k);
uint8_t      img_state_key_delta_sign     (ImgStateKey k);

/* ── Concrete delta (interpreter output — SPEC §4.3) ─────── */

typedef struct {
    int16_t add_core;
    int16_t add_link;
    int16_t add_delta;
    int16_t add_priority;

    uint8_t semantic_override;
    uint8_t semantic_override_on;

    uint8_t depth_override;
    uint8_t depth_override_on;

    uint8_t direction_override;
    uint8_t direction_override_on;

    uint8_t delta_sign_override;
    uint8_t delta_sign_override_on;
} ImgConcreteDelta;

/* Expand a bounded DeltaState payload against the cell's tags.
 *
 * Phase B: pure SoA table lookup per SPEC §13.2. No runtime
 * arithmetic over channel values — every channel contribution comes
 * from precomputed tables keyed by (mode, tier, scale, sign, tone,
 * depth). Direction / depth clamps use tiny precomputed step tables.
 *
 * Tables are built lazily on first call. Call img_delta_tables_init
 * at startup to avoid the cold-init cost.
 */
void img_delta_interpret(const ImgCECell* cell,
                         const ImgDeltaPayload* payload,
                         ImgConcreteDelta* out);

/* Build the SoA lookup tables eagerly. Safe to call repeatedly —
 * subsequent calls are no-ops. */
void img_delta_tables_init(void);

/* Total bytes of static memory used by the Phase B tables. Useful
 * for asserting SPEC §13.6 cache-tier budgets. */
uint32_t img_delta_tables_memory_bytes(void);

/* Number of entries in each SoA lookup array (the cartesian product
 * of (mode, tier, scale, sign, tone, depth)). */
uint32_t img_delta_tables_entry_count(void);

/* ── DeltaUnit / DeltaMemory ─────────────────────────────── */

/* Baseline weight for a "normal" delta (see ImgDeltaUnit.weight).
 * Callers that want a neutral insert pass IMG_DELTA_WEIGHT_DEFAULT.
 * Learning can boost weight above this value to amplify rare
 * deltas, but never filters below baseline — the point is that weak
 * signals still survive, just with less selection pressure. */
#define IMG_DELTA_WEIGHT_DEFAULT  1000u

typedef struct {
    uint32_t        id;
    ImgStateKey     pre_key;
    ImgDeltaPayload payload;
    ImgStateKey     post_hint;     /* 0 if unset */
    uint8_t         has_post_hint;

    uint32_t usage_count;
    uint32_t success_count;

    /* Rarity / learning-rate modulator. 1000 = baseline; higher =
     * rare at insert time (gets a selection nudge via img_delta_score
     * and wins tiebreaks); lower is possible but not recommended —
     * we don't filter, we just push rare evidence up. */
    uint16_t weight;
    uint16_t _pad;                 /* alignment */
} ImgDeltaUnit;

double img_delta_unit_success_rate(const ImgDeltaUnit* unit);

typedef struct ImgDeltaMemory ImgDeltaMemory;

ImgDeltaMemory* img_delta_memory_create(void);
void            img_delta_memory_destroy(ImgDeltaMemory* m);

uint32_t        img_delta_memory_count(const ImgDeltaMemory* m);

uint32_t        img_delta_memory_add(ImgDeltaMemory* m,
                                     ImgStateKey pre_key,
                                     ImgDeltaPayload payload);

uint32_t        img_delta_memory_add_with_hint(ImgDeltaMemory* m,
                                               ImgStateKey pre_key,
                                               ImgDeltaPayload payload,
                                               ImgStateKey post_hint);

/* Like img_delta_memory_add but sets the unit's weight explicitly.
 * Useful when the caller has a rarity / confidence estimate from
 * the learning signal. A weight of 0 clamps up to 1 to keep the
 * scoring contribution finite. */
uint32_t        img_delta_memory_add_weighted(ImgDeltaMemory* m,
                                              ImgStateKey pre_key,
                                              ImgDeltaPayload payload,
                                              uint16_t weight);

const ImgDeltaUnit* img_delta_memory_get(const ImgDeltaMemory* m,
                                          uint32_t id);

uint32_t        img_delta_memory_candidates(const ImgDeltaMemory* m,
                                            ImgStateKey key,
                                            const ImgDeltaUnit** out,
                                            uint32_t max_out,
                                            int* out_level);

double          img_delta_score(const ImgDeltaUnit* unit,
                                const ImgCECell* current,
                                int fallback_level);

const ImgDeltaUnit* img_delta_memory_best(const ImgDeltaMemory* m,
                                           const ImgCECell* current,
                                           double* out_score,
                                           int* out_level);

/* Top-G selection with optional presence penalty — analogue of a
 * language model's top-k sampling, adapted to per-cell delta choice.
 *
 *   1. Collect candidates via the same fallback chain as _best.
 *   2. Score each with img_delta_score(..., fallback_level).
 *   3. If `recent_counts` is non-NULL, subtract
 *        penalty_alpha × (double)recent_counts[unit->id]
 *      from each score. Drives diversity within a single drawing
 *      pass: a delta picked often in recent stamps loses score
 *      proportionally, giving under-used alternatives a chance.
 *   4. Sort descending by adjusted score; copy up to G into
 *      out_units (and out_scores if provided).
 *
 * `recent_counts` must be sized ≥ img_delta_memory_count(m) when set.
 * Returns the number of entries written (≤ G). Writes the fallback
 * level into *out_level when non-NULL (same semantics as _best). */
uint32_t img_delta_memory_topg(const ImgDeltaMemory* m,
                               const ImgCECell* current,
                               uint32_t G,
                               const uint32_t* recent_counts_or_null,
                               double penalty_alpha,
                               const ImgDeltaUnit** out_units,
                               double* out_scores_or_null,
                               int* out_level);

void            img_delta_memory_record_usage(ImgDeltaMemory* m,
                                              uint32_t delta_id,
                                              int success);

/* ── Auto-feedback from resolve ──────────────────────────────
 *
 * Walks a CE grid together with resolve's outlier / explained
 * masks and feeds the outcome of each applied delta back into
 * DeltaMemory usage / success counters. Outcome rule per cell:
 *
 *   last_delta_id == IMG_DELTA_ID_NONE
 *       → nothing touched this cell; skip.
 *   outlier_mask[i] == 0
 *       → cell did not stick out after delta → success.
 *   outlier_mask[i] == 1 && explained_mask[i] == 1
 *       → outlier absorbed by resolve (shared role/direction) →
 *         soft success (the delta was workable and reconciled).
 *   outlier_mask[i] == 1 && explained_mask[i] == 0
 *       → outlier promoted; resolve could not explain it → failure.
 *
 * Both masks may be NULL (treated as all-zero, i.e. everything is
 * counted as success). Stats struct is optional. */
typedef struct {
    uint32_t credited_success;
    uint32_t credited_failure;
    uint32_t skipped_untouched;   /* cells with last_delta_id == NONE */
} ImgDeltaFeedbackStats;

void            img_delta_memory_ingest_resolve(
                    ImgDeltaMemory* memory,
                    const ImgCEGrid* ce,
                    const uint8_t* outlier_mask_or_null,
                    const uint8_t* explained_mask_or_null,
                    ImgDeltaFeedbackStats* out_or_null);

/* Constrained apply (unchanged constraints: dir/depth ±1, role gated). */
void            img_delta_apply(ImgCECell* cell,
                                ImgDeltaMemory* m,
                                const ImgDeltaUnit* unit);

/* ── Persistence ──────────────────────────────────────────
 *
 * Compact binary format "IMEM" — header (16 bytes) + N records
 * of 40 bytes each (packed explicitly, little-endian native).
 *   magic[4]      = "IMEM"
 *   version       = uint32
 *   count         = uint32
 *   reserved      = uint32
 *   records[count]:
 *     id                  u32
 *     pre_key             u64
 *     post_hint           u64
 *     has_post_hint       u8
 *     payload.role_target u8
 *     payload.role_target_on u8
 *     _pad                u8   (align to next field)
 *     payload.state       u32
 *     usage_count         u32
 *     success_count       u32
 *     weight              u16
 *     _pad                u16
 *
 * Older readers that don't understand a newer `version` return
 * IMEM_ERR_VERSION; the format is bumped only when the record
 * layout changes. */
typedef enum {
    IMEM_OK = 0,
    IMEM_ERR_OPEN,
    IMEM_ERR_READ,
    IMEM_ERR_WRITE,
    IMEM_ERR_MAGIC,
    IMEM_ERR_VERSION,
    IMEM_ERR_ALLOC
} ImemStatus;

const char*      img_delta_memory_status_str(ImemStatus s);

/* Write every unit in `m` to `path`. Returns IMEM_OK on success. */
ImemStatus       img_delta_memory_save(const ImgDeltaMemory* m,
                                       const char* path);

/* Allocate a fresh ImgDeltaMemory and populate it from `path`.
 * Returns NULL on failure; out_status (if non-NULL) is written. */
ImgDeltaMemory*  img_delta_memory_load(const char* path,
                                       ImemStatus* out_status);

#endif /* IMG_DELTA_MEMORY_H */
