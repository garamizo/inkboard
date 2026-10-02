#!/usr/bin/env bash
# Throwaway (plan Task 1): build ENV, wait for the board's USB port, upload at once, then log serial.
# Usage: tools/flash-awake.sh [ENV]   (default spike-awake). Tap RESET when it says "waiting".
set -euo pipefail
cd "$(dirname "$0")/.."
env="${1:-spike-awake}"
pio run -e "$env"
echo "waiting for the board on USB (tap RESET now if it is asleep)..."
port=""
for _ in $(seq 1 3000); do
  port=$(ls /dev/ttyACM* 2>/dev/null | head -1 || true)
  [[ -n "$port" ]] && break
  sleep 0.1
done
[[ -n "$port" ]] || { echo "board never appeared on USB" >&2; exit 1; }
echo "found $port, uploading"
pio run -e "$env" -t upload -p "$port"
