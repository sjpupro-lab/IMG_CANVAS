/* posix_memalign needs this feature-test macro under glibc; MinGW
 * uses the _WIN32 branch in cv_aligned() and ignores it. */
#ifndef _WIN32
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200112L
#  endif
#endif

#include "spatial_canvas.h"
#include "spatial_layers.h"
#include "spatial_match.h"
#include "spatial_morpheme.h"
#include "spatial_q8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── DataType classification ────────────────────────────── */

static const char SPECIAL_CHARS[] = "{}()[];=<>#@$&|\\/";

DataType detect_data_type(const uint8_t* bytes, uint32_t len) {
    if (!bytes || len == 0) return DATA_SHORT;

    uint32_t special = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = bytes[i];
        if (b < 128 && b > 0 && strchr(SPECIAL_CHARS, (int)b)) special++;
    }
    float special_ratio = (float)special / (float)len;

    if (special_ratio > 0.15f) return DATA_CODE;
    if (len < 30)               return DATA_SHORT;
    if (len > 150)              return DATA_PROSE;
    return DATA_DIALOG;
}

const char* data_type_name(DataType t) {
    switch (t) {
        case DATA_PROSE:  return "PROSE";
        case DATA_DIALOG: return "DIALOG";
        case DATA_CODE:   return "CODE";
        case DATA_SHORT:  return "SHORT";
        default:          return "?";
    }
}

float data_type_boundary_weight(DataType t) {
    switch (t) {
        case DATA_PROSE:  return 0.5f;
        case DATA_DIALOG: return 0.3f;
        case DATA_CODE:   return 0.1f;
        case DATA_SHORT:  return 0.02f;
        default:          return 0.5f;
    }
}

/* djb2 topic hash used by canvas_add_clause */
static uint32_t topic_hash_djb2(const char* s) {
    uint32_t h = 5381;
    while (*s) h = h * 33u + (uint8_t)(*s++);
    return h;
}

#ifdef _WIN32
#include <malloc.h>
static void* cv_aligned(size_t alignment, size_t size) {
    return _aligned_malloc(size, alignment);
}
static void cv_aligned_free(void* p) { _aligned_free(p); }
#else
static void* cv_aligned(size_t alignment, size_t size) {
    void* p = NULL;
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
}
static void cv_aligned_free(void* p) { free(p); }
#endif

/* ── Lifecycle ─────────────────────────────────────────── */

SpatialCanvas* canvas_create(void) {
    SpatialCanvas* c = (SpatialCanvas*)calloc(1, sizeof(SpatialCanvas));
    if (!c) return NULL;

    c->A = (uint16_t*)cv_aligned(32, CV_TOTAL * sizeof(uint16_t));
    c->R = (uint8_t*) cv_aligned(32, CV_TOTAL);
    c->G = (uint8_t*) cv_aligned(32, CV_TOTAL);
    c->B = (uint8_t*) cv_aligned(32, CV_TOTAL);

    if (!c->A || !c->R || !c->G || !c->B) {
        canvas_destroy(c);
        return NULL;
    }
    memset(c->A, 0, CV_TOTAL * sizeof(uint16_t));
    memset(c->R, 0, CV_TOTAL);
    memset(c->G, 0, CV_TOTAL);
    memset(c->B, 0, CV_TOTAL);
    c->width  = CV_WIDTH;
    c->height = CV_HEIGHT;
    c->slot_count = 0;
    c->canvas_type = DATA_PROSE;  /* overwritten by first placement */
    /* Default meta: unoccupied, full-weight boundaries so canvases
     * that never call canvas_add_clause still diffuse uniformly. */
    for (uint32_t s = 0; s < CV_SLOTS; s++) {
        c->meta[s].type = DATA_PROSE;
        c->meta[s].boundary_weight = 1.0f;
        c->meta[s].byte_length = 0;
        c->meta[s].topic_hash = 0;
        c->meta[s].occupied = 0;
    }
    /* I/P defaults — unclassified canvas treated as IFRAME with no parent */
    c->frame_type = CANVAS_IFRAME;
    c->parent_canvas_id = UINT32_MAX;
    c->changed_ratio = 0.0f;
    c->classified = 0;
    rgba_clock_init(&c->clock);
    return c;
}

void canvas_destroy(SpatialCanvas* c) {
    if (!c) return;
    if (c->A) cv_aligned_free(c->A);
    if (c->R) cv_aligned_free(c->R);
    if (c->G) cv_aligned_free(c->G);
    if (c->B) cv_aligned_free(c->B);
    free(c);
}

void canvas_clear(SpatialCanvas* c) {
    if (!c) return;
    memset(c->A, 0, CV_TOTAL * sizeof(uint16_t));
    memset(c->R, 0, CV_TOTAL);
    memset(c->G, 0, CV_TOTAL);
    memset(c->B, 0, CV_TOTAL);
    c->slot_count = 0;
    for (uint32_t s = 0; s < CV_SLOTS; s++) {
        c->meta[s].type = DATA_PROSE;
        c->meta[s].boundary_weight = 1.0f;
        c->meta[s].byte_length = 0;
        c->meta[s].topic_hash = 0;
        c->meta[s].occupied = 0;
    }
    c->frame_type = CANVAS_IFRAME;
    c->parent_canvas_id = UINT32_MAX;
    c->changed_ratio = 0.0f;
    c->classified = 0;
    rgba_clock_init(&c->clock);
}

/* ── Slot → canvas-space coordinate helpers ────────────── */

uint32_t canvas_slot_byte_offset(uint32_t slot, uint32_t* out_x0, uint32_t* out_y0) {
    uint32_t col = slot % CV_COLS;
    uint32_t row = slot / CV_COLS;
    uint32_t x0  = col * CV_TILE;
    uint32_t y0  = row * CV_TILE;
    if (out_x0) *out_x0 = x0;
    if (out_y0) *out_y0 = y0;
    return y0 * CV_WIDTH + x0;
}

/* ── Tile placement ────────────────────────────────────── */

int canvas_add_clause(SpatialCanvas* c, const char* text) {
    if (!c || !text || c->slot_count >= CV_SLOTS) return -1;

    /* Encode into a temporary 256×256 tile */
    SpatialGrid* tile = grid_create();
    if (!tile) return -1;
    morpheme_init();
    layers_encode_clause(text, NULL, tile);
    /* Note: we intentionally skip update_rgb_directional on the tile;
       diffusion happens canvas-wide after placement (see
       canvas_update_rgb). B is already seeded with the clause's
       co-occurrence hash by layers_encode_clause. */

    /* Copy tile into the slot */
    uint32_t slot = c->slot_count;
    uint32_t x0, y0;
    canvas_slot_byte_offset(slot, &x0, &y0);

    for (uint32_t dy = 0; dy < CV_TILE; dy++) {
        for (uint32_t dx = 0; dx < CV_TILE; dx++) {
            uint32_t ti = dy * CV_TILE + dx;
            uint32_t ci = (y0 + dy) * CV_WIDTH + (x0 + dx);
            c->A[ci] = tile->A[ti];
            c->R[ci] = tile->R[ti];
            c->G[ci] = tile->G[ti];
            c->B[ci] = tile->B[ti];
        }
    }

    grid_destroy(tile);

    /* Populate meta for this slot from the clause's auto-detected type */
    uint32_t text_len = (uint32_t)strlen(text);
    DataType t = detect_data_type((const uint8_t*)text, text_len);
    c->meta[slot].type            = t;
    c->meta[slot].boundary_weight = data_type_boundary_weight(t);
    c->meta[slot].byte_length     = text_len;
    c->meta[slot].topic_hash      = topic_hash_djb2(text);
    c->meta[slot].occupied        = 1;

    /* First placement sets the canvas's overall type */
    if (c->slot_count == 0) c->canvas_type = t;

    c->slot_count++;
    return (int)slot;
}

/* ── Canvas-wide RGB directional diffusion ─────────────── */

/* Return the boundary-diffusion multiplier for an update that flows
 *   from (sx, sy) into (dx, dy). Within the same slot the multiplier
 *   is 1.0; crossing a slot boundary uses the min of the two slots'
 *   boundary_weight. If both slots have a non-zero freq_tag and the
 *   tags disagree, the result is dampened by an extra 0.1× — chapters
 *   inside a single canvas should diffuse weakly across each other. */
static inline float boundary_multiplier(const SpatialCanvas* c,
                                        uint32_t sx, uint32_t sy,
                                        uint32_t dx, uint32_t dy) {
    uint32_t s1 = (sy / CV_TILE) * CV_COLS + (sx / CV_TILE);
    uint32_t s2 = (dy / CV_TILE) * CV_COLS + (dx / CV_TILE);
    if (s1 == s2) return 1.0f;
    if (s1 >= CV_SLOTS || s2 >= CV_SLOTS) return 1.0f;
    float w1 = c->meta[s1].boundary_weight;
    float w2 = c->meta[s2].boundary_weight;
    float base = (w1 < w2) ? w1 : w2;

    uint16_t t1 = c->meta[s1].freq_tag;
    uint16_t t2 = c->meta[s2].freq_tag;
    if (t1 != 0 && t2 != 0 && t1 != t2) base *= 0.1f;

    return base;
}

/* ── freq_tag helpers ────────────────────────────────────
 *
 * Chapter grouping within a single canvas. The streaming trainer
 * places clauses into slots in arrival order regardless of any
 * higher-level structure (paragraphs, sections); freq_tag carves
 * those 32 slots into local "chapters" so RGB diffusion can stop at
 * meaningful boundaries even when DataType (and therefore
 * boundary_weight) is uniform across the canvas.
 */

/* Boundary signal between two slots.
 *
 * v1 sampled the B-channel at the literal pixel edge (right-most
 * columns of left slot + left-most columns of right slot). It worked
 * in theory but layers_encode_clause maps each byte to (byte_index,
 * byte_value), and ASCII byte values cluster in [32, 126]. That puts
 * active cells in *middle* columns and leaves the boundary band
 * empty for English text, so the metric collapsed to 0 → every
 * horizontal slot transition was tagged as "weak" → 29 chapters per
 * canvas (one per slot, modulo row crossings).
 *
 * v2 uses the A-channel cosine similarity between the two slots'
 * 256×256 regions. High cosine = same topic/structure = stay in
 * chapter; low cosine = topic shift = chapter break. Robust across
 * encodings (ASCII / UTF-8 / binary) because it doesn't depend on
 * where in the column space the active cells happen to land.
 *
 * Caller semantics flip: now b_threshold is the *minimum similarity*
 * required to stay in the same chapter, not a "below this is weak"
 * value. Reasonable default range 0.10 .. 0.30 for wiki-style text. */
float canvas_b_edge_value(const SpatialCanvas* c,
                          uint32_t slot_a, uint32_t slot_b) {
    if (!c || slot_a >= CV_SLOTS || slot_b >= CV_SLOTS) return -1.0f;

    uint32_t row_a = slot_a / CV_COLS;
    uint32_t col_a = slot_a % CV_COLS;
    uint32_t row_b = slot_b / CV_COLS;
    uint32_t col_b = slot_b % CV_COLS;

    /* Same row + adjacent columns only — see header comment. */
    if (row_a != row_b) return -1.0f;
    int adj = ((col_a + 1 == col_b) || (col_b + 1 == col_a));
    if (!adj) return -1.0f;

    uint32_t a_x0 = col_a * CV_TILE, a_y0 = row_a * CV_TILE;
    uint32_t b_x0 = col_b * CV_TILE, b_y0 = row_b * CV_TILE;

    double dot = 0.0, na = 0.0, nb = 0.0;
    for (uint32_t dy = 0; dy < CV_TILE; dy++) {
        for (uint32_t dx = 0; dx < CV_TILE; dx++) {
            uint32_t ai = (a_y0 + dy) * CV_WIDTH + (a_x0 + dx);
            uint32_t bi = (b_y0 + dy) * CV_WIDTH + (b_x0 + dx);
            double va = (double)c->A[ai];
            double vb = (double)c->A[bi];
            dot += va * vb;
            na  += va * va;
            nb  += vb * vb;
        }
    }
    if (na == 0.0 || nb == 0.0) return 0.0f;
    return (float)(dot / (sqrt(na) * sqrt(nb)));
}

uint32_t canvas_b_edge_sad(const SpatialCanvas* c,
                           uint32_t slot_a, uint32_t slot_b) {
    if (!c || slot_a >= CV_SLOTS || slot_b >= CV_SLOTS) return UINT32_MAX;

    uint32_t row_a = slot_a / CV_COLS;
    uint32_t col_a = slot_a % CV_COLS;
    uint32_t row_b = slot_b / CV_COLS;
    uint32_t col_b = slot_b % CV_COLS;
    if (row_a != row_b) return UINT32_MAX;
    int adj = ((col_a + 1 == col_b) || (col_b + 1 == col_a));
    if (!adj) return UINT32_MAX;

    uint32_t a_x0 = col_a * CV_TILE, a_y0 = row_a * CV_TILE;
    uint32_t b_x0 = col_b * CV_TILE, b_y0 = row_b * CV_TILE;

    uint32_t sad = 0;
    for (uint32_t dy = 0; dy < CV_TILE; dy++) {
        for (uint32_t dx = 0; dx < CV_TILE; dx++) {
            uint32_t ai = (a_y0 + dy) * CV_WIDTH + (a_x0 + dx);
            uint32_t bi = (b_y0 + dy) * CV_WIDTH + (b_x0 + dx);
            uint16_t va = c->A[ai];
            uint16_t vb = c->A[bi];
            sad += (va > vb) ? (uint32_t)(va - vb) : (uint32_t)(vb - va);
        }
    }
    return sad;
}

uint16_t canvas_b_edge_q16(const SpatialCanvas* c,
                           uint32_t slot_a, uint32_t slot_b) {
    if (!c || slot_a >= CV_SLOTS || slot_b >= CV_SLOTS) return 0;

    uint32_t row_a = slot_a / CV_COLS;
    uint32_t col_a = slot_a % CV_COLS;
    uint32_t row_b = slot_b / CV_COLS;
    uint32_t col_b = slot_b % CV_COLS;
    if (row_a != row_b) return 0;
    int adj = ((col_a + 1 == col_b) || (col_b + 1 == col_a));
    if (!adj) return 0;

    uint32_t a_x0 = col_a * CV_TILE, a_y0 = row_a * CV_TILE;
    uint32_t b_x0 = col_b * CV_TILE, b_y0 = row_b * CV_TILE;

    uint64_t dot = 0, na = 0, nb = 0;
    for (uint32_t dy = 0; dy < CV_TILE; dy++) {
        for (uint32_t dx = 0; dx < CV_TILE; dx++) {
            uint32_t ai = (a_y0 + dy) * CV_WIDTH + (a_x0 + dx);
            uint32_t bi = (b_y0 + dy) * CV_WIDTH + (b_x0 + dx);
            uint64_t va = (uint64_t)c->A[ai];
            uint64_t vb = (uint64_t)c->A[bi];
            dot += va * vb;
            na  += va * va;
            nb  += vb * vb;
        }
    }
    return q16_cosine(dot, na, nb);
}

uint16_t canvas_get_freq_tag(const SpatialCanvas* c,
                             uint32_t x, uint32_t y) {
    if (!c) return 0;
    if (x >= CV_WIDTH || y >= CV_HEIGHT) return 0;
    uint32_t slot = (y / CV_TILE) * CV_COLS + (x / CV_TILE);
    if (slot >= CV_SLOTS) return 0;
    return c->meta[slot].freq_tag;
}

/* ── Compute-canvas summaries ─────────────────────────────
 *
 * 3-byte (b_mean, hz_hist) summary per slot. b_mean = average B over
 * active cells (context strength); hz_hist = 4-bin column histogram
 * of A activity, 4 bits per bin (vocabulary / byte-spectrum
 * fingerprint). canvas_assign_freq_tags compares two slots'
 * summaries via L1 SAD instead of cosine on the raw 256×256.
 */

/* ── Full-channel delta (I/P-frame canvas storage) ────────── */

uint32_t canvas_full_delta(const SpatialCanvas* parent,
                           const SpatialCanvas* child,
                           CanvasFullDelta* out) {
    if (!parent || !child || !out) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < CV_TOTAL; i++) {
        int16_t dA = (int16_t)((int)child->A[i] - (int)parent->A[i]);
        int8_t  dR = (int8_t) ((int)child->R[i] - (int)parent->R[i]);
        int8_t  dG = (int8_t) ((int)child->G[i] - (int)parent->G[i]);
        int8_t  dB = (int8_t) ((int)child->B[i] - (int)parent->B[i]);
        if (dA != 0 || dR != 0 || dG != 0 || dB != 0) {
            out[n].index  = i;
            out[n].diff_A = dA;
            out[n].diff_R = dR;
            out[n].diff_G = dG;
            out[n].diff_B = dB;
            n++;
        }
    }
    return n;
}

void canvas_apply_full_delta(const SpatialCanvas* parent,
                             const CanvasFullDelta* entries,
                             uint32_t count,
                             SpatialCanvas* target) {
    if (!parent || !target) return;
    /* Start from parent pixels as the reconstruction base. */
    memcpy(target->A, parent->A, CV_TOTAL * sizeof(uint16_t));
    memcpy(target->R, parent->R, CV_TOTAL);
    memcpy(target->G, parent->G, CV_TOTAL);
    memcpy(target->B, parent->B, CV_TOTAL);

    if (!entries) return;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t i = entries[k].index;
        if (i >= CV_TOTAL) continue;
        int a = (int)target->A[i] + entries[k].diff_A;
        int r = (int)target->R[i] + entries[k].diff_R;
        int g = (int)target->G[i] + entries[k].diff_G;
        int b = (int)target->B[i] + entries[k].diff_B;
        if (a < 0) a = 0; else if (a > 65535) a = 65535;
        if (r < 0) r = 0; else if (r > 255)   r = 255;
        if (g < 0) g = 0; else if (g > 255)   g = 255;
        if (b < 0) b = 0; else if (b > 255)   b = 255;
        target->A[i] = (uint16_t)a;
        target->R[i] = (uint8_t)r;
        target->G[i] = (uint8_t)g;
        target->B[i] = (uint8_t)b;
    }
}

#define HZ_BINS         4
#define HZ_BIN_COLS     (CV_TILE / HZ_BINS)   /* = 64 */
#define HZ_BIN_MAX      15u                   /* 4 bits per bin */

void canvas_compute_slot_summary(SpatialCanvas* c, uint32_t slot) {
    if (!c || slot >= CV_SLOTS) return;
    uint32_t x0, y0;
    canvas_slot_byte_offset(slot, &x0, &y0);

    uint32_t bins[HZ_BINS] = {0};
    uint64_t b_sum = 0;
    uint32_t active = 0;

    for (uint32_t dy = 0; dy < CV_TILE; dy++) {
        for (uint32_t dx = 0; dx < CV_TILE; dx++) {
            uint32_t i = (y0 + dy) * CV_WIDTH + (x0 + dx);
            if (c->A[i] == 0) continue;
            active++;
            b_sum += c->B[i];
            uint32_t bin = dx / HZ_BIN_COLS;
            if (bin < HZ_BINS) bins[bin]++;
        }
    }

    SlotComputeSummary* s = &c->compute[slot];
    s->b_mean  = active ? (uint8_t)(b_sum / active) : 0;

    if (active == 0) { s->hz_hist = 0; return; }

    /* Normalize each bin to 0..15 and pack 4 bits per nibble. */
    uint16_t hist = 0;
    for (uint32_t k = 0; k < HZ_BINS; k++) {
        uint32_t n = (bins[k] * HZ_BIN_MAX) / active;
        if (n > HZ_BIN_MAX) n = HZ_BIN_MAX;
        hist |= (uint16_t)(n << (k * 4));
    }
    s->hz_hist = hist;
}

void canvas_compute_all_summaries(SpatialCanvas* c) {
    if (!c) return;
    for (uint32_t s = 0; s < c->slot_count; s++) {
        canvas_compute_slot_summary(c, s);
    }
}

uint16_t canvas_summary_sad(const SpatialCanvas* c,
                            uint32_t slot_a, uint32_t slot_b) {
    if (!c || slot_a >= CV_SLOTS || slot_b >= CV_SLOTS) return UINT16_MAX;
    const SlotComputeSummary* sa = &c->compute[slot_a];
    const SlotComputeSummary* sb = &c->compute[slot_b];

    uint16_t b_diff = (sa->b_mean > sb->b_mean)
                    ? (uint16_t)(sa->b_mean - sb->b_mean)
                    : (uint16_t)(sb->b_mean - sa->b_mean);

    uint16_t hz_diff = 0;
    for (uint32_t k = 0; k < HZ_BINS; k++) {
        uint8_t va = (uint8_t)((sa->hz_hist >> (k * 4)) & 0xF);
        uint8_t vb = (uint8_t)((sb->hz_hist >> (k * 4)) & 0xF);
        hz_diff += (va > vb) ? (uint16_t)(va - vb) : (uint16_t)(vb - va);
    }
    return (uint16_t)(b_diff + hz_diff);
}

/* Clockwork-engine chapter assignment.
 *
 * Per-slot transition: compute the four input signals and tick the
 * canvas's engine. SAD is measured against a snapshot taken at the
 * start of the current chapter — the "accumulated delta since the
 * chapter began". G_sad (chapter-group signal) is the primary
 * break trigger; R+B combined handles context + structure breaks
 * that don't manifest as a pure G shift.
 *
 * Inputs per slot:
 *   R_in = b_diff (b_mean[i] vs b_mean[i-1], clamped to u8)
 *   G_in = hz_diff (nibble-SAD of hz_hist[i] vs hz_hist[i-1], 0..60)
 *   B_in = hz_hist sum of slot i (0..60)
 *   A_in = min(active_cells × 256, 65535) — uint16
 */
void canvas_assign_freq_tags_clock(SpatialCanvas* c,
                                   uint64_t g_threshold,
                                   uint64_t rb_threshold,
                                   int      use_topic_hash) {
    if (!c || c->slot_count == 0) return;

    rgba_clock_init(&c->clock);
    RGBAClockEngine chapter_start;
    rgba_clock_copy(&chapter_start, &c->clock);

    uint16_t current_tag = 1;
    c->meta[0].freq_tag = current_tag;
    if (c->slot_count == 1) return;

    for (uint32_t i = 1; i < c->slot_count; i++) {
        const SlotMeta* prev = &c->meta[i - 1];
        SlotMeta*       cur  = &c->meta[i];

        int topic_changed = (use_topic_hash &&
                             prev->topic_hash != cur->topic_hash);

        /* Skip SAD check across physical row boundaries (8×4 layout). */
        uint32_t row_prev = (i - 1) / CV_COLS;
        uint32_t row_cur  = i       / CV_COLS;
        int same_row = (row_prev == row_cur);

        /* ── build tick inputs from compute summaries ── */
        const SlotComputeSummary* sa = &c->compute[i - 1];
        const SlotComputeSummary* sb = &c->compute[i];

        uint32_t b_diff = (sa->b_mean > sb->b_mean)
                        ? (uint32_t)(sa->b_mean - sb->b_mean)
                        : (uint32_t)(sb->b_mean - sa->b_mean);
        if (b_diff > 255u) b_diff = 255u;

        uint32_t hz_diff = 0;
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t va = (sa->hz_hist >> (k * 4)) & 0xF;
            uint32_t vb = (sb->hz_hist >> (k * 4)) & 0xF;
            hz_diff += (va > vb) ? (va - vb) : (vb - va);
        }
        if (hz_diff > 255u) hz_diff = 255u;

        uint32_t hz_sum = 0;
        for (uint32_t k = 0; k < 4; k++) {
            hz_sum += (sb->hz_hist >> (k * 4)) & 0xF;
        }
        if (hz_sum > 255u) hz_sum = 255u;

        /* Active-cell count of slot i, scaled × 256 into uint16 space. */
        uint32_t x0, y0;
        canvas_slot_byte_offset(i, &x0, &y0);
        uint32_t active = 0;
        for (uint32_t dy = 0; dy < CV_TILE; dy++) {
            for (uint32_t dx = 0; dx < CV_TILE; dx++) {
                if (c->A[(y0 + dy) * CV_WIDTH + (x0 + dx)] > 0) active++;
            }
        }
        uint32_t a_scaled = active * 256u;
        if (a_scaled > 65535u) a_scaled = 65535u;

        rgba_clock_tick(&c->clock,
                        (uint8_t)b_diff,
                        (uint8_t)hz_diff,
                        (uint8_t)hz_sum,
                        (uint16_t)a_scaled);

        int clock_break = 0;
        if (same_row) {
            RGBAClockSad sad = rgba_clock_sad(&chapter_start, &c->clock);
            if (sad.G_sad > g_threshold) {
                clock_break = 1;
            } else if (sad.R_sad + sad.B_sad > rb_threshold) {
                clock_break = 1;
            }
        }

        if (clock_break || topic_changed) {
            current_tag++;
            rgba_clock_copy(&chapter_start, &c->clock);
        }
        cur->freq_tag = current_tag;
    }
}

void canvas_assign_freq_tags(SpatialCanvas* c,
                             uint16_t sad_threshold,
                             int      use_topic_hash) {
    if (!c || c->slot_count == 0) return;

    uint16_t current_tag = 1;
    c->meta[0].freq_tag = current_tag;
    if (c->slot_count == 1) return;

    /* Compare adjacent slots via the compute-canvas summary.
     *   sad < threshold  → similar enough, stay in current chapter
     *   sad ≥ threshold  → different enough, start a new chapter
     * Row crossings (vertical placement neighbours) are skipped from
     * the SAD check; topic_changed alone may still split there. */
    for (uint32_t i = 1; i < c->slot_count; i++) {
        const SlotMeta* prev = &c->meta[i - 1];
        SlotMeta*       cur  = &c->meta[i];

        int topic_changed = (use_topic_hash &&
                             prev->topic_hash != cur->topic_hash);

        uint32_t row_prev = (i - 1) / CV_COLS;
        uint32_t row_cur  = i       / CV_COLS;
        int      sad_high = 0;
        if (row_prev == row_cur) {
            uint16_t sad = canvas_summary_sad(c, i - 1, i);
            sad_high = (sad >= sad_threshold);
        }

        if (sad_high || topic_changed) current_tag++;
        cur->freq_tag = current_tag;
    }
}

void canvas_update_rgb(SpatialCanvas* c) {
    if (!c) return;
    uint32_t W = c->width;
    uint32_t H = c->height;

    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            uint32_t i = y * W + x;
            if (c->A[i] == 0) continue;

            /* R: diagonal neighbours (semantic / morpheme relation) */
            static const int dx_off[4] = {1, 1, -1, -1};
            static const int dy_off[4] = {1, -1, 1, -1};
            for (int d = 0; d < 4; d++) {
                int nx = (int)x + dx_off[d], ny = (int)y + dy_off[d];
                if (nx < 0 || nx >= (int)W || ny < 0 || ny >= (int)H) continue;
                uint32_t ni = (uint32_t)ny * W + (uint32_t)nx;
                if (c->A[ni] == 0) continue;
                float bw = boundary_multiplier(c, (uint32_t)nx, (uint32_t)ny, x, y);
                int diff = (int)c->R[ni] - (int)c->R[i];
                int new_v = (int)c->R[i] + (int)(ALPHA_R * diff * bw);
                if (new_v < 0) new_v = 0;
                if (new_v > 255) new_v = 255;
                c->R[i] = (uint8_t)new_v;
            }
            /* G: vertical (substitution) — crosses tile row boundaries */
            for (int d = -1; d <= 1; d += 2) {
                int ny = (int)y + d;
                if (ny < 0 || ny >= (int)H) continue;
                uint32_t ni = (uint32_t)ny * W + x;
                if (c->A[ni] == 0) continue;
                float bw = boundary_multiplier(c, x, (uint32_t)ny, x, y);
                int diff = (int)c->G[ni] - (int)c->G[i];
                int new_v = (int)c->G[i] + (int)(BETA_G * diff * bw);
                if (new_v < 0) new_v = 0;
                if (new_v > 255) new_v = 255;
                c->G[i] = (uint8_t)new_v;
            }
            /* B: horizontal (clause order) — crosses tile column boundaries */
            for (int d = -1; d <= 1; d += 2) {
                int nx = (int)x + d;
                if (nx < 0 || nx >= (int)W) continue;
                uint32_t ni = y * W + (uint32_t)nx;
                if (c->A[ni] == 0) continue;
                float bw = boundary_multiplier(c, (uint32_t)nx, y, x, y);
                int diff = (int)c->B[ni] - (int)c->B[i];
                int new_v = (int)c->B[i] + (int)(GAMMA_B * diff * bw);
                if (new_v < 0) new_v = 0;
                if (new_v > 255) new_v = 255;
                c->B[i] = (uint8_t)new_v;
            }
        }
    }
}

/* ── Stats ─────────────────────────────────────────────── */

uint32_t canvas_active_count(const SpatialCanvas* c) {
    if (!c) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < CV_TOTAL; i++) if (c->A[i] > 0) n++;
    return n;
}

void canvas_compute_block_sums(const SpatialCanvas* c, CanvasBlockSummary* out) {
    if (!c || !out) return;
    memset(out->sums, 0, sizeof(out->sums));
    for (uint32_t by = 0; by < CV_BLOCKS_Y; by++) {
        for (uint32_t bx = 0; bx < CV_BLOCKS_X; bx++) {
            uint32_t s = 0;
            for (uint32_t dy = 0; dy < CV_BLOCK; dy++) {
                for (uint32_t dx = 0; dx < CV_BLOCK; dx++) {
                    uint32_t ci = (by * CV_BLOCK + dy) * CV_WIDTH
                                 + (bx * CV_BLOCK + dx);
                    s += c->A[ci];
                }
            }
            out->sums[by * CV_BLOCKS_X + bx] = s;
        }
    }
}

/* ── Slot → grid export ────────────────────────────────── */

void canvas_slot_to_grid(const SpatialCanvas* c, uint32_t slot, SpatialGrid* out) {
    if (!c || !out || slot >= CV_SLOTS) return;
    uint32_t x0, y0;
    canvas_slot_byte_offset(slot, &x0, &y0);
    for (uint32_t dy = 0; dy < CV_TILE; dy++) {
        for (uint32_t dx = 0; dx < CV_TILE; dx++) {
            uint32_t ti = dy * CV_TILE + dx;
            uint32_t ci = (y0 + dy) * CV_WIDTH + (x0 + dx);
            out->A[ti] = c->A[ci];
            out->R[ti] = c->R[ci];
            out->G[ti] = c->G[ci];
            out->B[ti] = c->B[ci];
        }
    }
}

/* ── Slot matching ─────────────────────────────────────── */

float canvas_match_slot(const SpatialCanvas* c, const SpatialGrid* query, uint32_t slot) {
    if (!c || !query || slot >= CV_SLOTS) return 0.0f;
    uint32_t x0, y0;
    canvas_slot_byte_offset(slot, &x0, &y0);

    double dot = 0.0, na = 0.0, nb = 0.0;
    for (uint32_t dy = 0; dy < CV_TILE; dy++) {
        for (uint32_t dx = 0; dx < CV_TILE; dx++) {
            uint32_t qi = dy * CV_TILE + dx;
            uint32_t ci = (y0 + dy) * CV_WIDTH + (x0 + dx);
            double qa = (double)query->A[qi];
            double ca = (double)c->A[ci];
            if (qa > 0.0 && ca > 0.0) {
                /* RGB per-cell weight (same formula as rgb_weight) */
                double dr = fabs((double)query->R[qi] - c->R[ci]) / 255.0;
                double dg = fabs((double)query->G[qi] - c->G[ci]) / 255.0;
                double db = fabs((double)query->B[qi] - c->B[ci]) / 255.0;
                double w  = 1.0 - (0.5*dr + 0.3*dg + 0.2*db);
                if (w < 0.0) w = 0.0;
                dot += qa * ca * w;
            }
            na += qa * qa;
            nb += ca * ca;
        }
    }
    if (na == 0.0 || nb == 0.0) return 0.0f;
    return (float)(dot / (sqrt(na) * sqrt(nb)));
}

uint32_t canvas_best_slot(const SpatialCanvas* c, const SpatialGrid* query, float* out_sim) {
    if (!c || !query || c->slot_count == 0) {
        if (out_sim) *out_sim = 0.0f;
        return 0;
    }
    uint32_t best_slot = 0;
    float    best_sim  = -1.0f;
    for (uint32_t s = 0; s < c->slot_count; s++) {
        float v = canvas_match_slot(c, query, s);
        if (v > best_sim) { best_sim = v; best_slot = s; }
    }
    if (out_sim) *out_sim = best_sim;
    return best_slot;
}

/* ── Delta (sparse + RLE byte estimate) ────────────────── */

uint32_t canvas_delta_sparse(const SpatialCanvas* a, const SpatialCanvas* b,
                             CanvasDeltaEntry* out, uint32_t max_out) {
    if (!a || !b || !out) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < CV_TOTAL && n < max_out; i++) {
        int16_t diff = (int16_t)b->A[i] - (int16_t)a->A[i];
        if (diff != 0) {
            out[n].index  = i;
            out[n].diff_A = diff;
            n++;
        }
    }
    return n;
}

uint32_t canvas_delta_rle_bytes(const CanvasDeltaEntry* entries, uint32_t count) {
    if (!entries || count == 0) return 0;
    /* RLE record: (start:u32, length:u16, diff:i16) = 8 bytes per run.
       A run = consecutive indices where diff_A is identical. */
    uint32_t runs = 1;
    uint32_t prev_idx  = entries[0].index;
    int16_t  prev_diff = entries[0].diff_A;
    for (uint32_t k = 1; k < count; k++) {
        if (entries[k].index == prev_idx + 1 && entries[k].diff_A == prev_diff) {
            /* extend current run */
        } else {
            runs++;
        }
        prev_idx  = entries[k].index;
        prev_diff = entries[k].diff_A;
    }
    return runs * 8;
}
