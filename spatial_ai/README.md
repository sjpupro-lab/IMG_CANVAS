# CANVAS

공간 패턴 기반 텍스트 학습 엔진. 문장을 256×256 격자의 밝기 패턴으로 바꿔 저장·검색·압축하는 C 라이브러리.

---

## 1. 전체 파이프라인

```
원시 텍스트 (절 단위 line)
        │
        ▼
┌──────────────────────────────────────┐
│ 1절 KF (256×256, 4채널 A/R/G/B)       │
│  ├─ layers_encode_clause              │
│  │    └─ POS seed (R/G/B), byte freq (A)
│  ├─ update_rgb_directional           │
│  ├─ apply_ema_to_grid                │
│  └─ ai_store_auto                    │
│       ├─ cos_a_q16 ≥ threshold → Δ   │
│       └─ 아니면 새 KF                 │
└──────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────┐
│ P-프레임 캔버스 (2048×1024 = 32 slot) │
│  ├─ pool_add_clause                  │
│  │   └─ DataType auto classify       │
│  │       (PROSE/DIALOG/CODE/SHORT)   │
│  ├─ canvas_update_rgb (diffusion)    │
│  ├─ canvas_compute_all_summaries     │
│  │   └─ (b_mean, hz_hist) per slot   │
│  └─ canvas_assign_freq_tags_clock    │
│       └─ RGBA ClockEngine → chapter   │
└──────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────┐
│ Post-training passes                  │
│  ├─ ai_repaint_ema                   │
│  ├─ ai_recluster (A+RGB 이중검증)     │
│  ├─ canvas_pool_recluster            │
│  │   └─ auto threshold (block-sum Q16)
│  └─ scene_change_classify (I/P)       │
└──────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────┐
│ 저장 (.spai v7)                        │
│  ├─ KF: full grid (327 KB each)      │
│  ├─ Δ:  sparse entries (~수 KB each) │
│  ├─ I-frame canvas: full (10 MB)     │
│  ├─ P-frame canvas: delta vs parent  │
│  └─ SubtitleTrack: 인덱스             │
└──────────────────────────────────────┘
        │
        ▼
┌──────────────────────────────────────┐
│ Retrieval (ai_predict)                │
│  ├─ coarse: overlap + bucket index    │
│  ├─ top-K KF scoring                  │
│  └─ 그 자식 Δ를 apply_delta로 복원해   │
│     같은 metric으로 re-score (v7)     │
└──────────────────────────────────────┘
```

---

## 2. 모듈별 동작·용도

### 2.1 `spatial_grid` — 4채널 격자 컨테이너

- `SpatialGrid`: `A (uint16)` + `R/G/B (uint8)`, 256×256 = 65,536 셀
- A = byte 빈도 누적 (brightness)
- R/G/B = POS 기반 seed (semantic/function/context)
- 32-byte 정렬 allocation (SIMD 친화)

### 2.2 `spatial_layers` — 절 → 격자 인코딩

`layers_encode_clause(text, label, grid)`:
- **3-레이어 가중합 (A 채널)**:
  - Base: 모든 byte +1
  - Word: 공백 분리 단어 +5
  - Morpheme: 형태소 분석 후 content POS(NOUN/VERB/ADJ/UNKNOWN) +3
- **POS seed (R/G/B)**: 형태소 POS에 따라 다른 색 블렌드
  - NOUN (40,30,100), VERB (120,40,140), ADJ (170,35,180)
  - PARTICLE (8,85,90), ENDING (12,95,110), PUNCT (5,120,60)
  - UNKNOWN (210,20,200)

### 2.3 `spatial_match` — 코사인 + Q16 정수 경로

float 버전은 레거시로 유지, hot path는 Q16:

- `cos_a_q16(a, b)` → uint16 (0..65535)
  - A채널 cosine, 정수 누적 + isqrt 1회
  - sqrt/mul 없이 `dot * 65536 / isqrt(na*nb)` 구조
- `cos_rgb_weighted_q16(a, b)` → uint16
  - rgb_weight = `65536 - (128·dr + 77·dg + 51·db)` (Q16 가중치)
  - dot_w accumulates va·vb·w_q16, shift right 16 후 `q16_cosine` 호출
- `spatial_match(ai, grid, mode, ctx)` → MatchResult
  - Step 1 coarse: bucket index + overlap_score
  - Step 2 precise: top-K scoring (MATCH_PREDICT/SEARCH/QA/GENERATE)
  - **Step 3 (v7+)**: top-K KF의 자식 Δ를 reconstruct 후 re-score

### 2.4 `spatial_q8.h` — Q16 fixed-point 유틸

- `Q16_ONE = 65536` (캐시: uint16_t 최대 = 65535)
- `q16_from_float(f)` — `round(f · 65536)` clamp
- `q16_to_float(q)` — 리포팅 전용
- `isqrt_u64(n)` — Newton method, ≤6 iter
- `q16_cosine(dot, na, nb)` — 공용 cosine 헬퍼

### 2.5 `spatial_keyframe` — 1절 레벨 KF/Δ 관리

- `ai_store_auto(ai, text, label)`:
  - 인코딩 → `cos_a_q16` 상위 매칭 → 임계값 이상이면 Δ, 아니면 새 KF
- `ai_recluster(ai, threshold)`:
  - **A+RGB 이중검증** 클러스터링: `cos_a_q16` 통과 후 `cos_rgb_weighted_q16`도 통과한 쌍만 같은 클러스터
  - 앵커 선정: 자식 Δ 수 > active 셀 수
  - 비-앵커 KF → Δ로 강등
- `ai_repaint_ema(ai)`:
  - 모든 KF의 active 셀을 최종 EMA 평균으로 다시 칠함 (후반 KF와 초반 KF 색 통일)

### 2.6 `spatial_canvas` — 캔버스 + 슬롯 + 클록

`SpatialCanvas`: 2048×1024 = 32개의 256×256 슬롯 (8×4 배치)

**slot-level compute summary** (각 슬롯당 3 bytes):
- `b_mean`: 슬롯 active 셀의 B채널 평균
- `hz_hist`: A채널 컬럼 4-bin 히스토그램 (nibble × 4 = uint16)

**RGBA ClockEngine** (캔버스당 256 KB):
- 4개 독립 채널 (R/G/B/A), 각 65,536 uint8 셀, init 255
- 공유 `pos` (0..65535 선형, 65536에서 wrap)
- 입력: 4개 값, 각각 자기 채널 cells를 pos부터 N만큼 연속 -1 소진
- 셀이 0되면 pos 전진, 아니면 같은 자리 유지

**Tick 입력 (slot i → i+1 전환마다)**:
- R = b_diff (b_mean[i-1] vs b_mean[i], 0..255)
- G = hz_diff (hz_hist nibble SAD, 0..60)
- B = hz_hist[i] 합 (b0+b1+b2+b3, 0..60)
- A = active_cells[i] × 256 (uint16, 0..65535)

**SAD 비교 — 합산 아니고 4개 독립 반환**:
```c
typedef struct {
    uint64_t R_sad, G_sad, B_sad, A_sad;
} RGBAClockSad;
```

**판정 규칙 (채널별 독립 해석)**:
| R (context) | G (chapter) | B (structure) | A (density) | 의미 |
|:-:|:-:|:-:|:-:|:--|
| 작음 | 작음 | — | — | 같은 챕터 |
| 작음 | 큼 | — | — | 챕터 전환 (같은 문맥, 새 챕터) |
| 큼 | 큼 | — | — | 완전히 다른 주제 |
| — | — | 큼 | — | 다른 데이터 타입/리듬 (대화↔코드 등) |
| — | — | — | 큼 | 정보량 차이 (긴 글↔짧은 글) |

**중요**: `R+G+B+A` 합산으로 축약하면 채널별 signal이 모두 손실됨. 항상 per-channel 분리 해석.

**freq_tag (캔버스 내 chapter 자동 분할)**:
- `canvas_assign_freq_tags_clock(c, g_threshold, rb_threshold, use_topic)`
- chapter 시작 시점의 엔진 snapshot 저장
- 매 slot 전환 후 `sad = rgba_clock_sad(snapshot, current)` 계산
- `sad.G_sad > g_threshold` → 새 chapter (챕터 전환 주요 신호)
- `sad.R_sad + sad.B_sad > rb_threshold` → 새 chapter (context + 구조 break)
- 같은 행 슬롯 간만 판정 (row 교차는 물리적 인접 아님)

**튜닝 결과 (wiki5k 기준 확정)**: `G=250, RB=150` → avg 4.04 chapters/canvas. 다른 코퍼스에서는 재튜닝 필요.

### 2.7 `spatial_subtitle` — 캔버스 풀 + 자막 + scene change

- `SpatialCanvasPool`: DataType별 캔버스 라우팅
- `pool_add_clause(p, text)`:
  - DataType 자동 감지
  - 같은 타입 + 빈 슬롯 있는 캔버스 찾기 (없으면 새로 생성)
  - 슬롯 가득 차면 → `canvas_update_rgb` → `canvas_compute_all_summaries` → `canvas_assign_freq_tags_clock` → `scene_change_classify`
- `SubtitleTrack`: (type, topic_hash, canvas_id, slot_id, byte_length) 인덱스
- `pool_match(p, grid, text)`: 4-step 검색 (type jump → A → RG → BA → fallback)
- `canvas_pool_recluster(p, threshold)`:
  - 같은 타입 캔버스 간 block-sum cosine (Q16)
  - greedy clustering → 앵커 = 기존 I-frame 우선, tiebreak active 셀 수
  - 비-앵커는 P-frame으로 재지정, parent 업데이트
- `canvas_pool_auto_threshold(p, target_merge)`:
  - pair 코사인 분포의 target_merge 백분위수를 threshold로 반환

### 2.8 `spatial_clock` — RGBA ClockEngine

독립 파일로 관리되는 태엽식 카운터 엔진. 설계와 구현:

- `rgba_clock_init(ce)` — 모든 셀 255 초기화
- `rgba_clock_tick(ce, r, g, b, a)` — 공유 pos 사용, 순차적으로 R→G→B→A 소진
- `rgba_clock_sad(a, b)` — 4개 채널별 독립 SAD 반환
- `rgba_clock_copy(dst, src)` — memcpy (snapshot용)

**드레인 동작 (`drain_channel`)**:
```
input N, pos p에서 시작:
while remaining > 0:
    take = min(cells[p], remaining)
    cells[p] -= take
    remaining -= take
    if cells[p] == 0: p = (p + 1) % 65536
*pos = p
```

**공유 pos 의미**: R이 먼저 drain → G는 R 후의 pos부터 → B → A. 각 채널의 drain 영역은 이전 채널들의 누적 입력에 의해 결정됨.

### 2.9 `spatial_io` — SPAI 바이너리 (현 버전 7)

**태그**:
- 0x01 Keyframe (full)
- 0x02 Delta
- 0x03 ChannelWeight
- 0x04 Canvas (full pixels) — I-frame, legacy
- 0x05 SubtitleTrack
- 0x06 EMA tables
- **0x07 Canvas Delta — P-frame sparse delta vs parent** (v7+)

**Save 경로**:
- P-frame + 유효한 `parent_canvas_id` → `write_canvas_delta_record` (sparse)
- 그 외 → `write_canvas_record` (full)

**Load 경로 — 2-pass 재구성**:
- Pass 1: 모든 레코드 순차 read. P-frame delta entries는 `pending_deltas[]` 배열에 보관, 픽셀은 비워둠
- Pass 2: `canvas_apply_full_delta(parent, entries, child)` 체인 resolve (최대 8 pass, parent가 P-frame이면 resolve 대기)

**저장 절감 (wiki5k 검증)**: full 3.2 GB → delta 1.6~1.7 GB (약 50% 축소).

---

## 3. 빌드

Windows (MSYS2 mingw64) 기준. Linux/make는 `make` 직접 사용 가능.

### 3.1 PowerShell 스크립트 (권장)

```powershell
cd C:\Users\canva\Desktop\CANVAS
$GCC = "C:\msys64\mingw64\bin\gcc.exe"
$CFLAGS = @('-Wall','-Wextra','-O2','-Iinclude','-std=gnu11')

$srcs = 'spatial_grid','spatial_morpheme','spatial_layers','spatial_match',
        'spatial_keyframe','spatial_context','spatial_generate','spatial_io',
        'spatial_canvas','spatial_subtitle','spatial_clock'
foreach ($s in $srcs) {
    & $GCC @CFLAGS -c "src/$s.c" -o "build/$s.o"
}

& $GCC @CFLAGS tools/stream_train.c `
    build/spatial_grid.o build/spatial_morpheme.o build/spatial_layers.o `
    build/spatial_match.o build/spatial_keyframe.o build/spatial_context.o `
    build/spatial_generate.o build/spatial_io.o build/spatial_canvas.o `
    build/spatial_subtitle.o build/spatial_clock.o `
    -o build/stream_train.exe -lm
```

### 3.2 Makefile (Linux/WSL)

```bash
make          # object files만
make test     # tests 전체 빌드+실행
make stream   # stream_train 빌드
```

**주의**: Makefile의 `-std=c11`은 mingw-w64에서 `timespec_get` 관련 이슈 발생. PowerShell 빌드는 `-std=gnu11` 사용.

---

## 4. 실행

### 4.1 기본 스트리밍 학습

```powershell
.\build\stream_train.exe --input data\wiki5k.txt --max 5000 --verify --save build\models\wiki5k.spai
```

### 4.2 주요 CLI 옵션

| 옵션 | 기본값 | 설명 |
|:--|:-:|:--|
| `--input <path>` | 필수 | 입력 텍스트 (절당 1줄) |
| `--max <N>` | 50000 | 최대 절 수 |
| `--save <path>` | `build/models/stream_auto.spai` | 출력 경로 |
| `--checkpoint <N>` | 5000 | N절마다 중간 저장 |
| `--threshold <F>` | 0.30 | ai_store_auto의 Δ/KF 결정 임계값 |
| `--target-delta <R>` | — | threshold 자동 보정 (Δ 비율 목표) |
| `--cluster-threshold <F>` | store threshold 재사용 | 후처리 KF recluster 임계 |
| `--canvas-target-merge <R>` | 0.5 | 캔버스 pool recluster auto-threshold 비율 |
| `--freq-tag-g-threshold <N>` | **250** | ClockEngine G채널 SAD 임계 (chapter 전환) |
| `--freq-tag-rb-threshold <N>` | **150** | ClockEngine R+B 합산 임계 (context 단절) |
| `--no-recluster` | off | 후처리 recluster 건너뜀 |
| `--verify` | off | 학습 후 미본 tail 500절 검색 품질 측정 |

---

## 5. 검증된 성능 지표 (wiki5k.txt, 5000 절)

| 지표 | 값 |
|:--|:-:|
| Ingest 속도 | 111~114 c/s (60s → **44s**, Q16 후 1.37× 빠름) |
| KF recluster | 29s → **14s** (Q16, 2.07× 빠름) |
| KF / Δ 분포 | 4,648 KF + 352 Δ |
| Canvas 수 | 159 (full 155) = 3 I-frame + 152 P-frame |
| DataType 분포 | PROSE 35, DIALOG 113, CODE 1, SHORT 10 |
| Chapters / canvas | avg **4.04**, max 5 (목표 3-5 적중) |
| Verify avg similarity | **0.1222** (float 대비 4자리 일치 — Q16 손실 없음) |
| Hits ≥0.10 (500 질의) | 344~345 (델타 포함 시 +1) |
| 저장 파일 크기 | full 3.2 GB → P-frame delta **1.6~1.7 GB** (50% 축소) |

---

## 6. 사업화·임베디드 준비도

- **hot path 정수만 사용**: `cos_a_q16`, `cos_rgb_weighted_q16`, `block_sum_cosine_q16`, `canvas_summary_sad`, RGBA ClockEngine tick/SAD — float 없음, sqrt는 호출당 1회 `isqrt_u64` (Newton, ≤6 iter)
- ARM NEON, x86 SIMD (PSADBW) 이식 가능한 구조
- 남은 float 영역: `cosine_a_only` (레거시 API, 비사용), `update_rgb_directional` 계수 (ALPHA/BETA/GAMMA), EMA tables → Step 4로 별도 track

---

## 7. 한계·특성

### 7.1 ClockEngine cross-canvas drift 감지 (영어 wiki)

wiki5k 검증 결과, same-type pair SAD 분포 (채널별 독립):

| 채널 | min | p50 | max | IQR ratio |
|:-:|:-:|:-:|:-:|:-:|
| R (ctx) | 148 | 265 | 615 | 1.2× |
| G (chp) | 52 | 112 | 243 | 1.25× |
| B (str) | 721 | 835 | 845 | **1.03× (사실상 상수)** |
| A (den) | 0 | 55k | 415k | **4× (유일한 실효 변별)** |

**해석**: 영어 wiki PROSE 캔버스는 모두 비슷한 R/G/B 누적 총합 → 클록엔진 drain 패턴이 비슷 → cross-canvas R/G/B 채널 SAD 낮음. A 채널만 정보 밀도 차이로 넓은 분포. **온라인 dedup용 signature로 단독 사용 부적합**. 다른 코퍼스(대화+코드+한국어 혼합)에서는 B 채널이 벌어질 것으로 예상.

### 7.2 freq_tag chapter 분리는 정상 작동

단일 canvas 내 slot 전환 SAD로 chapter 판정하는 경로는 **G=250, RB=150 default에서 avg 4 chapters 안정**. 이건 검증된 production 값.

### 7.3 델타 매칭 기여도 (ai_predict 확장)

wiki5k 기준 verify avg similarity 0.1222 → 0.1225 (+0.0003), hits ≥0.10 344 → 345. **인프라는 동작**, 영어 wiki에서는 이득 미미. 다른 코퍼스(델타 비율 높은)에서 효과 기대.

---

## 8. 파일 구조

```
CANVAS/
├─ include/           공개 헤더
│  ├─ spatial_grid.h
│  ├─ spatial_layers.h
│  ├─ spatial_morpheme.h
│  ├─ spatial_match.h
│  ├─ spatial_q8.h        Q16 정수 유틸 (파일명 legacy)
│  ├─ spatial_keyframe.h
│  ├─ spatial_context.h
│  ├─ spatial_generate.h
│  ├─ spatial_canvas.h
│  ├─ spatial_subtitle.h
│  ├─ spatial_clock.h     RGBA ClockEngine
│  └─ spatial_io.h
├─ src/               구현
├─ tools/
│  ├─ stream_train.c      학습 실행 파일
│  └─ chat.c              (미사용)
├─ tests/
│  ├─ test_*.c            단위 테스트 12개
│  ├─ bench_*.c           벤치마크
│  └─ test_wiki.c
├─ data/              학습 데이터 (wiki5k.txt 등)
├─ dict/              형태소 사전
├─ bench_reports/     벤치 결과
├─ build/             컴파일 산출물 (.o, .exe, models/)
├─ SPEC.md            원본 설계 스펙
├─ SPEC-ENGINE.md     엔진 최적화 로드맵
├─ TODO_recluster.md  recluster 설계 기록
└─ README.md         이 문서
```

---

## 9. 향후 작업 (Roadmap)

- **Step 4 (보류)**: 남은 float → Q16 변환 (spatial_match cascade, channel_sim, EMA)
- **드리프트 검증 확장**: 혼합 corpus (대화+코드)에서 ClockEngine 변별력 재측정
- **온라인 P-frame 재배치**: 스트리밍 중 새 캔버스 → 기존 캔버스 클록 비교하여 즉석 병합
- **2048×1024 → 4096×4096**: SPEC.md Phase 4 (문서 단위 캔버스)
