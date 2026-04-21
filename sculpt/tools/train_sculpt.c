/* train_sculpt — learn chisels from images, optionally persist to .slib.
 *
 * Usage:
 *   train_sculpt [-o out.slib] <image1.sraw> [image2.sraw ...]
 *
 * Without -o, prints stats and discards. With -o, also writes the library
 * to a binary .slib file that draw_sculpt / edit_sculpt can load directly.
 */

#include "sculpt_learn.h"
#include "sculpt_library.h"
#include "sculpt_libraryio.h"
#include "sculpt_tuning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *out_path = NULL;
    int arg_i = 1;
    if (argc >= 3 && strcmp(argv[1], "-o") == 0) {
        out_path = argv[2];
        arg_i = 3;
    }
    if (arg_i >= argc) {
        fprintf(stderr, "usage: %s [-o out.slib] <image.sraw> [image2.sraw ...]\n", argv[0]);
        return 2;
    }

    sculpt_library_t *lib = (sculpt_library_t *)calloc(1, sizeof(*lib));
    if (!lib) { fprintf(stderr, "oom\n"); return 1; }
    sculpt_library_init(lib);

    for (int i = arg_i; i < argc; ++i) {
        sculpt_learn_stats_t stats;
        int rc = sculpt_learn_image(argv[i], lib, &stats);
        if (rc != 0) {
            fprintf(stderr, "learn failed for %s (rc=%d)\n", argv[i], rc);
            free(lib);
            return 3;
        }
        printf("[learn] %s: per-level = {L0:%u L1:%u L2:%u L3:%u}\n",
               argv[i],
               stats.per_level_entries[0], stats.per_level_entries[1],
               stats.per_level_entries[2], stats.per_level_entries[3]);
    }

    printf("\n[library] total chisels: %u\n", sculpt_library_size(lib));
    for (int lv = 0; lv < SCULPT_NUM_LEVELS; ++lv) {
        printf("  L%d: %u unique\n", lv, sculpt_library_size_at_level(lib, lv));
    }

    if (out_path) {
        int rc = sculpt_library_save(out_path, lib);
        if (rc != 0) {
            fprintf(stderr, "save failed (rc=%d)\n", rc);
            free(lib);
            return 4;
        }
        printf("[save] %s\n", out_path);
    }

    free(lib);
    return 0;
}
