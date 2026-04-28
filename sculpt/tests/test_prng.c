#include "sculpt_prng.h"
#include <stdio.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while (0)

int main(void)
{
    sculpt_prng_t a, b;
    sculpt_prng_seed(&a, 42);
    sculpt_prng_seed(&b, 42);
    for (int i = 0; i < 1000; ++i) {
        CHECK(sculpt_prng_next_u64(&a) == sculpt_prng_next_u64(&b));
    }

    /* Different seeds diverge. */
    sculpt_prng_seed(&a, 42);
    sculpt_prng_seed(&b, 1337);
    int diverged = 0;
    for (int i = 0; i < 10; ++i) {
        if (sculpt_prng_next_u64(&a) != sculpt_prng_next_u64(&b)) { diverged = 1; break; }
    }
    CHECK(diverged);

    /* Range. */
    sculpt_prng_seed(&a, 100);
    for (int i = 0; i < 1000; ++i) {
        int v = sculpt_prng_next_in_range(&a, -16, 16);
        CHECK(v >= -16 && v <= 16);
    }

    /* derive_seed is deterministic and level/iter/cell sensitive. */
    uint64_t s1 = sculpt_derive_seed(42, 3, 0, 10);
    uint64_t s2 = sculpt_derive_seed(42, 3, 0, 10);
    uint64_t s3 = sculpt_derive_seed(42, 3, 0, 11);
    uint64_t s4 = sculpt_derive_seed(42, 2, 0, 10);
    CHECK(s1 == s2);
    CHECK(s1 != s3);
    CHECK(s1 != s4);

    printf("test_prng: OK\n");
    return 0;
}
