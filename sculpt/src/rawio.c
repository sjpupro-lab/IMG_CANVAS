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

static void write_u32_le(uint8_t b[4], uint32_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
}

int sculpt_image_save_raw(const char *path, int width, int height, const uint8_t *rgb)
{
    if (width <= 0 || height <= 0 || !rgb || !path) return 1;
    FILE *f = fopen(path, "wb");
    if (!f) return 2;

    uint8_t header[16];
    memcpy(header, SCULPT_RAW_MAGIC, 4);
    write_u32_le(header + 4, (uint32_t)width);
    write_u32_le(header + 8, (uint32_t)height);
    write_u32_le(header + 12, 3u);

    if (fwrite(header, 1, 16, f) != 16) { fclose(f); return 3; }
    size_t bytes = (size_t)width * (size_t)height * 3;
    if (fwrite(rgb, 1, bytes, f) != bytes) { fclose(f); return 4; }
    fclose(f);
    return 0;
}
