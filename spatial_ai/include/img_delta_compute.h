#ifndef IMG_DELTA_COMPUTE_H
#define IMG_DELTA_COMPUTE_H

#include "img_delta_memory.h"

#include <stddef.h>
#include <stdint.h>

/*
 * img_delta_compute — the pure-function side of Delta Memory.
 *
 *   Splits the work of "what should this DeltaState produce?" away
 *   from the storage/lookup side that lives in img_delta_memory.c.
 *
 *   The runtime never calls img_delta_compute_entry directly. The
 *   pre-baked tables (src/img_delta_tables_data.c) and the offline
 *   generator (tools/gen_delta_tables.c) both use it. Keeping the
 *   compute side dependency-free lets the generator link against it
 *   without dragging in the rest of the engine.
 */

/* Cell-context bucket counts that the lookup tables are keyed on. */
#define IMG_TONE_BUCKETS   3
#define IMG_DEPTH_BUCKETS  3
#define IMG_FLOW_BUCKETS   5     /* FLOW_NONE..FLOW_DIAGONAL_DOWN */

/* Total number of entries per SoA channel array.
 *   8 × 4 × 8 × 4 × 3 × 3 = 9216 */
#define IMG_DELTA_TABLE_N  (IMG_MODE_MAX * IMG_TIER_MAX * IMG_SCALE_MAX * \
                            IMG_SIGN_MAX * IMG_TONE_BUCKETS *            \
                            IMG_DEPTH_BUCKETS)

/* Flat row-major index into the SoA tables.
 * Order (LSB → MSB): mode, tier, scale, sign, tone, depth. */
size_t  img_delta_table_idx(uint8_t mode, uint8_t tier,
                            uint8_t scale, uint8_t sign,
                            uint8_t tone, uint8_t depth);

/* Compute one (mode, tier, scale, sign, tone, depth) entry. Pure
 * function — no globals, no I/O, no allocations. */
void    img_delta_compute_entry(uint8_t mode, uint8_t tier,
                                uint8_t scale, uint8_t sign,
                                uint8_t tone, uint8_t depth,
                                int16_t* out_core,
                                int16_t* out_link,
                                int16_t* out_delta,
                                int16_t* out_priority,
                                uint8_t* out_pattern);

/* Direction / depth ±1 clamped step lookups. Pure. */
uint8_t img_delta_compute_direction_step(uint8_t cur_dir, uint8_t sign);
uint8_t img_delta_compute_depth_step    (uint8_t cur_dep, uint8_t sign);

#endif /* IMG_DELTA_COMPUTE_H */
