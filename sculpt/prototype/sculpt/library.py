"""ChiselLibrary — store + lookup by (level, key) (DESIGN.md §5, §6.2)."""

from collections import defaultdict
from typing import Dict, List, Tuple

from .chisel import Chisel, NeighborStateKey


class ChiselLibrary:
    def __init__(self):
        self._by_level_and_key: Dict[Tuple[int, int], List[Chisel]] = defaultdict(list)
        self._by_level: Dict[int, List[Chisel]] = defaultdict(list)
        self._next_id: int = 1

    def register(self, level: int, key: NeighborStateKey,
                 subtract: Tuple[int, int, int, int]) -> Chisel:
        bucket = self._by_level_and_key[(level, key.pack_u64())]
        for existing in bucket:
            if existing.subtract_tuple() == subtract:
                existing.weight += 1
                return existing
        ch = Chisel(
            chisel_id=self._next_id,
            pre_state=key,
            subtract_r=subtract[0],
            subtract_g=subtract[1],
            subtract_b=subtract[2],
            subtract_a=subtract[3],
            level=level,
        )
        self._next_id += 1
        bucket.append(ch)
        self._by_level[level].append(ch)
        return ch

    def lookup(self, level: int, key: NeighborStateKey, top_g: int) -> List[Chisel]:
        bucket = self._by_level_and_key.get((level, key.pack_u64()), [])
        if bucket:
            ranked = sorted(bucket, key=lambda c: c.weight, reverse=True)
            return ranked[:top_g]
        all_at_level = self._by_level.get(level, [])
        if not all_at_level:
            return []
        ranked = sorted(all_at_level, key=lambda c: c.weight, reverse=True)
        return ranked[:top_g]

    def size(self) -> int:
        return sum(len(v) for v in self._by_level.values())

    def size_by_level(self) -> Dict[int, int]:
        return {lv: len(v) for lv, v in self._by_level.items()}
