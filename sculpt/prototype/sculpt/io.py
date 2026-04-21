"""Image I/O via Pillow (DESIGN.md §10 allows numpy + Pillow)."""

import numpy as np
from PIL import Image

from .tuning import GRID_SIZE


def load_image_as_grid_array(path: str, size: int = GRID_SIZE) -> np.ndarray:
    img = Image.open(path).convert("RGB")
    img = img.resize((size, size), Image.Resampling.BOX)
    return np.asarray(img, dtype=np.uint8)


def save_grid_as_png(arr: np.ndarray, path: str, upscale: int = 16) -> None:
    img = Image.fromarray(arr, mode="RGB")
    if upscale > 1:
        img = img.resize(
            (img.width * upscale, img.height * upscale),
            Image.Resampling.NEAREST,
        )
    img.save(path)
