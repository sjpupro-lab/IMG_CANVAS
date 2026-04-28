"""P4: noise margin is deterministic — same seed -> same result."""

import os

from sculpt.draw import draw
from sculpt.learn import learn_image
from sculpt.library import ChiselLibrary

REPO_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
IMG_PATH = os.path.join(REPO_ROOT, "assets", "characters", "char_01_ruby.png")


def _fresh_library():
    lib = ChiselLibrary()
    learn_image(IMG_PATH, lib)
    return lib


def test_same_seed_produces_identical_grids():
    lib = _fresh_library()
    g1, log1, _ = draw(lib, master_seed=42)
    g2, log2, _ = draw(lib, master_seed=42)
    assert log1 == log2, "edit logs differ for same seed"
    assert (g1.to_rgb_array() == g2.to_rgb_array()).all()


def test_different_seeds_produce_different_grids():
    lib = _fresh_library()
    g1, _, _ = draw(lib, master_seed=42)
    g2, _, _ = draw(lib, master_seed=1337)
    # Not guaranteed pixel-wise unique, but the edit logs must differ at least
    # in noise values.
    diff = (g1.to_rgb_array() != g2.to_rgb_array()).any()
    assert diff, "seeds 42 and 1337 should produce different output"


def test_learning_is_deterministic():
    lib1 = ChiselLibrary()
    lib2 = ChiselLibrary()
    learn_image(IMG_PATH, lib1)
    learn_image(IMG_PATH, lib2)
    assert lib1.size() == lib2.size()
    assert lib1.size_by_level() == lib2.size_by_level()
