"""ImageDraw with 1-bit text rendering.

Anti-aliased glyphs lose thin strokes when the frame is thresholded to 1 bit (a 13 px "$"
turns into "s"); FreeType's monochrome mode hints glyphs for crisp black-and-white output.
"""
from PIL import Image, ImageDraw


def drawer(img: Image.Image) -> ImageDraw.ImageDraw:
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    return d
