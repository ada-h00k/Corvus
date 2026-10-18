# Wren

| Light (Breeze Light) | Dark (Breeze Dark) |
| --- | --- |
| ![Hell](docs/screenshots/wren_BreezeLight.png) | ![Dunkel](docs/screenshots/wren_BreezeDark.png) |

A native Qt 6 GUI for [signal-cli](https://github.com/AsamK/signal-cli) with
an **encrypted local chat history**.

## Architecture

| Part | Language | Job |
|------|----------|-----|
| `src/` – GUI | C++ / Qt 6 Widgets | Windows, chat view, talking to signal-cli |
| `history-store/` – encrypted history | **Rust** | Everything that is new compared to signal-cli: key derivation, encryption, tamper detection, file format |
| signal-cli | (external, Java/native) | Installed by you, **not** part of this project |

### signal-cli stays independent

- signal-cli must be installed separately (package manager, release archive, …).
- The GUI only starts the `signal-cli` executable as a child process:
  one-shot commands (`--version`, `-o json listAccounts`, `link -n <name>`) and
  `signal-cli -a <number> jsonRpc` (JSON-RPC over stdin/stdout).
- No signal-cli code or library is linked, so updating signal-cli never
  requires rebuilding the GUI. After an update choose `File → Reconnect`.
- `Help → Check for signal-cli update` compares your installed version with the
  latest GitHub release (a silent check also runs at startup and only shows a
  hint in the status bar). You perform the update yourself.
- The path is looked up in `PATH` and can be changed under
  `Settings → Path to signal-cli …`.

### Encrypted chat history (Rust)

signal-cli does not keep a chat history, so the GUI stores one itself in
`history.wren` (in the application data directory, see *About*). Design:

- A random 256-bit **data key** encrypts every message with
  **XChaCha20-Poly1305** (random 192-bit nonce per record).
- The data key is wrapped using **Argon2id** (64 MiB, 3 passes). By default, the
  secret is random and kept in the system keychain; alternatively, you can use a
  passphrase. Changing the protection only re-wraps the key.
- Every record is authenticated together with its sequence number: modified,
  swapped or removed (non-final) records are detected and refused.
- Writes are append-only and `fsync`ed; compaction (deleting a conversation)
  and protection changes use atomic temp-file + rename. A torn last record
  after a crash is dropped automatically.
- The file is created with mode `0600`; keys are zeroized when dropped
  (best effort).
- Rust code is a static library with a small C ABI
  (`history-store/include/history_store.h`), covered by unit and FFI tests
  (`cargo test`).

With keychain protection, the history unlocks automatically while the system
keychain is available. With passphrase protection, you enter the passphrase at
every start. A passphrase cannot be recovered if forgotten; keychain-protected
history depends on retaining access to the system keychain. Use
`Settings → History protection …` to switch protection methods.

#### What is *not* protected

- signal-cli's own data directory (`~/.local/share/signal-cli`: account keys,
  sessions, contacts, attachments) is managed by signal-cli and is **not**
  encrypted by this program. Use full-disk/home-directory encryption for that.
- Decrypted messages and previews exist in RAM while the app runs; attachments
  you *open* are written to a temporary directory until the app exits.
- A screenshot pasted from the clipboard is saved as a temporary file until the
  app exits; afterwards only its preview (≤ 480 px) remains in the history.
- The file reveals the number and approximate size of messages (no padding),
  and an attacker with write access could delete the *last* records unnoticed.

## Upgrading from signal-cli Qt

The project was formerly called *signal-cli Qt*. On the first start, Wren moves the data of
the old version over automatically:

- the chat history `history.scq` becomes `history.wren` in the new data folder
  (`~/.local/share/wren/wren/` on Linux, `~/Library/Application Support/wren/wren/` on macOS),
- the settings are copied to the new location (macOS: new bundle ID `io.github.wren.mac`),
- the keychain entries `signal-cli-qt-history` and `signal-cli-qt` (GIPHY key) are copied to
  `wren-history` and `wren` the first time they are read, and the old entries are removed.

Close the old version before starting Wren; Wren refuses to migrate while the old program
still uses the history. Files that were not migrated are never deleted. The binary format of
the history file itself is unchanged.

## Features

- Choose an account / link this device via QR code (`signal-cli link`)
- Contacts and groups, conversation list with unread counters
- **Replies (quotes):** receive and display them; reply via the message
  context menu, click a quote to jump to the original
- **Attachments:** send files with the 📎 button, drag & drop, or by pasting:
  `Ctrl+V` / `Shift+Insert` (or right-click → *Attach from clipboard*) turns a
  **screenshot** in the clipboard, or files copied in a file manager, into an
  attachment with a small preview above the input field. If the clipboard also
  holds real text (e.g. copied from a document), the text is pasted instead.
  Receive and save attachments (context menu → *Save …*, or click to open).
  Files of risky types (`.exe`, `.sh`, …) are never opened directly, only saved
- **Inline images:** received and sent pictures are shown in the chat;
  click for the full size
- **Inline videos:** click a sent or received video to play it directly in the chat;
  pause or resume with the on-video control
- **Audio messages and files:** play received audio directly from its chat attachment;
  click again to pause or resume. The chat shows playback progress and duration.
- **Edit messages:** right-click one of your own text messages → *Edit*. The text moves into the
  input field (or press ↑ with an empty input; Shift+↑ selects the whole current draft).
  Enter saves, Esc or ✕ cancels and restores what you had typed. Same limits as the
  official clients: up to 3 hours after sending and at most 10 edits. Edited messages show
  *edited* next to the time. Edits by contacts and edits made on your phone are applied as well
  (only to a known message of the same author). The edit is stored in the encrypted history.
- **Disappearing messages:** right-click a chat to choose the Signal timer (off, 30 seconds,
  1 minute, 5 minutes, 1 hour, 6 hours, 12 hours, 1 day, 1 week or 4 weeks). The setting is
  synchronized with Signal for contacts and groups. New outgoing messages use that timer;
  incoming messages start their local timer when the chat is opened. Expired messages are
  removed from the encrypted history as well.
- **Delete messages:** right-click a message to delete it from this device. Your own messages
  can also be deleted for everyone within Signal's 24-hour window. Local deletion compacts the
  encrypted history so the removed message and its reactions are no longer stored.
- **Reactions:** react from the context menu (quick set or full picker);
  choose the same emoji again to take it back; incoming reactions are shown
  below the message
- **Emoji:** picker next to the input field
- **GIF search:** use the *GIF* button beside the composer to search GIPHY or browse trending
  GIFs, then select a result to send it in the current chat. Configure a GIPHY API key under
  `Settings → GIPHY API key …`; the picker includes GIPHY attribution.
- **Rounded message bubbles** like the official client: own messages on the
  right, received ones on the left, date separators, timestamps, clickable
  links, text selection within a message (double-click selects a word,
  `Ctrl+C` copies)
- **Appearance:** `Settings → Appearance …` lets you change the colour of your
  bubbles, received bubbles, their texts and the chat background, plus the
  corner radius, with a live preview. Untouched colours keep following a light
  or dark system theme. *Reset to defaults* restores them
- **Profile settings:** `Settings → Profile settings` lets you view/change your
  Signal username, set your display name, choose an avatar image, or remove the avatar
- **Profile settings:** `Settings → Profile settings …` opens a window of its own with your
  Signal username, display name and avatar (change / remove), like *Appearance*
- **Tray icon:** shows an unread counter (also in the window title and tooltip).
  Closing the window can keep the app running in the tray; click the icon to
  show/hide it, quit from the tray menu or *File → Quit*
- **Desktop notifications** and a **sound** for new messages, each with its own
  switch – you can have either, both or none (`Settings → Notifications & sound …`).
  Notifications can show name and message, name only, or nothing; clicking one opens
  the chat. A newer notification from the same chat replaces the old one
- **Sound:** three built-in sounds (Chime, Pop, Ding) or your own file, with a *Test* button
- Neither notification nor sound is triggered while you look at that conversation, nor for
  chats you **muted** (right-click a chat → *Mute notifications*; kept in the encrypted history)
- **Archived chats:** archived contacts are shown in an *Archived chats*
  folder (see below), like the official client
- Persistent, encrypted history; delete single conversations (right-click) –
  the file is compacted so the old ciphertext is gone
- Protect the history with the system keychain by default, or use a passphrase
  instead (`Settings → History protection …`)

### Archived chats

- Contacts you archived on your phone appear in the *Archived chats* folder
  at the top of the list (click it, *← Back to chats* to return). signal-cli
  reports this as `isArchived` in `listContacts`.
- A new message (sent or received) moves a chat out of the archive again,
  as in the official client.
- You can also archive/unarchive any chat locally (right-click). That state is
  kept in the encrypted history and is only overwritten when signal-cli
  reports a *change* (e.g. you unarchive on the phone).
- **Groups:** signal-cli does not report the archive state of groups, so
  they cannot be synchronised; archive them locally if you like.
- The state only arrives if your phone synced it to signal-cli; this is
  outside of the GUI's control.

### Attachments and the history

- Images keep a small preview (≤ 480 px) **inside the encrypted history**, so
  they are still displayed after a restart without signal-cli.
- Full-size files are fetched from signal-cli on demand (`getAttachment`).
  signal-cli keeps its own (unencrypted) copies in its data directory.
- *Open* writes a temporary copy that is removed when the app exits.

## Downloads

Every commit pushed to `main` or `master` is built by GitHub Actions and published
as a release tagged `build-<commit SHA>`:

| File | Description |
|------|-------------|
| `wren-linux-x86_64` | Linux x86_64 executable. Dynamically links to Qt; it does not bundle runtime libraries. |
| `wren-linux-x86_64-installer.tar.gz` | Install bundle containing the executable, `install.sh`, desktop launcher, and icon. |
| `wren-macos-arm64.zip` | Native macOS app for Apple silicon (macOS 14+), see [macos/README.md](macos/README.md). |
| `wren-windows-x64-setup.exe` | Windows x86_64 setup (English). Can download signal-cli (and Java) for you, see [Windows](#windows). |
| `wren-windows-x64-portable.zip` | Windows x86_64, no installation: unzip and run `wren.exe`. |
| `SHA256SUMS` | SHA-256 checksums for all files of the release. |

The Linux files are x86_64 only. Install Qt 6 runtime libraries (Widgets, Network, Multimedia
and MultimediaWidgets), QtKeychain 0.14 or newer, and a system credential store.
The installer does not install dependencies or `signal-cli`; install
`signal-cli` separately and make sure its executable is on `PATH`.

To install, download the standalone executable, installer archive, and
`SHA256SUMS`. Verify the downloads, extract the archive, and run the included
installer:

```sh
sha256sum -c SHA256SUMS
tar -xzf wren-linux-x86_64-installer.tar.gz
./install.sh
```

The installer adds the application to your desktop's application menu, installs
its icon, places the executable in `~/.local/opt/wren/`, and creates a
command link at `~/.local/bin/wren`. It makes no system-wide changes
and does not require `sudo`. Run it again after downloading a newer release to
update the installation.

### Windows

Windows 10 (1803) or newer, x86_64. Download `wren-windows-x64-setup.exe` from the latest release and run it.
The setup is in English, installs per user by default (no administrator rights needed; you can choose
"all users" in the dialog) and bundles the Qt runtime, so nothing else has to be installed.

On the **signal-cli** page of the setup you can choose to

- **download and install signal-cli** – the latest release of
  [AsamK/signal-cli](https://github.com/AsamK/signal-cli/releases) is fetched directly from GitHub
  (the JVM build `signal-cli-<version>.tar.gz`), its SHA-256 checksum (published by GitHub) is verified, and it
  is unpacked to `<install folder>\signal-cli`. Afterwards the setup starts `signal-cli --version` once to check it.
- **download a private Java runtime** – signal-cli needs Java 25 or newer. If no suitable Java is found
  (`JAVA_HOME` or `PATH`), the setup offers an Eclipse Temurin JRE (from Adoptium / github.com/adoptium,
  checksum verified), unpacked to `<install folder>\signal-cli\jre`. Wren uses it automatically for signal-cli;
  it does not change your system Java or `PATH`.

Wren finds `<install folder>\signal-cli\bin\signal-cli.bat` on its own. If you skip the download, install
signal-cli yourself and select it under `Settings → Path to signal-cli …`. To update signal-cli later,
run the setup again (it downloads the then-latest release) or use `Help → Check for signal-cli update`.

Silent install (the signal-cli page is skipped, downloads happen only if requested):

```
wren-windows-x64-setup.exe /VERYSILENT /InstallSignalCli=1 /InstallJava=1
```

Notes:

- The setup needs internet access only for the optional downloads (`github.com`, `api.github.com`,
  `api.adoptium.net`). It uses Windows' own `tar.exe` and PowerShell to unpack and to look up the release.
- The QR code for linking a device is rendered with a bundled `qrencode.exe`.
- The default history key and the GIPHY key are stored in the Windows Credential Manager (via QtKeychain).
- Uninstalling removes Wren and the signal-cli/Java copy created by the setup. Your Signal account data
  (`%USERPROFILE%\.local\share\signal-cli`) and Wren's encrypted chat history (`%APPDATA%\wren`) are kept.
- The setup file is not code-signed, so Windows SmartScreen may warn on first start.
- Notifications use the tray icon's messages and sounds are played through PowerShell
  (the freedesktop D-Bus service does not exist on Windows).

### Arch Linux

Every release also contains a ready-made package (`wren-<version>-1-x86_64.pkg.tar.zst`):

```
sudo pacman -U wren-*.pkg.tar.zst
```

It depends on `qt6-base`, `qt6-multimedia`, `qt6-svg` and `qtkeychain-qt6`. `signal-cli` is not in the official
repositories and must be installed separately (e.g. from the AUR). To build it yourself,
see the instructions at the top of `packaging/arch/PKGBUILD`.

## Build from source

The macOS app is built differently (Swift package + Xcode), see [macos/README.md](macos/README.md).

The Qt build on macOS produces a `wren.app` bundle with its own `Info.plist`
(`packaging/macos/Info.plist.in`). It sets `LSFileQuarantineEnabled`, so files the
app creates (received attachments, temporary copies made by *Open*) carry the
`com.apple.quarantine` flag and Gatekeeper checks them before they run. Check with
`xattr -l <file>` after opening an attachment.


Requirements: C++17 compiler, CMake ≥ 3.16, Qt 6 (Widgets, Network, Multimedia),
QtKeychain 0.14 or newer,
**Rust ≥ 1.75 with cargo** (e.g. via [rustup](https://rustup.rs) or your distro).

```sh
# Debian/Ubuntu
sudo apt install build-essential cmake qt6-base-dev qt6-multimedia-dev qt6-svg-dev qtkeychain-qt6-dev cargo qrencode

cmake -S . -B build
cmake --build build -j
./build/wren

# Rust tests only
cargo test --release --manifest-path history-store/Cargo.toml
```

`qrencode` is optional; it is needed to show the QR code when linking a device.
Without it only the link URI is shown. `Cargo.lock` is committed and the build
uses `--locked`; some dependencies are pinned to versions that still build on
older Rust toolchains.

The GIPHY API key and the default history key are stored in the operating system
credential store through QtKeychain. On Linux, a Secret Service provider such as
GNOME Keyring or KWallet must be available. Existing GIPHY keys in `QSettings`
are migrated when first read. Existing history files protected by a passphrase
remain passphrase-protected until you switch them using the File menu.

### Windows (MSVC)

Requirements: Visual Studio 2022 Build Tools (C++), CMake, Ninja, Qt 6 for MSVC (Widgets, Network, Multimedia),
[QtKeychain](https://github.com/frankosterfeld/qtkeychain) built with `-DBUILD_WITH_QT6=ON`, Rust (MSVC toolchain),
and optionally `qrencode.exe`. In a "x64 Native Tools" prompt:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:\Qt\6.x\msvc2022_64;C:\path\to\qtkeychain"
cmake --build build
mkdir stage & copy build\wren.exe stage
windeployqt --release --no-translations --dir stage stage\wren.exe
```

Copy `qt6keychain.dll` (and optionally `qrencode.exe`) next to `wren.exe`. To build the setup, install
[Inno Setup 6](https://jrsoftware.org/isinfo.php) and run
`iscc /DAppVersion=0.2.0 /DStageDir=<stage folder> packaging\windows\wren.iss`
(the result is `dist\wren-windows-x64-setup.exe`). `.github/workflows/release.yml` does all of this in the job `windows-app`.

## CI / release workflow

`.github/workflows/release.yml` runs on pushes to `main` or `master`, and can
also be started manually:

1. install Qt 6.12 and Rust, run `cargo test` for the history store
2. build with CMake (which invokes cargo)
3. package the standalone executable and an installer archive with its menu resources
4. publish both, with checksums, as a GitHub release tagged `build-<commit SHA>`

No secrets are needed (default `GITHUB_TOKEN` with `contents: write`).
One release per commit accumulates quickly – delete old ones from time to time.

### Notifications, sound and tray – how they work

- **Notifications** use the freedesktop.org Notifications service over D-Bus (what GNOME,
  KDE, … provide), independent of the tray icon. Where that service is missing, and on other
  platforms, the tray icon's own messages are used if the tray icon is shown.
- **Sound** is played by the desktop's command line player (`pw-play`, `paplay`,
  `canberra-gtk-play`, `aplay`, `play`; `afplay` on macOS) instead of Qt Multimedia, so
  there is no extra dependency and any format the player understands works. If none is
  found you get the system beep, and the settings dialog tells you.
- **Tray icon:** needs a system tray (StatusNotifier). GNOME has none by default (install
  the AppIndicator extension); then the setting is greyed out and closing the window simply
  quits.
- **Mute:** signal-cli does not report whether a chat is muted on your phone, so muting is
  local to this app.

## Troubleshooting

- **Every call to signal-cli is logged to the terminal** (command, result, duration;
  phone numbers are masked). Start the app from a terminal to see it, e.g.
  `signal-cli: signal-cli -o json listAccounts -> exit code 1 (1530 ms) | stderr: …`.
  Set `WREN_DEBUG=1` to see unmasked output.
- If signal-cli cannot be run or fails, the app says so with the command and the
  error message. It reports *"no account"* only if signal-cli itself returns an
  empty account list. The window never blocks while signal-cli starts.
- `qt.qpa.services: Failed to register with host portal … App info not found for
  'wren'` is harmless. It appears when a desktop application ID is announced
  but no matching `.desktop` file is installed; the app now announces its ID only if
  one exists.
## Tests

```sh
cargo test --release --manifest-path history-store/Cargo.toml   # Rust: crypto, file format, C ABI

cmake -S . -B build -DWREN_TESTS=ON && cmake --build build
ctest --test-dir build --output-on-failure                        # headless GUI test
```

The GUI test starts the real `MainWindow` (offscreen) against
`tests/mock-signal-cli.py`, a small stand-in for signal-cli that uses the JSON
field names of signal-cli's source. It covers first start / unlock, the
status bar (no phone number), archive folder, quotes, image thumbnails, attachments, reactions, sync messages,
persistence across a restart and that no message text is visible in the file.
`alerts_test` starts fake D-Bus services (notifications and a tray host) and a fake
`paplay`, and checks every combination of notification/sound on and off, content
settings, groups, attachments, mute, rate limiting, clicking a notification, the tray
(hide on close, unread counter, quit) and the settings dialog. It needs
`dbus-run-session` and, for the tray part, `xvfb-run`.
`startup_test` runs the real window against a mock that fails, returns an empty
list, is slow, or exits non-zero after printing valid data, and checks that the app
shows the real error, never blocks, and still connects when the output is valid.
A further test (`chatview_test`) renders the bubble view offscreen and checks
rounded corners, the configurable colours, layout, clicks on links /
attachments / quotes, text selection, scrolling and saving of the appearance
settings. It does **not** replace testing against a real signal-cli account.

## Known limitations

- No voice notes, stickers, link previews or mentions yet.
- Editing: only the text; the previous versions are not kept (no edit history view), and
  multi-line messages (sent from other devices) cannot be edited here because the input is
  single-line.
- Text can be selected within one message only (not across messages); use
  *Copy message text* in the context menu otherwise.
- Only one account is connected at a time; messages are single-line input.
- The whole history is decrypted into memory at startup (previews included);
  very large histories would need lazy loading.
- Unread counters are not persisted across restarts.
- Signal eventually rejects outdated clients, so keep signal-cli up to date –
  which is what the update check is for.
