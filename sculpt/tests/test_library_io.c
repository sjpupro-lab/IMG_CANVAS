/* Phase 6:
 *   1. Library save/load roundtrip preserves every byte of a known library.
 *   2. Learning the same image twice produces bytewise-identical .slib files
 *      (learn pipe is deterministic).
 *   3. Drawing from a loaded .slib == drawing from an in-memory library
 *      trained from the same inputs (full pipeline equivalence).
 */

#include "sculpt_chisel.h"
#include "sculpt_draw.h"
#include "sculpt_grid.h"
#include "sculpt_library.h"
#include "sculpt_libraryio.h"
#include "sculpt_learn.h"
#include "sculpt_tuning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while (0)

static void build_toy_library(sculpt_library_t *lib)
{
    sculpt_library_init(lib);
    sculpt_grid_t scratch;
    sculpt_grid_init(&scratch, SCULPT_GRID_SIZE);
    for (int v = 0; v < 5; ++v) {
        sculpt_cell_t *c = sculpt_grid_at(&scratch, v, v);
        c->depth_r = (uint8_t)(v * 50);
        c->depth_g = (uint8_t)(v * 30);
        const sculpt_cell_t *n[8];
        sculpt_grid_neighbor_8(&scratch, v, v, n);
        sculpt_neighbor_key_t key;
        sculpt_neighbor_key_build(c, n, &key);
        for (int lv = 0; lv < SCULPT_NUM_LEVELS; ++lv) {
            sculpt_library_register(lib, lv, &key,
                                     (uint8_t)(11 + v * 7),
                                     (uint8_t)(13 + v * 5),
                                     (uint8_t)(17 + v * 3),
                                     (uint8_t)(19 + v * 2));
        }
    }
}

static int libs_equal_bytes(const char *a_path, const char *b_path)
{
    FILE *a = fopen(a_path, "rb");
    FILE *b = fopen(b_path, "rb");
    if (!a || !b) { if (a) fclose(a); if (b) fclose(b); return 0; }
    int eq = 1;
    while (1) {
        uint8_t ba[4096], bb[4096];
        size_t na = fread(ba, 1, sizeof(ba), a);
        size_t nb = fread(bb, 1, sizeof(bb), b);
        if (na != nb || memcmp(ba, bb, na) != 0) { eq = 0; break; }
        if (na == 0) break;
    }
    fclose(a); fclose(b);
    return eq;
}

static int grids_equal(const sculpt_grid_t *a, const sculpt_grid_t *b)
{
    if (a->size != b->size) return 0;
    return memcmp(a->cells, b->cells,
                  (size_t)(a->size * a->size) * sizeof(sculpt_cell_t)) == 0;
}

int main(void)
{
    /* 1. Roundtrip toy library. */
    sculpt_library_t *orig = (sculpt_library_t *)calloc(1, sizeof(*orig));
    CHECK(orig);
    build_toy_library(orig);
    CHECK(sculpt_library_size(orig) > 0);

    const char *path_a = "/tmp/sculpt_p6_a.slib";
    const char *path_b = "/tmp/sculpt_p6_b.slib";
    CHECK(sculpt_library_save(path_a, orig) == 0);

    sculpt_library_t *loaded = (sculpt_library_t *)calloc(1, sizeof(*loaded));
    CHECK(loaded);
    CHECK(sculpt_library_load(path_a, loaded) == 0);

    /* Struct-level field equality on every entry. */
    CHECK(loaded->count == orig->count);
    CHECK(loaded->next_id == orig->next_id);
    for (uint32_t i = 0; i < orig->count; ++i) {
        const sculpt_chisel_t *x = &orig->items[i];
        const sculpt_chisel_t *y = &loaded->items[i];
        CHECK(x->chisel_id == y->chisel_id);
        CHECK(x->subtract_r == y->subtract_r && x->subtract_g == y->subtract_g);
        CHECK(x->subtract_b == y->subtract_b && x->subtract_a == y->subtract_a);
        CHECK(x->level == y->level);
        CHECK(x->weight == y->weight);
        CHECK(x->usage_count == y->usage_count);
        CHECK(sculpt_neighbor_key_equal(&x->pre_state, &y->pre_state));
    }

    /* 2. Save loaded lib again; files match byte-for-byte. */
    CHECK(sculpt_library_save(path_b, loaded) == 0);
    CHECK(libs_equal_bytes(path_a, path_b));

    /* 3. A draw using the loaded lib matches a draw using the original. */
    sculpt_grid_t g_orig, g_loaded;
    sculpt_grid_init(&g_orig, SCULPT_GRID_SIZE);
    sculpt_grid_init(&g_loaded, SCULPT_GRID_SIZE);
    sculpt_draw(orig,   0xABCDULL, NULL, &g_orig,   NULL, 0, NULL, NULL);
    sculpt_draw(loaded, 0xABCDULL, NULL, &g_loaded, NULL, 0, NULL, NULL);
    CHECK(grids_equal(&g_orig, &g_loaded));

    free(loaded);
    free(orig);

    /* 4. Learn-pipeline determinism: learn same image twice -> identical .slib. */
    const char *img = "data/char_01_ruby.sraw";
    FILE *probe = fopen(img, "rb");
    if (probe) {
        fclose(probe);
        sculpt_library_t *l1 = (sculpt_library_t *)calloc(1, sizeof(*l1));
        sculpt_library_t *l2 = (sculpt_library_t *)calloc(1, sizeof(*l2));
        CHECK(l1 && l2);
        sculpt_library_init(l1); sculpt_library_init(l2);
        CHECK(sculpt_learn_image(img, l1, NULL) == 0);
        CHECK(sculpt_learn_image(img, l2, NULL) == 0);
        CHECK(l1->count == l2->count);
        CHECK(l1->count > 0);

        const char *det_a = "/tmp/sculpt_p6_det_a.slib";
        const char *det_b = "/tmp/sculpt_p6_det_b.slib";
        CHECK(sculpt_library_save(det_a, l1) == 0);
        CHECK(sculpt_library_save(det_b, l2) == 0);
        CHECK(libs_equal_bytes(det_a, det_b));
        free(l1); free(l2);
        printf("test_library_io: OK (learn-determinism verified on %s)\n", img);
    } else {
        printf("test_library_io: OK (image-based determinism skipped, %s missing)\n", img);
    }
    return 0;
}
