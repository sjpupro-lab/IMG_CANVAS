"""Grid — 16x16 cell lattice with mandatory 8-neighbor access (DESIGN.md §4.2, P3)."""

from typing import List, Tuple

from .cell import Cell, EMPTY_NEIGHBOR
from .tuning import GRID_SIZE


DIRS_8 = [
    (-1, -1), (-1, 0), (-1, 1),
    ( 0, -1),          ( 0, 1),
    ( 1, -1), ( 1, 0), ( 1, 1),
]


class Grid:
    def __init__(self, size: int = GRID_SIZE):
        self.size = size
        self.cells: List[Cell] = [Cell() for _ in range(size * size)]

    def idx(self, x: int, y: int) -> int:
        return y * self.size + x

    def at(self, x: int, y: int) -> Cell:
        return self.cells[self.idx(x, y)]

    def in_bounds(self, x: int, y: int) -> bool:
        return 0 <= x < self.size and 0 <= y < self.size

    def neighbor_8(self, x: int, y: int) -> List[Cell]:
        """Return 8 neighbors in fixed DIRS_8 order.

        Out-of-bounds positions return EMPTY_NEIGHBOR (depth=0, i.e. not carved).
        """
        out = []
        for dx, dy in DIRS_8:
            nx, ny = x + dx, y + dy
            if self.in_bounds(nx, ny):
                out.append(self.at(nx, ny))
            else:
                out.append(EMPTY_NEIGHBOR)
        return out

    def iter_coords(self):
        for y in range(self.size):
            for x in range(self.size):
                yield x, y

    def to_rgb_array(self):
        """Recover original values = 255 - depth, for the RGB channels (P2)."""
        import numpy as np
        arr = np.zeros((self.size, self.size, 3), dtype=np.uint8)
        for x, y in self.iter_coords():
            c = self.at(x, y)
            arr[y, x, 0] = 255 - c.depth_r
            arr[y, x, 1] = 255 - c.depth_g
            arr[y, x, 2] = 255 - c.depth_b
        return arr
