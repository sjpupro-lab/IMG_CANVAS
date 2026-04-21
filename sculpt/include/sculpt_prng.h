#ifndef SCULPT_PRNG_H
#define SCULPT_PRNG_H

#include <stdint.h>

/* SplitMix64 — deterministic PRNG (DESIGN.md §6.4, P4). */

typedef struct {
    uint64_t state;
} sculpt_prng_t;

void sculpt_prng_seed(sculpt_prng_t *p, uint64_t seed);
uint64_t sculpt_prng_next_u64(sculpt_prng_t *p);
int sculpt_prng_next_in_range(sculpt_prng_t *p, int lo_inclusive, int hi_inclusive);

uint64_t sculpt_derive_seed(uint64_t master, int level, int iter_idx, int cell_id);

#endif /* SCULPT_PRNG_H */
