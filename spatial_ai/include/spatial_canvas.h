#ifndef SPATIAL_CANVAS_H
#define SPATIAL_CANVAS_H

#include "spatial_grid.h"
#include "spatial_clock.h"

/*
 * Multi-tile canvas per SPEC.md §6 "Scaling structure".
 *
 *   One canvas holds up to 32 clause tiles in an 8×4 grid:
 *
 *     +-------+-------+-------+-------+-------+-------+-------+-------+
 *     |slot 0 |slot 1 |slot 2 |slot 3 |slot 4 |slot 5 |slot 6 |slot 7 |   y 0..255
 *     +-------+-------+-------+-------+-------+-------+-------+-------+
 *     |slot 8 |slot 9 |slot10 |slot11 |slot12 |slot13 |slot14 |slot15 |   y 256..511
 *     +-------+-------+-------+-------+-------+-------+-------+-------+
 *     |slot16 |slot17 |slot18 |slot19 |slot20 |slot21 |slot22 |slot23 |   y 512..767
 *     +-------+-------+-------+-------+-------+-------+-------+-------+
 *     |slot24 |slot25 |slot26 |slot27 |slot28 |slot29 |slot30 |slot31 |   y 768..1023
 *     +-------+-------+-------+-------+-------+-------+-------+-------+
 *       x0-255  256-   512-   768-   1024-  1280-  1536-  1792-2047
 *
 *   Placement order: left → right, top → bottom.
 *
 *   Why one 2048×1024 canvas instead of 32 independent 256×256 frames?
 *
 *   (1)  update_rgb diffusion crosses clause boundaries. Slot k's right
 *        edge sits next to slot k+1's left edge; B-channel horizontal
 *        diffusion naturally flows between adjacent clauses.
 *
 *   (2)  Delta RLE works. Within a single tile, changes between
 *        similar clauses are scattered; across a full canvas tiled
 *        with related clauses, identical Y-rows contain contiguous
 *        regions where RLE compresses well (SPEC §D).
 *
 *   (3)  Retrieval becomes spatial. A query tile can be compared
 *        against every slot, and R/G/B from neighbor slots inform
 *        the match (context from surrounding clauses).
 */

#define CV_TILE       256
#define CV_COLS       8
#define CV_ROWS       4
#define CV_SLOTS      (CV_COLS * CV_ROWS)       /* 32 */
#define CV_WIDTH      (CV_TILE * CV_COLS)       /* 2048 */
#define CV_HEIGHT     (CV_TILE * CV_ROWS)       /* 1024 */
#define CV_TOTAL      (CV_WIDTH * CV_HEIGHT)    /* 2,097,152 cells */

/* Canvas-level block summary (for H.264-style scene change detection):
 *   16×16 pixels per block → 128 × 64 = 8192 blocks per canvas. */
#define CV_BLOCK        16
#define CV_BLOCKS_X     (CV_WIDTH  / CV_BLOCK)  /* 128 */
#define CV_BLOCKS_Y     (CV_HEIGHT / CV_BLOCK)  /*  64 */
#define CV_BLOCKS_TOTAL (CV_BLOCKS_X * CV_BLOCKS_Y)  /* 8192 */

typedef struct {
    uint32_t sums[CV_BLOCKS_TOTAL];
} CanvasBlockSummary;

/* I-frame / P-frame classification for a full canvas, decided by
 * scene-change detection when the canvas fills up. */
typedef enum {
    CANVAS_IFRAME = 0,   /* independent keyframe canvas */
    CANVAS_PFRAME = 1    /* delta relative to parent keyframe canvas */
} CanvasFrameType;

/* ── DataType — automatic classification of clauses ──────
 * Decides how strongly RGB diffusion flows across the slot
 * boundary into neighbouring tiles. Prose chapters lean on
 * each other; code blocks are independent; short dictionary
 * entries are fully isolated. */
typedef enum {
    DATA_PROSE  = 0,   /* len > 150 ASCII text, narrative flow */
    DATA_DIALOG = 1,   /* len 30..150, conversational turns */
    DATA_CODE   = 2,   /* special-char ratio > 15%, programming */
    DATA_SHORT  = 3,   /* len < 30, single words / dictionary */
    DATA_TYPE_COUNT = 4
} DataType;

/* Inspect a byte range and classify it into a DataType.
 * Rule order: CODE (by special chars) > SHORT (by length) >
 * PROSE (by length) > DIALOG (default). */
DataType    detect_data_type(const uint8_t* bytes, uint32_t len);
const char* data_type_name(DataType t);

/* Per-type boundary diffusion weight used by canvas_update_rgb at
 * slot boundaries. Values from user spec:
 *   PROSE  0.5  DIALOG 0.3  CODE 0.1  SHORT 0.02 */
float       data_type_boundary_weight(DataType t);

/* Per-slot metadata. Populated by canvas_add_clause. */
typedef struct {
    DataType type;
    float    boundary_weight;
    uint32_t byte_length;     /* original clause byte count */
    uint32_t topic_hash;      /* djb2 over the clause text */
    int      occupied;        /* 1 after first placement, 0 otherwise */

    /* Chapter-group identifier within a canvas. 0 = unassigned.
     * Slots sharing a freq_tag are treated as one logical chapter:
     * boundary_multiplier dampens RGB diffusion across freq_tag
     * boundaries by an extra 0.1× factor. Assigned by
     * canvas_assign_freq_tags after the canvas is full. */
    uint16_t freq_tag;
} SlotMeta;

/* ── Compute-canvas summary (per slot) ────────────────────
 *
 * Compact 3-byte signature pulled out of each filled slot. Holds two
 * orthogonal signals:
 *
 *   b_mean   — average B value over active cells. The B channel
 *              already carries POS-derived context strength after
 *              canvas_update_rgb runs, so the slot mean tells us
 *              "how strong is the contextual flow here".
 *
 *   hz_hist  — 4-bin column histogram of A activity, 4 bits per bin.
 *              Bins divide the 256-column slot into quarters; bins[k]
 *              counts active cells in column range [k·64, (k+1)·64).
 *              Each bin is normalized to 0..15 then packed:
 *                  hist = bin0 | (bin1 << 4) | (bin2 << 8) | (bin3 << 12)
 *              Reads as a "byte-value spectrum" — different vocabularies
 *              and writing styles produce different bin proportions.
 *              Two slots from the same article tend to land on the
 *              same hist; topic shifts move it.
 *
 * Comparison via SAD on this summary distinguishes context flow
 * (b_mean diff) from chapter shifts (hz_hist diff) cleanly — what
 * cosine on the raw 256×256 region collapses into a single fuzzy
 * scalar. Cost: 3 B per slot, 96 B per canvas. */
typedef struct {
    uint8_t  b_mean;
    uint16_t hz_hist;
} SlotComputeSummary;

typedef struct {
    uint16_t* A;            /* 32-byte aligned, 2 bytes per cell */
    uint8_t*  R;
    uint8_t*  G;
    uint8_t*  B;
    uint32_t  width;        /* CV_WIDTH */
    uint32_t  height;       /* CV_HEIGHT */
    uint32_t  slot_count;   /* clauses placed so far (0 … CV_SLOTS) */
    DataType  canvas_type;  /* set by the first placed clause; canvases
                               in a pool are typically type-homogeneous */
    SlotMeta  meta[CV_SLOTS];

    /* I/P classification — filled by the pool's scene-change detector
     * when the canvas becomes full. Unclassified canvases are treated
     * as IFRAMEs with parent_canvas_id = UINT32_MAX. */
    CanvasFrameType frame_type;
    uint32_t        parent_canvas_id;   /* UINT32_MAX if IFRAME */
    float           changed_ratio;      /* block fraction that changed */
    int             classified;         /* 1 after scene_change_classify ran */

    /* Compute-canvas summaries — see SlotComputeSummary above. Filled
     * by canvas_compute_all_summaries; consumed by
     * canvas_summary_sad / canvas_assign_freq_tags. Not persisted to
     * .spai (regenerable from A/B channels on load). */
    SlotComputeSummary compute[CV_SLOTS];

    /* RGBA clockwork engine — the primary signal path for chapter
     * detection. One engine per canvas, ticked once per slot
     * transition during assign_freq_tags. Not persisted (regenerable
     * during load by re-running assign_freq_tags). 256 KB. */
    RGBAClockEngine clock;
} SpatialCanvas;

/* Lifecycle */
SpatialCanvas* canvas_create(void);
void           canvas_destroy(SpatialCanvas* c);
void           canvas_clear(SpatialCanvas* c);

/* Placement:
 *   Encodes `text` into a 256×256 tile via layers_encode_clause and
 *   copies it into slot `c->slot_count`. Returns the slot index that
 *   was used, or -1 if the canvas is full.
 *   Note: caller typically runs canvas_update_rgb AFTER all slots are
 *   filled so cross-boundary diffusion has data to work with. */
int            canvas_add_clause(SpatialCanvas* c, const char* text);

/* Canvas-wide directional diffusion (SPEC §4) over width × height.
 *   R diagonal, G vertical, B horizontal — same α/β/γ as grid version. */
void           canvas_update_rgb(SpatialCanvas* c);

/* ── freq_tag (chapter grouping within a canvas) ──────────
 *
 * Assigns SlotMeta.freq_tag for every populated slot. Walks slots in
 * placement order; starts a new chapter (current_tag++) whenever:
 *   (a) horizontally-adjacent slots' compute summaries differ by
 *       canvas_summary_sad ≥ sad_threshold (B-mean + hz_hist L1),
 *       OR
 *   (b) use_topic_hash is non-zero and topic_hash[i-1] != topic_hash[i].
 * Vertical neighbours (across rows) are not physically adjacent in
 * the 8×4 placement and are skipped from the SAD check.
 *
 * Caller must have canvas_compute_all_summaries(c) called first
 * (the streaming path does this automatically when the canvas
 * fills — see pool_add_clause).
 *
 * The first slot always gets tag = 1. Single-slot canvases keep
 * tag = 1. Once assigned, boundary_multiplier knocks 0.1× off the
 * diffusion weight whenever neighbouring slots disagree on freq_tag,
 * isolating the chapter physically. Cost: O(slot_count) on the SAD
 * check itself; the heavy lifting was the per-slot summary pass. */
void           canvas_assign_freq_tags(SpatialCanvas* c,
                                       uint16_t sad_threshold,
                                       int      use_topic_hash);

/* Clockwork-engine variant. Feeds each slot's (b_diff, hz_diff,
 * hz_hist_sum, active_cells × 256) into canvas.clock, snapshots
 * the engine at chapter start, and compares current vs snapshot
 * SAD per channel:
 *   if  G_sad > g_threshold                       → new chapter
 *   elif R_sad + B_sad > rb_threshold             → new chapter
 * Row crossings in the 8×4 layout are skipped (same as the
 * summary-SAD path). */
void           canvas_assign_freq_tags_clock(SpatialCanvas* c,
                                             uint64_t g_threshold,
                                             uint64_t rb_threshold,
                                             int      use_topic_hash);

/* Mean B-channel value at the shared edge between two horizontally
 * adjacent slots (left/right neighbours in the 8×4 layout),
 * normalized to [0,1]. Returns -1.0f if the slots are not
 * horizontally adjacent (e.g. across rows) or arguments are invalid.
 * Returns 0.0 when no active cells line the edge. */
float          canvas_b_edge_value(const SpatialCanvas* c,
                                   uint32_t slot_a, uint32_t slot_b);

/* Q16 form of canvas_b_edge_value (slot A-cosine).
 * Returns 0..65535. Returns 0 (== "fully break") when slots are not
 * horizontally adjacent so callers don't need a sentinel value. */
uint16_t       canvas_b_edge_q16(const SpatialCanvas* c,
                                 uint32_t slot_a, uint32_t slot_b);

/* Sum of absolute A-channel differences between two slot regions.
 * Pure subtraction + accumulation — no multiply, no sqrt, no float.
 * Mirrors the metric scene_change_classify uses on canvas-level
 * block sums (just one tier finer). Range: 0 (identical slots) to
 * ~65 535 × 65 536 worst case (uint32 holds either way; uint16 A
 * values keep the upper bound far below uint32_max in practice).
 *
 * Returns UINT32_MAX for non-adjacent slot pairs (different rows in
 * the 8×4 placement). Callers compare `sad < threshold` for the
 * "still in same chapter" test; the UINT32_MAX sentinel naturally
 * fails the comparison if the caller forgets the adjacency guard. */
uint32_t       canvas_b_edge_sad(const SpatialCanvas* c,
                                 uint32_t slot_a, uint32_t slot_b);

/* ── Compute-canvas operations ──────────────────────────── */

/* Populate c->compute[slot] from the slot's pixel region. Cheap:
 * one pass over CV_TILE² cells per slot. Idempotent — call again
 * after canvas_update_rgb if you want B diffusion folded in. */
void           canvas_compute_slot_summary(SpatialCanvas* c, uint32_t slot);

/* Walk every populated slot and refresh its summary. Called once
 * after the canvas is full and RGB diffusion has been applied. */
void           canvas_compute_all_summaries(SpatialCanvas* c);

/* SAD between two slots' compute summaries. Returns:
 *   |b_mean_a - b_mean_b| + sum_k |hist_nibble_a[k] - hist_nibble_b[k]|
 * Range 0..(255 + 60) = 0..315. Returns UINT16_MAX on out-of-range
 * slot indices so callers' "<" guards naturally fail. */
uint16_t       canvas_summary_sad(const SpatialCanvas* c,
                                  uint32_t slot_a, uint32_t slot_b);

/* Look up the freq_tag of the slot containing pixel (x, y). Returns 0
 * if the coordinates are out of range or the slot is unassigned. */
uint16_t       canvas_get_freq_tag(const SpatialCanvas* c,
                                   uint32_t x, uint32_t y);

/* Stats */
uint32_t       canvas_active_count(const SpatialCanvas* c);
uint32_t       canvas_slot_byte_offset(uint32_t slot, uint32_t* out_x0,
                                       uint32_t* out_y0);

/* Compute per-16×16-block A-channel sums over the entire canvas.
 * Used for H.264-style scene-change detection. */
void           canvas_compute_block_sums(const SpatialCanvas* c,
                                          CanvasBlockSummary* out);

/* Matching:
 *   Compare a 256×256 query grid against the tile occupying `slot`.
 *   Uses RGB-weighted cosine over the tile's 256×256 sub-region. */
float          canvas_match_slot(const SpatialCanvas* c,
                                 const SpatialGrid* query, uint32_t slot);

/* Best slot over all populated slots. Returns slot index and writes
 *   similarity. */
uint32_t       canvas_best_slot(const SpatialCanvas* c,
                                const SpatialGrid* query,
                                float* out_sim);

/* Export a tile back into a 256×256 grid (allocates channels in out). */
void           canvas_slot_to_grid(const SpatialCanvas* c, uint32_t slot,
                                   SpatialGrid* out);

/* Compute sparse A-delta between two canvases.
 *   Returns number of changed cells; writes index + diff into entries.
 *   Useful for comparing canvas_a → canvas_b (RLE friendliness is
 *   measurable via canvas_delta_rle_bytes). */
typedef struct {
    uint32_t index;    /* y * CV_WIDTH + x */
    int16_t  diff_A;
} CanvasDeltaEntry;

uint32_t       canvas_delta_sparse(const SpatialCanvas* a,
                                   const SpatialCanvas* b,
                                   CanvasDeltaEntry* out, uint32_t max_out);

/* Estimate RLE byte cost of the sparse delta: consecutive-index runs
 *   compressed into (start, length, diff) triplets of 8 bytes each
 *   vs. 6 bytes per sparse entry. Returns RLE byte count. */
uint32_t       canvas_delta_rle_bytes(const CanvasDeltaEntry* entries,
                                      uint32_t count);

/* ── Full-channel delta (A + R + G + B) for P-frame canvas storage ──
 *
 * Each changed cell stores a 4-channel diff. Used by ai_save to write
 * P-frames as sparse deltas against their parent canvas, and by
 * ai_load to reconstruct them. 11 bytes per entry (uint32 index +
 * int16 diff_A + int8 diff_R + int8 diff_G + int8 diff_B).
 *
 * Typical P-frame has ~30-60% cells differing → 0.6-1.3M entries →
 * 6-15 MB vs the 10.5 MB full pixel payload. Storage win depends on
 * how similar the P-frame is to its parent. */
typedef struct {
    uint32_t index;    /* y * CV_WIDTH + x */
    int16_t  diff_A;
    int8_t   diff_R;
    int8_t   diff_G;
    int8_t   diff_B;
} CanvasFullDelta;

/* Compute sparse full-channel delta from parent→child canvas.
 * out must fit at least CV_TOTAL entries in the worst case.
 * Returns the number of differing cells written. */
uint32_t       canvas_full_delta(const SpatialCanvas* parent,
                                 const SpatialCanvas* child,
                                 CanvasFullDelta* out);

/* Apply a full-channel delta to parent_pixels → reconstruct child.
 * Caller owns target; channels must be pre-allocated CV_TOTAL size. */
void           canvas_apply_full_delta(const SpatialCanvas* parent,
                                       const CanvasFullDelta* entries,
                                       uint32_t count,
                                       SpatialCanvas* target);

#endif /* SPATIAL_CANVAS_H */
