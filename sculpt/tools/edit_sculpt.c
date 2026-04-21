/* edit_sculpt — Phase 5 CLI demonstrating rect edit + log replay.
 *
 * Usage:
 *   edit_sculpt draw   <seed> <out.sraw> <log.slog> <train1.sraw> [train2 ...]
 *       full-canvas draw; writes out image AND captures edit log
 *
 *   edit_sculpt rect   <seed> <x> <y> <w> <h> <out.sraw> <train1.sraw> [...]
 *       edit only the (x,y,w,h) region of a blank canvas
 *
 *   edit_sculpt replay <seed> <log.slog> <out.sraw> <train1.sraw> [...]
 *       replay a saved log onto a blank canvas -> out.sraw
 *       Success criterion: replay output matches the original `draw` output
 *       byte-for-byte when train set and seed match.
 */

#include "sculpt_draw.h"
#include "sculpt_grid.h"
#include "sculpt_learn.h"
#include "sculpt_library.h"
#include "sculpt_logio.h"
#include "sculpt_rawio.h"
#include "sculpt_tuning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_CAP 4096

static sculpt_library_t *learn_all(char **paths, int n_paths)
{
    sculpt_library_t *lib = (sculpt_library_t *)calloc(1, sizeof(*lib));
    if (!lib) return NULL;
    sculpt_library_init(lib);
    for (int i = 0; i < n_paths; ++i) {
        if (sculpt_learn_image(paths[i], lib, NULL) != 0) {
            fprintf(stderr, "learn failed for %s\n", paths[i]);
            free(lib);
            return NULL;
        }
    }
    return lib;
}

static int write_grid(const sculpt_grid_t *grid, const char *path)
{
    uint8_t rgb[SCULPT_GRID_SIZE * SCULPT_GRID_SIZE * 3];
    sculpt_grid_to_rgb(grid, rgb);
    return sculpt_image_save_raw(path, grid->size, grid->size, rgb);
}

static int cmd_draw(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: draw <seed> <out.sraw> <log.slog> <train1> [train2 ...]\n");
        return 2;
    }
    uint64_t seed = strtoull(argv[2], NULL, 0);
    const char *out_path = argv[3];
    const char *log_path = argv[4];
    sculpt_library_t *lib = learn_all(&argv[5], argc - 5);
    if (!lib) return 3;

    sculpt_grid_t g;
    sculpt_grid_init(&g, SCULPT_GRID_SIZE);
    sculpt_edit_log_entry_t *log = (sculpt_edit_log_entry_t *)calloc(LOG_CAP, sizeof(*log));
    int log_n = 0;
    sculpt_draw(lib, seed, NULL, &g, log, LOG_CAP, &log_n, NULL);

    if (write_grid(&g, out_path) != 0) { free(log); free(lib); return 4; }
    if (sculpt_log_save(log_path, log, log_n) != 0) { free(log); free(lib); return 5; }
    printf("[draw] seed=%llu log=%d entries -> %s, %s\n",
           (unsigned long long)seed, log_n, out_path, log_path);
    free(log); free(lib);
    return 0;
}

static int cmd_rect(int argc, char **argv)
{
    if (argc < 9) {
        fprintf(stderr, "usage: rect <seed> <x> <y> <w> <h> <out.sraw> <train1> [...]\n");
        return 2;
    }
    uint64_t seed = strtoull(argv[2], NULL, 0);
    sculpt_rect_t r = {
        (int16_t)atoi(argv[3]), (int16_t)atoi(argv[4]),
        (int16_t)atoi(argv[5]), (int16_t)atoi(argv[6])
    };
    const char *out_path = argv[7];
    sculpt_library_t *lib = learn_all(&argv[8], argc - 8);
    if (!lib) return 3;

    sculpt_grid_t g;
    sculpt_grid_init(&g, SCULPT_GRID_SIZE);
    sculpt_draw_stats_t stats;
    int log_n = 0;
    sculpt_edit_rect(lib, seed, NULL, r, &g, NULL, 0, &log_n, &stats);

    if (write_grid(&g, out_path) != 0) { free(lib); return 4; }
    printf("[rect] seed=%llu rect=(%d,%d,%d,%d) decisions=L0:%d L1:%d L2:%d L3:%d -> %s\n",
           (unsigned long long)seed, r.x, r.y, r.w, r.h,
           stats.score_count[0], stats.score_count[1],
           stats.score_count[2], stats.score_count[3], out_path);
    free(lib);
    return 0;
}

static int cmd_replay(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: replay <seed> <log.slog> <out.sraw> <train1> [...]\n");
        return 2;
    }
    uint64_t seed = strtoull(argv[2], NULL, 0);
    const char *log_path = argv[3];
    const char *out_path = argv[4];
    sculpt_library_t *lib = learn_all(&argv[5], argc - 5);
    if (!lib) return 3;

    sculpt_edit_log_entry_t *log = (sculpt_edit_log_entry_t *)calloc(LOG_CAP, sizeof(*log));
    int log_n = 0;
    if (sculpt_log_load(log_path, log, LOG_CAP, &log_n) != 0) {
        fprintf(stderr, "load log failed\n");
        free(log); free(lib); return 4;
    }

    sculpt_grid_t g;
    sculpt_grid_init(&g, SCULPT_GRID_SIZE);
    if (sculpt_replay(&g, lib, seed, log, log_n) != 0) {
        fprintf(stderr, "replay: missing chisel_id in library\n");
        free(log); free(lib); return 5;
    }
    if (write_grid(&g, out_path) != 0) { free(log); free(lib); return 6; }
    printf("[replay] seed=%llu replayed %d entries -> %s\n",
           (unsigned long long)seed, log_n, out_path);
    free(log); free(lib);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s {draw|rect|replay} ...\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "draw") == 0)   return cmd_draw(argc, argv);
    if (strcmp(argv[1], "rect") == 0)   return cmd_rect(argc, argv);
    if (strcmp(argv[1], "replay") == 0) return cmd_replay(argc, argv);
    fprintf(stderr, "unknown subcommand: %s\n", argv[1]);
    return 2;
}
