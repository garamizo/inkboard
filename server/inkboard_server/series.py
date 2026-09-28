"""Market series a board can request with series=... (spec §5.1)."""
from dataclasses import dataclass
from typing import Callable


@dataclass(frozen=True)
class SeriesDef:
    id: str
    fred_id: str
    label: str
    short: str
    fmt: Callable[[float], str]
    attribution: tuple[str, ...]


CATALOG: dict[str, SeriesDef] = {s.id: s for s in (
    SeriesDef("sp500", "SP500", "S&P 500", "S&P", lambda v: f"{v:,.0f}", ("FRED", "S&P DJI")),
    SeriesDef("btc", "CBBTCUSD", "Bitcoin", "BTC", lambda v: f"${v / 1000:.1f}k", ("FRED", "Coinbase")),
    SeriesDef("mortgage30", "MORTGAGE30US", "Mortgage", "Mort", lambda v: f"{v:.2f}%", ("FRED", "Freddie Mac")),
    SeriesDef("home_la", "MEDLISPRI31080", "LA home", "Home", lambda v: f"${v / 1e6:.2f}M", ("FRED", "Realtor.com")),
    SeriesDef("ust10y", "DGS10", "10-yr Treasury", "10y", lambda v: f"{v:.2f}%", ("FRED",)),
    SeriesDef("usd_broad", "DTWEXBGS", "Dollar index", "USD", lambda v: f"{v:.1f}", ("FRED",)),
)}

DEFAULT_SERIES: tuple[str, ...] = ("sp500", "btc", "mortgage30", "home_la")
