#include "img_delta_learn.h"
#include "img_ce.h"
#include "img_delta_memory.h"
#include "img_pipeline.h"

#include <assert.h>
#include <stdint.h>
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

/* Seed a CE cell with tags + channels under full control. */
static void seed_cell(ImgCECell* c,
                      uint8_t core, uint8_t link,
                      uint8_t delta, uint8_t priority,
                      uint8_t tone, uint8_t role,
                      uint8_t dir, uint8_t depth,
                      uint8_t sign) {
    memset(c, 0, sizeof(*c));
    c->core            = core;
    c->link            = link;
    c->delta           = delta;
    c->priority        = priority;
    c->tone_class      = tone;
    c->semantic_role   = role;
    c->direction_class = dir;
    c->depth_class     = depth;
    c->delta_sign      = sign;
    c->last_delta_id   = IMG_DELTA_ID_NONE;
}

/* Make a 256x256 synthetic RGB image with 3 horizontal bands. */
static uint8_t* make_banded_image(uint32_t w, uint32_t h,
                                  uint8_t r_top, uint8_t g_top, uint8_t b_top,
                                  uint8_t r_mid, uint8_t g_mid, uint8_t b_mid,
                                  uint8_t r_bot, uint8_t g_bot, uint8_t b_bot) {
    uint8_t* img = (uint8_t*)malloc((size_t)w * h * 3);
    assert(img);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t r, g, b;
        if (y < h / 3)            { r = r_top; g = g_top; b = b_top; }
        else if (y < 2 * h / 3)   { r = r_mid; g = g_mid; b = b_mid; }
        else                       { r = r_bot; g = g_bot; b = b_bot; }
        for (uint32_t x = 0; x < w; x++) {
            size_t p = ((size_t)y * w + x) * 3;
            img[p+0] = r; img[p+1] = g; img[p+2] = b;
        }
    }
    return img;
}

/* ── identical grids produce no deltas ──────────────────── */

static void test_identical_grids_zero_deltas(void) {
    TEST("identical before/after grids add 0 deltas");

    ImgCEGrid* before = img_ce_grid_create();
    ImgCEGrid* after  = img_ce_grid_create();
    assert(before && after);

    /* Both grids are still zero-init — identical by definition. */
    ImgDeltaMemory* mem = img_delta_memory_create();
    assert(mem);

    uint32_t added = img_delta_memory_learn_from_pair(mem, before, after);
    assert(added == 0);
    assert(img_delta_memory_count(mem) == 0);

    img_delta_memory_destroy(mem);
    img_ce_grid_destroy(after);
    img_ce_grid_destroy(before);
    PASS();
}

/* ── single-cell core diff → one intensity delta ────────── */

static void test_single_cell_core_diff(void) {
    TEST("single-cell Δcore yields one MODE_INTENSITY delta");

    ImgCEGrid* before = img_ce_grid_create();
    ImgCEGrid* after  = img_ce_grid_create();
    assert(before && after);

    /* Match tags so only the numeric Δcore is a signal. */
    seed_cell(&before->cells[img_ce_idx(10, 10)],
              /*core=*/100, 50, 20, 128,
              IMG_TONE_MID, IMG_ROLE_OBJECT,
              IMG_FLOW_HORIZONTAL, IMG_DEPTH_MIDGROUND,
              IMG_DELTA_NONE);
    seed_cell(&after->cells[img_ce_idx(10, 10)],
              /*core=*/140, 50, 20, 128,     /* +40 core */
              IMG_TONE_MID, IMG_ROLE_OBJECT,
              IMG_FLOW_HORIZONTAL, IMG_DEPTH_MIDGROUND,
              IMG_DELTA_NONE);

    ImgDeltaMemory* mem = img_delta_memory_create();
    uint32_t added = img_delta_memory_learn_from_pair(mem, before, after);
    assert(added == 1);
    assert(img_delta_memory_count(mem) == 1);

    const ImgDeltaUnit* u = img_delta_memory_get(mem, 0);
    assert(u);
    assert(img_delta_state_mode(u->payload.state) == IMG_MODE_INTENSITY);
    assert(img_delta_state_sign(u->payload.state) == IMG_SIGN_POS);
    /* Magnitude 40 → tier T3 (> 24). */
    assert(img_delta_state_tier(u->payload.state) == IMG_TIER_T3);

    /* pre_key carries the before cell's tags. */
    assert(img_state_key_semantic_role  (u->pre_key) == IMG_ROLE_OBJECT);
    assert(img_state_key_depth_class    (u->pre_key) == IMG_DEPTH_MIDGROUND);
    assert(img_state_key_direction_class(u->pre_key) == IMG_FLOW_HORIZONTAL);

    /* post_hint is set, too. */
    assert(u->has_post_hint);

    img_delta_memory_destroy(mem);
    img_ce_grid_destroy(after);
    img_ce_grid_destroy(before);
    PASS();
}

/* ── tag-level diff precedence: role > direction > depth ── */

static void test_tag_precedence(void) {
    TEST("tag change takes precedence over numeric diff");

    ImgCEGrid* before = img_ce_grid_create();
    ImgCEGrid* after  = img_ce_grid_create();
    assert(before && after);

    /* Role changes AND core also changes — role should win. */
    seed_cell(&before->cells[img_ce_idx(0, 0)],
              100, 0, 0, 0,
              IMG_TONE_MID, IMG_ROLE_UNKNOWN,
              IMG_FLOW_NONE, IMG_DEPTH_BACKGROUND,
              IMG_DELTA_NONE);
    seed_cell(&after->cells[img_ce_idx(0, 0)],
              60, 0, 0, 0,                        /* Δcore = -40 */
              IMG_TONE_MID, IMG_ROLE_PERSON,      /* role change */
              IMG_FLOW_NONE, IMG_DEPTH_BACKGROUND,
              IMG_DELTA_NONE);

    /* Direction changes (and depth doesn't) — direction should win. */
    seed_cell(&before->cells[img_ce_idx(0, 1)],
              50, 0, 0, 0, IMG_TONE_MID, IMG_ROLE_OBJECT,
              IMG_FLOW_NONE, IMG_DEPTH_MIDGROUND, IMG_DELTA_NONE);
    seed_cell(&after->cells[img_ce_idx(0, 1)],
              50, 0, 0, 0, IMG_TONE_MID, IMG_ROLE_OBJECT,
              IMG_FLOW_DIAGONAL_UP, IMG_DEPTH_MIDGROUND, IMG_DELTA_NONE);

    /* Depth changes only — depth wins. */
    seed_cell(&before->cells[img_ce_idx(0, 2)],
              50, 0, 0, 0, IMG_TONE_MID, IMG_ROLE_OBJECT,
              IMG_FLOW_NONE, IMG_DEPTH_BACKGROUND, IMG_DELTA_NONE);
    seed_cell(&after->cells[img_ce_idx(0, 2)],
              50, 0, 0, 0, IMG_TONE_MID, IMG_ROLE_OBJECT,
              IMG_FLOW_NONE, IMG_DEPTH_FOREGROUND, IMG_DELTA_NONE);

    ImgDeltaMemory* mem = img_delta_memory_create();
    uint32_t added = img_delta_memory_learn_from_pair(mem, before, after);
    assert(added == 3);

    const ImgDeltaUnit* role_u = img_delta_memory_get(mem, 0);
    assert(img_delta_state_mode(role_u->payload.state) == IMG_MODE_ROLE);
    assert(role_u->payload.role_target_on);
    assert(role_u->payload.role_target == IMG_ROLE_PERSON);

    const ImgDeltaUnit* dir_u = img_delta_memory_get(mem, 1);
    assert(img_delta_state_mode(dir_u->payload.state) == IMG_MODE_DIRECTION);
    assert(img_delta_state_sign(dir_u->payload.state) == IMG_SIGN_POS);

    const ImgDeltaUnit* dep_u = img_delta_memory_get(mem, 2);
    assert(img_delta_state_mode(dep_u->payload.state) == IMG_MODE_DEPTH);
    assert(img_delta_state_sign(dep_u->payload.state) == IMG_SIGN_POS);

    img_delta_memory_destroy(mem);
    img_ce_grid_destroy(after);
    img_ce_grid_destroy(before);
    PASS();
}

/* ── noise floor: tiny Δcore is ignored ─────────────────── */

static void test_numeric_noise_floor(void) {
    TEST("sub-threshold numeric diffs are skipped (noise floor)");

    ImgCEGrid* before = img_ce_grid_create();
    ImgCEGrid* after  = img_ce_grid_create();

    /* Δcore = 2 → below LEARN_NUMERIC_NOISE_FLOOR (=3). */
    seed_cell(&before->cells[img_ce_idx(5, 5)],
              50, 0, 0, 0, IMG_TONE_MID, IMG_ROLE_OBJECT,
              IMG_FLOW_NONE, IMG_DEPTH_MIDGROUND, IMG_DELTA_NONE);
    seed_cell(&after->cells[img_ce_idx(5, 5)],
              52, 0, 0, 0, IMG_TONE_MID, IMG_ROLE_OBJECT,
              IMG_FLOW_NONE, IMG_DEPTH_MIDGROUND, IMG_DELTA_NONE);

    ImgDeltaMemory* mem = img_delta_memory_create();
    uint32_t added = img_delta_memory_learn_from_pair(mem, before, after);
    assert(added == 0);

    img_delta_memory_destroy(mem);
    img_ce_grid_destroy(after);
    img_ce_grid_destroy(before);
    PASS();
}

/* ── end-to-end: learn from synthetic pair → pipeline expansions > 0 ── */

static void test_learn_drives_pipeline_expansions(void) {
    TEST("learn from image pair → pipeline expansions > 0 on the before image");

    /* Before: dark band in the middle, warm band on top, cool on bottom. */
    uint8_t* before = make_banded_image(512, 512,
                                        /*top=*/ 200, 100,  50,
                                        /*mid=*/  30,  30,  30,
                                        /*bot=*/  50, 100, 200);

    /* After: each band shifted (warmer top, brighter mid, warmer bot). */
    uint8_t* after = make_banded_image(512, 512,
                                       /*top=*/ 220, 140,  60,
                                       /*mid=*/  80,  80,  80,
                                       /*bot=*/  80, 130, 220);

    ImgDeltaMemory* mem = img_delta_memory_create();
    uint32_t added = img_delta_memory_learn_from_images(
        mem, before, 512, 512, after, 512, 512);
    assert(added > 0);
    assert(img_delta_memory_count(mem) == added);

    /* Now run the pipeline on the BEFORE image, with the learned
     * memory. Expansions should be non-zero because seed cells can
     * find matching deltas. */
    ImgPipelineResult r = {0};
    ImgPipelineOptions opt = img_pipeline_default_options();
    opt.expansion_steps = 3;
    assert(img_pipeline_run(before, 512, 512, mem, &opt, &r));

    assert(r.stats.seed_count > 0);
    assert(r.stats.expansions > 0);   /* ← the payoff */

    /* And at least one cell now carries a real last_delta_id. */
    int mutated = 0;
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        if (r.ce_grid->cells[i].last_delta_id != IMG_DELTA_ID_NONE) {
            mutated = 1; break;
        }
    }
    assert(mutated);

    img_pipeline_result_destroy(&r);
    img_delta_memory_destroy(mem);
    free(after);
    free(before);
    PASS();
}

/* ── rarity weighting across repeated patterns ────────────── */

static void test_rarity_weight_decreases_across_duplicates(void) {
    TEST("first insert of a pattern gets max boost; duplicates decay to baseline");

    ImgDeltaMemory* m = img_delta_memory_create();
    assert(m);

    /* Stage a pair whose single changed cell produces the SAME
     * payload + pre_key every time we learn. Run learn repeatedly so
     * we see multiple inserts for one L2 bucket. */
    ImgCEGrid* before = img_ce_grid_create();
    ImgCEGrid* after  = img_ce_grid_create();
    assert(before && after);

    /* Seed one cell on both grids; make `after` differ in the core
     * channel so derive_payload emits a MODE_INTENSITY delta. */
    ImgCECell* b = &before->cells[img_ce_idx(3, 3)];
    ImgCECell* a = &after->cells [img_ce_idx(3, 3)];
    b->semantic_role = a->semantic_role = IMG_ROLE_OBJECT;
    b->tone_class    = a->tone_class    = IMG_TONE_DARK;
    b->direction_class = a->direction_class = IMG_FLOW_NONE;
    b->depth_class   = a->depth_class   = IMG_DEPTH_FOREGROUND;
    b->last_delta_id = a->last_delta_id = IMG_DELTA_ID_NONE;
    b->core = 40;
    a->core = 80;   /* |Δ| = 40 → above noise floor */

    /* First learn — bucket empty → weight = 4 × baseline. */
    uint32_t added1 = img_delta_memory_learn_from_pair(m, before, after);
    assert(added1 == 1);
    uint16_t w1 = img_delta_memory_get(m, 0)->weight;
    assert(w1 == 4 * (uint16_t)IMG_DELTA_WEIGHT_DEFAULT);   /* 4000 */

    /* Second learn — bucket has 1 → weight = 2 × baseline. */
    uint32_t added2 = img_delta_memory_learn_from_pair(m, before, after);
    assert(added2 == 1);
    uint16_t w2 = img_delta_memory_get(m, 1)->weight;
    assert(w2 == 2 * (uint16_t)IMG_DELTA_WEIGHT_DEFAULT);   /* 2000 */

    /* Third — count=2 → boost = 4/3 = 1 (integer div) → baseline. */
    uint32_t added3 = img_delta_memory_learn_from_pair(m, before, after);
    assert(added3 == 1);
    uint16_t w3 = img_delta_memory_get(m, 2)->weight;
    assert(w3 == (uint16_t)IMG_DELTA_WEIGHT_DEFAULT);       /* 1000 */

    /* Ten more — all stay at baseline, never dip below. */
    for (int i = 0; i < 10; i++) {
        img_delta_memory_learn_from_pair(m, before, after);
    }
    for (uint32_t i = 3; i < img_delta_memory_count(m); i++) {
        assert(img_delta_memory_get(m, i)->weight >=
               (uint16_t)IMG_DELTA_WEIGHT_DEFAULT);
    }
    /* Rarity order is preserved across the run. */
    assert(w1 > w2 && w2 > w3);

    img_ce_grid_destroy(before);
    img_ce_grid_destroy(after);
    img_delta_memory_destroy(m);
    PASS();
}

/* ── Multi-scale learn: cascade produces deltas + tier diversity ── */

static void test_learn_multiscale_cascade(void) {
    TEST("learn_multiscale on a blurred cascade produces tier-diverse deltas");

    /* Synthesise a 128×128 gradient-like image so the blur has
     * meaningful differences at each level. */
    const uint32_t w = 128, h = 128;
    uint8_t* img = (uint8_t*)malloc((size_t)w * h * 3);
    assert(img);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            size_t p = ((size_t)y * w + x) * 3u;
            img[p + 0] = (uint8_t)(x * 2);        /* R ramp */
            img[p + 1] = (uint8_t)(y * 2);        /* G ramp */
            img[p + 2] = (uint8_t)((x ^ y) & 0xFF); /* B xor noise */
        }
    }

    ImgDeltaMemory* mem = img_delta_memory_create();
    assert(mem);

    /* Coarsest → finest: radius 8, 2, 0 */
    uint32_t radii[] = { 8, 2, 0 };
    uint32_t added = img_delta_memory_learn_multiscale(
        mem, img, w, h, radii, 3);
    assert(added > 0);
    assert(img_delta_memory_count(mem) == added);

    /* Check that deltas span multiple tiers — the cascade should
     * naturally place coarser (big blur step) deltas at T3 and
     * finer steps at T1. */
    uint32_t tier_hist[IMG_TIER_MAX] = {0};
    for (uint32_t i = 0; i < img_delta_memory_count(mem); i++) {
        const ImgDeltaUnit* u = img_delta_memory_get(mem, i);
        uint8_t t = img_delta_state_tier(u->payload.state);
        if (t < IMG_TIER_MAX) tier_hist[t]++;
    }
    uint32_t nonempty_tiers = 0;
    for (int t = 0; t < IMG_TIER_MAX; t++) {
        if (tier_hist[t] > 0) nonempty_tiers++;
    }
    /* With a 3-level cascade (2 pairs) and a noisy image we expect
     * activity across at least 2 tier classes. */
    assert(nonempty_tiers >= 2);

    free(img);
    img_delta_memory_destroy(mem);
    PASS();
}

static void test_learn_multiscale_guards(void) {
    TEST("learn_multiscale NULL / n<2 / zero-size → 0 + no side effect");

    ImgDeltaMemory* mem = img_delta_memory_create();
    uint8_t pixel[3] = {0, 0, 0};
    uint32_t radii[] = { 4, 0 };

    assert(img_delta_memory_learn_multiscale(NULL,  pixel, 1, 1, radii, 2) == 0);
    assert(img_delta_memory_learn_multiscale(mem,   NULL,  1, 1, radii, 2) == 0);
    assert(img_delta_memory_learn_multiscale(mem,   pixel, 0, 1, radii, 2) == 0);
    assert(img_delta_memory_learn_multiscale(mem,   pixel, 1, 0, radii, 2) == 0);
    assert(img_delta_memory_learn_multiscale(mem,   pixel, 1, 1, radii, 1) == 0);
    assert(img_delta_memory_learn_multiscale(mem,   pixel, 1, 1, NULL,  2) == 0);

    assert(img_delta_memory_count(mem) == 0);
    img_delta_memory_destroy(mem);
    PASS();
}

int main(void) {
    printf("=== test_img_delta_learn ===\n");

    test_identical_grids_zero_deltas();
    test_single_cell_core_diff();
    test_tag_precedence();
    test_numeric_noise_floor();
    test_learn_drives_pipeline_expansions();
    test_rarity_weight_decreases_across_duplicates();
    test_learn_multiscale_cascade();
    test_learn_multiscale_guards();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
