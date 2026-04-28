# Sculpt Engine

기존 `spatial_ai/` 와 완전히 분리된 새 엔진. "최대값 캔버스를 조각하듯 깎아
가며, 격자 공간의 상하좌우대각선 관계를 읽어 4계층 불확실성을 좁혀가는,
텍스트·이미지 공용 엔진."

## 네 원칙

1. **조각 연산 (Subtractive Only)** — 모든 상태 변경은 빼기. 더하기 없음.
2. **최대값 기준 저장** — `depth = 255 - original`. 깎인 양만 저장.
3. **이웃 관계 필수** — 모든 셀 결정은 8개 이웃을 참조한 뒤 내려짐.
4. **노이즈 여백** — 각 레벨 결정은 ±margin 의 불확실성을 남김.

상세 명세는 계획 파일 및 Phase 게이트 문서 참고. 이 엔진은 기존
`spatial_ai/` 와 심볼·파일·포맷을 공유하지 않는다.

## Phase 진행 상황

- [x] Phase 1 — Python 프로토타입 (`prototype/`)
- [x] Phase 2 — 방향 결정 게이트: **Go**
- [x] Phase 3 — C 엔진 기본 구조 (`include/`, `src/`, `tests/`, `tools/`, `Makefile`)
- [x] Phase 4 — 그리기 파이프 (`draw.c`, `draw_sculpt` CLI, 결정론 재현 검증)
- [x] Phase 5 — 편집 API (`edit_rect`, `replay`, log I/O, `edit_sculpt` CLI)
- [x] Phase 6 — 10장 학습 + `.slib` 직렬화 + 학습 결정론
- [x] Phase 7 — 성공 기준 검증 (결정론, 재현 지표, spatial_ai 독립성)
- [x] Phase 8 — 4-level 계층 파라미터 튜닝 (`tune_sculpt` CLI + `tune_sweep.py`)

## 시작하기

### Phase 1 — Python 프로토타입 (16x16 검증용)

```bash
cd sculpt/prototype
pip install -r requirements.txt
python -m pytest tests/ -v
python scripts/run_demo.py
```

상세 사용법은 `prototype/README.md`.

### Phase 3 — C 엔진 (학습 파이프)

```bash
cd sculpt
make test                                     # build + run 5 unit tests
python data/convert_png_to_sraw.py --all-characters
./build/train_sculpt data/char_*.sraw         # train on all 10 characters
```

명세 §10 에 따라 C 외부 의존성은 없음. 이미지 I/O 는 Python 헬퍼가
PNG → `.sraw` (SRAW magic + LE uint32 width/height/channels + RGB bytes)
로 한 번 변환, C 는 그 포맷을 읽는다.

### Phase 4 — 그리기 파이프 + 결정론 재현

```bash
cd sculpt
./build/draw_sculpt 42 /tmp/out.sraw data/char_*.sraw
./build/draw_sculpt 42 /tmp/out_again.sraw data/char_*.sraw
cmp /tmp/out.sraw /tmp/out_again.sraw  # 바이트 동일 (P4 결정론)
```

같은 `(library, master_seed)` 쌍은 항상 비트 동일 그리드를 생성 (DESIGN.md P4).
`test_determinism` 이 이 불변식을 유닛 테스트로 고정한다.

### Phase 5 — 편집 API + 로그 재생

```bash
# 전체 draw + edit log 캡처
./build/edit_sculpt draw 42 /tmp/out.sraw /tmp/out.slog data/char_*.sraw

# log 만 가지고 빈 canvas 에서 복원
./build/edit_sculpt replay 42 /tmp/out.slog /tmp/out_replay.sraw data/char_*.sraw
cmp /tmp/out.sraw /tmp/out_replay.sraw   # 바이트 동일

# 특정 rect 만 편집 (나머지 셀은 불변)
./build/edit_sculpt rect 42 4 4 4 4 /tmp/out_rect.sraw data/char_*.sraw
```

편집 로그 포맷: `SLOG` magic + `uint32 count` + `count * 12 bytes` per entry
(`level`, `iter_idx`, `cell_id`, `chisel_id`, `noise_xor`). `test_edit` 가
`draw → log capture → replay → bitwise equal`, `rect 외부 불변`, log 파일
라운드트립 무손실을 유닛 테스트로 고정.

### Phase 6 — 10장 학습 + library 직렬화

```bash
# 10장으로 학습 후 .slib 로 저장
./build/train_sculpt -o /tmp/chars.slib data/char_*.sraw

# draw/edit 은 .slib 또는 .sraw 목록 어느 쪽이든 받음 (확장자로 자동 분기)
./build/draw_sculpt 42 /tmp/out.sraw /tmp/chars.slib
./build/edit_sculpt draw 42 /tmp/out.sraw /tmp/out.slog /tmp/chars.slib
```

Library 바이너리 포맷 (`.slib`): `SLIB` magic + `version=1` + `count` + `next_id`
header (16 B) + 엔트리당 32 바이트 고정 (chisel_id, self 4nibble,
neighbors 8×3bit, subtract RGBA, level, weight, usage). 10장 학습 결과는
72,912 bytes (= 16 + 2278 × 32). `test_library_io` 가 라운드트립 + draw
동등성 + 학습 결정론(같은 입력 → 바이트 동일 .slib)을 고정.

### Phase 7 — 성공 기준 검증

```bash
make                                # build binaries
python sculpt/scripts/phase7_validate.py
```

스크립트가 네 가지 성공 기준을 모두 측정한다:

1. **결정론**: 모든 train 과 draw 를 두 번 실행해 SHA256 비교. 10 개 개별
   학습 + 1 개 혼합 학습 + 대응 draw — 합계 22 회의 연산이 전부 바이트
   동일 (`Phase 7: all determinism checks passed`).
2. **개별 학습 재현 지표**: 캐릭터 1 장으로만 학습한 라이브러리로 draw 한
   결과를 16×16 으로 다운샘플한 원본과 비교. 10 장 평균 MSE ≈ 41,500,
   PSNR ≈ 1.95 dB.
3. **혼합 학습 baseline**: 10 장 전체로 학습한 라이브러리 draw 1 회. 평균
   MSE ≈ 42,400, PSNR ≈ 1.86 dB. 개별 학습이 혼합 대비 ~2% 더 정합.
4. **spatial_ai 독립성**: sculpt 의 32 개 C/H 파일 전수 스캔, `spatial_ai`
   문자열 등장 0 회. 공유 헤더·빌드·심볼 없음 (명세 §10).

시각 확인용 `sculpt/out/phase7/contact_sheet.png` 도 생성된다 (행: 원본 /
개별 학습 draw / 혼합 학습 draw).

**알려진 한계 (Phase 8+ 튜닝 영역)**: `saturate_subtract` 가 iteration 마다
누적되어 채널이 빠르게 포화된다. 혼합 draw 의 채널 평균이
`(102, 0, 0)` 에 고정되는 현상은 이 구조적 saturation 때문이며, 원본
배경 회색 `(226, 223, 221)` 과의 거리가 수치(PSNR) 를 낮추는 주된 요인.
P1 (subtractive-only) 을 유지하면서 품질을 끌어올리려면 learn 단계의
per-level contribution 을 보수적으로 재조정하거나 draw iteration 예산을
적응적으로 축소해야 한다. 이 한계는 Phase 7 성공 기준 — **결정론 + 재현
가능성 + 독립성** — 에는 영향 없음.

### Phase 8 — 4-level 계층 파라미터 튜닝

Phase 1~7 은 모든 tuning 상수가 `static const` 로 컴파일 시점에 박혀
있었다. Phase 8 은 같은 값을 **런타임 변경 가능한 단일 구조체
`SCULPT_TUNING`** 으로 옮기고, 기존 매크로(`SCULPT_LEVEL_MARGIN[lv]`
등)는 새 구조체 필드를 가리키도록 재정의해 호출처 변경 없이 성립한다.
또한 단일 스칼라였던 `COHERENCE_THRESHOLD / BONUS / PENALTY` 를
**level 별 4-tuple** 로 승격시켜 4-level 계층 엔진을 사용하지 않던 유일한
draw 분기까지 계층화한다.

```bash
cd sculpt
make tune                                          # builds build/tune_sculpt
./build/tune_sculpt --seed 42 --out /tmp/out.sraw \
    --margin 0,8,0,32 --iters 1,1,1,1 \
    --coh-thresh 24,96,96,16 --coh-bonus 0,2,4,4 \
    /tmp/chars.slib

python scripts/tune_sweep.py --rounds 2            # coordinate-descent sweep
```

`tune_sculpt` 의 모든 플래그는 default 와 일치할 때 `draw_sculpt` 와
바이트 동일 출력을 낸다 (회귀 안전). 7 단위 테스트도 모두 default 로
재실행되어 통과 — 즉 Phase 8 변경은 기존 결정론을 깨지 않는다.

`tune_sweep.py` 는 좌표 하강법으로 한 번에 한 (parameter, level) 만
변경하면서 평균 MSE 를 낮추는 후보를 찾는다. 합본 라이브러리 (10 캐릭터)
에 대해 2 라운드 실행 시 측정된 결과:

| 항목 | Baseline (Phase 7) | Sweep best |
|---|---|---|
| 평균 MSE | 42,352 | **32,585 (Δ −9,767, −23.1%)** |
| PSNR    | 1.86 dB | **3.00 dB (+1.14 dB)** |

채택된 best tuning:

```
--margin 0,8,0,32      # L0/L2 노이즈 제거, L3 마진 2배
--iters 1,1,1,1        # 모든 level single-pass — saturation 누적 차단
--top-g 8,4,2,8        # L3 후보 풀 확대
--coh-thresh 24,96,96,16  # L1/L2 임계값 대폭 상향, L3 하향
--coh-bonus 0,2,4,4
--coh-penalty 1,1,1,1
--blur-box 1,2,4,8 / --a-band 32  (default 유지)
```

가장 큰 단일 향상은 `iters L3 2→1` (Δ −3,523) 와 `margin L3 16→32` (Δ
−1,375). **iter 가 모든 level 에서 1 로 수렴**한 것은 sculpt 가 본질적으로
single-pass 알고리즘이어야 함을 시사 — 다중 iteration 은 학습된 chisel
weight 보다 saturation 누적 효과가 더 커서 손해. 다음 phase 후보로
"iter 1 로 고정 + learn 단계 contribution 재분배" 가 자연스럽다.

전체 sweep 로그·확정된 config 는 `sculpt/out/phase8/best_tuning.json`
(git-ignored) 으로 저장된다.
