#!/usr/bin/env python3
"""Flash inkboard firmware and open the serial monitor (Linux, macOS, Windows).

    python tools/flash.py [prod|dev|smoke|minimal] [--dry-run]

prod   dashboard firmware talking to the public server (SERVER_URL in include/config.h)
dev    same firmware pointed at this machine's `just dev` server on the LAN
       (http://<this machine's LAN IP>:8765, or $INKBOARD_DEV_URL if set)
smoke  hardware smoke test (extras/smoke)
minimal bare display check (extras/minimal)
"""
import os
import socket
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PIO_ENV = {"prod": "supermini-c6", "dev": "supermini-c6", "smoke": "smoke", "minimal": "minimal"}
DEV_PORT = 8765


def lan_ip() -> str:
    """Address of the interface that routes outward; connect() on UDP sends no packet."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("192.0.2.1", 80))
        return s.getsockname()[0]


def main(argv: list[str]) -> int:
    args = [a for a in argv[1:] if a != "--dry-run"]
    dry_run = "--dry-run" in argv
    target = args[0] if args else "prod"
    if target not in PIO_ENV:
        print(f"target must be one of: {', '.join(PIO_ENV)}", file=sys.stderr)
        return 2

    if target in ("prod", "dev"):
        secrets = ROOT / "include" / "secrets.h"
        if not secrets.exists() or "your-network" in secrets.read_text():
            print("Put your Wi-Fi credentials in include/secrets.h (copy include/secrets.h.example).",
                  file=sys.stderr)
            return 1

    env = dict(os.environ)
    env.pop("PLATFORMIO_BUILD_FLAGS", None)
    if target == "dev":
        url = os.environ.get("INKBOARD_DEV_URL") or f"http://{lan_ip()}:{DEV_PORT}"
        print(f"dev firmware -> {url} (keep `just dev` running on that machine)")
        env["PLATFORMIO_BUILD_FLAGS"] = f'-DSERVER_URL=\\"{url}\\"'

    cmd = ["pio", "run", "-e", PIO_ENV[target], "-t", "upload", "-t", "monitor"]
    if dry_run:
        print(" ".join(cmd))
        print(f"PLATFORMIO_BUILD_FLAGS={env.get('PLATFORMIO_BUILD_FLAGS', '')}")
        return 0
    return subprocess.call(cmd, cwd=ROOT, env=env)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
