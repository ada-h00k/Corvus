#!/usr/bin/env python3
"""Minimal stand-in for signal-cli used by the GUI integration test.

Speaks the subset of the real CLI / JSON-RPC protocol the GUI uses, with the
JSON field names taken from signal-cli's source (JsonContact, JsonDataMessage,
JsonAttachment, JsonQuote, JsonReaction, ...).

Environment:
  MOCK_LOG     file: every JSON-RPC request is appended (one JSON per line)
  MOCK_INJECT  file: JSON lines appended here are emitted as `receive` envelopes
"""
import base64, io, json, os, struct, sys, threading, time, wave, zlib

args = sys.argv[1:]


def png(w=64, h=48):
    rows = b""
    for y in range(h):
        rows += b"\x00" + b"".join(bytes([(x * 4) % 256, (y * 5) % 256, 128]) for x in range(w))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def wav():
    data = io.BytesIO()
    with wave.open(data, "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(8000)
        audio.writeframes(b"\x00\x00" * 16000)
    return data.getvalue()


nap = float(os.environ.get("MOCK_SLEEP", "0"))      # simulate a slow JVM start
mode = os.environ.get("MOCK_ACCOUNTS", "ok")        # ok | empty | fail | okexit1

if "--version" in args:
    time.sleep(nap)
    print("signal-cli 0.14.9")
    sys.exit(0)

if "listAccounts" in args:
    time.sleep(nap)
    if mode == "fail":
        sys.stderr.write("boom: cannot read config\n")
        sys.exit(3)
    if mode == "empty":
        print("[]")
        sys.exit(0)
    print(json.dumps([{"number": "+491234567"}]))
    sys.exit(1 if mode == "okexit1" else 0)  # valid output but a non-zero exit code

if "jsonRpc" not in args:
    sys.stderr.write("mock: unsupported command %r\n" % (args,))
    sys.exit(2)

lock = threading.Lock()


def emit(obj):
    with lock:
        sys.stdout.write(json.dumps(obj) + "\n")
        sys.stdout.flush()


def injector(path):
    pos = 0
    while True:
        time.sleep(0.05)
        try:
            with open(path, "r") as f:
                f.seek(pos)
                data = f.read()
                pos = f.tell()
        except FileNotFoundError:
            continue
        for line in data.splitlines():
            if line.strip():
                emit({"jsonrpc": "2.0", "method": "receive", "params": {"envelope": json.loads(line)}})


inject = os.environ.get("MOCK_INJECT")
if inject:
    threading.Thread(target=injector, args=(inject,), daemon=True).start()

CONTACTS = [
    {"number": "+49111", "uuid": "u-alice", "name": "Alice", "isArchived": False, "isBlocked": False, "isHidden": False, "messageExpirationTime": 0},
    {"number": "+49222", "uuid": "u-bob", "name": "Bob", "isArchived": True, "isBlocked": False, "isHidden": False, "messageExpirationTime": 0},
    {"number": "+49333", "uuid": "u-carol", "name": "Carol", "isArchived": True, "isBlocked": False, "isHidden": False, "messageExpirationTime": 0},
]
IDENTITIES = [
    {"number": "+49111", "uuid": "u-alice", "fingerprint": "0568" + "ab" * 32,
     "safetyNumber": "12345" "67890" "11111" "22222" "33333" "44444" "55555" "66666" "77777" "88888" "99999" "00000",
     "scannableSafetyNumber": base64.b64encode(bytes([8, 1, 10, 0, 13, 255, 26] + list(range(100))) ).decode(), "trustLevel": "TRUSTED_UNVERIFIED", "addedTimestamp": 1700000000000},
]
GROUPS = [{"id": "R1JPVVA=", "name": "Team", "isMember": True, "isBlocked": False, "messageExpirationTime": 0}]
current_username = "oldname.1234"
own_given, own_family = "Old", "Name"
account_index = args.index("-a") + 1 if "-a" in args else -1
self_account = args[account_index] if account_index > 0 else ""
sent = 0
log = os.environ.get("MOCK_LOG")

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    req = json.loads(line)
    if log:
        with open(log, "a") as f:
            f.write(json.dumps(req) + "\n")
    m, p, rid = req.get("method"), req.get("params", {}), req.get("id")
    if m == "listContacts":
        if self_account in p.get("recipients", []):
            res = [{"number": self_account, "uuid": "self-uuid", "username": current_username,
                    "profile": {"givenName": own_given, "familyName": own_family}}]
        else:
            res = CONTACTS
    elif m == "getAvatar":
        # contact / group avatars exist, profile avatars do not (exercises the fallback path)
        if "profile" in p and p["profile"] != self_account:
            emit({"jsonrpc": "2.0", "id": rid, "error": {"code": -1, "message": "Could not find avatar"}})
            continue
        res = {"data": base64.b64encode(png(48, 48)).decode()}
    elif m == "updateAccount":
        current_username = p.get("username", current_username)
        if "." not in current_username:
            current_username += ".5678"
        res = {"username": current_username, "usernameLink": "https://signal.me/#eu/test-link"}
    elif m == "updateProfile":
        if "givenName" in p:
            own_given, own_family = p["givenName"], p.get("familyName", "")
        res = {}
    elif m == "listGroups":
        res = GROUPS
    elif m == "listIdentities":
        found = [i for i in IDENTITIES if p.get("number") in (None, i["number"], i["uuid"])]
        if p.get("number") and not found:
            emit({"jsonrpc": "2.0", "id": rid, "error": {"code": -1, "message": "Unknown recipient"}})
            continue
        res = found
    elif m == "trust":
        target = [i for i in IDENTITIES if p.get("recipient") in (i["number"], i["uuid"])]
        digits = "".join(c for c in p.get("verifiedSafetyNumber", "") if c.isdigit())
        if not target or digits != target[0]["safetyNumber"]:
            emit({"jsonrpc": "2.0", "id": rid, "error": {"code": -1, "message": "Failed to verify safety number"}})
            continue
        target[0]["trustLevel"] = "TRUSTED_VERIFIED"
        res = {}
    elif m == "updateContact":
        for contact in CONTACTS:
            if p.get("recipient") in (contact["number"], contact["uuid"]):
                contact["messageExpirationTime"] = p.get("expiration", 0)
        res = {}
    elif m == "updateGroup":
        for group in GROUPS:
            if p.get("groupId") == group["id"]:
                group["messageExpirationTime"] = p.get("expiration", 0)
        res = {}
    elif m == "remoteDelete":
        res = {"timestamp": int(time.time() * 1000), "results": []}
    elif m == "send":
        sent += 1
        res = {"timestamp": 1700000100000 + sent, "results": []}
        if "editTimestamp" in p:  # an edit gets a newer timestamp than the revision it edits
            res["timestamp"] = int(p["editTimestamp"]) + 1000
    elif m == "sendReaction":
        sent += 1
        res = {"timestamp": 1700000100000 + sent, "results": []}
    elif m == "getAttachment":
        if p.get("id") == "missing":
            emit({"jsonrpc": "2.0", "id": rid, "error": {"code": -1, "message": "Could not find attachment"}})
            continue
        if p.get("id") == "voice1":
            payload = wav()
        elif p.get("id") == "chatgif":
            payload = base64.b64decode(
                "R0lGODlhEAAQAPAAAP8AAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACwAAAAAEAAQAAACDoSP"
                "qcvtD6OctNqLsz4FACH5BAAKAAAALAAAAAAQABAAgAAA/wAAAAIOhI+py+0Po5y02ouzPgUAOw==")
        else:
            payload = png()
        res = {"data": base64.b64encode(payload).decode()}
    else:
        emit({"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "unknown method " + str(m)}})
        continue
    emit({"jsonrpc": "2.0", "id": rid, "result": res})
