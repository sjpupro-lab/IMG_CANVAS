#include "sculpt_library.h"
#include <string.h>

void sculpt_library_init(sculpt_library_t *lib)
{
    memset(lib, 0, sizeof(*lib));
    lib->next_id = 1;
}

sculpt_chisel_t *sculpt_library_register(sculpt_library_t *lib,
                                          int level,
                                          const sculpt_neighbor_key_t *key,
                                          uint8_t sub_r, uint8_t sub_g,
                                          uint8_t sub_b, uint8_t sub_a)
{
    /* Linear dedup: same (level, packed key, subtract vector) -> weight++. */
    uint64_t packed = sculpt_neighbor_key_pack(key);
    for (uint32_t i = 0; i < lib->count; ++i) {
        sculpt_chisel_t *c = &lib->items[i];
        if (c->level == level
            && c->subtract_r == sub_r && c->subtract_g == sub_g
            && c->subtract_b == sub_b && c->subtract_a == sub_a
            && sculpt_neighbor_key_pack(&c->pre_state) == packed) {
            c->weight += 1;
            return c;
        }
    }

    if (lib->count >= SCULPT_LIB_MAX_CHISELS) {
        return NULL;  /* caller should treat as overflow */
    }

    sculpt_chisel_t *c = &lib->items[lib->count++];
    c->chisel_id = lib->next_id++;
    c->pre_state = *key;
    c->subtract_r = sub_r;
    c->subtract_g = sub_g;
    c->subtract_b = sub_b;
    c->subtract_a = sub_a;
    c->level = (int8_t)level;
    c->weight = 1;
    c->usage_count = 0;
    return c;
}

int sculpt_library_lookup(const sculpt_library_t *lib,
                           int level,
                           const sculpt_neighbor_key_t *key,
                           int top_g,
                           const sculpt_chisel_t *out_candidates[])
{
    uint64_t packed = sculpt_neighbor_key_pack(key);

    /* Pass 1: exact key matches at this level. */
    int found = 0;
    for (uint32_t i = 0; i < lib->count && found < top_g; ++i) {
        const sculpt_chisel_t *c = &lib->items[i];
        if (c->level != level) continue;
        if (sculpt_neighbor_key_pack(&c->pre_state) != packed) continue;
        /* Insert by descending weight. */
        int pos = found;
        while (pos > 0 && out_candidates[pos - 1]->weight < c->weight) {
            out_candidates[pos] = out_candidates[pos - 1];
            --pos;
        }
        out_candidates[pos] = c;
        ++found;
    }
    if (found > 0) return found;

    /* Fallback: highest-weight at this level, regardless of key. */
    for (uint32_t i = 0; i < lib->count; ++i) {
        const sculpt_chisel_t *c = &lib->items[i];
        if (c->level != level) continue;
        int pos = found;
        /* Bounded insertion into a top_g array. */
        if (found < top_g) {
            while (pos > 0 && out_candidates[pos - 1]->weight < c->weight) {
                out_candidates[pos] = out_candidates[pos - 1];
                --pos;
            }
            out_candidates[pos] = c;
            ++found;
        } else if (out_candidates[top_g - 1]->weight < c->weight) {
            pos = top_g - 1;
            while (pos > 0 && out_candidates[pos - 1]->weight < c->weight) {
                out_candidates[pos] = out_candidates[pos - 1];
                --pos;
            }
            out_candidates[pos] = c;
        }
    }
    return found;
}

uint32_t sculpt_library_size(const sculpt_library_t *lib)
{
    return lib->count;
}

uint32_t sculpt_library_size_at_level(const sculpt_library_t *lib, int level)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < lib->count; ++i) {
        if (lib->items[i].level == level) ++n;
    }
    return n;
}

const sculpt_chisel_t *sculpt_library_get_by_id(const sculpt_library_t *lib,
                                                  uint32_t chisel_id)
{
    for (uint32_t i = 0; i < lib->count; ++i) {
        if (lib->items[i].chisel_id == chisel_id) return &lib->items[i];
    }
    return NULL;
}
