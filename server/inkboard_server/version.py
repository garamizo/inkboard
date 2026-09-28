"""Server and firmware versions for the footer (repo VERSION; dev builds add -<git hash>)."""
from __future__ import annotations

import re
from collections.abc import Mapping
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
_FIRMWARE_UA = re.compile(r"inkboard/([0-9A-Za-z.+-]{1,32})")


def server_version(environ: Mapping[str, str], root: Path = REPO_ROOT) -> str:
    """INKBOARD_VERSION (set by `just dev` and the Docker build), else the repo's VERSION file."""
    if v := environ.get("INKBOARD_VERSION", "").strip():
        return v
    try:
        return (root / "VERSION").read_text().strip() or "dev"
    except OSError:
        return "dev"


def firmware_version(user_agent: str | None) -> str | None:
    """The board sends User-Agent: inkboard/<version>. Anything else is not a board."""
    m = _FIRMWARE_UA.fullmatch(user_agent or "")
    return m.group(1) if m else None


def version_label(firmware: str | None, server: str | None) -> str:
    parts = ([f"fw {firmware}"] if firmware else []) + ([f"srv {server}"] if server else [])
    return " · ".join(parts)
