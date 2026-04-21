#ifndef IMG_CE_H
#define IMG_CE_H

#include <stdint.h>
#include <stdlib.h>

/*
 * img_ce — hierarchical image compression/interpretation engine.
 *
 * Pipeline (matches the design doc):
 *
 *   source image  (e.g. 512x512, 1024x1024 RGB)
 *        │
 *        ▼                                       1st compression
 *   SmallCanvas  (IMG_SC_SIZE × IMG_SC_SIZE, RGBA semantic channels)
 *        │
 *        ▼                                       2nd compression
 *   CE grid      (IMG_CE_SIZE × IMG_CE_SIZE, interpreted cells)
 *        │
 *        ▼
 *   delta coating (state overlay, not simple difference)
 *        │
 *        ▼
 *   resolve (sieve + repair): absorb explained outliers,
 *                             promote unexplained ones.
 *
 * Two channel semantics live in this header:
 *
 *   SmallCanvas RGBA       — input interpretation
 *     R = intensity / presence
 *     G = flow / direction / connection
 *     B = mood / style / context
 *     A = depth / occupancy / importance
 *
 *   CE RGBA                — generation / composition state
 *     R = core representative signal
 *     G = link / connection slot
 *     B = delta / mutability
 *     A = activity / confidence / priority
 *
 * A CE cell is NOT a pixel. It summarises a block of the SmallCanvas
 * with classification tags (tone, semantic role, direction, depth,
 * delta sign) so the generator can reason about meaning rather than
 * raw color.
 */

#define IMG_SC_SIZE         256
#define IMG_SC_TOTAL        (IMG_SC_SIZE * IMG_SC_SIZE)

#define IMG_CE_SIZE         64
#define IMG_CE_TOTAL        (IMG_CE_SIZE * IMG_CE_SIZE)
#define IMG_CE_BLOCK        (IMG_SC_SIZE / IMG_CE_SIZE)   /* 4 */

/* ── SmallCanvas classification tags ─────────────────────── */
typedef enum {
    IMG_TONE_DARK   = 0,
    IMG_TONE_MID    = 1,
    IMG_TONE_BRIGHT = 2
} ImgToneClass;

typedef enum {
    IMG_FLOW_NONE          = 0,
    IMG_FLOW_HORIZONTAL    = 1,
    IMG_FLOW_VERTICAL      = 2,
    IMG_FLOW_DIAGONAL_UP   = 3,
    IMG_FLOW_DIAGONAL_DOWN = 4
} ImgFlowClass;

typedef enum {
    IMG_MOOD_NEUTRAL  = 0,
    IMG_MOOD_WARM     = 1,
    IMG_MOOD_COOL     = 2,
    IMG_MOOD_DRAMATIC = 3
} ImgMoodClass;

typedef enum {
    IMG_DEPTH_BACKGROUND = 0,
    IMG_DEPTH_MIDGROUND  = 1,
    IMG_DEPTH_FOREGROUND = 2
} ImgDepthClass;

/* ── CE semantic tags (extendable) ───────────────────────── */
typedef enum {
    IMG_ROLE_UNKNOWN = 0,
    IMG_ROLE_PERSON  = 1,
    IMG_ROLE_FACE    = 2,
    IMG_ROLE_HAIR    = 3,
    IMG_ROLE_SKY     = 4,
    IMG_ROLE_GROUND  = 5,
    IMG_ROLE_OBJECT  = 6
} ImgSemanticRole;

typedef enum {
    IMG_DELTA_NONE     = 0,
    IMG_DELTA_POSITIVE = 1,
    IMG_DELTA_NEGATIVE = 2
} ImgDeltaSign;

/* ── SmallCanvas ─────────────────────────────────────────────
 * 1st compression layer. Each cell interprets the original image's
 * local patch as a 4-channel semantic summary. */
typedef struct {
    uint8_t intensity;  /* R: strength / presence */
    uint8_t flow;       /* G: ImgFlowClass        */
    uint8_t mood;       /* B: ImgMoodClass        */
    uint8_t depth;      /* A: ImgDepthClass       */
} ImgSmallCell;

typedef struct {
    ImgSmallCell* cells;   /* IMG_SC_TOTAL cells, row-major */
    uint32_t      width;   /* IMG_SC_SIZE */
    uint32_t      height;  /* IMG_SC_SIZE */
} ImgSmallCanvas;

ImgSmallCanvas* img_small_canvas_create(void);
void            img_small_canvas_destroy(ImgSmallCanvas* sc);
void            img_small_canvas_clear(ImgSmallCanvas* sc);

static inline uint32_t img_sc_idx(uint32_t y, uint32_t x) {
    return y * IMG_SC_SIZE + x;
}

/* Downsample an H×W RGB image (row-major, 3 bytes per pixel,
 * RGB order) into the 256×256 SmallCanvas. The original dimensions
 * may be any size ≥ IMG_SC_SIZE; nearest-block averaging is used. */
void img_image_to_small_canvas(const uint8_t* image_rgb,
                               uint32_t image_w, uint32_t image_h,
                               ImgSmallCanvas* out);

/* ── CE grid ─────────────────────────────────────────────────
 * 2nd compression layer. Each CE cell aggregates an IMG_CE_BLOCK ×
 * IMG_CE_BLOCK region of the SmallCanvas into an interpreted cell
 * with core/link/delta/priority plus classification tags. */
typedef struct {
    /* CE RGBA */
    uint8_t core;      /* R */
    uint8_t link;      /* G */
    uint8_t delta;     /* B */
    uint8_t priority;  /* A */

    /* interpretation tags */
    uint8_t tone_class;       /* ImgToneClass */
    uint8_t semantic_role;    /* ImgSemanticRole */
    uint8_t direction_class;  /* ImgFlowClass */
    uint8_t depth_class;      /* ImgDepthClass */
    uint8_t delta_sign;       /* ImgDeltaSign */

    /* Last delta unit applied to this cell (UINT32_MAX = none).
     * Lets resolve credit success/failure back to the originating
     * delta so DeltaMemory can learn from outcomes. */
    uint32_t last_delta_id;
} ImgCECell;

#define IMG_DELTA_ID_NONE 0xFFFFFFFFu

typedef struct ImgCEGrid {
    ImgCECell* cells;   /* IMG_CE_TOTAL cells, row-major */
    uint32_t   width;   /* IMG_CE_SIZE */
    uint32_t   height;  /* IMG_CE_SIZE */
} ImgCEGrid;

ImgCEGrid* img_ce_grid_create(void);
void       img_ce_grid_destroy(ImgCEGrid* ce);
void       img_ce_grid_clear(ImgCEGrid* ce);

static inline uint32_t img_ce_idx(uint32_t y, uint32_t x) {
    return y * IMG_CE_SIZE + x;
}

/* Re-interpret the SmallCanvas into the CE grid. Each CE cell
 * summarises an IMG_CE_BLOCK × IMG_CE_BLOCK tile and emits:
 *   core     = mean intensity
 *   link     = 64 + dominant_flow × 40  (clamped)
 *   delta    = dominant_mood    × 50    (clamped)
 *   priority = 80 + dominant_depth × 50 (clamped)
 * The classification tags are derived deterministically from the
 * dominant tile values. */
void img_small_canvas_to_ce(const ImgSmallCanvas* sc, ImgCEGrid* out);

/* ── Delta coating ───────────────────────────────────────────
 * A delta is not a scalar difference: it is a state coating that
 * can add energy (core/link/delta/priority) and/or override the
 * classification tags on the underlying cell. Each override field
 * is applied only when its *_override_on flag is set; this keeps
 * the struct POD-friendly while supporting optional overrides. */
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
} ImgDeltaCoating;

/* Apply `coating` to the cell at (y, x) in `ce`. Saturates uint8
 * channel adds at [0, 255]. Overrides replace tags when their
 * `_override_on` flag is non-zero. */
void img_ce_apply_coating(ImgCEGrid* ce, uint32_t y, uint32_t x,
                          const ImgDeltaCoating* coating);

/* Apply the same coating over a rectangular region (inclusive of
 * y0..y1-1 and x0..x1-1). Useful for painting a whole semantic
 * block (e.g. "this region is person / foreground / preserved"). */
void img_ce_apply_coating_region(ImgCEGrid* ce,
                                 uint32_t y0, uint32_t x0,
                                 uint32_t y1, uint32_t x1,
                                 const ImgDeltaCoating* coating);

/* ── Resolve = Sieve + Repair ────────────────────────────────
 * For every cell, compare its `core` against the 4-neighbour mean.
 * If |diff| > threshold the cell is flagged as an outlier. An
 * outlier is *explained* (and absorbed toward the neighborhood
 * mean) when at least one neighbour shares its semantic role or
 * direction class. Unexplained outliers are *promoted*: their
 * delta channel is raised, priority is nudged up, and delta_sign
 * is marked POSITIVE — making them candidates for a new keyframe
 * or repair pass downstream.
 *
 * The two masks are optional (pass NULL to skip). When provided
 * they must each point to IMG_CE_TOTAL bytes and receive 0/1 flags. */
typedef struct {
    uint32_t outlier_count;
    uint32_t explained_count;
    uint32_t repaired_count;
    uint32_t promoted_count;
} ImgResolveResult;

void img_ce_resolve(ImgCEGrid* ce, int threshold,
                    uint8_t* outlier_mask_or_null,
                    uint8_t* explained_mask_or_null,
                    ImgResolveResult* out_result);

#endif /* IMG_CE_H */
