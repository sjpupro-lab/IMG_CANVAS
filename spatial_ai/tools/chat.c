/*
 * chat.c — interactive chat REPL for a trained SPATIAL-PATTERN-AI model.
 *
 * Two things live here:
 *
 *   1. Session history (ring buffer of last N turns). Each chat turn prepends
 *      the last `context_turns` (user + bot) bytes to the current input before
 *      calling ai_generate_next / ai_predict, so replies stay on-topic across
 *      turns. Sessions can be saved to / loaded from a text file, and cleared
 *      with :reset.
 *
 *   2. Query router. Every input is classified into one of CHAT / RETRIEVE /
 *      GENERATE / IMAGE / TOPK / COMMAND / EMPTY, and dispatched through a
 *      small table. Adding a new route (e.g. video, audio, external tool)
 *      means one entry in g_routes[] + one handler function — the REPL loop
 *      does not change.
 *
 * Usage:
 *   ./build/chat --load build/models/wiki5k.spai
 *   ./build/chat --train data/wiki5k.txt --max 5000
 *   ./build/chat --load model.spai --session session.txt --img-bin ../img-canvas/build/imgcanvas
 *
 * In-session prefixes:
 *   /gen <text>   force generation with no history prepended
 *   /ret <text>   retrieval-only (no generation)
 *   /img <text>   image route — shells out to IMG_CANVAS_BIN (stub if unset)
 *   /topk[N] <t>  show top-N keyframe matches for <t>
 *
 * Commands (colon-prefixed, unchanged behaviour takes precedence):
 *   :q :quit                 quit
 *   :help                    commands
 *   :gen | :retr | :both     switch default mode for un-prefixed input
 *   :topk [N]                arm top-K print for next input
 *   :history                 show session turns
 *   :reset                   clear session history
 *   :save <path>             save session to file
 *   :load <path>             load session from file
 *   :ctx <N>                 set history turns prepended to each chat (0..HISTORY_MAX)
 */

#include "spatial_grid.h"
#include "spatial_layers.h"
#include "spatial_morpheme.h"
#include "spatial_match.h"
#include "spatial_keyframe.h"
#include "spatial_generate.h"
#include "spatial_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#endif

#define LINE_BUF      8192
#define OUT_BUF       4096
#define HISTORY_MAX   8
#define CTX_BUF      (LINE_BUF * 4)

/* ─────────────────────────── session ring buffer ────────────────────────── */

typedef struct {
    char user_text[LINE_BUF];
    char bot_text [OUT_BUF];
} ChatTurn;

typedef struct {
    ChatTurn turns[HISTORY_MAX];
    uint32_t count;          /* valid turns (0..HISTORY_MAX) */
    uint32_t head;           /* next write slot */
    uint32_t total;          /* lifetime turn count */
    uint32_t context_turns;  /* how many past turns to prepend (0..HISTORY_MAX) */
} ChatSession;

static void session_init(ChatSession* s) {
    memset(s, 0, sizeof *s);
    s->context_turns = 3;
}

static void session_push(ChatSession* s, const char* user, const char* bot) {
    ChatTurn* t = &s->turns[s->head];
    snprintf(t->user_text, sizeof t->user_text, "%s", user ? user : "");
    snprintf(t->bot_text,  sizeof t->bot_text,  "%s", bot  ? bot  : "");
    s->head = (s->head + 1) % HISTORY_MAX;
    if (s->count < HISTORY_MAX) s->count++;
    s->total++;
}

static void session_reset(ChatSession* s) {
    uint32_t ctx = s->context_turns;
    memset(s, 0, sizeof *s);
    s->context_turns = ctx;
}

/* Concatenate oldest→newest history turns into `out` then append `cur`.
 * Returns bytes written (excluding NUL). */
static uint32_t session_build_context(const ChatSession* s, const char* cur,
                                      char* out, uint32_t max_out) {
    if (!out || max_out == 0) return 0;
    uint32_t pos = 0;
    uint32_t n = s->count < s->context_turns ? s->count : s->context_turns;
    uint32_t start = (s->head + HISTORY_MAX - n) % HISTORY_MAX;
    for (uint32_t i = 0; i < n && pos + 1 < max_out; i++) {
        const ChatTurn* t = &s->turns[(start + i) % HISTORY_MAX];
        int w = snprintf(out + pos, max_out - pos, "%s %s ",
                         t->user_text, t->bot_text);
        if (w < 0) break;
        if ((uint32_t)w >= max_out - pos) { pos = max_out - 1; break; }
        pos += (uint32_t)w;
    }
    if (cur && pos + 1 < max_out) {
        int w = snprintf(out + pos, max_out - pos, "%s", cur);
        if (w > 0) pos += ((uint32_t)w < max_out - pos) ? (uint32_t)w : max_out - 1 - pos;
    }
    out[pos < max_out ? pos : max_out - 1] = 0;
    return pos;
}

static void session_print(const ChatSession* s) {
    if (s->count == 0) { printf("  (empty session)\n"); return; }
    uint32_t start = (s->head + HISTORY_MAX - s->count) % HISTORY_MAX;
    for (uint32_t i = 0; i < s->count; i++) {
        const ChatTurn* t = &s->turns[(start + i) % HISTORY_MAX];
        printf("  [%u] you: %.120s\n", i + 1, t->user_text);
        printf("      bot: %.120s\n", t->bot_text);
    }
    printf("  (count=%u total=%u context_turns=%u)\n",
           s->count, s->total, s->context_turns);
}

static int session_save(const ChatSession* s, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fprintf(f, "# canvas-chat-session v1\n");
    fprintf(f, "# count=%u context_turns=%u\n", s->count, s->context_turns);
    uint32_t start = (s->head + HISTORY_MAX - s->count) % HISTORY_MAX;
    for (uint32_t i = 0; i < s->count; i++) {
        const ChatTurn* t = &s->turns[(start + i) % HISTORY_MAX];
        fprintf(f, "--turn--\n");
        fprintf(f, "user:%s\n", t->user_text);
        fprintf(f, "bot:%s\n",  t->bot_text);
    }
    fclose(f);
    printf("  [session] saved %u turns to %s\n", s->count, path);
    return 0;
}

static int session_load(ChatSession* s, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    session_reset(s);
    char line[LINE_BUF];
    char pending_user[LINE_BUF]; pending_user[0] = 0;
    int has_user = 0;
    uint32_t loaded = 0;
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (L == 0 || line[0] == '#') continue;
        if (!strcmp(line, "--turn--")) { has_user = 0; pending_user[0] = 0; continue; }
        if (!strncmp(line, "user:", 5)) {
            snprintf(pending_user, sizeof pending_user, "%s", line + 5);
            has_user = 1;
        } else if (!strncmp(line, "bot:", 4)) {
            if (has_user) {
                session_push(s, pending_user, line + 4);
                loaded++;
            }
            has_user = 0; pending_user[0] = 0;
        }
    }
    fclose(f);
    printf("  [session] loaded %u turns from %s\n", loaded, path);
    return 0;
}

/* ───────────────────────────── query router ─────────────────────────────── */

typedef enum {
    QUERY_EMPTY = 0,
    QUERY_COMMAND,    /* :foo */
    QUERY_CHAT,       /* default / :gen default */
    QUERY_RETRIEVE,   /* /ret or :retr default */
    QUERY_GENERATE,   /* /gen — generation without history */
    QUERY_BOTH,       /* :both default */
    QUERY_IMAGE,      /* /img — image route */
    QUERY_TOPK        /* /topk[N] */
} QueryType;

typedef struct {
    QueryType type;
    const char* body;   /* pointer into input (after prefix) */
    int topk_n;         /* only meaningful for QUERY_TOPK */
} QueryRequest;

/* Skip leading whitespace, return pointer into original string. */
static const char* skip_ws(const char* s) {
    while (s && *s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

/* Classify without mutating input. default_mode is what bare text maps to. */
static QueryRequest query_classify(const char* line, QueryType default_mode) {
    QueryRequest r = { QUERY_EMPTY, line, 5 };
    if (!line || !*line) return r;
    if (line[0] == ':') { r.type = QUERY_COMMAND; r.body = line; return r; }
    if (line[0] == '/') {
        if (!strncmp(line, "/gen", 4) && (line[4] == 0 || line[4] == ' ')) {
            r.type = QUERY_GENERATE; r.body = skip_ws(line + 4); return r;
        }
        if (!strncmp(line, "/ret", 4) && (line[4] == 0 || line[4] == ' ')) {
            r.type = QUERY_RETRIEVE; r.body = skip_ws(line + 4); return r;
        }
        if (!strncmp(line, "/img", 4) && (line[4] == 0 || line[4] == ' ')) {
            r.type = QUERY_IMAGE; r.body = skip_ws(line + 4); return r;
        }
        if (!strncmp(line, "/topk", 5)) {
            const char* p = line + 5;
            int n = 0;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
            if (n < 1) n = 5;
            if (n > 16) n = 16;
            r.type = QUERY_TOPK; r.topk_n = n; r.body = skip_ws(p); return r;
        }
        /* unknown /x — treat as chat so users don't silently lose input */
    }
    r.type = default_mode;
    r.body = line;
    return r;
}

/* ───────────────────────── options + handlers ───────────────────────────── */

typedef struct {
    const char* img_bin;    /* path to external image generator */
    const char* session_path;
} ChatOptions;

static double now_sec(void) {
    /* Use clock() for progress logging only — avoids timespec_get /
     * clock_gettime portability gaps across mingw-w64 runtimes. Matches
     * stream_train.c. */
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static void print_topk(SpatialAI* ai, const char* text, uint32_t k) {
    SpatialGrid* g = grid_create();
    layers_encode_clause(text, NULL, g);
    update_rgb_directional(g);
    apply_ema_to_grid(ai, g);

    MatchContext ctx; memset(&ctx, 0, sizeof ctx);
    ctx.bucket_idx = &ai->bucket_idx;
    MatchResult r = spatial_match(ai, g, MATCH_PREDICT, &ctx);
    grid_destroy(g);

    if (k > r.topk_count) k = r.topk_count;
    printf("  top-%u matches:\n", k);
    for (uint32_t i = 0; i < k; i++) {
        uint32_t id = r.topk[i].id;
        if (id >= ai->kf_count) continue;
        const char* label = ai->keyframes[id].label;
        char decoded[256];
        uint32_t nb = grid_decode_text_utf8(&ai->keyframes[id].grid,
                                            decoded, sizeof decoded - 1);
        decoded[nb] = 0;
        printf("   %u. sim=%.4f  id=%u  label=\"%.40s\"\n       decode=\"%.80s\"\n",
               i + 1, r.topk[i].score, id,
               (label && label[0]) ? label : "(none)", decoded);
    }
}

/* CHAT: generate with history prepended. Updates session on success. */
static int handle_chat(SpatialAI* ai, ChatSession* s,
                       const QueryRequest* req, const ChatOptions* opts) {
    (void)opts;
    if (!req->body || !*req->body) return 0;

    static char ctx[CTX_BUF];
    session_build_context(s, req->body, ctx, sizeof ctx);

    char out[OUT_BUF];
    float sim = 0.0f;
    double t0 = now_sec();
    uint32_t n = ai_generate_next(ai, ctx, out, sizeof out - 1, &sim);
    out[n < sizeof out ? n : sizeof out - 1] = 0;
    double dt = now_sec() - t0;

    if (n == 0) {
        printf("  [no response — empty generation]\n");
        session_push(s, req->body, "");
    } else {
        printf("  bot: %s\n", out);
        session_push(s, req->body, out);
    }
    printf("  (sim=%.4f, %u bytes, %.1f ms, ctx=%u turns)\n",
           sim, n, dt * 1000.0,
           s->count < s->context_turns ? s->count : s->context_turns);
    return 0;
}

/* GENERATE: raw generate, no history context, do not update session. */
static int handle_generate(SpatialAI* ai, ChatSession* s,
                           const QueryRequest* req, const ChatOptions* opts) {
    (void)s; (void)opts;
    if (!req->body || !*req->body) return 0;
    char out[OUT_BUF];
    float sim = 0.0f;
    double t0 = now_sec();
    uint32_t n = ai_generate_next(ai, req->body, out, sizeof out - 1, &sim);
    out[n < sizeof out ? n : sizeof out - 1] = 0;
    double dt = now_sec() - t0;
    if (n == 0) printf("  [no response — empty generation]\n");
    else        printf("  bot: %s\n", out);
    printf("  (sim=%.4f, %u bytes, %.1f ms, no-history)\n", sim, n, dt * 1000.0);
    return 0;
}

/* RETRIEVE: show matched keyframe + decode. */
static int handle_retrieve(SpatialAI* ai, ChatSession* s,
                           const QueryRequest* req, const ChatOptions* opts) {
    (void)s; (void)opts;
    if (!req->body || !*req->body) return 0;
    float sim = 0.0f;
    double t0 = now_sec();
    uint32_t id = ai_predict(ai, req->body, &sim);
    double dt = now_sec() - t0;
    if (id >= ai->kf_count) { printf("  [no retrieval hit]\n"); return 0; }
    char decoded[256];
    uint32_t n = grid_decode_text_utf8(&ai->keyframes[id].grid,
                                       decoded, sizeof decoded - 1);
    decoded[n] = 0;
    printf("  retrieved kf=%u sim=%.4f (%.1f ms)\n    decode=\"%s\"\n",
           id, sim, dt * 1000.0, decoded);
    return 0;
}

static int handle_topk(SpatialAI* ai, ChatSession* s,
                       const QueryRequest* req, const ChatOptions* opts) {
    (void)s; (void)opts;
    if (!req->body || !*req->body) { printf("  /topk needs text\n"); return 0; }
    print_topk(ai, req->body, (uint32_t)req->topk_n);
    return 0;
}

/* Replace characters that would break a shelled-out command invocation.
 * Keeps the stub safe without pulling in a full argv exec helper. */
static void sanitize_for_shell(char* s) {
    for (; *s; s++) {
        if (*s == '"' || *s == '\'' || *s == '`' ||
            *s == '$' || *s == '\\' || *s == '\n' || *s == '\r') *s = ' ';
    }
}

/* IMAGE: routes to an external img-canvas binary. Contract (stub):
 *     <bin> --prompt "<text>"
 * Binary is taken from --img-bin or env IMG_CANVAS_BIN. If unset, prints
 * what would have been dispatched so the wiring is obvious. */
static int handle_image(SpatialAI* ai, ChatSession* s,
                        const QueryRequest* req, const ChatOptions* opts) {
    (void)ai; (void)s;
    if (!req->body || !*req->body) { printf("  /img needs a prompt\n"); return 0; }

    const char* bin = (opts && opts->img_bin && *opts->img_bin)
                      ? opts->img_bin
                      : getenv("IMG_CANVAS_BIN");

    if (!bin || !*bin) {
        printf("  [image route] not configured.\n");
        printf("    prompt: %s\n", req->body);
        printf("    wire it: --img-bin <path> or set IMG_CANVAS_BIN\n");
        printf("    contract: <bin> --prompt \"<text>\"  (stdout = image path or base64)\n");
        return 0;
    }

    char safe[LINE_BUF];
    snprintf(safe, sizeof safe, "%s", req->body);
    sanitize_for_shell(safe);

    char cmd[LINE_BUF * 2];
    snprintf(cmd, sizeof cmd, "\"%s\" --prompt \"%s\"", bin, safe);
    printf("  [image] %s\n", cmd);
    double t0 = now_sec();
    int rc = system(cmd);
    printf("  [image] exit=%d, %.1f ms\n", rc, (now_sec() - t0) * 1000.0);
    return rc;
}

/* ─────────────────────────── dispatch table ─────────────────────────────── */

typedef int (*QueryHandler)(SpatialAI*, ChatSession*,
                            const QueryRequest*, const ChatOptions*);
typedef struct {
    QueryType    type;
    const char*  name;
    QueryHandler fn;
} Route;

static const Route g_routes[] = {
    { QUERY_CHAT,     "chat",     handle_chat     },
    { QUERY_GENERATE, "generate", handle_generate },
    { QUERY_RETRIEVE, "retrieve", handle_retrieve },
    { QUERY_TOPK,     "topk",     handle_topk     },
    { QUERY_IMAGE,    "image",    handle_image    },
};

static int dispatch(QueryType t, SpatialAI* ai, ChatSession* s,
                    const QueryRequest* req, const ChatOptions* opts) {
    for (size_t i = 0; i < sizeof g_routes / sizeof g_routes[0]; i++) {
        if (g_routes[i].type == t) return g_routes[i].fn(ai, s, req, opts);
    }
    printf("  [router] no handler for type %d\n", (int)t);
    return -1;
}

/* BOTH default: run retrieve then chat. Kept out of the table because it
 * composes two routes rather than mapping to one. */
static void handle_both(SpatialAI* ai, ChatSession* s,
                        const QueryRequest* req, const ChatOptions* opts) {
    QueryRequest rr = *req; rr.type = QUERY_RETRIEVE;
    dispatch(QUERY_RETRIEVE, ai, s, &rr, opts);
    QueryRequest rc = *req; rc.type = QUERY_CHAT;
    dispatch(QUERY_CHAT, ai, s, &rc, opts);
}

/* ─────────────────────────── model load/train ───────────────────────────── */

static void usage(const char* prog) {
    fprintf(stderr,
        "usage: %s [--load <path>] [--train <path> [--max N]]\n"
        "         [--session <path>] [--ctx N] [--img-bin <path>]\n"
        "  --load    <path>  load a .spai model\n"
        "  --train   <path>  train from line-per-clause text\n"
        "  --max     <N>     cap training clauses (default 5000)\n"
        "  --session <path>  load/save chat session (also :save/:load)\n"
        "  --ctx     <N>     history turns to prepend (default 3, max %d)\n"
        "  --img-bin <path>  external image generator (or env IMG_CANVAS_BIN)\n",
        prog, HISTORY_MAX);
}

static SpatialAI* load_model(const char* path) {
    SpaiStatus st = SPAI_OK;
    fprintf(stderr, "[chat] loading model: %s\n", path);
    double t0 = now_sec();
    SpatialAI* ai = ai_load(path, &st);
    if (!ai) {
        fprintf(stderr, "[chat] ai_load failed: %s\n", spai_status_str(st));
        return NULL;
    }
    fprintf(stderr, "[chat] loaded KF=%u Delta=%u in %.2fs\n",
            ai->kf_count, ai->df_count, now_sec() - t0);
    return ai;
}

static SpatialAI* train_model(const char* path, uint32_t max_clauses) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    SpatialAI* ai = spatial_ai_create();
    char line[LINE_BUF];
    uint32_t n = 0;
    double t0 = now_sec();
    fprintf(stderr, "[chat] training from %s (max %u)...\n", path, max_clauses);
    while (n < max_clauses && fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r' ||
                     line[L-1] == ' '  || line[L-1] == '\t')) line[--L] = 0;
        if (L < 10) continue;
        if (line[0] == '<') continue;
        ai_store_auto(ai, line, NULL);
        if (++n % 500 == 0) {
            fprintf(stderr, "  %u / %u  KF=%u Delta=%u\n",
                    n, max_clauses, ai->kf_count, ai->df_count);
        }
    }
    fclose(f);
    fprintf(stderr, "[chat] trained %u clauses in %.2fs (KF=%u, Delta=%u)\n",
            n, now_sec() - t0, ai->kf_count, ai->df_count);
    return ai;
}

static void print_help(void) {
    printf(
        "  :q :quit                  quit\n"
        "  :help                     this message\n"
        "  :gen | :retr | :both      default mode for un-prefixed input\n"
        "  :topk [N]                 print top-N for next input (default 5)\n"
        "  :history                  show session turns\n"
        "  :reset                    clear session\n"
        "  :save <path>              save session to file\n"
        "  :load <path>              load session from file\n"
        "  :ctx <N>                  set history turns prepended (0..%d)\n"
        "\n"
        "  /gen <text>               force generation (no history)\n"
        "  /ret <text>               retrieval only\n"
        "  /img <text>               route to img-canvas binary\n"
        "  /topk[N] <text>           top-N matches for <text>\n"
        "\n"
        "  bare text → current default mode (starts as :gen with history)\n",
        HISTORY_MAX);
}

/* ─────────────────────────── REPL ───────────────────────────────────────── */

int main(int argc, char** argv) {
#ifdef _WIN32
    setvbuf(stdout, NULL, _IONBF, 0);
#endif

    const char* load_path    = NULL;
    const char* train_path   = NULL;
    const char* session_path = NULL;
    const char* img_bin      = NULL;
    uint32_t    max_clauses  = 5000;
    int         ctx_turns_cli = -1;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--load")    && i + 1 < argc) load_path    = argv[++i];
        else if (!strcmp(argv[i], "--train")   && i + 1 < argc) train_path   = argv[++i];
        else if (!strcmp(argv[i], "--max")     && i + 1 < argc) max_clauses  = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--session") && i + 1 < argc) session_path = argv[++i];
        else if (!strcmp(argv[i], "--ctx")     && i + 1 < argc) ctx_turns_cli = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--img-bin") && i + 1 < argc) img_bin      = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); usage(argv[0]); return 2; }
    }
    if (!load_path && !train_path) { usage(argv[0]); return 2; }

    SpatialAI* ai = load_path ? load_model(load_path) : train_model(train_path, max_clauses);
    if (!ai) return 1;
    if (ai->kf_count == 0) {
        fprintf(stderr, "[chat] model has no keyframes; nothing to chat about\n");
        spatial_ai_destroy(ai);
        return 1;
    }

    ChatSession session; session_init(&session);
    if (ctx_turns_cli >= 0) {
        if (ctx_turns_cli > HISTORY_MAX) ctx_turns_cli = HISTORY_MAX;
        session.context_turns = (uint32_t)ctx_turns_cli;
    }
    if (session_path) session_load(&session, session_path);

    ChatOptions opts = { img_bin, session_path };

    QueryType default_mode = QUERY_CHAT;
    int pending_topk = 0;
    int topk_n = 5;

    printf("\nCANVAS chat — KF=%u Delta=%u. Type :help for commands, :q to quit.\n",
           ai->kf_count, ai->df_count);
    printf("  default=%s  ctx=%u  img-bin=%s\n\n",
           default_mode == QUERY_CHAT ? "chat" :
           default_mode == QUERY_RETRIEVE ? "retr" : "both",
           session.context_turns,
           (opts.img_bin && *opts.img_bin) ? opts.img_bin :
           (getenv("IMG_CANVAS_BIN") ? getenv("IMG_CANVAS_BIN") : "(unset)"));
    fflush(stdout);

    char line[LINE_BUF];
    while (1) {
        printf("you> "); fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r' ||
                     line[L-1] == ' '  || line[L-1] == '\t')) line[--L] = 0;
        if (L == 0) continue;

        /* colon commands intercept before routing */
        if (line[0] == ':') {
            if      (!strcmp(line, ":q") || !strcmp(line, ":quit")) break;
            else if (!strcmp(line, ":help")) print_help();
            else if (!strcmp(line, ":gen"))  { default_mode = QUERY_CHAT;     printf("  [mode: chat]\n"); }
            else if (!strcmp(line, ":retr")) { default_mode = QUERY_RETRIEVE; printf("  [mode: retr]\n"); }
            else if (!strcmp(line, ":both")) { default_mode = QUERY_BOTH;     printf("  [mode: both]\n"); }
            else if (!strncmp(line, ":topk", 5)) {
                int n = 5;
                if (line[5] == ' ') n = atoi(line + 6);
                if (n < 1) n = 1;
                if (n > 16) n = 16;
                topk_n = n; pending_topk = 1;
                printf("  [topk=%d armed — next input will print %d candidates]\n", n, n);
            }
            else if (!strcmp(line, ":history")) session_print(&session);
            else if (!strcmp(line, ":reset"))   { session_reset(&session); printf("  [session reset]\n"); }
            else if (!strncmp(line, ":save", 5)) {
                const char* p = line[5] == ' ' ? line + 6 : session_path;
                if (!p || !*p) printf("  :save needs a path (or start with --session)\n");
                else            session_save(&session, p);
            }
            else if (!strncmp(line, ":load", 5)) {
                const char* p = line[5] == ' ' ? line + 6 : session_path;
                if (!p || !*p) printf("  :load needs a path (or start with --session)\n");
                else            session_load(&session, p);
            }
            else if (!strncmp(line, ":ctx", 4)) {
                if (line[4] == ' ') {
                    int v = atoi(line + 5);
                    if (v < 0) v = 0;
                    if (v > HISTORY_MAX) v = HISTORY_MAX;
                    session.context_turns = (uint32_t)v;
                    printf("  [ctx=%u]\n", session.context_turns);
                } else {
                    printf("  ctx=%u\n", session.context_turns);
                }
            }
            else printf("  unknown command: %s (try :help)\n", line);
            continue;
        }

        QueryRequest req = query_classify(line, default_mode);

        if (pending_topk) {
            print_topk(ai, req.body, (uint32_t)topk_n);
            pending_topk = 0;
        }

        if (req.type == QUERY_BOTH) { handle_both(ai, &session, &req, &opts); continue; }
        dispatch(req.type, ai, &session, &req, &opts);
    }

    if (session_path && session.count > 0) session_save(&session, session_path);
    spatial_ai_destroy(ai);
    printf("bye.\n");
    return 0;
}
