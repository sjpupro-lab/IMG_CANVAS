#include "sculpt_chisel.h"
#include <assert.h>
#include <string.h>

static uint8_t quantize_self_channel(uint8_t depth)
{
    return (depth >> 4) & 0xF;  /* 4 bits = 16 buckets */
}

static uint8_t quantize_neighbor(const sculpt_cell_t *c)
{
    int avg = ((int)c->depth_r + c->depth_g + c->depth_b + c->depth_a) / 4;
    return (uint8_t)((avg >> 5) & 0x7);  /* 3 bits = 8 buckets */
}

void sculpt_neighbor_key_build(const sculpt_cell_t *self,
                                const sculpt_cell_t *neighbors[8],
                                sculpt_neighbor_key_t *out_key)
{
    /* P3: caller must supply non-null neighbors[0..7]. Assert to catch misuse. */
    assert(self != NULL);
    assert(neighbors != NULL);
    for (int i = 0; i < 8; ++i) {
        assert(neighbors[i] != NULL);
    }

    out_key->self_r = quantize_self_channel(self->depth_r);
    out_key->self_g = quantize_self_channel(self->depth_g);
    out_key->self_b = quantize_self_channel(self->depth_b);
    out_key->self_a = quantize_self_channel(self->depth_a);
    for (int i = 0; i < 8; ++i) {
        out_key->n[i] = quantize_neighbor(neighbors[i]);
    }
}

uint64_t sculpt_neighbor_key_pack(const sculpt_neighbor_key_t *k)
{
    uint64_t v = 0;
    v = (v << 4) | (k->self_r & 0xF);
    v = (v << 4) | (k->self_g & 0xF);
    v = (v << 4) | (k->self_b & 0xF);
    v = (v << 4) | (k->self_a & 0xF);
    for (int i = 0; i < 8; ++i) {
        v = (v << 3) | (k->n[i] & 0x7);
    }
    return v;  /* 4*4 + 8*3 = 40 bits */
}

int sculpt_neighbor_key_equal(const sculpt_neighbor_key_t *a,
                               const sculpt_neighbor_key_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}
