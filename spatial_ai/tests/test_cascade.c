#include "spatial_grid.h"
#include "spatial_layers.h"
#include "spatial_morpheme.h"
#include "spatial_match.h"
#include "spatial_keyframe.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_total  = 0;

#define TEST(name) do { tests_total++; printf("  [TEST] %s ... ", name); fflush(stdout); } while(0)
#define PASS()     do { tests_passed++; printf("PASS\n"); } while(0)

/* ── 1. B-channel co-occurrence hash ──
   Two clauses that contain the same set of bytes get identical B
   fingerprints, even if byte order differs. */
static void test_b_cooccurrence_hash(void) {
    TEST("B-channel hash: same vocabulary → same B");
    morpheme_init();

    SpatialGrid* g1 = grid_create();
    SpatialGrid* g2 = grid_create();
    SpatialGrid* g3 = grid_create();

    /* Same letters, different order */
    layers_encode_clause("the cat ate.",  NULL, g1);
    layers_encode_clause("cat ate the.",  NULL, g2);
    /* Different letter set */
    layers_encode_clause("dogs run fast.", NULL, g3);

    /* Find first active cell in g1 and read its B */
    uint8_t b1 = 0, b2 = 0, b3 = 0;
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (g1->A[i] > 0) { b1 = g1->B[i]; break; }
    }
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (g2->A[i] > 0) { b2 = g2->B[i]; break; }
    }
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (g3->A[i] > 0) { b3 = g3->B[i]; break; }
    }
    printf("\n    b('the cat ate')=%u  b('cat ate the')=%u  b('dogs run fast')=%u\n",
           b1, b2, b3);
    assert(b1 == b2);   /* same vocabulary → same hash */
    assert(b1 != b3);   /* different vocabulary → different hash (with high prob) */

    grid_destroy(g1);
    grid_destroy(g2);
    grid_destroy(g3);
    PASS();
}

/* ── 2. CASCADE_SEARCH returns a sensible baseline ── */
static void test_cascade_search_baseline(void) {
    TEST("CASCADE_SEARCH finds self-query with high A similarity");
    morpheme_init();
    SpatialAI* ai = spatial_ai_create();

    const char* corpus[] = {
        "alpha eats bread here.",
        "beta drinks water there.",
        "gamma runs fast home.",
        "delta jumps high tree.",
        "epsilon sleeps soft bed.",
        NULL
    };
    for (int i = 0; corpus[i]; i++) {
        ai_force_keyframe(ai, corpus[i], NULL);
    }

    /* Query with the 2nd clause verbatim — should match id 1 with high sim */
    SpatialGrid* q = grid_create();
    layers_encode_clause("beta drinks water there.", NULL, q);

    float sim;
    uint32_t id = match_cascade(ai, q, CASCADE_SEARCH, &sim);
    printf("\n    match id=%u, sim=%.3f\n", id, sim);
    assert(id == 1);
    assert(sim > 0.9f);

    grid_destroy(q);
    spatial_ai_destroy(ai);
    PASS();
}

/* ── 3. CASCADE_QA/GENERATE: step-1 early-return on exact clause ── */
static void test_cascade_early_return(void) {
    TEST("cascade early-return when A sim >= threshold");
    morpheme_init();
    SpatialAI* ai = spatial_ai_create();

    ai_force_keyframe(ai, "cats eat fish every morning.", NULL);
    ai_force_keyframe(ai, "dogs bark loud at strangers.", NULL);
    ai_force_keyframe(ai, "birds sing sweet in trees.", NULL);

    SpatialGrid* q = grid_create();
    layers_encode_clause("cats eat fish every morning.", NULL, q);

    float sim_qa = 0, sim_gen = 0;
    uint32_t id_qa  = match_cascade(ai, q, CASCADE_QA,       &sim_qa);
    uint32_t id_gen = match_cascade(ai, q, CASCADE_GENERATE, &sim_gen);

    printf("\n    QA: id=%u sim=%.3f   GEN: id=%u sim=%.3f\n",
           id_qa, sim_qa, id_gen, sim_gen);
    assert(id_qa == 0 && id_gen == 0);
    /* Both should have fired step-1 early return (sim >= 0.5) */
    assert(sim_qa  >= CASCADE_STEP1_THRESHOLD);
    assert(sim_gen >= CASCADE_STEP1_THRESHOLD);

    grid_destroy(q);
    spatial_ai_destroy(ai);
    PASS();
}

/* ── 4. CASCADE_QA: cascade step 2/3 runs on non-trivial query ── */
static void test_cascade_qa_extended(void) {
    TEST("CASCADE_QA uses step 2/3 when step 1 fails");
    morpheme_init();
    SpatialAI* ai = spatial_ai_create();

    /* Mix of themed clauses */
    const char* clauses[] = {
        "morning sun rises over mountains bright.",
        "ocean waves crash against rocky shore.",
        "children play in the park after school.",
        "ancient forests cover the remote valley.",
        "tall buildings tower above the city streets.",
        NULL
    };
    for (int i = 0; clauses[i]; i++) ai_force_keyframe(ai, clauses[i], NULL);

    /* Query with very different surface structure */
    SpatialGrid* q = grid_create();
    layers_encode_clause("waves crashing on shore.", NULL, q);

    float sim_search = 0, sim_qa = 0;
    uint32_t id_search = match_cascade(ai, q, CASCADE_SEARCH, &sim_search);
    uint32_t id_qa     = match_cascade(ai, q, CASCADE_QA,     &sim_qa);

    printf("\n    SEARCH: id=%u sim=%.3f   QA: id=%u sim=%.3f\n",
           id_search, sim_search, id_qa, sim_qa);

    /* Both should return some valid id; cascade doesn't crash */
    assert(id_search < 5);
    assert(id_qa < 5);

    grid_destroy(q);
    spatial_ai_destroy(ai);
    PASS();
}

/* ── 5. match_cascade_topk returns K results with valid IDs ── */
static void test_cascade_topk(void) {
    TEST("match_cascade_topk returns K valid ids");
    morpheme_init();
    SpatialAI* ai = spatial_ai_create();

    for (int i = 0; i < 10; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "clause number %d about varied topics here.", i);
        ai_force_keyframe(ai, buf, NULL);
    }

    SpatialGrid* q = grid_create();
    layers_encode_clause("novel clause unrelated to anything prior.", NULL, q);

    uint32_t ids[5];
    float    scores[5];
    uint32_t k = match_cascade_topk(ai, q, CASCADE_GENERATE, 5, ids, scores);
    printf("\n    k=%u  ids=[%u,%u,%u,%u,%u]\n",
           k, ids[0], ids[1], ids[2], ids[3], ids[4]);
    assert(k == 5);
    for (uint32_t i = 0; i < k; i++) assert(ids[i] < ai->kf_count);

    grid_destroy(q);
    spatial_ai_destroy(ai);
    PASS();
}

/* ── 6. ai_force_keyframe gives 1-1 mapping ── */
static void test_force_keyframe_mapping(void) {
    TEST("ai_force_keyframe: every clause → its own keyframe");
    morpheme_init();
    SpatialAI* ai = spatial_ai_create();

    /* Store very similar clauses that would collapse into deltas under
       ai_store_auto (sim >= 0.3). With ai_force_keyframe every one
       should become a new keyframe. */
    const char* similar[] = {
        "the cat runs home.",
        "the dog runs home.",
        "the fox runs home.",
        "the bat runs home.",
        NULL
    };
    for (int i = 0; similar[i]; i++) {
        uint32_t id = ai_force_keyframe(ai, similar[i], NULL);
        assert(id == (uint32_t)i);
    }
    assert(ai->kf_count == 4);
    assert(ai->df_count == 0);   /* no deltas */
    PASS();
    spatial_ai_destroy(ai);
}

int main(void) {
    printf("=== test_cascade ===\n");

    test_b_cooccurrence_hash();
    test_cascade_search_baseline();
    test_cascade_early_return();
    test_cascade_qa_extended();
    test_cascade_topk();
    test_force_keyframe_mapping();

    printf("  %d/%d passed\n\n", tests_passed, tests_total);
    return (tests_passed == tests_total) ? 0 : 1;
}
