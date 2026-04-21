#include "sculpt_rawio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_u32_le(const uint8_t b[4])
{
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

int sculpt_image_load_raw(const char *path, sculpt_image_t *out)
{
    out->rgb = NULL;
    out->width = out->height = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 1;

    uint8_t header[16];
    if (fread(header, 1, 16, f) != 16) { fclose(f); return 2; }
    if (memcmp(header, SCULPT_RAW_MAGIC, 4) != 0) { fclose(f); return 3; }

    uint32_t w = read_u32_le(header + 4);
    uint32_t h = read_u32_le(header + 8);
    uint32_t c = read_u32_le(header + 12);
    if (c != 3 || w == 0 || h == 0 || w > 4096 || h > 4096) {
        fclose(f); return 4;
    }

    size_t bytes = (size_t)w * (size_t)h * 3;
    uint8_t *buf = (uint8_t *)malloc(bytes);
    if (!buf) { fclose(f); return 5; }
    if (fread(buf, 1, bytes, f) != bytes) { free(buf); fclose(f); return 6; }
    fclose(f);

    out->width = (int)w;
    out->height = (int)h;
    out->rgb = buf;
    return 0;
}

void sculpt_image_free(sculpt_image_t *img)
{
    if (img && img->rgb) {
        free(img->rgb);
        img->rgb = NULL;
    }
}
