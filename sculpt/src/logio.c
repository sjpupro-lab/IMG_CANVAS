#include "sculpt_logio.h"

#include <stdio.h>
#include <string.h>

static void put_u16_le(uint8_t b[2], uint16_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32_le(uint8_t b[4], uint32_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t get_u16_le(const uint8_t b[2])
{
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

static uint32_t get_u32_le(const uint8_t b[4])
{
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

int sculpt_log_save(const char *path,
                     const sculpt_edit_log_entry_t *log, int count)
{
    if (!path || count < 0) return 1;
    FILE *f = fopen(path, "wb");
    if (!f) return 2;

    uint8_t header[8];
    memcpy(header, SCULPT_LOG_MAGIC, 4);
    put_u32_le(header + 4, (uint32_t)count);
    if (fwrite(header, 1, 8, f) != 8) { fclose(f); return 3; }

    for (int i = 0; i < count; ++i) {
        uint8_t buf[12];
        buf[0] = (uint8_t)log[i].level;
        buf[1] = (uint8_t)log[i].iter_idx;
        put_u16_le(buf + 2, (uint16_t)log[i].cell_id);
        put_u32_le(buf + 4, log[i].chisel_id);
        put_u32_le(buf + 8, (uint32_t)log[i].noise_xor);
        if (fwrite(buf, 1, 12, f) != 12) { fclose(f); return 4; }
    }
    fclose(f);
    return 0;
}

int sculpt_log_load(const char *path,
                     sculpt_edit_log_entry_t *log, int capacity, int *out_count)
{
    if (!path || !log || !out_count || capacity < 0) return 1;
    FILE *f = fopen(path, "rb");
    if (!f) return 2;

    uint8_t header[8];
    if (fread(header, 1, 8, f) != 8) { fclose(f); return 3; }
    if (memcmp(header, SCULPT_LOG_MAGIC, 4) != 0) { fclose(f); return 4; }

    uint32_t count = get_u32_le(header + 4);
    if ((int)count > capacity) { fclose(f); return 5; }

    for (uint32_t i = 0; i < count; ++i) {
        uint8_t buf[12];
        if (fread(buf, 1, 12, f) != 12) { fclose(f); return 6; }
        log[i].level = (int8_t)buf[0];
        log[i].iter_idx = (int8_t)buf[1];
        log[i].cell_id = (int16_t)get_u16_le(buf + 2);
        log[i].chisel_id = get_u32_le(buf + 4);
        log[i].noise_xor = (int32_t)get_u32_le(buf + 8);
    }
    fclose(f);
    *out_count = (int)count;
    return 0;
}
