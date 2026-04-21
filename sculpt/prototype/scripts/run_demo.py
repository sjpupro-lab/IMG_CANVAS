"""Phase 1 demo: learn one character, draw, verify determinism.

Usage: python scripts/run_demo.py
Outputs to: sculpt/prototype/out/
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_ROOT = os.path.dirname(HERE)
REPO_ROOT = os.path.dirname(os.path.dirname(PROTO_ROOT))
sys.path.insert(0, PROTO_ROOT)

from sculpt.draw import draw  # noqa: E402
from sculpt.io import save_grid_as_png  # noqa: E402
from sculpt.learn import learn_image  # noqa: E402
from sculpt.library import ChiselLibrary  # noqa: E402


def main():
    img_path = os.path.join(REPO_ROOT, "assets", "characters", "char_01_ruby.png")
    out_dir = os.path.join(PROTO_ROOT, "out")
    os.makedirs(out_dir, exist_ok=True)

    print(f"[1] Learning {os.path.basename(img_path)} ...")
    library = ChiselLibrary()
    _, learn_stats = learn_image(img_path, library)
    print(f"    chisels registered: {library.size()}  "
          f"by_level={library.size_by_level()}")

    print("[2] Drawing with master_seed=42 ...")
    grid1, log1, stats1 = draw(library, master_seed=42)
    arr1 = grid1.to_rgb_array()
    save_grid_as_png(arr1, os.path.join(out_dir, "phase1_demo.png"))
    print(f"    decisions_by_level={stats1['decisions_by_level']}")
    print(f"    avg_score_by_level={stats1['avg_score_by_level']}")

    print("[3] Drawing again with master_seed=42 (determinism check) ...")
    grid2, log2, _ = draw(library, master_seed=42)
    arr2 = grid2.to_rgb_array()
    identical = (arr1 == arr2).all() and log1 == log2
    print(f"    identical: {identical}")
    save_grid_as_png(arr2, os.path.join(out_dir, "phase1_demo_run2.png"))

    print("[4] Drawing with master_seed=1337 (different-seed check) ...")
    grid3, log3, _ = draw(library, master_seed=1337)
    arr3 = grid3.to_rgb_array()
    differs = not (arr1 == arr3).all()
    print(f"    differs from seed=42: {differs}")
    save_grid_as_png(arr3, os.path.join(out_dir, "phase1_demo_seed1337.png"))

    report = {
        "learn_stats": {
            "per_level_entries": learn_stats["per_level_entries"],
            "library_size": library.size(),
            "library_size_by_level": library.size_by_level(),
        },
        "draw_stats_seed42": {
            "decisions_by_level": stats1["decisions_by_level"],
            "avg_score_by_level": stats1["avg_score_by_level"],
        },
        "determinism_same_seed": bool(identical),
        "different_seed_differs": bool(differs),
        "edit_log_length": len(log1),
    }
    report_path = os.path.join(out_dir, "phase1_report.json")
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)
    print(f"[5] Report saved to {report_path}")
    print("    DONE.")


if __name__ == "__main__":
    main()
