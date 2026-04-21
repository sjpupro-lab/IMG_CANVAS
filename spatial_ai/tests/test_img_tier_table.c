#include "img_tier_table.h"
#include "img_ce.h"
#include "img_render.h"

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

/* ── canonical defaults ──────────────────────────────────── */

static void test_canonical_table(void) {
    TEST("canonical tier table values match prior inline constants");

    assert(IMG_TIER_TABLE[IMG_TIER_NONE].scale_factor == 0);
    assert(IMG_TIER_TABLE[IMG_TIER_T1  ].scale_factor == 4);
    assert(IMG_TIER_TABLE[IMG_TIER_T2  ].scale_factor == 12);
    assert(IMG_TIER_TABLE[IMG_TIER_T3  ].scale_factor == 24);

    assert(IMG_TIER_TABLE[IMG_TIER_NONE].range_max == 0);
    assert(IMG_TIER_TABLE[IMG_TIER_T1  ].range_max == 8);
    assert(IMG_TIER_TABLE[IMG_TIER_T2  ].range_max == 32);
    assert(IMG_TIER_TABLE[IMG_TIER_T3  ].range_max == 96);

    /* range_max strictly increasing (so classify is well-defined). */
    assert(IMG_TIER_TABLE[IMG_TIER_T1].range_max <
           IMG_TIER_TABLE[IMG_TIER_T2].range_max);
    assert(IMG_TIER_TABLE[IMG_TIER_T2].range_max <
           IMG_TIER_TABLE[IMG_TIER_T3].range_max);

    PASS();
}

/* ── classification against canonical ────────────────────── */

static void test_classify_canonical(void) {
    TEST("img_tier_classify buckets values into tiers 0..4");

    assert(img_tier_classify(0)   == 0);
    assert(img_tier_classify(1)   == 1);
    assert(img_tier_classify(8)   == 1);   /* boundary */
    assert(img_tier_classify(9)   == 2);
    assert(img_tier_classify(32)  == 2);   /* boundary */
    assert(img_tier_classify(33)  == 3);
    assert(img_tier_classify(96)  == 3);   /* boundary */
    assert(img_tier_classify(97)  == 4);   /* saturated */
    assert(img_tier_classify(255) == 4);

    PASS();
}

/* ── classify_with: custom table ─────────────────────────── */

static void test_classify_with_custom(void) {
    TEST("img_tier_classify_with respects a custom range_max");

    ImgTierEntry custom[IMG_TIER_MAX];
    memcpy(custom, IMG_TIER_TABLE, sizeof(custom));
    custom[IMG_TIER_T1].range_max = 20;
    custom[IMG_TIER_T2].range_max = 60;
    custom[IMG_TIER_T3].range_max = 120;

    assert(img_tier_classify_with(15,  custom) == 1);
    assert(img_tier_classify_with(20,  custom) == 1);   /* boundary */
    assert(img_tier_classify_with(21,  custom) == 2);
    assert(img_tier_classify_with(121, custom) == 4);

    PASS();
}

/* ── adaptive tier (quantile) ───────────────────────────── */

static void test_adapt_quantile(void) {
    TEST("img_tier_adapt splits a skewed histogram into monotonic tiers");

    uint32_t hist[256] = {0};

    /* Mass heavily at low values: values 1..50 very common, 51..150
     * moderate, 151..255 rare. Expect t1 < t2 < t3 strictly. */
    for (int v = 1;   v <= 50;  v++) hist[v] = 100;
    for (int v = 51;  v <= 150; v++) hist[v] = 10;
    for (int v = 151; v <= 255; v++) hist[v] = 1;

    ImgTierEntry out[IMG_TIER_MAX];
    img_tier_adapt(hist, out);

    assert(out[IMG_TIER_T1].range_max <  out[IMG_TIER_T2].range_max);
    assert(out[IMG_TIER_T2].range_max <  out[IMG_TIER_T3].range_max);
    assert(out[IMG_TIER_T3].range_max <= 255);

    /* Since mass concentrates at low values, t1 should land inside
     * [1, 50] (the quartile of a left-skewed distribution). */
    assert(out[IMG_TIER_T1].range_max >= 1);
    assert(out[IMG_TIER_T1].range_max <= 50);

    /* scale_factor is preserved from canonical defaults. */
    assert(out[IMG_TIER_T1].scale_factor ==
           IMG_TIER_TABLE[IMG_TIER_T1].scale_factor);
    assert(out[IMG_TIER_T3].scale_factor ==
           IMG_TIER_TABLE[IMG_TIER_T3].scale_factor);

    PASS();
}

/* ── adapt falls back safely on empty histogram ──────────── */

static void test_adapt_empty_histogram(void) {
    TEST("empty histogram → output equals canonical defaults");

    uint32_t hist[256] = {0};
    ImgTierEntry out[IMG_TIER_MAX];
    memset(out, 0xAA, sizeof(out));
    img_tier_adapt(hist, out);

    assert(memcmp(out, IMG_TIER_TABLE, sizeof(out)) == 0);

    PASS();
}

/* ── histogram from a CE channel ────────────────────────── */

static void test_histogram_from_ce(void) {
    TEST("img_tier_build_histogram_ce counts each channel independently");

    ImgCEGrid* ce = img_ce_grid_create();
    assert(ce);

    /* Seed only two cells with distinct values so the histograms have
     * predictable non-zero bins. */
    ImgCECell* a = &ce->cells[img_ce_idx(0, 0)];
    ImgCECell* b = &ce->cells[img_ce_idx(0, 1)];
    a->core = 42; a->link = 10; a->delta =  5; a->priority = 200;
    b->core = 42; b->link = 10; b->delta = 77; b->priority =   3;

    uint32_t hist[256];

    img_tier_build_histogram_ce(ce, IMG_CE_CHANNEL_CORE, hist);
    assert(hist[42] == 2);
    assert(hist[0]  == IMG_CE_TOTAL - 2);

    img_tier_build_histogram_ce(ce, IMG_CE_CHANNEL_LINK, hist);
    assert(hist[10] == 2);

    img_tier_build_histogram_ce(ce, IMG_CE_CHANNEL_DELTA, hist);
    assert(hist[5]  == 1);
    assert(hist[77] == 1);

    img_tier_build_histogram_ce(ce, IMG_CE_CHANNEL_PRIORITY, hist);
    assert(hist[200] == 1);
    assert(hist[3]   == 1);
    /* Every other cell has priority=0, so the rest of the mass lives
     * in hist[0]. */
    assert(hist[0]   == IMG_CE_TOTAL - 2);

    /* Total mass across all bins equals cell count. */
    uint64_t total = 0;
    for (int v = 0; v < 256; v++) total += hist[v];
    assert(total == IMG_CE_TOTAL);

    img_ce_grid_destroy(ce);
    PASS();
}

/* ── options.adapt_to_ce modifies every channel spec ────── */

static void test_render_options_adapt_to_ce(void) {
    TEST("img_render_options_adapt_to_ce re-derives per-channel thresholds");

    ImgCEGrid* ce = img_ce_grid_create();
    assert(ce);

    /* Seed a skewed distribution on two channels; leave the others
     * at zero so their adapt should fall back to canonical defaults. */
    for (uint32_t i = 0; i < 500; i++) {
        ImgCECell* c = &ce->cells[i];
        c->core     = (uint8_t)(50 + (i % 20));   /* core peak near 50..69 */
        c->priority = (uint8_t)(200 + (i % 30));  /* priority peak near 200..229 */
    }

    ImgRenderOptions base = img_render_default_options();
    ImgRenderOptions adapted = base;
    img_render_options_adapt_to_ce(&adapted, ce);

    /* Core channel: values cluster around 50..69 → t1..t3 should land
     * inside that cluster (strictly above the canonical ≤ 96 mark but
     * well below the saturated cutoff). Either way it must stay
     * monotonic. */
    assert(adapted.tier_core.t1_max < adapted.tier_core.t2_max);
    assert(adapted.tier_core.t2_max < adapted.tier_core.t3_max);

    /* Priority channel populated near 200 → thresholds must shift
     * upward of the canonical range_max (96) for at least t3_max. */
    assert(adapted.tier_priority.t3_max > base.tier_priority.t3_max);

    /* Empty channels (link, delta) got a histogram of {bin 0 only} →
     * img_tier_adapt falls back to canonical defaults, so t*_max
     * equals the base values. */
    assert(adapted.tier_link.t1_max == base.tier_link.t1_max);
    assert(adapted.tier_link.t3_max == base.tier_link.t3_max);

    /* cell_px must not be touched by adapt. */
    assert(adapted.cell_px == base.cell_px);

    img_ce_grid_destroy(ce);
    PASS();
}

int main(void) {
    printf("=== test_img_tier_table ===\n");

    test_canonical_table();
    test_classify_canonical();
    test_classify_with_custom();
    test_adapt_quantile();
    test_adapt_empty_histogram();
    test_histogram_from_ce();
    test_render_options_adapt_to_ce();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
