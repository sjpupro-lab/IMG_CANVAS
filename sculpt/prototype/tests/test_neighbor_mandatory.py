"""P3: mandatory neighborhood — no decision can be made without 8 neighbors."""

import inspect

import pytest

from sculpt.cell import Cell
from sculpt.chisel import build_neighbor_key
from sculpt.grid import Grid


def test_build_neighbor_key_requires_8_neighbors():
    c = Cell()
    with pytest.raises(AssertionError):
        build_neighbor_key(c, [])
    with pytest.raises(AssertionError):
        build_neighbor_key(c, [Cell()] * 7)


def test_build_neighbor_key_signature_has_neighbors_param():
    sig = inspect.signature(build_neighbor_key)
    params = list(sig.parameters)
    assert "neighbors_8" in params, "P3: signature must expose neighbors_8"


def test_grid_neighbor_8_returns_exactly_8_cells():
    g = Grid(size=8)
    for x, y in ((0, 0), (7, 7), (3, 3), (0, 4), (7, 2)):
        neighbors = g.neighbor_8(x, y)
        assert len(neighbors) == 8
        for n in neighbors:
            assert isinstance(n, Cell)


def test_out_of_bounds_neighbors_are_uncarved():
    g = Grid(size=4)
    neighbors = g.neighbor_8(0, 0)
    # TL, T, TR, L, R, BL, B, BR — first four outside the grid for corner (0,0)
    for n in neighbors[:4]:
        assert n.depth_r == 0 and n.depth_g == 0
        assert n.depth_b == 0 and n.depth_a == 0
