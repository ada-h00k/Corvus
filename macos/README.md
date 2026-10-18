# Wren

This directory contains a native macOS version of Wren, written in Swift / SwiftUI (macOS 14 or newer). It has the
same features and talks to signal-cli the same way. It links the same Rust history store (`../history-store`), so the
`history.wren` format, encryption and keychain entries are identical to the Qt app.

| Part | Job |
|------|-----|
| `Sources/WrenCore` | UI-free logic: signal-cli process / JSON-RPC, envelope parsing, history records, chat model, keychain |
| `Sources/WrenMac` | SwiftUI app: window, chat view, composer, settings, notifications, menu bar icon |
| `Sources/CHistoryStore` | module map for `history-store/include/history_store.h` |

Requirements: Xcode (its toolchain provides the SwiftUI macros; the Command Line Tools alone are not
enough), Rust with cargo, and signal-cli (`brew install signal-cli`).

```sh
scripts/build-app.sh            # Rust store + Swift release build -> build/Wren.app
open "build/Wren.app"
swift test                      # unit tests (envelopes, records, Rust store round trip, mock signal-cli)
```

If `xcode-select` points to the Command Line Tools, the script uses `/Applications/Xcode.app`
automatically; for `swift build`/`swift test` set `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`.

Differences from the Qt app:

- The tray icon is a menu bar icon with the unread count, and the Dock icon shows a badge.
- Notifications use the macOS notification centre. One notification per chat; clicking it opens the chat.
- Settings live in the standard Settings window (⌘,).
- The QR code for linking is drawn by the app itself, so `qrencode` is not needed.
- The composer is multi-line: Return sends, Shift+Return or Option+Return starts a new line.
- The history is stored in `~/Library/Application Support/wren/wren/`, the same
  location a Qt build on macOS uses.

The release workflow (`.github/workflows/release.yml`) builds and tests the app and attaches
`wren-macos-arm64.zip` (Apple silicon) to every release. It is signed ad hoc only, so on
first launch open it with right-click → *Open*.

Debug builds (`swift build`) accept `SIGNAL_CLI_MAC_TEST_PASSPHRASE` and `SIGNAL_CLI_MAC_DEBUG_COMMANDS`
for unattended runs against `tests/mock-signal-cli.py` (see `Sources/WrenMac/DebugHooks.swift`).
