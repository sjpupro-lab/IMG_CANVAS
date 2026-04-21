"""P1: subtractive-only. No `+=` against depth channels in the package source.

We scan the `sculpt/` package (not tests/scripts) for forbidden patterns.
The only addition operation allowed against depths is inside saturate_subtract,
which is deliberately housed in cell.py as the single entry point for carving.
"""

import os
import re

PKG_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "sculpt",
)

FORBIDDEN = [
    re.compile(r"\.depth_[rgba]\s*\+="),
    re.compile(r"cell\.depth_[rgba]\s*=\s*cell\.depth_[rgba]\s*\+[^=]"),
]


def _iter_py_files():
    for name in os.listdir(PKG_DIR):
        if name.endswith(".py"):
            yield os.path.join(PKG_DIR, name)


def test_no_direct_addition_against_depth():
    offenders = []
    for path in _iter_py_files():
        with open(path) as f:
            src = f.read()
        for pat in FORBIDDEN:
            for m in pat.finditer(src):
                offenders.append((path, m.group(0)))
    assert not offenders, f"P1 violation, raw depth addition found: {offenders}"


def test_saturate_subtract_is_the_only_carving_api():
    """All depth mutations must go through saturate_subtract.

    A line is safe if it either:
      (a) calls saturate_subtract on the right-hand side, or
      (b) uses setattr with a saturate_subtract result, or
      (c) is a dataclass default assignment inside cell.py.
    Any other assignment to .depth_[rgba] is a P1 violation.
    """
    suspicious = []
    has_depth_assign = re.compile(r"\.depth_[rgba]\s*=")
    for path in _iter_py_files():
        if path.endswith("cell.py"):
            continue
        with open(path) as f:
            for lineno, line in enumerate(f, 1):
                if not has_depth_assign.search(line):
                    continue
                if "saturate_subtract" in line:
                    continue
                suspicious.append((path, lineno, line.strip()))
    assert not suspicious, f"direct depth assignment outside carving: {suspicious}"
