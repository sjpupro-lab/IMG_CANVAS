"""Learn a single image and print library stats.

Usage: python scripts/run_learn.py <image_path>
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_ROOT = os.path.dirname(HERE)
sys.path.insert(0, PROTO_ROOT)

from sculpt.learn import learn_image  # noqa: E402
from sculpt.library import ChiselLibrary  # noqa: E402


def main(argv):
    if len(argv) < 2:
        print("usage: run_learn.py <image_path>")
        return 2
    library = ChiselLibrary()
    _, stats = learn_image(argv[1], library)
    print(f"library size: {library.size()}")
    print(f"by level: {library.size_by_level()}")
    print(f"entries registered per level: {stats['per_level_entries']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
