#include "img_delta_learn.h"

#include <stdlib.h>
#include <string.h>

/* Noise floor for numeric channel diffs — below this we don't add a
 * rule, so that small quantization jitter from image compression
 * doesn't pollute the memory. Tag-level changes bypass this. */
#define LEARN_NUMERIC_NOISE_FLOOR 3

static inline int abs_i(int v) { return v < 0 ? -v : v; }

/* Map an unsigned magnitude to a DeltaState tier_idx. Coarse on
 * purpose — the interpret tables drive exact output; we just need
 * to pick a tier that gets us in the ballpark. */
static uint8_t tier_from_magnitude(int abs_diff) {
    if (abs_diff <= 0)  return IMG_TIER_NONE;
    if (abs_diff <= 6)  return IMG_TIER_T1;
    if (abs_diff <= 24) return IMG_TIER_T2;
    return IMG_TIER_T3;
}

/* Pick a scale index in [0, IMG_SCALE_MAX-1] roughly proportional
 * to the magnitude. scale=0..3 covers small diffs, 4..7 covers
 * larger ones. */
static uint8_t scale_from_magnitude(int abs_diff) {
    int s = abs_diff / 8;
    if (s < 0) s = 0;
    if (s > (int)IMG_SCALE_MAX - 1) s = IMG_SCALE_MAX - 1;
    return (uint8_t)s;
}

static uint8_t sign_of(int signed_diff) {
    if (signed_diff > 0) return IMG_SIGN_POS;
    if (signed_diff < 0) return IMG_SIGN_NEG;
    return IMG_SIGN_ZERO;
}

/* Returns 1 if a payload was produced, 0 if before≈after. */
static int derive_payload(const ImgCECell* pre,
                          const ImgCECell* post,
                          ImgDeltaPayload* out) {
    memset(out, 0, sizeof(*out));

    /* Tag-level precedence (discrete, always captured). */
    if (post->semantic_role != pre->semantic_role) {
        out->state = img_delta_state_simple(
            IMG_TIER_T2, /*scale=*/2, IMG_SIGN_POS, IMG_MODE_ROLE);
        out->role_target    = post->semantic_role;
        out->role_target_on = 1;
        return 1;
    }
    if (post->direction_class != pre->direction_class) {
        int diff = (int)post->direction_class - (int)pre->direction_class;
        out->state = img_delta_state_simple(
            IMG_TIER_T2, /*scale=*/0,
            sign_of(diff), IMG_MODE_DIRECTION);
        return 1;
    }
    if (post->depth_class != pre->depth_class) {
        int diff = (int)post->depth_class - (int)pre->depth_class;
        out->state = img_delta_state_simple(
            IMG_TIER_T2, /*scale=*/0,
            sign_of(diff), IMG_MODE_DEPTH);
        return 1;
    }

    /* Numeric channels: pick the axis with the largest |Δ|. */
    const int dcore  = (int)post->core     - (int)pre->core;
    const int dlink  = (int)post->link     - (int)pre->link;
    const int ddelta = (int)post->delta    - (int)pre->delta;
    const int dpri   = (int)post->priority - (int)pre->priority;

    int best_abs    = 0;
    int best_signed = 0;
    uint8_t best_mode = IMG_MODE_NONE;

    if (abs_i(dcore)  > best_abs) { best_abs = abs_i(dcore);
                                    best_signed = dcore;
                                    best_mode = IMG_MODE_INTENSITY; }
    if (abs_i(dlink)  > best_abs) { best_abs = abs_i(dlink);
                                    best_signed = dlink;
                                    best_mode = IMG_MODE_LINK; }
    if (abs_i(ddelta) > best_abs) { best_abs = abs_i(ddelta);
                                    best_signed = ddelta;
                                    best_mode = IMG_MODE_MOOD; }
    if (abs_i(dpri)   > best_abs) { best_abs = abs_i(dpri);
                                    best_signed = dpri;
                                    best_mode = IMG_MODE_PRIORITY; }

    if (best_mode == IMG_MODE_NONE) return 0;
    if (best_abs  <  LEARN_NUMERIC_NOISE_FLOOR) return 0;

    out->state = img_delta_state_simple(
        tier_from_magnitude(best_abs),
        scale_from_magnitude(best_abs),
        sign_of(best_signed),
        best_mode);
    return 1;
}

/* ── rarity weighting (hierarchical sieve) ───────────────── */

/* Mask that keys what counts as "same bucket" for rarity. Uses the
 * L2 fallback mask layout: role + tone + direction + depth, dropping
 * link_bucket and delta_sign. Two inserts with the same role/tone/
 * direction/depth are treated as observing the same pattern regardless
 * of link bucketing, so rarity reflects the semantic pattern, not
 * every surface-level permutation. */
#define LEARN_BUCKET_MASK                                       \
    (((uint64_t)0xFFu << 40) |   /* semantic_role   */          \
     ((uint64_t)0xFFu << 32) |   /* tone_class      */          \
     ((uint64_t)0xFFu << 24) |   /* direction_class */          \
     ((uint64_t)0xFFu << 16))    /* depth_class     */

/* Rarity boost cap — a brand-new pattern (count=0) inserts at
 * BOOST_MAX × baseline. Ceiling at 4× to stay far below uint16 max
 * so there is always headroom for future multiplicative refinements. */
#define LEARN_BOOST_MAX  4u

/* Count how many existing units share the target's L2 bucket. */
static uint32_t count_same_bucket(const ImgDeltaMemory* memory,
                                  ImgStateKey pre_key) {
    const uint32_t n = img_delta_memory_count(memory);
    const ImgStateKey target = pre_key & LEARN_BUCKET_MASK;
    uint32_t same = 0;
    for (uint32_t i = 0; i < n; i++) {
        const ImgDeltaUnit* u = img_delta_memory_get(memory, i);
        if (!u) continue;
        if ((u->pre_key & LEARN_BUCKET_MASK) == target) same++;
    }
    return same;
}

/* weight = baseline × max(1, BOOST_MAX / (same + 1))
 *   same=0 → boost = 4           → weight = 4000
 *   same=1 → boost = 2           → weight = 2000
 *   same=3 → boost = 1 (clamped) → weight = 1000
 *   large  → still baseline, never below.
 *
 * Never dips under baseline (no filtering), only amplifies rare
 * patterns so their cumulative feedback learns faster than common
 * patterns' noise. */
static uint16_t rarity_weight_for(const ImgDeltaMemory* memory,
                                  ImgStateKey pre_key) {
    const uint32_t same = count_same_bucket(memory, pre_key);
    uint32_t boost = LEARN_BOOST_MAX / (same + 1);
    if (boost < 1) boost = 1;
    uint32_t w = IMG_DELTA_WEIGHT_DEFAULT * boost;
    if (w > 0xFFFFu) w = 0xFFFFu;
    return (uint16_t)w;
}

/* ── public API ─────────────────────────────────────────── */

uint32_t img_delta_memory_learn_from_pair(ImgDeltaMemory* memory,
                                          const ImgCEGrid* before,
                                          const ImgCEGrid* after) {
    if (!memory || !before || !after) return 0;
    if (!before->cells || !after->cells) return 0;
    if (before->width  != after->width)  return 0;
    if (before->height != after->height) return 0;

    const uint32_t n = before->width * before->height;
    uint32_t added = 0;

    for (uint32_t i = 0; i < n; i++) {
        const ImgCECell* pre  = &before->cells[i];
        const ImgCECell* post = &after->cells[i];

        ImgDeltaPayload payload;
        if (!derive_payload(pre, post, &payload)) continue;

        const ImgStateKey pre_key  = img_state_key_from_cell(pre);
        const ImgStateKey post_key = img_state_key_from_cell(post);

        /* Hierarchical sieve: a pattern that hasn't been seen in this
         * memory's L2 bucket gets a weight boost so resolve-time
         * feedback bites harder. Common patterns stay at baseline —
         * weak signals aren't filtered, just not amplified. */
        const uint16_t w = rarity_weight_for(memory, pre_key);

        uint32_t id = img_delta_memory_add_with_hint(memory, pre_key,
                                                     payload, post_key);
        if (id != IMG_DELTA_ID_NONE) {
            /* add_with_hint set baseline weight; rewrite to the rarity
             * weight. Cast away const — this is the only sanctioned
             * post-insert field tweak in the learn path. */
            ImgDeltaUnit* mut =
                (ImgDeltaUnit*)img_delta_memory_get(memory, id);
            if (mut) mut->weight = w ? w : 1u;
            added++;
        }
    }
    return added;
}

uint32_t img_delta_memory_learn_from_images(ImgDeltaMemory* memory,
                                            const uint8_t* before_rgb,
                                            uint32_t bw, uint32_t bh,
                                            const uint8_t* after_rgb,
                                            uint32_t aw, uint32_t ah) {
    if (!memory || !before_rgb || !after_rgb) return 0;
    if (bw == 0 || bh == 0 || aw == 0 || ah == 0) return 0;

    ImgSmallCanvas* bsc = img_small_canvas_create();
    ImgSmallCanvas* asc = img_small_canvas_create();
    ImgCEGrid*      bce = img_ce_grid_create();
    ImgCEGrid*      ace = img_ce_grid_create();

    uint32_t added = 0;
    if (bsc && asc && bce && ace) {
        img_image_to_small_canvas(before_rgb, bw, bh, bsc);
        img_image_to_small_canvas(after_rgb,  aw, ah, asc);
        img_small_canvas_to_ce(bsc, bce);
        img_small_canvas_to_ce(asc, ace);
        added = img_delta_memory_learn_from_pair(memory, bce, ace);
    }

    if (bsc) img_small_canvas_destroy(bsc);
    if (asc) img_small_canvas_destroy(asc);
    if (bce) img_ce_grid_destroy(bce);
    if (ace) img_ce_grid_destroy(ace);
    return added;
}

/* ── Multi-scale learn (box-blur cascade) ────────────────── */

/* Separable box blur with running-window. In-place is NOT supported —
 * `dst` must be distinct from `src`. For radius r the kernel size is
 * (2r+1). `scratch` is a caller-provided temp buffer of the same size
 * as src/dst, used to hold the horizontal-pass result before the
 * vertical pass writes into dst. All three buffers must be
 * w*h*3 bytes. Radius 0 just memcpy's src to dst. */
static void box_blur_rgb(const uint8_t* src,
                         uint32_t w, uint32_t h, uint32_t r,
                         uint8_t* scratch, uint8_t* dst) {
    const size_t n = (size_t)w * h * 3u;
    if (r == 0) { memcpy(dst, src, n); return; }

    /* Horizontal pass: src → scratch. */
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* row_src = src     + (size_t)y * w * 3u;
        uint8_t*       row_dst = scratch + (size_t)y * w * 3u;
        for (int c = 0; c < 3; c++) {
            uint32_t sum = 0;
            /* Prime the window using mirrored borders at x < 0. */
            for (int x = -(int)r; x <= (int)r; x++) {
                int xi = x < 0 ? 0 : (x >= (int)w ? (int)w - 1 : x);
                sum += row_src[xi * 3 + c];
            }
            const uint32_t win = 2u * r + 1u;
            for (uint32_t x = 0; x < w; x++) {
                row_dst[x * 3 + c] = (uint8_t)(sum / win);
                /* slide window: drop (x - r), add (x + r + 1) */
                int drop = (int)x - (int)r;
                int add  = (int)x + (int)r + 1;
                int drop_i = drop < 0 ? 0 : (drop >= (int)w ? (int)w - 1 : drop);
                int add_i  = add  < 0 ? 0 : (add  >= (int)w ? (int)w - 1 : add);
                sum -= row_src[drop_i * 3 + c];
                sum += row_src[add_i  * 3 + c];
            }
        }
    }

    /* Vertical pass: scratch → dst. */
    for (uint32_t x = 0; x < w; x++) {
        for (int c = 0; c < 3; c++) {
            uint32_t sum = 0;
            for (int y = -(int)r; y <= (int)r; y++) {
                int yi = y < 0 ? 0 : (y >= (int)h ? (int)h - 1 : y);
                sum += scratch[(size_t)yi * w * 3u + x * 3 + c];
            }
            const uint32_t win = 2u * r + 1u;
            for (uint32_t y = 0; y < h; y++) {
                dst[(size_t)y * w * 3u + x * 3 + c] = (uint8_t)(sum / win);
                int drop = (int)y - (int)r;
                int add  = (int)y + (int)r + 1;
                int drop_i = drop < 0 ? 0 : (drop >= (int)h ? (int)h - 1 : drop);
                int add_i  = add  < 0 ? 0 : (add  >= (int)h ? (int)h - 1 : add);
                sum -= scratch[(size_t)drop_i * w * 3u + x * 3 + c];
                sum += scratch[(size_t)add_i  * w * 3u + x * 3 + c];
            }
        }
    }
}

uint32_t img_delta_memory_learn_multiscale(ImgDeltaMemory* memory,
                                           const uint8_t* image_rgb,
                                           uint32_t w, uint32_t h,
                                           const uint32_t* blur_radii,
                                           uint32_t n_radii) {
    if (!memory || !image_rgb || !blur_radii) return 0;
    if (w == 0 || h == 0 || n_radii < 2) return 0;

    const size_t bytes = (size_t)w * h * 3u;
    uint8_t* scratch  = (uint8_t*)malloc(bytes);
    uint8_t* coarser  = (uint8_t*)malloc(bytes);
    uint8_t* finer    = (uint8_t*)malloc(bytes);
    if (!scratch || !coarser || !finer) {
        free(scratch); free(coarser); free(finer);
        return 0;
    }

    uint32_t total_added = 0;

    /* Pair iteration: (radii[0], radii[1]) → (radii[1], radii[2]) → ...
     * radii are expected coarsest-first. No validation of order; the
     * caller controls semantics. */
    for (uint32_t i = 0; i + 1 < n_radii; i++) {
        box_blur_rgb(image_rgb, w, h, blur_radii[i],     scratch, coarser);
        box_blur_rgb(image_rgb, w, h, blur_radii[i + 1], scratch, finer);
        total_added += img_delta_memory_learn_from_images(
            memory,
            coarser, w, h,
            finer,   w, h);
    }

    free(scratch);
    free(coarser);
    free(finer);
    return total_added;
}
