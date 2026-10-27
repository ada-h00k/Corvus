#!/usr/bin/env python3
"""Minimal stand-in for signal-cli used by the GUI integration test.

Speaks the subset of the real CLI / JSON-RPC protocol the GUI uses, with the
JSON field names taken from signal-cli's source (JsonContact, JsonDataMessage,
JsonAttachment, JsonQuote, JsonReaction, ...).

Environment:
  MOCK_LOG     file: every JSON-RPC request is appended (one JSON per line)
  MOCK_INJECT  file: JSON lines appended here are emitted as `receive` envelopes
  MOCK_IDENTITY_FILE  file: JSON list replacing the identities of listIdentities (key changes)
  MOCK_EXTRA_GROUPS   set: also lists a group where we are no admin (permissions only for admins)
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
    {"number": "+49444", "uuid": "u-dave", "name": "Dave", "isArchived": False, "isBlocked": False, "isHidden": True, "messageExpirationTime": 0},
    {"number": "+49555", "uuid": "u-erin", "name": "Erin", "isArchived": False, "isBlocked": False, "isHidden": False, "messageExpirationTime": 0},
]
IDENTITIES = [
    {"number": "+49111", "uuid": "u-alice", "fingerprint": "0568" + "ab" * 32,
     "safetyNumber": "12345" "67890" "11111" "22222" "33333" "44444" "55555" "66666" "77777" "88888" "99999" "00000",
     "scannableSafetyNumber": base64.b64encode(bytes([8, 1, 10, 0, 13, 255, 26] + list(range(100))) ).decode(), "trustLevel": "TRUSTED_UNVERIFIED", "addedTimestamp": 1700000000000},
]


def reload_identities():
    """MOCK_IDENTITY_FILE: a JSON list that replaces the identities (the test changes keys with it)."""
    global IDENTITIES
    path = os.environ.get("MOCK_IDENTITY_FILE")
    if path and os.path.exists(path):
        with open(path) as f:
            IDENTITIES = json.load(f)


GROUPS = [{"id": "R1JPVVA=", "name": "Team", "isMember": True, "isBlocked": False, "messageExpirationTime": 0}]
current_username = "oldname.1234"
own_given, own_family = "Old", "Name"
account_index = args.index("-a") + 1 if "-a" in args else -1
self_account = args[account_index] if account_index > 0 else ""
sent = 0
log = os.environ.get("MOCK_LOG")


# ---- groups (JsonGroup of signal-cli: members with isAdmin, pending / requesting / banned addresses) ----
def addr(who):
    if who in (self_account, "self-uuid"):
        return {"number": self_account, "uuid": "self-uuid"}
    for c in CONTACTS:
        if who in (c["number"], c["uuid"]):
            return {"number": c["number"], "uuid": c["uuid"]}
    if who.startswith("+"):
        return {"number": who, "uuid": "u-" + who[1:]}
    return {"number": None, "uuid": who}


def same(a, b):
    return (a.get("number") and a.get("number") == b.get("number")) or (a.get("uuid") and a.get("uuid") == b.get("uuid"))


def group_template(gid, name):
    return {"id": gid, "name": name, "description": "", "isMember": True, "isBlocked": False, "isTerminated": False,
            "messageExpirationTime": 0, "members": [], "pendingMembers": [], "requestingMembers": [], "admins": [],
            "banned": [], "permissionAddMember": "EVERY_MEMBER", "permissionEditDetails": "EVERY_MEMBER",
            "permissionSendMessage": "EVERY_MEMBER", "groupInviteLink": None}


def member(who, admin=False):
    a = addr(who)
    a["isAdmin"] = admin
    return a


team = GROUPS[0]
team.update(group_template(team["id"], "Team"))
team["members"] = [member(self_account, True), member("+49111"), member("+49222", True)]
team["requestingMembers"] = [addr("+49333")]
team["pendingMembers"] = [addr("+49444")]
if os.environ.get("MOCK_EXTRA_GROUPS"):  # a group where we are a plain member and only admins may change things
    club = group_template("Q0xVQg==", "Club")
    club["members"] = [member("+49111", True), member(self_account), member("+49222")]
    club["permissionAddMember"] = "ONLY_ADMINS"
    club["permissionEditDetails"] = "ONLY_ADMINS"
    GROUPS.append(club)
    for gid, title in (("Qk9PSw==", "Book club"), ("SElLRQ==", "Hiking")):  # invitations: we are pending, not members
        invite = group_template(gid, title)
        invite["isMember"] = False
        invite["members"] = [member("+49111", True), member("+49222")]
        invite["pendingMembers"] = [addr(self_account)]
        GROUPS.append(invite)

ADMIN_ONLY = ("removeMember", "admin", "removeAdmin", "ban", "unban", "link", "resetLink", "setPermissionAddMember",
              "setPermissionEditDetails", "setPermissionSendMessages")
PERMISSION = {"every-member": "EVERY_MEMBER", "only-admins": "ONLY_ADMINS", "everyMember": "EVERY_MEMBER",
              "onlyAdmins": "ONLY_ADMINS"}


def listed(g):
    out = dict(g)
    out["admins"] = [{"number": x["number"], "uuid": x["uuid"]} for x in g["members"] if x["isAdmin"]]
    return out


def group_error(rid, message):
    emit({"jsonrpc": "2.0", "id": rid, "error": {"code": -1, "message": message}})


def update_group(p):
    """Returns (result, error message)."""
    gid = p.get("groupId")
    if gid is None:  # create
        new_id = base64.b64encode(("NEW-%d" % len(GROUPS)).encode()).decode()
        g = group_template(new_id, p.get("name", ""))
        g["description"] = p.get("description", "")
        g["members"] = [member(self_account, True)] + [member(x) for x in p.get("member", [])]
        GROUPS.append(g)
        return {"groupId": new_id, "timestamp": int(time.time() * 1000), "results": []}, None
    g = next((x for x in GROUPS if x["id"] == gid), None)
    if g is None:
        return None, "Group not found"
    me = next((x for x in g["members"] if same(x, addr(self_account))), None)
    if me is None and any(same(x, addr(self_account)) for x in g["pendingMembers"]):
        # like signal-cli: updating a group we are invited to accepts the invitation
        g["pendingMembers"] = [x for x in g["pendingMembers"] if not same(x, addr(self_account))]
        g["members"].append(member(self_account))
        g["isMember"] = True
        return {"timestamp": int(time.time() * 1000), "results": []}, None
    if me is None:
        return None, "You are not a member of this group"
    for key in ADMIN_ONLY:
        if key in p and not me["isAdmin"]:
            return None, "Only admins can do that (%s)" % key
    if "member" in p and g["permissionAddMember"] == "ONLY_ADMINS" and not me["isAdmin"]:
        return None, "Only admins can add members"
    if any(k in p for k in ("name", "description", "avatar")) and g["permissionEditDetails"] == "ONLY_ADMINS" and not me["isAdmin"]:
        return None, "Only admins can edit the group details"
    if "name" in p:
        g["name"] = p["name"]
    if "description" in p:
        g["description"] = p["description"]
    for who in p.get("member", []):
        a = addr(who)
        req = [x for x in g["requestingMembers"] if same(x, a)]
        if req:  # approve a join request
            g["requestingMembers"].remove(req[0])
        if not any(same(x, a) for x in g["members"]):
            g["members"].append(member(who))
    for who in p.get("removeMember", []):
        a = addr(who)
        g["members"] = [x for x in g["members"] if not same(x, a)]
        g["pendingMembers"] = [x for x in g["pendingMembers"] if not same(x, a)]
        g["requestingMembers"] = [x for x in g["requestingMembers"] if not same(x, a)]
    for key, flag in (("admin", True), ("removeAdmin", False)):
        for who in p.get(key, []):
            for x in g["members"]:
                if same(x, addr(who)):
                    x["isAdmin"] = flag
    for who in p.get("ban", []):
        a = addr(who)
        g["members"] = [x for x in g["members"] if not same(x, a)]
        if not any(same(x, a) for x in g["banned"]):
            g["banned"].append(a)
    for who in p.get("unban", []):
        g["banned"] = [x for x in g["banned"] if not same(x, addr(who))]
    if "link" in p:
        g["groupInviteLink"] = None if p["link"] == "disabled" else "https://signal.group/#MOCK-LINK"
    if p.get("resetLink") and g["groupInviteLink"]:
        g["groupInviteLink"] = "https://signal.group/#MOCK-LINK-2"
    for key, field in (("setPermissionAddMember", "permissionAddMember"),
                       ("setPermissionEditDetails", "permissionEditDetails"),
                       ("setPermissionSendMessages", "permissionSendMessage")):
        if key in p:
            g[field] = PERMISSION.get(p[key], "EVERY_MEMBER")
    if "expiration" in p:
        g["messageExpirationTime"] = p["expiration"]
    return {"timestamp": int(time.time() * 1000), "results": []}, None

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
        wanted = p.get("groupId")
        res = [listed(g) for g in GROUPS if not wanted or g["id"] in wanted]
    elif m == "listIdentities":
        reload_identities()
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
        if os.environ.get("MOCK_IDENTITY_FILE"):  # keep the change when the file is read again
            with open(os.environ["MOCK_IDENTITY_FILE"], "w") as f:
                json.dump(IDENTITIES, f)
        res = {}
    elif m == "updateContact":
        for contact in CONTACTS:
            if p.get("recipient") in (contact["number"], contact["uuid"]):
                contact["messageExpirationTime"] = p.get("expiration", 0)
        res = {}
    elif m in ("block", "unblock"):
        targets = p.get("recipient", [])
        if isinstance(targets, str):
            targets = [targets]
        for contact in CONTACTS:
            if contact["number"] in targets or contact["uuid"] in targets:
                contact["isBlocked"] = (m == "block")
        for g in GROUPS:
            if any(gid in targets for gid in (g.get("id"),)):
                g["isBlocked"] = (m == "block")
        res = {}
    elif m == "removeContact":
        target = next((c for c in CONTACTS if p.get("recipient") in (c["number"], c["uuid"])), None)
        if target:
            if p.get("forget"):
                CONTACTS.remove(target)
            elif p.get("hide"):
                target["isHidden"] = True
            else:  # plain removeContact: clear the contact details (and the hidden state)
                target["isHidden"] = False
        res = {}
    elif m == "updateGroup":
        res, failure = update_group(p)
        if failure:
            group_error(rid, failure)
            continue
    elif m == "quitGroup":
        g = next((x for x in GROUPS if x["id"] == p.get("groupId")), None)
        me = next((x for x in g["members"] if same(x, addr(self_account))), None) if g else None
        if g and me is None and any(same(x, addr(self_account)) for x in g["pendingMembers"]):
            # like signal-cli: quitting a group we are invited to declines the invitation
            g["pendingMembers"] = [x for x in g["pendingMembers"] if not same(x, addr(self_account))]
            res = {"timestamp": int(time.time() * 1000), "results": []}
            emit({"jsonrpc": "2.0", "id": rid, "result": res})
            continue
        if me is None:
            group_error(rid, "Group not found or not a member")
            continue
        others = [x for x in g["members"] if x is not me]
        if me["isAdmin"] and not any(x["isAdmin"] for x in others) and others and not p.get("admin"):
            group_error(rid, "You need to specify a new admin with --admin: only admin")
            continue
        for who in p.get("admin", []):
            for x in g["members"]:
                if same(x, addr(who)):
                    x["isAdmin"] = True
        g["members"] = others
        g["isMember"] = False
        res = {"timestamp": int(time.time() * 1000), "results": []}
    elif m == "joinGroup":
        uri = p.get("uri", "")
        if not uri.startswith("https://signal.group/#"):
            group_error(rid, "Group link is invalid: not a group link")
            continue
        new_id = base64.b64encode(("JOIN-%d" % len(GROUPS)).encode()).decode()
        g = group_template(new_id, "Joined group")
        requested = uri.endswith("APPROVAL")
        g["members"] = [member("+49111", True)] + ([] if requested else [member(self_account)])
        g["isMember"] = not requested
        if requested:
            g["requestingMembers"] = [addr(self_account)]
        GROUPS.append(g)
        res = {"timestamp": int(time.time() * 1000), "results": [], "groupId": new_id}
        if requested:
            res["onlyRequested"] = True
    elif m == "remoteDelete":
        res = {"timestamp": int(time.time() * 1000), "results": []}
    elif m == "send":
        reload_identities()
        blocked = [i for i in IDENTITIES if i["trustLevel"] == "UNTRUSTED" and i["number"] in p.get("recipient", [])]
        if blocked:  # like signal-cli: no sending to a key that has not been accepted
            emit({"jsonrpc": "2.0", "id": rid, "error": {"code": -1,
                  "message": "Failed to send message: Untrusted Identity for \"%s\"" % blocked[0]["number"]}})
            continue
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
