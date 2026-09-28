"""Frame geometry, 1-bit conversion, packing, ETag and the hardware calibration pattern."""
import hashlib
import io

from PIL import Image, ImageDraw

from .draw.fonts import font

WIDTH, HEIGHT = 800, 480
FRAME_BYTES = WIDTH * HEIGHT // 8
THRESHOLD = 140


def new_canvas() -> Image.Image:
    return Image.new("L", (WIDTH, HEIGHT), 255)


def to_1bit(img: Image.Image) -> Image.Image:
    """Threshold an "L" image into mode "1" (no dithering: text and lines stay crisp)."""
    if img.size != (WIDTH, HEIGHT):
        raise ValueError(f"frame must be {WIDTH}x{HEIGHT}, got {img.size[0]}x{img.size[1]}")
    bw = img.convert("L").point(lambda p: 255 if p > THRESHOLD else 0)
    return bw.convert("1", dither=Image.Dither.NONE)


def pack(img1: Image.Image) -> bytes:
    """Rows top to bottom, MSB = leftmost pixel, bit 1 = white (Pillow's mode "1" layout)."""
    bits = img1.tobytes()
    if len(bits) != FRAME_BYTES:
        raise ValueError(f"packed frame is {len(bits)} bytes, expected {FRAME_BYTES}")
    return bits


def etag_for(bits: bytes) -> str:
    return '"' + hashlib.sha256(bits).hexdigest()[:16] + '"'


def png_bytes(img1: Image.Image) -> bytes:
    buf = io.BytesIO()
    img1.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def calibration_pattern() -> Image.Image:
    """Asymmetric marks so rotation, mirroring and inversion are all visible on the panel."""
    img = new_canvas()
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 39, 39], fill=0)                                          # top-left: solid square
    d.line([(WIDTH - 40, 1), (WIDTH - 2, 1), (WIDTH - 2, 39)], fill=0, width=4)   # top-right: bracket
    d.line([(1, HEIGHT - 40), (1, HEIGHT - 2), (39, HEIGHT - 2)], fill=0, width=4)  # bottom-left: bracket
    d.ellipse([WIDTH - 40, HEIGHT - 40, WIDTH - 1, HEIGHT - 1], fill=0)            # bottom-right: dot
    d.text((52, 10), "TOP LEFT (solid square)", font=font(20, bold=True), fill=0)
    d.text((WIDTH - 52, HEIGHT - 10), "BOTTOM RIGHT (dot)", font=font(20, bold=True), fill=0, anchor="rb")
    d.text((WIDTH // 2, 110), "inkboard calibration", font=font(36, bold=True), fill=0, anchor="mm")
    d.text((WIDTH // 2, 155), "Black text on white means the invert setting is right.",
           font=font(18), fill=0, anchor="mm")
    for y in range(200, 400, 8):
        for x in range(200, 600, 8):
            if ((x - 200) // 8 + (y - 200) // 8) % 2 == 0:
                d.rectangle([x, y, x + 7, y + 7], fill=0)
    return to_1bit(img)
