#!/usr/bin/env bash
# Creates screenshots of Corvus using the real Breeze style (Light + Dark)
# in 16:9 format (1280x720, as Flathub expects for app screenshots)
# and saves them directly to docs/screenshots/. The mock backend reports
# as corvus-cli; all contacts use the default avatars (no photos).
# Run from anywhere:  tests/make-screenshots.sh
# Prerequisite: a running KDE Plasma session with Breeze (default) installed,
# build dependencies as listed in the README (cmake, Qt6 including multimedia, qtkeychain, cargo).

set -euo pipefail

# This script is located in <repo>/tests; all paths are relative to the repo root directory.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/docs/screenshots"
mkdir -p "$OUT"

cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" -j"$(nproc)" --target shots
chmod +x "$ROOT/tests/mock-signal-cli.py"

old=$(kreadconfig6 --file kdeglobals --group General --key ColorScheme || echo BreezeLight)
restore() { plasma-apply-colorscheme "$old" >/dev/null 2>&1 || true; }
trap restore EXIT

for scheme in BreezeLight BreezeDark; do
  plasma-apply-colorscheme "$scheme" >/dev/null
  sleep 2
  # MOCK_VERSION: The mock reports corvus-cli as the backend (shown in Settings → Advanced).
  # MOCK_NO_AVATARS: getAvatar fails; the GUI displays the default avatars (circle with a letter).
  # Last argument of `shots`: image size “WIDTH_HEIGHT” (default 1280x720, 16:9 for Flathub).
  MOCK_VERSION="corvus-cli 1.2.0 compatible with signal-cli 0.14.9" MOCK_NO_AVATARS=1 \
    QT_STYLE_OVERRIDE=breeze "$ROOT/build/shots" native "$OUT/Corvus_${scheme}.png" \
    "$ROOT/tests/mock-signal-cli.py" 1280x720
done
echo "Finished: $OUT/"
