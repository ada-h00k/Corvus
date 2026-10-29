# corvus-signal

A Signal backend for Corvus written in Rust on [presage](https://github.com/whisperfish/presage),
in place of the Java signal-cli. It runs the same command lines Corvus gives signal-cli
(`--version`, `-o json listAccounts`, `link -n NAME`, `-a ACCOUNT jsonRpc`) and speaks
signal-cli's JSON-RPC on stdin/stdout, so Corvus works with either.

To use it, build Corvus with `-DCORVUS_RUST_BACKEND=ON` and choose the `corvus-signal`
program next to `corvus` in Settings → Advanced → Choose …. It has its own device link: Corvus
offers to link the computer with the phone again (the chat history in Corvus is kept).

## Building

Needs Rust 1.85+, `protoc`, and clang and cmake (libsignal's BoringSSL). CMake builds it with
the option above; by hand:

    cargo build --release

## presage and libsignal-service

corvus-signal needs work that is not in presage and libsignal-service-rs yet. It is built
against integration branches of both, branch `corvus` of
[cyberneticangel/presage](https://github.com/cyberneticangel/presage) and
[cyberneticangel/libsignal-service-rs](https://github.com/cyberneticangel/libsignal-service-rs)
(pinned by commit in Cargo.toml and presage's Cargo.toml).

To work on them, clone both forks next to this repository and add a `[patch]` section in a
local, uncommitted change:

    [patch."https://github.com/cyberneticangel/presage"]
    presage = { path = "../../presage/presage" }
    presage-store-sqlite = { path = "../../presage/presage-store-sqlite" }

They are upstream plus these open pull requests:

- libsignal-service-rs #473: create and change groups
- presage #478 (automatic link previews off), #485 (public push service), #444 (storage service
  from a linked device), #449 (only real identity changes), #448 (create and change groups)

and these additions, to be offered upstream:

- libsignal-service: group permission, announcements-only and ban changes; profile avatar upload
  (was `unimplemented!`); username reserve/confirm; storage service writes and raw record reads
- presage: the above in `GroupEdit`, withdrawing an invitation, `set_profile` (with avatar),
  `set_username`, sending to a group leaving out some members, a public `groups_manager`

## Behaviour that follows signal-cli

- Safety numbers: trust on first use; a changed key is untrusted until verified (`trust`).
  Nothing is sent to it; group messages leave such members out and report them as
  `IDENTITY_FAILURE`. Decisions are kept in `trust.json` in the account directory.
- Blocking, accepting message requests, and the username are written to the storage service
  (only the changed field of a record; fields this program doesn't know are kept), and the
  phone is told to read it again. Messages from blocked people and groups are ignored.
- Attachments, link preview images and stickers are downloaded when a message arrives;
  delivery receipts are sent for received messages.
- Link previews are only sent when Corvus asks for one (presage's automatic previews are off).

Data lives in `$XDG_DATA_HOME/corvus-signal` (or `$CORVUS_SIGNAL_DIR`), one directory per
account. `CORVUS_SIGNAL_LOG` sets the log filter (stderr); `-v`/`-vv` raise the level.

## Tests

    cargo test

`src/contract.rs` writes the JSON corvus-signal emits to `tests/data/corvus-signal-contract.json`
(`CORVUS_UPDATE_CONTRACT=1 cargo test`), and `tests/backend_contract_test.cpp` checks that
Corvus' parsers read it as intended.
