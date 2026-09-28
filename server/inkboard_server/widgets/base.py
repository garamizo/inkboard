"""Widget contract: sizes, boxes, params, fetch/render split, and the registry."""
from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from datetime import date
from enum import Enum
from typing import Any, Callable, ClassVar
from zoneinfo import ZoneInfo

from PIL import Image

from ..sources.base import SourceResult

WIDGET_H = 464  # 480 minus the 16 px footer


class Size(Enum):
    THIRD = (1, 266)
    TWO_THIRDS = (2, 534)
    FULL = (3, 800)

    @property
    def thirds(self) -> int:
        return self.value[0]

    @property
    def width(self) -> int:
        return self.value[1]

    @property
    def token(self) -> str:
        return {1: "1/3", 2: "2/3", 3: "1"}[self.thirds]

    @classmethod
    def from_token(cls, token: str) -> "Size":
        for s in cls:
            if s.token == token:
                return s
        raise ValueError(token)


@dataclass(frozen=True)
class Box:
    x: int
    y: int
    w: int
    h: int


class _Required:
    def __repr__(self) -> str:
        return "REQUIRED"


REQUIRED: Any = _Required()


@dataclass(frozen=True)
class ParamSpec:
    parse: Callable[[str], Any]           # raises ValueError with a short reason
    default: Any = REQUIRED
    fmt: Callable[[Any], str] = str       # canonical text for the frame-cache key


@dataclass(frozen=True)
class RenderContext:
    today: date        # local date in the request tz
    tz: ZoneInfo


@dataclass
class WidgetData:
    payload: Any
    sources: list[SourceResult] = field(default_factory=list)

    @property
    def stale(self) -> bool:
        return any(s.stale for s in self.sources)


class Widget(ABC):
    type_name: ClassVar[str]
    supported_sizes: ClassVar[frozenset[Size]]
    params: ClassVar[dict[str, ParamSpec]] = {}

    def __init__(self, size: Size, options: dict[str, Any]):
        self.size = size
        self.options = options

    @abstractmethod
    def fetch(self, sources, ctx: RenderContext) -> WidgetData:
        """Read through cached sources. May raise NoData."""

    @abstractmethod
    def render(self, img: Image.Image, box: Box, data: WidgetData, ctx: RenderContext) -> None:
        """Draw into `box` of an "L" image. Pure: no I/O, output depends only on inputs."""

    def attributions(self) -> list[str]:
        return []


REGISTRY: dict[str, type[Widget]] = {}


def register(cls: type[Widget]) -> type[Widget]:
    REGISTRY[cls.type_name] = cls
    return cls
