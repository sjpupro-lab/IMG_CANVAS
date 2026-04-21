#include "spatial_bimodal.h"
#include "spatial_io.h"
#include "img_ce.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_total  = 0;

#define TEST(name) do {                 \
    tests_total++;                      \
    printf("  [TEST] %s ... ", name);   \
} while (0)

#define PASS() do {                     \
    tests_passed++;                     \
    printf("PASS\n");                   \
} while (0)

/* ── helpers ─────────────────────────────────────────────── */

/* Mini 128×96 RGB banded fixture — small enough for fast tests,
 * big enough for the pipeline to populate a non-trivial CE grid. */
static uint8_t* make_banded_image(uint32_t w, uint32_t h) {
    uint8_t* img = (uint8_t*)malloc((size_t)w * h * 3);
    assert(img);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t r = (y < h / 3)     ?  80
                   : (y < 2 * h / 3) ?  30
                   :                   160;
        uint8_t g = (y < h / 3)     ? 120
                   : (y < 2 * h / 3) ?  30
                   :                   110;
        uint8_t b = (y < h / 3)     ? 200
                   : (y < 2 * h / 3) ?  30
                   :                    70;
        for (uint32_t x = 0; x < w; x++) {
            size_t p = ((size_t)y * w + x) * 3;
            img[p] = r; img[p+1] = g; img[p+2] = b;
        }
    }
    return img;
}

static int ce_grids_equal_ignoring_last_id(const ImgCEGrid* a,
                                           const ImgCEGrid* b) {
    if (a->width != b->width || a->height != b->height) return 0;
    const uint32_t n = a->width * a->height;
    for (uint32_t i = 0; i < n; i++) {
        const ImgCECell* x = &a->cells[i];
        const ImgCECell* y = &b->cells[i];
        if (x->core          != y->core          ||
            x->link          != y->link          ||
            x->delta         != y->delta         ||
            x->priority      != y->priority      ||
            x->tone_class    != y->tone_class    ||
            x->semantic_role != y->semantic_role ||
            x->direction_class != y->direction_class ||
            x->depth_class   != y->depth_class   ||
            x->delta_sign    != y->delta_sign) {
            return 0;
        }
    }
    return 1;
}

/* ── unbound keyframe returns NULL ──────────────────────── */

static void test_unbound_returns_null(void) {
    TEST("ai_get_ce_snapshot on a fresh keyframe returns NULL");

    SpatialAI* ai = spatial_ai_create();
    uint32_t id = ai_force_keyframe(ai, "hello", "kf0");
    assert(id == 0);

    assert(ai_get_ce_snapshot(ai, id) == NULL);
    assert(ai_ce_snapshot_count(ai)  == 0);
    /* Out-of-range id is also NULL. */
    assert(ai_get_ce_snapshot(ai, 99) == NULL);

    spatial_ai_destroy(ai);
    PASS();
}

/* ── bind_image roundtrip: pipeline runs, snapshot attached ── */

static void test_bind_image_attaches_ce_snapshot(void) {
    TEST("ai_bind_image_to_kf runs pipeline and attaches CE snapshot");

    SpatialAI* ai = spatial_ai_create();
    uint32_t id = ai_force_keyframe(ai, "banded image", "kf-img");
    assert(id == 0);

    const uint32_t w = 128, h = 96;
    uint8_t* img = make_banded_image(w, h);

    assert(ai_bind_image_to_kf(ai, id, img, w, h, /*memory=*/NULL));
    assert(ai_ce_snapshot_count(ai) == 1);

    const ImgCEGrid* ce = ai_get_ce_snapshot(ai, id);
    assert(ce && ce->cells);
    assert(ce->width  == IMG_CE_SIZE);
    assert(ce->height == IMG_CE_SIZE);

    /* Banded fixture → depth classifier split by row, priority tracks
     * depth. Confirm CE actually compressed something non-trivial. */
    int has_nonzero_core = 0;
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        if (ce->cells[i].core > 0) { has_nonzero_core = 1; break; }
    }
    assert(has_nonzero_core);

    /* Replacing the binding frees the previous grid. */
    assert(ai_bind_image_to_kf(ai, id, img, w, h, NULL));
    assert(ai_ce_snapshot_count(ai) == 1);

    free(img);
    spatial_ai_destroy(ai);
    PASS();
}

/* ── release clears the binding ─────────────────────────── */

static void test_release_clears_binding(void) {
    TEST("ai_release_ce_snapshot drops the grid and count goes back to 0");

    SpatialAI* ai = spatial_ai_create();
    uint32_t id = ai_force_keyframe(ai, "any", "k");

    uint8_t* img = make_banded_image(128, 96);
    ai_bind_image_to_kf(ai, id, img, 128, 96, NULL);
    assert(ai_ce_snapshot_count(ai) == 1);

    ai_release_ce_snapshot(ai, id);
    assert(ai_get_ce_snapshot(ai, id) == NULL);
    assert(ai_ce_snapshot_count(ai) == 0);

    /* Double-release is a no-op. */
    ai_release_ce_snapshot(ai, id);
    /* Out-of-range id is safe. */
    ai_release_ce_snapshot(ai, 99);

    free(img);
    spatial_ai_destroy(ai);
    PASS();
}

/* ── save → load roundtrip preserves CE snapshots ─────────── */

static void test_save_load_roundtrip(void) {
    TEST("save/load preserves bound CE snapshots byte-for-byte");

    SpatialAI* ai = spatial_ai_create();
    uint32_t k0 = ai_force_keyframe(ai, "first clause",  "kf0");
    uint32_t k1 = ai_force_keyframe(ai, "second clause", "kf1");
    uint32_t k2 = ai_force_keyframe(ai, "third clause",  "kf2");
    assert(k0 == 0 && k1 == 1 && k2 == 2);

    /* Bind images only to k0 and k2, leave k1 unbound. */
    uint8_t* img = make_banded_image(128, 96);
    assert(ai_bind_image_to_kf(ai, k0, img, 128, 96, NULL));
    assert(ai_bind_image_to_kf(ai, k2, img, 128, 96, NULL));
    assert(ai_ce_snapshot_count(ai) == 2);

    /* Save. */
    const char* path = "build/test_bimodal_model.spai";
    SpaiStatus s = ai_save(ai, path);
    assert(s == SPAI_OK);

    /* Independently clone the pre-save CE snapshots for later compare. */
    const ImgCEGrid* src_k0 = ai_get_ce_snapshot(ai, k0);
    const ImgCEGrid* src_k2 = ai_get_ce_snapshot(ai, k2);
    ImgCEGrid* expect_k0 = img_ce_grid_create();
    ImgCEGrid* expect_k2 = img_ce_grid_create();
    memcpy(expect_k0->cells, src_k0->cells,
           IMG_CE_TOTAL * sizeof(ImgCECell));
    memcpy(expect_k2->cells, src_k2->cells,
           IMG_CE_TOTAL * sizeof(ImgCECell));

    /* Load and compare. */
    SpaiStatus ls = SPAI_OK;
    SpatialAI* loaded = ai_load(path, &ls);
    assert(ls == SPAI_OK);
    assert(loaded);
    assert(loaded->kf_count == 3);

    assert(ai_ce_snapshot_count(loaded) == 2);
    const ImgCEGrid* got_k0 = ai_get_ce_snapshot(loaded, k0);
    const ImgCEGrid* got_k1 = ai_get_ce_snapshot(loaded, k1);
    const ImgCEGrid* got_k2 = ai_get_ce_snapshot(loaded, k2);
    assert(got_k0 && got_k2);
    assert(got_k1 == NULL);

    assert(ce_grids_equal_ignoring_last_id(got_k0, expect_k0));
    assert(ce_grids_equal_ignoring_last_id(got_k2, expect_k2));

    /* last_delta_id should come back as IMG_DELTA_ID_NONE on load
     * (it's a runtime-only resume pointer). */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        assert(got_k0->cells[i].last_delta_id == IMG_DELTA_ID_NONE);
        assert(got_k2->cells[i].last_delta_id == IMG_DELTA_ID_NONE);
    }

    img_ce_grid_destroy(expect_k0);
    img_ce_grid_destroy(expect_k2);
    spatial_ai_destroy(loaded);
    spatial_ai_destroy(ai);
    free(img);
    PASS();
}

int main(void) {
    printf("=== test_bimodal ===\n");

    test_unbound_returns_null();
    test_bind_image_attaches_ce_snapshot();
    test_release_clears_binding();
    test_save_load_roundtrip();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
