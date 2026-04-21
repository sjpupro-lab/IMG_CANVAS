#include "sculpt_learn.h"
#include "sculpt_cell.h"
#include "sculpt_chisel.h"
#include "sculpt_grid.h"
#include "sculpt_tuning.h"

#include <stdlib.h>
#include <string.h>

#define N SCULPT_GRID_SIZE
#define CHANNELS 4

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int abs_i(int v) { return v < 0 ? -v : v; }

/* Nearest-neighbor resize from (src_w, src_h, 3ch) to (N, N, 3ch). */
static void resize_to_grid(const uint8_t *src, int src_w, int src_h,
                            uint8_t *dst_nxnx3)
{
    for (int y = 0; y < N; ++y) {
        int sy = (y * src_h) / N;
        for (int x = 0; x < N; ++x) {
            int sx = (x * src_w) / N;
            const uint8_t *p = &src[(sy * src_w + sx) * 3];
            uint8_t *q = &dst_nxnx3[(y * N + x) * 3];
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
        }
    }
}

/* 4-channel interpretation mirroring prototype/sculpt/learn.py.
 * C0 = luminance, C1 = |dx| (horizontal gradient), C2 = |dy|,
 * C3 = local 3x3 mean-abs-deviation. Each scaled to [0,255].
 */
static void interpret_4channels(const uint8_t rgb[N * N * 3], uint8_t ch[N * N * CHANNELS])
{
    uint8_t lum[N * N];
    for (int i = 0; i < N * N; ++i) {
        int r = rgb[i * 3 + 0], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        lum[i] = (uint8_t)clamp_i((r * 77 + g * 150 + b * 29) >> 8, 0, 255);
    }

    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            int i = y * N + x;
            int dx = (x > 0) ? abs_i((int)lum[i] - lum[i - 1]) : 0;
            int dy = (y > 0) ? abs_i((int)lum[i] - lum[i - N]) : 0;

            int acc = 0;
            for (int dyi = -1; dyi <= 1; ++dyi) {
                for (int dxi = -1; dxi <= 1; ++dxi) {
                    int nx = clamp_i(x + dxi, 0, N - 1);
                    int ny = clamp_i(y + dyi, 0, N - 1);
                    acc += abs_i((int)lum[ny * N + nx] - lum[i]);
                }
            }
            int var = acc / 8;

            ch[i * CHANNELS + 0] = lum[i];
            ch[i * CHANNELS + 1] = (uint8_t)clamp_i(dx * 2, 0, 255);
            ch[i * CHANNELS + 2] = (uint8_t)clamp_i(dy * 2, 0, 255);
            ch[i * CHANNELS + 3] = (uint8_t)clamp_i(var * 2, 0, 255);
        }
    }
}

/* Block-box blur: output pixel = mean of its box*box block (matches Python _box_blur). */
static void box_blur_channel(const uint8_t *in_plane, uint8_t *out_plane, int box)
{
    if (box <= 1) {
        memcpy(out_plane, in_plane, N * N);
        return;
    }
    for (int y = 0; y < N; ++y) {
        int y0 = (y / box) * box;
        int y1 = y0 + box; if (y1 > N) y1 = N;
        for (int x = 0; x < N; ++x) {
            int x0 = (x / box) * box;
            int x1 = x0 + box; if (x1 > N) x1 = N;
            int acc = 0, cnt = 0;
            for (int yy = y0; yy < y1; ++yy) {
                for (int xx = x0; xx < x1; ++xx) {
                    acc += in_plane[yy * N + xx];
                    ++cnt;
                }
            }
            out_plane[y * N + x] = (uint8_t)(acc / cnt);
        }
    }
}

static void blur_at_level(const uint8_t depth[N * N * CHANNELS],
                           int level,
                           uint8_t out[N * N * CHANNELS])
{
    int box = SCULPT_LEVEL_BLUR_BOX[level];
    uint8_t plane_in[N * N], plane_out[N * N];
    for (int c = 0; c < CHANNELS; ++c) {
        for (int i = 0; i < N * N; ++i) plane_in[i] = depth[i * CHANNELS + c];
        box_blur_channel(plane_in, plane_out, box);
        for (int i = 0; i < N * N; ++i) out[i * CHANNELS + c] = plane_out[i];
    }
}

int sculpt_learn_image(const char *sraw_path,
                        sculpt_library_t *lib,
                        sculpt_learn_stats_t *out_stats)
{
    sculpt_image_t img;
    int rc = sculpt_image_load_raw(sraw_path, &img);
    if (rc != 0) return rc;

    uint8_t rgb16[N * N * 3];
    resize_to_grid(img.rgb, img.width, img.height, rgb16);
    sculpt_image_free(&img);

    uint8_t ch[N * N * CHANNELS];
    interpret_4channels(rgb16, ch);

    /* P2: depth = 255 - original. */
    uint8_t depth[N * N * CHANNELS];
    for (int i = 0; i < N * N * CHANNELS; ++i) depth[i] = (uint8_t)(255 - ch[i]);

    /* Precompute per-level blurred depth. */
    uint8_t depth_l[SCULPT_NUM_LEVELS][N * N * CHANNELS];
    for (int lv = 0; lv < SCULPT_NUM_LEVELS; ++lv) {
        blur_at_level(depth, lv, depth_l[lv]);
    }

    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
    }

    /* Process levels high -> low, registering chisels against the running grid. */
    sculpt_grid_t running;
    sculpt_grid_init(&running, N);
    const int order[SCULPT_NUM_LEVELS] = { 3, 2, 1, 0 };

    for (int oi = 0; oi < SCULPT_NUM_LEVELS; ++oi) {
        int level = order[oi];
        /* contribution = depth_l[level] - (depth_l[next_higher] or 0) */
        uint8_t contrib[N * N * CHANNELS];
        if (level == 3) {
            memcpy(contrib, depth_l[3], sizeof(contrib));
        } else {
            for (int i = 0; i < N * N * CHANNELS; ++i) {
                int d = (int)depth_l[level][i] - (int)depth_l[level + 1][i];
                contrib[i] = (uint8_t)clamp_i(d, 0, 255);
            }
        }

        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                sculpt_cell_t *cell = sculpt_grid_at(&running, x, y);
                const sculpt_cell_t *neighbors[8];
                sculpt_grid_neighbor_8(&running, x, y, neighbors);

                sculpt_neighbor_key_t key;
                sculpt_neighbor_key_build(cell, neighbors, &key);

                int i = y * N + x;
                uint8_t sr = contrib[i * CHANNELS + 0];
                uint8_t sg = contrib[i * CHANNELS + 1];
                uint8_t sb = contrib[i * CHANNELS + 2];
                uint8_t sa = contrib[i * CHANNELS + 3];

                if (sr || sg || sb || sa) {
                    sculpt_library_register(lib, level, &key, sr, sg, sb, sa);
                    if (out_stats) out_stats->per_level_entries[level] += 1;
                }

                cell->depth_r = sculpt_saturate_subtract(cell->depth_r, sr);
                cell->depth_g = sculpt_saturate_subtract(cell->depth_g, sg);
                cell->depth_b = sculpt_saturate_subtract(cell->depth_b, sb);
                cell->depth_a = sculpt_saturate_subtract(cell->depth_a, sa);
            }
        }
    }

    return 0;
}
