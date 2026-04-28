#ifndef SCULPT_TUNING_H
#define SCULPT_TUNING_H

/* Phase 8 — runtime-mutable tuning surface.
 *
 * Pre-Phase-8, every parameter was a `static const` array compiled into the
 * binary; tuning required a recompile. Phase 8 moves the same values into a
 * single mutable struct (`SCULPT_TUNING`) with a `_reset_default` reset.
 * Existing call sites keep using the old `SCULPT_LEVEL_MARGIN[lv]` macro —
 * those macros now expand to `SCULPT_TUNING.level_margin[lv]`, so no
 * mechanical rewrite of draw.c / learn.c is needed.
 *
 * Two coherence-related constants that used to be single scalars have been
 * promoted to per-level arrays so `tune_sculpt` can sweep them per tier
 * (the L3 "atmosphere" tier needs a far looser threshold than L0 "point").
 * The pre-Phase-8 default fills every level with the old scalar, so any
 * test that previously relied on the constants still sees identical
 * behavior unless the caller overrides.
 */

#define SCULPT_GRID_SIZE   16
#define SCULPT_NUM_LEVELS  4

typedef struct {
    int level_margin[SCULPT_NUM_LEVELS];
    int level_blur_box[SCULPT_NUM_LEVELS];
    int level_top_g[SCULPT_NUM_LEVELS];
    int default_iters[SCULPT_NUM_LEVELS];

    /* Per-level coherence parameters. The threshold is the per-channel
     * cutoff; draw.c multiplies by 4 to compare against summed |Δ| over
     * RGBA. bonus/penalty are score deltas applied per matching neighbor.
     */
    int coherence_threshold[SCULPT_NUM_LEVELS];
    int coherence_bonus[SCULPT_NUM_LEVELS];
    int coherence_penalty[SCULPT_NUM_LEVELS];

    /* Reserved for the alpha-band heuristic (declared since Phase 1
     * prototype, not consumed by draw yet). Surface it as a tunable knob
     * now so future work can flip it on without another structural edit.
     */
    int a_band_width;
} sculpt_tuning_t;

extern sculpt_tuning_t SCULPT_TUNING;

/* Reset SCULPT_TUNING to the Phase 1–7 baked defaults. Tests should call
 * this before exercising draw/learn so prior tuning overrides don't leak.
 */
void sculpt_tuning_reset_default(void);

/* Backwards-compatibility shims for existing call sites. */
#define SCULPT_LEVEL_MARGIN     SCULPT_TUNING.level_margin
#define SCULPT_LEVEL_BLUR_BOX   SCULPT_TUNING.level_blur_box
#define SCULPT_TOP_G            SCULPT_TUNING.level_top_g
#define SCULPT_DEFAULT_ITERS    SCULPT_TUNING.default_iters
#define SCULPT_A_BAND_WIDTH     (SCULPT_TUNING.a_band_width)

#endif /* SCULPT_TUNING_H */
