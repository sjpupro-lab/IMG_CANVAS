#include "img_set16.h"
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

/* ── layout / sizes ──────────────────────────────────────── */

static void test_layout_and_sizes(void) {
    TEST("Set16 layout: 4×4 = 16, four quads × 4");

    assert(IMG_SET16_DIM   == 4);
    assert(IMG_SET16_CELLS == 16);
    assert(IMG_QUAD_CELLS  == 4);
    assert(IMG_QUAD_COUNT  == 4);

    /* SoA channel arrays must each be exactly 16 bytes (one 128-bit
     * SIMD register). 9 channels × 16 bytes = 144 bytes total. */
    ImgSet16 s;
    assert(sizeof(s.core)       == 16);
    assert(sizeof(s.link)       == 16);
    assert(sizeof(s.delta)      == 16);
    assert(sizeof(s.priority)   == 16);
    assert(sizeof(s.tone)       == 16);
    assert(sizeof(s.role)       == 16);
    assert(sizeof(s.direction)  == 16);
    assert(sizeof(s.depth)      == 16);
    assert(sizeof(s.delta_sign) == 16);
    assert(sizeof(s) == 9 * 16);

    PASS();
}

/* ── quad indices ────────────────────────────────────────── */

static void test_quad_indices(void) {
    TEST("quad indices match SPEC §6 layout");

    const uint8_t* q0 = img_set16_quad_indices(IMG_QUAD_PLUS);
    const uint8_t* q1 = img_set16_quad_indices(IMG_QUAD_MINUS);
    const uint8_t* q2 = img_set16_quad_indices(IMG_QUAD_SCALE);
    const uint8_t* q3 = img_set16_quad_indices(IMG_QUAD_PRECISION);

    /* Q0 PLUS: {0,1,4,5} */
    assert(q0[0] ==  0 && q0[1] ==  1 && q0[2] ==  4 && q0[3] ==  5);
    /* Q1 MINUS: {2,3,6,7} */
    assert(q1[0] ==  2 && q1[1] ==  3 && q1[2] ==  6 && q1[3] ==  7);
    /* Q2 SCALE: {8,9,12,13} */
    assert(q2[0] ==  8 && q2[1] ==  9 && q2[2] == 12 && q2[3] == 13);
    /* Q3 PRECISION: {10,11,14,15} */
    assert(q3[0] == 10 && q3[1] == 11 && q3[2] == 14 && q3[3] == 15);

    /* Together the four quads must cover {0..15} exactly. */
    int seen[16] = {0};
    const uint8_t* quads[4] = { q0, q1, q2, q3 };
    for (int q = 0; q < 4; q++) {
        for (int i = 0; i < IMG_QUAD_CELLS; i++) {
            assert(quads[q][i] < 16);
            assert(seen[quads[q][i]] == 0);
            seen[quads[q][i]] = 1;
        }
    }
    for (int i = 0; i < 16; i++) assert(seen[i] == 1);

    PASS();
}

/* ── quad_for(row, col) ──────────────────────────────────── */

static void test_quad_for_each_cell(void) {
    TEST("img_set16_quad_for classifies every cell correctly");

    for (uint8_t r = 0; r < IMG_SET16_DIM; r++) {
        for (uint8_t c = 0; c < IMG_SET16_DIM; c++) {
            ImgQuadRole q = img_set16_quad_for(r, c);
            ImgQuadRole expected =
                (r < IMG_QUAD_DIM && c < IMG_QUAD_DIM) ? IMG_QUAD_PLUS      :
                (r < IMG_QUAD_DIM)                     ? IMG_QUAD_MINUS     :
                (c < IMG_QUAD_DIM)                     ? IMG_QUAD_SCALE     :
                                                         IMG_QUAD_PRECISION;
            assert(q == expected);

            /* And the index appears in that quad's gather list. */
            const uint8_t flat = img_set16_idx(r, c);
            const uint8_t* idxs = img_set16_quad_indices(q);
            int found = 0;
            for (int i = 0; i < IMG_QUAD_CELLS; i++) {
                if (idxs[i] == flat) { found = 1; break; }
            }
            assert(found);
        }
    }

    PASS();
}

/* ── load/store roundtrip ───────────────────────────────── */

static void seed_ce_region(ImgCEGrid* ce, uint32_t x0, uint32_t y0) {
    for (uint32_t dy = 0; dy < IMG_SET16_DIM; dy++) {
        for (uint32_t dx = 0; dx < IMG_SET16_DIM; dx++) {
            ImgCECell* c = &ce->cells[img_ce_idx(y0 + dy, x0 + dx)];
            uint8_t v = (uint8_t)((dy << 4) | dx);  /* unique per cell */
            c->core            = (uint8_t)(v + 10);
            c->link            = (uint8_t)(v + 20);
            c->delta           = (uint8_t)(v + 30);
            c->priority        = (uint8_t)(v + 40);
            c->tone_class      = (uint8_t)((dy + dx)     % 3);
            c->semantic_role   = (uint8_t)((dy * 2 + dx) % 7);
            c->direction_class = (uint8_t)((dx)          % 5);
            c->depth_class     = (uint8_t)((dy)          % 3);
            c->delta_sign      = (uint8_t)((dx + dy)     % 3);
            c->last_delta_id   = 0xCAFEBABEu + v;
        }
    }
}

static void test_load_store_roundtrip(void) {
    TEST("CE region → Set16 → CE region preserves all 9 channels");

    ImgCEGrid* ce = img_ce_grid_create();
    assert(ce);

    const uint32_t x0 = 12, y0 = 20;
    seed_ce_region(ce, x0, y0);

    ImgSet16 s;
    img_set16_load_from_ce(ce, x0, y0, &s);

    /* Mirror-clear the region to force store-back to repopulate it. */
    for (uint32_t dy = 0; dy < IMG_SET16_DIM; dy++) {
        for (uint32_t dx = 0; dx < IMG_SET16_DIM; dx++) {
            ImgCECell* c = &ce->cells[img_ce_idx(y0 + dy, x0 + dx)];
            uint32_t saved_id = c->last_delta_id;
            memset(c, 0, sizeof(*c));
            c->last_delta_id = saved_id;   /* store-back must preserve it */
        }
    }

    img_set16_store_to_ce(&s, x0, y0, ce);

    for (uint32_t dy = 0; dy < IMG_SET16_DIM; dy++) {
        for (uint32_t dx = 0; dx < IMG_SET16_DIM; dx++) {
            ImgCECell* c = &ce->cells[img_ce_idx(y0 + dy, x0 + dx)];
            uint8_t v = (uint8_t)((dy << 4) | dx);
            assert(c->core            == (uint8_t)(v + 10));
            assert(c->link            == (uint8_t)(v + 20));
            assert(c->delta           == (uint8_t)(v + 30));
            assert(c->priority        == (uint8_t)(v + 40));
            assert(c->tone_class      == (uint8_t)((dy + dx)     % 3));
            assert(c->semantic_role   == (uint8_t)((dy * 2 + dx) % 7));
            assert(c->direction_class == (uint8_t)((dx)          % 5));
            assert(c->depth_class     == (uint8_t)((dy)          % 3));
            assert(c->delta_sign      == (uint8_t)((dx + dy)     % 3));
            /* last_delta_id must be the value preserved by the test
             * setup, NOT touched by store. */
            assert(c->last_delta_id   == 0xCAFEBABEu + v);
        }
    }

    img_ce_grid_destroy(ce);
    PASS();
}

/* ── boundary clipping ──────────────────────────────────── */

static void test_load_clips_at_grid_edge(void) {
    TEST("load_from_ce zeros out-of-grid cells");

    ImgCEGrid* ce = img_ce_grid_create();
    assert(ce);

    /* Fill grid with sentinel = 0xAA so we can detect any silent reads. */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        ce->cells[i].core = 0xAA;
    }

    /* Pre-fill Set16 with sentinel 0xFF so we can verify it gets zeroed. */
    ImgSet16 s;
    memset(&s, 0xFF, sizeof(s));

    /* Origin near the right/bottom edge: only a 2×2 sub-region falls
     * inside the grid; the other 12 cells should be zeroed. */
    const uint32_t x0 = IMG_CE_SIZE - 2;
    const uint32_t y0 = IMG_CE_SIZE - 2;
    img_set16_load_from_ce(ce, x0, y0, &s);

    int in_count = 0;
    for (uint32_t dy = 0; dy < IMG_SET16_DIM; dy++) {
        for (uint32_t dx = 0; dx < IMG_SET16_DIM; dx++) {
            uint8_t i = img_set16_idx((uint8_t)dy, (uint8_t)dx);
            const uint32_t y = y0 + dy;
            const uint32_t x = x0 + dx;
            int in = (y < IMG_CE_SIZE && x < IMG_CE_SIZE);
            if (in) {
                assert(s.core[i] == 0xAA);
                in_count++;
            } else {
                assert(s.core[i] == 0);  /* zeroed by the memset on load */
            }
        }
    }
    assert(in_count == 4);

    img_ce_grid_destroy(ce);
    PASS();
}

int main(void) {
    printf("=== test_img_set16 ===\n");

    test_layout_and_sizes();
    test_quad_indices();
    test_quad_for_each_cell();
    test_load_store_roundtrip();
    test_load_clips_at_grid_edge();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
