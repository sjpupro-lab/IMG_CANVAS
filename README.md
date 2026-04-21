# IMG-CANVAS

**Dual-modality spatial pattern engine — text and image share one keyframe space, and the engine draws by *stamping* learned deltas, not by denoising noise.**

![hero](assets/main_hero.png)

Text is stored as a 256 × 256 brightness grid per clause. Images are stored as a 64 × 64 CE grid of interpreted cells. Both bind to the same `Keyframe`. Generation is *printer-mode* — the engine observes the cell's current state, picks a learned delta by top-G sampling with presence penalty, and stamps. Detail per region is not a mode switch; it's a cell-level attribute (`tier`, `priority`, `role`) and a brush reads those directly.

---

## At a glance

| Modality | Input | Storage | Inference / generation |
|---|---|---|---|
| **Text** | line-per-clause UTF-8 | 256 × 256 RGBA grid, keyframe + delta chain, EMA priors, topic-hash bucket | `ai_predict`, `ai_generate_next`, `ai_recluster` |
| **Image** | PNG / JPEG / BMP / TGA / PPM, any dim | 64 × 64 CE grid of interpreted cells; 9 216-entry pre-baked SoA delta tables; multi-scale tier-diverse memory | `img_pipeline_run` (compress), `img_drawing_pass` (stamp), `img_delta_memory_learn_multiscale` |
| **Bimodal** | text label + image | `Keyframe.ce_snapshot` pointer per KF, trailing record `SPAI_TAG_CE_SNAPSHOT = 0x08` | text-match returns paired CE snapshot; CE match returns paired text grid |

Each side scores independently on a 0..1 scale; callers combine (`joint = α·text + β·ce`) — no mixed state-key space.

> **Sibling engine under `sculpt/`** — a separate subtractive 16×16 chisel
> carver (Python prototype + standalone C engine) that shares no files,
> symbols, or build with `spatial_ai/`. See the [sculpt section](#sculpt-engine--subtractive-chisel-carver) below.

---

## Architecture

```
           ┌────────────── text clause ──────────────┐
           │                                         │
           ▼                                         ▼
       layers_encode_clause                    detect_data_type
       (3-layer bitmap, EMA prior)             PROSE/DIALOG/CODE/SHORT
           │                                         │
           ▼                                         ▼
       256 × 256 RGBA grid ◄─── topic_hash + seq_in_topic
           │
           ▼
       Keyframe { id, grid, topic_hash, seq_in_topic,
                  data_kind, ce_snapshot }
           │                                         ▲
           │                                         │
           │    ┌── image (RGB, any dim) ────────────┘
           │    ▼
           │   img_image_to_small_canvas  →  SmallCanvas 256 × 256
           │          (R=intensity · G=flow · B=mood · A=depth)
           │    ▼
           │   img_small_canvas_to_ce     →  CE grid 64 × 64
           │          (R=core · G=link · B=delta · A=priority
           │           + tone / role / direction / depth / delta_sign
           │           + tier / last_delta_id)
           │
           ▼
      ┌─── COMPRESS PATH ───────────────────────────────┐
      │  img_pipeline_run: seed → BFS expand → resolve │
      │  → auto-feedback → .spai / .imem                │
      └────────────────────────────────────────────────┘

      ┌─── GENERATE PATH ───────────────────────────────┐
      │  img_drawing_pass(grid, memory, opts):          │
      │    for each cell:                                │
      │      topg + presence penalty + brush bias →     │
      │      pick → apply → bump recent_counts          │
      │  Brush: region_mask · target_tier · target_role │
      └────────────────────────────────────────────────┘
```

The two engines meet at `Keyframe`. `img_pipeline_run` and `img_drawing_pass` are mirror entry points — compress / decompress / stamp all through the same CE primitives.

---

## Quick start

```bash
cd spatial_ai

make              # build engine objects
make test         # 22 suites: 12 text + 10 image/bimodal
make demo         # image pipeline visualiser
make train        # bimodal training CLI (manifest → .spai + .imem)
make draw         # frame-by-frame image generation CLI
make chat         # interactive text/image REPL
make stream       # text streaming trainer
make gen-tables   # regenerate baked CE delta tables (rare)
```

### Windows (PowerShell, MSYS2 + MinGW-w64)

```powershell
# From the repo root, with mingw32-make + gcc on PATH.
scripts\windows\build.ps1               # builds engine + all tools
scripts\windows\test.ps1                # runs every suite

scripts\windows\train.ps1 `
  -Manifest spatial_ai\data\characters_manifest.tsv `
  -Name characters                      # → out\models\characters.{spai,imem}

scripts\windows\draw.ps1 `
  -Memory out\models\characters.imem `
  -Model  out\models\characters.spai `
  -SeedKf 0 -Frames 8 -Name kf0         # → out\draw\kf0\final.png (+frames\)

scripts\windows\demo.ps1 `
  -Image assets\main_hero.png `
  -Name hero                            # → out\demo\hero_{plain,masked}.png
```

All outputs land under `out\` (gitignored). See `out\README.md` for the layout.

### Run bimodal training on the bundled characters

```bash
./build/train --model out.spai --memory out.imem data/characters_manifest.tsv
```

Ten synthetic stick-figure silhouettes (`assets/characters/`) chained in a ring. Baseline result:

```
deltas added:      7 633
keyframes:         10
CE snapshots:      10
rarity buckets:    baseline 7 607 · ×2–4 13 · ≥4× 13
```

### Visualise CE compression of any image

```bash
./build/demo_pipeline --adapt assets/main_hero.png out/hero
```

Writes `out/hero_plain.{png,ppm}` and `out/hero_masked.{png,ppm}` — the masked variant tints cyan for resolve-absorbed cells and red for promoted (unresolved) ones.

### Stream-train on a text corpus

```bash
./build/stream_train --input data/wiki5k.txt --max 5000 \
                     --save build/models/wiki5k.spai \
                     --checkpoint 5000 --verify
```

Supports `--target-delta R` for threshold auto-calibration and post-training recluster.

### Interactive chat

```bash
./build/chat --load build/models/wiki5k.spai --session build/chat.session
> /ret how are you          # retrieve nearest clauses
> /topk 5 eiffel tower      # top-K retrieval
> /gen tell me about it     # text generation
> /img sunset over paris    # route to $IMG_CANVAS_BIN if set
> :history / :reset / :ctx 5 / :save <path> / :load <path>
```

Ring-buffer turn context (max 8), per-turn query router, session disk round-trip.

---

## Sculpt engine — subtractive chisel carver

A **sibling engine**, not a subsystem. `sculpt/` is completely independent of
`spatial_ai/` per its §10 rule (no shared files, symbols, headers, or build).
It draws by *carving* a max-value 16×16 canvas — every mutation is
`depth ← min(255, depth + Δ)` (no restorative writes). Use it when you want
deterministic, replay-able image generation on a tiny grid.

Seven phases, all landed:

| Phase | Artefact |
|---|---|
| 1 — Python prototype          | `sculpt/prototype/` (pytest-covered) |
| 2 — Go/NoGo gate              | Go |
| 3 — C engine + learn pipe     | `sculpt/{include,src}/`, `sculpt/Makefile`, `train_sculpt` |
| 4 — draw + determinism        | `sculpt/src/draw.c`, `draw_sculpt`, `test_determinism` |
| 5 — edit API (rect + replay)  | `sculpt_edit_rect`, `sculpt_replay`, `.slog` log format, `edit_sculpt` |
| 6 — `.slib` library serialization | `sculpt_libraryio`, `-o` flag, CLI auto-dispatch by extension |
| 7 — success-criteria validation | `sculpt/scripts/phase7_validate.py` (22 determinism checks + MSE/PSNR + spatial_ai independence scan) |

Formats:

| Extension | Contents |
|---|---|
| `.sraw` | header `"SRAW" + LE u32 width/height/channels` + RGB bytes (image I/O without external C deps) |
| `.slib` | header `"SLIB" + version + count + next_id` + 32-byte-per-chisel entries (LE-packed, no struct padding) |
| `.slog` | header `"SLOG" + u32 count` + 12-byte-per-entry edit log (level, iter, cell, chisel id, noise xor) |

Quick start:

```bash
cd sculpt
make && make test                                    # 7 C unit tests
python data/convert_png_to_sraw.py --all-characters   # PNG → .sraw

./build/train_sculpt -o /tmp/chars.slib data/char_*.sraw
./build/draw_sculpt  42 /tmp/out.sraw   /tmp/chars.slib
./build/edit_sculpt  draw 42 /tmp/out.sraw /tmp/out.slog /tmp/chars.slib
./build/edit_sculpt  replay 42 /tmp/out.slog /tmp/replay.sraw /tmp/chars.slib
cmp /tmp/out.sraw /tmp/replay.sraw                    # bytewise identical

python scripts/phase7_validate.py                     # end-to-end validation
```

Phase 7 numbers on the bundled 10-character set: all 22 determinism checks
bytewise-identical, 32 C/H files scanned with zero `spatial_ai` references,
mixed-library vs per-character training quantified (per-char beats mixed by
~2% MSE). Visual quality itself (current PSNR ≈ 1.9 dB) is a draw-tuning
frontier — iterative `saturate_subtract` drives G/B channels to full
saturation; future work adjusts learn-stage contribution rather than
relaxing P1 (subtractive-only).

See [`sculpt/README.md`](sculpt/README.md) for the detailed phase log,
tuning constants, and known limits.

---

## Drawing mode — the "printer"

The engine doesn't denoise from noise. It observes the current CE state, picks a learned delta, and stamps. Diversity comes from a presence penalty (same idea as LM coverage / presence penalty) instead of temperature.

```c
ImgDrawingOptions opt = img_drawing_default_options();
opt.top_g            = 4;     /* pool size per cell */
opt.presence_penalty = 0.5;   /* α; subtracted per recent pick */
opt.passes           = 3;     /* underdrawing → detail layering */

img_drawing_pass(grid, memory, &opt, &stats);
```

### Brush — region · tier · role control

Single memory, different brushes, different outputs:

```c
uint8_t face_mask[IMG_CE_TOTAL];
img_brush_mask_rect(face_mask, 22, 10, 42, 28);

opt.region_mask  = face_mask;         /* only these cells */
opt.target_tier  = IMG_TIER_T3;       /* structure-level detail */
opt.target_role  = IMG_ROLE_FACE;
opt.tier_bonus   = 0.25;              /* score bonus when payload tier matches */
opt.role_bonus   = 0.20;              /* ditto for role */

img_drawing_pass(grid, memory, &opt, &face_stats);

// then a broader, looser brush for clothes:
img_brush_mask_rect(clothes_mask, 15, 28, 50, 58);
opt.region_mask = clothes_mask;
opt.target_tier = IMG_TIER_T2;
opt.target_role = IMG_ROLE_OBJECT;
img_drawing_pass(grid, memory, &opt, &clothes_stats);
```

Per-pass stats surface `cells_masked_out`, `brush_bonus_wins`, `unique_deltas_used`, `max_recent_count`.

---

## Frame-by-frame drawing — the CLI

`tools/draw.c` drives the engine as a small "video" generator: one drawing pass per frame, the grid rendered and saved after each pass. The last frame is the output. Earlier frames are the progressive stamping — underdrawing → detail.

```bash
./build/draw \
    --memory out/models/characters.imem \
    --model  out/models/characters.spai \
    --seed-kf 0 \
    --frames 8 \
    --out out/draw/kf0
```

Seeds (most-specific wins):

- `--seed-image <path>` — runs `img_pipeline_run` on the image and starts from the resulting CE grid.
- `--seed-kf <id>` — copies `Keyframe.ce_snapshot` from the given keyframe in `--model`.
- no seed — blank CE grid (every cell at the L6 fallback, so output is abstract).

Tunables: `--frames N`, `--top-g N`, `--penalty F` (presence penalty). Output:

```
out/draw/kf0/
├── frames/
│   ├── frame_000.{png,ppm}     state after pass 1
│   ├── frame_001.{png,ppm}     state after pass 2
│   ├── ...
│   └── frame_NNN.{png,ppm}     state after pass N (= last)
└── final.{png,ppm}             same as the last frame — the result
```

Empirically, seeding from a learned keyframe jumps `unique_deltas_used` per frame roughly 10× over a blank seed (each cell starts with real tone/role/depth context, so `topg` fallback pulls from L0 rather than L6).

---

## Multi-scale learn — tier-diverse rules from one image

```c
uint32_t radii[] = { 32, 12, 4, 0 };    /* coarsest → finest */
img_delta_memory_learn_multiscale(memory, image_rgb, w, h, radii, 4);
```

A separable O(w·h) box-blur cascade produces three adjacent (coarser → finer) pairs. Each pair feeds `learn_from_images`; the blur step controls which tier each pair lights up: big blur gap → T3 (structure) deltas; small gap → T1 (fine). Run on a 1280 × 720 portrait:

```
deltas added:      2 560
tier histogram:    T1 814 · T2 1 425 · T3 321
mode histogram:    INTENSITY 1 849 · DIRECTION 394 · MOOD 201 · ROLE 116
rarity buckets:    baseline 2 490 · 2–4× 33 · ≥4× 37
```

The tier spread is exactly what the cascade was designed to produce. `Drawing_pass` on an empty canvas with this memory still yields abstract pattern — that's the **Phase D** gap: there's no spatial seed yet, so every cell sees the same fallback context. Seed from a matching keyframe (`ai_predict` → `Keyframe.ce_snapshot`) is the next PR.

---

## Tools

| Tool | What it does | Source |
|---|---|---|
| `chat`          | REPL with turn-context + query router (`/gen /ret /img /topk`) + session persistence | `tools/chat.c` |
| `stream_train`  | Line-by-line text ingest with checkpointing, long-line auto-split, auto-threshold calibration | `tools/stream_train.c` |
| `train`         | Batch image training from a TSV manifest; emits `.spai` + `.imem`; `--resume` supported | `tools/train.c` |
| `draw`          | Frame-by-frame image generation: N drawing passes → `frames/frame_NNN.{png,ppm}` + `final.{png,ppm}`. Seed from a keyframe, an image, or empty canvas | `tools/draw.c` |
| `demo_pipeline` | One-shot image → CE → render. `--adapt` for per-image tier thresholds; emits PNG + PPM | `tools/demo_pipeline.c` |
| `gen_delta_tables` | Offline generator for the baked SoA CE delta tables | `tools/gen_delta_tables.c` |
| `bench_*`       | Text-engine benchmarks (perplexity, word-predict, QA, STS-B) | `tests/bench_*.c` |

---

## Training data formats

- **Text** — one clause per UTF-8 line. Auto-classified (PROSE / DIALOG / CODE / SHORT) by length + special-char ratio; per-type thresholds drive keyframe/delta routing. Lines longer than `--max-line-bytes` (default 256 = grid Y axis) auto-split at sentence boundaries.
- **Image** — any format `stb_image` reads (PNG / JPEG / BMP / TGA) or binary P6 PPM. Dim bounds `[16, 16384]` enforced. RGB only — alpha dropped. Block-averaged to 256 × 256.
- **Bimodal TSV manifest** — one row per learning example:
  ```
  <text_label>\t<before_image>\t<after_image>
  hero to visualization 1	assets/main_hero.png	assets/visualization_1.png
  ```
  Comments (`#`) and blank lines ignored.

---

## File formats

**SPAI (text side + bimodal)** — `magic[4]="SPAI"` · `version` · `kf_count` · `df_count` · `reserved[3]` (`reserved[0]` = save Unix timestamp), followed by a tagged record stream:

| Tag | Meaning |
|---|---|
| `0x01` KEYFRAME      | `id, label, text_byte_count, topic_hash, seq_in_topic, data_kind, grid.ARGB` |
| `0x02` DELTA         | sparse `(index, dA, dR, dG, dB)` entries against parent |
| `0x03` WEIGHTS       | per-channel adaptive weights (4 × float) |
| `0x04` CANVAS        | full 2048 × 1024 canvas snapshot |
| `0x05` SUBTITLE      | subtitle track |
| `0x06` EMA           | RGB EMA priors (4 × GRID_TOTAL × float) |
| `0x07` CANVAS_DELTA  | P-frame canvas (sparse A/R/G/B diff vs parent) |
| `0x08` CE_SNAPSHOT   | **bimodal: image-side CE grid bound to a keyframe** |

Older readers stop cleanly at unknown tags. `ai_peek_header_ex` surfaces `kf_count / df_count / version / save_timestamp` without loading the model.

**IMEM (image-side delta memory)** — `magic[4]="IMEM"` · `version` · `count` · `reserved`, followed by `count` × 40-byte unit records. Fields packed explicitly (little-endian); layout independent of struct padding. One record per `ImgDeltaUnit` including `id / pre_key / post_hint / payload.state / role_target / usage_count / success_count / weight`.

---

## Project layout

```
IMG-CANVAS/
├── assets/
│   ├── main_hero.png  visualization_{1,2}.png
│   └── characters/               bundled 10-char training set
├── docs/
│   └── benchmarks/v2_text_engine/  wiki5k / wiki20k reports
├── scripts/
│   └── windows/                  PowerShell wrappers (build/test/train/draw/demo)
├── out/                          runtime outputs (gitignored; see out/README.md)
├── sculpt/                       sibling engine — subtractive 16×16 chisel carver
│   ├── README.md                 phase log + usage + known limits
│   ├── prototype/                Phase 1 Python validator (pytest)
│   ├── include/  src/            Phase 3+ C engine, no external deps
│   ├── tests/                    7 C unit suites (cell/grid/prng/chisel/draw/edit/libio)
│   ├── tools/                    train_sculpt · draw_sculpt · edit_sculpt
│   ├── scripts/phase7_validate.py  end-to-end success-criteria runner
│   ├── data/                     .sraw / .slib / .slog (gitignored)
│   └── Makefile
├── spatial_ai/
│   ├── SPEC.md                   text engine spec v3
│   ├── SPEC-CE.md                image CE engine spec v1
│   ├── SPEC-ENGINE.md            optimisation notes
│   ├── TODO_recluster.md         recluster / calibration roadmap
│   ├── README.md                 text-engine local README (KR)
│   ├── Makefile
│   ├── include/
│   │   ├── spatial_*.h           text engine (12 headers)
│   │   ├── spatial_bimodal.h     text ↔ image binding
│   │   └── img_*.h               image CE engine + drawing (10 headers)
│   ├── src/                      one .c per header + img_delta_tables_data.c (GENERATED)
│   ├── tests/                    22 unit suites (make test)
│   ├── tools/                    chat / stream_train / train / draw / demo_pipeline / gen_delta_tables
│   ├── third_party/              stb_image + stb_image_write (public domain)
│   └── data/
│       ├── characters_manifest.tsv
│       ├── train_manifest.tsv
│       └── training/             user-local images (gitignored)
├── README.md                     this file
└── README_KO.md                  Korean summary
```

---

## Testing

```bash
cd spatial_ai && make test
```

| Group | Suite | Tests |
|---|---|---|
| **Text engine** | grid, morpheme, layers, match, keyframe, context, integration, io, cascade, canvas, adaptive, subtitle | 12 |
| **Image CE**    | img_ce, img_delta_memory (24 cases), img_set16, img_render, img_pipeline, img_tier_table, img_delta_learn (8 cases), img_ce_diff, img_drawing (9 cases) | 9 |
| **Bimodal**     | test_bimodal | 1 |
| **Total**       | | **22 suites** |

All green on every commit.

---

## Benchmarks

- **Text** — `docs/benchmarks/v2_text_engine/` — reference wiki5k and wiki20k runs with matching / self-recall / word-prediction / byte-perplexity / generation numbers under the v2 engine (commit `4c4e108`).
- **Image** — bimodal `./build/train` on the bundled character manifest reproduces `+7 633 deltas / 10 kf / 10 ce snapshots / 13 + 13 rare-bucket hits`. Multi-scale learn on a 1280 × 720 subject produces `+2 560 deltas` with tier spread `T1 814 · T2 1 425 · T3 321`.

---

## Known limits (as of current main)

- **Drawing on an empty canvas has no spatial intent yet.** Every cell sees the same L6-fallback context, so `drawing_pass` produces abstract pattern. Fix: seed from a matching keyframe's `ce_snapshot` (Phase D).
- **Pixel realisation is still SlotShape** (center / cross / corners / border dots per cell scaled by tier). A tier-aware rasteriser that turns each cell into coherent pixel strokes is the next render-side upgrade.
- **Compositional operators** (pose + palette + style as independent axes) aren't factored yet. Current axes are bounded (tier / scale / sign / mode / tick / channel_layout / slot_shape) but training never slices by intent.

---

## Related docs

- [`spatial_ai/SPEC.md`](spatial_ai/SPEC.md) — text engine specification
- [`spatial_ai/SPEC-CE.md`](spatial_ai/SPEC-CE.md) — image CE engine specification v1
- [`spatial_ai/SPEC-ENGINE.md`](spatial_ai/SPEC-ENGINE.md) — performance / layout notes
- [`spatial_ai/TODO_recluster.md`](spatial_ai/TODO_recluster.md) — recluster roadmap
- [`sculpt/README.md`](sculpt/README.md) — subtractive chisel engine phase log
- [`README_KO.md`](README_KO.md) — 한국어 요약

---

## License

See [LICENSE](LICENSE).
