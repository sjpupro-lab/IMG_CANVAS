#include "img_noise_memory.h"
#include "img_delta_memory.h"   /* for IMG_TIER_T1..T3 enum values */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile-time layout guarantees. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(ImgNoiseSample)      == 8,  "ImgNoiseSample must be 8 bytes");
_Static_assert(sizeof(ImgNoiseCellProfile) == 64, "ImgNoiseCellProfile must be 64 bytes");
_Static_assert(sizeof(ImgNoiseLabelEntry)  == 16, "ImgNoiseLabelEntry must be 16 bytes");
#endif

/* ─── bit helpers for the 8-byte sample packing ───────────── */

/* tags0 layout (LSB → MSB):
 *   [0..1]  tone       (2 bits, 0..3)
 *   [2..4]  flow       (3 bits, 0..7)
 *   [5..6]  mood       (2 bits, 0..3)
 *   [7]     reserved
 * tags1 layout:
 *   [0..1]  depth      (2 bits, 0..3)
 *   [2..4]  role       (3 bits, 0..7)
 *   [5..6]  tier       (2 bits, 0..3)  — IMG_TIER_NONE..IMG_TIER_T3
 *   [7]     reserved
 * direction layout:
 *   [0..2]  dir_bucket (3 bits, 0..7)
 *   [3..4]  delta_sign (2 bits, 0..3)
 *   [5..7]  reserved
 */

static inline uint8_t ns_pack_tags0(uint8_t tone, uint8_t flow, uint8_t mood) {
    return (uint8_t)((tone & 0x3)
                   | ((flow & 0x7) << 2)
                   | ((mood & 0x3) << 5));
}

static inline uint8_t ns_pack_tags1(uint8_t depth, uint8_t role, uint8_t tier) {
    return (uint8_t)((depth & 0x3)
                   | ((role  & 0x7) << 2)
                   | ((tier  & 0x3) << 5));
}

static inline uint8_t ns_pack_direction(uint8_t dir, uint8_t delta_sign) {
    return (uint8_t)((dir & 0x7) | ((delta_sign & 0x3) << 3));
}

static inline uint8_t ns_tone(uint8_t tags0)  { return (uint8_t)( tags0        & 0x3); }
static inline uint8_t ns_flow(uint8_t tags0)  { return (uint8_t)((tags0 >> 2)  & 0x7); }
static inline uint8_t ns_mood(uint8_t tags0)  { return (uint8_t)((tags0 >> 5)  & 0x3); }
static inline uint8_t ns_depth(uint8_t tags1) { return (uint8_t)( tags1        & 0x3); }
static inline uint8_t ns_role(uint8_t tags1)  { return (uint8_t)((tags1 >> 2)  & 0x7); }
static inline uint8_t ns_tier(uint8_t tags1)  { return (uint8_t)((tags1 >> 5)  & 0x3); }
static inline uint8_t ns_dir(uint8_t d)       { return (uint8_t)( d            & 0x7); }
static inline uint8_t ns_dsign(uint8_t d)     { return (uint8_t)((d >> 3)      & 0x3); }

/* ─── utilities ────────────────────────────────────────────── */

static inline uint8_t ns_sat_add_u8(uint8_t a, uint8_t b) {
    unsigned v = (unsigned)a + (unsigned)b;
    return (v > 255u) ? (uint8_t)255u : (uint8_t)v;
}

/* Classify a CE direction_class into one of 8 buckets. The engine's
 * direction_class is ImgFlowClass (0..4). Until finer direction data
 * flows in we keep a 1-to-1 map and leave buckets 5..7 for future
 * diagonals. */
static inline uint8_t ns_dir_bucket_from_flow(uint8_t flow_class) {
    return (uint8_t)(flow_class & 0x7);
}

/* FNV-1a 64 over a NUL-terminated string. */
uint64_t img_noise_fnv1a64(const char* s) {
    if (!s) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ull;
    }
    return h;
}

/* Deterministic 64-bit PRNG — xorshift64. */
static inline uint64_t ns_xorshift64(uint64_t x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

/* ─── sample compression / round-trip ─────────────────────── */

static ImgNoiseSample ns_compress_cell(const ImgCECell* cell) {
    ImgNoiseSample s;
    s.ce_r = cell->core;
    s.ce_g = cell->link;
    s.ce_b = cell->delta;
    s.ce_a = cell->priority;
    s.tags0 = ns_pack_tags0(cell->tone_class,
                            cell->direction_class,    /* 1st: flow bucket */
                            0u /* mood not stored on CE cell */);
    /* tier is derived from priority band: high priority → T3, else T2/T1.
     * This keeps tier information in the prior without adding a CE field. */
    uint8_t tier;
    if      (cell->priority >= 180) tier = IMG_TIER_T3;
    else if (cell->priority >=  90) tier = IMG_TIER_T2;
    else                            tier = IMG_TIER_T1;
    s.tags1 = ns_pack_tags1(cell->depth_class,
                            cell->semantic_role,
                            tier);
    s.direction = ns_pack_direction(
        ns_dir_bucket_from_flow(cell->direction_class),
        cell->delta_sign);
    s.weight = 0;
    return s;
}

static void ns_write_sample_to_cell(ImgCECell* cell, const ImgNoiseSample* s) {
    cell->core       = s->ce_r;
    cell->link       = s->ce_g;
    cell->delta      = s->ce_b;
    cell->priority   = s->ce_a;
    cell->tone_class      = ns_tone(s->tags0);
    cell->direction_class = ns_flow(s->tags0);
    cell->depth_class     = ns_depth(s->tags1);
    cell->semantic_role   = ns_role(s->tags1);
    cell->delta_sign      = ns_dsign(s->direction);
    cell->last_delta_id   = IMG_DELTA_ID_NONE;
}

/* Byte-level hamming-ish distance over (RGBA + tags0 + tags1 + direction).
 * weight is ignored. */
static uint32_t ns_sample_distance(const ImgNoiseSample* a,
                                   const ImgNoiseSample* b) {
    uint32_t d = 0;
    if (a->ce_r      != b->ce_r)      d++;
    if (a->ce_g      != b->ce_g)      d++;
    if (a->ce_b      != b->ce_b)      d++;
    if (a->ce_a      != b->ce_a)      d++;
    if (a->tags0     != b->tags0)     d++;
    if (a->tags1     != b->tags1)     d++;
    if (a->direction != b->direction) d++;
    return d;
}

/* ─── profile accumulation ────────────────────────────────── */

/* Threshold below which two samples are counted as "the same".
 * Tuned conservatively — bump if stage-2 shows too-rapid slot churn. */
#define IMG_NOISE_SIM_THRESHOLD 2u

static void ns_accumulate_sample(ImgNoiseCellProfile* profile,
                                 const ImgNoiseSample* sample) {
    int best_match_idx = -1;
    uint32_t best_match_dist = 0;

    int weakest_idx = 0;
    uint8_t weakest_weight = profile->top_k[0].weight;

    for (int i = 0; i < IMG_NOISE_TOPK; i++) {
        ImgNoiseSample* entry = &profile->top_k[i];
        if (entry->weight == 0) {
            weakest_idx = i;
            weakest_weight = 0;
            continue;
        }
        uint32_t d = ns_sample_distance(entry, sample);
        if (d <= IMG_NOISE_SIM_THRESHOLD &&
            (best_match_idx < 0 || d < best_match_dist)) {
            best_match_idx  = i;
            best_match_dist = d;
        }
        if (entry->weight < weakest_weight) {
            weakest_weight = entry->weight;
            weakest_idx    = i;
        }
    }

    if (best_match_idx >= 0) {
        profile->top_k[best_match_idx].weight =
            ns_sat_add_u8(profile->top_k[best_match_idx].weight, 1);
        return;
    }

    /* Only displace a slot if it's weak (≤ 1). Otherwise we drop the
     * new sample — the profile is already saturated with stronger
     * evidence and we don't want one-off outliers to displace it. */
    if (weakest_weight <= 1u) {
        profile->top_k[weakest_idx]        = *sample;
        profile->top_k[weakest_idx].weight = 1u;
    }
}

static void ns_decay_profile(ImgNoiseCellProfile* profile) {
    for (int i = 0; i < IMG_NOISE_TOPK; i++) {
        uint8_t w = profile->top_k[i].weight;
        if (w == 0) continue;
        /* integer 0.95: (w * 243) >> 8 — matches spec recipe. */
        uint32_t v = ((uint32_t)w * 243u) >> 8;
        profile->top_k[i].weight = (uint8_t)v;
    }
}

static void ns_decay_all(ImgNoiseMemory* nmem) {
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        ns_decay_profile(&nmem->cell_priors[i]);
    }
    for (int t = 0; t < 3; t++) {
        ns_decay_profile(&nmem->tier_priors[t]);
    }
    ns_decay_profile(&nmem->global_prior);
}

/* ─── lifecycle ───────────────────────────────────────────── */

int img_noise_memory_init(ImgNoiseMemory* nmem) {
    if (!nmem) return 0;
    memset(nmem, 0, sizeof(*nmem));
    return 1;
}

void img_noise_memory_free(ImgNoiseMemory* nmem) {
    if (!nmem) return;
    free(nmem->label_index);
    nmem->label_index    = NULL;
    nmem->label_count    = 0;
    nmem->label_capacity = 0;
}

/* ─── label index (minimal v1) ────────────────────────────── */

static int ns_label_grow(ImgNoiseMemory* nmem, uint32_t want) {
    if (want <= nmem->label_capacity) return 1;
    uint32_t cap = nmem->label_capacity ? nmem->label_capacity : 8u;
    while (cap < want) cap *= 2u;
    ImgNoiseLabelEntry* p = (ImgNoiseLabelEntry*)realloc(
        nmem->label_index, (size_t)cap * sizeof(ImgNoiseLabelEntry));
    if (!p) return 0;
    nmem->label_index    = p;
    nmem->label_capacity = cap;
    return 1;
}

static void ns_register_label(ImgNoiseMemory* nmem, const char* label) {
    if (!label || !*label) return;
    uint64_t h = img_noise_fnv1a64(label);
    /* Skip dup. */
    for (uint32_t i = 0; i < nmem->label_count; i++) {
        if (nmem->label_index[i].label_hash == h) return;
    }
    if (!ns_label_grow(nmem, nmem->label_count + 1)) return;
    ImgNoiseLabelEntry* e = &nmem->label_index[nmem->label_count++];
    e->label_hash     = h;
    e->profile_offset = 0u;                /* reserved for v2 retrieval */
    e->keyframe_ref   = 0xFFFFFFFFu;       /* caller may later patch this */
}

/* ─── observe ─────────────────────────────────────────────── */

int img_noise_memory_observe(ImgNoiseMemory* nmem,
                             const ImgCEGrid* grid,
                             const char* label_or_null) {
    if (!nmem || !grid || !grid->cells) return 0;
    if (grid->width != IMG_CE_SIZE || grid->height != IMG_CE_SIZE) return 0;

    nmem->observe_count++;

    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        ImgNoiseSample s = ns_compress_cell(&grid->cells[i]);

        ns_accumulate_sample(&nmem->cell_priors[i], &s);

        uint8_t tier = ns_tier(s.tags1);
        if (tier >= IMG_TIER_T1 && tier <= IMG_TIER_T3) {
            ns_accumulate_sample(&nmem->tier_priors[tier - 1], &s);
        }
        ns_accumulate_sample(&nmem->global_prior, &s);
    }

    if ((nmem->observe_count % 1024u) == 0u) {
        ns_decay_all(nmem);
    }

    if (label_or_null) ns_register_label(nmem, label_or_null);

    return 1;
}

/* ─── sampling ────────────────────────────────────────────── */

ImgNoiseSampleOptions img_noise_sample_default_options(void) {
    ImgNoiseSampleOptions o;
    o.seed            = 0;
    o.tier_hint       = 0;
    o.role_hint       = 0;
    o.k_mix           = 1;
    o.flags           = 0;
    o.region_mask     = NULL;
    o.temperature_q8  = 256u;     /* 1.0 */
    o.label_hash      = 0;
    return o;
}

/* Select candidates from a profile that match the hints. Returns the
 * number of candidates written into `out` (≤ IMG_NOISE_TOPK) and only
 * includes samples with weight > 0. */
static uint32_t ns_collect(const ImgNoiseCellProfile* profile,
                           uint8_t tier_hint,
                           uint8_t role_hint,
                           const ImgNoiseSample** out) {
    uint32_t n = 0;
    for (int i = 0; i < IMG_NOISE_TOPK; i++) {
        const ImgNoiseSample* s = &profile->top_k[i];
        if (s->weight == 0) continue;
        if (tier_hint != 0 && ns_tier(s->tags1) != tier_hint) continue;
        if (role_hint != 0 && ns_role(s->tags1) != role_hint) continue;
        out[n++] = s;
    }
    return n;
}

/* Apply temperature to a weight in 8.8 fixed point, producing a u32.
 * temperature_q8 == 256 → identity, 0 → greedy (weight^∞ ≈ indicator
 * on max), large → flattened. We do not implement a real exponent;
 * instead we approximate: t > 256 tames dominant weights by mixing
 * in a uniform component; t < 256 sharpens via a rank-squared boost.
 * Cheap, fully deterministic, good enough for drawing seed priors. */
static uint32_t ns_weight_q8(uint8_t w, uint32_t temperature_q8) {
    if (w == 0) return 0;
    if (temperature_q8 == 0) {
        /* greedy: keep only the peak via rank-dominance; we emit w^2. */
        return (uint32_t)w * (uint32_t)w;
    }
    if (temperature_q8 == 256u) {
        return (uint32_t)w;
    }
    if (temperature_q8 < 256u) {
        /* sharpen: blend w with w^2 by (256 - t). */
        uint32_t sq     = (uint32_t)w * (uint32_t)w;
        uint32_t amt    = 256u - temperature_q8;
        return ((uint32_t)w * temperature_q8 + sq * amt) >> 8;
    }
    /* flatten: blend w with a constant of 1 by (t - 256). */
    uint32_t base = (uint32_t)w;
    uint32_t amt  = temperature_q8 - 256u;
    if (amt > 1024u) amt = 1024u;   /* clamp to keep u32 safe */
    return (base * 256u + amt * 16u) / 256u;
}

static const ImgNoiseSample* ns_weighted_pick(const ImgNoiseSample** cands,
                                              uint32_t n,
                                              uint64_t* prng,
                                              uint32_t temperature_q8) {
    if (n == 0) return NULL;

    uint32_t total = 0;
    uint32_t weights[IMG_NOISE_TOPK];
    for (uint32_t i = 0; i < n; i++) {
        weights[i] = ns_weight_q8(cands[i]->weight, temperature_q8);
        total += weights[i];
    }
    if (total == 0) return cands[0];

    *prng = ns_xorshift64(*prng);
    uint32_t r = (uint32_t)(*prng % (uint64_t)total);

    uint32_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc += weights[i];
        if (r < acc) return cands[i];
    }
    return cands[n - 1];
}

/* Integer-weighted blend of the top-k samples. The RGBA + direction
 * channels are arithmetic-averaged (rounded); tag bytes take the
 * winner's bits verbatim (bit-packed fields don't blend meaningfully). */
static ImgNoiseSample ns_weighted_blend(const ImgNoiseSample** cands,
                                        uint32_t n,
                                        uint32_t temperature_q8) {
    ImgNoiseSample out = {0, 0, 0, 0, 0, 0, 0, 0};
    if (n == 0) return out;

    uint32_t total = 0;
    uint32_t weights[IMG_NOISE_TOPK];
    for (uint32_t i = 0; i < n; i++) {
        weights[i] = ns_weight_q8(cands[i]->weight, temperature_q8);
        total += weights[i];
    }
    if (total == 0) return *cands[0];

    uint32_t acc_r = 0, acc_g = 0, acc_b = 0, acc_a = 0;
    uint32_t best_w = 0; uint32_t best_i = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc_r += (uint32_t)cands[i]->ce_r * weights[i];
        acc_g += (uint32_t)cands[i]->ce_g * weights[i];
        acc_b += (uint32_t)cands[i]->ce_b * weights[i];
        acc_a += (uint32_t)cands[i]->ce_a * weights[i];
        if (weights[i] > best_w) { best_w = weights[i]; best_i = i; }
    }
    out.ce_r      = (uint8_t)((acc_r + total / 2u) / total);
    out.ce_g      = (uint8_t)((acc_g + total / 2u) / total);
    out.ce_b      = (uint8_t)((acc_b + total / 2u) / total);
    out.ce_a      = (uint8_t)((acc_a + total / 2u) / total);
    out.tags0     = cands[best_i]->tags0;
    out.tags1     = cands[best_i]->tags1;
    out.direction = cands[best_i]->direction;
    out.weight    = cands[best_i]->weight;
    return out;
}

int img_noise_memory_sample_grid(const ImgNoiseMemory*        nmem,
                                 ImgCEGrid*                   out_grid,
                                 const ImgNoiseSampleOptions* opts_or_null) {
    if (!nmem || !out_grid || !out_grid->cells) return 0;
    if (out_grid->width != IMG_CE_SIZE || out_grid->height != IMG_CE_SIZE) return 0;

    ImgNoiseSampleOptions opt = opts_or_null
        ? *opts_or_null
        : img_noise_sample_default_options();

    uint64_t prng = opt.seed ? opt.seed : 0xDEADBEEFCAFEBABEull;
    const uint32_t k_mix = (opt.k_mix == 0) ? 1u : (uint32_t)opt.k_mix;

    const ImgNoiseSample* cand_buf[IMG_NOISE_TOPK];

    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        if (opt.region_mask && opt.region_mask[i] == 0) continue;

        /* 1) cell prior */
        const ImgNoiseCellProfile* profile = &nmem->cell_priors[i];
        uint32_t n = ns_collect(profile, opt.tier_hint, opt.role_hint, cand_buf);

        /* 2) tier fallback */
        if (n == 0 && opt.tier_hint >= IMG_TIER_T1 && opt.tier_hint <= IMG_TIER_T3) {
            profile = &nmem->tier_priors[opt.tier_hint - 1];
            n = ns_collect(profile, 0, opt.role_hint, cand_buf);
        }

        /* 3) global fallback */
        if (n == 0) {
            profile = &nmem->global_prior;
            n = ns_collect(profile, 0, 0, cand_buf);
        }

        if (n == 0) continue;   /* nothing learned yet — leave cell alone */

        ImgNoiseSample chosen;
        if (k_mix <= 1u) {
            const ImgNoiseSample* pick = ns_weighted_pick(
                cand_buf, n, &prng, opt.temperature_q8);
            chosen = *pick;
        } else {
            uint32_t take = (k_mix < n) ? k_mix : n;
            chosen = ns_weighted_blend(cand_buf, take, opt.temperature_q8);
        }

        ns_write_sample_to_cell(&out_grid->cells[i], &chosen);

        /* advance PRNG even for blend path to keep trajectory stable. */
        prng = ns_xorshift64(prng ^ ((uint64_t)i * 0x9E3779B97F4A7C15ull));
    }

    return 1;
}

/* ─── persistence (NMEM v1, little-endian) ───────────────── */

static int ns_write_u16_le(FILE* fp, uint16_t v) {
    uint8_t b[2];
    b[0] = (uint8_t)( v        & 0xFFu);
    b[1] = (uint8_t)((v >> 8)  & 0xFFu);
    return fwrite(b, 1, 2, fp) == 2;
}
static int ns_write_u32_le(FILE* fp, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)( v        & 0xFFu);
    b[1] = (uint8_t)((v >> 8)  & 0xFFu);
    b[2] = (uint8_t)((v >> 16) & 0xFFu);
    b[3] = (uint8_t)((v >> 24) & 0xFFu);
    return fwrite(b, 1, 4, fp) == 4;
}
static int ns_write_u64_le(FILE* fp, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
    return fwrite(b, 1, 8, fp) == 8;
}

static int ns_read_u16_le(FILE* fp, uint16_t* out) {
    uint8_t b[2];
    if (fread(b, 1, 2, fp) != 2) return 0;
    *out = (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
    return 1;
}
static int ns_read_u32_le(FILE* fp, uint32_t* out) {
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4) return 0;
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return 1;
}
static int ns_read_u64_le(FILE* fp, uint64_t* out) {
    uint8_t b[8];
    if (fread(b, 1, 8, fp) != 8) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)b[i]) << (8 * i);
    *out = v;
    return 1;
}

static int ns_write_sample(FILE* fp, const ImgNoiseSample* s) {
    uint8_t b[8] = { s->ce_r, s->ce_g, s->ce_b, s->ce_a,
                     s->tags0, s->tags1, s->direction, s->weight };
    return fwrite(b, 1, 8, fp) == 8;
}

static int ns_read_sample(FILE* fp, ImgNoiseSample* s) {
    uint8_t b[8];
    if (fread(b, 1, 8, fp) != 8) return 0;
    s->ce_r      = b[0];
    s->ce_g      = b[1];
    s->ce_b      = b[2];
    s->ce_a      = b[3];
    s->tags0     = b[4];
    s->tags1     = b[5];
    s->direction = b[6];
    s->weight    = b[7];
    return 1;
}

static int ns_write_profile(FILE* fp, const ImgNoiseCellProfile* p) {
    for (int i = 0; i < IMG_NOISE_TOPK; i++) {
        if (!ns_write_sample(fp, &p->top_k[i])) return 0;
    }
    return 1;
}

static int ns_read_profile(FILE* fp, ImgNoiseCellProfile* p) {
    for (int i = 0; i < IMG_NOISE_TOPK; i++) {
        if (!ns_read_sample(fp, &p->top_k[i])) return 0;
    }
    return 1;
}

/* Header layout (32 bytes):
 *   0..3    magic "NMEM"
 *   4..5    version u16
 *   6..7    flags   u16
 *   8..11   cell_count u32
 *   12..15  global_offset u32
 *   16..19  tier_offset   u32
 *   20..23  cell_offset   u32
 *   24..27  label_offset  u32  (0 = none)
 *   28..31  retrieval_offset u32 (reserved, 0 in v1)
 */
#define NS_HEADER_SIZE       32u
#define NS_GLOBAL_BYTES      (uint32_t)sizeof(ImgNoiseCellProfile)         /* 64  */
#define NS_TIER_BYTES        (uint32_t)(3u * sizeof(ImgNoiseCellProfile))  /* 192 */
#define NS_CELL_BYTES        (uint32_t)(IMG_CE_TOTAL * sizeof(ImgNoiseCellProfile))

int img_noise_memory_save(const ImgNoiseMemory* nmem, const char* path) {
    if (!nmem || !path) return 0;
    FILE* fp = fopen(path, "wb");
    if (!fp) return 0;

    const uint32_t global_off    = NS_HEADER_SIZE;
    const uint32_t tier_off      = global_off + NS_GLOBAL_BYTES;
    const uint32_t cell_off      = tier_off   + NS_TIER_BYTES;
    const uint32_t after_cells   = cell_off   + NS_CELL_BYTES;
    const uint32_t label_off     = nmem->label_count ? after_cells : 0u;

    uint16_t flags = 0;
    if (nmem->label_count) flags |= 0x1u;

    if (fwrite(IMG_NOISE_MAGIC, 1, 4, fp) != 4 ||
        !ns_write_u16_le(fp, (uint16_t)IMG_NOISE_VERSION) ||
        !ns_write_u16_le(fp, flags) ||
        !ns_write_u32_le(fp, (uint32_t)IMG_CE_TOTAL) ||
        !ns_write_u32_le(fp, global_off) ||
        !ns_write_u32_le(fp, tier_off) ||
        !ns_write_u32_le(fp, cell_off) ||
        !ns_write_u32_le(fp, label_off) ||
        !ns_write_u32_le(fp, 0u /* retrieval_offset reserved */)) {
        fclose(fp); return 0;
    }

    if (!ns_write_profile(fp, &nmem->global_prior)) { fclose(fp); return 0; }
    for (int t = 0; t < 3; t++) {
        if (!ns_write_profile(fp, &nmem->tier_priors[t])) {
            fclose(fp); return 0;
        }
    }
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        if (!ns_write_profile(fp, &nmem->cell_priors[i])) {
            fclose(fp); return 0;
        }
    }

    if (nmem->label_count) {
        if (!ns_write_u32_le(fp, nmem->label_count) ||
            !ns_write_u32_le(fp, 0u /* reserved */)) {
            fclose(fp); return 0;
        }
        for (uint32_t i = 0; i < nmem->label_count; i++) {
            const ImgNoiseLabelEntry* e = &nmem->label_index[i];
            if (!ns_write_u64_le(fp, e->label_hash) ||
                !ns_write_u32_le(fp, e->profile_offset) ||
                !ns_write_u32_le(fp, e->keyframe_ref)) {
                fclose(fp); return 0;
            }
        }
    }

    if (fclose(fp) != 0) return 0;
    return 1;
}

int img_noise_memory_load(ImgNoiseMemory* nmem, const char* path) {
    if (!nmem || !path) return 0;
    FILE* fp = fopen(path, "rb");
    if (!fp) return 0;

    char magic[4];
    uint16_t version = 0, flags = 0;
    uint32_t cell_count = 0;
    uint32_t global_off = 0, tier_off = 0, cell_off = 0;
    uint32_t label_off = 0, retrieval_off = 0;

    if (fread(magic, 1, 4, fp) != 4 ||
        memcmp(magic, IMG_NOISE_MAGIC, 4) != 0) {
        fclose(fp); return 0;
    }
    if (!ns_read_u16_le(fp, &version) ||
        !ns_read_u16_le(fp, &flags)   ||
        !ns_read_u32_le(fp, &cell_count) ||
        !ns_read_u32_le(fp, &global_off) ||
        !ns_read_u32_le(fp, &tier_off)   ||
        !ns_read_u32_le(fp, &cell_off)   ||
        !ns_read_u32_le(fp, &label_off)  ||
        !ns_read_u32_le(fp, &retrieval_off)) {
        fclose(fp); return 0;
    }
    if (version != IMG_NOISE_VERSION) { fclose(fp); return 0; }
    if (cell_count != IMG_CE_TOTAL)   { fclose(fp); return 0; }

    /* Reset to a clean state, but keep any existing label heap; we
     * free it to avoid leaks from a previously loaded file. */
    img_noise_memory_free(nmem);
    memset(nmem, 0, sizeof(*nmem));

    if (fseek(fp, (long)global_off, SEEK_SET) != 0 ||
        !ns_read_profile(fp, &nmem->global_prior)) {
        fclose(fp); return 0;
    }
    if (fseek(fp, (long)tier_off, SEEK_SET) != 0) { fclose(fp); return 0; }
    for (int t = 0; t < 3; t++) {
        if (!ns_read_profile(fp, &nmem->tier_priors[t])) { fclose(fp); return 0; }
    }
    if (fseek(fp, (long)cell_off, SEEK_SET) != 0) { fclose(fp); return 0; }
    for (uint32_t i = 0; i < IMG_CE_TOTAL; i++) {
        if (!ns_read_profile(fp, &nmem->cell_priors[i])) { fclose(fp); return 0; }
    }

    if (label_off != 0u && (flags & 0x1u)) {
        if (fseek(fp, (long)label_off, SEEK_SET) != 0) { fclose(fp); return 0; }
        uint32_t label_count = 0, reserved = 0;
        if (!ns_read_u32_le(fp, &label_count) ||
            !ns_read_u32_le(fp, &reserved)) {
            fclose(fp); return 0;
        }
        (void)reserved;
        if (label_count > 0u) {
            if (!ns_label_grow(nmem, label_count)) { fclose(fp); return 0; }
            for (uint32_t i = 0; i < label_count; i++) {
                ImgNoiseLabelEntry e;
                if (!ns_read_u64_le(fp, &e.label_hash) ||
                    !ns_read_u32_le(fp, &e.profile_offset) ||
                    !ns_read_u32_le(fp, &e.keyframe_ref)) {
                    fclose(fp); return 0;
                }
                nmem->label_index[i] = e;
            }
            nmem->label_count = label_count;
        }
    }

    fclose(fp);
    nmem->flags = (uint32_t)flags;
    return 1;
}
