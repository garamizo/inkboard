from pathlib import Path

import pytest
from PIL import Image

GOLDEN_DIR = Path(__file__).parent / "goldens"
FIXTURES = Path(__file__).parent / "fixtures"


def pytest_addoption(parser):
    parser.addoption("--update-goldens", action="store_true", help="rewrite golden PNGs")


@pytest.fixture
def golden(request):
    """Compare a mode-"1" image with tests/goldens/<name>.png pixel for pixel."""
    update = request.config.getoption("--update-goldens")

    def check(name: str, img: Image.Image) -> None:
        path = GOLDEN_DIR / f"{name}.png"
        if update:
            GOLDEN_DIR.mkdir(exist_ok=True)
            img.save(path)
            return
        assert path.exists(), f"missing golden {path.name}: run pytest --update-goldens, then inspect it"
        want = Image.open(path)
        assert (want.mode, want.size) == (img.mode, img.size), f"{name}: mode/size changed"
        if want.tobytes() != img.tobytes():
            actual = path.with_name(f"{name}.actual.png")
            img.save(actual)
            pytest.fail(f"{name} differs from its golden; wrote {actual.name}")

    return check


from datetime import datetime, timedelta, timezone


class FakeClock:
    def __init__(self, t: datetime):
        self.t = t

    def __call__(self) -> datetime:
        return self.t

    def advance(self, **kw) -> None:
        self.t += timedelta(**kw)


@pytest.fixture
def clock():
    return FakeClock(datetime(2026, 9, 27, 17, 0, tzinfo=timezone.utc))
