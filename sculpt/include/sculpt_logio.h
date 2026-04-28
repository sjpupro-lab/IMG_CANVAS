#ifndef SCULPT_LOGIO_H
#define SCULPT_LOGIO_H

#include <stdint.h>
#include "sculpt_draw.h"

/* Edit-log binary container (Phase 5).
 *
 * File layout:
 *   4 bytes   magic "SLOG"
 *   4 bytes   uint32 count (LE)
 *   count * 12 bytes, each:
 *     int8    level
 *     int8    iter_idx
 *     int16   cell_id  (LE)
 *     uint32  chisel_id (LE)
 *     int32   noise_xor (LE)
 */

#define SCULPT_LOG_MAGIC "SLOG"

int sculpt_log_save(const char *path,
                     const sculpt_edit_log_entry_t *log, int count);

/* On success, *out_count holds the number of entries read (<= capacity). */
int sculpt_log_load(const char *path,
                     sculpt_edit_log_entry_t *log, int capacity, int *out_count);

#endif /* SCULPT_LOGIO_H */
