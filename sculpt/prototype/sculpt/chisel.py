"""Chisel and NeighborStateKey (DESIGN.md §4.3, §4.4).

Chisel = absolute carving instruction at one level.
NeighborStateKey = hash key encoding self + 8 neighbor depths. Self-only
matching is forbidden (P3).
"""

from dataclasses import dataclass
from typing import List, Tuple

from .cell import Cell


def _quantize_self_channel(depth: int) -> int:
    # 4 bits per self channel = 16 buckets of 16.
    return (depth >> 4) & 0xF


def _quantize_neighbor(cell: Cell) -> int:
    # 3 bits per neighbor, averaged across 4 channels = 8 buckets of ~32.
    avg = (cell.depth_r + cell.depth_g + cell.depth_b + cell.depth_a) // 4
    return (avg >> 5) & 0x7


@dataclass(frozen=True)
class NeighborStateKey:
    self_r: int
    self_g: int
    self_b: int
    self_a: int
    n_tl: int
    n_t: int
    n_tr: int
    n_l: int
    n_r: int
    n_bl: int
    n_b: int
    n_br: int

    def pack_u64(self) -> int:
        # 4*4 + 8*3 = 40 bits, fits uint64 with room.
        v = 0
        for bits, val in (
            (4, self.self_r), (4, self.self_g), (4, self.self_b), (4, self.self_a),
            (3, self.n_tl), (3, self.n_t), (3, self.n_tr),
            (3, self.n_l),                   (3, self.n_r),
            (3, self.n_bl), (3, self.n_b), (3, self.n_br),
        ):
            v = (v << bits) | (val & ((1 << bits) - 1))
        return v


def build_neighbor_key(self_cell: Cell, neighbors_8: List[Cell]) -> NeighborStateKey:
    """Build the matching key. neighbors_8 is REQUIRED — signature enforces P3.

    Expected neighbor order (grid.DIRS_8):
      TL, T, TR, L, R, BL, B, BR
    """
    assert len(neighbors_8) == 8, "P3 violation: must supply all 8 neighbors"
    tl, t, tr, l, r, bl, b, br = neighbors_8
    return NeighborStateKey(
        self_r=_quantize_self_channel(self_cell.depth_r),
        self_g=_quantize_self_channel(self_cell.depth_g),
        self_b=_quantize_self_channel(self_cell.depth_b),
        self_a=_quantize_self_channel(self_cell.depth_a),
        n_tl=_quantize_neighbor(tl),
        n_t=_quantize_neighbor(t),
        n_tr=_quantize_neighbor(tr),
        n_l=_quantize_neighbor(l),
        n_r=_quantize_neighbor(r),
        n_bl=_quantize_neighbor(bl),
        n_b=_quantize_neighbor(b),
        n_br=_quantize_neighbor(br),
    )


@dataclass
class Chisel:
    chisel_id: int
    pre_state: NeighborStateKey
    subtract_r: int
    subtract_g: int
    subtract_b: int
    subtract_a: int
    level: int
    weight: int = 1
    usage_count: int = 0

    def subtract_tuple(self) -> Tuple[int, int, int, int]:
        return (self.subtract_r, self.subtract_g, self.subtract_b, self.subtract_a)
