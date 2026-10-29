# The corvus-signal branch

This branch adds **corvus-signal**, a Signal backend for Corvus written in Rust on
[presage](https://github.com/whisperfish/presage). It can be used instead of the Java
signal-cli. It accepts the same command lines and speaks the same JSON-RPC, so Corvus works with
either, and signal-cli stays the default.

Why: signal-cli runs on the JVM. Measured on my machine (corvus-signal's daemon running for my
account; signal-cli's daemon was not started, so only its one-shot commands were measured):

| | corvus-signal | signal-cli 0.14.9 (JVM) |
|---|---|---|
| Daemon while Corvus runs | about 48 MB RSS, 5 threads | not measured; more than the row below |
| `-o json listAccounts` | 12 MB, under 0.01 s | 223 MB, 1.5 s |
| `--version` | 12 MB, under 0.01 s | 152 MB, 0.3 s |

## Trying it

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCORVUS_RUST_BACKEND=ON
    cmake --build build --parallel
    ./build/corvus

Besides Corvus' usual dependencies this needs `protoc`, plus clang and cmake (libsignal builds
BoringSSL). The first build takes a few minutes.

In Corvus, open Settings → Advanced → Choose … and pick `build/corvus-signal`. Corvus then
finds no account and offers to link the computer. corvus-signal is a new linked device with its
own keys, so link it with the phone again. The chat history in Corvus is kept. When connected,
the status bar says "Connected · corvus-signal 0.1.0". To go back, choose signal-cli the same
way.

## What changed in Corvus

The Qt side barely changed (`git diff main -- src`):

- `SignalCli::isNative()`: the macOS helper app is only for signal-cli.
- The status bar shows the backend's own version, and the signal-cli update check is skipped
  for corvus-signal.
- `CORVUS_RUST_BACKEND` in CMakeLists.txt (off by default).

There are no new features and no new settings. The backend is chosen with the existing
"Choose …" button.

## What corvus-signal does

Everything Corvus asks of signal-cli:

- messages, attachments, quotes, edits, deletes, reactions, receipts, typing, view once, voice
  notes, link previews, stickers
- groups: create, edit, members, admins, permissions, invite links, join requests, bans,
  invitations, leaving
- safety numbers, block/unblock, message requests, profile name and avatar, username,
  disappearing messages

It follows signal-cli's behaviour where Corvus relies on it:

- Trust on first use; a changed safety number blocks sending until it is verified.
- Messages from blocked people are ignored.
- Delivery receipts are sent.
- Contacts' numbers come from the storage service, so existing chats stay matched.

Details are in [signal-backend/README.md](signal-backend/README.md).

presage and libsignal-service-rs don't have all of this yet. corvus-signal is built against forks
with the open upstream PRs it needs plus additions that should go upstream (branch `corvus`):

- [cyberneticangel/presage](https://github.com/cyberneticangel/presage)
- [cyberneticangel/libsignal-service-rs](https://github.com/cyberneticangel/libsignal-service-rs)

## How it was tested, and what wasn't

- `cargo test` in `signal-backend/`: 24 tests (JSON mapping, text styles, RPC framing, trust,
  storage record edits, sticker links).
- New `tests/backend_contract_test.cpp`: the JSON corvus-signal emits for each kind of message,
  and the group list, are read by Corvus' own parsers (`parseEnvelope`, `GroupInfo::parse`).
  14 of 14 pass.
- The existing tests still use the mock and are unchanged. `startup`, `alerts`, `receipt_typing`
  and `chatview` fail here, but they fail the same way on `main`. `gui_integration_test` doesn't
  compile on `main` either (MessageInput is a QTextEdit since 248f173).
- Linking and connecting work against Signal's servers. **The features have not been gone
  through one by one with real contacts yet.** That's the next step.

Worth a close look:

- **Group changes.** They come from presage #448 / libsignal-service-rs #473, which their author
  says have not been run against Signal's production servers.
- **Storage service writes** (block, accepting a message request, username). Only the one field
  of a record is changed, in its bytes, so fields the code doesn't know are kept. A bug here
  would still change data on the phone. See `signal-backend/src/storage.rs`.

Logs go to stderr. Set `CORVUS_SIGNAL_LOG=debug` for more. Data lives in
`~/.local/share/corvus-signal`.
