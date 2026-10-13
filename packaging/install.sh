#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
binary_source="$script_dir/signal-cli-qt-linux-x86_64"
desktop_source="$script_dir/signal-cli-qt.desktop"
icon_source="$script_dir/signal-cli-qt.svg"

for source in "$binary_source" "$desktop_source" "$icon_source"; do
    if [[ ! -f "$source" ]]; then
        printf 'Required release file is missing: %s\n' "$source" >&2
        exit 1
    fi
done

app_dir="$HOME/.local/opt/signal-cli-qt"
bin_dir="$HOME/.local/bin"
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
applications_dir="$data_home/applications"
icons_dir="$data_home/icons/hicolor/scalable/apps"

mkdir -p "$app_dir" "$bin_dir" "$applications_dir" "$icons_dir"
install -m 755 "$binary_source" "$app_dir/signal-cli-qt"
ln -sfn "$app_dir/signal-cli-qt" "$bin_dir/signal-cli-qt"
install -m 644 "$icon_source" "$icons_dir/signal-cli-qt.svg"

desktop_exec="$app_dir/signal-cli-qt"
desktop_exec=${desktop_exec//\\/\\\\}
desktop_exec=${desktop_exec//\"/\\\"}
desktop_exec=${desktop_exec//%/%%}
awk -v executable="$desktop_exec" '
    /^Exec=/ { print "Exec=\"" executable "\""; next }
    { print }
' "$desktop_source" > "$applications_dir/signal-cli-qt.desktop"
chmod 644 "$applications_dir/signal-cli-qt.desktop"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$applications_dir" >/dev/null 2>&1 || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$data_home/icons/hicolor" >/dev/null 2>&1 || true
fi

printf 'Installed signal-cli Qt for %s\n' "$USER"
printf 'Application menu entry: %s\n' "$applications_dir/signal-cli-qt.desktop"
printf 'Command-line link: %s\n' "$bin_dir/signal-cli-qt"
printf 'Install signal-cli and ensure its executable is on PATH before launching.\n'