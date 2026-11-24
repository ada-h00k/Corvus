#!/usr/bin/env bash
# Builds corvus-linux-x86_64.AppImage from an already built and installed Corvus.
#
# The AppImage is meant to work like the Flatpak: besides the Qt libraries it bundles
#   * corvus-cli (the Signal backend),
#   * qrencode (QR code when linking a device) with its libraries,
#   * libhunspell and the Hunspell dictionaries of the Flatpak.
#
# Inputs (environment):
#   APPDIR           AppDir that already holds the `cmake --install` result (usr/...)
#   QMAKE            qmake of the Qt that Corvus was built with
#   OUTPUT           file name of the AppImage to create
#   EXTRA_LIB_PATH   optional, ':'-separated library directories (e.g. QtKeychain)
#   TOOLS_DIR        optional, where linuxdeploy and friends are downloaded to
#   CORVUS_CLI_TAG   optional, corvus-cli release to bundle (default: newest release)
#   UPDATE_INFORMATION  optional, AppImage update information to embed (default: the latest
#                    GitHub release of this repository, delta updates through zsync)
#   GITHUB_TOKEN     optional, used for the GitHub API request that finds that release
set -euo pipefail

: "${APPDIR:?}" "${QMAKE:?}" "${OUTPUT:?}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tools_dir="${TOOLS_DIR:-$PWD/appimage-tools}"
APPDIR="$(cd "$APPDIR" && pwd)"
mkdir -p "$tools_dir"
tools_dir="$(cd "$tools_dir" && pwd)"

# Pinned tools, verified by SHA-256.
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20250213-2/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA256="4648f278ab3ef31f819e67c30d50f462640e5365a77637d7e6f2ad9fd0b4522a"
PLUGIN_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage"
PLUGIN_QT_SHA256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
# appimagetool would otherwise download the moving "continuous" runtime at build time.
RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
RUNTIME_SHA256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"

# Update information for AppImageUpdate, Gear Lever, AppImageLauncher, ...: look for
# corvus-linux-x86_64.AppImage.zsync in the latest GitHub release.
UPDATE_INFORMATION="${UPDATE_INFORMATION:-gh-releases-zsync|ada-h00k|Corvus|latest|corvus-linux-x86_64.AppImage.zsync}"

# AppImages run without FUSE (CI runners and containers have none).
export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH=x86_64

download() { # url sha256 file
    local url="$1" sha="$2" file="$3"
    if [[ ! -f "$file" ]]; then
        curl -fsSL --retry 3 -o "$file" "$url"
    fi
    echo "$sha  $file" | sha256sum -c -
    chmod +x "$file"
}

download "$LINUXDEPLOY_URL" "$LINUXDEPLOY_SHA256" "$tools_dir/linuxdeploy-x86_64.AppImage"
download "$PLUGIN_QT_URL" "$PLUGIN_QT_SHA256" "$tools_dir/linuxdeploy-plugin-qt-x86_64.AppImage"
download "$APPIMAGETOOL_URL" "$APPIMAGETOOL_SHA256" "$tools_dir/appimagetool-x86_64.AppImage"
download "$RUNTIME_URL" "$RUNTIME_SHA256" "$tools_dir/runtime-x86_64"

# --------------------------------------------------
# corvus-cli: newest published release, checked against the SHA256SUMS of that release
# --------------------------------------------------
echo "::group::corvus-cli"
cli_api="https://api.github.com/repos/ada-h00k/corvus-cli/releases/latest"
auth=()
if [[ -n "${GITHUB_TOKEN:-}" ]]; then auth=(-H "Authorization: Bearer $GITHUB_TOKEN"); fi
if [[ -z "${CORVUS_CLI_TAG:-}" ]]; then
    CORVUS_CLI_TAG="$(curl -fsSL --retry 3 "${auth[@]}" -H 'Accept: application/vnd.github+json' "$cli_api" \
        | python3 -c 'import json,re,sys; t=json.load(sys.stdin)["tag_name"]; assert re.fullmatch(r"v\d+\.\d+\.\d+|commit-[0-9a-f]{40}", t), t; print(t)')"
fi
cli_tag="$CORVUS_CLI_TAG"
echo "corvus-cli release: $cli_tag"

cli_dir="$(mktemp -d)"
cli_base="https://github.com/ada-h00k/corvus-cli/releases/download/$cli_tag"
curl -fsSL --retry 3 -o "$cli_dir/corvus-cli-linux-x86_64" "$cli_base/corvus-cli-linux-x86_64"
curl -fsSL --retry 3 -o "$cli_dir/SHA256SUMS" "$cli_base/SHA256SUMS"
(cd "$cli_dir" && grep ' corvus-cli-linux-x86_64$' SHA256SUMS | sha256sum -c -)
install -Dm755 "$cli_dir/corvus-cli-linux-x86_64" "$APPDIR/usr/bin/corvus-cli"
rm -rf "$cli_dir"
"$APPDIR/usr/bin/corvus-cli" --version
echo "::endgroup::"

# --------------------------------------------------
# Hunspell dictionaries: exactly the files (and checksums) of the Flatpak manifest
# --------------------------------------------------
echo "::group::Hunspell dictionaries"
python3 - "$repo_root/packaging/flatpak/io.github.ada_h00k.Corvus.yml" "$APPDIR" <<'PY'
import hashlib
import sys
import urllib.request
from pathlib import Path

import yaml

manifest = yaml.safe_load(Path(sys.argv[1]).read_text())
appdir = Path(sys.argv[2])
module = next(m for m in manifest["modules"] if m["name"] == "hunspell-dictionaries")

dict_dir = appdir / "usr/share/hunspell"
license_dir = appdir / "usr/share/licenses/corvus/dictionaries"
dict_dir.mkdir(parents=True, exist_ok=True)
license_dir.mkdir(parents=True, exist_ok=True)

count = 0
for source in module["sources"]:
    name = source["dest-filename"]
    with urllib.request.urlopen(source["url"], timeout=60) as response:
        data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != source["sha256"]:
        raise SystemExit(f"{name}: SHA-256 mismatch ({digest})")
    target = license_dir if name.startswith("README_") else dict_dir
    (target / name).write_bytes(data)
    count += 1
print(f"{count} dictionary files installed")
PY
ls "$APPDIR/usr/share/hunspell"
echo "::endgroup::"

# --------------------------------------------------
# Qt plugins the plugin-qt module detection does not cover on its own
# --------------------------------------------------
qt_plugins="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
export EXTRA_PLATFORM_PLUGINS=""
if [[ -f "$qt_plugins/platforms/libqwayland.so" ]]; then
    EXTRA_PLATFORM_PLUGINS="libqwayland.so"
elif [[ -f "$qt_plugins/platforms/libqwayland-generic.so" ]]; then
    EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so"
fi
export QMAKE
# Libraries that corvus (and qrencode) need, so ldd inside linuxdeploy finds them.
export LD_LIBRARY_PATH="${EXTRA_LIB_PATH:+$EXTRA_LIB_PATH:}$("$QMAKE" -query QT_INSTALL_LIBS)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# --------------------------------------------------
# linuxdeploy: bundle libraries; qrencode comes with its own libraries
# --------------------------------------------------
echo "::group::linuxdeploy"
qrencode_bin="$(command -v qrencode)"
cp "$repo_root/packaging/appimage/AppRun" "$tools_dir/AppRun"
chmod +x "$tools_dir/AppRun"

PATH="$tools_dir:$PATH" "$tools_dir/linuxdeploy-x86_64.AppImage" \
    --appdir "$APPDIR" \
    --plugin qt \
    --executable "$APPDIR/usr/bin/corvus" \
    --executable "$qrencode_bin" \
    --desktop-file "$APPDIR/usr/share/applications/corvus.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/corvus.svg" \
    --custom-apprun "$tools_dir/AppRun"
echo "::endgroup::"

# TLS (HTTPS for GIPHY, update check, ...) and, if Qt has them, the Wayland plugins that
# the Qt plugin of linuxdeploy does not always pick up. Existing files are kept.
mkdir -p "$APPDIR/usr/plugins"
for dir in tls networkinformation wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration; do
    if [[ -d "$qt_plugins/$dir" && ! -d "$APPDIR/usr/plugins/$dir" ]]; then
        cp -a "$qt_plugins/$dir" "$APPDIR/usr/plugins/$dir"
    fi
done
# The desktop portal platform theme (file chooser, dark mode) has no dependencies besides Qt.
if [[ -f "$qt_plugins/platformthemes/libqxdgdesktopportal.so" ]]; then
    mkdir -p "$APPDIR/usr/plugins/platformthemes"
    cp -n "$qt_plugins/platformthemes/libqxdgdesktopportal.so" "$APPDIR/usr/plugins/platformthemes/"
fi
# No GTK3 platform theme: it would pull GTK and GLib into the bundle. Corvus draws its own
# appearance, so empty the theme hook of the Qt plugin as well.
rm -f "$APPDIR/usr/plugins/platformthemes/libqgtk3.so"
if [[ -f "$APPDIR/apprun-hooks/linuxdeploy-plugin-qt-hook.sh" ]]; then
    # The generated AppRun sources this file, so it must stay.
    echo '# GTK3 theme hook removed (see build-appimage.sh)' > "$APPDIR/apprun-hooks/linuxdeploy-plugin-qt-hook.sh"
fi

# --------------------------------------------------
# Check the bundle before packing it
# --------------------------------------------------
echo "::group::Bundle check"
missing=0
for f in usr/bin/corvus usr/bin/corvus-cli usr/bin/qrencode AppRun \
         usr/share/hunspell/de_DE.dic usr/share/hunspell/en_US.dic; do
    if [[ ! -e "$APPDIR/$f" ]]; then echo "::error::Missing in AppDir: $f"; missing=1; fi
done
if ! ls "$APPDIR"/usr/lib/libhunspell-*.so* >/dev/null 2>&1; then
    echo "::error::libhunspell is not bundled"; missing=1
fi
if ! ls "$APPDIR"/usr/lib/libqrencode.so* >/dev/null 2>&1; then
    echo "::error::libqrencode is not bundled"; missing=1
fi
if ! ls "$APPDIR"/usr/lib/libqt6keychain.so* >/dev/null 2>&1; then
    echo "::error::QtKeychain is not bundled"; missing=1
fi
if ! ls "$APPDIR"/usr/plugins/platforms/libqxcb.so >/dev/null 2>&1; then
    echo "::error::Qt xcb platform plugin is not bundled"; missing=1
fi
# Qt Multimedia (audio/video) needs its backend plugin.
if ls "$APPDIR"/usr/lib/libQt6Multimedia.so* >/dev/null 2>&1 \
   && ! ls "$APPDIR"/usr/plugins/multimedia/*.so >/dev/null 2>&1; then
    echo "::error::Qt Multimedia is bundled, but its backend plugin (usr/plugins/multimedia) is not"; missing=1
fi
[[ $missing -eq 0 ]] || exit 1

# No unresolved libraries for the programs inside the bundle (other than the system's).
for exe in corvus qrencode; do
    echo "--- ldd $exe"
    unresolved="$(LD_LIBRARY_PATH="$APPDIR/usr/lib" ldd "$APPDIR/usr/bin/$exe" | grep 'not found' || true)"
    if [[ -n "$unresolved" ]]; then echo "$unresolved"; echo "::error::Unresolved libraries for $exe"; exit 1; fi
done
echo "::endgroup::"

# --------------------------------------------------
# Pack
# --------------------------------------------------
echo "::group::appimagetool"
# appimagetool writes the .zsync file (needs zsyncmake) into the current directory.
output_dir="$(cd "$(dirname "$OUTPUT")" && pwd)"
output_name="$(basename "$OUTPUT")"
command -v zsyncmake >/dev/null || { echo "::error::zsyncmake is missing (package: zsync)"; exit 1; }
(
    cd "$output_dir"
    "$tools_dir/appimagetool-x86_64.AppImage" \
        --runtime-file "$tools_dir/runtime-x86_64" \
        --updateinformation "$UPDATE_INFORMATION" \
        "$APPDIR" "$output_name"
)
chmod +x "$OUTPUT"
echo "::endgroup::"

# The update information and the matching zsync file must both exist.
# (Without APPIMAGE_EXTRACT_AND_RUN, which would start the app instead of answering.)
embedded="$(env -u APPIMAGE_EXTRACT_AND_RUN "$OUTPUT" --appimage-updateinformation)"
echo "Embedded update information: $embedded"
[[ "$embedded" == "$UPDATE_INFORMATION" ]] || { echo "::error::Update information not embedded"; exit 1; }
test -s "$OUTPUT.zsync" || { echo "::error::$OUTPUT.zsync was not created"; exit 1; }
ls -lh "$OUTPUT" "$OUTPUT.zsync"
