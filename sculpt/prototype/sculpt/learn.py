"""Learning pipe — one image -> chisel library (DESIGN.md §5)."""

from typing import Tuple

import numpy as np

from .cell import Cell
from .chisel import build_neighbor_key
from .grid import Grid
from .io import load_image_as_grid_array
from .library import ChiselLibrary
from .tuning import GRID_SIZE, LEVEL_BLUR_BOX


def _interpret_4channels(rgb: np.ndarray) -> np.ndarray:
    """RGB image -> 4-channel values in [0, 255].

    Phase 1 initial rules:
      C0 (R): brightness (luminance)
      C1 (G): horizontal gradient magnitude
      C2 (B): vertical gradient magnitude
      C3 (A): local variance
    """
    h, w, _ = rgb.shape
    r = rgb[..., 0].astype(np.int32)
    g = rgb[..., 1].astype(np.int32)
    b = rgb[..., 2].astype(np.int32)
    lum = (r * 77 + g * 150 + b * 29) >> 8

    dx = np.zeros_like(lum)
    dy = np.zeros_like(lum)
    dx[:, 1:] = np.abs(lum[:, 1:] - lum[:, :-1])
    dy[1:, :] = np.abs(lum[1:, :] - lum[:-1, :])

    local_var = np.zeros_like(lum)
    pad = np.pad(lum, 1, mode="edge")
    for dyi in range(3):
        for dxi in range(3):
            local_var += np.abs(pad[dyi:dyi + h, dxi:dxi + w] - lum)
    local_var = local_var // 8

    out = np.stack([lum, dx * 2, dy * 2, local_var * 2], axis=-1)
    return np.clip(out, 0, 255).astype(np.uint8)


def _box_blur(channel_img: np.ndarray, box: int) -> np.ndarray:
    if box <= 1:
        return channel_img.copy()
    h, w = channel_img.shape
    out = np.zeros_like(channel_img, dtype=np.int32)
    for yy in range(h):
        y0 = (yy // box) * box
        y1 = min(y0 + box, h)
        for xx in range(w):
            x0 = (xx // box) * box
            x1 = min(x0 + box, w)
            out[yy, xx] = channel_img[y0:y1, x0:x1].mean()
    return out.astype(np.uint8)


def _depth_at_level(depth_img: np.ndarray, level: int) -> np.ndarray:
    box = LEVEL_BLUR_BOX[level]
    out = np.zeros_like(depth_img)
    for c in range(depth_img.shape[-1]):
        out[..., c] = _box_blur(depth_img[..., c], box)
    return out


def learn_image(img_path: str, library: ChiselLibrary,
                size: int = GRID_SIZE) -> Tuple[ChiselLibrary, dict]:
    rgb = load_image_as_grid_array(img_path, size=size)
    ch4 = _interpret_4channels(rgb)
    depth = (255 - ch4.astype(np.int32)).astype(np.uint8)  # P2: depth = 255 - original

    depth_by_level = {lv: _depth_at_level(depth, lv) for lv in (3, 2, 1, 0)}

    level_contribution = {
        3: depth_by_level[3].astype(np.int32),
        2: depth_by_level[2].astype(np.int32) - depth_by_level[3].astype(np.int32),
        1: depth_by_level[1].astype(np.int32) - depth_by_level[2].astype(np.int32),
        0: depth_by_level[0].astype(np.int32) - depth_by_level[1].astype(np.int32),
    }

    stats = {"per_level_entries": {lv: 0 for lv in (3, 2, 1, 0)}}

    # For key computation we walk the level-3 grid first (all depth=0 initially),
    # then level by level accumulating.
    running_grid = Grid(size=size)
    for level in (3, 2, 1, 0):
        contrib = np.clip(level_contribution[level], 0, 255).astype(np.uint8)
        for y in range(size):
            for x in range(size):
                cell = running_grid.at(x, y)
                neighbors = running_grid.neighbor_8(x, y)
                key = build_neighbor_key(cell, neighbors)
                sub = (
                    int(contrib[y, x, 0]),
                    int(contrib[y, x, 1]),
                    int(contrib[y, x, 2]),
                    int(contrib[y, x, 3]),
                )
                if any(sub):
                    library.register(level, key, sub)
                    stats["per_level_entries"][level] += 1
                # Advance running_grid (subtractive application, P1)
                from .cell import saturate_subtract
                cell.depth_r = saturate_subtract(cell.depth_r, sub[0])
                cell.depth_g = saturate_subtract(cell.depth_g, sub[1])
                cell.depth_b = saturate_subtract(cell.depth_b, sub[2])
                cell.depth_a = saturate_subtract(cell.depth_a, sub[3])

    return library, stats
