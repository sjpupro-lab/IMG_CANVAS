#include "img_drawing.h"
#include "img_delta_memory.h"
#include "img_delta_learn.h"
#include "img_ce.h"

#include <assert.h>
#include <stdio.h>
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

/* ── defaults ────────────────────────────────────────────── */

static void test_default_options(void) {
    TEST("drawing default options sane");

    ImgDrawingOptions o = img_drawing_default_options();
    assert(o.top_g            >= 1);
    assert(o.presence_penalty >  0.0);
    assert(o.passes           >= 1);
    /* skip_zero_cells may be 0 or 1; just sanity. */
    assert(o.skip_zero_cells == 0 || o.skip_zero_cells == 1);

    PASS();
}

/* ── no-op when memory is missing ────────────────────────── */

static void test_null_memory_is_noop(void) {
    TEST("NULL or empty memory → no stamps, returns success");

    ImgCEGrid* grid = img_ce_grid_create();
    assert(grid);

    ImgDrawingStats st;
    memset(&st, 0xAA, sizeof(st));
    assert(img_drawing_pass(grid, NULL, NULL, &st) == 1);
    assert(st.stamps_applied    == 0);
    assert(st.cells_visited     == 0);
    assert(st.unique_deltas_used == 0);

    ImgDeltaMemory* empty = img_delta_memory_create();
    memset(&st, 0xAA, sizeof(st));
    assert(img_drawing_pass(grid, empty, NULL, &st) == 1);
    assert(st.stamps_applied == 0);

    img_delta_memory_destroy(empty);
    img_ce_grid_destroy(grid);
    PASS();
}

/* ── drawing pass actually stamps cells ──────────────────── */

static void test_drawing_pass_stamps_cells(void) {
    TEST("drawing_pass stamps cells (last_delta_id set + stats reflect)");

    /* Populate memory with a few wildcard deltas (pre_key=0 matches
     * every cell at the L6 fallback level). */
    ImgDeltaMemory* mem = img_delta_memory_create();
    ImgDeltaPayload p;
    for (int t = 0; t < 4; t++) {
        memset(&p, 0, sizeof(p));
        p.state = img_delta_state_simple(
            (uint8_t)(IMG_TIER_T1 + (t % 3)),
            (uint8_t)((t * 2) % IMG_SCALE_MAX),
            (t % 2 == 0) ? IMG_SIGN_POS : IMG_SIGN_NEG,
            (uint8_t)(IMG_MODE_INTENSITY + (t % 3)));
        img_delta_memory_add(mem, 0, p);
    }

    ImgCEGrid* grid = img_ce_grid_create();
    assert(grid);
    /* All cells start with last_delta_id = IMG_DELTA_ID_NONE. */
    assert(grid->cells[0].last_delta_id == IMG_DELTA_ID_NONE);

    ImgDrawingOptions opt = img_drawing_default_options();
    opt.top_g = 4;
    opt.passes = 1;
    opt.skip_zero_cells = 0;

    ImgDrawingStats st = {0};
    assert(img_drawing_pass(grid, mem, &opt, &st));
    assert(st.stamps_applied == IMG_CE_TOTAL);
    assert(st.cells_visited  == IMG_CE_TOTAL);
    assert(st.unique_deltas_used >= 2);   /* penalty should diversify */

    /* Every cell got a last_delta_id. */
    int mutated = 0;
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        if (grid->cells[i].last_delta_id != IMG_DELTA_ID_NONE) mutated++;
    }
    assert(mutated == (int)IMG_CE_TOTAL);

    img_ce_grid_destroy(grid);
    img_delta_memory_destroy(mem);
    PASS();
}

/* ── presence penalty diversifies picks ──────────────────── */

static void test_presence_penalty_diversifies(void) {
    TEST("presence penalty: α > 0 uses more unique deltas than α == 0");

    /* Seed 4 deltas; one has a big veteran success record so argmax
     * would otherwise dominate. */
    ImgDeltaMemory* mem = img_delta_memory_create();
    ImgDeltaPayload p;
    for (int t = 0; t < 4; t++) {
        memset(&p, 0, sizeof(p));
        p.state = img_delta_state_simple(IMG_TIER_T1 + (t % 3),
                                         2, IMG_SIGN_POS,
                                         IMG_MODE_INTENSITY);
        img_delta_memory_add(mem, 0, p);
    }
    /* Boost unit 0 heavily so without a penalty it would dominate. */
    for (int j = 0; j < 100; j++) {
        img_delta_memory_record_usage(mem, 0, (j < 90) ? 1 : 0);
    }

    /* Pass 1: no penalty, same grid → single dominant pick. */
    ImgDrawingOptions o_noα = img_drawing_default_options();
    o_noα.presence_penalty = 0.0;
    ImgCEGrid* g1 = img_ce_grid_create();
    ImgDrawingStats s_noα;
    img_drawing_pass(g1, mem, &o_noα, &s_noα);

    /* Pass 2: with penalty, same memory, fresh grid. */
    ImgDrawingOptions o_α = img_drawing_default_options();
    o_α.presence_penalty = 0.5;
    ImgCEGrid* g2 = img_ce_grid_create();
    ImgDrawingStats s_α;
    img_drawing_pass(g2, mem, &o_α, &s_α);

    /* Penalty should not reduce diversity — and in practice makes
     * the max-repeat count drop because it pushes on popular picks
     * faster than the implicit usage_count-bump degradation can.
     *
     * (Implicit degradation: img_delta_apply bumps usage_count, so
     * Laplace-smoothed success_rate of the veteran drops over a
     * long pass. That naturally diversifies too — the explicit
     * penalty just accelerates it per-pick.) */
    assert(s_α.unique_deltas_used >= s_noα.unique_deltas_used);
    assert(s_α.max_recent_count   <= s_noα.max_recent_count);
    /* With explicit penalty, diversification is immediate — at least
     * 2 distinct deltas must see action. */
    assert(s_α.unique_deltas_used >= 2);

    img_ce_grid_destroy(g1);
    img_ce_grid_destroy(g2);
    img_delta_memory_destroy(mem);
    PASS();
}

/* ── skip_zero_cells respects the flag ───────────────────── */

static void test_skip_zero_cells(void) {
    TEST("skip_zero_cells = 1 only stamps already-populated cells");

    ImgDeltaMemory* mem = img_delta_memory_create();
    ImgDeltaPayload p;
    memset(&p, 0, sizeof(p));
    p.state = img_delta_state_simple(IMG_TIER_T1, 2, IMG_SIGN_POS,
                                     IMG_MODE_INTENSITY);
    img_delta_memory_add(mem, 0, p);

    ImgCEGrid* grid = img_ce_grid_create();
    /* Seed 10 cells with non-zero core; leave the rest at 0. */
    for (uint32_t i = 0; i < 10; i++) {
        grid->cells[i].core = 50;
    }

    ImgDrawingOptions opt = img_drawing_default_options();
    opt.skip_zero_cells = 1;
    opt.top_g = 1;

    ImgDrawingStats st;
    img_drawing_pass(grid, mem, &opt, &st);
    assert(st.cells_visited == 10);
    assert(st.stamps_applied == 10);

    img_ce_grid_destroy(grid);
    img_delta_memory_destroy(mem);
    PASS();
}

/* ── Brush: region_mask restricts stamping area ──────── */

static void test_brush_region_mask_scope(void) {
    TEST("region_mask gates stamping to the masked cells only");

    ImgDeltaMemory* mem = img_delta_memory_create();
    ImgDeltaPayload p;
    memset(&p, 0, sizeof(p));
    p.state = img_delta_state_simple(IMG_TIER_T1, 2, IMG_SIGN_POS,
                                     IMG_MODE_INTENSITY);
    img_delta_memory_add(mem, 0, p);

    ImgCEGrid* grid = img_ce_grid_create();

    /* Mask: 8×8 rect in the top-left. */
    uint8_t mask[IMG_CE_TOTAL];
    img_brush_mask_rect(mask, 0, 0, 8, 8);

    ImgDrawingOptions opt = img_drawing_default_options();
    opt.region_mask = mask;
    opt.top_g = 1;

    ImgDrawingStats st = {0, 0, 0, 0, 0, 0};
    assert(img_drawing_pass(grid, mem, &opt, &st));

    /* 64 cells in the 8×8 rect; rest of the grid (IMG_CE_TOTAL - 64)
     * is masked out. */
    assert(st.cells_visited     == 64);
    assert(st.cells_masked_out  == IMG_CE_TOTAL - 64);
    assert(st.stamps_applied    == 64);

    /* Inside-mask cells have last_delta_id set; outside untouched. */
    for (uint32_t y = 0; y < IMG_CE_SIZE; y++) {
        for (uint32_t x = 0; x < IMG_CE_SIZE; x++) {
            uint32_t idx = img_ce_idx(y, x);
            if (y < 8 && x < 8) {
                assert(grid->cells[idx].last_delta_id != IMG_DELTA_ID_NONE);
            } else {
                assert(grid->cells[idx].last_delta_id == IMG_DELTA_ID_NONE);
            }
        }
    }

    img_ce_grid_destroy(grid);
    img_delta_memory_destroy(mem);
    PASS();
}

/* ── Brush: target_tier shifts picks toward matching tier ── */

static void test_brush_target_tier_shifts_picks(void) {
    TEST("target_tier + tier_bonus reorders the winner");

    /* Three identical-key deltas at different tiers. */
    ImgDeltaMemory* mem = img_delta_memory_create();
    ImgStateKey k = img_state_key_make(IMG_ROLE_UNKNOWN, IMG_TONE_DARK,
                                       IMG_FLOW_NONE, IMG_DEPTH_BACKGROUND,
                                       0, IMG_DELTA_NONE);
    uint32_t id_t1, id_t2, id_t3;
    {
        ImgDeltaPayload p;
        memset(&p, 0, sizeof(p));
        p.state = img_delta_state_simple(IMG_TIER_T1, 2, IMG_SIGN_POS,
                                         IMG_MODE_INTENSITY);
        id_t1 = img_delta_memory_add(mem, k, p);
    }
    {
        ImgDeltaPayload p;
        memset(&p, 0, sizeof(p));
        p.state = img_delta_state_simple(IMG_TIER_T2, 2, IMG_SIGN_POS,
                                         IMG_MODE_INTENSITY);
        id_t2 = img_delta_memory_add(mem, k, p);
    }
    {
        ImgDeltaPayload p;
        memset(&p, 0, sizeof(p));
        p.state = img_delta_state_simple(IMG_TIER_T3, 2, IMG_SIGN_POS,
                                         IMG_MODE_INTENSITY);
        id_t3 = img_delta_memory_add(mem, k, p);
    }
    /* All three tie on StateKey match; give id_t1 a heavy usage bump
     * so it would win by default (higher Laplace success_rate at
     * start), then the brush biases toward T3. */
    for (int i = 0; i < 50; i++) img_delta_memory_record_usage(mem, id_t1, 1);

    ImgCEGrid* grid = img_ce_grid_create();
    /* Stamp just one cell for a clean before/after comparison. */
    uint8_t mask[IMG_CE_TOTAL];
    img_brush_mask_rect(mask, 0, 0, 1, 1);

    /* Baseline: no tier bias — id_t1 should win (fresh usage edge). */
    {
        ImgDrawingOptions opt = img_drawing_default_options();
        opt.region_mask = mask;
        opt.top_g = 4;
        opt.passes = 1;
        ImgDrawingStats st = {0, 0, 0, 0, 0, 0};
        assert(img_drawing_pass(grid, mem, &opt, &st));
        assert(st.stamps_applied == 1);
        assert(grid->cells[0].last_delta_id == id_t1);
    }

    /* Brush with target_tier = T3 should shift the winner to id_t3. */
    ImgCEGrid* grid2 = img_ce_grid_create();
    {
        ImgDrawingOptions opt = img_drawing_default_options();
        opt.region_mask = mask;
        opt.top_g = 4;
        opt.passes = 1;
        opt.target_tier = IMG_TIER_T3;
        opt.tier_bonus  = 1.0;   /* strong enough to beat any Laplace gap */
        ImgDrawingStats st = {0, 0, 0, 0, 0, 0};
        assert(img_drawing_pass(grid2, mem, &opt, &st));
        assert(st.brush_bonus_wins >= 1);
        assert(grid2->cells[0].last_delta_id == id_t3);
    }

    (void)id_t2;
    img_ce_grid_destroy(grid);
    img_ce_grid_destroy(grid2);
    img_delta_memory_destroy(mem);
    PASS();
}

/* ── Brush: target_role shifts picks toward matching role ── */

static void test_brush_target_role_shifts_picks(void) {
    TEST("target_role + role_bonus reorders by pre_key.semantic_role");

    ImgDeltaMemory* mem = img_delta_memory_create();
    ImgDeltaPayload p;
    memset(&p, 0, sizeof(p));
    p.state = img_delta_state_simple(IMG_TIER_T1, 2, IMG_SIGN_POS,
                                     IMG_MODE_INTENSITY);

    /* Two deltas that fallback-match the same cell (both have
     * role=SKY vs role=PERSON; the cell is UNKNOWN so both hit at
     * the L5+ fallback where role is dropped). Use key equal to
     * each role. */
    ImgStateKey k_sky = img_state_key_make(IMG_ROLE_SKY, IMG_TONE_DARK,
                                           IMG_FLOW_NONE, IMG_DEPTH_BACKGROUND,
                                           0, IMG_DELTA_NONE);
    ImgStateKey k_person = img_state_key_make(IMG_ROLE_PERSON, IMG_TONE_DARK,
                                              IMG_FLOW_NONE, IMG_DEPTH_BACKGROUND,
                                              0, IMG_DELTA_NONE);
    uint32_t id_sky    = img_delta_memory_add(mem, k_sky, p);
    uint32_t id_person = img_delta_memory_add(mem, k_person, p);

    /* Cell: UNKNOWN role. Both deltas match only at fallback level.
     * Without brush, whichever appears first in the candidates list
     * wins. Brush bias: prefer PERSON → id_person wins. */
    ImgCEGrid* grid = img_ce_grid_create();
    uint8_t mask[IMG_CE_TOTAL];
    img_brush_mask_rect(mask, 0, 0, 1, 1);

    ImgDrawingOptions opt = img_drawing_default_options();
    opt.region_mask = mask;
    opt.top_g = 4;
    opt.target_role = IMG_ROLE_PERSON;
    opt.role_bonus  = 0.50;

    ImgDrawingStats st = {0, 0, 0, 0, 0, 0};
    assert(img_drawing_pass(grid, mem, &opt, &st));
    assert(st.stamps_applied == 1);
    assert(grid->cells[0].last_delta_id == id_person);

    (void)id_sky;
    img_ce_grid_destroy(grid);
    img_delta_memory_destroy(mem);
    PASS();
}

/* ── Brush: rect helper ──────────────────────────────── */

static void test_brush_mask_rect_helper(void) {
    TEST("img_brush_mask_rect fills interior + clamps out-of-bounds");

    uint8_t mask[IMG_CE_TOTAL];
    memset(mask, 0xFF, IMG_CE_TOTAL);
    img_brush_mask_rect(mask, 4, 4, 12, 10);

    for (uint32_t y = 0; y < IMG_CE_SIZE; y++) {
        for (uint32_t x = 0; x < IMG_CE_SIZE; x++) {
            uint8_t v = mask[img_ce_idx(y, x)];
            if (x >= 4 && x < 12 && y >= 4 && y < 10) {
                assert(v == 1);
            } else {
                assert(v == 0);
            }
        }
    }

    /* Out-of-bounds: clamp to grid. */
    img_brush_mask_rect(mask, 0, 0, 1000, 1000);
    /* Every cell should be set. */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) assert(mask[i] == 1);

    /* Degenerate: x0 >= x1 → all zero. */
    img_brush_mask_rect(mask, 10, 10, 10, 10);
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) assert(mask[i] == 0);

    PASS();
}

int main(void) {
    printf("=== test_img_drawing ===\n");

    test_default_options();
    test_null_memory_is_noop();
    test_drawing_pass_stamps_cells();
    test_presence_penalty_diversifies();
    test_skip_zero_cells();

    test_brush_mask_rect_helper();
    test_brush_region_mask_scope();
    test_brush_target_tier_shifts_picks();
    test_brush_target_role_shifts_picks();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
