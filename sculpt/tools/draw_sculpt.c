/* draw_sculpt — Phase 4 drawing CLI.
 *
 * Usage: draw_sculpt <seed> <out.sraw> <train1.sraw> [train2.sraw ...]
 *
 * 1. Learns chisels from all training images into a single library.
 * 2. Draws onto a blank grid using (library, seed).
 * 3. Writes the grid's RGB reconstruction to out.sraw.
 *
 * Phase 4 success criterion: running the same command twice produces
 * bitwise-identical out.sraw files.
 */

#include "sculpt_draw.h"
#include "sculpt_grid.h"
#include "sculpt_library.h"
#include "sculpt_rawio.h"
#include "sculpt_tuning.h"
#include "cli_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <seed> <out.sraw> <library.slib | train*.sraw ...>\n", argv[0]);
        return 2;
    }

    uint64_t seed = strtoull(argv[1], NULL, 0);
    const char *out_path = argv[2];

    sculpt_library_t *lib = cli_load_library(&argv[3], argc - 3);
    if (!lib) return 3;
    printf("[train] library size: %u (L0:%u L1:%u L2:%u L3:%u)\n",
           sculpt_library_size(lib),
           sculpt_library_size_at_level(lib, 0),
           sculpt_library_size_at_level(lib, 1),
           sculpt_library_size_at_level(lib, 2),
           sculpt_library_size_at_level(lib, 3));

    sculpt_grid_t grid;
    sculpt_grid_init(&grid, SCULPT_GRID_SIZE);

    sculpt_draw_stats_t stats;
    int log_written = 0;
    sculpt_draw(lib, seed, NULL, &grid, NULL, 0, &log_written, &stats);

    printf("[draw] seed=%llu decisions=L0:%d L1:%d L2:%d L3:%d\n",
           (unsigned long long)seed,
           stats.score_count[0], stats.score_count[1],
           stats.score_count[2], stats.score_count[3]);

    uint8_t rgb[SCULPT_GRID_SIZE * SCULPT_GRID_SIZE * 3];
    sculpt_grid_to_rgb(&grid, rgb);

    int rc = sculpt_image_save_raw(out_path, SCULPT_GRID_SIZE, SCULPT_GRID_SIZE, rgb);
    if (rc != 0) {
        fprintf(stderr, "save failed (rc=%d)\n", rc);
        free(lib);
        return 4;
    }
    printf("[write] %s (%dx%d RGB)\n", out_path, SCULPT_GRID_SIZE, SCULPT_GRID_SIZE);

    free(lib);
    return 0;
}
