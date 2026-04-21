/*
 * stream_train.c — streaming training for SPATIAL-PATTERN-AI
 *
 * Reads a large text corpus line-by-line (one clause per line, as in
 * wiki-extractor output) and calls ai_store_auto() per clause without
 * ever holding the full file in memory.
 *
 * Usage:
 *   ./build/stream_train --input <path>
 *                        [--max <N>]          (default 50000)
 *                        [--save <path>]      (default build/models/stream_auto.spai)
 *                        [--checkpoint <N>]   (default 5000, 0 disables)
 *                        [--verbose]          (per-clause progress line)
 *                        [--verify]           (unseen-query sanity pass)
 *
 * The binary itself uses ~4 KB of line buffer + whatever the SpatialAI
 * keyframe/delta store accumulates — no full-file buffering.
 */

/* Expose fseeko/ftello via the POSIX.1-2001 + large-file feature macros.
 * Must precede any libc header. MinGW ignores them; glibc needs them
 * to prototype fseeko/ftello. */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200112L
#endif
#ifndef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS 64
#endif

#include "spatial_grid.h"
#include "spatial_layers.h"
#include "spatial_morpheme.h"
#include "spatial_match.h"
#include "spatial_keyframe.h"
#include "spatial_canvas.h"
#include "spatial_subtitle.h"
#include "spatial_clock.h"
#include "spatial_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0755)
#endif

#define LINE_BUF        4096
#define MIN_CLAUSE_LEN  10
#define DEFAULT_MAX     50000
#define DEFAULT_CKPT    5000
#define VERIFY_PROBE    500

/* ── CLI ──────────────────────────────────────────────────── */

typedef struct {
    const char* input;
    const char* save;
    const char* event_log;       /* --log <path>; NULL = disabled */
    uint32_t    max_clauses;
    uint32_t    checkpoint;
    uint32_t    max_line_bytes;  /* long-line auto-split soft cap; 0 = disabled */
    int         verbose;
    int         verify;
    float       threshold;       /* <0 means "leave engine default" */
    float       target_delta;    /* <0 means "disabled" (explicit threshold wins) */
    uint32_t    calibrate_samples;
    int         no_recluster;    /* 1 = skip post-training recluster pass */
    float       cluster_threshold; /* <0 means "auto-calibrate per DataType" */
    float       cluster_target_merge;     /* target ratio for KF-level auto-calibrate */
    float       canvas_cluster_threshold; /* <0 means "auto-calibrate" */
    float       canvas_target_merge;      /* target ratio for auto-calibrate */
    uint64_t    clock_g_threshold;        /* RGBA engine G-channel SAD */
    uint64_t    clock_rb_threshold;       /* RGBA engine R+B combined SAD */
} StreamArgs;

static void usage(const char* prog) {
    fprintf(stderr,
        "usage: %s --input <path> [--max N] [--save path] [--checkpoint N]\n"
        "       [--log path] [--threshold F | --target-delta R] [--verbose] [--verify]\n"
        "\n"
        "  --input <path>        input text file, one clause per line (required)\n"
        "  --max <N>             max clauses to ingest (default %d)\n"
        "  --save <path>         final model path (default build/models/stream_auto.spai)\n"
        "  --checkpoint <N>      save every N clauses (default %d, 0 disables)\n"
        "  --max-line-bytes <N>  auto-split lines longer than N bytes at the\n"
        "                        nearest sentence boundary ('.', '!', '?') or\n"
        "                        whitespace; 0 disables, default 256 (matches\n"
        "                        the grid Y axis so encoding doesn't wrap)\n"
        "  --log <path>          emit a binary training-event log consumed by\n"
        "                        tools/animate_training.py (clause-level cell\n"
        "                        events for the twinkling-grid visualization)\n"
        "  --threshold <F>       delta decision threshold in [0, 1]\n"
        "                        (default 0.30; try 0.15 on wiki-style corpora)\n"
        "  --target-delta <R>    auto-calibrate threshold so about R (0..1) of the\n"
        "                        first --calibrate-samples clauses would become\n"
        "                        deltas. Ignored when --threshold is given explicitly.\n"
        "  --calibrate-samples <N>\n"
        "                        clauses used for --target-delta calibration\n"
        "                        (default 500; capped at --max)\n"
        "  --verbose             per-clause progress line\n"
        "  --verify              after training, run an unseen-query sanity pass\n"
        "  --no-recluster        skip the post-training keyframe re-clustering pass\n"
        "  --cluster-threshold <F>\n"
        "                        cosine threshold for grouping KFs during recluster\n"
        "                        (default: auto-calibrate per DataType from the\n"
        "                        post-training KF-vs-KF cos_A distribution)\n"
        "  --cluster-target-merge <R>\n"
        "                        target merge ratio for KF-level auto-calibrate in\n"
        "                        (0..1). 0.3 strict (few merges, more KFs remain),\n"
        "                        0.5 median, 0.8 aggressive (few KFs, lots of\n"
        "                        deltas). Ignored if --cluster-threshold is given.\n"
        "                        (default 0.5)\n"
        "  --canvas-cluster-threshold <F>\n"
        "                        block-sum cosine threshold for canvas pool\n"
        "                        recluster (default: auto-calibrate from data)\n"
        "  --canvas-target-merge <R>\n"
        "                        target merge ratio for canvas auto-calibrate in\n"
        "                        (0..1). 0.3 conservative, 0.5 median, 0.8\n"
        "                        aggressive. (default 0.5; ignored if\n"
        "                        --canvas-cluster-threshold is given)\n"
        "  --freq-tag-g-threshold <N>\n"
        "                        RGBA clock engine G-channel SAD threshold. New\n"
        "                        chapter when the engine's G state drifts by this\n"
        "                        much since chapter start. (default 250 → avg ~4\n"
        "                        chapters/canvas on wiki5k)\n"
        "  --freq-tag-rb-threshold <N>\n"
        "                        RGBA clock engine R+B combined SAD threshold.\n"
        "                        Catches context/structure breaks G alone misses.\n"
        "                        (default 150)\n",
        prog, DEFAULT_MAX, DEFAULT_CKPT);
}

static int parse_args(int argc, char** argv, StreamArgs* a) {
    a->input             = NULL;
    a->save              = "build/models/stream_auto.spai";
    a->event_log         = NULL;
    a->max_clauses       = DEFAULT_MAX;
    a->checkpoint        = DEFAULT_CKPT;
    a->max_line_bytes    = GRID_SIZE;  /* 256 — matches the grid Y axis */
    a->verbose           = 0;
    a->verify            = 0;
    a->threshold         = -1.0f;
    a->target_delta      = -1.0f;
    a->calibrate_samples = 500;
    a->no_recluster             = 0;
    a->cluster_threshold        = -1.0f;
    a->cluster_target_merge     = 0.5f;
    a->canvas_cluster_threshold = -1.0f;
    a->canvas_target_merge      = 0.5f;
    a->clock_g_threshold        = 250u;  /* wiki5k tuned → avg ~4 chapters */
    a->clock_rb_threshold       = 150u;

    for (int i = 1; i < argc; i++) {
        const char* k = argv[i];
        if (strcmp(k, "--input") == 0 && i + 1 < argc) {
            a->input = argv[++i];
        } else if (strcmp(k, "--max") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v < 0) v = 0;
            a->max_clauses = (uint32_t)v;
        } else if (strcmp(k, "--save") == 0 && i + 1 < argc) {
            a->save = argv[++i];
        } else if (strcmp(k, "--checkpoint") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v < 0) v = 0;
            a->checkpoint = (uint32_t)v;
        } else if (strcmp(k, "--max-line-bytes") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v < 0) v = 0;                 /* 0 = disable splitting */
            if (v > 65535) v = 65535;
            a->max_line_bytes = (uint32_t)v;
        } else if (strcmp(k, "--log") == 0 && i + 1 < argc) {
            a->event_log = argv[++i];
        } else if (strcmp(k, "--threshold") == 0 && i + 1 < argc) {
            a->threshold = strtof(argv[++i], NULL);
        } else if (strcmp(k, "--target-delta") == 0 && i + 1 < argc) {
            a->target_delta = strtof(argv[++i], NULL);
            if (a->target_delta < 0.0f) a->target_delta = 0.0f;
            if (a->target_delta > 1.0f) a->target_delta = 1.0f;
        } else if (strcmp(k, "--calibrate-samples") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v < 10) v = 10;
            a->calibrate_samples = (uint32_t)v;
        } else if (strcmp(k, "--verbose") == 0) {
            a->verbose = 1;
        } else if (strcmp(k, "--verify") == 0) {
            a->verify = 1;
        } else if (strcmp(k, "--no-recluster") == 0) {
            a->no_recluster = 1;
        } else if (strcmp(k, "--cluster-threshold") == 0 && i + 1 < argc) {
            a->cluster_threshold = strtof(argv[++i], NULL);
            if (a->cluster_threshold < 0.0f) a->cluster_threshold = 0.0f;
            if (a->cluster_threshold > 1.0f) a->cluster_threshold = 1.0f;
        } else if (strcmp(k, "--cluster-target-merge") == 0 && i + 1 < argc) {
            a->cluster_target_merge = strtof(argv[++i], NULL);
            if (a->cluster_target_merge < 0.0f) a->cluster_target_merge = 0.0f;
            if (a->cluster_target_merge > 1.0f) a->cluster_target_merge = 1.0f;
        } else if (strcmp(k, "--canvas-cluster-threshold") == 0 && i + 1 < argc) {
            a->canvas_cluster_threshold = strtof(argv[++i], NULL);
            if (a->canvas_cluster_threshold < 0.0f) a->canvas_cluster_threshold = 0.0f;
            if (a->canvas_cluster_threshold > 1.0f) a->canvas_cluster_threshold = 1.0f;
        } else if (strcmp(k, "--canvas-target-merge") == 0 && i + 1 < argc) {
            a->canvas_target_merge = strtof(argv[++i], NULL);
            if (a->canvas_target_merge < 0.0f) a->canvas_target_merge = 0.0f;
            if (a->canvas_target_merge > 1.0f) a->canvas_target_merge = 1.0f;
        } else if (strcmp(k, "--freq-tag-g-threshold") == 0 && i + 1 < argc) {
            long long v = strtoll(argv[++i], NULL, 10);
            if (v < 0) v = 0;
            a->clock_g_threshold = (uint64_t)v;
        } else if (strcmp(k, "--freq-tag-rb-threshold") == 0 && i + 1 < argc) {
            long long v = strtoll(argv[++i], NULL, 10);
            if (v < 0) v = 0;
            a->clock_rb_threshold = (uint64_t)v;
        } else {
            fprintf(stderr, "unknown arg: %s\n", k);
            return -1;
        }
    }
    if (!a->input) return -1;
    return 0;
}

/* ── helpers ─────────────────────────────────────────────── */

static double now_sec(void) {
    /* clock() gives CPU time, which for this single-threaded I/O-bound
     * trainer tracks wall time closely enough for progress logging.
     * Avoids timespec_get / clock_gettime portability issues across
     * mingw-w64 runtime versions. */
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

/* Strip trailing whitespace in place. */
static void strip_trailing(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t')) {
        s[--n] = '\0';
    }
}

/* Skip obvious non-content lines (empty, XML/HTML-ish). */
static int is_skippable(const char* s) {
    if (s[0] == '\0') return 1;
    if (s[0] == '<')  return 1;            /* <doc>, </doc>, ... */
    if (s[0] == '#')  return 1;            /* comment / markdown */
    return 0;
}

/* Ensure the parent directory of `path` exists. Silently tolerates
 * existing directories. Path must be writable. */
static void ensure_parent_dir(const char* path) {
    if (!path) return;
    char buf[1024];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return;
    memcpy(buf, path, n + 1);

    /* Walk each separator and mkdir on the way. */
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char c = buf[i];
            buf[i] = '\0';
            MKDIR(buf);
            buf[i] = c;
        }
    }
}

static long long file_size_or_zero(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return 0;
    /* 64-bit seek: a populated canvas pool pushes the .spai past 2 GB
     * (155 canvases × 10.5 MB each), which overflows signed long on
     * Windows where long is 32-bit. */
#ifdef _WIN32
    _fseeki64(fp, 0, SEEK_END);
    long long n = _ftelli64(fp);
#else
    fseeko(fp, 0, SEEK_END);
    long long n = (long long)ftello(fp);
#endif
    fclose(fp);
    return n < 0 ? 0 : n;
}

/* ── verify: unseen-query pass over the last batch from disk ── */

typedef struct {
    uint32_t clauses_scanned;
    uint32_t clauses_matched;
    double   sum_sim;
    double   min_sim;
    double   max_sim;
    uint32_t hits_90;
    uint32_t hits_50;
    uint32_t hits_10;
} VerifyStats;

/* Re-scan the corpus, match each clause against the trained engine,
 * report similarity distribution. Uses ai_predict which also runs the
 * full encode + RGB diffusion path. */
static void verify_run(SpatialAI* ai, const char* input_path,
                       uint32_t probe_limit, VerifyStats* out) {
    memset(out, 0, sizeof(*out));
    out->min_sim = 1.0;
    out->max_sim = 0.0;

    FILE* fp = fopen(input_path, "r");
    if (!fp) {
        printf("[verify] cannot reopen %s\n", input_path);
        return;
    }

    char line[LINE_BUF];
    uint32_t seen = 0;
    while (fgets(line, sizeof(line), fp)) {
        strip_trailing(line);
        if (is_skippable(line)) continue;
        if (strlen(line) < MIN_CLAUSE_LEN) continue;
        seen++;
    }

    /* Probe the last `probe_limit` clauses (treated as "unseen tail"). */
    uint32_t start = (seen > probe_limit) ? (seen - probe_limit) : 0;
    rewind(fp);

    uint32_t idx = 0;
    while (fgets(line, sizeof(line), fp)) {
        strip_trailing(line);
        if (is_skippable(line)) continue;
        if (strlen(line) < MIN_CLAUSE_LEN) continue;
        if (idx++ < start) continue;

        float sim = 0.0f;
        uint32_t kf = ai_predict(ai, line, &sim);
        (void)kf;

        out->clauses_scanned++;
        if (sim > 0.0f) out->clauses_matched++;
        out->sum_sim += sim;
        if (sim < out->min_sim) out->min_sim = sim;
        if (sim > out->max_sim) out->max_sim = sim;
        if (sim >= 0.90f) out->hits_90++;
        if (sim >= 0.50f) out->hits_50++;
        if (sim >= 0.10f) out->hits_10++;

        if (out->clauses_scanned >= probe_limit) break;
    }

    fclose(fp);
}

/* Print summary stats for a trained engine: KF/Delta counts,
 * R/G range over active cells, avg A. */
static void report_engine_stats(const SpatialAI* ai) {
    uint32_t active_cells_total = 0;
    uint64_t a_sum = 0;
    uint8_t  r_min = 255, r_max = 0;
    uint8_t  g_min = 255, g_max = 0;
    uint8_t  b_min = 255, b_max = 0;

    for (uint32_t k = 0; k < ai->kf_count; k++) {
        const SpatialGrid* g = &ai->keyframes[k].grid;
        for (uint32_t i = 0; i < GRID_TOTAL; i++) {
            if (g->A[i] == 0) continue;
            active_cells_total++;
            a_sum += g->A[i];
            if (g->R[i] < r_min) r_min = g->R[i];
            if (g->R[i] > r_max) r_max = g->R[i];
            if (g->G[i] < g_min) g_min = g->G[i];
            if (g->G[i] > g_max) g_max = g->G[i];
            if (g->B[i] < b_min) b_min = g->B[i];
            if (g->B[i] > b_max) b_max = g->B[i];
        }
    }
    double avg_a = active_cells_total ? (double)a_sum / active_cells_total : 0.0;

    printf("  KF count:        %u\n", ai->kf_count);
    printf("  Delta count:     %u\n", ai->df_count);
    printf("  Active cells:    %u (across all KFs)\n", active_cells_total);
    printf("  Avg A (active):  %.2f\n", avg_a);
    printf("  R range:         %u..%u\n", r_min, r_max);
    printf("  G range:         %u..%u\n", g_min, g_max);
    printf("  B range:         %u..%u\n", b_min, b_max);
}

/* ── event log ──
 *
 * Binary stream consumed by tools/animate_training.py to render the
 * training trajectory as a twinkling 256x256 grid animation.
 *
 * Layout:
 *   header (12 B):
 *     char     magic[4] = "CEVT"
 *     uint32   version  = 1
 *     uint32   reserved = 0
 *
 *   per clause record (variable):
 *     uint32   clause_idx
 *     uint8    decision    ( 0 = new keyframe,
 *                            1 = delta,
 *                            2 = identical / skipped )
 *     uint16   byte_count  ( clauses longer than this are truncated
 *                            to 65535 which is also stream_train's
 *                            per-line cap )
 *     uint8[byte_count * 2] = (y, x) pairs, one per byte of the
 *                             clause: y = byte_index % 256,
 *                             x = clause_byte_value.  Order matches
 *                             the write order in layers_encode_clause
 *                             so animators can replay bytes in stream
 *                             order for extra motion.
 */

static FILE* event_log_open(const char* path) {
    if (!path) return NULL;
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[stream] cannot open log %s\n", path);
        return NULL;
    }
    const char magic[4] = { 'C', 'E', 'V', 'T' };
    uint32_t version  = 1;
    uint32_t reserved = 0;
    fwrite(magic, 1, 4, f);
    fwrite(&version,  sizeof(uint32_t), 1, f);
    fwrite(&reserved, sizeof(uint32_t), 1, f);
    return f;
}

static void event_log_write(FILE* f, uint32_t clause_idx,
                            uint8_t decision, const char* text, uint32_t tlen) {
    if (!f) return;
    if (tlen > 65535) tlen = 65535;
    uint16_t n = (uint16_t)tlen;

    fwrite(&clause_idx, sizeof(uint32_t), 1, f);
    fwrite(&decision,   sizeof(uint8_t),  1, f);
    fwrite(&n,          sizeof(uint16_t), 1, f);

    /* Flatten (y, x) pairs. 256 byte per-row limit matches GRID_SIZE. */
    uint8_t buf[512];
    uint32_t written = 0;
    for (uint32_t i = 0; i < tlen; i++) {
        buf[written++] = (uint8_t)(i % 256);
        buf[written++] = (uint8_t)(unsigned char)text[i];
        if (written >= sizeof(buf)) {
            fwrite(buf, 1, written, f);
            written = 0;
        }
    }
    if (written) fwrite(buf, 1, written, f);
}

static uint8_t decode_decision(uint32_t ret) {
    if (ret == UINT32_MAX) return 2;              /* skip / error */
    if (ret & 0x80000000u) return 1;              /* delta */
    return 0;                                     /* new keyframe */
}

/* ── Threshold auto-calibration ──
 *
 * Reads the first `sample_cap` valid clauses from the input, encodes
 * each one, and records the best cosine_a_only against prior same-
 * topic clauses. The observed best_sim distribution is what the real
 * ai_store_auto path would compare against threshold, so the
 * threshold sits at the target_ratio percentile of those values.
 *
 * Returns a threshold in [0, 1], or -1 if calibration produced no
 * usable samples (e.g. every clause had a unique topic).
 *
 * Pre-pass cost: O(N × max_topic_bucket × GRID_TOTAL). For 500
 * samples with a handful of topics, that's a few seconds. */

static int cmp_float_desc(const void* a, const void* b) {
    float fa = *(const float*)a, fb = *(const float*)b;
    if (fa > fb) return -1;
    if (fa < fb) return  1;
    return 0;
}

int cmp_u64_asc(const void* a, const void* b) {
    uint64_t fa = *(const uint64_t*)a, fb = *(const uint64_t*)b;
    if (fa < fb) return -1;
    if (fa > fb) return  1;
    return 0;
}

static float calibrate_threshold(const char* input_path,
                                  uint32_t sample_cap,
                                  float target_ratio) {
    FILE* fp = fopen(input_path, "r");
    if (!fp) return -1.0f;

    SpatialAI* tmp = spatial_ai_create();
    if (!tmp) { fclose(fp); return -1.0f; }

    float*   sims   = (float*)malloc(sample_cap * sizeof(float));
    uint32_t n_sims = 0;
    uint32_t seen   = 0;

    char line[LINE_BUF];
    while (seen < sample_cap && fgets(line, sizeof(line), fp)) {
        strip_trailing(line);
        if (is_skippable(line)) continue;
        if (strlen(line) < MIN_CLAUSE_LEN) continue;

        uint32_t topic = ai_resolve_topic(line, NULL);

        SpatialGrid* grid = grid_create();
        if (!grid) break;
        layers_encode_clause(line, NULL, grid);
        update_rgb_directional(grid);

        /* Best same-topic cosine against what we've already stored */
        if (topic != 0 && n_sims < sample_cap) {
            float best = 0.0f;
            int   any  = 0;
            for (uint32_t i = 0; i < tmp->kf_count; i++) {
                if (tmp->keyframes[i].topic_hash != topic) continue;
                float s = cosine_a_only(grid, &tmp->keyframes[i].grid);
                if (s > best) best = s;
                any = 1;
            }
            if (any) sims[n_sims++] = best;
        }

        /* Accumulate all calibration clauses as keyframes so subsequent
         * same-topic clauses have something to compare against. */
        ai_force_keyframe(tmp, line, NULL);
        grid_destroy(grid);
        seen++;
    }
    fclose(fp);

    float threshold = -1.0f;
    if (n_sims >= 10) {
        qsort(sims, n_sims, sizeof(float), cmp_float_desc);
        /* target_ratio fraction of clauses should be at or above
         * threshold. With the array sorted descending, the value at
         * position ⌊target × N⌋ is that threshold. */
        uint32_t idx = (uint32_t)(target_ratio * (float)n_sims);
        if (idx >= n_sims) idx = n_sims - 1;
        threshold = sims[idx];
        printf("[calibrate] sampled=%u  same-topic_pairs=%u\n"
               "[calibrate] sim distribution: min=%.3f  p%02d=%.3f  p50=%.3f  p10=%.3f  max=%.3f\n",
               seen, n_sims, sims[n_sims - 1],
               (int)(target_ratio * 100), threshold,
               sims[n_sims / 2], sims[n_sims / 10], sims[0]);
        printf("[calibrate] picked threshold=%.3f for target delta ratio %.1f%%\n",
               threshold, target_ratio * 100.0f);
    } else {
        printf("[calibrate] only %u same-topic pairs in %u clauses — "
               "not enough to tune a threshold\n",
               n_sims, seen);
    }

    free(sims);
    spatial_ai_destroy(tmp);
    return threshold;
}

/* ── Long-line split (--max-line-bytes) ───────────────────
 *
 * Stream input can carry clauses longer than the 256-byte Y-axis of
 * the encoding grid. `grid_encode` wraps via y = i % 256 silently,
 * which means the tail of a 400-byte line overwrites its head.
 *
 * split_line_into_clauses picks a cut position in [0, limit] that
 * ends on the latest sentence boundary ('.', '!', '?') or failing
 * that the latest whitespace. Runs iteratively: the producer stores
 * each piece separately. Empty or whitespace-only tails are dropped.
 *
 * The emitted pointer is a non-owning reference into `buf`. Callers
 * mutate `buf` by replacing one byte with '\0' at the cut point for
 * the duration of the per-clause store, then restore it.
 *
 * Returns the number of bytes consumed (clause + the delimiter byte
 * we landed on). The remainder starts at `buf + consumed` and is
 * ready for the next iteration. Returns the full length when `len`
 * already fits within `limit` (single-clause fast path).
 */
static uint32_t pick_cut(const char* buf, uint32_t len, uint32_t limit) {
    if (limit == 0 || len <= limit) return len;

    /* Look backwards from `limit` for the latest sentence terminator. */
    for (uint32_t i = limit; i > 0; i--) {
        char c = buf[i - 1];
        if (c == '.' || c == '!' || c == '?') return i;
    }
    /* Fall back to the latest whitespace. */
    for (uint32_t i = limit; i > 0; i--) {
        char c = buf[i - 1];
        if (c == ' ' || c == '\t') return i;
    }
    /* No punctuation, no whitespace — hard cut at the limit. */
    return limit;
}

/* Trim leading ASCII whitespace in-place by shifting. */
static void left_trim(char* s) {
    if (!s) return;
    uint32_t skip = 0;
    while (s[skip] == ' ' || s[skip] == '\t') skip++;
    if (skip) memmove(s, s + skip, strlen(s + skip) + 1);
}

/* ── main ────────────────────────────────────────────────── */

int main(int argc, char** argv) {
    StreamArgs args;
    if (parse_args(argc, argv, &args) != 0) {
        usage(argv[0]);
        return 2;
    }

    morpheme_init();

    SpatialAI* ai = spatial_ai_create();
    if (!ai) {
        fprintf(stderr, "[stream] spatial_ai_create failed\n");
        return 1;
    }

    /* Threshold precedence: explicit --threshold > --target-delta
     * auto-calibration > engine default. */
    if (args.threshold >= 0.0f) {
        ai_set_store_threshold(args.threshold);
    } else if (args.target_delta >= 0.0f) {
        uint32_t sample_cap = args.calibrate_samples;
        if (sample_cap > args.max_clauses) sample_cap = args.max_clauses;
        printf("[calibrate] target_delta=%.1f%%  samples=%u\n",
               args.target_delta * 100.0f, sample_cap);
        float t = calibrate_threshold(args.input, sample_cap, args.target_delta);
        if (t >= 0.0f) {
            ai_set_store_threshold(t);
        } else {
            printf("[calibrate] falling back to engine default %.2f\n",
                   ai_get_store_threshold());
        }
    }

    printf("[stream] reading: %s\n", args.input);
    printf("[stream] max=%u  checkpoint=%u  threshold=%.3f  save=%s\n",
           args.max_clauses, args.checkpoint,
           ai_get_store_threshold(), args.save);
    printf("[stream] clock_g_threshold=%llu  clock_rb_threshold=%llu\n",
           (unsigned long long)args.clock_g_threshold,
           (unsigned long long)args.clock_rb_threshold);

    /* Eagerly create the canvas pool so we can pin the engine
     * thresholds before the first clause arrives. */
    {
        SpatialCanvasPool* pool = ai_get_canvas_pool(ai);
        if (pool) {
            pool->clock_g_threshold  = args.clock_g_threshold;
            pool->clock_rb_threshold = args.clock_rb_threshold;
        }
    }

    FILE* fp = fopen(args.input, "r");
    if (!fp) {
        fprintf(stderr, "[stream] cannot open %s\n", args.input);
        spatial_ai_destroy(ai);
        return 1;
    }

    ensure_parent_dir(args.save);
    if (args.event_log) ensure_parent_dir(args.event_log);
    FILE* log = event_log_open(args.event_log);
    if (log) printf("[stream] event log: %s\n", args.event_log);

    char line[LINE_BUF];
    uint32_t count = 0;
    uint32_t skipped = 0;
    double   t0 = now_sec();

    uint32_t split_events = 0;   /* # of input lines we cut up */
    uint32_t pieces_total  = 0;  /* # of post-split clauses actually stored */
    while (count < args.max_clauses && fgets(line, sizeof(line), fp)) {
        strip_trailing(line);
        if (is_skippable(line)) { skipped++; continue; }
        uint32_t line_len = (uint32_t)strlen(line);
        if (line_len < MIN_CLAUSE_LEN) { skipped++; continue; }

        /* Long-line auto-split:
         *   --max-line-bytes defaults to 256 (grid Y axis). Lines
         *   beyond that are cut at the latest sentence boundary
         *   (falling back to whitespace / hard cut). Each piece is
         *   stored as its own clause so grid_encode never wraps. */
        const int split_enabled = (args.max_line_bytes > 0) &&
                                  (line_len > args.max_line_bytes);
        if (split_enabled) split_events++;

        uint32_t offset = 0;
        while (count < args.max_clauses) {
            uint32_t remaining = line_len - offset;
            if (remaining == 0) break;

            uint32_t chunk = (args.max_line_bytes > 0 &&
                              remaining > args.max_line_bytes)
                           ? pick_cut(line + offset, remaining,
                                      args.max_line_bytes)
                           : remaining;
            if (chunk == 0) break;   /* safety — shouldn't happen */

            char saved = line[offset + chunk];
            line[offset + chunk] = '\0';

            char* piece = line + offset;
            left_trim(piece);
            uint32_t piece_len = (uint32_t)strlen(piece);

            if (piece_len >= MIN_CLAUSE_LEN && !is_skippable(piece)) {
                /* Full training step: layers_encode_clause → update_rgb_directional
                 * → cosine vs existing KFs → delta (≥threshold) or new KF (<threshold).
                 * All of that lives inside ai_store_auto. */
                uint32_t rid = ai_store_auto(ai, piece, NULL);

                /* ── Canvas layer (2048×1024, 32 clauses per canvas) ───── */
                pool_add_clause(ai_get_canvas_pool(ai), piece);

                count++;
                pieces_total++;

                if (log) {
                    event_log_write(log, count - 1, decode_decision(rid),
                                    piece, piece_len);
                }

                if (args.verbose) {
                    printf("[stream] %u: kf=%u df=%u  %.40s%s%s\n",
                           count, ai->kf_count, ai->df_count, piece,
                           strlen(piece) > 40 ? "..." : "",
                           split_enabled ? "  [split]" : "");
                }
            } else {
                skipped++;
            }

            line[offset + chunk] = saved;
            offset += chunk;
        }

        if (!args.verbose && count % 5000 == 0 && count > 0) {
            double dt = now_sec() - t0;
            printf("[stream] %u clauses, KF=%u, Delta=%u, elapsed=%.1fs (%.0f c/s)\n",
                   count, ai->kf_count, ai->df_count, dt,
                   dt > 0 ? (double)count / dt : 0.0);
        }

        if (args.checkpoint > 0 && count % args.checkpoint == 0) {
            /* Derive a checkpoint path from the save path: save=foo.spai →
             * checkpoint=foo.ckpt_000005000.spai (same dir). */
            char ckpt[1280];
            size_t base_len = strlen(args.save);
            const char* dot = strrchr(args.save, '.');
            size_t stem_len = dot ? (size_t)(dot - args.save) : base_len;
            if (stem_len > 1100) stem_len = 1100;
            memcpy(ckpt, args.save, stem_len);
            snprintf(ckpt + stem_len, sizeof(ckpt) - stem_len,
                     ".ckpt_%06u.spai", count);

            SpaiStatus s = ai_save(ai, ckpt);
            if (s == SPAI_OK) {
                printf("[checkpoint] saved: %s (%.2f MB)\n",
                       ckpt, file_size_or_zero(ckpt) / 1e6);
            } else {
                printf("[checkpoint] FAILED: %s (%s)\n",
                       ckpt, spai_status_str(s));
            }
        }
    }

    fclose(fp);
    if (log) { fclose(log); printf("[stream] event log closed\n"); }

    double elapsed = now_sec() - t0;
    printf("[stream] ingest done: clauses=%u skipped=%u KF=%u Delta=%u elapsed=%.2fs\n",
           count, skipped, ai->kf_count, ai->df_count, elapsed);
    if (split_events > 0) {
        printf("[stream] split: %u long lines → %u pieces (cap=%u bytes)\n",
               split_events, pieces_total, args.max_line_bytes);
    }

    /* ── Canvas layer summary ──
     *
     * Breakdown of the 2048×1024 canvas pool populated alongside the
     * per-clause KFs. Each canvas is 32 clauses of one DataType; once
     * full it's classified I/P by scene_change_classify. */
    /* ── Drift / online-dedup feasibility check (Step C) ──
     *
     * Core question: does clock SAD actually track raw content
     * similarity? If yes, the clock engine is a cheap online
     * fingerprint → online dedup is viable. If no (clock SAD
     * collapses distinct content to similar signatures) → clock is
     * not the right tool for dedup.
     *
     * Method: for every same-type canvas pair, compute BOTH
     *   clock_total = R+G+B+A SAD on the clock engine
     *   raw_A_sad   = sum |A_i - A_j| over the full 2M-cell canvas
     * Sort by clock_total, report top-5 closest and top-5 farthest
     * pairs with BOTH metrics. Eyeball correlation. */
    if (ai->canvas_pool && ai->canvas_pool->count >= 2) {
        SpatialCanvasPool* pool = ai->canvas_pool;
        uint32_t cnt = pool->count;

        typedef struct { uint32_t i, j; uint64_t clock_sad; uint64_t raw_sad; } PairRow;
        uint64_t max_pairs = (uint64_t)cnt * (uint64_t)(cnt - 1) / 2ull;
        if (max_pairs > 0 && max_pairs <= 200000) {
            PairRow* rows = (PairRow*)malloc(max_pairs * sizeof(PairRow));
            if (rows) {
                uint32_t np = 0;
                for (uint32_t i = 0; i < cnt; i++) {
                    SpatialCanvas* ci = pool->canvases[i];
                    if (!ci || ci->slot_count < CV_SLOTS) continue;
                    for (uint32_t j = i + 1; j < cnt; j++) {
                        SpatialCanvas* cj = pool->canvases[j];
                        if (!cj || cj->slot_count < CV_SLOTS) continue;
                        if (cj->canvas_type != ci->canvas_type) continue;
                        RGBAClockSad s = rgba_clock_sad(&ci->clock, &cj->clock);
                        rows[np].i = i; rows[np].j = j;
                        rows[np].clock_sad = s.R_sad + s.G_sad + s.B_sad + s.A_sad;
                        rows[np].raw_sad  = 0;  /* filled on-demand for shown rows */
                        np++;
                    }
                }

                /* Partial sort: we only need the 5 smallest and 5 largest
                 * by clock_sad. Full qsort is fine at this size. */
                for (uint32_t i = 0; i < np; i++) {
                    for (uint32_t j = i + 1; j < np; j++) {
                        if (rows[j].clock_sad < rows[i].clock_sad) {
                            PairRow t = rows[i]; rows[i] = rows[j]; rows[j] = t;
                        }
                    }
                    if (i >= 5 && i < np - 6) i = np - 6;  /* skip middle */
                }

                /* Compute raw A-channel SAD only for the shown rows. */
                uint32_t to_show = (np < 10) ? np : 10;
                for (uint32_t k = 0; k < to_show; k++) {
                    uint32_t idx = (k < 5) ? k : (np - 10 + k);
                    if (idx >= np) continue;
                    SpatialCanvas* ci = pool->canvases[rows[idx].i];
                    SpatialCanvas* cj = pool->canvases[rows[idx].j];
                    uint64_t raw = 0;
                    for (uint32_t x = 0; x < CV_TOTAL; x++) {
                        uint32_t a = ci->A[x], b = cj->A[x];
                        raw += (a > b) ? (a - b) : (b - a);
                    }
                    rows[idx].raw_sad = raw;
                }

                printf("[drift] pair table (clock vs raw A-channel SAD):\n");
                printf("[drift]        pair         clock_sad    raw_A_sad    topic_match\n");
                printf("[drift]   -- 5 smallest clock_sad (candidate duplicates) --\n");
                for (uint32_t k = 0; k < 5 && k < np; k++) {
                    SpatialCanvas* ci = pool->canvases[rows[k].i];
                    SpatialCanvas* cj = pool->canvases[rows[k].j];
                    /* Simple topic-match count: how many slot topic_hashes
                     * of ci also appear in cj. Real duplicates share many. */
                    uint32_t topic_match = 0;
                    for (uint32_t a = 0; a < ci->slot_count; a++) {
                        uint32_t ha = ci->meta[a].topic_hash;
                        if (ha == 0) continue;
                        for (uint32_t b = 0; b < cj->slot_count; b++) {
                            if (cj->meta[b].topic_hash == ha) { topic_match++; break; }
                        }
                    }
                    printf("[drift]   %4u <-> %4u  %10llu   %10llu    %u/%u\n",
                           rows[k].i, rows[k].j,
                           (unsigned long long)rows[k].clock_sad,
                           (unsigned long long)rows[k].raw_sad,
                           topic_match, ci->slot_count);
                }
                printf("[drift]   -- 5 largest clock_sad (should be unrelated) --\n");
                for (uint32_t k = (np < 5) ? 0 : (np - 5); k < np; k++) {
                    SpatialCanvas* ci = pool->canvases[rows[k].i];
                    SpatialCanvas* cj = pool->canvases[rows[k].j];
                    uint32_t topic_match = 0;
                    for (uint32_t a = 0; a < ci->slot_count; a++) {
                        uint32_t ha = ci->meta[a].topic_hash;
                        if (ha == 0) continue;
                        for (uint32_t b = 0; b < cj->slot_count; b++) {
                            if (cj->meta[b].topic_hash == ha) { topic_match++; break; }
                        }
                    }
                    printf("[drift]   %4u <-> %4u  %10llu   %10llu    %u/%u\n",
                           rows[k].i, rows[k].j,
                           (unsigned long long)rows[k].clock_sad,
                           (unsigned long long)rows[k].raw_sad,
                           topic_match, ci->slot_count);
                }
            }
            free(rows);
        }
    }

    if (ai->canvas_pool) {
        SpatialCanvasPool* pool = ai->canvas_pool;
        uint32_t cv_total = pool->count;
        uint32_t cv_full = 0, cv_iframe = 0, cv_pframe = 0;
        uint32_t per_type[DATA_TYPE_COUNT] = {0};

        /* freq_tag stats: how many chapters (distinct freq_tag values)
         * exist on average per filled canvas, and what's the max. */
        uint64_t total_chapters = 0;
        uint32_t cvs_with_tags  = 0;
        uint16_t max_chapters   = 0;
        for (uint32_t i = 0; i < cv_total; i++) {
            SpatialCanvas* c = pool->canvases[i];
            if (!c) continue;
            if (c->slot_count == CV_SLOTS) cv_full++;
            if (c->classified) {
                if (c->frame_type == CANVAS_IFRAME) cv_iframe++;
                else                                cv_pframe++;
            }
            if ((uint32_t)c->canvas_type < DATA_TYPE_COUNT) {
                per_type[(uint32_t)c->canvas_type]++;
            }
            if (c->slot_count > 0) {
                uint16_t maxtag = 0;
                for (uint32_t s = 0; s < c->slot_count; s++) {
                    if (c->meta[s].freq_tag > maxtag) maxtag = c->meta[s].freq_tag;
                }
                if (maxtag > 0) {
                    total_chapters += maxtag;
                    cvs_with_tags++;
                    if (maxtag > max_chapters) max_chapters = maxtag;
                }
            }
        }
        printf("[canvas] pool=%u  full=%u  I-frame=%u  P-frame=%u  slots=%u\n",
               cv_total, cv_full, cv_iframe, cv_pframe,
               pool_total_slots(pool));
        printf("[canvas] per-type: prose=%u dialog=%u code=%u short=%u\n",
               per_type[DATA_PROSE], per_type[DATA_DIALOG],
               per_type[DATA_CODE],  per_type[DATA_SHORT]);
        if (cvs_with_tags > 0) {
            double avg_chapters = (double)total_chapters / (double)cvs_with_tags;
            printf("[canvas] chapters/canvas: avg=%.2f  max=%u  (over %u tagged canvases)\n",
                   avg_chapters, max_chapters, cvs_with_tags);
        }
    }

    /* ── Post-training: EMA repaint + keyframe re-cluster ───────
     *
     * Arrival-order KFs suffer two issues: (1) early KFs' R/G/B were
     * set before EMA had enough evidence, and (2) KFs that should have
     * been deltas got stored as full frames because the matching
     * anchor hadn't arrived yet. Repaint fixes (1); recluster fixes
     * (2). Together they typically shrink the on-disk model ~80%. */
    if (!args.no_recluster && ai->kf_count > 1) {
        double tp0 = now_sec();
        printf("[repaint] updating %u keyframes with final EMA...\n", ai->kf_count);
        ai_repaint_ema(ai);
        printf("[repaint] done in %.2fs\n", now_sec() - tp0);

        /* Threshold precedence:
         *   explicit --cluster-threshold wins (single value broadcast to all types).
         *   otherwise auto-calibrate per DataType at --cluster-target-merge,
         *   because the A-channel is frozen at encode time and reusing the
         *   store threshold is guaranteed to merge zero pairs. */
        double tc0 = now_sec();
        if (args.cluster_threshold >= 0.0f) {
            printf("[recluster] re-arranging keyframes (threshold=%.3f explicit)...\n",
                   args.cluster_threshold);
        } else {
            printf("[recluster] re-arranging keyframes "
                   "(auto-calibrate per DataType, target_merge=%.2f)...\n",
                   args.cluster_target_merge);
        }
        ai_recluster_ex(ai, args.cluster_threshold, args.cluster_target_merge);
        printf("[recluster] done in %.2fs\n", now_sec() - tc0);

        /* Canvas-level recluster: same idea one tier up. Online
         * scene_change_classify decided I/P based on canvases visible
         * at that point. Now we have all canvases and can pick better
         * parents.
         *
         * Threshold precedence:
         *   1. Explicit --canvas-cluster-threshold wins.
         *   2. Else auto-calibrate from the corpus's own same-type
         *      block-sum cosine distribution at --canvas-target-merge
         *      (default 0.5 = median).
         *   3. Fall back to 0.8 if the pool is too sparse to calibrate. */
        if (ai->canvas_pool && ai->canvas_pool->count > 1) {
            float cvt;
            if (args.canvas_cluster_threshold >= 0.0f) {
                cvt = args.canvas_cluster_threshold;
                printf("[canvas-recluster] threshold=%.3f (explicit)  canvases=%u\n",
                       cvt, ai->canvas_pool->count);
            } else {
                float auto_t = canvas_pool_auto_threshold(
                    ai->canvas_pool, args.canvas_target_merge);
                if (auto_t >= 0.0f) {
                    cvt = auto_t;
                    printf("[canvas-recluster] threshold=%.3f (auto, target_merge=%.2f)  canvases=%u\n",
                           cvt, args.canvas_target_merge, ai->canvas_pool->count);
                } else {
                    cvt = 0.8f;
                    printf("[canvas-recluster] threshold=%.3f (fallback)  canvases=%u\n",
                           cvt, ai->canvas_pool->count);
                }
            }
            double tcv0 = now_sec();
            canvas_pool_recluster(ai->canvas_pool, cvt);
            printf("[canvas-recluster] done in %.2fs\n", now_sec() - tcv0);
        }
    } else if (args.no_recluster) {
        printf("[recluster] skipped (--no-recluster)\n");
    }

    /* Final save */
    SpaiStatus s = ai_save(ai, args.save);
    if (s != SPAI_OK) {
        fprintf(stderr, "[stream] final save FAILED (%s)\n", spai_status_str(s));
        spatial_ai_destroy(ai);
        return 1;
    }
    printf("[done] saved: %s (%.2f MB)\n",
           args.save, file_size_or_zero(args.save) / 1e6);

    /* Engine stats */
    printf("\n[stats] engine summary\n");
    report_engine_stats(ai);

    /* Optional verify */
    if (args.verify && count > 0) {
        uint32_t probe = count > VERIFY_PROBE ? VERIFY_PROBE : count;
        printf("\n[verify] unseen-query pass on last %u clauses\n", probe);
        VerifyStats vs;
        verify_run(ai, args.input, probe, &vs);

        double avg = vs.clauses_scanned ? vs.sum_sim / vs.clauses_scanned : 0.0;
        printf("  scanned:         %u\n", vs.clauses_scanned);
        printf("  matched (>0):    %u\n", vs.clauses_matched);
        printf("  avg similarity:  %.4f\n", avg);
        printf("  min / max:       %.4f / %.4f\n", vs.min_sim, vs.max_sim);
        printf("  hits >= 0.90:    %u\n", vs.hits_90);
        printf("  hits >= 0.50:    %u\n", vs.hits_50);
        printf("  hits >= 0.10:    %u\n", vs.hits_10);
    }

    spatial_ai_destroy(ai);
    return 0;
}
