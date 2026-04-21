#include "img_set16.h"

#include <string.h>

/* Static gather indices per quad (SPEC §6 layout). */
static const uint8_t QUAD_INDICES[IMG_QUAD_COUNT][IMG_QUAD_CELLS] = {
    /* Q0 PLUS      */ {  0,  1,  4,  5 },
    /* Q1 MINUS     */ {  2,  3,  6,  7 },
    /* Q2 SCALE     */ {  8,  9, 12, 13 },
    /* Q3 PRECISION */ { 10, 11, 14, 15 }
};

const uint8_t* img_set16_quad_indices(ImgQuadRole quad) {
    if (quad < 0 || quad >= IMG_QUAD_COUNT) return QUAD_INDICES[0];
    return QUAD_INDICES[quad];
}

ImgQuadRole img_set16_quad_for(uint8_t row, uint8_t col) {
    /* Top-half / bottom-half × left-half / right-half. */
    const uint8_t top  = (row < IMG_QUAD_DIM) ? 1u : 0u;
    const uint8_t left = (col < IMG_QUAD_DIM) ? 1u : 0u;
    if (top  &&  left) return IMG_QUAD_PLUS;
    if (top  && !left) return IMG_QUAD_MINUS;
    if (!top &&  left) return IMG_QUAD_SCALE;
    return IMG_QUAD_PRECISION;
}

void img_set16_load_from_ce(const ImgCEGrid* ce,
                            uint32_t ce_x0, uint32_t ce_y0,
                            ImgSet16* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!ce || !ce->cells) return;

    for (uint32_t dy = 0; dy < IMG_SET16_DIM; dy++) {
        for (uint32_t dx = 0; dx < IMG_SET16_DIM; dx++) {
            const uint32_t y = ce_y0 + dy;
            const uint32_t x = ce_x0 + dx;
            if (y >= ce->height || x >= ce->width) continue;

            const ImgCECell* c = &ce->cells[img_ce_idx(y, x)];
            const uint8_t i = img_set16_idx((uint8_t)dy, (uint8_t)dx);

            out->core      [i] = c->core;
            out->link      [i] = c->link;
            out->delta     [i] = c->delta;
            out->priority  [i] = c->priority;
            out->tone      [i] = c->tone_class;
            out->role      [i] = c->semantic_role;
            out->direction [i] = c->direction_class;
            out->depth     [i] = c->depth_class;
            out->delta_sign[i] = c->delta_sign;
        }
    }
}

void img_set16_store_to_ce(const ImgSet16* set,
                           uint32_t ce_x0, uint32_t ce_y0,
                           ImgCEGrid* ce) {
    if (!set || !ce || !ce->cells) return;

    for (uint32_t dy = 0; dy < IMG_SET16_DIM; dy++) {
        for (uint32_t dx = 0; dx < IMG_SET16_DIM; dx++) {
            const uint32_t y = ce_y0 + dy;
            const uint32_t x = ce_x0 + dx;
            if (y >= ce->height || x >= ce->width) continue;

            ImgCECell* c = &ce->cells[img_ce_idx(y, x)];
            const uint8_t i = img_set16_idx((uint8_t)dy, (uint8_t)dx);

            c->core            = set->core      [i];
            c->link            = set->link      [i];
            c->delta           = set->delta     [i];
            c->priority        = set->priority  [i];
            c->tone_class      = set->tone      [i];
            c->semantic_role   = set->role      [i];
            c->direction_class = set->direction [i];
            c->depth_class     = set->depth     [i];
            c->delta_sign      = set->delta_sign[i];
            /* last_delta_id intentionally untouched */
        }
    }
}
