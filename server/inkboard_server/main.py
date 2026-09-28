"""Production entry point: uvicorn inkboard_server.main:app"""
import logging
import os
from pathlib import Path

from .app import create_app
from .sources import build_sources
from .version import server_version


def configure_logging(level: str) -> None:
    logging.basicConfig(level=level, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    # httpx logs every request URL at INFO, and FRED's URL carries the API key.
    for noisy in ("httpx", "httpcore"):
        logging.getLogger(noisy).setLevel(logging.WARNING)


configure_logging(os.environ.get("LOG_LEVEL", "INFO"))
_key = os.environ.get("FRED_API_KEY", "")
if not _key:
    logging.getLogger(__name__).warning("FRED_API_KEY is not set: market widgets will show 'No data yet'")

app = create_app(build_sources(Path(os.environ.get("INKBOARD_CACHE_DIR", ".cache")), _key),
                 client_ip_header=os.environ.get("INKBOARD_CLIENT_IP_HEADER") or None,
                 version=server_version(os.environ))
