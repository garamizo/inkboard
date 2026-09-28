"""Widgets. Importing this package registers every concrete widget."""
from .base import REGISTRY, WIDGET_H, Box, ParamSpec, RenderContext, Size, Widget, WidgetData, register
from . import market_trends  # noqa: F401  (registers)
from . import calendar_weather  # noqa: F401  (registers)

__all__ = ["REGISTRY", "WIDGET_H", "Box", "ParamSpec", "RenderContext", "Size", "Widget", "WidgetData", "register"]
