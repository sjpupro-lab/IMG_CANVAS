#ifndef SCULPT_LEARN_H
#define SCULPT_LEARN_H

#include "sculpt_library.h"
#include "sculpt_rawio.h"

/* Learning pipe (DESIGN.md §5). Loads a .sraw image, decomposes into
 * 4-channel depths, blurs per level, and registers chisels into `lib`.
 */

typedef struct {
    uint32_t per_level_entries[SCULPT_NUM_LEVELS];
} sculpt_learn_stats_t;

/* Returns 0 on success. Grid size is taken from SCULPT_GRID_SIZE. */
int sculpt_learn_image(const char *sraw_path,
                        sculpt_library_t *lib,
                        sculpt_learn_stats_t *out_stats);

#endif /* SCULPT_LEARN_H */
