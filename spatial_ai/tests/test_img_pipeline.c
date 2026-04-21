#include "img_pipeline.h"
#include "img_render.h"

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

/* Build a 512x512 RGB image with three horizontal bands, mirroring
 * the img_ce test fixture:
 *   top    = cool sky     (80, 120, 200)
 *   middle = dark object  (30,  30,  30)
 *   bottom = warm ground  (160, 110,  70)
 */
static uint8_t* make_banded_image(uint32_t w, uint32_t h) {
    uint8_t* img = (uint8_t*)malloc((size_t)w * h * 3);
    assert(img);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t r, g, b;
        if      (y < h / 3)       { r =  80; g = 120; b = 200; }
        else if (y < 2 * h / 3)   { r =  30; g =  30; b =  30; }
        else                       { r = 160; g = 110; b =  70; }
        for (uint32_t x = 0; x < w; x++) {
            size_t p = ((size_t)y * w + x) * 3;
            img[p+0] = r; img[p+1] = g; img[p+2] = b;
        }
    }
    return img;
}

/* ── defaults + baseline (NULL memory) ───────────────────── */

static void test_defaults_and_baseline(void) {
    TEST("defaults + NULL-memory baseline: seeds > 0, expansions == 0");

    ImgPipelineOptions opt = img_pipeline_default_options();
    assert(opt.seed_fraction     >= 0.01f && opt.seed_fraction <= 0.05f);
    assert(opt.expansion_steps   > 0);
    assert(opt.frontier_max      > 0);
    assert(opt.resolve_threshold > 0);

    uint8_t* img = make_banded_image(512, 512);
    ImgPipelineResult r = {0};
    assert(img_pipeline_run(img, 512, 512,
                            /*memory=*/NULL, /*opt=*/NULL, &r));

    assert(r.small_canvas && r.ce_grid);
    /* Default seed_fraction = 0.03, IMG_CE_TOTAL = 4096 → ~122. */
    assert(r.stats.seed_count > 0);
    assert(r.stats.seed_count <= IMG_CE_TOTAL);
    assert(r.stats.expansions == 0);     /* no memory → no applies */
    assert(r.stats.visited    >= r.stats.seed_count);

    free(img);
    img_pipeline_result_destroy(&r);
    PASS();
}

/* ── seed fraction bounds ───────────────────────────────── */

static void test_seed_fraction_bounds(void) {
    TEST("seed_fraction = 0 → 0 seeds; seed_fraction = 1 → all cells");

    uint8_t* img = make_banded_image(256, 256);

    {
        ImgPipelineOptions opt = img_pipeline_default_options();
        opt.seed_fraction = 0.0f;
        opt.expansion_steps = 0;
        ImgPipelineResult r = {0};
        assert(img_pipeline_run(img, 256, 256, NULL, &opt, &r));
        assert(r.stats.seed_count == 0);
        assert(r.stats.visited    == 0);
        img_pipeline_result_destroy(&r);
    }

    {
        ImgPipelineOptions opt = img_pipeline_default_options();
        opt.seed_fraction = 1.0f;
        opt.expansion_steps = 0;
        ImgPipelineResult r = {0};
        assert(img_pipeline_run(img, 256, 256, NULL, &opt, &r));
        assert(r.stats.seed_count == IMG_CE_TOTAL);
        assert(r.stats.visited    == IMG_CE_TOTAL);
        img_pipeline_result_destroy(&r);
    }

    free(img);
    PASS();
}

/* ── memory-driven expansion ────────────────────────────── */

static void test_expansion_with_memory(void) {
    TEST("populated memory triggers expansion events");

    ImgDeltaMemory* mem = img_delta_memory_create();
    assert(mem);

    /* A wildcard-friendly delta: pre_key = 0 matches every cell at
     * the L6 fallback level, so the BFS will find and apply it for
     * any cell regardless of tags.
     *
     * MODE_INTENSITY + TIER_T1 + scale=2 + SIGN_POS → small core bump. */
    ImgDeltaPayload p;
    memset(&p, 0, sizeof(p));
    p.state = img_delta_state_simple(IMG_TIER_T1, 2,
                                     IMG_SIGN_POS, IMG_MODE_INTENSITY);
    img_delta_memory_add(mem, /*pre_key=*/0, p);

    uint8_t* img = make_banded_image(512, 512);

    /* With a non-empty memory, expansions should be > 0 and ≤ visited. */
    ImgPipelineResult r = {0};
    ImgPipelineOptions opt = img_pipeline_default_options();
    opt.expansion_steps = 3;
    opt.frontier_max    = 2048;
    assert(img_pipeline_run(img, 512, 512, mem, &opt, &r));

    assert(r.stats.seed_count > 0);
    assert(r.stats.expansions > 0);
    assert(r.stats.expansions <= r.stats.visited);

    /* Verify the delta actually mutated the grid: at least one cell
     * now has last_delta_id != IMG_DELTA_ID_NONE. */
    int mutated = 0;
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        if (r.ce_grid->cells[i].last_delta_id != IMG_DELTA_ID_NONE) {
            mutated = 1;
            break;
        }
    }
    assert(mutated);

    free(img);
    img_pipeline_result_destroy(&r);
    img_delta_memory_destroy(mem);
    PASS();
}

/* ── pipeline → render roundtrip ────────────────────────── */

static void test_pipeline_then_render(void) {
    TEST("pipeline result feeds img_render to a non-black image");

    uint8_t* img = make_banded_image(512, 512);
    ImgPipelineResult r = {0};
    assert(img_pipeline_run(img, 512, 512, NULL, NULL, &r));

    ImgRenderImage out = {0};
    ImgRenderOptions ropt = img_render_default_options();
    assert(img_render_ce_grid(r.ce_grid, &ropt, &out));
    assert(out.width  == IMG_CE_SIZE * ropt.cell_px);
    assert(out.height == IMG_CE_SIZE * ropt.cell_px);

    /* The banded image should produce non-zero output because the
     * CE has non-zero priority/core everywhere. */
    unsigned long long s = 0;
    size_t n = (size_t)out.width * out.height * 3u;
    for (size_t i = 0; i < n; i++) s += out.rgb[i];
    assert(s > 0);

    img_render_free_image(&out);
    img_pipeline_result_destroy(&r);
    free(img);
    PASS();
}

/* ── destroy safe on zero-init ───────────────────────────── */

static void test_destroy_zero_init_safe(void) {
    TEST("img_pipeline_result_destroy on zero-init is a no-op");
    ImgPipelineResult r = {0};
    img_pipeline_result_destroy(&r);   /* must not crash */
    img_pipeline_result_destroy(NULL); /* must not crash */
    PASS();
}

/* ── pipeline fills resolve masks, render consumes them ─── */

static void test_pipeline_masks_flow_to_render(void) {
    TEST("pipeline fills outlier/explained masks, render_masked consumes them");

    uint8_t* img = make_banded_image(512, 512);
    ImgPipelineResult r = {0};
    ImgPipelineOptions opt = img_pipeline_default_options();
    /* Low threshold → resolve more aggressive → more outliers flagged
     * at the banded image's horizontal seams. */
    opt.resolve_threshold = 10;
    assert(img_pipeline_run(img, 512, 512, NULL, &opt, &r));

    assert(r.outlier_mask   != NULL);
    assert(r.explained_mask != NULL);

    /* Mask counts agree with the stats. */
    uint32_t outlier_c   = 0;
    uint32_t explained_c = 0;
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        if (r.outlier_mask[i])   outlier_c++;
        if (r.explained_mask[i]) explained_c++;
    }
    assert(outlier_c   == r.stats.resolve_outliers);
    assert(explained_c == r.stats.resolve_explained);

    /* The banded image has clear seams — something must flag. */
    assert(outlier_c > 0);

    /* Render with and without masks — output differs iff at least one
     * cell is flagged (otherwise the mask overlay is a no-op). */
    ImgRenderOptions ropt = img_render_default_options();
    ImgRenderMasks masks = { r.outlier_mask, r.explained_mask };

    ImgRenderImage plain = {0}, tinted = {0};
    assert(img_render_ce_grid(r.ce_grid, &ropt, &plain));
    assert(img_render_ce_grid_masked(r.ce_grid, &ropt, &masks, &tinted));

    size_t n = (size_t)plain.width * plain.height * 3u;
    assert(memcmp(plain.rgb, tinted.rgb, n) != 0);

    img_render_free_image(&plain);
    img_render_free_image(&tinted);
    img_pipeline_result_destroy(&r);
    free(img);
    PASS();
}

/* ── auto feedback closes the loop ───────────────────────── */

static void test_pipeline_feedback_ingests_outcomes(void) {
    TEST("pipeline feedback=1 bumps memory usage/success after resolve");

    /* Wildcard delta (pre_key=0, L6 match on every cell) so every
     * frontier step lands a delta and the ingest step has cells to
     * credit. */
    ImgDeltaMemory* mem = img_delta_memory_create();
    ImgDeltaPayload p;
    memset(&p, 0, sizeof(p));
    p.state = img_delta_state_simple(IMG_TIER_T1, 2,
                                     IMG_SIGN_POS, IMG_MODE_INTENSITY);
    uint32_t id = img_delta_memory_add(mem, 0, p);

    uint8_t* img = make_banded_image(512, 512);
    ImgPipelineResult r = {0};
    ImgPipelineOptions opt = img_pipeline_default_options();
    assert(opt.feedback == 1);   /* default is on */
    assert(img_pipeline_run(img, 512, 512, mem, &opt, &r));

    /* expansions happened → feedback should have credited them. */
    assert(r.stats.expansions > 0);
    assert(r.stats.feedback_success + r.stats.feedback_failure
           == r.stats.expansions);
    /* At least some credits landed. */
    assert(r.stats.feedback_success + r.stats.feedback_failure > 0);

    /* The single stored delta should have usage_count == expansions
     * (every expansion credited to it, whether success or failure). */
    assert(img_delta_memory_get(mem, id)->usage_count == r.stats.expansions);

    /* Running again with feedback=0 must NOT bump counts further. */
    uint32_t before_usage = img_delta_memory_get(mem, id)->usage_count;
    uint32_t before_success = img_delta_memory_get(mem, id)->success_count;

    ImgPipelineResult r2 = {0};
    ImgPipelineOptions opt_nofb = img_pipeline_default_options();
    opt_nofb.feedback = 0;
    assert(img_pipeline_run(img, 512, 512, mem, &opt_nofb, &r2));
    assert(r2.stats.feedback_success == 0);
    assert(r2.stats.feedback_failure == 0);

    /* With feedback off, usage shouldn't change — except for the
     * per-apply bump inside img_delta_apply itself, which still
     * counts every apply. Expected net change: +expansions usage,
     * +0 success. */
    uint32_t after_usage = img_delta_memory_get(mem, id)->usage_count;
    uint32_t after_success = img_delta_memory_get(mem, id)->success_count;
    assert(after_usage   == before_usage + r2.stats.expansions);
    assert(after_success == before_success);

    img_pipeline_result_destroy(&r2);
    img_pipeline_result_destroy(&r);
    img_delta_memory_destroy(mem);
    free(img);
    PASS();
}

/* ── dimension guards ────────────────────────────────────── */

static void test_pipeline_rejects_degenerate_dims(void) {
    TEST("pipeline rejects dims outside [16..16384] per side");

    /* A 1×1 image: fails min-dim guard. */
    uint8_t tiny[3] = {128, 128, 128};
    ImgPipelineResult r = {0};
    /* fprintf to stderr is expected; we only assert the return. */
    fprintf(stderr, "  (expected [img_pipeline] reject log below)\n");
    assert(img_pipeline_run(tiny, 1, 1, NULL, NULL, &r) == 0);
    assert(r.small_canvas == NULL);
    assert(r.ce_grid      == NULL);
    img_pipeline_result_destroy(&r);

    /* 8×32 — one side below MIN_DIM. */
    uint8_t* thin = (uint8_t*)calloc(8 * 32 * 3, 1);
    assert(thin);
    fprintf(stderr, "  (expected [img_pipeline] reject log below)\n");
    ImgPipelineResult r2 = {0};
    assert(img_pipeline_run(thin, 8, 32, NULL, NULL, &r2) == 0);
    free(thin);

    /* 20000×10: max-dim trip. (We don't need to allocate a real buffer
     * this size — the guard fires before we touch pixels.) */
    uint8_t stub[1];
    ImgPipelineResult r3 = {0};
    fprintf(stderr, "  (expected [img_pipeline] reject log below)\n");
    assert(img_pipeline_run(stub, 20000, 10, NULL, NULL, &r3) == 0);

    PASS();
}

int main(void) {
    printf("=== test_img_pipeline ===\n");

    test_defaults_and_baseline();
    test_seed_fraction_bounds();
    test_expansion_with_memory();
    test_pipeline_then_render();
    test_destroy_zero_init_safe();
    test_pipeline_masks_flow_to_render();
    test_pipeline_feedback_ingests_outcomes();
    test_pipeline_rejects_degenerate_dims();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
