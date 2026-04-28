"""P2: max-baseline storage invariants (DESIGN.md §2.2)."""

from sculpt.cell import Cell, saturate_subtract
from sculpt.grid import Grid


def test_empty_grid_recovers_max_value():
    g = Grid(size=8)
    arr = g.to_rgb_array()
    assert (arr == 255).all(), "empty grid must render as full max (255) on every channel"


def test_depth_and_original_are_complements():
    for d in (0, 1, 50, 127, 200, 255):
        cell = Cell(depth_r=d, depth_g=d, depth_b=d, depth_a=d)
        orig = cell.original_tuple()
        assert all(o == 255 - d for o in orig)


def test_saturate_subtract_clamps_at_255():
    assert saturate_subtract(200, 100) == 255
    assert saturate_subtract(0, 300) == 255
    assert saturate_subtract(255, 10) == 255
    assert saturate_subtract(100, 0) == 100


def test_saturate_subtract_never_decreases_depth():
    for start in (0, 50, 128, 200, 255):
        for delta in (-5, 0, 1, 10, 100, 300):
            out = saturate_subtract(start, delta)
            assert out >= start, "P1 violation: depth decreased"
            assert 0 <= out <= 255
