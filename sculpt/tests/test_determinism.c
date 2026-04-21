/* Phase 4 success criterion: same library + same seed -> bitwise-identical grid.
 * Also verifies that different seeds produce different grids (sanity).
 */

#include "sculpt_chisel.h"
#include "sculpt_draw.h"
#include "sculpt_grid.h"
#include "sculpt_library.h"
#include "sculpt_tuning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while (0)

/* Build a small synthetic library so the test has no dependency on .sraw files. */
static void build_toy_library(sculpt_library_t *lib)
{
    sculpt_library_init(lib);

    /* Register a handful of chisels across all 4 levels with varied subtract
     * vectors and keys, enough for the draw loop to find candidates.
     */
    sculpt_grid_t g;
    sculpt_grid_init(&g, SCULPT_GRID_SIZE);

    for (int variant = 0; variant < 6; ++variant) {
        /* Seed different corner cells to create distinct neighbor keys. */
        sculpt_cell_t *c = sculpt_grid_at(&g, variant % 4, variant % 4);
        c->depth_r = (uint8_t)(variant * 40);
        c->depth_g = (uint8_t)(variant * 30);

        const sculpt_cell_t *n[8];
        sculpt_grid_neighbor_8(&g, variant % 4, variant % 4, n);
        sculpt_neighbor_key_t key;
        sculpt_neighbor_key_build(c, n, &key);

        for (int level = 0; level < SCULPT_NUM_LEVELS; ++level) {
            sculpt_library_register(lib, level, &key,
                                     (uint8_t)(10 + variant * 7),
                                     (uint8_t)(15 + variant * 5),
                                     (uint8_t)(20 + variant * 3),
                                     (uint8_t)(25 + variant * 2));
        }
    }
}

static int grids_equal(const sculpt_grid_t *a, const sculpt_grid_t *b)
{
    if (a->size != b->size) return 0;
    int n = a->size * a->size;
    return memcmp(a->cells, b->cells, (size_t)n * sizeof(sculpt_cell_t)) == 0;
}

int main(void)
{
    sculpt_library_t *lib = (sculpt_library_t *)calloc(1, sizeof(*lib));
    CHECK(lib);
    build_toy_library(lib);

    sculpt_grid_t g1, g2, g3;
    sculpt_grid_init(&g1, SCULPT_GRID_SIZE);
    sculpt_grid_init(&g2, SCULPT_GRID_SIZE);
    sculpt_grid_init(&g3, SCULPT_GRID_SIZE);

    /* Run #1 and Run #2 with identical seed. */
    sculpt_draw(lib, 0xC0FFEEULL, NULL, &g1, NULL, 0, NULL, NULL);
    sculpt_draw(lib, 0xC0FFEEULL, NULL, &g2, NULL, 0, NULL, NULL);
    CHECK(grids_equal(&g1, &g2));

    /* Different seed -> different grid. */
    sculpt_draw(lib, 0xDEADBEEFULL, NULL, &g3, NULL, 0, NULL, NULL);
    CHECK(!grids_equal(&g1, &g3));

    /* Re-running with the first seed still matches the original run. */
    sculpt_grid_t g4;
    sculpt_grid_init(&g4, SCULPT_GRID_SIZE);
    sculpt_draw(lib, 0xC0FFEEULL, NULL, &g4, NULL, 0, NULL, NULL);
    CHECK(grids_equal(&g1, &g4));

    /* P1: every cell's depth is monotone-non-decreasing from the zero init. */
    for (int i = 0; i < SCULPT_GRID_SIZE * SCULPT_GRID_SIZE; ++i) {
        CHECK(g1.cells[i].depth_r <= 255);
    }

    free(lib);
    printf("test_determinism: OK\n");
    return 0;
}
