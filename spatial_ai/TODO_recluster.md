# TODO: 학습 후 키프레임 재배열 (Re-cluster)

> **저장소**: https://github.com/sjpupro-lab/CANVAS.git
> **브랜치**: main
> **상태**: 설계 완료, 미구현
> **목적**: 학습 완료 후 키프레임을 유사도 기준으로 클러스터링하고, 클러스터 내 앵커 1개만 키프레임으로 남기고 나머지를 델타로 변환

---

## 문제

학습 중 키프레임은 시간순(도착 순서)으로 저장된다.

```
시간순 저장:
KF#0    "Anarchism is a political philosophy..."     (1번째)
KF#1    "Albert Einstein was a theoretical..."       (2번째)
KF#2    "The cat eats rice."                         (3번째)
...
KF#2000 "Anarchism advocates for the replacement..." (20,000번째)
KF#2001 "Einstein developed the theory of..."        (20,001번째)
```

KF#0과 KF#2000은 같은 토픽인데, 학습 시점에 KF#0이 아직 저장되지 않은 상태에서는 비교가 안 됐거나, 사이에 수천 개의 다른 KF가 끼어 있어서 유사도가 threshold 밑으로 떨어졌을 수 있다.

결과: 비슷한 절이 각각 독립 키프레임(320KB씩)으로 저장됨. 델타 비율이 낮고 모델이 거대해짐.

---

## 해결: 학습 후 재배열

학습이 끝나면 모든 데이터가 있다. 전체를 한 번 보고 최적 배열을 결정한다.

### 핵심 알고리즘

```
1. 모든 키프레임을 A채널 코사인으로 상호 비교
2. 유사도가 높은 것끼리 클러스터로 묶기
3. 클러스터마다 앵커 1개 선정 (A 활성 셀이 가장 많은 것)
4. 나머지는 앵커 대비 델타로 변환
5. 모델 재저장
```

### 단계별 상세

**Step 1: 클러스터링**

greedy 방식으로 충분하다. KF 수만 개를 N² 비교하면 느리니까, overlap coarse + top-K 사용.

```c
void ai_recluster(SpatialAI *ai, float cluster_threshold) {
    if (!ai || ai->kf_count < 2) return;

    uint32_t n = ai->kf_count;

    /* 각 KF의 클러스터 ID. -1 = 미배정 */
    int32_t *cluster_id = calloc(n, sizeof(int32_t));
    for (uint32_t i = 0; i < n; i++) cluster_id[i] = -1;

    uint32_t num_clusters = 0;

    /* 앵커 목록: cluster_anchor[c] = KF index */
    uint32_t *cluster_anchor = calloc(n, sizeof(uint32_t));

    for (uint32_t i = 0; i < n; i++) {
        if (cluster_id[i] >= 0) continue;  /* 이미 배정됨 */

        /* 새 클러스터 시작. i가 앵커 */
        uint32_t c = num_clusters++;
        cluster_id[i] = (int32_t)c;
        cluster_anchor[c] = i;

        /* i와 유사한 나머지 KF를 같은 클러스터에 배정 */
        for (uint32_t j = i + 1; j < n; j++) {
            if (cluster_id[j] >= 0) continue;

            float sim = cosine_a_only(
                &ai->keyframes[i].grid,
                &ai->keyframes[j].grid
            );
            if (sim >= cluster_threshold) {
                cluster_id[j] = (int32_t)c;
            }
        }
    }

    /* Step 2: 클러스터별 앵커 선정 (가장 활성 셀 많은 KF) */
    for (uint32_t c = 0; c < num_clusters; c++) {
        uint32_t best_active = 0;
        uint32_t best_idx = cluster_anchor[c];

        for (uint32_t i = 0; i < n; i++) {
            if (cluster_id[i] != (int32_t)c) continue;
            uint32_t active = grid_active_count(&ai->keyframes[i].grid);
            if (active > best_active) {
                best_active = active;
                best_idx = i;
            }
        }
        cluster_anchor[c] = best_idx;
    }

    /* Step 3: 앵커가 아닌 KF를 델타로 변환 */
    uint32_t converted = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = (uint32_t)cluster_id[i];
        uint32_t anchor = cluster_anchor[c];
        if (i == anchor) continue;  /* 앵커는 키프레임 유지 */

        /* KF#i → 앵커 대비 델타 생성 */
        DeltaEntry *entries = malloc(GRID_TOTAL * sizeof(DeltaEntry));
        if (!entries) continue;

        uint32_t delta_count = compute_delta(
            &ai->keyframes[anchor].grid,
            &ai->keyframes[i].grid,
            entries, GRID_TOTAL
        );

        if (!ensure_df_capacity(ai)) { free(entries); continue; }

        DeltaFrame *df = &ai->deltas[ai->df_count];
        df->id = ai->df_count;
        df->parent_id = anchor;
        strncpy(df->label, ai->keyframes[i].label, 63);
        df->label[63] = '\0';
        df->count = delta_count;

        if (delta_count > 0) {
            DeltaEntry *shrunk = realloc(entries, delta_count * sizeof(DeltaEntry));
            df->entries = shrunk ? shrunk : entries;
        } else {
            free(entries);
            df->entries = NULL;
        }
        df->change_ratio = grid_active_count(&ai->keyframes[i].grid)
            ? (float)delta_count / (float)grid_active_count(&ai->keyframes[i].grid) : 0;

        ai->df_count++;
        converted++;

        /* KF#i를 "제거 대상"으로 마킹 */
        cluster_id[i] = -2;  /* -2 = 델타로 변환됨 */
    }

    /* Step 4: 앵커만 남기고 키프레임 배열 압축 */
    uint32_t write = 0;
    uint32_t *id_remap = calloc(n, sizeof(uint32_t));

    for (uint32_t i = 0; i < n; i++) {
        if (cluster_id[i] == -2) {
            /* 델타로 변환된 KF — grid 메모리 해제 */
            SpatialGrid *g = &ai->keyframes[i].grid;
            if (g->A) { free(g->A); g->A = NULL; }
            if (g->R) { free(g->R); g->R = NULL; }
            if (g->G) { free(g->G); g->G = NULL; }
            if (g->B) { free(g->B); g->B = NULL; }
            continue;
        }
        /* 앵커 KF — 유지 */
        id_remap[i] = write;
        if (write != i) {
            ai->keyframes[write] = ai->keyframes[i];
            ai->keyframes[write].id = write;
        }
        write++;
    }
    ai->kf_count = write;

    /* Step 5: 델타의 parent_id를 새 인덱스로 갱신 */
    for (uint32_t d = 0; d < ai->df_count; d++) {
        uint32_t old_parent = ai->deltas[d].parent_id;
        if (old_parent < n) {
            ai->deltas[d].parent_id = id_remap[old_parent];
        }
    }

    printf("[recluster] %u clusters, %u anchors kept, %u converted to delta\n",
           num_clusters, write, converted);
    printf("[recluster] KF: %u -> %u, Delta: +%u\n",
           n, ai->kf_count, converted);

    free(cluster_id);
    free(cluster_anchor);
    free(id_remap);
}
```

---

## 속도 문제: O(N²) 회피

KF가 20,000개면 N² = 4억 비교. 느리다.

### 해결: 버킷 + 토픽 사전 필터

```c
/* 같은 topic_hash를 가진 KF끼리만 비교 */
for (uint32_t i = 0; i < n; i++) {
    if (cluster_id[i] >= 0) continue;

    uint32_t c = num_clusters++;
    cluster_id[i] = (int32_t)c;
    cluster_anchor[c] = i;

    uint32_t topic = ai->keyframes[i].topic_hash;

    for (uint32_t j = i + 1; j < n; j++) {
        if (cluster_id[j] >= 0) continue;

        /* 토픽이 다르면 스킵 — O(1) 체크 */
        if (ai->keyframes[j].topic_hash != topic) continue;

        float sim = cosine_a_only(
            &ai->keyframes[i].grid,
            &ai->keyframes[j].grid
        );
        if (sim >= cluster_threshold) {
            cluster_id[j] = (int32_t)c;
        }
    }
}
```

같은 토픽(같은 위키 문서)끼리만 비교하면 비교 횟수가 1/100 이하로 줄어든다.

---

## 헤더 선언

```c
/* include/spatial_keyframe.h */

/* 학습 후 키프레임 재배열.
 * 유사한 KF를 클러스터링하고, 클러스터 앵커만 KF로 남기고
 * 나머지를 델타로 변환.
 * cluster_threshold: 같은 클러스터로 묶을 최소 코사인 유사도 (예: 0.15) */
void ai_recluster(SpatialAI *ai, float cluster_threshold);
```

---

## stream_train.c 호출

학습 끝 → EMA repaint → recluster → 저장:

```c
/* tools/stream_train.c — ingest 루프 끝난 뒤 */

printf("[repaint] updating %u keyframes with final EMA...\n", ai->kf_count);
ai_repaint_ema(ai);

printf("[recluster] re-arranging keyframes (threshold=%.3f)...\n", threshold);
ai_recluster(ai, threshold);

/* 저장 */
ai_save(ai, save_path);
```

### CLI 옵션

```
--no-recluster    재배열 비활성화 (디버깅용)
--cluster-threshold 0.15   클러스터 임계값 직접 지정 (기본: 학습 시 calibrate된 threshold 사용)
```

---

## 기대 효과

```
현재 (시간순, 5000절):
  KF = 4,649  Delta = 351  (delta 7%)
  모델 = 1,525 MB

재배열 후 (예상):
  클러스터 ~500개 → 앵커 500 KF + 델타 4,149
  KF = 500   Delta = 4,500  (delta 90%)
  모델 = 500 × 320KB + 4500 × ~5KB ≈ 183 MB

압축률: 1,525 MB → ~183 MB (88% 감소)
```

---

## EMA repaint과의 관계

repaint 먼저, recluster 나중:

```
1. ai_repaint_ema()   — 모든 KF의 R/G/B를 최종 EMA로 갱신
2. ai_recluster()     — 갱신된 R/G/B 기준으로 유사도 비교 → 클러스터링
3. ai_save()          — 최종 모델 저장
```

repaint를 먼저 하면 초반/후반 KF의 R/G/B가 통일되어 코사인 유사도가 더 정확해지고, 클러스터링 품질이 올라간다.

---

## 검증

- [ ] `make test` 전체 PASS
- [ ] recluster 후 KF 수 < recluster 전 KF 수
- [ ] recluster 후 Delta 수 > recluster 전 Delta 수
- [ ] 델타의 parent_id가 유효한 KF 인덱스를 가리킴
- [ ] recluster 후 모델 저장/로드 정상
- [ ] verify unseen 유사도가 recluster 전과 같거나 높음
- [ ] 모델 파일 크기가 recluster 전보다 작음
- [ ] stream_train 출력에 `[recluster]` 로그 표시
