#include "sculpt_libraryio.h"

#include <stdio.h>
#include <string.h>

static void put_u32_le(uint8_t b[4], uint32_t v)
{
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32_le(const uint8_t b[4])
{
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

int sculpt_library_save(const char *path, const sculpt_library_t *lib)
{
    if (!path || !lib) return 1;
    FILE *f = fopen(path, "wb");
    if (!f) return 2;

    uint8_t header[16];
    memcpy(header, SCULPT_LIB_MAGIC, 4);
    put_u32_le(header + 4, SCULPT_LIB_VERSION);
    put_u32_le(header + 8, lib->count);
    put_u32_le(header + 12, lib->next_id);
    if (fwrite(header, 1, 16, f) != 16) { fclose(f); return 3; }

    for (uint32_t i = 0; i < lib->count; ++i) {
        const sculpt_chisel_t *c = &lib->items[i];
        uint8_t buf[32];
        memset(buf, 0, sizeof(buf));

        put_u32_le(buf + 0, c->chisel_id);
        buf[4] = c->pre_state.self_r & 0xF;
        buf[5] = c->pre_state.self_g & 0xF;
        buf[6] = c->pre_state.self_b & 0xF;
        buf[7] = c->pre_state.self_a & 0xF;
        for (int k = 0; k < 8; ++k) buf[8 + k] = c->pre_state.n[k] & 0x7;
        buf[16] = c->subtract_r;
        buf[17] = c->subtract_g;
        buf[18] = c->subtract_b;
        buf[19] = c->subtract_a;
        buf[20] = (uint8_t)c->level;
        /* buf[21..23] reserved, already zeroed */
        put_u32_le(buf + 24, c->weight);
        put_u32_le(buf + 28, c->usage_count);

        if (fwrite(buf, 1, 32, f) != 32) { fclose(f); return 4; }
    }
    fclose(f);
    return 0;
}

int sculpt_library_load(const char *path, sculpt_library_t *out)
{
    if (!path || !out) return 1;
    FILE *f = fopen(path, "rb");
    if (!f) return 2;

    uint8_t header[16];
    if (fread(header, 1, 16, f) != 16) { fclose(f); return 3; }
    if (memcmp(header, SCULPT_LIB_MAGIC, 4) != 0) { fclose(f); return 4; }
    uint32_t version = get_u32_le(header + 4);
    if (version != SCULPT_LIB_VERSION) { fclose(f); return 5; }
    uint32_t count = get_u32_le(header + 8);
    uint32_t next_id = get_u32_le(header + 12);
    if (count > SCULPT_LIB_MAX_CHISELS) { fclose(f); return 6; }

    sculpt_library_init(out);
    out->count = count;
    out->next_id = next_id;

    for (uint32_t i = 0; i < count; ++i) {
        uint8_t buf[32];
        if (fread(buf, 1, 32, f) != 32) { fclose(f); return 7; }

        sculpt_chisel_t *c = &out->items[i];
        c->chisel_id = get_u32_le(buf + 0);
        c->pre_state.self_r = buf[4] & 0xF;
        c->pre_state.self_g = buf[5] & 0xF;
        c->pre_state.self_b = buf[6] & 0xF;
        c->pre_state.self_a = buf[7] & 0xF;
        for (int k = 0; k < 8; ++k) c->pre_state.n[k] = buf[8 + k] & 0x7;
        c->subtract_r = buf[16];
        c->subtract_g = buf[17];
        c->subtract_b = buf[18];
        c->subtract_a = buf[19];
        c->level = (int8_t)buf[20];
        c->weight = get_u32_le(buf + 24);
        c->usage_count = get_u32_le(buf + 28);
    }
    fclose(f);
    return 0;
}
