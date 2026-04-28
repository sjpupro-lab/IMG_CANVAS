/* tune_sculpt — Phase 8 tuning CLI.
 *
 * Identical to draw_sculpt in inputs/outputs (seed + library/train images
 * -> .sraw), but every entry in sculpt_tuning_t is exposed as a CLI flag.
 * Each per-level flag accepts a comma-separated 4-tuple "L0,L1,L2,L3".
 *
 *   --margin       1,4,8,16
 *   --blur-box     1,2,4,8
 *   --top-g        8,4,4,2
 *   --iters        1,2,2,2
 *   --coh-thresh   24,24,24,24
 *   --coh-bonus    2,2,2,2
 *   --coh-penalty  1,1,1,1
 *   --a-band       32
 *
 * Required:
 *   --seed <u64>
 *   --out  <out.sraw>
 *   <library.slib> | <train1.sraw> [train2.sraw ...]
 *
 * Output: stdout echoes the resolved tuning + draw stats; .sraw written.
 * The script driving the sweep parses these lines.
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

static int parse_quad(const char *arg, int out[SCULPT_NUM_LEVELS])
{
    int n = 0;
    const char *p = arg;
    while (*p && n < SCULPT_NUM_LEVELS) {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) return -1;
        out[n++] = (int)v;
        p = end;
        if (*p == ',') ++p;
    }
    return n == SCULPT_NUM_LEVELS ? 0 : -1;
}

static void print_quad(const char *name, const int q[SCULPT_NUM_LEVELS])
{
    printf("  %-15s %d,%d,%d,%d\n", name, q[0], q[1], q[2], q[3]);
}

int main(int argc, char **argv)
{
    sculpt_tuning_reset_default();

    uint64_t seed = 0;
    int seed_seen = 0;
    const char *out_path = NULL;

    /* Parse named args; remaining positional args are library/train inputs. */
    int positional_start = -1;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--seed") == 0 && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 0);
            seed_seen = 1;
        } else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(a, "--margin") == 0 && i + 1 < argc) {
            if (parse_quad(argv[++i], SCULPT_TUNING.level_margin) != 0) {
                fprintf(stderr, "bad --margin\n"); return 2;
            }
        } else if (strcmp(a, "--blur-box") == 0 && i + 1 < argc) {
            if (parse_quad(argv[++i], SCULPT_TUNING.level_blur_box) != 0) {
                fprintf(stderr, "bad --blur-box\n"); return 2;
            }
        } else if (strcmp(a, "--top-g") == 0 && i + 1 < argc) {
            if (parse_quad(argv[++i], SCULPT_TUNING.level_top_g) != 0) {
                fprintf(stderr, "bad --top-g\n"); return 2;
            }
        } else if (strcmp(a, "--iters") == 0 && i + 1 < argc) {
            if (parse_quad(argv[++i], SCULPT_TUNING.default_iters) != 0) {
                fprintf(stderr, "bad --iters\n"); return 2;
            }
        } else if (strcmp(a, "--coh-thresh") == 0 && i + 1 < argc) {
            if (parse_quad(argv[++i], SCULPT_TUNING.coherence_threshold) != 0) {
                fprintf(stderr, "bad --coh-thresh\n"); return 2;
            }
        } else if (strcmp(a, "--coh-bonus") == 0 && i + 1 < argc) {
            if (parse_quad(argv[++i], SCULPT_TUNING.coherence_bonus) != 0) {
                fprintf(stderr, "bad --coh-bonus\n"); return 2;
            }
        } else if (strcmp(a, "--coh-penalty") == 0 && i + 1 < argc) {
            if (parse_quad(argv[++i], SCULPT_TUNING.coherence_penalty) != 0) {
                fprintf(stderr, "bad --coh-penalty\n"); return 2;
            }
        } else if (strcmp(a, "--a-band") == 0 && i + 1 < argc) {
            SCULPT_TUNING.a_band_width = atoi(argv[++i]);
        } else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "unknown flag: %s\n", a);
            return 2;
        } else {
            positional_start = i;
            break;
        }
    }

    if (!seed_seen || !out_path || positional_start < 0) {
        fprintf(stderr,
            "usage: tune_sculpt --seed <N> --out <out.sraw> [tuning flags...] "
            "<library.slib | train*.sraw ...>\n");
        return 2;
    }

    sculpt_library_t *lib = cli_load_library(&argv[positional_start],
                                              argc - positional_start);
    if (!lib) return 3;

    /* Echo resolved tuning so the sweep script can parse / log. */
    printf("[tuning]\n");
    print_quad("margin",     SCULPT_TUNING.level_margin);
    print_quad("blur-box",   SCULPT_TUNING.level_blur_box);
    print_quad("top-g",      SCULPT_TUNING.level_top_g);
    print_quad("iters",      SCULPT_TUNING.default_iters);
    print_quad("coh-thresh", SCULPT_TUNING.coherence_threshold);
    print_quad("coh-bonus",  SCULPT_TUNING.coherence_bonus);
    print_quad("coh-penalty",SCULPT_TUNING.coherence_penalty);
    printf("  %-15s %d\n", "a-band", SCULPT_TUNING.a_band_width);

    sculpt_grid_t g;
    sculpt_grid_init(&g, SCULPT_GRID_SIZE);
    sculpt_draw_stats_t stats;
    int log_written = 0;
    sculpt_draw(lib, seed, NULL, &g, NULL, 0, &log_written, &stats);

    printf("[draw] seed=%llu lib_size=%u "
           "decisions=L0:%d L1:%d L2:%d L3:%d\n",
           (unsigned long long)seed, sculpt_library_size(lib),
           stats.score_count[0], stats.score_count[1],
           stats.score_count[2], stats.score_count[3]);

    uint8_t rgb[SCULPT_GRID_SIZE * SCULPT_GRID_SIZE * 3];
    sculpt_grid_to_rgb(&g, rgb);
    int rc = sculpt_image_save_raw(out_path, SCULPT_GRID_SIZE, SCULPT_GRID_SIZE, rgb);
    if (rc != 0) { fprintf(stderr, "save failed (rc=%d)\n", rc); free(lib); return 4; }
    printf("[write] %s\n", out_path);

    free(lib);
    return 0;
}
