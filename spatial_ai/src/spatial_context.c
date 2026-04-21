#include "spatial_context.h"
#include "spatial_layers.h"
#include <string.h>
#include <stdio.h>

#define CTX_INITIAL_CAP 64

/* ── LRU Cache ── */

void cache_init(FrameCache* fc) {
    if (!fc) return;
    memset(fc, 0, sizeof(FrameCache));
}

SpatialGrid* cache_get(FrameCache* fc, uint32_t frame_id) {
    if (!fc) return NULL;

    for (uint32_t i = 0; i < fc->count; i++) {
        if (fc->entries[i].frame_id == frame_id && fc->entries[i].grid != NULL) {
            fc->entries[i].access_order = ++fc->clock;
            return fc->entries[i].grid;
        }
    }
    return NULL;
}

void cache_put(FrameCache* fc, uint32_t frame_id, SpatialGrid* grid) {
    if (!fc || !grid) return;

    /* Check if already present */
    for (uint32_t i = 0; i < fc->count; i++) {
        if (fc->entries[i].frame_id == frame_id) {
            fc->entries[i].access_order = ++fc->clock;
            return;
        }
    }

    if (fc->count < CACHE_SIZE) {
        /* Empty slot available */
        CacheEntry* e = &fc->entries[fc->count];
        e->frame_id = frame_id;
        e->grid = grid;
        e->access_order = ++fc->clock;
        fc->count++;
    } else {
        /* Evict LRU */
        uint32_t min_order = UINT32_MAX;
        uint32_t min_idx = 0;
        for (uint32_t i = 0; i < fc->count; i++) {
            if (fc->entries[i].access_order < min_order) {
                min_order = fc->entries[i].access_order;
                min_idx = i;
            }
        }
        /* Note: we don't free the evicted grid since it's owned by keyframes */
        fc->entries[min_idx].frame_id = frame_id;
        fc->entries[min_idx].grid = grid;
        fc->entries[min_idx].access_order = ++fc->clock;
    }
}

/* ── Context Manager ── */

ContextManager* context_create(void) {
    ContextManager* ctx = (ContextManager*)malloc(sizeof(ContextManager));
    if (!ctx) return NULL;

    ctx->frame_count = 0;
    ctx->frame_capacity = CTX_INITIAL_CAP;
    ctx->frames = (ContextFrame*)calloc(CTX_INITIAL_CAP, sizeof(ContextFrame));
    if (!ctx->frames) {
        free(ctx);
        return NULL;
    }

    cache_init(&ctx->cache);
    return ctx;
}

void context_destroy(ContextManager* ctx) {
    if (!ctx) return;

    if (ctx->frames) {
        for (uint32_t i = 0; i < ctx->frame_count; i++) {
            if (ctx->frames[i].grid) {
                grid_destroy(ctx->frames[i].grid);
            }
        }
        free(ctx->frames);
    }
    free(ctx);
}

/* Simple topic hash: djb2 on the topic string */
static uint32_t topic_hash(const char* topic) {
    if (!topic) return 0;
    uint32_t h = 5381;
    while (*topic) {
        h = h * 33 + (uint8_t)(*topic);
        topic++;
    }
    return h;
}

uint32_t context_add_frame(ContextManager* ctx, SpatialAI* ai,
                           const char* clause_text, const char* topic) {
    if (!ctx || !ai || !clause_text) return UINT32_MAX;

    /* Grow if needed */
    if (ctx->frame_count >= ctx->frame_capacity) {
        uint32_t new_cap = ctx->frame_capacity * 2;
        ContextFrame* new_frames = (ContextFrame*)realloc(
            ctx->frames, new_cap * sizeof(ContextFrame));
        if (!new_frames) return UINT32_MAX;
        ctx->frames = new_frames;
        memset(&ctx->frames[ctx->frame_capacity], 0,
               (new_cap - ctx->frame_capacity) * sizeof(ContextFrame));
        ctx->frame_capacity = new_cap;
    }

    /* Create grid for this frame */
    SpatialGrid* grid = grid_create();
    if (!grid) return UINT32_MAX;

    morpheme_init();
    layers_encode_clause(clause_text, NULL, grid);
    update_rgb_directional(grid);

    /* Determine frame type: check against existing keyframes */
    uint8_t frame_type = 0; /* I-frame by default */
    uint32_t parent_id = 0;

    if (ai->kf_count > 0) {
        float best_sim = -1.0f;
        for (uint32_t i = 0; i < ai->kf_count; i++) {
            float sim = cosine_a_only(grid, &ai->keyframes[i].grid);
            if (sim > best_sim) {
                best_sim = sim;
                parent_id = i;
            }
        }
        if (best_sim >= 0.3f) {
            frame_type = 1; /* P-frame */
        }
    }

    /* Store in AI engine */
    ai_store_auto(ai, clause_text, topic);

    /* Add context frame */
    uint32_t fid = ctx->frame_count;
    ContextFrame* cf = &ctx->frames[fid];
    cf->frame_id = fid;
    cf->topic_hash = topic_hash(topic);
    if (topic) strncpy(cf->topic_label, topic, 63);
    cf->topic_label[63] = '\0';
    cf->frame_type = frame_type;
    cf->parent_id = parent_id;
    cf->grid = grid;

    ctx->frame_count++;

    /* Add to cache */
    cache_put(&ctx->cache, fid, grid);

    return fid;
}

/* ── Integrated matching engine (SPEC-ENGINE final) ── */

uint32_t match_engine(SpatialAI* ai, SpatialGrid* input,
                      BucketIndex* bidx, BlockSummary* bs_all,
                      FrameCache* cache, float* out_sim) {
    if (!ai || !input || ai->kf_count == 0) {
        if (out_sim) *out_sim = 0.0f;
        return UINT32_MAX;
    }

    uint32_t n = ai->kf_count;
    Candidate* pool = (Candidate*)malloc(n * sizeof(Candidate));
    if (!pool) {
        if (out_sim) *out_sim = 0.0f;
        return UINT32_MAX;
    }
    uint32_t pool_size = 0;

    /* Step 1: Adaptive candidate selection */
    if (n < BUCKET_THRESHOLD) {
        /* Small-scale: full overlap scan */
        for (uint32_t i = 0; i < n; i++) {
            pool[i].id = i;
            pool[i].score = (float)overlap_score(input, &ai->keyframes[i].grid);
        }
        pool_size = n;
    } else if (bidx) {
        /* Large-scale: bucket → overlap */
        uint32_t cand_ids[1024];
        uint32_t cand_count = 0;
        uint32_t h = grid_hash(input);
        bucket_candidates(bidx, h, 5, cand_ids, &cand_count, 1024);

        if (cand_count < TOP_K) {
            /* Fallback: full scan */
            for (uint32_t i = 0; i < n; i++) {
                pool[i].id = i;
                pool[i].score = (float)overlap_score(input, &ai->keyframes[i].grid);
            }
            pool_size = n;
        } else {
            for (uint32_t i = 0; i < cand_count && i < 1024; i++) {
                pool[i].id = cand_ids[i];
                pool[i].score = (float)overlap_score(input, &ai->keyframes[cand_ids[i]].grid);
            }
            pool_size = (cand_count < 1024) ? cand_count : 1024;
        }
    } else {
        /* No bucket index: full scan */
        for (uint32_t i = 0; i < n; i++) {
            pool[i].id = i;
            pool[i].score = (float)overlap_score(input, &ai->keyframes[i].grid);
        }
        pool_size = n;
    }

    /* Top-K selection */
    topk_select(pool, pool_size, TOP_K);

    /* Step 2: Precise cosine with cache + block skip */
    BlockSummary inp_bs;
    compute_block_sums(input, &inp_bs);

    uint32_t best_id = 0;
    float best_sim = -1.0f;
    uint32_t eval_count = (pool_size < TOP_K) ? pool_size : TOP_K;

    for (uint32_t i = 0; i < eval_count; i++) {
        uint32_t fid = pool[i].id;

        /* Cache lookup */
        SpatialGrid* target = NULL;
        if (cache) {
            target = cache_get(cache, fid);
        }
        if (!target) {
            target = &ai->keyframes[fid].grid;
            if (cache) {
                cache_put(cache, fid, target);
            }
        }

        float sim;
        if (bs_all) {
            sim = cosine_block_skip(input, target, &inp_bs, &bs_all[fid]);
        } else {
            sim = cosine_rgb_weighted(input, target);
        }

        if (sim > best_sim) {
            best_sim = sim;
            best_id = fid;
        }
    }

    free(pool);

    if (out_sim) *out_sim = best_sim;
    return best_id;
}
