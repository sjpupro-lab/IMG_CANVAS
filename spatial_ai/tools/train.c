/*
 * train — batch training CLI.
 *
 *   ./train [options] <manifest.tsv>
 *
 *     --model <path>      output SpatialAI file (default out.spai)
 *     --memory <path>     output DeltaMemory file (default out.imem)
 *     --resume            if the outputs already exist, load them and
 *                         keep accumulating; otherwise start fresh
 *     --quiet             suppress per-row progress
 *
 *   Manifest format (tab-separated, one row per learning example):
 *     <text_label>\t<before_image>\t<after_image>
 *
 *   '#' at the start of a line = comment. Empty lines are skipped.
 *
 *   For each row:
 *     1. load both images (PNG/JPEG/BMP via stb_image, or raw PPM)
 *     2. img_delta_memory_learn_from_images(memory, before, after)
 *     3. force a text keyframe with <text_label>
 *     4. bind the `after` image as the keyframe's ce_snapshot (runs
 *        the full image pipeline with the accumulated memory so
 *        newly-learned deltas immediately participate in feedback)
 *
 *   At the end the SpatialAI and the DeltaMemory are written to
 *   their respective output paths and a summary is printed.
 */

#include "img_delta_learn.h"
#include "img_delta_memory.h"
#include "img_noise_memory.h"
#include "spatial_bimodal.h"
#include "spatial_io.h"
#include "spatial_keyframe.h"
#include "stb_image.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── image loader (PPM P6 + stb_image for everything else) ── */

static uint8_t* load_ppm_p6(const char* path,
                            uint32_t* out_w, uint32_t* out_h) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) {
        fclose(f); return NULL;
    }
    int vals[3]; int got = 0;
    while (got < 3) {
        int c = fgetc(f);
        if (c == EOF) break;
        if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n') {} continue; }
        if (isspace(c)) continue;
        ungetc(c, f);
        if (fscanf(f, "%d", &vals[got]) != 1) break;
        got++;
    }
    if (got != 3 || vals[2] != 255) { fclose(f); return NULL; }
    fgetc(f);
    const size_t n = (size_t)vals[0] * vals[1] * 3u;
    uint8_t* buf = (uint8_t*)malloc(n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, n, f) != n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_w = (uint32_t)vals[0];
    *out_h = (uint32_t)vals[1];
    return buf;
}

static int ends_with_ci(const char* s, const char* suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return 0;
    for (size_t i = 0; i < lf; i++) {
        if (tolower((unsigned char)s[ls - lf + i]) !=
            tolower((unsigned char)suffix[i])) return 0;
    }
    return 1;
}

static uint8_t* load_image(const char* path,
                           uint32_t* out_w, uint32_t* out_h) {
    if (ends_with_ci(path, ".ppm")) return load_ppm_p6(path, out_w, out_h);
    int w = 0, h = 0, c = 0;
    stbi_uc* pixels = stbi_load(path, &w, &h, &c, /*desired=*/3);
    if (!pixels) {
        fprintf(stderr, "  [load] %s: %s\n", path, stbi_failure_reason());
        return NULL;
    }
    const size_t n = (size_t)w * h * 3;
    uint8_t* buf = (uint8_t*)malloc(n);
    if (buf) memcpy(buf, pixels, n);
    stbi_image_free(pixels);
    *out_w = (uint32_t)w;
    *out_h = (uint32_t)h;
    return buf;
}

/* ── manifest parser: strip \r\n, skip '#' + blank, split on tabs ── */

typedef struct {
    char label[256];
    char before[512];
    char after[512];
} ManifestRow;

static int parse_line(char* line, ManifestRow* out) {
    /* strip trailing \r\n */
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == '\r' || line[n-1] == '\n')) line[--n] = '\0';

    /* skip leading whitespace */
    char* p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0' || *p == '#') return 0;

    char* fields[3] = {0};
    int fi = 0;
    fields[fi++] = p;
    while (*p && fi < 3) {
        if (*p == '\t') {
            *p = '\0';
            fields[fi++] = p + 1;
        }
        p++;
    }
    if (fi != 3) return 0;

    snprintf(out->label,  sizeof(out->label),  "%s", fields[0]);
    snprintf(out->before, sizeof(out->before), "%s", fields[1]);
    snprintf(out->after,  sizeof(out->after),  "%s", fields[2]);
    return 1;
}

/* ── main ────────────────────────────────────────────────── */

typedef struct {
    const char* model_path;
    const char* memory_path;
    const char* nmem_path;
    const char* manifest_path;
    int quiet;
    int resume;
} Args;

static void print_usage(const char* prog) {
    fprintf(stderr,
        "usage: %s [options] <manifest.tsv>\n"
        "\n"
        "  --model <path>    output SpatialAI file (default out.spai)\n"
        "  --memory <path>   output DeltaMemory file (default out.imem)\n"
        "  --nmem <path>     output NoiseMemory file (optional; learned\n"
        "                    spatial prior for deterministic drawing seed)\n"
        "  --resume          load existing outputs and keep accumulating\n"
        "  --quiet           suppress per-row progress\n"
        "\n"
        "Manifest TSV rows: <label>\\t<before>\\t<after>\n",
        prog);
}

static int parse_args(int argc, char** argv, Args* out) {
    out->model_path   = "out.spai";
    out->memory_path  = "out.imem";
    out->nmem_path    = NULL;
    out->manifest_path = NULL;
    out->quiet = 0;
    out->resume = 0;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--model") == 0 && i + 1 < argc) {
            out->model_path = argv[++i];
        } else if (strcmp(a, "--memory") == 0 && i + 1 < argc) {
            out->memory_path = argv[++i];
        } else if (strcmp(a, "--nmem") == 0 && i + 1 < argc) {
            out->nmem_path = argv[++i];
        } else if (strcmp(a, "--quiet") == 0) {
            out->quiet = 1;
        } else if (strcmp(a, "--resume") == 0) {
            out->resume = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(argv[0]); return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "unknown flag: %s\n", a);
            return 0;
        } else {
            out->manifest_path = a;
        }
    }
    return out->manifest_path != NULL;
}

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, &args)) { print_usage(argv[0]); return 2; }

    /* Load or create outputs. */
    SpatialAI* ai = NULL;
    ImgDeltaMemory* mem = NULL;
    ImgNoiseMemory nmem;
    int nmem_ready = 0;

    if (args.resume) {
        SpaiStatus ss = SPAI_OK;
        ai = ai_load(args.model_path, &ss);
        if (ai) {
            printf("  [resume] loaded model %s (kf=%u df=%u)\n",
                   args.model_path, ai->kf_count, ai->df_count);
        }

        ImemStatus ms = IMEM_OK;
        mem = img_delta_memory_load(args.memory_path, &ms);
        if (mem) {
            printf("  [resume] loaded memory %s (units=%u)\n",
                   args.memory_path, img_delta_memory_count(mem));
        }
    }
    if (!ai)  ai  = spatial_ai_create();
    if (!mem) mem = img_delta_memory_create();
    if (!ai || !mem) {
        fprintf(stderr, "alloc failed\n");
        if (ai)  spatial_ai_destroy(ai);
        if (mem) img_delta_memory_destroy(mem);
        return 1;
    }

    if (args.nmem_path) {
        if (!img_noise_memory_init(&nmem)) {
            fprintf(stderr, "nmem init failed\n");
            spatial_ai_destroy(ai); img_delta_memory_destroy(mem);
            return 1;
        }
        nmem_ready = 1;
        if (args.resume) {
            if (img_noise_memory_load(&nmem, args.nmem_path)) {
                printf("  [resume] loaded nmem %s\n", args.nmem_path);
            }
        }
    }

    /* Walk the manifest. */
    FILE* f = fopen(args.manifest_path, "r");
    if (!f) { perror(args.manifest_path);
              spatial_ai_destroy(ai);
              img_delta_memory_destroy(mem);
              return 1; }

    uint32_t rows_total = 0, rows_ok = 0;
    uint32_t deltas_added_total = 0;
    uint32_t units_before = img_delta_memory_count(mem);

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        ManifestRow row;
        if (!parse_line(line, &row)) continue;
        rows_total++;

        uint32_t bw = 0, bh = 0, aw = 0, ah = 0;
        uint8_t* before = load_image(row.before, &bw, &bh);
        uint8_t* after  = load_image(row.after,  &aw, &ah);
        if (!before || !after) {
            fprintf(stderr, "  [skip row %u] image load failed\n", rows_total);
            free(before); free(after);
            continue;
        }

        uint32_t before_count = img_delta_memory_count(mem);
        uint32_t added = img_delta_memory_learn_from_images(
            mem, before, bw, bh, after, aw, ah);
        deltas_added_total += added;

        uint32_t kf = ai_force_keyframe(ai, row.label, row.label);
        int bound = ai_bind_image_to_kf(ai, kf, after, aw, ah, mem);

        /* Fold the freshly-bound CE snapshot into the noise memory,
         * which captures "what usually lives where" so drawing from
         * an empty canvas starts from a learned spatial prior instead
         * of a flat fallback context. */
        int nmem_observed = 0;
        if (nmem_ready && bound) {
            const ImgCEGrid* snap = ai_get_ce_snapshot(ai, kf);
            if (snap) {
                if (img_noise_memory_observe(&nmem, snap, row.label)) {
                    nmem_observed = 1;
                }
            }
        }

        if (!args.quiet) {
            printf("  [%u] %-40.40s  +%4u deltas  kf=%u  ce=%s  nmem=%s\n",
                   rows_total, row.label, added, kf,
                   bound ? "yes" : "no",
                   nmem_ready ? (nmem_observed ? "yes" : "no") : "off");
        }

        free(before);
        free(after);
        rows_ok++;
        (void)before_count;
    }
    fclose(f);

    /* Save outputs. */
    SpaiStatus ss = ai_save(ai, args.model_path);
    if (ss != SPAI_OK) {
        fprintf(stderr, "model save failed: %s\n", spai_status_str(ss));
    }
    ImemStatus ms = img_delta_memory_save(mem, args.memory_path);
    if (ms != IMEM_OK) {
        fprintf(stderr, "memory save failed: %s\n",
                img_delta_memory_status_str(ms));
    }
    if (nmem_ready) {
        if (!img_noise_memory_save(&nmem, args.nmem_path)) {
            fprintf(stderr, "nmem save failed: %s\n", args.nmem_path);
        }
    }

    /* Summary. */
    uint32_t units_after = img_delta_memory_count(mem);
    uint32_t rarity_counts[5] = {0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < units_after; i++) {
        const ImgDeltaUnit* u = img_delta_memory_get(mem, i);
        if (u->weight >= 4000u) rarity_counts[4]++;
        else if (u->weight >= 2000u) rarity_counts[3]++;
        else if (u->weight >= 1500u) rarity_counts[2]++;
        else if (u->weight >= 1000u) rarity_counts[1]++;
        else                          rarity_counts[0]++;
    }

    printf("\n=== train summary ===\n");
    printf("  manifest rows:    %u (%u ok)\n", rows_total, rows_ok);
    printf("  deltas added:     %u\n", deltas_added_total);
    printf("  units (before):   %u\n", units_before);
    printf("  units (after):    %u\n", units_after);
    printf("  weight buckets:\n");
    printf("    <1000  (edge):  %u\n", rarity_counts[0]);
    printf("    = 1000 (base):  %u\n", rarity_counts[1]);
    printf("    1500-1999    :  %u\n", rarity_counts[2]);
    printf("    2000-3999    :  %u\n", rarity_counts[3]);
    printf("    >=4000 (rare):  %u\n", rarity_counts[4]);
    printf("  keyframes:        %u\n", ai->kf_count);
    printf("  ce snapshots:     %u\n", ai_ce_snapshot_count(ai));
    printf("  model:            %s\n", args.model_path);
    printf("  memory:           %s\n", args.memory_path);
    if (nmem_ready) {
        printf("  nmem:             %s  (observations=%u)\n",
               args.nmem_path, nmem.observe_count);
    }

    spatial_ai_destroy(ai);
    img_delta_memory_destroy(mem);
    if (nmem_ready) img_noise_memory_free(&nmem);
    return 0;
}
