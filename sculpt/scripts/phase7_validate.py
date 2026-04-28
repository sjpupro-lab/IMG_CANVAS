"""Phase 7 — end-to-end validation of the sculpt C engine.

Covers four success criteria:
  1. Per-character reproduction: train on a single character and draw with
     a fixed seed; measure MSE / PSNR against the 16x16-downsampled source.
  2. Mixed-library baseline: train on all 10 characters and draw; same
     metrics for comparison.
  3. Pipeline determinism: every train and every draw is executed twice
     and the output bytes are diffed; any mismatch aborts.
  4. Visual contact sheet: write a PNG grouping source / individual-library
     draw / mixed-library draw side by side for each character.

Run from repo root:
    python sculpt/scripts/phase7_validate.py

Requires: Pillow (already used by data/convert_png_to_sraw.py).
Depends on the prebuilt binaries in sculpt/build/ (run `make` first).
"""

from __future__ import annotations

import hashlib
import math
import os
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[2]
SCULPT = REPO / "sculpt"
BUILD = SCULPT / "build"
DATA = SCULPT / "data"
OUT = SCULPT / "out" / "phase7"


SEED = 42
# Character slugs (file order == visual order)
CHARS = [
    "char_01_ruby", "char_02_azure", "char_03_moss", "char_04_amber",
    "char_05_slate", "char_06_rose", "char_07_noir", "char_08_mint",
    "char_09_sand", "char_10_violet",
]


def must_exist(path: Path) -> Path:
    if not path.exists():
        sys.exit(f"required path missing: {path}")
    return path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def read_sraw(path: Path) -> tuple[int, int, bytes]:
    with open(path, "rb") as f:
        if f.read(4) != b"SRAW":
            sys.exit(f"not an SRAW file: {path}")
        w, h, c = struct.unpack("<III", f.read(12))
        if c != 3:
            sys.exit(f"unexpected channel count {c}")
        return w, h, f.read(w * h * c)


def downsampled_reference_rgb(sraw_path: Path, target: int = 16) -> bytes:
    """Nearest-neighbor downsample 256x256 .sraw to target x target,
    matching the resize rule inside sculpt/src/learn.c.
    """
    w, h, rgb = read_sraw(sraw_path)
    out = bytearray(target * target * 3)
    for y in range(target):
        sy = (y * h) // target
        for x in range(target):
            sx = (x * w) // target
            src = (sy * w + sx) * 3
            dst = (y * target + x) * 3
            out[dst:dst + 3] = rgb[src:src + 3]
    return bytes(out)


def mse(a: bytes, b: bytes) -> float:
    if len(a) != len(b):
        raise ValueError("length mismatch")
    s = 0
    for x, y in zip(a, b):
        d = x - y
        s += d * d
    return s / len(a)


def psnr(mse_value: float) -> float:
    if mse_value <= 0:
        return float("inf")
    return 10.0 * math.log10((255.0 ** 2) / mse_value)


def channel_means(rgb: bytes) -> tuple[float, float, float]:
    n = len(rgb) // 3
    r = sum(rgb[i * 3 + 0] for i in range(n)) / n
    g = sum(rgb[i * 3 + 1] for i in range(n)) / n
    b = sum(rgb[i * 3 + 2] for i in range(n)) / n
    return r, g, b


def run(cmd: list[str]) -> None:
    result = subprocess.run(cmd, cwd=SCULPT, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        sys.exit(f"command failed ({result.returncode}): {' '.join(cmd)}")


def train(slib: Path, images: list[Path]) -> None:
    cmd = [str(BUILD / "train_sculpt"), "-o", str(slib)] + [str(p) for p in images]
    run(cmd)


def draw(slib: Path, out: Path, seed: int = SEED) -> None:
    cmd = [str(BUILD / "draw_sculpt"), str(seed), str(out), str(slib)]
    run(cmd)


def train_twice_and_hash(label: str, images: list[Path]) -> str:
    a = OUT / f"{label}_a.slib"
    b = OUT / f"{label}_b.slib"
    train(a, images)
    train(b, images)
    if sha256_file(a) != sha256_file(b):
        sys.exit(f"train non-determinism: {label}")
    return sha256_file(a)


def draw_twice_and_hash(label: str, slib: Path) -> str:
    a = OUT / f"{label}_a.sraw"
    b = OUT / f"{label}_b.sraw"
    draw(slib, a)
    draw(slib, b)
    if sha256_file(a) != sha256_file(b):
        sys.exit(f"draw non-determinism: {label}")
    return sha256_file(a)


@dataclass
class CharResult:
    name: str
    ref_rgb: bytes
    indiv_rgb: bytes
    mixed_rgb: bytes
    indiv_mse: float
    mixed_mse: float


def make_contact_sheet(results: list[CharResult], target: Path, tile: int = 64) -> None:
    """Three rows per character: source, individual-library draw,
    mixed-library draw. Upscaled to tile x tile with nearest neighbor so the
    16x16 grid is visible.
    """
    cols = len(results)
    sheet = Image.new("RGB", (cols * tile, 3 * tile), (255, 255, 255))
    for col, r in enumerate(results):
        for row, rgb in enumerate([r.ref_rgb, r.indiv_rgb, r.mixed_rgb]):
            img = Image.frombytes("RGB", (16, 16), rgb).resize((tile, tile), Image.NEAREST)
            sheet.paste(img, (col * tile, row * tile))
    sheet.save(target)


def verify_spatial_ai_independence() -> None:
    """Confirm sculpt/ neither includes nor references spatial_ai/. A single
    subprocess invocation keeps the output compact.
    """
    spatial = REPO / "spatial_ai"
    if not spatial.exists():
        print("[indep] spatial_ai/ not present, skipping independence check")
        return
    sculpt_code = list((SCULPT / "include").rglob("*.h")) \
                + list((SCULPT / "src").rglob("*.c")) \
                + list((SCULPT / "tools").rglob("*.c")) \
                + list((SCULPT / "tools").rglob("*.h")) \
                + list((SCULPT / "tests").rglob("*.c"))
    offenders = []
    for path in sculpt_code:
        text = path.read_text()
        if "spatial_ai" in text:
            offenders.append(str(path.relative_to(REPO)))
    if offenders:
        sys.exit(f"[indep] FAIL: sculpt/ references spatial_ai/: {offenders}")
    print(f"[indep] OK: {len(sculpt_code)} C/H files scanned, zero spatial_ai references")


def main() -> None:
    must_exist(BUILD / "train_sculpt")
    must_exist(BUILD / "draw_sculpt")
    OUT.mkdir(parents=True, exist_ok=True)

    # Ensure .sraw sources exist; regenerate if missing.
    missing_sraw = [DATA / f"{c}.sraw" for c in CHARS if not (DATA / f"{c}.sraw").exists()]
    if missing_sraw:
        print("[setup] regenerating .sraw files from PNGs")
        run(["python3", str(DATA / "convert_png_to_sraw.py"), "--all-characters"])

    print("=" * 64)
    print("Phase 7 — end-to-end validation")
    print("=" * 64)

    # 1 + 3: per-character determinism + MSE/PSNR.
    results: list[CharResult] = []
    for name in CHARS:
        src = DATA / f"{name}.sraw"
        ref_rgb = downsampled_reference_rgb(src)
        slib_hash = train_twice_and_hash(f"indiv_{name}", [src])
        indiv_slib = OUT / f"indiv_{name}_a.slib"
        out_hash = draw_twice_and_hash(f"indiv_{name}", indiv_slib)
        _, _, indiv_rgb = read_sraw(OUT / f"indiv_{name}_a.sraw")
        indiv_mse = mse(ref_rgb, indiv_rgb)
        results.append(CharResult(
            name=name, ref_rgb=ref_rgb, indiv_rgb=indiv_rgb,
            mixed_rgb=b"", indiv_mse=indiv_mse, mixed_mse=0.0,
        ))
        print(f"[indiv] {name:<18} slib.sha256={slib_hash[:12]}..  "
              f"out.sha256={out_hash[:12]}..  mse={indiv_mse:6.1f}  "
              f"psnr={psnr(indiv_mse):5.2f} dB")

    # 2: mixed library baseline.
    mixed_hash = train_twice_and_hash("mixed", [DATA / f"{c}.sraw" for c in CHARS])
    mixed_slib = OUT / "mixed_a.slib"
    mixed_out_hash = draw_twice_and_hash("mixed", mixed_slib)
    _, _, mixed_rgb = read_sraw(OUT / "mixed_a.sraw")
    print()
    print(f"[mixed] library (10 chars)   slib.sha256={mixed_hash[:12]}..  "
          f"out.sha256={mixed_out_hash[:12]}..")
    print(f"[mixed] sample per-char MSE vs. source (single mixed draw, not per-char):")
    for r in results:
        r.mixed_rgb = mixed_rgb  # single draw shared across rows of the sheet
        r.mixed_mse = mse(r.ref_rgb, mixed_rgb)
        print(f"        {r.name:<18} mse={r.mixed_mse:6.1f}  psnr={psnr(r.mixed_mse):5.2f} dB")

    # Aggregate summary.
    indiv_avg = sum(r.indiv_mse for r in results) / len(results)
    mixed_avg = sum(r.mixed_mse for r in results) / len(results)
    print()
    print(f"[aggregate] indiv avg MSE = {indiv_avg:.1f}  (PSNR {psnr(indiv_avg):.2f} dB)")
    print(f"[aggregate] mixed avg MSE = {mixed_avg:.1f}  (PSNR {psnr(mixed_avg):.2f} dB)")
    improvement = (mixed_avg - indiv_avg) / mixed_avg * 100.0 if mixed_avg else 0.0
    print(f"[aggregate] per-char training reduces MSE by {improvement:.1f}% vs mixed")

    # Channel bias inspection (helps tuning in future phases).
    print()
    print("[bias] channel means (R, G, B):")
    print(f"        mixed draw : ({channel_means(mixed_rgb)[0]:5.1f}, "
          f"{channel_means(mixed_rgb)[1]:5.1f}, {channel_means(mixed_rgb)[2]:5.1f})")
    ref_means = [channel_means(r.ref_rgb) for r in results]
    ref_r = sum(m[0] for m in ref_means) / len(ref_means)
    ref_g = sum(m[1] for m in ref_means) / len(ref_means)
    ref_b = sum(m[2] for m in ref_means) / len(ref_means)
    print(f"        reference  : ({ref_r:5.1f}, {ref_g:5.1f}, {ref_b:5.1f})")

    # 4: contact sheet.
    sheet_path = OUT / "contact_sheet.png"
    make_contact_sheet(results, sheet_path)
    print()
    print(f"[sheet] {sheet_path.relative_to(REPO)}")

    # 5: spatial_ai independence.
    print()
    verify_spatial_ai_independence()

    print()
    print("Phase 7: all determinism checks passed")


if __name__ == "__main__":
    main()
