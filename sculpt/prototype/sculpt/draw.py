"""Drawing pipe — max-value canvas -> finished image (DESIGN.md §6)."""

from typing import Dict, List, Tuple

from .cell import Cell, saturate_subtract
from .chisel import Chisel, build_neighbor_key
from .grid import Grid
from .library import ChiselLibrary
from .prng import SplitMix64, derive_seed
from .tuning import (
    COHERENCE_BONUS, COHERENCE_PENALTY, COHERENCE_THRESHOLD,
    DEFAULT_ITERS, GRID_SIZE, LEVEL_MARGIN, TOP_G,
)


EditLogEntry = Tuple[int, int, int, int, int]  # (level, iter, cell_id, chisel_id, noise)


def _neighborhood_coherence(chisel: Chisel, cell: Cell, neighbors_8: List[Cell]) -> int:
    """Score how well a chisel blends with current neighbors (P3 + DESIGN §6.3)."""
    new_r = saturate_subtract(cell.depth_r, chisel.subtract_r)
    new_g = saturate_subtract(cell.depth_g, chisel.subtract_g)
    new_b = saturate_subtract(cell.depth_b, chisel.subtract_b)
    new_a = saturate_subtract(cell.depth_a, chisel.subtract_a)
    score = 0
    for n in neighbors_8:
        diff = (
            abs(new_r - n.depth_r) + abs(new_g - n.depth_g) +
            abs(new_b - n.depth_b) + abs(new_a - n.depth_a)
        )
        if diff < COHERENCE_THRESHOLD * 4:
            score += COHERENCE_BONUS
        else:
            score -= COHERENCE_PENALTY
    return score


def _apply_noise(ideal: int, margin: int, rng: SplitMix64) -> Tuple[int, int]:
    """Return (actual, noise_used). actual clamped to [0, 255]."""
    if margin <= 0:
        return ideal, 0
    noise = rng.next_in_range(-margin, margin)
    actual = max(0, min(255, ideal + noise))
    return actual, noise


def draw(library: ChiselLibrary, master_seed: int,
         iters: Dict[int, int] = None, size: int = GRID_SIZE):
    iters = iters or DEFAULT_ITERS
    grid = Grid(size=size)
    edit_log: List[EditLogEntry] = []
    score_sum_by_level = {lv: 0 for lv in (3, 2, 1, 0)}
    score_count_by_level = {lv: 0 for lv in (3, 2, 1, 0)}

    for level in (3, 2, 1, 0):
        margin = LEVEL_MARGIN[level]
        top_g = TOP_G[level]
        for iter_idx in range(iters[level]):
            for y in range(size):
                for x in range(size):
                    cell = grid.at(x, y)
                    neighbors = grid.neighbor_8(x, y)
                    key = build_neighbor_key(cell, neighbors)
                    candidates = library.lookup(level, key, top_g)
                    if not candidates:
                        continue
                    best = None
                    best_score = -10**9
                    for cand in candidates:
                        s = _neighborhood_coherence(cand, cell, neighbors) + cand.weight
                        if s > best_score:
                            best_score = s
                            best = cand
                    if best is None:
                        continue
                    cell_id = y * size + x
                    rng = SplitMix64(derive_seed(master_seed, level, iter_idx, cell_id))
                    used_noise = 0
                    for chan in ("r", "g", "b", "a"):
                        ideal = getattr(best, f"subtract_{chan}")
                        actual, noise = _apply_noise(ideal, margin, rng)
                        used_noise ^= noise & 0xFFFF
                        current = getattr(cell, f"depth_{chan}")
                        new_val = saturate_subtract(current, actual)
                        setattr(cell, f"depth_{chan}", new_val)
                    cell.last_chisel_id = best.chisel_id
                    setattr(cell, f"margin_l{level}", margin)
                    best.usage_count += 1
                    edit_log.append((level, iter_idx, cell_id, best.chisel_id, used_noise))
                    score_sum_by_level[level] += best_score
                    score_count_by_level[level] += 1

    avg_score_by_level = {
        lv: (score_sum_by_level[lv] / score_count_by_level[lv]
             if score_count_by_level[lv] else 0.0)
        for lv in (3, 2, 1, 0)
    }

    return grid, edit_log, {
        "avg_score_by_level": avg_score_by_level,
        "decisions_by_level": score_count_by_level,
    }
