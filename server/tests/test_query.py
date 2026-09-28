from datetime import date

import pytest

from inkboard_server.query import FrameRequest, QueryError, parse_query
from inkboard_server.widgets.base import REQUIRED, ParamSpec, Size, Widget
from inkboard_server.widgets.params import coordinate, fmt_coord, int_in_range, one_of


class FakeA(Widget):
    type_name = "fake_a"
    supported_sizes = frozenset(Size)
    params = {"lat": ParamSpec(coordinate(-90, 90), fmt=fmt_coord),
              "units": ParamSpec(one_of("imperial", "metric"), default="imperial")}

    def fetch(self, sources, ctx): ...
    def render(self, img, box, data, ctx): ...


class FakeB(Widget):
    type_name = "fake_b"
    supported_sizes = frozenset({Size.THIRD})
    params = {"n": ParamSpec(int_in_range(1, 10), default=3)}

    def fetch(self, sources, ctx): ...
    def render(self, img, box, data, ctx): ...


REG = {"fake_a": FakeA, "fake_b": FakeB}


def parse(q: str) -> FrameRequest:
    return parse_query(q, REG)


def err(q: str) -> str:
    with pytest.raises(QueryError) as e:
        parse(q)
    return str(e.value)


def test_valid_with_defaults():
    r = parse("w=fake_a:2/3,fake_b:1/3&lat=34.05")
    assert [(s.type_name, s.size) for s in r.widgets] == [("fake_a", Size.TWO_THIRDS), ("fake_b", Size.THIRD)]
    assert r.tz.key == "UTC"
    assert dict(r.options) == {"lat": 34.0, "units": "imperial", "n": 3}
    assert r.canonical == "lat=34.0&n=3&tz=UTC&units=imperial&w=fake_a%3A2%2F3%2Cfake_b%3A1%2F3"


def test_canonical_ignores_order_and_rounding_noise():
    a = parse("w=fake_a:1&lat=34.04&tz=America/Los_Angeles")
    b = parse("tz=America/Los_Angeles&lat=34.0&w=fake_a:1&units=imperial")
    assert a.canonical == b.canonical


def test_url_encoded_w():
    assert parse("w=fake_a%3A1&lat=1").widgets == parse("w=fake_a:1&lat=1").widgets


def test_sizes_must_sum_to_three():
    assert err("w=fake_a:2/3,fake_a:2/3&lat=1") == "w: sizes add up to 4/3, need 3/3"
    assert err("w=fake_a:2/3&lat=1") == "w: sizes add up to 2/3, need 3/3"


def test_w_errors():
    assert err("lat=1").startswith("w: required")
    assert err("w=nope:1") == "w: unknown widget 'nope'"
    assert err("w=fake_a") == "w: expected type:size, got 'fake_a'"
    assert err("w=fake_a:1/2&lat=1") == "w: size must be 1/3, 2/3 or 1, got '1/2'"
    assert err("w=fake_b:1") == "w: fake_b does not support size 1"
    assert err("w=fake_b:1/3,fake_b:1/3,fake_b:1/3,fake_b:1/3") == "w: at most 3 widgets"


def test_duplicate_param():
    assert err("w=fake_a:1&w=fake_a:1&lat=1") == "w: given more than once"


def test_unknown_and_unused_params():
    assert err("w=fake_a:1&lat=1&foo=2") == "foo: unknown parameter"
    assert err("w=fake_a:1&lat=1&n=2") == "n: not used by any widget in w"


def test_required_param_missing():
    assert err("w=fake_a:1") == "lat: required by fake_a"


def test_param_value_errors():
    assert err("w=fake_a:1&lat=91").startswith("lat: ")
    assert err("w=fake_a:1&lat=nan").startswith("lat: ")
    assert err("w=fake_a:1&lat=abc").startswith("lat: ")
    assert err("w=fake_a:1&lat=1&units=kelvin").startswith("units: ")
    assert err("w=fake_b:1/3,fake_a:2/3&lat=1&n=11").startswith("n: ")


def test_tz_validation():
    assert parse("w=fake_a:1&lat=1&tz=Asia/Kolkata").tz.key == "Asia/Kolkata"
    assert err("w=fake_a:1&lat=1&tz=Nope/Zone") == "tz: unknown timezone 'Nope/Zone'"
    assert err("w=fake_a:1&lat=1&tz=../../etc/passwd").startswith("tz: unknown timezone")
    assert err("w=fake_a:1&lat=1&tz=America").startswith("tz: unknown timezone")


def test_query_limits():
    assert err("w=fake_a:1&lat=1&" + "x" * 1100) == "query longer than 1024 bytes"
    assert err("w=fake_a:1&lat=1&") == "malformed query string"
