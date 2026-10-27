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


# A demo sticker pack covering the three sticker flavours the client must play:
# an ANIMATED APNG (Signal's real animated sticker format — bouncing ball, 8
# frames), an ANIMATED WebP (the QMovie path) and a static single-frame one, so
# the sticker pipeline (picker previews and chat playback) can be exercised
# without a phone. Generated once with Pillow (scripts/make_sticker_apng.py).
STICKER_PACK_ID = "corvusanim"
STICKER_PNG_ANIMATED = base64.b64decode(
    "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"
    "+FvaoPgA6QAAAABJRU5ErkJggg=="
)
STICKER_WEBP_ANIMATED = base64.b64decode(
    "UklGRgYIAABXRUJQVlA4WAoAAAASAAAAXwAAXwAAQU5JTQYAAAAAAAAAAABBTk1GRgEAAAUAAAQA"
    "AEwAAEYAAG4AAANWUDhMLgEAAC9MgBEQHyAQIBTbEWRSIEAoUWLXp0CA0KxHpYL5D0B95QGs2trj"
    "VJs7PPcJHwi4k4QxJn8VlVQBXSsFkvwFDET0n4HbRopyONudg4F9BADgGe7BPD2S5fORrPVGvssx"
    "56uU8tp8Qa19AP9LKeU6Umutl4H22lzWjkZvOJrbY+rskdnJpeXQmZ13JyzX47NTWuFsNnandEJj"
    "yria5+C3M/uJ+zgbY882gnek1vX8oDYsg8DUad7Ds5y5sk4pnyw512kpy/UVbxmnK6fMra2pXaTx"
    "uzNp3m4grYNbSOtq5Jh7JnW6Rxg01LDuR4z6o+u8rI+yy/A66stUBwAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAIBdC3JEREREREREREQ8PZQnQU5NRrQAAAAFAAARAABMAAAsAABu"
    "AAAAVlA4TJsAAAAvTAALEBcgEEjSHX8bgUCKI5lkgUAS8+gTzH9A/wJXkWS7yr6ABPwgIR6T45Li"
    "hCV+R/Rfgds2So9vdNB9QpJ0jqMkSZKk1E4RcRhxj4gwxn7I8ice4nltLCNjmSRZMEmyYJJkgYdB"
    "syp66C2r4fvQP0t9mP+Hz77z+/7ybdzweOXxz+OpxWeP915+WHn0cr55eQkAAAAArB/FCgBBTk1G"
    "KgEAAAgAAAQAAEYAAFEAAG4AAANWUDhMEgEAAC9GQBQQHyAQIFNKlGgiECCUKLHrUyBAaNajUsH8"
    "B6Cu+gBuJEmOnASTT4ITkGAANYGuk2PFewBXYMBH7NGdfSpH9J+B27ZxtNPNMSBzPwEAnuFg9iei"
    "3p2EXR1Own99z1ZqrbVOC63ominYHNSaXC7hTGbFC9O00uKlVSbz0Sy6hx5xHNKR/02NUuxT3meV"
    "uWSwqxl3ZLwsWQnD8oVkWKvcDjcBAAAAAAAAAAAAAAAAAECH7Uu5ya7fUkpyfZRSykP6nB9JfGYO"
    "JzqVn2sek8gq0I5sAs3IJtBDM3OT2EOzclN4HFpjaAzi+nDArCIiIiIiIiIi4ulUHPbHe3zS4aUe"
    "v/V4sse3Hd7u8X/HHjlsHwFBTk1GsAAAAAgAAB0AAEYAACUAAG4AAABWUDhMlwAAAC9GQAkQFyAQ"
    "SOIcfyqBQIoz2G+BQBLsPDPPf0D3BjeNJElSac31l8TyST4L5cxTwezu+QlE9F9h27aNu5vd+gpJ"
    "si9JkiQpx9/2YHy2fQ/X7lLkgRAPzsPjr+IdSIqgL2kEXUkjmEFXs1GcQdeyMdyD3jOMHwIAAACA"
    "5xRmVLbj0Hvu+ak9Pz7DkwkuzfBthpMzvF3htgAAQU5NRjoBAAAIAAAEAABGAABXAABuAAADVlA4"
    "TCIBAAAvRsAVEB8gECB0alKpSiBApowZUUAgQChQqUfz/AfAumsBTmxt05MzScdaYYIKAZuK1w9S"
    "KJlXRRxgBQGZ+f79/bL1Ef1n4LaRohz2Bk4a+AQAeIOTOV6Itr8Ih/Z8Ef7bV/Wltdba+GHLumQ0"
    "bhZqSS1ncyWr4JmxWmXwnKqSdWlm3UOPOA9lyf/GRCWOJV+Tqqbk2aFV7Kl4n70Knuc/FMVa+HZM"
    "EwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAdKxz5q66fjOzuL4zM+/K"
    "5/Qo7BOTudCl/FnyWlgWhm3LyrBpWRl62PRcOfawGVkGRkRERERERETEy6XIrIt1I/TU8yycp1Pn"
    "6XjPnHTM0inzBkFOTUawAAAACAAAHQAARgAAJQAAbgAAAVZQOEyXAAAAL0ZACRAXIBBI4hx/KoFA"
    "iuMcYYFAEuw8M89/QPUHN40kSVJpzfWXxPJJPgvlzFPB7O75CUT0X2Hbto27m936CkmyL0mSJCnH"
    "3/ZgfLZ9D9fuUuSBEA/Ow+Ov4h1IiqAvaQRdSSOYQVezUZxB17Ix3IPeM4wfAgAAAIDnFGZUtuPQ"
    "e+75qT0/PsOTCS7N8G2GkzO8XeG2AABBTk1GLgEAAAgAAAQAAEYAAFEAAG4AAAFWUDhMFQEAAC9G"
    "QBQQHyAQIHRqUqlKIEAYMKhMrkCAUKBSj+b5D4D1lwKcWLZlK6v4GWOECSIETCqOH6QQUlfFd4AV"
    "BPyq++Zz/kQc0X8GbtvG0U43x4DM/QQAeIWjOZyJvjsL+/50Fv76Z7bSe+99XNiKLhmDm4Nakss5"
    "nMmseGacVlo8t8pkPppZV6iI05CO/HdslOKQ8jmpzCVP9j1jR8bbnJXwNH8hGdYit+AmAAAAAAAA"
    "AAAAAAAAAAAo2L61m+z6aa0l11drrd2mz+mRxCemcKJT+b3kJYksAtuRVWAzsgpU2MxcJVbYrFwV"
    "noatMWwM4v/hiFlFRERERERERMTzuTjuj1d8UvBSxW8VT1Z8W/B2xf+FPXLcPgIAQU5NRrYAAAAI"
    "AAARAABGAAAsAABuAAAAVlA4TJ4AAAAvRgALEBcgEEjiHH8qgUAS52TjKkjbgMWV2p35D1D/ApvG"
    "tqXcUNJjAj/HD1IoyZX5l/f+klBH9F+BJMnMyi5ifUKS7EOSJEmq7bOdtMe2z3QMh0ZeUMTJOl2+"
    "Le5E0ghiSScIJZ1gBKFmpziC0LIzXIPoGoKL+B9sus81L7XmxUd4MsClEb6NcHKEtwPcHuH/3B8Z"
    "+UcAAAAAXJtwAQ=="
)
STICKER_WEBP_STATIC = base64.b64decode(
    "UklGRmQBAABXRUJQVlA4TFgBAAAvX8AXEB8gJCBT5sjWEQgkg+W3tlgwxd3Daf6DIsMPcCPbjipR"
    "mApF+ahkEgo/FEzqYSgEoiAJ+X/2mcfydN+d3Y3oPwO3beOoRxCsdzvnD90/niaRyBYu38Sk4DNL"
    "VS0ihH3RM62+mauaVMS1Hxb1zHTaxkTHP21rHF8BS/B85Ho+554+dHvwpWJKALxgVsD0gdGIW3IN"
    "RZVsWD1Kj1p6DXnInqrxs8zqskCyJUPqRXo3pHczFGBMd7UjfRJwbEPaFIy0hp/FxBDt8fvd9NM4"
    "5wxx7gUD05/Gi89quDGEn0VXfUr/9fD/C81X/I/De8N2gf7rfLeMyfvfh80ybFCAql2f6jUN0o1O"
    "Ttp3e7K0zB92vGY4wTqWjUnJ6hhqOF5t89V9DY5OJpPejU5igRsuNqOrng43VsXXbVtK5HWejgt8"
    "HOHjDh2n+LhGx0E+btJxlo/L/1/qAA=="
)


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
    elif m == "listStickerPacks":
        res = [{"packId": STICKER_PACK_ID, "title": "Corvus Animation", "author": "Corvus Test",
                "stickers": [{"id": 0, "emoji": "\U0001F642"}, {"id": 1, "emoji": "\u2B50"},
                             {"id": 2, "emoji": "\U0001F422"}]}]
    elif m == "getSticker":
        if p.get("packId") != STICKER_PACK_ID:
            emit({"jsonrpc": "2.0", "id": rid, "error": {"code": -1, "message": "Could not find sticker pack"}})
            continue
        if p.get("stickerId") == 0:
            payload = STICKER_PNG_ANIMATED    # animated APNG: Signal's real sticker format
        elif p.get("stickerId") == 1:
            payload = STICKER_WEBP_ANIMATED   # animated WebP: the QMovie path
        else:
            payload = STICKER_WEBP_STATIC     # single-frame sticker
        res = {"data": base64.b64encode(payload).decode()}
    else:
        emit({"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "unknown method " + str(m)}})
        continue
    emit({"jsonrpc": "2.0", "id": rid, "result": res})
