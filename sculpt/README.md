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
- [ ] Phase 6 — 10장 캐릭터 학습 (학습 파이프는 이미 Phase 3 에서 동작)
- [ ] Phase 7 — 성공 기준 검증

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
