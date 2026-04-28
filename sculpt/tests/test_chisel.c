#include "sculpt_chisel.h"
#include "sculpt_grid.h"
#include "sculpt_library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while (0)

int main(void)
{
    sculpt_grid_t g;
    sculpt_grid_init(&g, 16);

    /* Build a key at a corner — uses out-of-bounds sentinels. */
    sculpt_cell_t *self = sculpt_grid_at(&g, 0, 0);
    const sculpt_cell_t *n[8];
    sculpt_grid_neighbor_8(&g, 0, 0, n);

    sculpt_neighbor_key_t key;
    sculpt_neighbor_key_build(self, n, &key);
    /* All zero depths -> all zero quantizations. */
    CHECK(key.self_r == 0 && key.self_g == 0 && key.self_b == 0 && key.self_a == 0);
    for (int i = 0; i < 8; ++i) CHECK(key.n[i] == 0);
    CHECK(sculpt_neighbor_key_pack(&key) == 0);

    /* Mutate self to check quantization. depth 240 -> 15 (top 4 bits). */
    self->depth_r = 240;
    sculpt_neighbor_key_build(self, n, &key);
    CHECK(key.self_r == 15);
    CHECK(sculpt_neighbor_key_pack(&key) != 0);

    /* Register chisels — duplicates merge, weight increments. */
    sculpt_library_t *lib = (sculpt_library_t *)calloc(1, sizeof(*lib));
    CHECK(lib != NULL);
    sculpt_library_init(lib);

    sculpt_chisel_t *c1 = sculpt_library_register(lib, 2, &key, 10, 20, 30, 40);
    sculpt_chisel_t *c2 = sculpt_library_register(lib, 2, &key, 10, 20, 30, 40);
    CHECK(c1 == c2);  /* same entry */
    CHECK(c1->weight == 2);

    /* Different subtract -> new entry. */
    sculpt_chisel_t *c3 = sculpt_library_register(lib, 2, &key, 11, 20, 30, 40);
    CHECK(c3 != c1);
    CHECK(sculpt_library_size(lib) == 2);

    /* Lookup at level 2 returns c1 (weight=2) before c3. */
    const sculpt_chisel_t *out[4] = {0};
    int found = sculpt_library_lookup(lib, 2, &key, 4, out);
    CHECK(found == 2);
    CHECK(out[0]->weight >= out[1]->weight);
    CHECK(out[0] == c1);

    /* Lookup at a level with no entries but fallback should still return 0. */
    int none = sculpt_library_lookup(lib, 0, &key, 4, out);
    CHECK(none == 0);

    free(lib);
    printf("test_chisel: OK\n");
    return 0;
}
