/*
 * demo_pipeline — run an input image through the full CE pipeline
 * and save both a plain and a mask-overlayed image so the result
 * is visually inspectable.
 *
 *   usage: demo_pipeline [--adapt] <input> [output_prefix]
 *
 *   Reads PNG / JPEG / BMP / TGA via vendored stb_image, and
 *   binary P6 PPM via a small built-in parser. The format is
 *   picked from the file extension.
 *
 *   Writes both PNG (via stb_image_write) and PPM for every output:
 *     <prefix>_plain.png  / <prefix>_plain.ppm
 *     <prefix>_masked.png / <prefix>_masked.ppm
 *
 *   When output_prefix is omitted, "demo_out" is used.
 */

#include "img_pipeline.h"
#include "img_render.h"
#include "img_delta_learn.h"
#include "img_delta_memory.h"
#include "stb_image.h"
#include "stb_image_write.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── format sniffing ────────────────────────────────────── */

static int ends_with_ci(const char* s, const char* suffix) {
    if (!s || !suffix) return 0;
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return 0;
    for (size_t i = 0; i < lf; i++) {
        int a = tolower((unsigned char)s[ls - lf + i]);
        int b = tolower((unsigned char)suffix[i]);
        if (a != b) return 0;
    }
    return 1;
}

/* ── PPM (P6) loader ─────────────────────────────────────── */

static uint8_t* load_ppm_p6(const char* path,
                            uint32_t* out_w, uint32_t* out_h) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }

    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) {
        fprintf(stderr, "%s: not a P6 PPM\n", path);
        fclose(f); return NULL;
    }

    /* Parse three whitespace-separated integers (w, h, maxval),
     * skipping '#' comment lines. */
    int vals[3];
    int got = 0;
    while (got < 3) {
        int c = fgetc(f);
        if (c == EOF) break;
        if (c == '#') {
            while ((c = fgetc(f)) != EOF && c != '\n') { /* skip */ }
            continue;
        }
        if (isspace(c)) continue;
        ungetc(c, f);
        if (fscanf(f, "%d", &vals[got]) != 1) break;
        got++;
    }
    if (got != 3) {
        fprintf(stderr, "%s: malformed header\n", path);
        fclose(f); return NULL;
    }

    const uint32_t w = (uint32_t)vals[0];
    const uint32_t h = (uint32_t)vals[1];
    const int maxval = vals[2];
    if (w == 0 || h == 0) {
        fprintf(stderr, "%s: zero dimension\n", path);
        fclose(f); return NULL;
    }
    if (maxval != 255) {
        fprintf(stderr, "%s: maxval=%d (only 255 supported)\n", path, maxval);
        fclose(f); return NULL;
    }

    /* Exactly one whitespace separator after maxval, per PPM spec. */
    fgetc(f);

    const size_t n = (size_t)w * h * 3u;
    uint8_t* buf = (uint8_t*)malloc(n);
    if (!buf) { fclose(f); return NULL; }
    const size_t got_bytes = fread(buf, 1, n, f);
    fclose(f);
    if (got_bytes != n) {
        fprintf(stderr, "%s: short read (%zu / %zu bytes)\n",
                path, got_bytes, n);
        free(buf); return NULL;
    }

    *out_w = w;
    *out_h = h;
    return buf;
}

/* ── stb-backed loader (PNG/JPEG/BMP/TGA) ────────────────── */

static uint8_t* load_with_stb(const char* path,
                              uint32_t* out_w, uint32_t* out_h) {
    int w = 0, h = 0, c = 0;
    stbi_uc* pixels = stbi_load(path, &w, &h, &c, /*desired_channels=*/3);
    if (!pixels) {
        fprintf(stderr, "%s: stb_image failed: %s\n",
                path, stbi_failure_reason());
        return NULL;
    }
    const size_t n = (size_t)w * (size_t)h * 3u;
    uint8_t* buf = (uint8_t*)malloc(n);
    if (!buf) { stbi_image_free(pixels); return NULL; }
    memcpy(buf, pixels, n);
    stbi_image_free(pixels);
    *out_w = (uint32_t)w;
    *out_h = (uint32_t)h;
    return buf;
}

/* ── dispatcher ──────────────────────────────────────────── */

static uint8_t* load_image(const char* path,
                           uint32_t* out_w, uint32_t* out_h) {
    if (ends_with_ci(path, ".ppm")) {
        return load_ppm_p6(path, out_w, out_h);
    }
    /* Everything else goes through stb — PNG, JPEG, BMP, TGA, PSD. */
    return load_with_stb(path, out_w, out_h);
}

/* ── PNG writer (via stb_image_write) ────────────────────── */

static int save_png(const char* path, const ImgRenderImage* img) {
    if (!path || !img || !img->rgb) return 0;
    const int stride = (int)(img->width * 3u);
    return stbi_write_png(path, (int)img->width, (int)img->height,
                          /*comp=*/3, img->rgb, stride) != 0;
}

/* ── main ────────────────────────────────────────────────── */

static void print_usage(const char* prog) {
    fprintf(stderr,
        "usage: %s [flags] <input> [output_prefix]\n"
        "\n"
        "   --adapt, -a\n"
        "       Run img_render_options_adapt_to_ce — per-channel tier\n"
        "       thresholds are re-derived from this image's CE histogram\n"
        "       before rendering.\n"
        "\n"
        "   --learn <before> <after>\n"
        "       Populate a DeltaMemory from the before/after image pair\n"
        "       (CE-grid differences drive img_delta_learn). The pipeline\n"
        "       then runs with that memory on <input>, producing non-zero\n"
        "       expansions when the input's cells match the learned keys.\n"
        "\n"
        "   Reads PNG / JPEG / BMP / TGA (stb_image) or P6 PPM.\n"
        "\n"
        "   Outputs both .ppm and .png for each:\n"
        "     <prefix>_plain   plain CE render\n"
        "     <prefix>_masked  CE render + resolve mask overlay\n",
        prog);
}

int main(int argc, char** argv) {
    int adapt = 0;
    const char* learn_before = NULL;
    const char* learn_after  = NULL;

    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--adapt") == 0 ||
            strcmp(argv[argi], "-a")      == 0) {
            adapt = 1;
            argi++;
        } else if (strcmp(argv[argi], "--learn") == 0) {
            if (argi + 2 >= argc) {
                fprintf(stderr, "--learn needs <before> <after>\n");
                print_usage(argv[0]);
                return 2;
            }
            learn_before = argv[argi + 1];
            learn_after  = argv[argi + 2];
            argi += 3;
        } else if (strcmp(argv[argi], "--help") == 0 ||
                   strcmp(argv[argi], "-h")     == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown flag: %s\n", argv[argi]);
            print_usage(argv[0]);
            return 2;
        }
    }
    if (argi >= argc) { print_usage(argv[0]); return 2; }

    const char* input  = argv[argi];
    const char* prefix = (argi + 1 < argc) ? argv[argi + 1] : "demo_out";

    uint32_t w = 0, h = 0;
    uint8_t* img = load_image(input, &w, &h);
    if (!img) return 1;

    /* Optionally learn a DeltaMemory from a before/after pair. */
    ImgDeltaMemory* memory = NULL;
    if (learn_before && learn_after) {
        uint32_t bw = 0, bh = 0, aw = 0, ah = 0;
        uint8_t* b = load_image(learn_before, &bw, &bh);
        uint8_t* a = load_image(learn_after,  &aw, &ah);
        if (!b || !a) {
            free(b); free(a); free(img);
            return 1;
        }
        memory = img_delta_memory_create();
        uint32_t added = img_delta_memory_learn_from_images(
            memory, b, bw, bh, a, aw, ah);
        printf("=== learn ===\n");
        printf("  before: %s (%u x %u)\n", learn_before, bw, bh);
        printf("  after:  %s (%u x %u)\n", learn_after,  aw, ah);
        printf("  deltas added: %u  (memory.count = %u)\n",
               added, img_delta_memory_count(memory));
        free(b);
        free(a);
    }

    ImgPipelineResult r = {0};
    if (!img_pipeline_run(img, w, h, memory, /*opt=*/NULL, &r)) {
        fprintf(stderr, "pipeline failed\n");
        if (memory) img_delta_memory_destroy(memory);
        free(img); return 1;
    }

    printf("=== demo_pipeline ===\n");
    printf("  input:              %s (%u x %u)\n", input, w, h);
    printf("  seed_count:         %u\n", r.stats.seed_count);
    printf("  expansions:         %u\n", r.stats.expansions);
    printf("  visited:            %u\n", r.stats.visited);
    printf("  resolve_outliers:   %u\n", r.stats.resolve_outliers);
    printf("  resolve_explained:  %u\n", r.stats.resolve_explained);
    printf("  resolve_promoted:   %u\n", r.stats.resolve_promoted);
    printf("  feedback_success:   %u\n", r.stats.feedback_success);
    printf("  feedback_failure:   %u\n", r.stats.feedback_failure);

    char path[1024];
    int ok_plain = 0, ok_masked = 0;

    ImgRenderOptions ropt = img_render_default_options();
    if (adapt) {
        img_render_options_adapt_to_ce(&ropt, r.ce_grid);
        printf("  adapted tier.core:    {%u, %u, %u}\n",
               ropt.tier_core.t1_max, ropt.tier_core.t2_max,
               ropt.tier_core.t3_max);
        printf("  adapted tier.link:    {%u, %u, %u}\n",
               ropt.tier_link.t1_max, ropt.tier_link.t2_max,
               ropt.tier_link.t3_max);
        printf("  adapted tier.delta:   {%u, %u, %u}\n",
               ropt.tier_delta.t1_max, ropt.tier_delta.t2_max,
               ropt.tier_delta.t3_max);
        printf("  adapted tier.priority:{%u, %u, %u}\n",
               ropt.tier_priority.t1_max, ropt.tier_priority.t2_max,
               ropt.tier_priority.t3_max);
    }

    ImgRenderImage plain = {0};
    if (img_render_ce_grid(r.ce_grid, &ropt, &plain)) {
        snprintf(path, sizeof(path), "%s_plain.ppm", prefix);
        int ppm_ok = img_render_save_ppm(path, &plain);
        if (ppm_ok) printf("  wrote %s  (%u x %u)\n", path,
                           plain.width, plain.height);

        snprintf(path, sizeof(path), "%s_plain.png", prefix);
        int png_ok = save_png(path, &plain);
        if (png_ok) printf("  wrote %s  (%u x %u)\n", path,
                           plain.width, plain.height);

        ok_plain = ppm_ok && png_ok;
        img_render_free_image(&plain);
    }

    ImgRenderImage masked = {0};
    ImgRenderMasks masks = { r.outlier_mask, r.explained_mask };
    if (img_render_ce_grid_masked(r.ce_grid, &ropt, &masks, &masked)) {
        snprintf(path, sizeof(path), "%s_masked.ppm", prefix);
        int ppm_ok = img_render_save_ppm(path, &masked);
        if (ppm_ok) printf("  wrote %s (%u x %u)\n", path,
                           masked.width, masked.height);

        snprintf(path, sizeof(path), "%s_masked.png", prefix);
        int png_ok = save_png(path, &masked);
        if (png_ok) printf("  wrote %s (%u x %u)\n", path,
                           masked.width, masked.height);

        ok_masked = ppm_ok && png_ok;
        img_render_free_image(&masked);
    }

    img_pipeline_result_destroy(&r);
    if (memory) img_delta_memory_destroy(memory);
    free(img);
    return (ok_plain && ok_masked) ? 0 : 1;
}
