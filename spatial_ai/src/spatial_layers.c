#include "spatial_layers.h"
#include <string.h>

LayerBitmaps* layers_create(void) {
    LayerBitmaps* lb = (LayerBitmaps*)malloc(sizeof(LayerBitmaps));
    if (!lb) return NULL;
    memset(lb, 0, sizeof(LayerBitmaps));
    return lb;
}

void layers_destroy(LayerBitmaps* lb) {
    free(lb);
}

/* Encode raw UTF-8 bytes into a 1D layer array with given weight */
static void layer_encode_bytes(const uint8_t* bytes, uint32_t len,
                               uint16_t* layer, uint16_t weight) {
    for (uint32_t i = 0; i < len; i++) {
        uint32_t x = bytes[i];
        uint32_t y = i % GRID_SIZE;
        uint32_t idx = y * GRID_SIZE + x;
        uint32_t new_val = (uint32_t)layer[idx] + weight;
        layer[idx] = (new_val > 65535) ? 65535 : (uint16_t)new_val;
    }
}

/* Split text by spaces and encode each word (with its position offset) */
static void layer_encode_words(const char* text, uint16_t* layer, uint16_t weight) {
    const uint8_t* bytes = (const uint8_t*)text;
    uint32_t total_len = (uint32_t)strlen(text);
    uint32_t pos = 0;

    while (pos < total_len) {
        /* Skip spaces */
        while (pos < total_len && bytes[pos] == ' ') pos++;
        if (pos >= total_len) break;

        /* Find word end */
        uint32_t word_start = pos;
        while (pos < total_len && bytes[pos] != ' ') pos++;

        /* Encode word bytes at their original positions */
        for (uint32_t i = word_start; i < pos; i++) {
            uint32_t x = bytes[i];
            uint32_t y = i % GRID_SIZE;
            uint32_t idx = y * GRID_SIZE + x;
            uint32_t new_val = (uint32_t)layer[idx] + weight;
            layer[idx] = (new_val > 65535) ? 65535 : (uint16_t)new_val;
        }
    }
}

/* Encode morphemes: analyze each word, then encode morpheme tokens
   at their original byte positions */
static void layer_encode_morphemes(const char* text, uint16_t* layer, uint16_t weight) {
    Morpheme morphs[128];
    uint32_t n = morpheme_tokenize_clause(text, morphs, 128);

    /* For morpheme layer, we re-encode the original bytes at original positions.
       The morpheme analysis confirms which bytes belong to content morphemes.
       Since we encode at original byte positions, we just re-encode the full
       text bytes for tokens that are content (noun, verb, adj, morpheme parts). */
    /* Simplified: encode all non-space bytes from the clause, matching base layer
       coverage. The morpheme layer captures the same positions with +1 weight. */
    const uint8_t* bytes = (const uint8_t*)text;
    uint32_t total_len = (uint32_t)strlen(text);

    /* Track byte position in original text per morpheme */
    uint32_t byte_pos = 0;
    for (uint32_t m = 0; m < n; m++) {
        uint32_t tlen = (uint32_t)strlen(morphs[m].token);
        /* Find this token in text starting from byte_pos */
        const char* found = strstr(text + byte_pos, morphs[m].token);
        if (found) {
            uint32_t start = (uint32_t)(found - text);
            for (uint32_t i = 0; i < tlen; i++) {
                uint32_t bi = start + i;
                if (bi >= total_len) break;
                uint32_t x = bytes[bi];
                uint32_t y = bi % GRID_SIZE;
                uint32_t idx = y * GRID_SIZE + x;
                uint32_t new_val = (uint32_t)layer[idx] + weight;
                layer[idx] = (new_val > 65535) ? 65535 : (uint16_t)new_val;
            }
            byte_pos = start + tlen;
        }
    }
    (void)byte_pos;
}

/* Seed B channel with a co-occurrence hash of the clause's unique active
 * byte values. Every A>0 cell in the clause receives the same hash h, so
 * two clauses with the same vocabulary get identical B fingerprints.
 *
 * This survives update_rgb_directional's horizontal diffusion because
 * all active neighbors share h (intra-clause diff is zero), while
 * different clauses compare at the B-channel level by vocabulary overlap.
 */
static void seed_cooccurrence_b(const char* text, SpatialGrid* grid) {
    if (!text || !grid) return;

    /* 1. Collect unique active X values (= unique byte values) */
    uint8_t seen[256] = {0};
    const uint8_t* bytes = (const uint8_t*)text;
    for (uint32_t i = 0; bytes[i]; i++) seen[bytes[i]] = 1;

    /* 2. Hash: iterate X = 0..255 in ascending order for determinism */
    uint8_t h = 0;
    for (int x = 0; x < 256; x++) {
        if (seen[x]) h = (uint8_t)(h * 31u + (uint32_t)x);
    }

    /* 3. Paint h on every A>0 cell. Skip zero (leave inactive cells at 0). */
    if (h == 0) h = 1;  /* avoid collision with "inactive" sentinel */
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        if (grid->A[i] > 0) grid->B[i] = h;
    }
}

void layers_encode_clause(const char* clause_text,
                          LayerBitmaps* out_layers,
                          SpatialGrid* out_combined) {
    if (!clause_text || !out_combined) return;

    LayerBitmaps local_layers;
    LayerBitmaps* lb = out_layers ? out_layers : &local_layers;
    memset(lb, 0, sizeof(LayerBitmaps));

    const uint8_t* bytes = (const uint8_t*)clause_text;
    uint32_t len = (uint32_t)strlen(clause_text);

    /* Layer 1: Base layer — all bytes, weight +1 */
    layer_encode_bytes(bytes, len, lb->base, 1);

    /* Layer 2: Word layer — space-separated words, weight +2 */
    layer_encode_words(clause_text, lb->word, 2);

    /* Layer 3: Morpheme layer — morpheme tokens, weight +1 */
    layer_encode_morphemes(clause_text, lb->morpheme, 1);

    /* Sum into combined grid: A = base + word + morpheme */
    grid_clear(out_combined);
    for (uint32_t i = 0; i < GRID_TOTAL; i++) {
        uint32_t sum = (uint32_t)lb->base[i] + lb->word[i] + lb->morpheme[i];
        out_combined->A[i] = (sum > 65535) ? 65535 : (uint16_t)sum;
    }

    /* Seed B with the clause's co-occurrence hash BEFORE RGB diffusion.
     * Callers who subsequently run update_rgb_directional will still see
     * this fingerprint preserved (same-h neighbors diffuse with zero diff).
     */
    seed_cooccurrence_b(clause_text, out_combined);
}
