# SPIS / CE CORE SPEC (FINAL v1.0)

Delta-State Engine + Slot Layout + Execution Pipeline

---

## 1. 시스템 정의

SPIS/CE는 다음과 같이 정의된다:

CE는 작은 정수 기반의 유한 상태를 저장하고,
precomputed lookup을 통해 큰 값과 구조를 즉시 재구성하는
SIMD 친화적 상태 재개 엔진이다.

---

## 2. 핵심 개념

### 2.1 상태 vs 값

- 값(Value): 최종 결과 (픽셀, 수치 등)
- 상태(State): 값을 생성할 수 있는 압축 표현

CE는 값을 저장하지 않는다.

CE는 "값을 다시 만들 수 있는 상태"를 저장한다.

### 2.2 Delta의 본질

Delta는:

- ❌ 차이값
- ❌ float 변화량

이 아니다.

Delta는:

유한 상태 공간 내에서 CE 실행을 재개하기 위한 정수 상태 코드 (resume code)

이다.

---

## 3. Delta State 구조

### 3.1 정의

```
DeltaState = (
  tier_idx
  scale_idx
  precision_idx
  sign_idx
  tick_idx
  mode_idx
  channel_layout_idx
  slot_shape_idx
  [optional axes...]
)
```

### 3.2 조건

모든 축은:

- bounded
- 정수
- 최대값 존재

### 3.3 StateKey

```c
typedef uint64_t StateKey;
```

- bit packing 사용
- 필요 시 multi-key 확장 가능

---

## 4. Lookup 모델

### 4.1 기본 구조

```
StateKey → DeltaTable → OutputState
```

### 4.2 특징

- O(1)
- branch-free 가능
- runtime 산술 없음

### 4.3 OutputState

```
OutputState = {
  core_value
  link_value
  delta_value
  priority_value
  slot_activation_pattern
  next_state_hint
}
```

---

## 5. Channel 구조

RGBA는 단순 4값이 아니다.

각 채널은 독립적인 계층형 상태 공간이다

### 기본 의미 (권장)

| 채널 | 역할     |
|------|----------|
| R    | Core     |
| G    | Link     |
| B    | Delta    |
| A    | Priority |

### 확장

- 채널 재정의 가능
- 내부 구조 자유
- multi-channel 가능

---

## 6. Set16 (SIMD 단위)

### 정의

4 × 4 = 16 cells = 1 Set

### Quad 구조

```
[Q0][Q1]
[Q2][Q3]
```

### 역할 (권장)

| Quad | 의미              |
|------|-------------------|
| Q0   | +                 |
| Q1   | -                 |
| Q2   | * / scale         |
| Q3   | slow / precision  |

### 목적

- SIMD 최적화
- 병렬 처리
- 의미 분리

---

## 7. Slot 구조

### 정의

Slot은 값이 아니라 계층형 확산 구조이다

### 계층

- Core (중심)
- Inner (빠른 확산)
- Outer (느린 확산 / 안정화)

### 형태

- ring 구조
- 1→5→11 확산
- grid 기반
- 자유 확장 가능

---

## 8. Slot Shape Signature

### 정의

```
SlotShapeSignature = {
  core_state
  inner_pattern
  outer_pattern
  symmetry
  direction_bias
}
```

### 특징

- 값이 아닌 패턴
- 비교 가능
- lookup key로 사용 가능

---

## 9. Delta 적용 방식

### 기본

```
S(t+1) = S(t) + Δ_state
```

### 실제 처리

```
Δ_state → lookup → slot activation → state update
```

### 특징

- 직접 값 변경 없음
- 상태 재구성
- 패턴 기반 적용

---

## 10. Tier 시스템

| Tier | 역할   |
|------|--------|
| T1   | 미세   |
| T2   | 중간   |
| T3   | 구조   |

### 확장

- tier 수 증가 가능
- adaptive tier 가능

---

## 11. 확산 규칙

### 기본 흐름

core → inner → outer

### 조건

- semantic match
- direction match
- depth consistency
- slot compatibility

---

## 12. Resolve

### 역할

- slot shape 붕괴 감지
- outlier 제거
- delta 승격 판단

### 특징

- 후처리 아님
- 안정화 계층

---

## 13. Memory & Table Layout (SPEC-03.1)

### 13.1 DeltaTable

```
DeltaTable[StateKey] → OutputState
```

### 13.2 권장 구조 (SoA)

```
core_table[]
link_table[]
delta_table[]
priority_table[]
pattern_table[]
```

### 13.3 Pattern Table

```
pattern_table[StateKey] → activation mask
```

### 13.4 Tier Table

```
tier_table[tier_idx] → {
  scale_factor
  range
}
```

### 13.5 SIMD 처리

```
Set16 → vector
mask → apply
```

### 13.6 캐시 구조

- L1: hot delta
- L2: tier bucket
- L3: full table

---

## 14. 실행 파이프라인 (SPEC-04)

### 전체 흐름

```
Input
→ SmallCanvas
→ Seed Selection
→ CE Expansion (Delta)
→ Resolve
→ Output
```

### 14.1 Seed

- 중심 / 경계 / 영역
- 전체의 1~5%

### 14.2 Expansion

```
current_state
→ Delta lookup
→ apply
→ frontier 확장
```

### 14.3 Delta 적용

```
cell ← apply_delta(cell, delta_code)
```

### 14.4 Resolve

- 튐 제거
- 안정화
- 승격 판단

---

## 15. 시스템 구조 요약

```
작은캔버스 (구조)
  ↓
Seed (시작점)
  ↓
Delta (상태 코드)
  ↓
Lookup (O1)
  ↓
Slot (패턴 적용)
  ↓
확산
  ↓
Resolve
  ↓
출력
```

---

## 16. 핵심 성능 포인트

- O(1) lookup
- bounded state
- SIMD 처리
- no runtime float
- delta 기반 재사용

---

## 17. 확장 가능성

이 구조는 고정되지 않는다.

확장 가능한 요소:

- Delta 축 추가
- Slot shape 다양화
- Set 크기 변경
- Channel 재정의
- Tier 확장
- Hybrid 모드 추가

---

## 18. 금지 사항

- ❌ float 중심 설계
- ❌ unbounded state
- ❌ branch-heavy 구조
- ❌ delta를 단순 값으로 사용
- ❌ slot shape 무시

---

## 🔥 최종 정의

CE는 계산 엔진이 아니라,
작은 정수 상태(Delta)를 저장하고
lookup을 통해 큰 상태를 즉시 재구성하는
SIMD 기반 상태 재개 엔진이다.
