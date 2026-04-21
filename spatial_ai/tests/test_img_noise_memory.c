#include "img_noise_memory.h"
#include "img_ce.h"
#include "img_drawing.h"
#include "img_delta_memory.h"

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

/* ── test fixture: synthesize a deterministic CE grid ── */

static void fill_grid_pattern(ImgCEGrid* g, uint32_t salt) {
    for (uint32_t y = 0; y < IMG_CE_SIZE; y++) {
        for (uint32_t x = 0; x < IMG_CE_SIZE; x++) {
            ImgCECell* c = &g->cells[img_ce_idx(y, x)];
            c->core       = (uint8_t)((x * 3u + salt) & 0xFFu);
            c->link       = (uint8_t)((y * 5u + salt) & 0xFFu);
            c->delta      = (uint8_t)(((x + y) * 7u + salt) & 0xFFu);
            c->priority   = (uint8_t)((x ^ y ^ salt) & 0xFFu);
            c->tone_class      = (uint8_t)((x + y) % 3u);
            c->semantic_role   = (uint8_t)((x / 8u) % 7u);
            c->direction_class = (uint8_t)((y / 4u) % 5u);
            c->depth_class     = (uint8_t)((x / 16u) % 3u);
            c->delta_sign      = (uint8_t)((x + 1u) % 3u);
            c->last_delta_id   = IMG_DELTA_ID_NONE;
        }
    }
}

/* ── sizes & defaults ─────────────────────────────────────── */

static void test_compile_time_layout(void) {
    TEST("ImgNoiseSample/Profile sizes as spec");
    assert(sizeof(ImgNoiseSample)      == 8);
    assert(sizeof(ImgNoiseCellProfile) == 64);
    assert(sizeof(ImgNoiseLabelEntry)  == 16);
    PASS();
}

static void test_default_options(void) {
    TEST("sample default options sane");
    ImgNoiseSampleOptions o = img_noise_sample_default_options();
    assert(o.seed == 0);
    assert(o.k_mix == 1);
    assert(o.temperature_q8 == 256);
    assert(o.region_mask == NULL);
    PASS();
}

/* ── observe: weights accumulate monotonically ─────────────── */

static void test_observe_accumulates(void) {
    TEST("observe increases total weight across top-K");

    ImgNoiseMemory nmem;
    assert(img_noise_memory_init(&nmem));

    ImgCEGrid* g = img_ce_grid_create();
    assert(g);
    fill_grid_pattern(g, 42);

    /* Accumulated weight on cell 0 before / after. */
    uint32_t before_total = 0;
    for (int i = 0; i < IMG_NOISE_TOPK; i++) {
        before_total += nmem.cell_priors[0].top_k[i].weight;
    }
    assert(before_total == 0);

    for (int n = 0; n < 5; n++) {
        assert(img_noise_memory_observe(&nmem, g, NULL));
    }
    uint32_t after_total = 0;
    for (int i = 0; i < IMG_NOISE_TOPK; i++) {
        after_total += nmem.cell_priors[0].top_k[i].weight;
    }
    assert(after_total >= 5);

    /* Global prior should also be non-empty. */
    uint32_t global_total = 0;
    for (int i = 0; i < IMG_NOISE_TOPK; i++) {
        global_total += nmem.global_prior.top_k[i].weight;
    }
    assert(global_total > 0);

    img_ce_grid_destroy(g);
    img_noise_memory_free(&nmem);
    PASS();
}

/* ── determinism: same seed → byte-identical grids ─────────── */

static void test_sample_determinism(void) {
    TEST("sample_grid deterministic across repeated calls");

    ImgNoiseMemory nmem;
    assert(img_noise_memory_init(&nmem));

    ImgCEGrid* train = img_ce_grid_create();
    fill_grid_pattern(train, 11);
    for (int n = 0; n < 4; n++) img_noise_memory_observe(&nmem, train, NULL);
    fill_grid_pattern(train, 23);
    for (int n = 0; n < 3; n++) img_noise_memory_observe(&nmem, train, NULL);
    img_ce_grid_destroy(train);

    ImgNoiseSampleOptions opt = img_noise_sample_default_options();
    opt.seed = 0xA5A5A5A5DEADBEEFull;

    ImgCEGrid* a = img_ce_grid_create();
    ImgCEGrid* b = img_ce_grid_create();

    assert(img_noise_memory_sample_grid(&nmem, a, &opt));
    assert(img_noise_memory_sample_grid(&nmem, b, &opt));
    assert(memcmp(a->cells, b->cells,
                  IMG_CE_TOTAL * sizeof(ImgCECell)) == 0);

    /* Different seed → result must differ somewhere. */
    ImgNoiseSampleOptions opt2 = opt;
    opt2.seed = 0xA5A5A5A5DEADBEF0ull;
    ImgCEGrid* c = img_ce_grid_create();
    assert(img_noise_memory_sample_grid(&nmem, c, &opt2));
    int diff = memcmp(a->cells, c->cells,
                      IMG_CE_TOTAL * sizeof(ImgCECell));
    assert(diff != 0);

    img_ce_grid_destroy(a);
    img_ce_grid_destroy(b);
    img_ce_grid_destroy(c);
    img_noise_memory_free(&nmem);
    PASS();
}

/* ── backcompat: NULL nmem path leaves drawing_pass unchanged ── */

static void test_null_nmem_leaves_grid_untouched(void) {
    TEST("sample with empty nmem leaves cells default-zero");

    /* Empty nmem with no observations. sample_grid on an untouched
     * grid must produce the same grid because every fallback chain
     * ends with no candidates and we leave cells alone. */
    ImgNoiseMemory nmem;
    assert(img_noise_memory_init(&nmem));

    ImgCEGrid* g = img_ce_grid_create();
    assert(g);

    /* snapshot before */
    size_t bytes = IMG_CE_TOTAL * sizeof(ImgCECell);
    ImgCECell* before = (ImgCECell*)malloc(bytes);
    memcpy(before, g->cells, bytes);

    ImgNoiseSampleOptions opt = img_noise_sample_default_options();
    opt.seed = 0xCAFE;
    assert(img_noise_memory_sample_grid(&nmem, g, &opt));

    assert(memcmp(before, g->cells, bytes) == 0);

    free(before);
    img_ce_grid_destroy(g);
    img_noise_memory_free(&nmem);
    PASS();
}

/* ── save → load round-trip ────────────────────────────────── */

static void test_save_load_roundtrip(void) {
    TEST("save → load reproduces profiles byte-for-byte");

    ImgNoiseMemory a;
    assert(img_noise_memory_init(&a));

    ImgCEGrid* g = img_ce_grid_create();
    fill_grid_pattern(g, 101);
    for (int n = 0; n < 3; n++) img_noise_memory_observe(&a, g, "alpha");
    fill_grid_pattern(g, 202);
    for (int n = 0; n < 2; n++) img_noise_memory_observe(&a, g, "beta");
    img_ce_grid_destroy(g);

    const char* path = "test_nmem_roundtrip.nmem";
    assert(img_noise_memory_save(&a, path));

    ImgNoiseMemory b;
    assert(img_noise_memory_init(&b));
    assert(img_noise_memory_load(&b, path));

    assert(memcmp(&a.global_prior, &b.global_prior,
                  sizeof(ImgNoiseCellProfile)) == 0);
    assert(memcmp(a.tier_priors, b.tier_priors,
                  sizeof(a.tier_priors)) == 0);
    assert(memcmp(a.cell_priors, b.cell_priors,
                  sizeof(a.cell_priors)) == 0);
    assert(b.label_count == a.label_count);
    if (a.label_count) {
        for (uint32_t i = 0; i < a.label_count; i++) {
            assert(a.label_index[i].label_hash ==
                   b.label_index[i].label_hash);
        }
    }

    /* Sampling from the loaded copy should match sampling from the
     * original, confirming the in-memory state is complete. */
    ImgNoiseSampleOptions opt = img_noise_sample_default_options();
    opt.seed = 0x9A9AE11E11E11E11ull;
    ImgCEGrid* ga = img_ce_grid_create();
    ImgCEGrid* gb = img_ce_grid_create();
    assert(img_noise_memory_sample_grid(&a, ga, &opt));
    assert(img_noise_memory_sample_grid(&b, gb, &opt));
    assert(memcmp(ga->cells, gb->cells,
                  IMG_CE_TOTAL * sizeof(ImgCECell)) == 0);

    img_ce_grid_destroy(ga);
    img_ce_grid_destroy(gb);
    img_noise_memory_free(&a);
    img_noise_memory_free(&b);
    remove(path);
    PASS();
}

/* ── corrupt magic is rejected ─────────────────────────────── */

static void test_load_rejects_bad_magic(void) {
    TEST("load rejects file with bad magic bytes");

    const char* path = "test_nmem_bad.nmem";
    FILE* f = fopen(path, "wb");
    assert(f);
    const char bad[4] = { 'X', 'X', 'X', 'X' };
    fwrite(bad, 1, 4, f);
    for (int i = 0; i < 64; i++) fputc(0, f);
    fclose(f);

    ImgNoiseMemory n;
    assert(img_noise_memory_init(&n));
    assert(img_noise_memory_load(&n, path) == 0);
    img_noise_memory_free(&n);
    remove(path);
    PASS();
}

/* ── region mask is honoured ───────────────────────────────── */

static void test_region_mask_honored(void) {
    TEST("region_mask zeros leave cells unchanged");

    ImgNoiseMemory nmem;
    assert(img_noise_memory_init(&nmem));
    ImgCEGrid* train = img_ce_grid_create();
    fill_grid_pattern(train, 7);
    for (int n = 0; n < 4; n++) img_noise_memory_observe(&nmem, train, NULL);
    img_ce_grid_destroy(train);

    ImgCEGrid* g = img_ce_grid_create();
    /* seed cells to a unique sentinel so we can detect untouched slots */
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        g->cells[i].core = 0xAB;
    }

    uint8_t mask[IMG_CE_TOTAL];
    memset(mask, 0, sizeof(mask));
    /* unmask the first row only */
    for (uint32_t x = 0; x < IMG_CE_SIZE; x++) mask[x] = 1;

    ImgNoiseSampleOptions opt = img_noise_sample_default_options();
    opt.region_mask = mask;
    opt.seed = 0x55;
    assert(img_noise_memory_sample_grid(&nmem, g, &opt));

    /* masked-out cells keep the sentinel */
    for (uint32_t i = IMG_CE_SIZE; i < IMG_CE_TOTAL; i++) {
        assert(g->cells[i].core == 0xAB);
    }

    img_ce_grid_destroy(g);
    img_noise_memory_free(&nmem);
    PASS();
}

/* ── drawing wrapper: NULL noise path == plain drawing_pass ── */

static void test_wrapper_null_noise_matches_baseline(void) {
    TEST("drawing_pass_with_prior(NULL, ...) equals drawing_pass");

    ImgDeltaMemory* mem = img_delta_memory_create();
    assert(mem);
    ImgDeltaPayload p;
    memset(&p, 0, sizeof(p));
    for (int t = 0; t < 4; t++) {
        p.state = img_delta_state_simple(
            (uint8_t)(IMG_TIER_T1 + (t % 3)),
            (uint8_t)((t * 2) % IMG_SCALE_MAX),
            (t % 2 == 0) ? IMG_SIGN_POS : IMG_SIGN_NEG,
            (uint8_t)(IMG_MODE_INTENSITY + (t % 3)));
        img_delta_memory_add(mem, 0, p);
    }

    ImgCEGrid* g1 = img_ce_grid_create();
    ImgCEGrid* g2 = img_ce_grid_create();
    fill_grid_pattern(g1, 99);
    memcpy(g2->cells, g1->cells, IMG_CE_TOTAL * sizeof(ImgCECell));

    ImgDrawingOptions opt = img_drawing_default_options();
    opt.passes = 1;

    ImgDrawingStats s1 = {0,0,0,0,0,0}, s2 = {0,0,0,0,0,0};
    assert(img_drawing_pass(g1, mem, &opt, &s1));
    assert(img_drawing_pass_with_prior(g2, mem, NULL, NULL, &opt, &s2));

    assert(memcmp(g1->cells, g2->cells,
                  IMG_CE_TOTAL * sizeof(ImgCECell)) == 0);
    assert(s1.stamps_applied == s2.stamps_applied);

    img_ce_grid_destroy(g1);
    img_ce_grid_destroy(g2);
    img_delta_memory_destroy(mem);
    PASS();
}

int main(void) {
    printf("=== test_img_noise_memory ===\n");

    test_compile_time_layout();
    test_default_options();
    test_observe_accumulates();
    test_sample_determinism();
    test_null_nmem_leaves_grid_untouched();
    test_save_load_roundtrip();
    test_load_rejects_bad_magic();
    test_region_mask_honored();
    test_wrapper_null_noise_matches_baseline();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
