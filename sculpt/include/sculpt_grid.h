#ifndef SCULPT_GRID_H
#define SCULPT_GRID_H

#include <stdint.h>
#include "sculpt_cell.h"
#include "sculpt_tuning.h"

/* Grid — NxN lattice. Default size = SCULPT_GRID_SIZE. (DESIGN.md §4.2) */

#define SCULPT_MAX_GRID_SIZE 64

typedef struct {
    int size;
    sculpt_cell_t cells[SCULPT_MAX_GRID_SIZE * SCULPT_MAX_GRID_SIZE];
} sculpt_grid_t;

/* 8-neighbor direction offsets, fixed order: TL, T, TR, L, R, BL, B, BR. */
extern const int SCULPT_DIRS_8[8][2];

void sculpt_grid_init(sculpt_grid_t *g, int size);

/* Return pointer to cell at (x,y). */
sculpt_cell_t *sculpt_grid_at(sculpt_grid_t *g, int x, int y);
const sculpt_cell_t *sculpt_grid_at_const(const sculpt_grid_t *g, int x, int y);

/* Fill `out_neighbors[8]` with pointers to the 8 neighbors of (x,y).
 * Out-of-bounds positions point to a shared uncarved sentinel cell.
 * P3: caller MUST call this and pass the result into any decision.
 */
void sculpt_grid_neighbor_8(const sculpt_grid_t *g, int x, int y,
                             const sculpt_cell_t *out_neighbors[8]);

/* Reconstruct the RGB image = 255 - depth per channel.
 * out_rgb must point to size*size*3 bytes. (P2)
 */
void sculpt_grid_to_rgb(const sculpt_grid_t *g, uint8_t *out_rgb);

#endif /* SCULPT_GRID_H */
