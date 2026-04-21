#ifndef SPATIAL_CLOCK_H
#define SPATIAL_CLOCK_H

#include <stdint.h>

/*
 * RGBA Clockwork Engine
 *
 * Four independent 256×256 uint8 surfaces (R/G/B/A), all initialized
 * to 255. Inputs drain cells from a shared write head: each tick
 * decrements cells starting at `pos` by the input amount (one cell
 * at a time, saturating at 0 then advancing). Draining is performed
 * channel-by-channel sequentially in a single tick call, so the
 * write head walks R cells first, then G, then B, then A, advancing
 * past each channel's portion.
 *
 * Because inputs are 4 independent signals (b_diff / hz_diff /
 * hz_hist_sum / active_cell_count), two engines fed with similar
 * input histories land in near-identical states; SAD over each
 * channel tells the caller which specific signal diverged.
 *
 * Capacity per channel: 256*256 cells × 255 max value = 16.7M tick
 * units. Easily absorbs a full training session on wiki-sized data
 * before wrap-around.
 *
 * Memory: 4 × 65 536 B = 256 KB per engine.
 */

#define CLOCK_DIM    256u
#define CLOCK_CELLS  (CLOCK_DIM * CLOCK_DIM)   /* 65 536 */
#define CLOCK_INIT   255u

typedef struct {
    uint8_t  R[CLOCK_CELLS];
    uint8_t  G[CLOCK_CELLS];
    uint8_t  B[CLOCK_CELLS];
    uint8_t  A[CLOCK_CELLS];
    uint32_t pos;   /* shared write head; wraps at CLOCK_CELLS */
} RGBAClockEngine;

/* Per-channel SAD so callers can make independent decisions
 * ("context moved but chapter stable" vs "chapter shifted" vs
 * "totally different data type"). Summing is intentionally avoided. */
typedef struct {
    uint64_t R_sad;
    uint64_t G_sad;
    uint64_t B_sad;
    uint64_t A_sad;
} RGBAClockSad;

/* All cells to CLOCK_INIT, pos to 0. */
void rgba_clock_init(RGBAClockEngine* ce);

/* Tick all four channels from the shared write head.
 * r/g/b are the three small-range signals (typical range 0..60).
 * a_val is uint16 because active_cells × 256 scales into the
 * thousands — allowed to drain many cells in a single tick. */
void rgba_clock_tick(RGBAClockEngine* ce,
                     uint8_t  r_val,
                     uint8_t  g_val,
                     uint8_t  b_val,
                     uint16_t a_val);

/* Byte-wise per-channel SAD. Integer-only, SIMD-friendly (compiles
 * to VPSADBW on x86 at -O2). */
RGBAClockSad rgba_clock_sad(const RGBAClockEngine* a,
                            const RGBAClockEngine* b);

/* memcpy of the full engine. Used by chapter detection to snapshot
 * the engine at chapter start so subsequent SADs measure delta
 * since the chapter began. */
void rgba_clock_copy(RGBAClockEngine* dst, const RGBAClockEngine* src);

#endif /* SPATIAL_CLOCK_H */
