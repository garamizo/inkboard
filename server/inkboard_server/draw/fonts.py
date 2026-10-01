"""Bundled DejaVu fonts, cached per size and weight."""
from functools import lru_cache
from pathlib import Path

from PIL import ImageFont

FONT_DIR = Path(__file__).resolve().parents[3] / "tools" / "fonts"


@lru_cache(maxsize=None)
def font(size: int, bold: bool = False, condensed: bool = False) -> ImageFont.FreeTypeFont:
    name = "DejaVuSans" + ("Condensed" if condensed else "") + ("-Bold" if bold else "") + ".ttf"
    return ImageFont.truetype(str(FONT_DIR / name), size)
