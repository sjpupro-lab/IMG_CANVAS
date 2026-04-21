#include "sculpt_prng.h"

void sculpt_prng_seed(sculpt_prng_t *p, uint64_t seed)
{
    p->state = seed;
}

uint64_t sculpt_prng_next_u64(sculpt_prng_t *p)
{
    p->state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = p->state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

int sculpt_prng_next_in_range(sculpt_prng_t *p, int lo_inclusive, int hi_inclusive)
{
    int span = hi_inclusive - lo_inclusive + 1;
    if (span <= 0) return lo_inclusive;
    uint64_t r = sculpt_prng_next_u64(p);
    return lo_inclusive + (int)(r % (uint64_t)span);
}

uint64_t sculpt_derive_seed(uint64_t master, int level, int iter_idx, int cell_id)
{
    return master
         ^ ((uint64_t)level << 48)
         ^ ((uint64_t)iter_idx << 32)
         ^ (uint64_t)cell_id;
}
