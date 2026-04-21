/* Small CLI helper shared by draw_sculpt and edit_sculpt (Phase 6).
 *
 * Accept a mixed list of input paths: `.slib` files are loaded as a chisel
 * library directly; anything else is treated as a .sraw training image and
 * learned into the library. This lets tools accept either a pre-trained
 * library or raw images without duplicated argument parsing.
 */

#ifndef CLI_COMMON_H
#define CLI_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sculpt_learn.h"
#include "sculpt_library.h"
#include "sculpt_libraryio.h"

static int cli_path_is_slib(const char *path)
{
    size_t n = strlen(path);
    return n >= 5 && strcmp(path + n - 5, ".slib") == 0;
}

/* Caller owns the returned library (free with free()). Returns NULL on error. */
static sculpt_library_t *cli_load_library(char **paths, int n_paths)
{
    if (n_paths <= 0) return NULL;
    sculpt_library_t *lib = (sculpt_library_t *)calloc(1, sizeof(*lib));
    if (!lib) return NULL;
    sculpt_library_init(lib);

    for (int i = 0; i < n_paths; ++i) {
        if (cli_path_is_slib(paths[i])) {
            /* .slib replaces the library wholesale; takes priority over
             * any already-learned state. Typical usage: a single .slib.
             */
            int rc = sculpt_library_load(paths[i], lib);
            if (rc != 0) {
                fprintf(stderr, "load failed for %s (rc=%d)\n", paths[i], rc);
                free(lib);
                return NULL;
            }
        } else {
            int rc = sculpt_learn_image(paths[i], lib, NULL);
            if (rc != 0) {
                fprintf(stderr, "learn failed for %s (rc=%d)\n", paths[i], rc);
                free(lib);
                return NULL;
            }
        }
    }
    return lib;
}

#endif /* CLI_COMMON_H */
