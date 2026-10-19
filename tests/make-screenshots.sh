#!/usr/bin/env bash
# Erstellt Screenshots von Corvus mit dem echten Breeze-Stil (Hell + Dunkel)
# und legt sie direkt in docs/screenshots/ ab.
# Aufruf von überall:  tests/make-screenshots.sh
# Voraussetzung: laufende KDE-Plasma-Sitzung mit installiertem Breeze (Standard),
# Build-Abhängigkeiten laut README (cmake, Qt6 inkl. Multimedia, qtkeychain, cargo).
set -euo pipefail

# Dieses Skript liegt in <repo>/tests; alle Pfade beziehen sich auf das Repo-Wurzelverzeichnis.
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
  QT_STYLE_OVERRIDE=breeze "$ROOT/build/shots" native "$OUT/Corvus_${scheme}.png" "$ROOT/tests/mock-signal-cli.py"
done
echo "Fertig: $OUT/"
