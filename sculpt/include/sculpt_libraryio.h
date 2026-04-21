#ifndef SCULPT_LIBRARYIO_H
#define SCULPT_LIBRARYIO_H

#include <stdint.h>
#include "sculpt_library.h"

/* Chisel library binary container (Phase 6).
 *
 * File layout:
 *   16 bytes   header
 *       4 B    magic  "SLIB"
 *       4 B    uint32 version      (LE) == 1
 *       4 B    uint32 count        (LE)
 *       4 B    uint32 next_id      (LE)
 *   count * 32 bytes   entries; per-entry:
 *       4 B    uint32 chisel_id    (LE)
 *       4 B    self_r,g,b,a        (each uint8, nibble in low 4 bits)
 *       8 B    neighbors[0..7]     (each uint8, octal in low 3 bits)
 *       4 B    subtract_r,g,b,a    (uint8)
 *       1 B    int8   level
 *       3 B    reserved (must be 0)
 *       4 B    uint32 weight       (LE)
 *       4 B    uint32 usage_count  (LE)
 *
 * Endianness: every multi-byte integer is little-endian, explicit byte
 * packing, independent of host struct padding.
 */

#define SCULPT_LIB_MAGIC   "SLIB"
#define SCULPT_LIB_VERSION 1u

int sculpt_library_save(const char *path, const sculpt_library_t *lib);
int sculpt_library_load(const char *path, sculpt_library_t *out);

#endif /* SCULPT_LIBRARYIO_H */
