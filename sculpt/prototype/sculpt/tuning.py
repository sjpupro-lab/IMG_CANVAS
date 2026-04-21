"""Phase 1 initial values for open questions (DESIGN.md §12).

All values are starting points to be answered experimentally in Phase 1.
Keep every tunable constant in this single file.
"""

GRID_SIZE = 16

LEVEL_MARGIN = {3: 16, 2: 8, 1: 4, 0: 1}

LEVEL_BLUR_BOX = {3: 8, 2: 4, 1: 2, 0: 1}

TOP_G = {3: 2, 2: 4, 1: 4, 0: 8}

COHERENCE_BONUS = 2
COHERENCE_PENALTY = 1
COHERENCE_THRESHOLD = 24

A_BAND_WIDTH = 32

DEFAULT_ITERS = {3: 2, 2: 2, 1: 2, 0: 1}
