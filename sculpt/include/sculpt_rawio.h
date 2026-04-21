#ifndef SCULPT_RAWIO_H
#define SCULPT_RAWIO_H

#include <stdint.h>

/* Minimal image I/O — no external libs (DESIGN.md §10). Uses a tiny custom
 * header for 8-bit RGB images. A companion Python helper converts PNG to
 * this format.
 *
 * File layout:
 *   4 bytes   magic "SRAW"
 *   4 bytes   uint32 width (little-endian)
 *   4 bytes   uint32 height (little-endian)
 *   4 bytes   uint32 channels (must be 3)
 *   W*H*3     RGB bytes, row-major top-down
 */

#define SCULPT_RAW_MAGIC "SRAW"

typedef struct {
    int width;
    int height;
    uint8_t *rgb;   /* owned heap buffer, W*H*3 bytes */
} sculpt_image_t;

/* Returns 0 on success, non-zero on failure. Caller must sculpt_image_free. */
int sculpt_image_load_raw(const char *path, sculpt_image_t *out);

void sculpt_image_free(sculpt_image_t *img);

/* Write an 8-bit RGB image to a .sraw file. rgb points to width*height*3 bytes.
 * Returns 0 on success, non-zero on I/O error.
 */
int sculpt_image_save_raw(const char *path, int width, int height, const uint8_t *rgb);

#endif /* SCULPT_RAWIO_H */
