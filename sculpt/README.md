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
- [ ] Phase 2 — 방향 결정 게이트 (호시 판단)
- [ ] Phase 3 — C 엔진 기본 구조
- [ ] Phase 4 — 그리기 파이프
- [ ] Phase 5 — 편집 API
- [ ] Phase 6 — 10장 캐릭터 학습
- [ ] Phase 7 — 성공 기준 검증

## 시작하기

```bash
cd sculpt/prototype
pip install -r requirements.txt
python -m pytest tests/ -v
python scripts/run_demo.py
```

상세 사용법은 `prototype/README.md`.
