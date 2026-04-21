#ifndef SCULPT_LIBRARY_H
#define SCULPT_LIBRARY_H

#include <stdint.h>
#include "sculpt_chisel.h"
#include "sculpt_grid.h"
#include "sculpt_tuning.h"

/* Chisel library. Phase 3 implementation: flat array + linear (level, packed-key)
 * search. Good enough for 16x16 and Phase 3 bring-up; Phase 4 can swap in a
 * hash table without touching callers.
 */

#define SCULPT_LIB_MAX_CHISELS (SCULPT_MAX_GRID_SIZE * SCULPT_MAX_GRID_SIZE * SCULPT_NUM_LEVELS * 4)

typedef struct {
    sculpt_chisel_t items[SCULPT_LIB_MAX_CHISELS];
    uint32_t count;
    uint32_t next_id;
} sculpt_library_t;

void sculpt_library_init(sculpt_library_t *lib);

/* Register a (level, key, subtract) tuple. Duplicates (same level+packed_key+
 * subtract vector) collapse into the existing entry with weight++.
 * Returns pointer to the chisel inside the library (stable until next register).
 */
sculpt_chisel_t *sculpt_library_register(sculpt_library_t *lib,
                                          int level,
                                          const sculpt_neighbor_key_t *key,
                                          uint8_t sub_r, uint8_t sub_g,
                                          uint8_t sub_b, uint8_t sub_a);

/* Fill out_candidates (size top_g) with up to top_g chisels for (level,key).
 * Prefer exact-key matches, then fall back to highest-weight at that level.
 * Returns number of candidates written.
 */
int sculpt_library_lookup(const sculpt_library_t *lib,
                           int level,
                           const sculpt_neighbor_key_t *key,
                           int top_g,
                           const sculpt_chisel_t *out_candidates[]);

/* Stats. */
uint32_t sculpt_library_size(const sculpt_library_t *lib);
uint32_t sculpt_library_size_at_level(const sculpt_library_t *lib, int level);

#endif /* SCULPT_LIBRARY_H */
