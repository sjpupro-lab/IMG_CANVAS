#include "sculpt_grid.h"
#include <string.h>

const int SCULPT_DIRS_8[8][2] = {
    {-1, -1}, {0, -1}, {1, -1},
    {-1,  0},          {1,  0},
    {-1,  1}, {0,  1}, {1,  1},
};

/* Shared sentinel: out-of-bounds neighbors look like uncarved cells. */
static const sculpt_cell_t SCULPT_EMPTY_NEIGHBOR = {0};

void sculpt_grid_init(sculpt_grid_t *g, int size)
{
    if (size > SCULPT_MAX_GRID_SIZE) size = SCULPT_MAX_GRID_SIZE;
    g->size = size;
    memset(g->cells, 0, sizeof(g->cells));
}

sculpt_cell_t *sculpt_grid_at(sculpt_grid_t *g, int x, int y)
{
    return &g->cells[y * g->size + x];
}

const sculpt_cell_t *sculpt_grid_at_const(const sculpt_grid_t *g, int x, int y)
{
    return &g->cells[y * g->size + x];
}

void sculpt_grid_neighbor_8(const sculpt_grid_t *g, int x, int y,
                             const sculpt_cell_t *out_neighbors[8])
{
    for (int i = 0; i < 8; ++i) {
        int nx = x + SCULPT_DIRS_8[i][0];
        int ny = y + SCULPT_DIRS_8[i][1];
        if (nx >= 0 && nx < g->size && ny >= 0 && ny < g->size) {
            out_neighbors[i] = &g->cells[ny * g->size + nx];
        } else {
            out_neighbors[i] = &SCULPT_EMPTY_NEIGHBOR;
        }
    }
}

void sculpt_grid_to_rgb(const sculpt_grid_t *g, uint8_t *out_rgb)
{
    int n = g->size * g->size;
    for (int i = 0; i < n; ++i) {
        out_rgb[i * 3 + 0] = 255 - g->cells[i].depth_r;
        out_rgb[i * 3 + 1] = 255 - g->cells[i].depth_g;
        out_rgb[i * 3 + 2] = 255 - g->cells[i].depth_b;
    }
}
