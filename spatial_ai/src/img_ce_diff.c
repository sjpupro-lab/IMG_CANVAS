#include "img_ce_diff.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── lifecycle ──────────────────────────────────────────── */

void img_ce_diff_destroy(ImgCEDiff* diff) {
    if (!diff) return;
    if (diff->entries) free(diff->entries);
    diff->entries  = NULL;
    diff->count    = 0;
    diff->capacity = 0;
}

static int ensure_capacity(ImgCEDiff* d, uint32_t needed) {
    if (needed <= d->capacity) return 1;
    uint32_t cap = d->capacity ? d->capacity : 32;
    while (cap < needed) cap *= 2;
    ImgCEDiffEntry* p = (ImgCEDiffEntry*)realloc(d->entries,
                                                 cap * sizeof(ImgCEDiffEntry));
    if (!p) return 0;
    d->entries  = p;
    d->capacity = cap;
    return 1;
}

/* ── compute ────────────────────────────────────────────── */

uint32_t img_ce_diff_compute(const ImgCEGrid* base,
                             const ImgCEGrid* target,
                             ImgCEDiff* out) {
    if (!base || !target || !out) return 0;
    if (!base->cells || !target->cells) return 0;
    if (base->width  != target->width)  return 0;
    if (base->height != target->height) return 0;

    /* Wipe any prior contents. */
    img_ce_diff_destroy(out);

    const uint32_t n = base->width * base->height;

    for (uint32_t i = 0; i < n; i++) {
        const ImgCECell* b = &base->cells[i];
        const ImgCECell* t = &target->cells[i];

        const int d_core  = (int)t->core     - (int)b->core;
        const int d_link  = (int)t->link     - (int)b->link;
        const int d_delta = (int)t->delta    - (int)b->delta;
        const int d_pri   = (int)t->priority - (int)b->priority;

        uint8_t tag_mask = 0;
        if (t->tone_class      != b->tone_class)      tag_mask |= IMG_CE_DIFF_TAG_TONE;
        if (t->semantic_role   != b->semantic_role)   tag_mask |= IMG_CE_DIFF_TAG_ROLE;
        if (t->direction_class != b->direction_class) tag_mask |= IMG_CE_DIFF_TAG_DIRECTION;
        if (t->depth_class     != b->depth_class)     tag_mask |= IMG_CE_DIFF_TAG_DEPTH;
        if (t->delta_sign      != b->delta_sign)      tag_mask |= IMG_CE_DIFF_TAG_DELTA_SIGN;

        if (d_core == 0 && d_link == 0 && d_delta == 0 && d_pri == 0 &&
            tag_mask == 0) {
            continue;  /* identical cell — skip */
        }

        if (!ensure_capacity(out, out->count + 1)) break;

        ImgCEDiffEntry* e = &out->entries[out->count++];
        e->idx                 = (uint16_t)i;
        e->d_core              = (int16_t)d_core;
        e->d_link              = (int16_t)d_link;
        e->d_delta             = (int16_t)d_delta;
        e->d_priority          = (int16_t)d_pri;
        e->new_tone_class      = t->tone_class;
        e->new_semantic_role   = t->semantic_role;
        e->new_direction_class = t->direction_class;
        e->new_depth_class     = t->depth_class;
        e->new_delta_sign      = t->delta_sign;
        e->tag_mask            = tag_mask;
    }

    return out->count;
}

/* ── apply ──────────────────────────────────────────────── */

static inline uint8_t sat_add_u8(uint8_t a, int d) {
    int v = (int)a + d;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

int img_ce_diff_apply(const ImgCEGrid* base,
                      const ImgCEDiff* diff,
                      ImgCEGrid* out) {
    if (!base || !out) return 0;
    if (!base->cells || !out->cells) return 0;
    if (base->width  != out->width)  return 0;
    if (base->height != out->height) return 0;

    const uint32_t n = base->width * base->height;

    /* Memcpy the base over out unless they already alias the same
     * cells pointer (self-apply). */
    if (out->cells != base->cells) {
        memcpy(out->cells, base->cells, n * sizeof(ImgCECell));
    }

    if (!diff || !diff->entries || diff->count == 0) return 1;

    for (uint32_t k = 0; k < diff->count; k++) {
        const ImgCEDiffEntry* e = &diff->entries[k];
        if (e->idx >= n) continue;
        ImgCECell* c = &out->cells[e->idx];

        if (e->d_core)     c->core     = sat_add_u8(c->core,     e->d_core);
        if (e->d_link)     c->link     = sat_add_u8(c->link,     e->d_link);
        if (e->d_delta)    c->delta    = sat_add_u8(c->delta,    e->d_delta);
        if (e->d_priority) c->priority = sat_add_u8(c->priority, e->d_priority);

        if (e->tag_mask & IMG_CE_DIFF_TAG_TONE)       c->tone_class      = e->new_tone_class;
        if (e->tag_mask & IMG_CE_DIFF_TAG_ROLE)       c->semantic_role   = e->new_semantic_role;
        if (e->tag_mask & IMG_CE_DIFF_TAG_DIRECTION)  c->direction_class = e->new_direction_class;
        if (e->tag_mask & IMG_CE_DIFF_TAG_DEPTH)      c->depth_class     = e->new_depth_class;
        if (e->tag_mask & IMG_CE_DIFF_TAG_DELTA_SIGN) c->delta_sign      = e->new_delta_sign;
    }

    return 1;
}

/* ── size estimate ──────────────────────────────────────── */

uint32_t img_ce_diff_byte_size(const ImgCEDiff* diff) {
    if (!diff) return 0;
    return (uint32_t)(sizeof(uint32_t) /* count header */ +
                      (size_t)diff->count * sizeof(ImgCEDiffEntry));
}
