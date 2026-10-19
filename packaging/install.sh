#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

app_dir="$HOME/.local/opt/corvus"
bin_dir="$HOME/.local/bin"
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
applications_dir="$data_home/applications"
icons_dir="$data_home/icons/hicolor/scalable/apps"

desktop_file="$applications_dir/corvus.desktop"
icon_file="$icons_dir/corvus.svg"
binary_file="$app_dir/corvus"
bin_link="$bin_dir/corvus"

# ------------------------------------------------------------
# Former name (Wren)
# ------------------------------------------------------------

# Removes an installation made under the former name "Wren". Chat history, settings and
# keychain entries are not touched; Corvus migrates them on first start.
remove_former_install() {
    local old_app_dir="$HOME/.local/opt/wren"
    local old_link="$bin_dir/wren"

    # Only remove the link if it points into the old installation directory.
    if [[ -L "$old_link" && "$(readlink -- "$old_link")" == "$old_app_dir/"* ]]; then
        rm -f -- "$old_link"
    fi
    rm -f -- "$applications_dir/wren.desktop"
    rm -f -- "$icons_dir/wren.svg"
    rm -f -- "$old_app_dir/wren"
    rmdir -- "$old_app_dir" 2>/dev/null || true
}

# ------------------------------------------------------------
# Uninstall
# ------------------------------------------------------------

if [[ "${1:-}" == "--uninstall" ]]; then
    printf 'Uninstalling Corvus...\n'

    rm -f -- "$bin_link"
    rm -f -- "$desktop_file"
    rm -f -- "$icon_file"
    rm -f -- "$binary_file"
    remove_former_install

    # Remove installation directory if it is now empty.
    rmdir -- "$app_dir" 2>/dev/null || true

    # Remove empty parent directories that were created by us.
    rmdir -- "$icons_dir" 2>/dev/null || true
    rmdir -- "$(dirname "$icons_dir")" 2>/dev/null || true
    rmdir -- "$applications_dir" 2>/dev/null || true

    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$applications_dir" >/dev/null 2>&1 || true
    fi

    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -t "$data_home/icons/hicolor" >/dev/null 2>&1 || true
    fi

    printf 'Corvus has been uninstalled.\n'
    exit 0
fi

# Reject unknown arguments.
if [[ $# -gt 0 ]]; then
    printf 'Usage: %s [--uninstall]\n' "${BASH_SOURCE[0]}" >&2
    exit 1
fi

# ------------------------------------------------------------
# Install
# ------------------------------------------------------------

binary_source="$script_dir/corvus-linux-x86_64"
desktop_source="$script_dir/corvus.desktop"
icon_source="$script_dir/corvus.svg"

for source in "$binary_source" "$desktop_source" "$icon_source"; do
    if [[ ! -f "$source" ]]; then
        printf 'Required release file is missing: %s\n' "$source" >&2
        exit 1
    fi
done

mkdir -p "$app_dir" "$bin_dir" "$applications_dir" "$icons_dir"

remove_former_install

install -m 755 "$binary_source" "$binary_file"
ln -sfn "$binary_file" "$bin_link"
install -m 644 "$icon_source" "$icon_file"

desktop_exec="$binary_file"
desktop_exec=${desktop_exec//\\/\\\\}
desktop_exec=${desktop_exec//\"/\\\"}
desktop_exec=${desktop_exec//%/%%}

awk -v executable="$desktop_exec" '
    /^Exec=/ { print "Exec=\"" executable "\""; next }
    { print }
' "$desktop_source" > "$desktop_file"

chmod 644 "$desktop_file"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$applications_dir" >/dev/null 2>&1 || true
fi

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$data_home/icons/hicolor" >/dev/null 2>&1 || true
fi

printf 'Installed Corvus for %s\n' "$USER"
printf 'Application menu entry: %s\n' "$desktop_file"
printf 'Command-line link: %s\n' "$bin_link"
printf 'Install signal-cli and ensure its executable is on PATH before launching.\n'
