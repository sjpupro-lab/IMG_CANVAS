#ifndef IMG_CE_DIFF_H
#define IMG_CE_DIFF_H

#include "img_ce.h"

/*
 * img_ce_diff — I-frame / P-frame encoding for a CE grid.
 *
 *   An ImgCEGrid is the keyframe (I-frame) — the full 64×64 state.
 *   An ImgCEDiff is the P-frame — a sparse list of per-cell changes
 *   relative to a keyframe.
 *
 *     base (ImgCEGrid)   ──►  diff (ImgCEDiff)   ──►  target (ImgCEGrid)
 *                compute                      apply
 *
 *   Numeric channels (core / link / delta / priority) are stored as
 *   signed deltas; classification tags (tone / role / direction /
 *   depth / delta_sign) are stored as the target's replacement value
 *   (tags are discrete; signed diffs would be meaningless).
 *
 *   last_delta_id is not part of the diff — resume pointers are
 *   produced at apply time by the pipeline, not by encoding.
 *
 *   Rationale: the spatial_ai text engine already uses a keyframe /
 *   delta codec (SPEC.md §D, README_KO §4). Applying the same pattern
 *   to the CE grid lets callers persist / transmit / stack CE states
 *   compactly — base keyframe plus deltas — instead of 4096-cell
 *   snapshots.
 */

#pragma pack(push, 1)
typedef struct {
    uint16_t idx;               /* 0..IMG_CE_TOTAL-1 */

    int16_t  d_core;
    int16_t  d_link;
    int16_t  d_delta;
    int16_t  d_priority;

    /* Tag replacements; cheaper than per-tag diffs since tags are
     * enums with ≤ 8 values. Applied only when (tag_mask & bit) != 0. */
    uint8_t  new_tone_class;
    uint8_t  new_semantic_role;
    uint8_t  new_direction_class;
    uint8_t  new_depth_class;
    uint8_t  new_delta_sign;

    /* Bitmask of which tag fields differ from the base:
     *   bit 0 tone_class
     *   bit 1 semantic_role
     *   bit 2 direction_class
     *   bit 3 depth_class
     *   bit 4 delta_sign
     * Channel deltas are applied whenever their d_* field is non-zero,
     * no mask needed. */
    uint8_t  tag_mask;
} ImgCEDiffEntry;
#pragma pack(pop)

#define IMG_CE_DIFF_TAG_TONE       (1u << 0)
#define IMG_CE_DIFF_TAG_ROLE       (1u << 1)
#define IMG_CE_DIFF_TAG_DIRECTION  (1u << 2)
#define IMG_CE_DIFF_TAG_DEPTH      (1u << 3)
#define IMG_CE_DIFF_TAG_DELTA_SIGN (1u << 4)

typedef struct {
    ImgCEDiffEntry* entries;    /* malloc-owned; free with img_ce_diff_destroy */
    uint32_t        count;
    uint32_t        capacity;
} ImgCEDiff;

/* Lifecycle. A zero-initialised ImgCEDiff (all fields 0/NULL) is a
 * valid empty diff and safe to pass to destroy. */
void img_ce_diff_destroy(ImgCEDiff* diff);

/* Compute the sparse diff from base → target. Only cells where at
 * least one channel or tag differs produce an entry. Returns the
 * entry count (also stored in out->count); returns 0 if inputs are
 * NULL or dimensions disagree. Any prior contents of *out are freed. */
uint32_t img_ce_diff_compute(const ImgCEGrid* base,
                             const ImgCEGrid* target,
                             ImgCEDiff* out);

/* Apply diff to base, writing into out. `base` and `out` may alias
 * (self-apply). last_delta_id is copied from base — diffs do not
 * track the resume pointer. Returns 1 on success, 0 on argument /
 * dimension mismatch. */
int      img_ce_diff_apply(const ImgCEGrid* base,
                           const ImgCEDiff* diff,
                           ImgCEGrid* out);

/* Estimated byte size for the sparse diff on the wire:
 *   sizeof header (count) + count × sizeof(ImgCEDiffEntry). */
uint32_t img_ce_diff_byte_size(const ImgCEDiff* diff);

#endif /* IMG_CE_DIFF_H */
