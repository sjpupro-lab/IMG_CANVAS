# IMG-CANVAS (한국어)

**이중 모달 공간-패턴 엔진 — 텍스트·이미지가 같은 Keyframe 공간을 공유하고, "노이즈를 지우는" 방식이 아니라 배운 델타를 **찍어서** 그리는 프린터 모델.**

![hero](assets/main_hero.png)

텍스트 1절 = 256 × 256 밝기 격자. 이미지 1장 = 64 × 64 해석된 CE 셀. 둘 다 같은 `Keyframe`에 바인딩. 생성은 프린터 방식 — 엔진이 셀 현재 상태를 관찰 → top-G + presence penalty로 학습된 delta 선택 → 스탬프. **상세도는 모드 스위치가 아니라 셀 속성 (`tier / priority / role`)**, 브러시가 그걸 직접 읽어서 "어디에 얼마나" 찍을지 결정.

전체 영문 버전은 [README.md](README.md) 참조.

---

## 한눈에

| 모달 | 입력 | 저장 | 추론 / 생성 |
|---|---|---|---|
| **텍스트** | UTF-8 한 줄 = 한 절 | 256 × 256 RGBA 격자, keyframe + delta 체인, EMA 사전, topic-hash 버킷 | `ai_predict`, `ai_generate_next`, `ai_recluster` |
| **이미지** | PNG / JPEG / BMP / TGA / PPM, 어떤 크기든 OK | 64 × 64 해석 CE 격자; 9 216-entry 사전-구운 SoA delta 테이블; 멀티스케일 tier-다양 메모리 | `img_pipeline_run` (압축), `img_drawing_pass` (찍기), `img_delta_memory_learn_multiscale` |
| **양모달** | 텍스트 label + 이미지 | `Keyframe.ce_snapshot` 포인터, trailing record `SPAI_TAG_CE_SNAPSHOT = 0x08` | 텍스트 매칭이 짝지어진 CE 스냅샷 반환 (역도 동일) |

각 엔진이 독립적으로 0..1 점수 산출 → 호출자가 `joint = α·text + β·ce`로 합산. 내부에 섞인 state-key 없음.

> **자매 엔진 `sculpt/`** — spatial_ai 와 파일·심볼·빌드를 전혀 공유하지 않는
> 독립 16×16 subtractive chisel 조각 엔진. 아래 [sculpt 섹션](#sculpt-엔진--subtractive-chisel-조각)
> 참조.

---

## 아키텍처

```
           ┌──────────────── 텍스트 절 ────────────────┐
           │                                           │
           ▼                                           ▼
       layers_encode_clause                      detect_data_type
       (3-레이어 비트맵 + EMA 사전)              PROSE/DIALOG/CODE/SHORT
           │                                           │
           ▼                                           ▼
       256 × 256 RGBA 격자 ◄─── topic_hash + seq_in_topic
           │
           ▼
       Keyframe { id, grid, topic_hash, seq_in_topic,
                  data_kind, ce_snapshot }
           │                                           ▲
           │                                           │
           │   ┌─ 이미지 (RGB, 어떤 크기) ─────────────┘
           │   ▼
           │  img_image_to_small_canvas  →  SmallCanvas 256 × 256
           │        (R=강도 · G=흐름 · B=분위기 · A=깊이)
           │   ▼
           │  img_small_canvas_to_ce     →  CE 격자 64 × 64
           │        (R=core · G=link · B=delta · A=priority
           │         + tone / role / direction / depth / delta_sign
           │         + tier / last_delta_id)
           │
           ▼
      ┌─── 압축 경로 ──────────────────────────────────┐
      │  img_pipeline_run: seed → BFS → resolve        │
      │  → 자동 피드백 → .spai / .imem                  │
      └────────────────────────────────────────────────┘

      ┌─── 생성 경로 ──────────────────────────────────┐
      │  img_drawing_pass(grid, memory, opts):         │
      │    각 셀마다:                                   │
      │      topg + presence penalty + 브러시 bias →    │
      │      픽 → apply → recent_counts 갱신            │
      │  브러시: region_mask · target_tier · target_role│
      └────────────────────────────────────────────────┘
```

두 엔진은 `Keyframe`에서 만남. `img_pipeline_run`과 `img_drawing_pass`는 거울상 — 압축/해제/찍기 모두 같은 CE 원시자를 공유.

---

## 빠른 실행

```bash
cd spatial_ai

make              # 엔진 오브젝트 빌드
make test         # 22 suites (텍스트 12 + 이미지/양모달 10)
make demo         # 이미지 파이프라인 시각화
make train        # 양모달 배치 학습 (매니페스트 → .spai + .imem)
make draw         # 프레임 단위 이미지 생성 CLI
make chat         # 대화형 텍스트/이미지 REPL
make stream       # 텍스트 스트리밍 학습기
make gen-tables   # 구운 CE delta 테이블 재생성 (드묾)
```

### Windows (PowerShell, MSYS2 + MinGW-w64)

```powershell
# repo 루트에서, PATH에 mingw32-make + gcc가 잡힌 상태로.
spatial_ai\scripts\windows\build.ps1               # 엔진 + 모든 도구 빌드
spatial_ai\scripts\windows\test.ps1                # 전체 suite 실행

spatial_ai\scripts\windows\train.ps1 `
  -Manifest spatial_ai\data\characters_manifest.tsv `
  -Name characters                      # → out\models\characters.{spai,imem}

spatial_ai\scripts\windows\draw.ps1 `
  -Memory out\models\characters.imem `
  -Model  out\models\characters.spai `
  -SeedKf 0 -Frames 8 -Name kf0         # → out\draw\kf0\final.png (+frames\)

spatial_ai\scripts\windows\demo.ps1 `
  -Image assets\main_hero.png `
  -Name hero                            # → out\demo\hero_{plain,masked}.png
```

모든 출력물은 `out\` 아래로 떨어짐 (gitignore됨). 구조는 `out\README.md` 참조.

### 번들 캐릭터로 양모달 학습

```bash
./build/train --model out.spai --memory out.imem data/characters_manifest.tsv
```

`assets/characters/`의 10장 스틱피겨 링 체인. 기준 결과:

```
deltas added:      7 633
keyframes:         10
CE snapshots:      10
rarity buckets:    baseline 7 607 · ×2–4 13 · ≥4× 13
```

### 임의 이미지의 CE 압축 시각화

```bash
./build/demo_pipeline --adapt assets/main_hero.png out/hero
```

`out/hero_plain.{png,ppm}`, `out/hero_masked.{png,ppm}` 출력. masked 판은 resolve-흡수 셀은 cyan, 미해결(promoted) 셀은 red로 틴트.

### 텍스트 코퍼스 스트리밍 학습

```bash
./build/stream_train --input data/wiki5k.txt --max 5000 \
                     --save build/models/wiki5k.spai \
                     --checkpoint 5000 --verify
```

`--target-delta R`로 임계값 auto-calibrate, 학습 후 재클러스터링 지원.

### 대화형 REPL

```bash
./build/chat --load build/models/wiki5k.spai --session build/chat.session
> /ret how are you          # 검색
> /topk 5 eiffel tower      # top-K
> /gen tell me about it     # 생성
> /img sunset over paris    # $IMG_CANVAS_BIN 환경변수로 외부 라우팅
> :history / :reset / :ctx 5 / :save <path> / :load <path>
```

링버퍼 턴 컨텍스트(최대 8), 쿼리 라우터, 세션 디스크 왕복.

---

## Sculpt 엔진 — subtractive chisel 조각

**서브시스템이 아니라 자매 엔진.** `sculpt/` 는 명세 §10 에 따라
`spatial_ai/` 와 파일·심볼·헤더·빌드를 전혀 공유하지 않는다. 255·255·255·255
의 max-value 16×16 캔버스를 **깎아서** 이미지를 만든다. 모든 변형은
`depth ← min(255, depth + Δ)` — 한번 깎은 깊이는 줄어들지 않는다 (P1,
subtractive-only). 결정론적이고 재생 가능한 소형 그리드 생성에 쓴다.

7 단계 전부 완료:

| Phase | 산출물 |
|---|---|
| 1 — Python 프로토타입          | `sculpt/prototype/` (pytest) |
| 2 — Go/NoGo 게이트             | Go |
| 3 — C 엔진 + 학습 파이프       | `sculpt/{include,src}/`, `sculpt/Makefile`, `train_sculpt` |
| 4 — 그리기 + 결정론            | `sculpt/src/draw.c`, `draw_sculpt`, `test_determinism` |
| 5 — 편집 API (rect + replay)   | `sculpt_edit_rect`, `sculpt_replay`, `.slog` 로그, `edit_sculpt` |
| 6 — `.slib` library 직렬화     | `sculpt_libraryio`, `-o` 플래그, CLI 확장자 자동 분기 |
| 7 — 성공 기준 검증             | `sculpt/scripts/phase7_validate.py` (결정론 22 회 + MSE/PSNR + spatial_ai 독립성 스캔) |

포맷:

| 확장자 | 내용 |
|---|---|
| `.sraw` | `"SRAW" + LE u32 width/height/channels` + RGB 바이트 (C 외부 의존성 없는 이미지 I/O) |
| `.slib` | `"SLIB" + version + count + next_id` + chisel 당 32 바이트 (리틀엔디언 명시 패킹) |
| `.slog` | `"SLOG" + u32 count` + 엔트리당 12 바이트 (level, iter, cell, chisel id, noise xor) |

빠른 실행:

```bash
cd sculpt
make && make test                                    # C 유닛 테스트 7 개
python data/convert_png_to_sraw.py --all-characters   # PNG → .sraw

./build/train_sculpt -o /tmp/chars.slib data/char_*.sraw
./build/draw_sculpt  42 /tmp/out.sraw   /tmp/chars.slib
./build/edit_sculpt  draw 42 /tmp/out.sraw /tmp/out.slog /tmp/chars.slib
./build/edit_sculpt  replay 42 /tmp/out.slog /tmp/replay.sraw /tmp/chars.slib
cmp /tmp/out.sraw /tmp/replay.sraw                    # 바이트 동일

python scripts/phase7_validate.py                     # 엔드투엔드 검증
```

번들 10 캐릭터에서 Phase 7 결과: 22 회 결정론 검증 모두 바이트 동일,
C/H 32 개 파일 스캔 `spatial_ai` 참조 0 건, 혼합 학습 vs 개별 학습 정량화
(개별이 혼합보다 MSE ~2% 우위). 현재 draw 품질 자체(PSNR ≈ 1.9 dB)는 튜닝
영역 — `saturate_subtract` 누적으로 G/B 채널이 포화되는 구조적 한계이며,
P1 을 지키면서 풀려면 learn 단계 contribution 재조정이 필요.

자세한 phase 로그·튜닝 상수·한계는 [`sculpt/README.md`](sculpt/README.md) 에.

---

## 드로잉 모드 — "프린터"

노이즈에서 지워가며 복원하는 방식이 아니야. 엔진이 현재 CE 상태를 관찰 → 배운 델타 픽 → 스탬프. 다양성은 temperature 대신 presence penalty (LM의 coverage / presence penalty와 같은 개념).

```c
ImgDrawingOptions opt = img_drawing_default_options();
opt.top_g            = 4;     /* 셀당 후보 풀 */
opt.presence_penalty = 0.5;   /* α; 최근 픽 수에 비례해 감점 */
opt.passes           = 3;     /* 밑그림 → 디테일 레이어링 */

img_drawing_pass(grid, memory, &opt, &stats);
```

### 브러시 — 영역·tier·role 제어

같은 메모리 + 다른 브러시 = 다른 출력:

```c
uint8_t face_mask[IMG_CE_TOTAL];
img_brush_mask_rect(face_mask, 22, 10, 42, 28);

opt.region_mask  = face_mask;         /* 이 셀들만 */
opt.target_tier  = IMG_TIER_T3;       /* 구조 레벨 디테일 */
opt.target_role  = IMG_ROLE_FACE;
opt.tier_bonus   = 0.25;              /* 페이로드 tier 일치 시 점수 가산 */
opt.role_bonus   = 0.20;              /* role도 동일 */

img_drawing_pass(grid, memory, &opt, &face_stats);

// 이후 더 넓고 느슨한 브러시로 옷:
img_brush_mask_rect(clothes_mask, 15, 28, 50, 58);
opt.region_mask = clothes_mask;
opt.target_tier = IMG_TIER_T2;
opt.target_role = IMG_ROLE_OBJECT;
img_drawing_pass(grid, memory, &opt, &clothes_stats);
```

패스별 stats: `cells_masked_out`, `brush_bonus_wins`, `unique_deltas_used`, `max_recent_count`.

---

## 프레임 단위 드로잉 — CLI

`tools/draw.c`는 엔진을 작은 "비디오" 생성기로 돌림: **프레임당 drawing pass 1회**, 각 패스 후에 격자를 렌더·저장. 마지막 프레임이 결과물. 이전 프레임들은 점진적 스탬핑 과정 — 밑그림 → 디테일.

```bash
./build/draw \
    --memory out/models/characters.imem \
    --model  out/models/characters.spai \
    --seed-kf 0 \
    --frames 8 \
    --out out/draw/kf0
```

Seed (가장 구체적인 걸로 덮어쓰기):

- `--seed-image <path>` — 이미지에 `img_pipeline_run` 돌린 결과 CE 격자에서 시작.
- `--seed-kf <id>` — `--model`의 해당 keyframe의 `ce_snapshot`을 복사.
- seed 없음 — 빈 CE 격자 (모든 셀 L6 fallback, 결과는 추상).

튜너블: `--frames N`, `--top-g N`, `--penalty F` (presence penalty). 출력:

```
out/draw/kf0/
├── frames/
│   ├── frame_000.{png,ppm}     pass 1 직후 상태
│   ├── frame_001.{png,ppm}     pass 2 직후 상태
│   ├── ...
│   └── frame_NNN.{png,ppm}     pass N (= 마지막) 직후 상태
└── final.{png,ppm}             마지막 프레임과 동일 — 결과물
```

경험적으로 학습된 keyframe으로 seed하면 프레임당 `unique_deltas_used`가 빈 seed 대비 약 10× (각 셀이 실제 tone/role/depth 컨텍스트로 시작하니 `topg` fallback이 L6가 아닌 L0에서 뽑힘).

---

## 멀티스케일 학습 — 한 장에서 tier-다양 규칙

```c
uint32_t radii[] = { 32, 12, 4, 0 };    /* 가장 거친 → 가장 세밀 */
img_delta_memory_learn_multiscale(memory, image_rgb, w, h, radii, 4);
```

분리가능한 O(w·h) box-blur 캐스케이드가 인접한 (거친 → 세밀) 쌍 3개 생성. 각 쌍이 `learn_from_images`로 들어감; blur 간격이 tier를 결정 — 큰 간격 → T3(구조), 작은 간격 → T1(세밀). 1280 × 720 인물 한 장 돌린 결과:

```
deltas added:      2 560
tier histogram:    T1 814 · T2 1 425 · T3 321
mode histogram:    INTENSITY 1 849 · DIRECTION 394 · MOOD 201 · ROLE 116
rarity buckets:    baseline 2 490 · 2–4× 33 · ≥4× 37
```

Tier 스프레드는 캐스케이드 설계 그대로. 이 메모리로 빈 캔버스에 `drawing_pass` 돌리면 여전히 추상 무늬만 나옴 — 빈 격자는 모든 셀이 같은 fallback 컨텍스트라 공간 의도가 없음. 매칭된 키프레임의 `ce_snapshot`으로 seed하는 게 **Phase D**이자 다음 PR.

---

## 도구

| 도구 | 기능 | 소스 |
|---|---|---|
| `chat`          | 턴 컨텍스트 + 쿼리 라우터 (`/gen /ret /img /topk`) + 세션 영속화 REPL | `tools/chat.c` |
| `stream_train`  | 라인별 텍스트 인제스트 + 체크포인트 + 긴 라인 자동 분할 + auto-threshold 캘리브 | `tools/stream_train.c` |
| `train`         | TSV 매니페스트 → `.spai` + `.imem` 배치 학습; `--resume` 지원 | `tools/train.c` |
| `draw`          | 프레임 단위 이미지 생성: N 패스 → `frames/frame_NNN.{png,ppm}` + `final.{png,ppm}`. seed는 keyframe / 이미지 / 빈 캔버스 중 택 | `tools/draw.c` |
| `demo_pipeline` | 이미지 → CE → 렌더 원샷. `--adapt`로 이미지별 tier 임계값; PNG + PPM 출력 | `tools/demo_pipeline.c` |
| `gen_delta_tables` | CE delta 테이블 오프라인 생성기 | `tools/gen_delta_tables.c` |
| `bench_*`       | 텍스트 엔진 벤치마크 (perplexity, word-predict, QA, STS-B) | `tests/bench_*.c` |

---

## 학습 데이터 형식

- **텍스트** — UTF-8 한 줄 한 절. 길이 + 특수문자 비율로 자동 분류 (PROSE / DIALOG / CODE / SHORT), 데이터 타입별 임계값이 keyframe/delta 결정. `--max-line-bytes`(기본 256 = 격자 Y축) 넘는 라인은 **문장 경계에서 자동 분할**.
- **이미지** — `stb_image`가 읽는 모든 포맷 (PNG / JPEG / BMP / TGA) + P6 PPM. 차원 `[16, 16384]` 범위 강제. RGB만 (알파 drop). 256 × 256으로 블록 평균 다운샘플.
- **양모달 TSV 매니페스트** — 학습 예시 한 행:
  ```
  <text_label>\t<before_image>\t<after_image>
  hero to visualization 1	assets/main_hero.png	assets/visualization_1.png
  ```
  `#` 주석 / 빈 줄 무시.

---

## 파일 포맷

**SPAI (텍스트 + 양모달)** — `magic[4]="SPAI"` · `version` · `kf_count` · `df_count` · `reserved[3]` (`reserved[0]` = 저장 Unix timestamp), 이후 태그 레코드 스트림:

| Tag | 의미 |
|---|---|
| `0x01` KEYFRAME      | `id, label, text_byte_count, topic_hash, seq_in_topic, data_kind, grid.ARGB` |
| `0x02` DELTA         | parent 대비 sparse `(index, dA, dR, dG, dB)` |
| `0x03` WEIGHTS       | 채널별 adaptive weight (4 × float) |
| `0x04` CANVAS        | 2048 × 1024 canvas 전체 스냅샷 |
| `0x05` SUBTITLE      | subtitle track |
| `0x06` EMA           | RGB EMA priors (4 × GRID_TOTAL × float) |
| `0x07` CANVAS_DELTA  | P-frame canvas (parent 대비 sparse A/R/G/B diff) |
| `0x08` CE_SNAPSHOT   | **양모달: keyframe 바인딩된 이미지 CE 격자** |

구 버전 리더는 unknown tag에서 깔끔히 멈춤. `ai_peek_header_ex`로 모델 로드 없이 `kf_count / df_count / version / save_timestamp` 조회 가능.

**IMEM (이미지 delta memory)** — `magic[4]="IMEM"` · `version` · `count` · `reserved`, 이후 `count` × 40-byte 유닛 레코드. 필드 명시적 패킹(little-endian), 구조체 패딩 비의존. 유닛당 `id / pre_key / post_hint / payload.state / role_target / usage_count / success_count / weight`.

---

## 디렉토리

```
IMG-CANVAS/
├── assets/                       공유 이미지 자산 (PNG)
│   ├── main_hero.png  visualization_{1,2}.png
│   └── characters/               10장 번들 캐릭터
├── out/                          런타임 출력 (gitignore; out/README.md 참조)
├── sculpt/                       자매 엔진 — subtractive 16×16 chisel 조각
│   ├── README.md                 phase 로그 + 사용법 + 한계
│   ├── prototype/                Phase 1 Python 검증용 (pytest)
│   ├── include/  src/            Phase 3+ C 엔진, 외부 의존성 없음
│   ├── tests/                    C 유닛 스위트 7 개
│   ├── tools/                    train_sculpt · draw_sculpt · edit_sculpt
│   ├── scripts/phase7_validate.py  엔드투엔드 성공 기준 러너
│   ├── data/                     .sraw / .slib / .slog (gitignored)
│   └── Makefile
├── spatial_ai/
│   ├── SPEC.md                   텍스트 엔진 명세 v3
│   ├── SPEC-CE.md                이미지 CE 엔진 명세 v1
│   ├── SPEC-ENGINE.md            최적화 노트
│   ├── TODO_recluster.md         recluster / 캘리브 로드맵
│   ├── README.md                 텍스트 엔진 로컬 README
│   ├── docs/benchmarks/          wiki5k / wiki20k 엔진 리포트
│   ├── scripts/windows/          PowerShell 래퍼 (build/test/train/draw/demo)
│   ├── Makefile
│   ├── include/
│   │   ├── spatial_*.h           텍스트 엔진 (12 헤더)
│   │   ├── spatial_bimodal.h     텍스트 ↔ 이미지 바인딩
│   │   └── img_*.h               이미지 CE 엔진 + drawing (10 헤더)
│   ├── src/                      헤더당 .c + img_delta_tables_data.c (GENERATED)
│   ├── tests/                    22 unit suite (make test)
│   ├── tools/                    chat / stream_train / train / demo_pipeline / gen_delta_tables
│   ├── third_party/              stb_image + stb_image_write (public domain)
│   └── data/
│       ├── characters_manifest.tsv
│       ├── train_manifest.tsv
│       └── training/             사용자 로컬 이미지 (gitignore됨)
├── README.md                     영문 메인
└── README_KO.md                  이 파일
```

---

## 테스트

```bash
cd spatial_ai && make test
```

| 그룹 | Suite | 테스트 |
|---|---|---|
| **텍스트 엔진** | grid, morpheme, layers, match, keyframe, context, integration, io, cascade, canvas, adaptive, subtitle | 12 |
| **이미지 CE**   | img_ce, img_delta_memory (24), img_set16, img_render, img_pipeline, img_tier_table, img_delta_learn (8), img_ce_diff, img_drawing (9) | 9 |
| **양모달**      | test_bimodal | 1 |
| **합계**        | | **22 suites** |

매 커밋마다 전부 그린 유지.

---

## 벤치마크

- **텍스트** — `spatial_ai/docs/benchmarks/v2_text_engine/` — wiki5k / wiki20k 레퍼런스 실행 결과 (matching / self-recall / word-predict / byte-perplexity / generation). v2 엔진 (commit `4c4e108`).
- **이미지** — 번들 캐릭터 매니페스트 양모달 학습 → `+7 633 deltas / 10 kf / 10 ce snapshots / 13 + 13 rare-bucket`. 1280 × 720 인물 멀티스케일 학습 → `+2 560 deltas`, 스프레드 `T1 814 · T2 1 425 · T3 321`.

---

## 현재 메인의 한계

- **빈 캔버스 드로잉엔 아직 공간 의도가 없음.** 모든 셀이 같은 L6-fallback 컨텍스트를 봐서 `drawing_pass`가 추상 무늬를 생성. 해결책: 매칭된 키프레임의 `ce_snapshot`으로 seed (Phase D).
- **픽셀 실현은 여전히 SlotShape** (셀당 center / cross / corner / border 점 패턴, tier로 스케일). 셀을 일관된 선·면·획으로 풀어주는 tier-aware 래스터라이저가 다음 렌더 업그레이드.
- **구성 연산자(compositional operators)** — 포즈 / 팔레트 / 스타일을 독립 축으로 분리 학습은 미완. 축 자체는 bounded (tier / scale / sign / mode / tick / channel_layout / slot_shape)로 준비됐지만 학습이 아직 의도별 슬라이싱을 안 함.

---

## 관련 문서

- [`spatial_ai/SPEC.md`](spatial_ai/SPEC.md) — 텍스트 엔진 명세
- [`spatial_ai/SPEC-CE.md`](spatial_ai/SPEC-CE.md) — 이미지 CE 엔진 명세 v1
- [`spatial_ai/SPEC-ENGINE.md`](spatial_ai/SPEC-ENGINE.md) — 성능 / 레이아웃 노트
- [`spatial_ai/TODO_recluster.md`](spatial_ai/TODO_recluster.md) — recluster 로드맵
- [`sculpt/README.md`](sculpt/README.md) — subtractive chisel 엔진 phase 로그
- [`README.md`](README.md) — 영문 메인

---

## 라이선스

[LICENSE](LICENSE) 참조.
