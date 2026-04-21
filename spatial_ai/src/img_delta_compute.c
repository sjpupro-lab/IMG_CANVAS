#include "img_delta_compute.h"
#include "img_tier_table.h"

/* ── small helpers ──────────────────────────────────────── */

static inline int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Per-bucket multipliers used when mode + bucket interact. Tier
 * magnitudes live in img_tier_table.c as the canonical source. */
static const int TONE_MULT_INTENSITY [IMG_TONE_BUCKETS ] = { 12,  6,  3 };
static const int DEPTH_MULT_PRIORITY [IMG_DEPTH_BUCKETS] = {  3,  6, 10 };

/* ── flat index ─────────────────────────────────────────── */

size_t img_delta_table_idx(uint8_t mode, uint8_t tier,
                           uint8_t scale, uint8_t sign,
                           uint8_t tone, uint8_t depth) {
    return (size_t)mode
         + (size_t)tier  * IMG_MODE_MAX
         + (size_t)scale * (IMG_MODE_MAX * IMG_TIER_MAX)
         + (size_t)sign  * (IMG_MODE_MAX * IMG_TIER_MAX * IMG_SCALE_MAX)
         + (size_t)tone  * (IMG_MODE_MAX * IMG_TIER_MAX * IMG_SCALE_MAX * IMG_SIGN_MAX)
         + (size_t)depth * (IMG_MODE_MAX * IMG_TIER_MAX * IMG_SCALE_MAX * IMG_SIGN_MAX * IMG_TONE_BUCKETS);
}

/* ── per-entry compute ──────────────────────────────────── */

void img_delta_compute_entry(uint8_t mode, uint8_t tier,
                             uint8_t scale, uint8_t sign,
                             uint8_t tone, uint8_t depth,
                             int16_t* out_core,
                             int16_t* out_link,
                             int16_t* out_delta,
                             int16_t* out_priority,
                             uint8_t* out_pattern) {
    int16_t core = 0, link = 0, dch = 0, prio = 0;
    uint8_t pat  = 0;

    int sgn = (sign == IMG_SIGN_POS) ? +1
            : (sign == IMG_SIGN_NEG) ? -1 : 0;

    if (tier != 0 && sgn != 0 && mode != IMG_MODE_NONE
        && tier  < IMG_TIER_MAX
        && scale < IMG_SCALE_MAX
        && tone  < IMG_TONE_BUCKETS
        && depth < IMG_DEPTH_BUCKETS) {

        int base = (int)IMG_TIER_TABLE[tier].scale_factor * (2 + scale) / 2;

        switch (mode) {
            case IMG_MODE_INTENSITY:
                core = (int16_t)clamp_i(
                    sgn * base * TONE_MULT_INTENSITY[tone] / 4,
                    -200, 200);
                break;
            case IMG_MODE_LINK:
                link = (int16_t)clamp_i(sgn * base, -120, 120);
                break;
            case IMG_MODE_PRIORITY:
                prio = (int16_t)clamp_i(
                    sgn * base * DEPTH_MULT_PRIORITY[depth] / 4,
                    -200, 200);
                break;
            case IMG_MODE_MOOD:
                dch = (int16_t)clamp_i(sgn * base, -120, 120);
                /* mood_sign_fire bits 4..5 */
                pat |= (uint8_t)((sgn > 0 ? 1u : 2u) << 4);
                break;
            case IMG_MODE_DIRECTION:
                /* direction_sign bits 0..1 */
                pat |= (uint8_t)(sgn > 0 ? 1u : 2u);
                break;
            case IMG_MODE_DEPTH:
                /* depth_sign bits 2..3 */
                pat |= (uint8_t)((sgn > 0 ? 1u : 2u) << 2);
                break;
            case IMG_MODE_ROLE:
                /* role_flag bits 6..7 */
                pat |= (uint8_t)((sgn > 0 ? 1u : 2u) << 6);
                break;
            default:
                break;
        }
    }

    if (out_core)     *out_core     = core;
    if (out_link)     *out_link     = link;
    if (out_delta)    *out_delta    = dch;
    if (out_priority) *out_priority = prio;
    if (out_pattern)  *out_pattern  = pat;
}

/* ── ±1 step lookups ────────────────────────────────────── */

uint8_t img_delta_compute_direction_step(uint8_t cur_dir, uint8_t sign) {
    if (cur_dir >= IMG_FLOW_BUCKETS) cur_dir = 0;
    int up   = (int)cur_dir + 1;
    int down = (int)cur_dir - 1;
    if (up   > IMG_FLOW_DIAGONAL_DOWN) up   = IMG_FLOW_DIAGONAL_DOWN;
    if (down < 0)                      down = 0;
    if (sign == IMG_SIGN_POS) return (uint8_t)up;
    if (sign == IMG_SIGN_NEG) return (uint8_t)down;
    return cur_dir;
}

uint8_t img_delta_compute_depth_step(uint8_t cur_dep, uint8_t sign) {
    if (cur_dep >= IMG_DEPTH_BUCKETS) cur_dep = 0;
    int up   = (int)cur_dep + 1;
    int down = (int)cur_dep - 1;
    if (up   > IMG_DEPTH_FOREGROUND) up   = IMG_DEPTH_FOREGROUND;
    if (down < 0)                    down = 0;
    if (sign == IMG_SIGN_POS) return (uint8_t)up;
    if (sign == IMG_SIGN_NEG) return (uint8_t)down;
    return cur_dep;
}
