#ifndef SCULPT_CELL_H
#define SCULPT_CELL_H

#include <stdint.h>

/* Cell — one 4-channel carved unit (DESIGN.md §4.1).
 *
 * depth = amount carved from max-value 255 per channel.
 * original value on that channel = 255 - depth.
 * Invariant: 0 <= depth <= 255.
 */
typedef struct {
    uint8_t depth_r;
    uint8_t depth_g;
    uint8_t depth_b;
    uint8_t depth_a;

    uint8_t margin_l0;
    uint8_t margin_l1;
    uint8_t margin_l2;
    uint8_t margin_l3;

    uint32_t last_chisel_id;
    uint16_t region_id;
} sculpt_cell_t;

/* P1 enforcement: the ONLY carving API. All depth mutations must go through
 * this function. Returns the new depth value, clamped at 255.
 */
uint8_t sculpt_saturate_subtract(uint8_t current, int delta);

/* Reset a cell to the uncarved max-value state. */
void sculpt_cell_zero(sculpt_cell_t *c);

#endif /* SCULPT_CELL_H */
