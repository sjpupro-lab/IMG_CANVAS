"""Phase 8 — coordinate-descent sweep over sculpt_tuning_t.

Holds a fixed library + reference set, walks one parameter at a time, and
keeps any change that lowers the average MSE against the 16x16-downsampled
sources. Cheap and explainable; not a global optimum, but pinpoints which
knobs actually move the needle.

Usage:
    cd sculpt
    make
    python scripts/tune_sweep.py [--rounds N]

Defaults to one round, seed 42, mixed library over the 10 bundled
characters (consistent with Phase 7's metric definition).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCULPT = REPO / "sculpt"
BUILD = SCULPT / "build"
DATA = SCULPT / "data"
OUT = SCULPT / "out" / "phase8"

CHARS = [
    "char_01_ruby", "char_02_azure", "char_03_moss", "char_04_amber",
    "char_05_slate", "char_06_rose", "char_07_noir", "char_08_mint",
    "char_09_sand", "char_10_violet",
]


# ---- I/O helpers (mirror sculpt/src/learn.c resize rule) -------------------

def read_sraw(path: Path) -> bytes:
    with open(path, "rb") as f:
        if f.read(4) != b"SRAW":
            sys.exit(f"not SRAW: {path}")
        w, h, c = struct.unpack("<III", f.read(12))
        if c != 3:
            sys.exit("expected 3 channels")
        return f.read(w * h * c)


def downsample_16(rgb: bytes, w: int = 256, h: int = 256, target: int = 16) -> bytes:
    out = bytearray(target * target * 3)
    for y in range(target):
        sy = (y * h) // target
        for x in range(target):
            sx = (x * w) // target
            src = (sy * w + sx) * 3
            dst = (y * target + x) * 3
            out[dst:dst + 3] = rgb[src:src + 3]
    return bytes(out)


def mse_bytes(a: bytes, b: bytes) -> float:
    s = 0
    for x, y in zip(a, b):
        d = x - y
        s += d * d
    return s / len(a)


def psnr(mse: float) -> float:
    if mse <= 0:
        return float("inf")
    return 10.0 * math.log10((255.0 ** 2) / mse)


# ---- tuning struct mirror --------------------------------------------------

@dataclass
class Tuning:
    margin:       list[int] = field(default_factory=lambda: [1, 4, 8, 16])
    blur_box:     list[int] = field(default_factory=lambda: [1, 2, 4, 8])
    top_g:        list[int] = field(default_factory=lambda: [8, 4, 4, 2])
    iters:        list[int] = field(default_factory=lambda: [1, 2, 2, 2])
    coh_thresh:   list[int] = field(default_factory=lambda: [24, 24, 24, 24])
    coh_bonus:    list[int] = field(default_factory=lambda: [2, 2, 2, 2])
    coh_penalty:  list[int] = field(default_factory=lambda: [1, 1, 1, 1])
    a_band:       int = 32

    def clone(self) -> "Tuning":
        return Tuning(
            margin=list(self.margin), blur_box=list(self.blur_box),
            top_g=list(self.top_g), iters=list(self.iters),
            coh_thresh=list(self.coh_thresh), coh_bonus=list(self.coh_bonus),
            coh_penalty=list(self.coh_penalty), a_band=self.a_band,
        )

    def cli_args(self) -> list[str]:
        def quad(arr: list[int]) -> str:
            return ",".join(str(v) for v in arr)
        return [
            "--margin",       quad(self.margin),
            "--blur-box",     quad(self.blur_box),
            "--top-g",        quad(self.top_g),
            "--iters",        quad(self.iters),
            "--coh-thresh",   quad(self.coh_thresh),
            "--coh-bonus",    quad(self.coh_bonus),
            "--coh-penalty",  quad(self.coh_penalty),
            "--a-band",       str(self.a_band),
        ]


# ---- evaluator -------------------------------------------------------------

class Evaluator:
    def __init__(self, library: Path, refs: list[bytes], seed: int = 42) -> None:
        self.library = library
        self.refs = refs
        self.seed = seed
        self.tmp_out = OUT / "_trial.sraw"

    def __call__(self, t: Tuning) -> float:
        cmd = [str(BUILD / "tune_sculpt"),
               "--seed", str(self.seed),
               "--out", str(self.tmp_out)]
        cmd.extend(t.cli_args())
        cmd.append(str(self.library))
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            sys.exit("tune_sculpt failed")
        out_rgb = read_sraw(self.tmp_out)
        return sum(mse_bytes(out_rgb, r) for r in self.refs) / len(self.refs)


# ---- sweep -----------------------------------------------------------------

PER_LEVEL_GRIDS = {
    "margin":       [0, 1, 4, 8, 16, 32],
    "blur_box":     [1, 2, 4, 8],
    "top_g":        [1, 2, 4, 8],
    "iters":        [1, 2, 3],
    "coh_thresh":   [8, 16, 24, 32, 48, 96],
    "coh_bonus":    [0, 1, 2, 4],
    "coh_penalty":  [0, 1, 2],
}

SCALAR_GRID = {
    "a_band": [0, 16, 32, 64],
}


def sweep_one_round(eval_fn: Evaluator, best: Tuning, best_mse: float
                     ) -> tuple[Tuning, float, list[tuple[str, str, int, int, float, float]]]:
    """Returns (new_best, new_mse, change_log).

    change_log entry: (param_name, scope, level_or_-1, new_value, old_mse, new_mse).
    """
    log: list[tuple[str, str, int, int, float, float]] = []

    # Per-level parameters: walk each level independently.
    for name, grid in PER_LEVEL_GRIDS.items():
        for level in range(4):
            current = getattr(best, name)
            base_value = current[level]
            best_local_value = base_value
            best_local_mse = best_mse
            for v in grid:
                if v == base_value:
                    continue
                trial = best.clone()
                getattr(trial, name)[level] = v
                m = eval_fn(trial)
                if m < best_local_mse:
                    best_local_mse = m
                    best_local_value = v
            if best_local_value != base_value:
                old_mse = best_mse
                getattr(best, name)[level] = best_local_value
                best_mse = best_local_mse
                log.append((name, "level", level, best_local_value, old_mse, best_mse))

    # Scalar parameters.
    for name, grid in SCALAR_GRID.items():
        base_value = getattr(best, name)
        best_local_value = base_value
        best_local_mse = best_mse
        for v in grid:
            if v == base_value:
                continue
            trial = best.clone()
            setattr(trial, name, v)
            m = eval_fn(trial)
            if m < best_local_mse:
                best_local_mse = m
                best_local_value = v
        if best_local_value != base_value:
            old_mse = best_mse
            setattr(best, name, best_local_value)
            best_mse = best_local_mse
            log.append((name, "scalar", -1, best_local_value, old_mse, best_mse))

    return best, best_mse, log


# ---- main ------------------------------------------------------------------

def ensure_inputs() -> Path:
    OUT.mkdir(parents=True, exist_ok=True)
    if not (BUILD / "tune_sculpt").exists():
        sys.exit("build first: (cd sculpt && make)")
    # Regenerate .sraw if needed.
    missing = [DATA / f"{c}.sraw" for c in CHARS if not (DATA / f"{c}.sraw").exists()]
    if missing:
        subprocess.run(
            ["python3", str(DATA / "convert_png_to_sraw.py"), "--all-characters"],
            cwd=SCULPT, check=True,
        )
    library = OUT / "mixed.slib"
    if not library.exists():
        cmd = [str(BUILD / "train_sculpt"), "-o", str(library)] \
              + [str(DATA / f"{c}.sraw") for c in CHARS]
        subprocess.run(cmd, cwd=SCULPT, check=True, capture_output=True)
    return library


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    library = ensure_inputs()
    refs = [downsample_16(read_sraw(DATA / f"{c}.sraw")) for c in CHARS]
    evaluator = Evaluator(library, refs, seed=args.seed)

    best = Tuning()
    baseline_mse = evaluator(best)
    print(f"[baseline] mse={baseline_mse:.1f}  psnr={psnr(baseline_mse):.2f} dB")
    best_mse = baseline_mse

    full_log: list[tuple[str, str, int, int, float, float]] = []
    for r in range(args.rounds):
        print(f"\n[round {r + 1}/{args.rounds}] coordinate descent")
        best, best_mse, round_log = sweep_one_round(evaluator, best, best_mse)
        for entry in round_log:
            name, scope, lvl, v, om, nm = entry
            target = f"L{lvl}" if scope == "level" else "scalar"
            print(f"  {name:<13} {target:<6} -> {v:>4}   "
                  f"mse {om:.1f} -> {nm:.1f}  (Δ {om - nm:+.1f})")
        full_log.extend(round_log)
        if not round_log:
            print("  (no improving moves; converged)")
            break

    # Persist best config + summary.
    summary = {
        "seed": args.seed,
        "baseline_mse": baseline_mse,
        "best_mse": best_mse,
        "delta_mse": baseline_mse - best_mse,
        "delta_pct": (baseline_mse - best_mse) / baseline_mse * 100.0
                     if baseline_mse else 0.0,
        "best": {
            "margin": best.margin, "blur_box": best.blur_box,
            "top_g": best.top_g, "iters": best.iters,
            "coh_thresh": best.coh_thresh, "coh_bonus": best.coh_bonus,
            "coh_penalty": best.coh_penalty, "a_band": best.a_band,
        },
        "changes": [
            {"param": n, "scope": s, "level": l, "value": v,
             "old_mse": om, "new_mse": nm}
            for (n, s, l, v, om, nm) in full_log
        ],
    }
    summary_path = OUT / "best_tuning.json"
    summary_path.write_text(json.dumps(summary, indent=2))

    print()
    print(f"[summary] baseline mse {baseline_mse:.1f}  ->  best mse {best_mse:.1f}  "
          f"(Δ {baseline_mse - best_mse:+.1f}, {summary['delta_pct']:.1f}%)")
    print(f"[summary] PSNR {psnr(baseline_mse):.2f} dB  ->  {psnr(best_mse):.2f} dB")
    print(f"[summary] best config saved to {summary_path.relative_to(REPO)}")
    print()
    print("[best tuning] (drop into tune_sculpt to reproduce)")
    print("  --margin "      + ",".join(map(str, best.margin)))
    print("  --blur-box "    + ",".join(map(str, best.blur_box)))
    print("  --top-g "       + ",".join(map(str, best.top_g)))
    print("  --iters "       + ",".join(map(str, best.iters)))
    print("  --coh-thresh "  + ",".join(map(str, best.coh_thresh)))
    print("  --coh-bonus "   + ",".join(map(str, best.coh_bonus)))
    print("  --coh-penalty " + ",".join(map(str, best.coh_penalty)))
    print(f"  --a-band {best.a_band}")


if __name__ == "__main__":
    main()
