#include "sculpt_tuning.h"

/* Phase 1–7 baseline values — preserved so default behavior is identical
 * to pre-Phase-8 binaries. Coherence threshold/bonus/penalty were single
 * scalars (24 / 2 / 1); they're now per-level but seeded with the same
 * value across all four levels, so existing tests don't shift.
 */
sculpt_tuning_t SCULPT_TUNING = {
    .level_margin       = { 1, 4, 8, 16 },
    .level_blur_box     = { 1, 2, 4, 8 },
    .level_top_g        = { 8, 4, 4, 2 },
    .default_iters      = { 1, 2, 2, 2 },
    .coherence_threshold = { 24, 24, 24, 24 },
    .coherence_bonus     = {  2,  2,  2,  2 },
    .coherence_penalty   = {  1,  1,  1,  1 },
    .a_band_width        = 32,
};

void sculpt_tuning_reset_default(void)
{
    sculpt_tuning_t reset = {
        .level_margin        = { 1, 4, 8, 16 },
        .level_blur_box      = { 1, 2, 4, 8 },
        .level_top_g         = { 8, 4, 4, 2 },
        .default_iters       = { 1, 2, 2, 2 },
        .coherence_threshold = { 24, 24, 24, 24 },
        .coherence_bonus     = {  2,  2,  2,  2 },
        .coherence_penalty   = {  1,  1,  1,  1 },
        .a_band_width        = 32,
    };
    SCULPT_TUNING = reset;
}
