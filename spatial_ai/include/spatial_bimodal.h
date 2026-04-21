#ifndef SPATIAL_BIMODAL_H
#define SPATIAL_BIMODAL_H

#include "spatial_keyframe.h"
#include "img_ce.h"
#include "img_pipeline.h"

/*
 * spatial_bimodal — text ↔ image pairing at the Keyframe level.
 *
 *   Each Keyframe already carries a text grid (256×256 RGBA) that
 *   the existing engine matches against. This module attaches an
 *   optional image-side CE snapshot (ImgCEGrid, 64×64 interpreted
 *   cells) to the same Keyframe — so a single Keyframe id can be
 *   looked up in either direction:
 *
 *     text input  → text-side match → paired CE snapshot → render
 *     image input → CE compression  → paired text grid   → decode
 *
 *   Two engines live side by side (text CE and image CE); the
 *   pairing is a thin binding layer at the Keyframe boundary — no
 *   mixed StateKey space, no score normalization needed at this
 *   layer (each engine returns its own 0..1 match score; callers
 *   can combine them as they see fit).
 */

/* Deep-copy `source_ce` (if any) and attach it to the keyframe at
 * `kf_id`. Any previous binding is freed. Passing a NULL source
 * clears the binding. Returns 1 on success, 0 on failure
 * (out-of-range id, allocation failure). */
int ai_bind_ce_snapshot(SpatialAI* ai,
                        uint32_t kf_id,
                        const ImgCEGrid* source_ce);

/* Run the image pipeline on `(image_rgb, image_w, image_h)` with
 * default options and bind the resulting CE grid to `kf_id`.
 * `memory_or_null` is forwarded to img_pipeline_run. Returns 1 on
 * success. */
int ai_bind_image_to_kf(SpatialAI* ai,
                        uint32_t kf_id,
                        const uint8_t* image_rgb,
                        uint32_t image_w, uint32_t image_h,
                        ImgDeltaMemory* memory_or_null);

/* Return the CE snapshot bound to `kf_id`, or NULL if none / bad id. */
const ImgCEGrid* ai_get_ce_snapshot(const SpatialAI* ai, uint32_t kf_id);

/* Free and clear the binding on `kf_id`. Safe on unbound ids. */
void ai_release_ce_snapshot(SpatialAI* ai, uint32_t kf_id);

/* Number of keyframes that currently have a CE snapshot bound. */
uint32_t ai_ce_snapshot_count(const SpatialAI* ai);

#endif /* SPATIAL_BIMODAL_H */
