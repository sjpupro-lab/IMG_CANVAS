#include "img_ce_diff.h"
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

static int cells_equal_ignoring_last_id(const ImgCECell* a,
                                        const ImgCECell* b) {
    return a->core          == b->core
        && a->link          == b->link
        && a->delta         == b->delta
        && a->priority      == b->priority
        && a->tone_class    == b->tone_class
        && a->semantic_role == b->semantic_role
        && a->direction_class == b->direction_class
        && a->depth_class   == b->depth_class
        && a->delta_sign    == b->delta_sign;
}

static int grids_equal_ignoring_last_id(const ImgCEGrid* a,
                                        const ImgCEGrid* b) {
    if (a->width != b->width || a->height != b->height) return 0;
    const uint32_t n = a->width * a->height;
    for (uint32_t i = 0; i < n; i++) {
        if (!cells_equal_ignoring_last_id(&a->cells[i], &b->cells[i])) {
            return 0;
        }
    }
    return 1;
}

/* ── identical → empty diff ─────────────────────────────── */

static void test_identical_grids_empty_diff(void) {
    TEST("identical grids produce a zero-entry diff");

    ImgCEGrid* a = img_ce_grid_create();
    ImgCEGrid* b = img_ce_grid_create();

    ImgCEDiff diff = {0};
    uint32_t n = img_ce_diff_compute(a, b, &diff);
    assert(n == 0);
    assert(diff.count == 0);

    /* byte_size: only the count header, no entries. */
    assert(img_ce_diff_byte_size(&diff) == sizeof(uint32_t));

    img_ce_diff_destroy(&diff);
    img_ce_grid_destroy(a);
    img_ce_grid_destroy(b);
    PASS();
}

/* ── single-cell numeric roundtrip ──────────────────────── */

static void test_single_numeric_roundtrip(void) {
    TEST("base → target single-cell Δcore roundtrips exactly");

    ImgCEGrid* base   = img_ce_grid_create();
    ImgCEGrid* target = img_ce_grid_create();

    /* Fill base with a nontrivial pattern to make sure untouched
     * cells are memcpy'd identically by apply(). */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        base->cells[i].core     = (uint8_t)(i & 0xFF);
        base->cells[i].priority = 100;
    }
    memcpy(target->cells, base->cells,
           IMG_CE_TOTAL * sizeof(ImgCECell));
    /* restore last_delta_id on target (memcpy just copied base's). */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        target->cells[i].last_delta_id = IMG_DELTA_ID_NONE;
    }

    /* Change one cell's core only. */
    target->cells[img_ce_idx(7, 11)].core = 200;

    ImgCEDiff diff = {0};
    assert(img_ce_diff_compute(base, target, &diff) == 1);
    assert(diff.entries[0].idx == img_ce_idx(7, 11));
    assert(diff.entries[0].d_core  == 200 - (int)(img_ce_idx(7, 11) & 0xFF));
    assert(diff.entries[0].tag_mask == 0);

    ImgCEGrid* applied = img_ce_grid_create();
    assert(img_ce_diff_apply(base, &diff, applied));
    assert(grids_equal_ignoring_last_id(applied, target));

    img_ce_diff_destroy(&diff);
    img_ce_grid_destroy(base);
    img_ce_grid_destroy(target);
    img_ce_grid_destroy(applied);
    PASS();
}

/* ── tag-only diff ──────────────────────────────────────── */

static void test_tag_only_diff(void) {
    TEST("pure tag change records tag_mask without channel deltas");

    ImgCEGrid* base   = img_ce_grid_create();
    ImgCEGrid* target = img_ce_grid_create();

    /* Same channels, different tags on a single cell. */
    ImgCECell* b = &base  ->cells[img_ce_idx(2, 2)];
    ImgCECell* t = &target->cells[img_ce_idx(2, 2)];
    b->core = t->core = 50;
    b->semantic_role = IMG_ROLE_UNKNOWN;
    t->semantic_role = IMG_ROLE_PERSON;
    b->depth_class   = IMG_DEPTH_BACKGROUND;
    t->depth_class   = IMG_DEPTH_FOREGROUND;

    ImgCEDiff diff = {0};
    assert(img_ce_diff_compute(base, target, &diff) == 1);
    assert(diff.entries[0].d_core == 0);
    assert(diff.entries[0].d_link == 0);
    assert(diff.entries[0].d_delta == 0);
    assert(diff.entries[0].d_priority == 0);
    assert(diff.entries[0].tag_mask & IMG_CE_DIFF_TAG_ROLE);
    assert(diff.entries[0].tag_mask & IMG_CE_DIFF_TAG_DEPTH);
    assert((diff.entries[0].tag_mask & IMG_CE_DIFF_TAG_TONE) == 0);

    ImgCEGrid* applied = img_ce_grid_create();
    assert(img_ce_diff_apply(base, &diff, applied));
    assert(applied->cells[img_ce_idx(2, 2)].semantic_role == IMG_ROLE_PERSON);
    assert(applied->cells[img_ce_idx(2, 2)].depth_class   == IMG_DEPTH_FOREGROUND);
    /* Unchanged tag is preserved from base. */
    assert(applied->cells[img_ce_idx(2, 2)].tone_class ==
           base->cells[img_ce_idx(2, 2)].tone_class);

    img_ce_diff_destroy(&diff);
    img_ce_grid_destroy(base);
    img_ce_grid_destroy(target);
    img_ce_grid_destroy(applied);
    PASS();
}

/* ── many-cell roundtrip ────────────────────────────────── */

static void test_many_cell_roundtrip(void) {
    TEST("many-cell diff roundtrips base → target exactly");

    ImgCEGrid* base   = img_ce_grid_create();
    ImgCEGrid* target = img_ce_grid_create();

    /* Different populated state on each grid. */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        base->cells[i].core     = (uint8_t)((i * 3) & 0xFF);
        base->cells[i].priority = 90;
        target->cells[i].core     = (uint8_t)((i * 7) & 0xFF);
        target->cells[i].priority = (uint8_t)(90 + ((i * 11) & 0x3F));
        if ((i % 13) == 0) target->cells[i].semantic_role = IMG_ROLE_OBJECT;
    }

    ImgCEDiff diff = {0};
    uint32_t count = img_ce_diff_compute(base, target, &diff);
    /* Just about every cell differs — count should be close to
     * IMG_CE_TOTAL. Lower bound is generous. */
    assert(count > IMG_CE_TOTAL / 2);
    assert(img_ce_diff_byte_size(&diff) ==
           sizeof(uint32_t) + count * sizeof(ImgCEDiffEntry));

    ImgCEGrid* applied = img_ce_grid_create();
    assert(img_ce_diff_apply(base, &diff, applied));
    assert(grids_equal_ignoring_last_id(applied, target));

    img_ce_diff_destroy(&diff);
    img_ce_grid_destroy(base);
    img_ce_grid_destroy(target);
    img_ce_grid_destroy(applied);
    PASS();
}

/* ── self-apply (base and out alias) ─────────────────────── */

static void test_self_apply(void) {
    TEST("apply where base == out mutates in place");

    ImgCEGrid* a = img_ce_grid_create();
    ImgCEGrid* target = img_ce_grid_create();
    target->cells[img_ce_idx(1, 1)].core     = 77;
    target->cells[img_ce_idx(5, 5)].semantic_role = IMG_ROLE_SKY;

    ImgCEDiff diff = {0};
    img_ce_diff_compute(a, target, &diff);

    /* In-place: pass the same grid as base and out. */
    assert(img_ce_diff_apply(a, &diff, a));
    assert(a->cells[img_ce_idx(1, 1)].core == 77);
    assert(a->cells[img_ce_idx(5, 5)].semantic_role == IMG_ROLE_SKY);

    img_ce_diff_destroy(&diff);
    img_ce_grid_destroy(a);
    img_ce_grid_destroy(target);
    PASS();
}

/* ── destroy on zero-init is safe ───────────────────────── */

static void test_destroy_zero_init(void) {
    TEST("img_ce_diff_destroy on a zero-init struct is a no-op");
    ImgCEDiff d = {0};
    img_ce_diff_destroy(&d);
    img_ce_diff_destroy(NULL);
    PASS();
}

int main(void) {
    printf("=== test_img_ce_diff ===\n");

    test_identical_grids_empty_diff();
    test_single_numeric_roundtrip();
    test_tag_only_diff();
    test_many_cell_roundtrip();
    test_self_apply();
    test_destroy_zero_init();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
