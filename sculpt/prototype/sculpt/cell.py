"""Cell — the basic unit (DESIGN.md §4.1).

Depth = how much the max-value (255) has been carved away.
Original = 255 - depth. Invariant: 0 <= depth <= 255.
"""

from dataclasses import dataclass


@dataclass
class Cell:
    depth_r: int = 0
    depth_g: int = 0
    depth_b: int = 0
    depth_a: int = 0

    margin_l3: int = 0
    margin_l2: int = 0
    margin_l1: int = 0
    margin_l0: int = 0

    last_chisel_id: int = 0
    region_id: int = 0

    def depth_tuple(self):
        return (self.depth_r, self.depth_g, self.depth_b, self.depth_a)

    def original_tuple(self):
        return tuple(255 - d for d in self.depth_tuple())


def saturate_subtract(current: int, delta: int) -> int:
    """P1 enforcement: the ONLY way depth values change.

    Depth is stored as "amount already carved". Applying a chisel carves more,
    so depth increases; we clamp at 255. We call this saturate_subtract because
    conceptually we are subtracting from the remaining (255 - current) budget.
    """
    if delta <= 0:
        return current
    remaining_budget = 255 - current
    actual = min(delta, remaining_budget)
    return current + actual


EMPTY_NEIGHBOR = Cell()
