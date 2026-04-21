/* train_sculpt — Phase 3 training CLI.
 *
 * Usage: train_sculpt <image.sraw> [image2.sraw ...]
 *
 * Loads one or more .sraw images, learns chisels into a single library,
 * prints library statistics. Persistence of the library to disk is Phase 4+.
 */

#include "sculpt_learn.h"
#include "sculpt_library.h"
#include "sculpt_tuning.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <image.sraw> [image2.sraw ...]\n", argv[0]);
        return 2;
    }

    sculpt_library_t *lib = (sculpt_library_t *)calloc(1, sizeof(*lib));
    if (!lib) {
        fprintf(stderr, "error: out of memory allocating library\n");
        return 1;
    }
    sculpt_library_init(lib);

    for (int i = 1; i < argc; ++i) {
        sculpt_learn_stats_t stats;
        int rc = sculpt_learn_image(argv[i], lib, &stats);
        if (rc != 0) {
            fprintf(stderr, "learn failed for %s (rc=%d)\n", argv[i], rc);
            free(lib);
            return 3;
        }
        printf("[learn] %s: entries per level = {L0:%u L1:%u L2:%u L3:%u}\n",
               argv[i],
               stats.per_level_entries[0], stats.per_level_entries[1],
               stats.per_level_entries[2], stats.per_level_entries[3]);
    }

    printf("\n[library] total chisels: %u\n", sculpt_library_size(lib));
    for (int lv = 0; lv < SCULPT_NUM_LEVELS; ++lv) {
        printf("  L%d: %u unique\n", lv, sculpt_library_size_at_level(lib, lv));
    }

    free(lib);
    return 0;
}
