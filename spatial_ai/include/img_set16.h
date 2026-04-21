#ifndef IMG_SET16_H
#define IMG_SET16_H

#include "img_ce.h"

/*
 * img_set16 — SPEC §6 SIMD unit.
 *
 *   Set16 = 4 × 4 = 16 cells, divided into four 2×2 Quads:
 *
 *      col 0   col 1   col 2   col 3
 *     +-------+-------+-------+-------+
 *  r0 |   0   |   1   |   2   |   3   |
 *     +-------+-------+-------+-------+
 *  r1 |   4   |   5   |   6   |   7   |
 *     +-------+-------+-------+-------+
 *  r2 |   8   |   9   |  10   |  11   |
 *     +-------+-------+-------+-------+
 *  r3 |  12   |  13   |  14   |  15   |
 *     +-------+-------+-------+-------+
 *
 *      Q0 = top-left  cells {0,1,4,5}     role: PLUS       (additive)
 *      Q1 = top-right cells {2,3,6,7}     role: MINUS      (subtractive)
 *      Q2 = bot-left  cells {8,9,12,13}   role: SCALE      (multiplicative)
 *      Q3 = bot-right cells {10,11,14,15} role: PRECISION  (slow / refinement)
 *
 * Layout: per-channel arrays of 16 bytes (SoA). Each channel array
 * lands exactly on a 128-bit SIMD register (SSE2 / NEON), and a pair
 * fits AVX2's 256-bit lane. A quad's 4 cells are scattered (not
 * contiguous in the flat array), so quad-wide ops use a 4-index
 * gather defined in g_quad_indices below — typically resolved at
 * compile time when the quad role is a constant.
 */

#define IMG_SET16_DIM       4
#define IMG_SET16_CELLS     16
#define IMG_QUAD_DIM        2
#define IMG_QUAD_CELLS      4
#define IMG_QUAD_COUNT      4

typedef enum {
    IMG_QUAD_PLUS      = 0,   /* Q0 — additive contribution     */
    IMG_QUAD_MINUS     = 1,   /* Q1 — subtractive               */
    IMG_QUAD_SCALE     = 2,   /* Q2 — multiplicative / scale    */
    IMG_QUAD_PRECISION = 3    /* Q3 — slow / precision channel  */
} ImgQuadRole;

/* SoA layout. Channel arrays mirror ImgCECell fields. */
typedef struct {
    uint8_t core      [IMG_SET16_CELLS];
    uint8_t link      [IMG_SET16_CELLS];
    uint8_t delta     [IMG_SET16_CELLS];
    uint8_t priority  [IMG_SET16_CELLS];

    uint8_t tone      [IMG_SET16_CELLS];
    uint8_t role      [IMG_SET16_CELLS];
    uint8_t direction [IMG_SET16_CELLS];
    uint8_t depth     [IMG_SET16_CELLS];
    uint8_t delta_sign[IMG_SET16_CELLS];
} ImgSet16;

/* Flat-index helper: (row, col) within a Set16 → 0..15. */
static inline uint8_t img_set16_idx(uint8_t row, uint8_t col) {
    return (uint8_t)(row * IMG_SET16_DIM + col);
}

/* Returns a pointer to a static const uint8_t[IMG_QUAD_CELLS] giving
 * the four flat indices that belong to `quad`. */
const uint8_t* img_set16_quad_indices(ImgQuadRole quad);

/* Returns the quad role for a given (row, col) within the Set16. */
ImgQuadRole    img_set16_quad_for(uint8_t row, uint8_t col);

/* Pack a 4×4 sub-region of a CE grid (origin at ce_x0/ce_y0) into
 * an ImgSet16. If the region runs off the grid edge, missing cells
 * are zeroed (safe near borders).
 *
 * Phase C body is scalar; the SoA shape is what enables SSE2/NEON
 * loads later without re-architecting callers. */
void img_set16_load_from_ce(const ImgCEGrid* ce,
                            uint32_t ce_x0, uint32_t ce_y0,
                            ImgSet16* out);

/* Store a Set16 back to a 4×4 sub-region of a CE grid. Off-grid
 * cells are silently skipped. last_delta_id is preserved (Set16 does
 * not carry it — store-back leaves the existing field untouched). */
void img_set16_store_to_ce(const ImgSet16* set,
                           uint32_t ce_x0, uint32_t ce_y0,
                           ImgCEGrid* ce);

#endif /* IMG_SET16_H */
