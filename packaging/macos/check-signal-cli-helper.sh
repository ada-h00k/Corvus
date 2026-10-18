#!/bin/sh
# Checks a built macOS app: files it creates are quarantined (LSFileQuarantineEnabled), and signal-cli
# runs through the embedded helper app (src/SignalCliHelper.cpp), passing output and exit code through.
#   packaging/macos/check-signal-cli-helper.sh <path to .app>
set -eu

app=$1
quarantine=$(/usr/libexec/PlistBuddy -c 'Print :LSFileQuarantineEnabled' "$app/Contents/Info.plist" 2>/dev/null || true)
if [ "$quarantine" != "true" ]; then
    echo "error: LSFileQuarantineEnabled is not set in $app/Contents/Info.plist" >&2
    exit 1
fi

# The command prints its parent's parent: launchd (1) when the helper app ran it. The direct fallback
# would print the caller instead, and a warning on stderr.
helper="$app/Contents/Helpers/wren-signal-cli.app/Contents/MacOS/wren-signal-cli"
err=$(mktemp)
out=$("$helper" /bin/sh -c 'ps -o ppid= -p $PPID | tr -d " "; exit 3' 2>"$err") && rc=0 || rc=$?
if [ "$out" != 1 ] || [ "$rc" != 3 ] || [ -s "$err" ]; then
    echo "error: signal-cli helper failed (output '$out', exit code $rc, stderr '$(cat "$err")')" >&2
    exit 1
fi
rm -f "$err"
echo "signal-cli helper ok: $app"
