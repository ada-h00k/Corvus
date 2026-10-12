#!/usr/bin/env python3
"""Minimal stand-in for signal-cli used by the GUI integration test.

Speaks the subset of the real CLI / JSON-RPC protocol the GUI uses, with the
JSON field names taken from signal-cli's source (JsonContact, JsonDataMessage,
JsonAttachment, JsonQuote, JsonReaction, ...).

Environment:
  MOCK_LOG     file: every JSON-RPC request is appended (one JSON per line)
  MOCK_INJECT  file: JSON lines appended here are emitted as `receive` envelopes
"""
import base64, json, os, struct, sys, threading, time, zlib

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


nap = float(os.environ.get("MOCK_SLEEP", "0"))      # simulate a slow JVM start
mode = os.environ.get("MOCK_ACCOUNTS", "ok")        # ok | empty | fail | okexit1

if "--version" in args:
    time.sleep(nap)
    print("signal-cli 0.13.99")
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
GROUPS = [{"id": "R1JPVVA=", "name": "Team", "isMember": True, "isBlocked": False, "messageExpirationTime": 0}]
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
        res = CONTACTS
    elif m == "listGroups":
        res = GROUPS
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
        res = {"data": base64.b64encode(png()).decode()}
    else:
        emit({"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "unknown method " + str(m)}})
        continue
    emit({"jsonrpc": "2.0", "id": rid, "result": res})
