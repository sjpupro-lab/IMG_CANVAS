/*
 * draw — frame-by-frame image generation CLI.
 *
 *   ./draw --memory <out.imem>
 *          [--model <out.spai>] [--seed-kf <id>]
 *          [--seed-image <path>]
 *          [--out <dir>]
 *          [--frames <N>]
 *          [--top-g <N>] [--penalty <α>]
 *
 *   The engine paints an image by running N drawing passes on a
 *   shared CE grid. Each pass is a "video frame" — the grid is
 *   rendered after the pass and saved as frame_###.png. The last
 *   pass is also saved as final.png / final.ppm; that's the output.
 *
 *   Seeds (most-specific wins):
 *     --seed-image <path>   run img_pipeline_run on the image,
 *                           use its CE grid as the starting state
 *     --seed-kf <id>        copy ce_snapshot from SpatialAI KF id
 *                           (requires --model)
 *     (neither)             start from an empty CE grid
 *
 *   Output layout:
 *     <out>/frames/frame_000.png  frame_000.ppm   // state after pass 1
 *     <out>/frames/frame_001.png  frame_001.ppm   //             pass 2
 *     ...
 *     <out>/final.png            final.ppm        // same as the last frame
 */

#include "img_delta_memory.h"
#include "img_delta_learn.h"
#include "img_drawing.h"
#include "img_noise_memory.h"
#include "img_render.h"
#include "img_pipeline.h"
#include "img_ce.h"
#include "spatial_bimodal.h"
#include "spatial_io.h"
#include "spatial_keyframe.h"
#include "stb_image.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ── portable mkdir -p (single-level; caller chains for depth) ── */

#ifdef _WIN32
#include <direct.h>
static int mkdir_portable(const char* path) { return _mkdir(path); }
#else
static int mkdir_portable(const char* path) {
    return mkdir(path, 0755);
}
#endif

/* mkdir -p: walk the path and create every missing parent.
 * Accepts both '/' and '\' separators; tolerates trailing slashes.
 * Returns 1 on success (or "already exists"), 0 on failure. */
static int ensure_dir(const char* path) {
    if (!path || !*path) return 0;
    struct stat st;
    if (stat(path, &st) == 0) return 1;

    char buf[4096];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return 0;
    memcpy(buf, path, n + 1);

    /* Normalise separators to '/' so we can walk them uniformly. */
    for (size_t i = 0; i < n; i++) if (buf[i] == '\\') buf[i] = '/';

    /* Skip leading '/' (absolute) or drive-letter (Windows "C:/"). */
    size_t start = 0;
    if (buf[0] == '/') start = 1;
#ifdef _WIN32
    if (n >= 3 && buf[1] == ':' && buf[2] == '/') start = 3;
#endif

    for (size_t i = start; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char save = buf[i];
            buf[i] = '\0';
            if (buf[start] && stat(buf, &st) != 0) {
                if (mkdir_portable(buf) != 0) return 0;
            }
            buf[i] = save;
        }
    }
    return 1;
}

static int ensure_subdir(const char* base, const char* sub, char* out, size_t cap) {
    snprintf(out, cap, "%s/%s", base, sub);
    return ensure_dir(out);
}

/* ── args ────────────────────────────────────────────────── */

typedef struct {
    const char* memory_path;
    const char* model_path;
    const char* seed_image;
    int         seed_kf;     /* -1 = unset */
    const char* out_dir;
    uint32_t    frames;
    uint32_t    top_g;
    double      penalty;
    const char* nmem_path;
    uint64_t    nmem_seed;
    double      nmem_temperature;
} Args;

static void usage(const char* prog) {
    fprintf(stderr,
        "usage: %s --memory <imem> [options]\n"
        "\n"
        "  --memory <path>      DeltaMemory file (required)\n"
        "  --model <path>       SpatialAI file (for --seed-kf)\n"
        "  --seed-image <path>  run pipeline on this image as seed\n"
        "  --seed-kf <id>       use this keyframe's CE snapshot as seed\n"
        "                       (requires --model)\n"
        "  --out <dir>          output directory (default out/draw)\n"
        "  --frames <N>         number of drawing passes (default 8)\n"
        "  --top-g <N>          top-G candidate pool (default 4)\n"
        "  --penalty <α>        presence penalty (default 0.5)\n"
        "  --noise <path>       NoiseMemory file — seeds every frame\n"
        "                       from a learned spatial prior instead\n"
        "                       of an empty grid (applied only to the\n"
        "                       first frame when no other seed is set)\n"
        "  --noise-seed <u64>   PRNG seed for --noise sampling\n"
        "  --noise-temperature <F>  0=greedy, 1=nominal, >1=flatter\n"
        "\n"
        "Output: <out>/frames/frame_NNN.{png,ppm} per pass,\n"
        "         <out>/final.{png,ppm} — the last frame (the result).\n",
        prog);
}

static int parse_args(int argc, char** argv, Args* a) {
    a->memory_path = NULL;
    a->model_path  = NULL;
    a->seed_image  = NULL;
    a->seed_kf     = -1;
    a->out_dir     = "out/draw";
    a->frames      = 8;
    a->top_g       = 4;
    a->penalty     = 0.5;
    a->nmem_path   = NULL;
    a->nmem_seed   = 0;
    a->nmem_temperature = 1.0;
    for (int i = 1; i < argc; i++) {
        const char* k = argv[i];
        if      (strcmp(k, "--memory")     == 0 && i + 1 < argc) a->memory_path = argv[++i];
        else if (strcmp(k, "--model")      == 0 && i + 1 < argc) a->model_path  = argv[++i];
        else if (strcmp(k, "--seed-image") == 0 && i + 1 < argc) a->seed_image  = argv[++i];
        else if (strcmp(k, "--seed-kf")    == 0 && i + 1 < argc) a->seed_kf     = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(k, "--out")        == 0 && i + 1 < argc) a->out_dir     = argv[++i];
        else if (strcmp(k, "--frames")     == 0 && i + 1 < argc) a->frames      = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(k, "--top-g")      == 0 && i + 1 < argc) a->top_g       = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(k, "--penalty")    == 0 && i + 1 < argc) a->penalty     = strtod(argv[++i], NULL);
        else if (strcmp(k, "--noise")      == 0 && i + 1 < argc) a->nmem_path   = argv[++i];
        else if (strcmp(k, "--noise-seed") == 0 && i + 1 < argc) a->nmem_seed   = strtoull(argv[++i], NULL, 10);
        else if (strcmp(k, "--noise-temperature") == 0 && i + 1 < argc) a->nmem_temperature = strtod(argv[++i], NULL);
        else if (strcmp(k, "-h")           == 0 || strcmp(k, "--help") == 0) {
            usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "unknown flag: %s\n", k);
            return 0;
        }
    }
    if (!a->memory_path) { fprintf(stderr, "missing --memory\n"); return 0; }
    if (a->frames < 1)   a->frames = 1;
    if (a->top_g  < 1)   a->top_g  = 1;
    if (a->penalty < 0)  a->penalty = 0;
    return 1;
}

/* ── PNG save (via stb) ─────────────────────────────────── */

#include "stb_image_write.h"
static int save_png(const char* path, const ImgRenderImage* img) {
    return stbi_write_png(path, (int)img->width, (int)img->height,
                          3, img->rgb, (int)img->width * 3) != 0;
}

/* ── main ────────────────────────────────────────────────── */

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, &args)) { usage(argv[0]); return 2; }

    /* Load memory (required). */
    ImemStatus ms = IMEM_OK;
    ImgDeltaMemory* mem = img_delta_memory_load(args.memory_path, &ms);
    if (!mem) {
        fprintf(stderr, "failed to load %s: %s\n",
                args.memory_path, img_delta_memory_status_str(ms));
        return 1;
    }
    const uint32_t n_units = img_delta_memory_count(mem);
    fprintf(stderr, "[draw] memory: %u units\n", n_units);

    /* Seed grid. */
    ImgCEGrid* grid = img_ce_grid_create();
    if (!grid) { fprintf(stderr, "OOM\n"); img_delta_memory_destroy(mem); return 1; }

    if (args.seed_image) {
        int w = 0, h = 0, c = 0;
        stbi_uc* px = stbi_load(args.seed_image, &w, &h, &c, 3);
        if (!px) {
            fprintf(stderr, "seed-image load failed: %s\n", stbi_failure_reason());
            img_ce_grid_destroy(grid); img_delta_memory_destroy(mem);
            return 1;
        }
        ImgPipelineResult r = {0};
        if (!img_pipeline_run((const uint8_t*)px, (uint32_t)w, (uint32_t)h,
                              NULL, NULL, &r)) {
            fprintf(stderr, "pipeline_run on seed-image failed\n");
            stbi_image_free(px);
            img_ce_grid_destroy(grid); img_delta_memory_destroy(mem);
            return 1;
        }
        /* Copy CE cells over. */
        memcpy(grid->cells, r.ce_grid->cells,
               IMG_CE_TOTAL * sizeof(ImgCECell));
        img_pipeline_result_destroy(&r);
        stbi_image_free(px);
        fprintf(stderr, "[draw] seeded from image %s\n", args.seed_image);
    } else if (args.seed_kf >= 0) {
        if (!args.model_path) {
            fprintf(stderr, "--seed-kf requires --model\n");
            img_ce_grid_destroy(grid); img_delta_memory_destroy(mem);
            return 1;
        }
        SpaiStatus ss = SPAI_OK;
        SpatialAI* ai = ai_load(args.model_path, &ss);
        if (!ai) {
            fprintf(stderr, "model load failed: %s\n", spai_status_str(ss));
            img_ce_grid_destroy(grid); img_delta_memory_destroy(mem);
            return 1;
        }
        const ImgCEGrid* snap = ai_get_ce_snapshot(ai, (uint32_t)args.seed_kf);
        if (!snap) {
            fprintf(stderr, "--seed-kf %d: no ce_snapshot bound\n", args.seed_kf);
            spatial_ai_destroy(ai);
            img_ce_grid_destroy(grid); img_delta_memory_destroy(mem);
            return 1;
        }
        memcpy(grid->cells, snap->cells,
               IMG_CE_TOTAL * sizeof(ImgCECell));
        spatial_ai_destroy(ai);
        fprintf(stderr, "[draw] seeded from kf %d of %s\n",
                args.seed_kf, args.model_path);
    } else {
        fprintf(stderr, "[draw] seeded from empty grid\n");
    }

    /* Optional: learned spatial prior. Loaded into a local NMEM and
     * sampled into the grid once, before the drawing loop starts.
     * This replaces "empty canvas → abstract pattern" with a stamp
     * over a learned-what-usually-lives-here layout. */
    ImgNoiseMemory nmem;
    int nmem_ready = 0;
    if (args.nmem_path) {
        if (!img_noise_memory_init(&nmem)) {
            fprintf(stderr, "[draw] nmem init failed\n");
        } else if (!img_noise_memory_load(&nmem, args.nmem_path)) {
            fprintf(stderr, "[draw] nmem load failed: %s\n", args.nmem_path);
            img_noise_memory_free(&nmem);
        } else {
            nmem_ready = 1;
            ImgNoiseSampleOptions nopt = img_noise_sample_default_options();
            nopt.seed = args.nmem_seed;
            if (args.nmem_temperature >= 0.0) {
                double t = args.nmem_temperature * 256.0;
                if      (t < 0.0)       t = 0.0;
                else if (t > 65535.0)   t = 65535.0;
                nopt.temperature_q8 = (uint32_t)(t + 0.5);
            }
            if (img_noise_memory_sample_grid(&nmem, grid, &nopt)) {
                fprintf(stderr,
                        "[draw] prior: sampled grid from %s (seed=%llu t=%.3f)\n",
                        args.nmem_path,
                        (unsigned long long)args.nmem_seed,
                        args.nmem_temperature);
            } else {
                fprintf(stderr, "[draw] prior sample failed\n");
            }
        }
    }

    /* Prepare output directory. */
    char frames_dir[2048];
    if (!ensure_dir(args.out_dir) ||
        !ensure_subdir(args.out_dir, "frames", frames_dir, sizeof(frames_dir))) {
        fprintf(stderr, "cannot create output dir %s\n", args.out_dir);
        img_ce_grid_destroy(grid); img_delta_memory_destroy(mem);
        return 1;
    }

    /* Run one pass per frame; render + save after each. */
    ImgDrawingOptions opt = img_drawing_default_options();
    opt.top_g            = args.top_g;
    opt.presence_penalty = args.penalty;
    opt.passes           = 1;   /* we drive the pass count externally */

    ImgRenderOptions ropt = img_render_default_options();
    char path[4096];
    uint32_t total_stamps = 0, total_unique_max = 0;

    for (uint32_t f = 0; f < args.frames; f++) {
        ImgDrawingStats ds = {0, 0, 0, 0, 0, 0};
        if (!img_drawing_pass(grid, mem, &opt, &ds)) {
            fprintf(stderr, "drawing_pass failed at frame %u\n", f);
            img_ce_grid_destroy(grid); img_delta_memory_destroy(mem);
            return 1;
        }
        total_stamps += ds.stamps_applied;
        if (ds.unique_deltas_used > total_unique_max) {
            total_unique_max = ds.unique_deltas_used;
        }

        ImgRenderImage img = {0};
        if (!img_render_ce_grid(grid, &ropt, &img)) {
            fprintf(stderr, "render failed at frame %u\n", f);
            img_ce_grid_destroy(grid); img_delta_memory_destroy(mem);
            return 1;
        }

        snprintf(path, sizeof(path), "%s/frame_%03u.png", frames_dir, f);
        save_png(path, &img);
        snprintf(path, sizeof(path), "%s/frame_%03u.ppm", frames_dir, f);
        img_render_save_ppm(path, &img);

        /* Final pass → also save <out>/final.{png,ppm} */
        if (f + 1 == args.frames) {
            snprintf(path, sizeof(path), "%s/final.png", args.out_dir);
            save_png(path, &img);
            snprintf(path, sizeof(path), "%s/final.ppm", args.out_dir);
            img_render_save_ppm(path, &img);
        }

        fprintf(stderr, "  frame %02u/%02u  stamps=%u  unique=%u\n",
                f + 1, args.frames, ds.stamps_applied,
                ds.unique_deltas_used);

        img_render_free_image(&img);
    }

    fprintf(stderr, "\n[draw] done — %u frames, %u total stamps,"
                    " max unique per frame %u\n",
            args.frames, total_stamps, total_unique_max);
    fprintf(stderr, "[draw] output: %s/final.png (+frames/)\n", args.out_dir);

    img_ce_grid_destroy(grid);
    img_delta_memory_destroy(mem);
    if (nmem_ready) img_noise_memory_free(&nmem);
    return 0;
}
