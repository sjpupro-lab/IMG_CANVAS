"""Draw from a freshly learned library (no persistence in Phase 1).

Usage: python scripts/run_draw.py <image_path> [seed]
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_ROOT = os.path.dirname(HERE)
sys.path.insert(0, PROTO_ROOT)

from sculpt.draw import draw  # noqa: E402
from sculpt.io import save_grid_as_png  # noqa: E402
from sculpt.learn import learn_image  # noqa: E402
from sculpt.library import ChiselLibrary  # noqa: E402


def main(argv):
    if len(argv) < 2:
        print("usage: run_draw.py <image_path> [seed]")
        return 2
    seed = int(argv[2]) if len(argv) >= 3 else 42
    library = ChiselLibrary()
    learn_image(argv[1], library)
    grid, _, stats = draw(library, master_seed=seed)
    out_dir = os.path.join(PROTO_ROOT, "out")
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f"run_draw_seed{seed}.png")
    save_grid_as_png(grid.to_rgb_array(), out_path)
    print(f"saved: {out_path}")
    print(f"stats: {stats}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
