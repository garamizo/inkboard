from dataclasses import replace
from datetime import date

import pytest

from inkboard_server.market_data import summarize
from inkboard_server.series import DEFAULT_SERIES
from inkboard_server.sources.base import NoData
from inkboard_server.widgets import REGISTRY
from inkboard_server.widgets.base import Size, WidgetData
from inkboard_server.widgets.market_trends import MarketTrends

from .conftest import render_widget

OPTS = {"series": DEFAULT_SERIES, "years": 5}


def test_registered():
    assert REGISTRY["market_trends"] is MarketTrends


def test_fetch_builds_one_summary_per_series(sources, ctx):
    data = MarketTrends(Size.TWO_THIRDS, OPTS).fetch(sources, ctx)
    assert [s.id for s in data.payload.series] == list(DEFAULT_SERIES)
    assert len(data.sources) == 4 and not data.stale
    assert all(s.points[-1][0] == ctx.today for s in data.payload.series)


def test_fetch_propagates_nodata(sources, upstream, ctx):
    upstream.fail = True
    with pytest.raises(NoData):
        MarketTrends(Size.FULL, OPTS).fetch(sources, ctx)


def test_attributions_ordered_unique():
    w = MarketTrends(Size.FULL, OPTS)
    assert w.attributions() == ["FRED", "S&P DJI", "Coinbase", "Freddie Mac", "Realtor.com"]
    assert MarketTrends(Size.FULL, {"series": ("ust10y",), "years": 5}).attributions() == ["FRED"]


@pytest.mark.parametrize("size,name", [(Size.THIRD, "third"), (Size.TWO_THIRDS, "two_thirds"), (Size.FULL, "full")])
def test_golden(size, name, sources, ctx, golden):
    w = MarketTrends(size, OPTS)
    golden(f"market_trends_{name}", render_widget(w, w.fetch(sources, ctx), ctx))


def test_golden_short_series_label(sources, ctx, golden):
    # A series with less history than the window is labeled "(since YYYY)" and starts later on the x-axis.
    w = MarketTrends(Size.TWO_THIRDS, {"series": ("sp500", "home_la"), "years": 5})
    data = w.fetch(sources, ctx)
    home = sources.fred_series("MEDLISPRI31080").data
    recent = [(date.fromisoformat(d), v) for d, v in home if d >= "2024-01-01"]
    short = summarize("home_la", recent, ctx.today, 5)
    assert short.short_history
    payload = replace(data.payload, series=(data.payload.series[0], short))
    golden("market_trends_short_series", render_widget(w, WidgetData(payload, data.sources), ctx))
