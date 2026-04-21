#include "spatial_bimodal.h"

#include <string.h>

/* Deep-copy helper: allocate a fresh ImgCEGrid and mirror `src`. */
static ImgCEGrid* ce_grid_clone(const ImgCEGrid* src) {
    if (!src || !src->cells) return NULL;
    ImgCEGrid* dst = img_ce_grid_create();
    if (!dst) return NULL;
    memcpy(dst->cells, src->cells, IMG_CE_TOTAL * sizeof(ImgCECell));
    return dst;
}

int ai_bind_ce_snapshot(SpatialAI* ai,
                        uint32_t kf_id,
                        const ImgCEGrid* source_ce) {
    if (!ai) return 0;
    if (kf_id >= ai->kf_count) return 0;

    Keyframe* kf = &ai->keyframes[kf_id];

    /* Free any prior binding. */
    if (kf->ce_snapshot) {
        img_ce_grid_destroy(kf->ce_snapshot);
        kf->ce_snapshot = NULL;
    }

    if (!source_ce) return 1;   /* explicit clear is success */

    kf->ce_snapshot = ce_grid_clone(source_ce);
    return kf->ce_snapshot != NULL;
}

int ai_bind_image_to_kf(SpatialAI* ai,
                        uint32_t kf_id,
                        const uint8_t* image_rgb,
                        uint32_t image_w, uint32_t image_h,
                        ImgDeltaMemory* memory_or_null) {
    if (!ai || !image_rgb) return 0;
    if (kf_id >= ai->kf_count) return 0;
    if (image_w == 0 || image_h == 0) return 0;

    ImgPipelineResult r = {0};
    if (!img_pipeline_run(image_rgb, image_w, image_h,
                          memory_or_null, /*opt=*/NULL, &r)) {
        return 0;
    }

    /* Steal ownership of r.ce_grid by clearing the pointer in the
     * result before destroy. ce_grid is bound directly — no extra
     * clone needed since the pipeline just allocated it. */
    Keyframe* kf = &ai->keyframes[kf_id];
    if (kf->ce_snapshot) {
        img_ce_grid_destroy(kf->ce_snapshot);
    }
    kf->ce_snapshot = r.ce_grid;
    r.ce_grid = NULL;

    img_pipeline_result_destroy(&r);
    return 1;
}

const ImgCEGrid* ai_get_ce_snapshot(const SpatialAI* ai, uint32_t kf_id) {
    if (!ai) return NULL;
    if (kf_id >= ai->kf_count) return NULL;
    return ai->keyframes[kf_id].ce_snapshot;
}

void ai_release_ce_snapshot(SpatialAI* ai, uint32_t kf_id) {
    if (!ai) return;
    if (kf_id >= ai->kf_count) return;
    Keyframe* kf = &ai->keyframes[kf_id];
    if (kf->ce_snapshot) {
        img_ce_grid_destroy(kf->ce_snapshot);
        kf->ce_snapshot = NULL;
    }
}

uint32_t ai_ce_snapshot_count(const SpatialAI* ai) {
    if (!ai) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < ai->kf_count; i++) {
        if (ai->keyframes[i].ce_snapshot) n++;
    }
    return n;
}
