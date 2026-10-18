#!/bin/sh
# Builds "signal-cli Mac.app" into macos/build/.
#   1. the Rust history store (static library, shared with the Qt app)
#   2. the Swift package (release)
#   3. the app bundle: executable, resources, Info.plist, icon, ad-hoc signature
set -eu

cd "$(dirname "$0")/.."
MACOS_DIR=$(pwd)
REPO_DIR=$(cd .. && pwd)

# The SwiftUI macros ship with Xcode, not with the Command Line Tools.
if [ -z "${DEVELOPER_DIR:-}" ] && [ -d /Applications/Xcode.app/Contents/Developer ]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi

VERSION=$(sed -n 's/^project(signal-cli-qt VERSION \([0-9.]*\).*/\1/p' "$REPO_DIR/CMakeLists.txt")
VERSION=${VERSION:-0.0.0}

echo "==> Building Rust history store"
cargo build --release --locked --manifest-path "$REPO_DIR/history-store/Cargo.toml"

echo "==> Building Swift package"
swift build -c release --package-path "$MACOS_DIR"
BIN=$(swift build -c release --package-path "$MACOS_DIR" --show-bin-path)

APP="$MACOS_DIR/build/signal-cli Mac.app"
echo "==> Assembling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN/signal-cli-mac" "$APP/Contents/MacOS/"
# SwiftPM resource bundle (sound) and the emoji data shared with the Qt app.
cp -R "$BIN/SignalCliMac_SignalCliMac.bundle" "$APP/Contents/Resources/"
cp "$REPO_DIR/resources/emoji-catalog.json" "$REPO_DIR/resources/emoji-names.json" "$APP/Contents/Resources/"
sed "s/@VERSION@/$VERSION/g" "$MACOS_DIR/Support/Info.plist" > "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"

# Icon from the shared SVG (Quick Look renders SVG; sips/iconutil build the .icns).
ICONSET=$(mktemp -d)/AppIcon.iconset
mkdir -p "$ICONSET"
if qlmanage -t -s 1024 -o "$(dirname "$ICONSET")" "$REPO_DIR/packaging/signal-cli-qt.svg" >/dev/null 2>&1; then
    SRC="$(dirname "$ICONSET")/signal-cli-qt.svg.png"
    for size in 16 32 128 256 512; do
        sips -z $size $size "$SRC" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
        sips -z $((size * 2)) $((size * 2)) "$SRC" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns" || echo "warning: icon could not be created"
else
    echo "warning: icon could not be rendered, the app uses the generic icon"
fi

echo "==> Signing (ad hoc)"
codesign --force --sign - --timestamp=none "$APP"

echo "Done: $APP"
