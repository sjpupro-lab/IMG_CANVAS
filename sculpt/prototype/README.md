# Sculpt Engine — Phase 1 Python Prototype

네 원칙이 실제로 동작하는지 16×16 격자로 검증하기 위한 Python 프로토타입.

명세: `sculpt/DESIGN.md` (v1.0). 이 디렉토리는 명세 §8 Phase 1 범위만 구현한다.

## 디렉토리

```
prototype/
├── requirements.txt           numpy, Pillow, pytest
├── sculpt/                    Python 패키지
│   ├── cell.py                Cell + saturate_subtract (P1 유일한 carving API)
│   ├── grid.py                16x16 격자 + neighbor_8 (P3)
│   ├── chisel.py              Chisel + NeighborStateKey (P3 시그니처 강제)
│   ├── library.py             ChiselLibrary (level/key 기반 lookup)
│   ├── prng.py                SplitMix64 (P4 결정론)
│   ├── learn.py               이미지 -> chisel 집합
│   ├── draw.py                최대값 캔버스 -> 완성 (이웃 일관성 점수)
│   ├── io.py                  Pillow 기반 PNG I/O
│   └── tuning.py              열린 질문 §12 초기값 모음
├── scripts/
│   ├── run_learn.py           CLI: 이미지 한 장 학습
│   ├── run_draw.py            CLI: 학습 후 그리기
│   └── run_demo.py            학습 + 그리기 + 결정론 검증 한 번에
└── tests/
    ├── test_max_baseline.py    P2 왕복 검증
    ├── test_subtractive_only.py P1 소스 스캔
    ├── test_neighbor_mandatory.py P3 시그니처/예외
    └── test_determinism.py     P4 같은 시드 -> 같은 결과
```

## 실행

```bash
cd sculpt/prototype
pip install -r requirements.txt

python -m pytest tests/ -v       # 13 tests, all pass
python scripts/run_demo.py       # out/ 에 3개 PNG + report.json 생성
```

## 데모 결과 (master_seed=42, char_01_ruby.png)

```
[1] chisels registered: 280  by_level={3: 11, 2: 47, 1: 106, 0: 116}
[2] decisions_by_level={3: 512, 2: 512, 1: 512, 0: 256}
    avg_score_by_level={3: 55.41, 2: 37.73, 1: 25.80, 0: 19.80}
[3] identical: True    (같은 시드 재실행 -> PNG 바이트 동일)
[4] differs from seed=42: True    (다른 시드 -> 결과 다름)
```

`out/phase1_demo.png` 와 `out/phase1_demo_run2.png` 가 **바이트 단위로 동일** 함을
`cmp` 로 확인. `out/phase1_demo_seed1337.png` 는 다름.

## 관찰 결과

### 네 원칙 모두 동작

- **P1 빼기 전용** — `test_subtractive_only.py::test_no_direct_addition_against_depth`
  가 `.depth_[rgba] +=` 패턴을 패키지 전체에서 스캔, 통과. 모든 depth 변경은
  `cell.saturate_subtract` 단일 함수를 거친다.
- **P2 최대값 기준** — 저장과 복원이 항상 `depth = 255 - original` 대칭.
  `Grid.to_rgb_array()` 가 `255 - depth` 로 복원.
- **P3 이웃 필수** — `build_neighbor_key(cell, neighbors_8)` 시그니처 자체가
  이웃 8개를 요구. `assert len(neighbors_8) == 8` 로 방어. 빈 리스트를 넘기면
  `AssertionError` (테스트로 확인).
- **P4 노이즈 여백 결정론** — `SplitMix64(derive_seed(master, level, iter, cell_id))`
  으로 노이즈 추출. 같은 master_seed → 모든 노이즈 동일 → PNG 바이트 동일.

### 레벨 기여도 (명세 §7.3 성공 기준)

평균 점수 기여도가 레벨별로 뚜렷한 스펙트럼:
- L3 (분위기): 55.41 — 빈 캔버스에서 가장 매끄럽게 어울림
- L2 (면)  : 37.73
- L1 (선)  : 25.80
- L0 (점)  : 19.80 — 가장 작은 여백, 이웃과 더 미세하게 튀는 경향

네 레벨 모두 **각각 10% 이상의 결정에 기여** 했다 (분산된 점수). 한 레벨이
전부 먹는 현상은 없음.

### 라이브러리 크기

16×16 × 4레벨 = 이론상 1024 엔트리, 실제 등록은 280개 (중복 병합 성공).
L3 은 11개로 가장 작음 — 저주파수 블러라 셀 간 유사성이 큼. L0 은 116개로
가장 다양.

### Phase 1 판단 방향

네 원칙 모두 동작하며, 학습·그리기 경로가 결정론적으로 재현 가능. 레벨별
기여도도 균형적. **이 방향이 맞다**고 판단되면 Phase 2 (게이트) 에서 Go
결정을 내려 Phase 3 (C 엔진 이식) 로 진행.

### 열린 질문 §12 에 대한 Phase 1 답변

|#|질문|Phase 1 초기값 (`tuning.py`)|관찰|
|-|-|-|-|
|1|이웃 일관성 점수|bonus=+2, penalty=-1, threshold=24 (4채널 합계 기준)|레벨별 avg score 분산 정상|
|2|A 밴드 너비|A_BAND_WIDTH=32 (Phase 1 에선 미사용, draw 에 region 미구현)|Phase 3 에서 도입|
|3|NeighborStateKey 비트|자기 4×4 + 이웃 8×3 = 40비트, uint64 여유|매칭 버킷 분포 적절|
|4|top-G|L3=2, L2=4, L1=4, L0=8|모든 셀에서 최소 1개 후보 발견|
|5|노이즈 margin|{±16, ±8, ±4, ±1} 명세 기본값|결정론 유지, 시드 차이 결과 차이 확인|

## 비범위 (Phase 1 에서 안 한 것)

- 편집 API 로그 기반 부분 재생성 (Phase 5)
- 영역 맵 A 밴드 그룹핑 기반 의사결정 (Phase 3 이후)
- 여러 이미지 혼합 학습 (Phase 6)
- 기존 spatial_ai 와 시각적 비교 (Phase 7)
- C 엔진 포팅 (Phase 3)

이것들은 모두 Phase 2 게이트에서 Go 결정 후 진행.
