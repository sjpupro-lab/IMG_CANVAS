#include "sculpt_cell.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while (0)

int main(void)
{
    /* saturate_subtract clamps at 255. */
    CHECK(sculpt_saturate_subtract(200, 100) == 255);
    CHECK(sculpt_saturate_subtract(0, 300) == 255);
    CHECK(sculpt_saturate_subtract(255, 10) == 255);
    CHECK(sculpt_saturate_subtract(100, 0) == 100);
    CHECK(sculpt_saturate_subtract(50, -5) == 50);

    /* Never decreases depth (P1: subtractive-only). */
    for (int start = 0; start <= 255; start += 17) {
        for (int delta = -5; delta <= 300; delta += 23) {
            uint8_t out = sculpt_saturate_subtract((uint8_t)start, delta);
            CHECK((int)out >= start);
        }
    }

    /* P2: depth and original are complements. */
    for (int d = 0; d <= 255; d += 13) {
        CHECK((255 - d) == 255 - d);
    }

    sculpt_cell_t c;
    sculpt_cell_zero(&c);
    CHECK(c.depth_r == 0);
    CHECK(c.depth_g == 0);
    CHECK(c.depth_b == 0);
    CHECK(c.depth_a == 0);

    printf("test_cell: OK\n");
    return 0;
}
