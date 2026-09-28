"""Reusable query-param parsers. Each raises ValueError with a short reason."""
import math
from typing import Callable, Iterable


def coordinate(lo: float, hi: float) -> Callable[[str], float]:
    def parse(text: str) -> float:
        v = float(text)
        if not math.isfinite(v) or not lo <= v <= hi:
            raise ValueError(f"must be a number from {lo:g} to {hi:g}")
        return float(f"{v:.1f}") + 0.0  # 0.1 deg cells; -0.0 -> 0.0

    return parse


def fmt_coord(v: float) -> str:
    return f"{v:.1f}"


def int_in_range(lo: int, hi: int) -> Callable[[str], int]:
    def parse(text: str) -> int:
        try:
            v = int(text)
        except ValueError:
            raise ValueError(f"must be a whole number from {lo} to {hi}") from None
        if not lo <= v <= hi:
            raise ValueError(f"must be a whole number from {lo} to {hi}")
        return v

    return parse


def one_of(*choices: str) -> Callable[[str], str]:
    def parse(text: str) -> str:
        if text not in choices:
            raise ValueError("must be one of " + ", ".join(choices))
        return text

    return parse


def id_list(allowed: Iterable[str], max_n: int) -> Callable[[str], tuple[str, ...]]:
    allowed = tuple(allowed)

    def parse(text: str) -> tuple[str, ...]:
        ids = tuple(text.split(","))
        if not 1 <= len(ids) <= max_n:
            raise ValueError(f"give 1 to {max_n} ids")
        if len(set(ids)) != len(ids):
            raise ValueError("ids must be unique")
        unknown = [i for i in ids if i not in allowed]
        if unknown:
            raise ValueError(f"unknown id {unknown[0]!r}; choose from " + ", ".join(allowed))
        return ids

    return parse
