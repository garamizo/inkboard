import io

import pytest
from PIL import Image

from inkboard_server.frame import (
    FRAME_BYTES, HEIGHT, WIDTH, calibration_pattern, etag_for, new_canvas, pack, png_bytes, to_1bit,
)


def test_pack_is_msb_first_and_white_is_one():
    img = new_canvas()
    img.putpixel((0, 0), 0)
    bits = pack(to_1bit(img))
    assert len(bits) == FRAME_BYTES == 48000
    assert bits[0] == 0x7F
    assert bits[1] == 0xFF
    assert bits[100] == 0xFF  # second row starts at byte 100


def test_threshold_140_is_black_141_is_white():
    img = new_canvas()
    img.putpixel((0, 0), 140)
    img.putpixel((1, 0), 141)
    out = to_1bit(img)
    assert out.getpixel((0, 0)) == 0
    assert out.getpixel((1, 0)) == 255


def test_wrong_size_rejected():
    with pytest.raises(ValueError, match="800x480"):
        to_1bit(Image.new("L", (10, 10), 255))


def test_etag_is_quoted_16_hex_and_stable():
    bits = pack(to_1bit(new_canvas()))
    tag = etag_for(bits)
    assert tag == etag_for(bits)
    assert tag.startswith('"') and tag.endswith('"') and len(tag) == 18
    int(tag[1:-1], 16)


def test_png_matches_packed_bits():
    img1 = to_1bit(new_canvas())
    decoded = Image.open(io.BytesIO(png_bytes(img1))).convert("1")
    assert decoded.size == (WIDTH, HEIGHT)
    assert decoded.tobytes() == pack(img1)


def test_calibration_pattern_corners():
    img = calibration_pattern()
    assert img.mode == "1"
    assert img.getpixel((5, 5)) == 0             # top-left solid square
    assert img.getpixel((WIDTH - 20, 20)) == 255  # top-right bracket is hollow
    assert img.getpixel((WIDTH - 20, HEIGHT - 20)) == 0  # bottom-right dot
