/* Phase 5: edit_rect isolates mutations to a rect, and a captured edit log
 * replays bitwise to the same grid on a blank canvas. Plus: log save/load
 * roundtrip preserves every entry.
 */

#include "sculpt_chisel.h"
#include "sculpt_draw.h"
#include "sculpt_grid.h"
#include "sculpt_library.h"
#include "sculpt_logio.h"
#include "sculpt_tuning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while (0)

/* Shared toy library; keep keys varied so the draw loop finds candidates. */
static void build_toy_library(sculpt_library_t *lib)
{
    sculpt_library_init(lib);
    sculpt_grid_t scratch;
    sculpt_grid_init(&scratch, SCULPT_GRID_SIZE);
    for (int v = 0; v < 6; ++v) {
        sculpt_cell_t *c = sculpt_grid_at(&scratch, v % 4, v % 4);
        c->depth_r = (uint8_t)(v * 40);
        c->depth_g = (uint8_t)(v * 30);
        const sculpt_cell_t *n[8];
        sculpt_grid_neighbor_8(&scratch, v % 4, v % 4, n);
        sculpt_neighbor_key_t key;
        sculpt_neighbor_key_build(c, n, &key);
        for (int lv = 0; lv < SCULPT_NUM_LEVELS; ++lv) {
            sculpt_library_register(lib, lv, &key,
                                     (uint8_t)(10 + v * 7),
                                     (uint8_t)(15 + v * 5),
                                     (uint8_t)(20 + v * 3),
                                     (uint8_t)(25 + v * 2));
        }
    }
}

static int grids_equal(const sculpt_grid_t *a, const sculpt_grid_t *b)
{
    if (a->size != b->size) return 0;
    return memcmp(a->cells, b->cells,
                  (size_t)(a->size * a->size) * sizeof(sculpt_cell_t)) == 0;
}

/* Log capacity for default iters (1+2+2+2)=7 sweeps * 16*16 = 1792 entries. */
#define LOG_CAP 2048

int main(void)
{
    sculpt_library_t *lib = (sculpt_library_t *)calloc(1, sizeof(*lib));
    CHECK(lib);
    build_toy_library(lib);

    /* 1. edit_rect on the full canvas == sculpt_draw on a fresh grid. */
    sculpt_grid_t full_draw, full_edit;
    sculpt_grid_init(&full_draw, SCULPT_GRID_SIZE);
    sculpt_grid_init(&full_edit, SCULPT_GRID_SIZE);

    sculpt_draw(lib, 0xC0FFEEULL, NULL, &full_draw, NULL, 0, NULL, NULL);
    sculpt_rect_t full = { 0, 0, SCULPT_GRID_SIZE, SCULPT_GRID_SIZE };
    sculpt_edit_rect(lib, 0xC0FFEEULL, NULL, full, &full_edit, NULL, 0, NULL, NULL);
    CHECK(grids_equal(&full_draw, &full_edit));

    /* 2. Small rect keeps out-of-rect cells untouched. */
    sculpt_grid_t rect_grid;
    sculpt_grid_init(&rect_grid, SCULPT_GRID_SIZE);
    sculpt_rect_t small = { 4, 4, 4, 4 };
    sculpt_edit_rect(lib, 42, NULL, small, &rect_grid, NULL, 0, NULL, NULL);
    for (int y = 0; y < SCULPT_GRID_SIZE; ++y) {
        for (int x = 0; x < SCULPT_GRID_SIZE; ++x) {
            const sculpt_cell_t *c = sculpt_grid_at_const(&rect_grid, x, y);
            int inside = (x >= 4 && x < 8 && y >= 4 && y < 8);
            if (!inside) {
                CHECK(c->depth_r == 0 && c->depth_g == 0 && c->depth_b == 0 && c->depth_a == 0);
                CHECK(c->last_chisel_id == 0);
            }
        }
    }

    /* 3. Replay roundtrip: capture log during draw, replay on blank grid,
     * expect bitwise-identical result.
     */
    sculpt_grid_t orig;
    sculpt_grid_init(&orig, SCULPT_GRID_SIZE);
    sculpt_edit_log_entry_t *log = (sculpt_edit_log_entry_t *)
        calloc(LOG_CAP, sizeof(*log));
    CHECK(log);
    int log_n = 0;
    sculpt_draw(lib, 0xBEEFULL, NULL, &orig, log, LOG_CAP, &log_n, NULL);
    CHECK(log_n > 0);
    CHECK(log_n < LOG_CAP);

    sculpt_grid_t replayed;
    sculpt_grid_init(&replayed, SCULPT_GRID_SIZE);
    int rc = sculpt_replay(&replayed, lib, 0xBEEFULL, log, log_n);
    CHECK(rc == 0);
    CHECK(grids_equal(&orig, &replayed));

    /* 4. Log save/load roundtrip preserves all entries. */
    const char *log_path = "/tmp/sculpt_phase5_test.slog";
    CHECK(sculpt_log_save(log_path, log, log_n) == 0);

    sculpt_edit_log_entry_t *loaded = (sculpt_edit_log_entry_t *)
        calloc(LOG_CAP, sizeof(*loaded));
    CHECK(loaded);
    int loaded_n = 0;
    CHECK(sculpt_log_load(log_path, loaded, LOG_CAP, &loaded_n) == 0);
    CHECK(loaded_n == log_n);
    CHECK(memcmp(loaded, log, (size_t)log_n * sizeof(*log)) == 0);

    /* 5. Replay from the loaded log also matches. */
    sculpt_grid_t replayed2;
    sculpt_grid_init(&replayed2, SCULPT_GRID_SIZE);
    CHECK(sculpt_replay(&replayed2, lib, 0xBEEFULL, loaded, loaded_n) == 0);
    CHECK(grids_equal(&orig, &replayed2));

    free(loaded);
    free(log);
    free(lib);
    printf("test_edit: OK (log_n=%d)\n", log_n);
    return 0;
}
