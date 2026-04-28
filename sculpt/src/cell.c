#include "sculpt_cell.h"
#include <string.h>

uint8_t sculpt_saturate_subtract(uint8_t current, int delta)
{
    if (delta <= 0) {
        return current;
    }
    int remaining_budget = 255 - (int)current;
    int actual = delta < remaining_budget ? delta : remaining_budget;
    return (uint8_t)(current + actual);
}

void sculpt_cell_zero(sculpt_cell_t *c)
{
    memset(c, 0, sizeof(*c));
}
