#include "sculpt_grid.h"
#include <stdio.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while (0)

int main(void)
{
    sculpt_grid_t g;
    sculpt_grid_init(&g, 16);
    CHECK(g.size == 16);

    /* Empty grid = full max: RGB reconstruction is 255 everywhere. */
    uint8_t rgb[16 * 16 * 3];
    sculpt_grid_to_rgb(&g, rgb);
    for (int i = 0; i < 16 * 16 * 3; ++i) {
        CHECK(rgb[i] == 255);
    }

    /* neighbor_8 returns 8 pointers; corner has out-of-bounds sentinels (depth=0). */
    const sculpt_cell_t *n[8];
    sculpt_grid_neighbor_8(&g, 0, 0, n);
    for (int i = 0; i < 8; ++i) {
        CHECK(n[i] != NULL);
        CHECK(n[i]->depth_r == 0);
        CHECK(n[i]->depth_g == 0);
        CHECK(n[i]->depth_b == 0);
        CHECK(n[i]->depth_a == 0);
    }

    /* Interior cell sees real neighbors. Mutate one neighbor, re-read. */
    sculpt_grid_at(&g, 5, 5)->depth_r = 100;
    sculpt_grid_neighbor_8(&g, 6, 5, n);
    /* neighbor at (-1, 0) = (5, 5) is index 3 (L) in DIRS_8 */
    CHECK(n[3]->depth_r == 100);

    printf("test_grid: OK\n");
    return 0;
}
