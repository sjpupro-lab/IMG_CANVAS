"""Convert PNG files to the Sculpt .sraw format (DESIGN.md §10: no external
C libs, so image I/O happens in Python and the C engine reads a trivial
little-endian RGB dump).

File layout:
  4 bytes   "SRAW"
  4 bytes   uint32 width  (LE)
  4 bytes   uint32 height (LE)
  4 bytes   uint32 channels (3)
  W*H*3     RGB bytes, row-major top-down

Usage:
  python convert_png_to_sraw.py <input.png> [output.sraw]
  python convert_png_to_sraw.py --all-characters   # convert assets/characters/*.png
"""

import os
import struct
import sys

from PIL import Image


MAGIC = b"SRAW"


def convert(in_path: str, out_path: str) -> None:
    img = Image.open(in_path).convert("RGB")
    w, h = img.size
    raw = img.tobytes()
    with open(out_path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<III", w, h, 3))
        f.write(raw)
    print(f"wrote {out_path}  ({w}x{h}, {len(raw)} bytes)")


def main(argv):
    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

    if len(argv) >= 2 and argv[1] == "--all-characters":
        char_dir = os.path.join(repo_root, "assets", "characters")
        out_dir = os.path.dirname(os.path.abspath(__file__))
        count = 0
        for name in sorted(os.listdir(char_dir)):
            if not name.endswith(".png"):
                continue
            convert(os.path.join(char_dir, name),
                    os.path.join(out_dir, name.replace(".png", ".sraw")))
            count += 1
        print(f"converted {count} images")
        return 0

    if len(argv) < 2:
        print("usage: convert_png_to_sraw.py <input.png> [output.sraw]")
        print("       convert_png_to_sraw.py --all-characters")
        return 2

    in_path = argv[1]
    out_path = argv[2] if len(argv) >= 3 else in_path.replace(".png", ".sraw")
    convert(in_path, out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
