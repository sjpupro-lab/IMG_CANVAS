"""Deterministic PRNG for noise-margin application (DESIGN.md §6.4).

Same seed -> same sequence. Used to make generation fully reproducible.
"""


class SplitMix64:
    _MASK = 0xFFFFFFFFFFFFFFFF

    def __init__(self, seed: int):
        self.state = seed & self._MASK

    def next_u64(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & self._MASK
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & self._MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & self._MASK
        return (z ^ (z >> 31)) & self._MASK

    def next_in_range(self, lo_inclusive: int, hi_inclusive: int) -> int:
        span = hi_inclusive - lo_inclusive + 1
        return lo_inclusive + (self.next_u64() % span)


def derive_seed(master: int, level: int, iter_idx: int, cell_id: int) -> int:
    return (master ^ (level << 48) ^ (iter_idx << 32) ^ cell_id) & 0xFFFFFFFFFFFFFFFF
