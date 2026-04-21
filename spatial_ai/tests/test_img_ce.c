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

/* Build a 512x512 RGB test image with three regions:
 *   upper third       = cool sky       (80, 120, 200)
 *   middle third      = dark object    (30, 30, 30)
 *   lower third       = warm ground    (160, 110, 70)
 */
static uint8_t* make_test_image(uint32_t w, uint32_t h) {
    uint8_t* img = (uint8_t*)malloc((size_t)w * h * 3);
    assert(img);

    for (uint32_t y = 0; y < h; y++) {
        uint8_t r, g, b;
        if (y < h / 3)            { r = 80;  g = 120; b = 200; }
        else if (y < 2 * h / 3)   { r = 30;  g = 30;  b = 30;  }
        else                       { r = 160; g = 110; b = 70;  }
        for (uint32_t x = 0; x < w; x++) {
            size_t p = ((size_t)y * w + x) * 3;
            img[p + 0] = r;
            img[p + 1] = g;
            img[p + 2] = b;
        }
    }
    return img;
}

/* ── lifecycle ───────────────────────────────────────────── */

static void test_lifecycle(void) {
    TEST("small canvas + ce grid create/destroy");
    ImgSmallCanvas* sc = img_small_canvas_create();
    assert(sc && sc->cells);
    assert(sc->width == IMG_SC_SIZE && sc->height == IMG_SC_SIZE);

    ImgCEGrid* ce = img_ce_grid_create();
    assert(ce && ce->cells);
    assert(ce->width == IMG_CE_SIZE && ce->height == IMG_CE_SIZE);

    /* zero-initialised */
    for (uint32_t i = 0; i < IMG_SC_TOTAL; i++) {
        assert(sc->cells[i].intensity == 0);
    }
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        assert(ce->cells[i].core == 0);
    }

    img_small_canvas_destroy(sc);
    img_ce_grid_destroy(ce);
    PASS();
}

/* ── image → SmallCanvas ─────────────────────────────────── */

static void test_image_to_small_canvas(void) {
    TEST("image → SmallCanvas captures tone and mood");

    uint32_t w = 512, h = 512;
    uint8_t* img = make_test_image(w, h);
    ImgSmallCanvas* sc = img_small_canvas_create();

    img_image_to_small_canvas(img, w, h, sc);

    /* Sample rows representative of each region. IMG_SC_SIZE = 256. */
    const ImgSmallCell* top    = &sc->cells[img_sc_idx(20,  128)];   /* sky   */
    const ImgSmallCell* middle = &sc->cells[img_sc_idx(128, 128)];   /* dark  */
    const ImgSmallCell* bottom = &sc->cells[img_sc_idx(230, 128)];   /* ground*/

    /* Sky: cool + background band */
    assert(top->mood  == IMG_MOOD_COOL);
    assert(top->depth == IMG_DEPTH_BACKGROUND);

    /* Middle band is dark and near-neutral; mood should be dramatic
     * because the (r+b)/2 = 30 < 80. */
    assert(middle->mood  == IMG_MOOD_DRAMATIC);
    assert(middle->depth == IMG_DEPTH_MIDGROUND);

    /* Ground: warm + foreground band */
    assert(bottom->mood  == IMG_MOOD_WARM);
    assert(bottom->depth == IMG_DEPTH_FOREGROUND);

    free(img);
    img_small_canvas_destroy(sc);
    PASS();
}

/* ── SmallCanvas → CE ────────────────────────────────────── */

static void test_small_canvas_to_ce(void) {
    TEST("SmallCanvas → CE preserves dominant tags");

    uint32_t w = 512, h = 512;
    uint8_t* img = make_test_image(w, h);
    ImgSmallCanvas* sc = img_small_canvas_create();
    ImgCEGrid* ce = img_ce_grid_create();

    img_image_to_small_canvas(img, w, h, sc);
    img_small_canvas_to_ce(sc, ce);

    /* CE is 64x64; 1/3 rows → ~row 10 = background, row 32 = mid, row 54 = fore. */
    const ImgCECell* top    = &ce->cells[img_ce_idx(10, 32)];
    const ImgCECell* middle = &ce->cells[img_ce_idx(32, 32)];
    const ImgCECell* bottom = &ce->cells[img_ce_idx(54, 32)];

    assert(top->depth_class    == IMG_DEPTH_BACKGROUND);
    assert(middle->depth_class == IMG_DEPTH_MIDGROUND);
    assert(bottom->depth_class == IMG_DEPTH_FOREGROUND);

    /* priority formula: 80 + depth * 50 → 80, 130, 180 */
    assert(top->priority    == 80);
    assert(middle->priority == 130);
    assert(bottom->priority == 180);

    /* sky cell should map to ROLE_SKY because it is cool + background */
    assert(top->semantic_role == IMG_ROLE_SKY);

    /* dark middle stays unknown (mood is dramatic, depth mid) */
    assert(middle->semantic_role == IMG_ROLE_UNKNOWN);

    free(img);
    img_small_canvas_destroy(sc);
    img_ce_grid_destroy(ce);
    PASS();
}

/* ── delta coating ───────────────────────────────────────── */

static void test_delta_coating(void) {
    TEST("delta coating overrides tags and saturates channels");

    ImgCEGrid* ce = img_ce_grid_create();
    ImgCECell* c = &ce->cells[img_ce_idx(5, 5)];
    c->core = 200;
    c->priority = 220;
    c->semantic_role = IMG_ROLE_UNKNOWN;
    c->depth_class   = IMG_DEPTH_MIDGROUND;

    ImgDeltaCoating k;
    memset(&k, 0, sizeof(k));
    k.add_core = 100;          /* saturates at 255 */
    k.add_priority = 50;       /* saturates at 255 */
    k.semantic_override = IMG_ROLE_PERSON;
    k.semantic_override_on = 1;
    k.depth_override = IMG_DEPTH_FOREGROUND;
    k.depth_override_on = 1;
    k.delta_sign_override = IMG_DELTA_POSITIVE;
    k.delta_sign_override_on = 1;

    img_ce_apply_coating(ce, 5, 5, &k);

    assert(c->core == 255);
    assert(c->priority == 255);
    assert(c->semantic_role == IMG_ROLE_PERSON);
    assert(c->depth_class   == IMG_DEPTH_FOREGROUND);
    assert(c->delta_sign    == IMG_DELTA_POSITIVE);

    /* non-override fields must not mutate tags */
    ImgCECell* c2 = &ce->cells[img_ce_idx(6, 6)];
    c2->direction_class = IMG_FLOW_VERTICAL;
    ImgDeltaCoating k2;
    memset(&k2, 0, sizeof(k2));
    k2.add_link = 10;
    img_ce_apply_coating(ce, 6, 6, &k2);
    assert(c2->direction_class == IMG_FLOW_VERTICAL);
    assert(c2->link == 10);

    /* region apply */
    ImgDeltaCoating k3;
    memset(&k3, 0, sizeof(k3));
    k3.add_priority = 5;
    img_ce_apply_coating_region(ce, 10, 10, 12, 13, &k3);
    for (uint32_t y = 10; y < 12; y++) {
        for (uint32_t x = 10; x < 13; x++) {
            assert(ce->cells[img_ce_idx(y, x)].priority == 5);
        }
    }
    /* outside region untouched */
    assert(ce->cells[img_ce_idx(9, 9)].priority == 0);

    img_ce_grid_destroy(ce);
    PASS();
}

/* ── resolve ─────────────────────────────────────────────── */

static void test_resolve_sieve_and_repair(void) {
    TEST("resolve absorbs explained outliers, promotes unexplained");

    ImgCEGrid* ce = img_ce_grid_create();

    /* Flat neighbourhood at core=50, semantic=SKY, flow=NONE. */
    for (uint32_t y = 0; y < IMG_CE_SIZE; y++) {
        for (uint32_t x = 0; x < IMG_CE_SIZE; x++) {
            ImgCECell* c = &ce->cells[img_ce_idx(y, x)];
            c->core = 50;
            c->semantic_role   = IMG_ROLE_SKY;
            c->direction_class = IMG_FLOW_NONE;
        }
    }

    /* Explained outlier: same SKY role, spike to 200. */
    ce->cells[img_ce_idx(10, 10)].core = 200;

    /* Unexplained outlier: different role and direction, spike to 220. */
    ImgCECell* unexpl = &ce->cells[img_ce_idx(40, 40)];
    unexpl->core = 220;
    unexpl->semantic_role   = IMG_ROLE_OBJECT;
    unexpl->direction_class = IMG_FLOW_DIAGONAL_DOWN;
    uint8_t pre_delta    = unexpl->delta;
    uint8_t pre_priority = unexpl->priority;

    uint8_t* outlier_mask   = (uint8_t*)calloc(IMG_CE_TOTAL, 1);
    uint8_t* explained_mask = (uint8_t*)calloc(IMG_CE_TOTAL, 1);
    ImgResolveResult r;

    img_ce_resolve(ce, 60, outlier_mask, explained_mask, &r);

    assert(r.outlier_count   >= 2);
    assert(r.explained_count >= 1);
    assert(r.promoted_count  >= 1);

    assert(outlier_mask[img_ce_idx(10, 10)] == 1);
    assert(outlier_mask[img_ce_idx(40, 40)] == 1);
    assert(explained_mask[img_ce_idx(10, 10)] == 1);
    assert(explained_mask[img_ce_idx(40, 40)] == 0);

    /* Explained cell pulled toward neighbour mean (50) from 200.
     * After one pass: (200 + 50) / 2 = 125. */
    assert(ce->cells[img_ce_idx(10, 10)].core == 125);

    /* Unexplained cell promoted: delta +32, priority +16, delta_sign=POS. */
    assert(unexpl->delta      == (uint8_t)(pre_delta    + 32));
    assert(unexpl->priority   == (uint8_t)(pre_priority + 16));
    assert(unexpl->delta_sign == IMG_DELTA_POSITIVE);

    /* Stable cells must not be flagged. */
    assert(outlier_mask[img_ce_idx(0, 0)] == 0);
    assert(outlier_mask[img_ce_idx(20, 20)] == 0);

    free(outlier_mask);
    free(explained_mask);
    img_ce_grid_destroy(ce);
    PASS();
}

/* ── end-to-end pipeline ─────────────────────────────────── */

static void test_pipeline_end_to_end(void) {
    TEST("image → SmallCanvas → CE → coating → resolve");

    uint32_t w = 512, h = 512;
    uint8_t* img = make_test_image(w, h);
    ImgSmallCanvas* sc = img_small_canvas_create();
    ImgCEGrid* ce = img_ce_grid_create();

    img_image_to_small_canvas(img, w, h, sc);
    img_small_canvas_to_ce(sc, ce);

    /* Paint a "person, foreground, preserved" region on the middle strip.
     * add_core creates a core discontinuity at the painted boundary, which
     * lets resolve actually detect outliers against the flat mid band. */
    ImgDeltaCoating k;
    memset(&k, 0, sizeof(k));
    k.add_core     = 80;
    k.add_priority = 40;
    k.add_delta    = 20;
    k.semantic_override = IMG_ROLE_PERSON;
    k.semantic_override_on = 1;
    k.depth_override = IMG_DEPTH_FOREGROUND;
    k.depth_override_on = 1;
    k.delta_sign_override = IMG_DELTA_POSITIVE;
    k.delta_sign_override_on = 1;

    img_ce_apply_coating_region(ce, 25, 25, 40, 40, &k);

    /* Verify painted cell adopted person + foreground + sign. */
    const ImgCECell* painted = &ce->cells[img_ce_idx(30, 30)];
    assert(painted->semantic_role == IMG_ROLE_PERSON);
    assert(painted->depth_class   == IMG_DEPTH_FOREGROUND);
    assert(painted->delta_sign    == IMG_DELTA_POSITIVE);

    /* Resolve should detect the painted boundary as something worth
     * flagging (the coated region looks different from its neighbours). */
    ImgResolveResult r;
    img_ce_resolve(ce, 30, NULL, NULL, &r);

    /* Run should complete; counts can be zero on perfectly flat regions
     * but the mid/foreground transition is not flat in this fixture. */
    assert(r.outlier_count >= 1);

    free(img);
    img_small_canvas_destroy(sc);
    img_ce_grid_destroy(ce);
    PASS();
}

int main(void) {
    printf("=== test_img_ce ===\n");

    test_lifecycle();
    test_image_to_small_canvas();
    test_small_canvas_to_ce();
    test_delta_coating();
    test_resolve_sieve_and_repair();
    test_pipeline_end_to_end();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
