#include "spatial_clock.h"
#include <string.h>
#include <stdlib.h>

/* ── init / copy ───────────────────────────────────────── */

void rgba_clock_init(RGBAClockEngine* ce) {
    if (!ce) return;
    memset(ce->R, CLOCK_INIT, CLOCK_CELLS);
    memset(ce->G, CLOCK_INIT, CLOCK_CELLS);
    memset(ce->B, CLOCK_INIT, CLOCK_CELLS);
    memset(ce->A, CLOCK_INIT, CLOCK_CELLS);
    ce->pos = 0;
}

void rgba_clock_copy(RGBAClockEngine* dst, const RGBAClockEngine* src) {
    if (!dst || !src) return;
    memcpy(dst, src, sizeof(*src));
}

/* ── drain helper ──────────────────────────────────────── */

/* Drain `input` units from `cells` starting at `*pos`. One pass
 * moves the head through cells; each cell supplies up to 255. If
 * the engine is already fully drained (would loop indefinitely) we
 * bail after one full sweep so the function always terminates. */
static void drain_channel(uint8_t* cells, uint32_t input, uint32_t* pos) {
    if (input == 0) return;
    uint32_t remaining = input;
    uint32_t p = *pos;
    if (p >= CLOCK_CELLS) p = 0;

    uint32_t visited = 0;
    while (remaining > 0 && visited < CLOCK_CELLS) {
        uint8_t cell = cells[p];
        if (cell == 0) {
            p = (p + 1u) % CLOCK_CELLS;
            visited++;
            continue;
        }
        if (cell >= remaining) {
            cells[p] = (uint8_t)(cell - remaining);
            if (cells[p] == 0) p = (p + 1u) % CLOCK_CELLS;
            remaining = 0;
        } else {
            remaining -= cell;
            cells[p] = 0;
            p = (p + 1u) % CLOCK_CELLS;
            visited++;
        }
    }
    *pos = p;
}

/* ── tick ──────────────────────────────────────────────── */

void rgba_clock_tick(RGBAClockEngine* ce,
                     uint8_t  r_val,
                     uint8_t  g_val,
                     uint8_t  b_val,
                     uint16_t a_val) {
    if (!ce) return;
    /* Sequential drain across the 4 channels. The shared `pos`
     * advances through R's depleted cells, then G's, etc. Each
     * channel's SAD vs a reference engine therefore reflects the
     * cumulative input of that specific channel, isolated from the
     * others' drain amounts. */
    drain_channel(ce->R, (uint32_t)r_val, &ce->pos);
    drain_channel(ce->G, (uint32_t)g_val, &ce->pos);
    drain_channel(ce->B, (uint32_t)b_val, &ce->pos);
    drain_channel(ce->A, (uint32_t)a_val, &ce->pos);
}

/* ── SAD ───────────────────────────────────────────────── */

static uint64_t sad_u8(const uint8_t* a, const uint8_t* b, uint32_t n) {
    uint64_t s = 0;
    for (uint32_t i = 0; i < n; i++) {
        int d = (int)a[i] - (int)b[i];
        s += (uint64_t)(d < 0 ? -d : d);
    }
    return s;
}

RGBAClockSad rgba_clock_sad(const RGBAClockEngine* a, const RGBAClockEngine* b) {
    RGBAClockSad s = {0, 0, 0, 0};
    if (!a || !b) return s;
    s.R_sad = sad_u8(a->R, b->R, CLOCK_CELLS);
    s.G_sad = sad_u8(a->G, b->G, CLOCK_CELLS);
    s.B_sad = sad_u8(a->B, b->B, CLOCK_CELLS);
    s.A_sad = sad_u8(a->A, b->A, CLOCK_CELLS);
    return s;
}
