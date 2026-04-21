#ifndef SCULPT_TUNING_H
#define SCULPT_TUNING_H

/* Phase 3 initial values, mirror of prototype/sculpt/tuning.py. */

#define SCULPT_GRID_SIZE      16
#define SCULPT_NUM_LEVELS     4

/* Per-level noise margin (P4). Indexed by level (0..3). */
static const int SCULPT_LEVEL_MARGIN[SCULPT_NUM_LEVELS] = { 1, 4, 8, 16 };

/* Per-level blur box size used during learning. */
static const int SCULPT_LEVEL_BLUR_BOX[SCULPT_NUM_LEVELS] = { 1, 2, 4, 8 };

/* Per-level top-G candidate count during drawing. */
static const int SCULPT_TOP_G[SCULPT_NUM_LEVELS] = { 8, 4, 4, 2 };

#define SCULPT_COHERENCE_BONUS     2
#define SCULPT_COHERENCE_PENALTY   1
#define SCULPT_COHERENCE_THRESHOLD 24

#define SCULPT_A_BAND_WIDTH        32

/* Default iteration counts per level, indexed 0..3. */
static const int SCULPT_DEFAULT_ITERS[SCULPT_NUM_LEVELS] = { 1, 2, 2, 2 };

#endif /* SCULPT_TUNING_H */
