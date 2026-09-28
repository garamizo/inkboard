"""Query string -> validated FrameRequest (spec §2.2). Every error is a one-line QueryError."""
from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType
from typing import Any, Mapping
from urllib.parse import parse_qsl, urlencode
from zoneinfo import ZoneInfo

from .widgets.base import REQUIRED, REGISTRY, Size, Widget

MAX_QUERY_LEN = 1024
MAX_WIDGETS = 3
GLOBAL_PARAMS = frozenset({"w", "tz"})


class QueryError(ValueError):
    pass


@dataclass(frozen=True)
class WidgetSpec:
    type_name: str
    size: Size


@dataclass(frozen=True)
class FrameRequest:
    widgets: tuple[WidgetSpec, ...]
    tz: ZoneInfo
    options: Mapping[str, Any]
    canonical: str


def parse_tz(value: str) -> ZoneInfo:
    try:
        return ZoneInfo(value)
    except (KeyError, ValueError, OSError):
        raise QueryError(f"tz: unknown timezone {value!r}") from None


def parse_w(value: str, registry: Mapping[str, type[Widget]]) -> tuple[WidgetSpec, ...]:
    items = value.split(",")
    if len(items) > MAX_WIDGETS:
        raise QueryError(f"w: at most {MAX_WIDGETS} widgets")
    specs = []
    for item in items:
        type_name, sep, token = item.partition(":")
        if not sep:
            raise QueryError(f"w: expected type:size, got {item!r}")
        cls = registry.get(type_name)
        if cls is None:
            raise QueryError(f"w: unknown widget {type_name!r}")
        try:
            size = Size.from_token(token)
        except ValueError:
            raise QueryError(f"w: size must be 1/3, 2/3 or 1, got {token!r}") from None
        if size not in cls.supported_sizes:
            raise QueryError(f"w: {type_name} does not support size {token}")
        specs.append(WidgetSpec(type_name, size))
    total = sum(s.size.thirds for s in specs)
    if total != 3:
        raise QueryError(f"w: sizes add up to {total}/3, need 3/3")
    return tuple(specs)


def parse_query(raw: str, registry: Mapping[str, type[Widget]] | None = None) -> FrameRequest:
    registry = REGISTRY if registry is None else registry
    if len(raw) > MAX_QUERY_LEN:
        raise QueryError(f"query longer than {MAX_QUERY_LEN} bytes")
    try:
        pairs = parse_qsl(raw, keep_blank_values=True, strict_parsing=bool(raw))
    except ValueError:
        raise QueryError("malformed query string") from None
    params: dict[str, str] = {}
    for k, v in pairs:
        if k in params:
            raise QueryError(f"{k}: given more than once")
        params[k] = v
    if "w" not in params:
        raise QueryError("w: required, e.g. w=market_trends:2/3,calendar_weather:1/3")
    specs = parse_w(params["w"], registry)
    tz = parse_tz(params.get("tz", "UTC"))

    used: dict[str, tuple[Any, str]] = {}
    for s in specs:
        for name, spec in registry[s.type_name].params.items():
            used.setdefault(name, (spec, s.type_name))
    known = GLOBAL_PARAMS | {n for cls in registry.values() for n in cls.params}
    for k in params:
        if k not in known:
            raise QueryError(f"{k}: unknown parameter")
        if k not in GLOBAL_PARAMS and k not in used:
            raise QueryError(f"{k}: not used by any widget in w")

    options: dict[str, Any] = {}
    for name, (spec, owner) in sorted(used.items()):
        if name in params:
            try:
                options[name] = spec.parse(params[name])
            except ValueError as e:
                raise QueryError(f"{name}: {e}") from None
        elif spec.default is REQUIRED:
            raise QueryError(f"{name}: required by {owner}")
        else:
            options[name] = spec.default

    canonical = [("w", ",".join(f"{s.type_name}:{s.size.token}" for s in specs)), ("tz", tz.key)]
    canonical += [(n, used[n][0].fmt(options[n])) for n in options]
    return FrameRequest(specs, tz, MappingProxyType(options), urlencode(sorted(canonical)))
