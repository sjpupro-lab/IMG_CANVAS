#ifndef SCULPT_CHISEL_H
#define SCULPT_CHISEL_H

#include <stdint.h>
#include "sculpt_cell.h"

/* Chisel + NeighborStateKey (DESIGN.md §4.3, §4.4).
 * Self-only matching is forbidden (P3) — keys always include 8 neighbors.
 */

typedef struct {
    uint8_t self_r;   /* 4-bit quantized (0..15) */
    uint8_t self_g;
    uint8_t self_b;
    uint8_t self_a;
    uint8_t n[8];     /* 3-bit quantized (0..7), order: TL,T,TR,L,R,BL,B,BR */
} sculpt_neighbor_key_t;

typedef struct {
    uint32_t chisel_id;
    sculpt_neighbor_key_t pre_state;
    uint8_t subtract_r;
    uint8_t subtract_g;
    uint8_t subtract_b;
    uint8_t subtract_a;
    int8_t level;     /* 0..3 */
    uint32_t weight;
    uint32_t usage_count;
} sculpt_chisel_t;

void sculpt_neighbor_key_build(const sculpt_cell_t *self,
                                const sculpt_cell_t *neighbors[8],
                                sculpt_neighbor_key_t *out_key);

uint64_t sculpt_neighbor_key_pack(const sculpt_neighbor_key_t *k);

int sculpt_neighbor_key_equal(const sculpt_neighbor_key_t *a,
                               const sculpt_neighbor_key_t *b);

#endif /* SCULPT_CHISEL_H */
