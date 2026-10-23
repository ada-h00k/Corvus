import XCTest
@testable import CorvusCore

final class EnvelopeTests: XCTestCase {
    func testIncomingMessageWithQuoteAndAttachment() {
        let env: JSON = [
            "sourceNumber": "+4915112345", "sourceUuid": "u-1", "sourceName": "Alice", "timestamp": 1000,
            "dataMessage": [
                "timestamp": 1234, "message": "hi",
                "attachments": [["id": "att1", "contentType": "image/png", "filename": "a.png", "size": 42, "width": 3, "height": 4]],
                "quote": ["id": 999, "authorUuid": "u-2", "text": "orig", "attachments": [["contentType": "image/jpeg"]]],
            ] as JSON,
        ]
        let pe = parseEnvelope(env)
        XCTAssertEqual(pe.kind, .message)
        XCTAssertEqual(pe.src, "+4915112345")
        XCTAssertEqual(pe.srcName, "Alice")
        XCTAssertEqual(pe.msg.ts, 1234)
        XCTAssertEqual(pe.msg.text, "hi")
        XCTAssertEqual(pe.msg.attachments.first?.id, "att1")
        XCTAssertEqual(pe.msg.attachments.first?.size, 42)
        XCTAssertTrue(pe.msg.quote.valid)
        XCTAssertEqual(pe.msg.quote.authorKey, "u-2")
        XCTAssertEqual(pe.msg.quote.attachmentNote, "image/jpeg")
    }

    func testMentions() {
        let pe = parseEnvelope(["sourceNumber": "+49111", "dataMessage": [
            "timestamp": 1, "message": "\u{FFFC} hi \u{FFFC}",
            "mentions": [["name": "+49222", "number": "+49222", "uuid": "u-2", "start": 0, "length": 1],
                         ["name": "Bob", "number": "", "uuid": "u-3", "start": 5, "length": 1]],
        ] as JSON])
        XCTAssertEqual(pe.mentions, [Mention(start: 0, length: 1, number: "+49222", uuid: "u-2", name: "+49222"),
                                     Mention(start: 5, length: 1, uuid: "u-3", name: "Bob")])
        let names = ["+49222": "Alice", "u-3": "Bob"]
        XCTAssertEqual(applyMentions(pe.msg.text, pe.mentions) { names[$0.number.isEmpty ? $0.uuid : $0.number]! }, "@Alice hi @Bob")
        // Emoji before the placeholder: offsets are UTF-16 units.
        XCTAssertEqual(applyMentions("😀 \u{FFFC}", [Mention(start: 3, length: 1)]) { _ in "X" }, "😀 @X")
        // Out of range mentions are ignored.
        XCTAssertEqual(applyMentions("hi", [Mention(start: 5, length: 1)]) { _ in "X" }, "hi")
    }

    func testReactionEditSyncAndReceipts() {
        let reaction = parseEnvelope(["sourceUuid": "u-1", "dataMessage": [
            "reaction": ["emoji": "👍", "targetAuthorNumber": "+49111", "targetSentTimestamp": 77, "isRemove": false],
            "groupInfo": ["groupId": "G1"],
        ] as JSON])
        XCTAssertEqual(reaction.kind, .reaction)
        XCTAssertEqual(reaction.src, "u-1")
        XCTAssertEqual(reaction.groupId, "G1")
        XCTAssertEqual(reaction.targetTs, 77)

        let edit = parseEnvelope(["source": "+49222", "editMessage": [
            "targetSentTimestamp": 5, "dataMessage": ["timestamp": 9, "message": "fixed"],
        ] as JSON])
        XCTAssertEqual(edit.kind, .edit)
        XCTAssertEqual(edit.targetTs, 5)
        XCTAssertEqual(edit.msg.ts, 9)

        let sync = parseEnvelope(["sourceNumber": "+49me", "syncMessage": ["sentMessage": [
            "destinationNumber": "+49333", "timestamp": 11, "message": "from phone", "expiresInSeconds": 30,
        ] as JSON]])
        XCTAssertEqual(sync.kind, .message)
        XCTAssertTrue(sync.outgoing)
        XCTAssertTrue(sync.msg.outgoing)
        XCTAssertEqual(sync.destination, "+49333")
        XCTAssertEqual(sync.msg.expiresInSeconds, 30)

        let delete = parseEnvelope(["sourceUuid": "u-1", "dataMessage": [
            "timestamp": 20, "remoteDelete": ["timestamp": 15], "groupInfo": ["groupId": "G1"],
        ] as JSON])
        XCTAssertEqual(delete.kind, .remoteDelete)
        XCTAssertEqual(delete.src, "u-1")
        XCTAssertEqual(delete.groupId, "G1")
        XCTAssertEqual(delete.targetTs, 15)

        let syncDelete = parseEnvelope(["sourceNumber": "+49me", "syncMessage": ["sentMessage": [
            "destinationNumber": "+49333", "timestamp": 21, "remoteDelete": ["timestamp": 11],
        ] as JSON]])
        XCTAssertEqual(syncDelete.kind, .remoteDelete)
        XCTAssertTrue(syncDelete.outgoing)
        XCTAssertEqual(syncDelete.destination, "+49333")
        XCTAssertEqual(syncDelete.targetTs, 11)

        XCTAssertEqual(parseEnvelope(["sourceNumber": "+1", "receiptMessage": ["isRead": true]]).kind, .none)
        XCTAssertEqual(parseEnvelope(["sourceNumber": "+1", "dataMessage": ["timestamp": 1]]).kind, .none)
        XCTAssertEqual(parseEnvelope(["sourceNumber": "+1", "dataMessage": ["isExpirationUpdate": true, "expiresInSeconds": 60]]).kind,
                       .expirationUpdate)
    }
}

final class SignalCliHelperTests: XCTestCase {
    func testParseAccounts() {
        XCTAssertEqual(SignalCli.parseAccounts(#"[{"number":"+491234567"},"+15550001"]"#), ["+491234567", "+15550001"])
        XCTAssertEqual(SignalCli.parseAccounts("WARN something\nNumber: +4917000000\nNumber: +4917000000"), ["+4917000000"])
        XCTAssertEqual(SignalCli.parseAccounts("[]"), [])
    }

    func testMaskNumbers() {
        XCTAssertEqual(SignalCli.maskNumbers("call +4915112345678 now"), "call +49…78 now")
    }

    func testDescribe() {
        var r = CliResult()
        r.exitCode = 3
        r.err = "boom"
        XCTAssertEqual(r.describe(), "signal-cli exited with code 3.\n\nboom")
        r.timedOut = true
        XCTAssertEqual(r.describe(), "signal-cli did not answer in time.")
    }
}

final class ModelTests: XCTestCase {
    func testEditRules() {
        var m = Message()
        m.outgoing = true
        m.text = "x"
        m.ts = 1_000
        XCTAssertTrue(m.canEdit(nowMs: 1_000 + Message.editWindowMs))
        XCTAssertFalse(m.canEdit(nowMs: 1_001 + Message.editWindowMs))
        m.editCount = Message.maxEdits
        XCTAssertFalse(m.canEdit(nowMs: 1_000))
    }

    func testAttachmentKinds() {
        var a = Attachment()
        a.filename = "voice.OPUS"
        XCTAssertTrue(a.isAudio)
        a.filename = "clip.mov"
        XCTAssertTrue(a.isVideo)
        a.contentType = "image/gif"
        XCTAssertTrue(a.isGif)
        XCTAssertTrue(a.isImage)
    }
}

final class ChatStateTests: XCTestCase {
    func makeState() -> ChatState {
        var s = ChatState()
        s.account = "+49me"
        s.alias["u-alice"] = "+49alice"
        return s
    }

    func testAddDeduplicatesAndUnarchives() {
        var s = makeState()
        var m = Message()
        m.ts = 10
        m.text = "a"
        m.authorKey = "+49alice"
        s.ensure("+49alice")
        s.convs["+49alice"]!.archived = true
        let first = s.addMessage("+49alice", m)
        XCTAssertNotNil(first)
        XCTAssertEqual(first?.records.count, 2) // message + archive
        XCTAssertFalse(s.convs["+49alice"]!.archived)
        XCTAssertNil(s.addMessage("+49alice", m))
    }

    func testIncorrectSenderClockDoesNotInsertNewMessageIntoPast() {
        var s = makeState()
        s.ensure("+49alice")

        // Simulate a legacy history entry whose sender clock was far ahead of ours.
        var old = Message()
        old.ts = nowMs() + 60 * 60 * 1000
        old.text = "Earlier message with a bad clock"
        old.authorKey = "+49alice"
        s.convs["+49alice"]!.msgs.append(old)

        // The next message has a sender timestamp far in the past, but arrived now.
        var incoming = Message()
        incoming.ts = 1_000
        incoming.text = "New message"
        incoming.authorKey = "+49alice"
        XCTAssertNotNil(s.addMessage("+49alice", incoming))

        let conversation = s.convs["+49alice"]!
        XCTAssertEqual(conversation.msgs.last?.text, "New message")
        XCTAssertGreaterThan(conversation.msgs.last!.sortTs, old.sortTs)

        // The independent ordering value must survive encrypted-history compaction/reload.
        var reloaded = makeState()
        reloaded.loadHistory(s.allRecords())
        XCTAssertEqual(reloaded.convs["+49alice"]!.msgs.last?.text, "New message")
        XCTAssertEqual(reloaded.convs["+49alice"]!.msgs.last?.orderTs, conversation.msgs.last?.orderTs)
    }

    func testReactionEditAndReload() {
        var s = makeState()
        var m = Message()
        m.ts = 10
        m.text = "a"
        m.authorKey = "+49alice"
        var records = s.addMessage("+49alice", m)!.records
        // reaction refers to the author by uuid -> resolved through the alias
        records.append(s.applyReaction("+49alice", ts: 10, authors: ["u-alice"], reactor: "+49me", emoji: "❤️")!)
        records.append(s.applyEdit("+49alice", targetTs: 10, authors: ["+49alice"], text: "b", newTs: 20)!)
        XCTAssertNil(s.applyEdit("+49alice", targetTs: 10, authors: ["+49alice"], text: "old", newTs: 15))
        // the edited message is also found by its latest timestamp
        XCTAssertNotNil(s.findMessage(s.convs["+49alice"]!, ts: 20, authors: []))

        var reloaded = makeState()
        reloaded.loadHistory(records)
        let msg = reloaded.convs["+49alice"]!.msgs[0]
        XCTAssertEqual(msg.text, "b")
        XCTAssertEqual(msg.editCount, 1)
        XCTAssertEqual(msg.reactions["+49me"], "❤️")

        // compaction output reproduces the same state
        var compacted = makeState()
        compacted.loadHistory(reloaded.allRecords())
        XCTAssertEqual(compacted.convs["+49alice"]!.msgs, reloaded.convs["+49alice"]!.msgs)
    }

    func testDeleteLeavesPlaceholder() {
        var s = makeState()
        var m = Message()
        m.ts = 10
        m.text = "secret"
        m.authorKey = "+49alice"
        m.quote.valid = true
        m.attachments = [Attachment()]
        var records = s.addMessage("+49alice", m)!.records
        records.append(s.applyReaction("+49alice", ts: 10, authors: ["+49alice"], reactor: "+49me", emoji: "❤️")!)
        // only the author can delete for everyone
        XCTAssertNil(s.applyDelete("+49alice", targetTs: 10, authors: ["+49bob"]))
        records.append(s.applyDelete("+49alice", targetTs: 10, authors: ["u-alice"])!)
        XCTAssertNil(s.applyDelete("+49alice", targetTs: 10, authors: ["+49alice"])) // redelivery

        let placeholder = s.convs["+49alice"]!.msgs[0]
        XCTAssertTrue(placeholder.deleted)
        XCTAssertEqual(placeholder.text, "")
        XCTAssertTrue(placeholder.attachments.isEmpty && placeholder.reactions.isEmpty && !placeholder.quote.valid)
        // a deleted message takes no more reactions or edits
        XCTAssertNil(s.applyReaction("+49alice", ts: 10, authors: [], reactor: "+49me", emoji: "👍"))
        XCTAssertNil(s.applyEdit("+49alice", targetTs: 10, authors: ["+49alice"], text: "back", newTs: 20))

        // appended history and compacted history both reload as the placeholder, without the content
        var reloaded = makeState()
        reloaded.loadHistory(records)
        XCTAssertEqual(reloaded.convs["+49alice"]!.msgs, [placeholder])
        let compactedRecords = s.allRecords()
        XCTAssertEqual(jsonString(compactedRecords.first { jsonString($0["t"]) == "m" }?["x"]), "")
        var compacted = makeState()
        compacted.loadHistory(compactedRecords)
        XCTAssertEqual(compacted.convs["+49alice"]!.msgs, [placeholder])
    }

    func testExpiry() {
        var s = makeState()
        var m = Message()
        m.ts = 10
        m.text = "gone"
        m.expiresInSeconds = 30
        _ = s.addMessage("+49bob", m)
        XCTAssertEqual(s.startIncomingExpirationTimers("+49bob", now: 1_000).count, 1)
        XCTAssertFalse(s.hasExpiredMessages(now: 1_000 + 29_999))
        XCTAssertFalse(s.expireMessages(now: 1_000 + 29_999))
        XCTAssertTrue(s.hasExpiredMessages(now: 1_000 + 30_000))
        XCTAssertTrue(s.expireMessages(now: 1_000 + 30_000))
        XCTAssertFalse(s.hasExpiredMessages(now: 1_000 + 30_000))
        XCTAssertTrue(s.convs["+49bob"]!.msgs.isEmpty)
    }

    func testPendingSendConfirmedByEcho() {
        var s = makeState()
        s.ensure("+49bob")
        var preview = Message()
        preview.ts = 100
        preview.text = "hello"
        preview.outgoing = true
        preview.authorKey = "+49me"
        s.convs["+49bob"]!.insertSorted(preview)
        var p = PendingSend()
        p.conversationKey = "+49bob"
        p.temporaryTimestamp = 100
        p.message = preview
        s.pendingSends["id1"] = p
        XCTAssertEqual(s.allRecords().filter { jsonString($0["t"]) == "m" }.count, 0) // previews are not persisted

        var echo = preview
        echo.ts = 150
        XCTAssertEqual(s.pendingSendMatching(echo: echo, key: "+49bob"), "id1")
        let recs = s.finalizePendingSend("id1", timestamp: 150, echo: echo)
        XCTAssertEqual(recs.count, 1)
        XCTAssertEqual(s.convs["+49bob"]!.msgs.map(\.ts), [150])
        XCTAssertTrue(s.pendingSends["id1"]!.echoReceived)
        // A second identical echo belongs to an already confirmed message.
        XCTAssertNil(s.pendingSendMatching(echo: echo, key: "+49bob"))
    }
}

final class HistoryStoreTests: XCTestCase {
    func testRoundTripThroughRustStore() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let path = dir.appendingPathComponent("history.corvus").path

        var m = Message()
        m.ts = 1_700_000_000_000
        m.text = "secret text"
        m.authorKey = "+49alice"
        m.quote.valid = true
        m.quote.id = 5
        var a = Attachment()
        a.id = "x"
        a.contentType = "image/png"
        a.thumb = Data([1, 2, 3])
        m.attachments = [a]
        m.expiresInSeconds = 60
        m.expirationStartMs = 2
        var second = m
        second.ts += 10
        second.text = "deleted later"

        let records: [JSON] = [
            Records.message("+49alice", m),
            Records.name("+49alice", "Alice"),
            Records.nickname("+49alice", "Ali"),
            Records.archive("+49alice", archived: true, sigArchived: false),
            Records.mute("+49alice", true),
            Records.expiration("+49alice", 60),
            Records.reaction("+49alice", ts: m.ts, author: "+49alice", by: "+49me", emoji: "👍"),
            Records.edit("+49alice", { var e = m; e.text = "edited"; e.editTs = m.ts + 1; e.editCount = 1; return e }()),
            Records.attachmentsUpdate("+49alice", m),
            Records.expirationUpdate("+49alice", m),
            Records.message("+49alice", second),
            Records.deletion("+49alice", second),
            Records.message("+49alice", { var d = second; d.ts += 5; d.markDeleted(); return d }()),
        ]

        let store = HistoryStore()
        XCTAssertFalse(HistoryStore.exists(path))
        try store.create(path: path, passphrase: "pw")
        for r in records { try store.append(r) } // the Rust Record enum must accept every shape
        store.close()

        XCTAssertThrowsError(try HistoryStore().open(path: path, passphrase: "wrong")) { e in
            XCTAssertEqual(e as? HistoryStoreError, .wrongPassphrase)
        }
        let reopened = HistoryStore()
        let loaded = try reopened.open(path: path, passphrase: "pw")
        XCTAssertEqual(loaded.count, records.count)

        var s = ChatState()
        s.loadHistory(loaded)
        let c = s.convs["+49alice"]!
        XCTAssertEqual(c.title, "Ali")
        XCTAssertTrue(c.archived && c.muted)
        XCTAssertEqual(c.msgs.first?.text, "edited")
        XCTAssertEqual(c.msgs.first?.attachments.first?.thumb, Data([1, 2, 3]))
        XCTAssertEqual(c.msgs.first?.reactions["+49me"], "👍")
        XCTAssertEqual(c.msgs.count, 3)
        XCTAssertTrue(c.msgs[1].deleted && c.msgs[2].deleted)
        XCTAssertEqual(c.msgs[1].text, "")

        try reopened.rewrite(s.allRecords())
        try reopened.changePassphrase(old: "pw", new: "pw2")
        reopened.close()
        XCTAssertNoThrow(try HistoryStore().open(path: path, passphrase: "pw2"))

        // The file never contains the plaintext.
        let raw = try Data(contentsOf: URL(fileURLWithPath: path))
        XCTAssertNil(raw.range(of: Data("secret text".utf8)))
    }
}

@MainActor
final class SignalCliProcessTests: XCTestCase {
    /// Runs the Python mock of the Qt test-suite as signal-cli. The repository copy is not
    /// executable (CMake copies it with the right mode), so the test does the same.
    func mockPath() throws -> String {
        let source = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().appendingPathComponent("../../../tests/mock-signal-cli.py").standardized
        let target = FileManager.default.temporaryDirectory.appendingPathComponent("mock-signal-cli-\(UUID().uuidString).py")
        try FileManager.default.copyItem(at: source, to: target)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: target.path)
        return target.path
    }

    func testOneShotAndJsonRpcAgainstMock() async throws {
        try XCTSkipUnless(FileManager.default.isExecutableFile(atPath: "/usr/bin/python3"), "python3 missing")
        let cli = SignalCli()
        cli.executable = try mockPath()
        defer { try? FileManager.default.removeItem(atPath: cli.executable) }
        let (vr, version) = await cli.queryVersion()
        XCTAssertTrue(vr.ok, vr.describe())
        XCTAssertEqual(version, "signal-cli 0.14.9")
        let (ar, accounts) = await cli.queryAccounts()
        XCTAssertTrue(ar.ok)
        XCTAssertEqual(accounts, ["+491234567"])

        cli.start(account: "+491234567")
        let contacts = try await cli.call("listContacts")
        XCTAssertNotNil(contacts as? [Any])
        do {
            _ = try await cli.call("noSuchMethodHopefully")
        } catch {
            // either an RPC error or a result, but never a hang
        }
        cli.stop()
        XCTAssertFalse(cli.isRunning)
    }
}

final class AttachmentPolicyTests: XCTestCase {
    private func policy(_ name: String, _ mime: String = "application/octet-stream", media: Bool = false) -> AttachmentPolicy {
        AttachmentPolicy.classify(filename: name, contentType: mime, isMedia: media)
    }

    func testRunnableFilesAreRisky() {
        for name in ["x.terminal", "run.sh", "a.command", "Tool.app", "x.py", "s.applescript", "l.fileloc", "noext",
                     "X.TERMINAL", "x.terminal ", "x.terminal.", "dir/x.zsh"] {
            XCTAssertEqual(policy(name), .risky, name)
        }
    }

    func testSenderContentTypeCannotHideRiskyFiles() {
        XCTAssertEqual(policy("x.terminal", "image/png", media: true), .risky)
        XCTAssertEqual(policy("x.sh", "video/mp4", media: true), .risky)
        XCTAssertEqual(policy("a.pdf", "application/x-sh"), .risky)
        XCTAssertEqual(policy("a.pdf", "text/x-shellscript; charset=utf-8"), .risky)
    }

    func testOtherFilesAskAndMediaStaysInApp() {
        XCTAssertEqual(policy("a.pdf", "application/pdf"), .ask)
        XCTAssertEqual(policy("notes.txt", "text/plain"), .ask)
        XCTAssertEqual(policy("archive.zip", "application/zip"), .ask)
        XCTAssertEqual(policy("photo.jpg", "image/jpeg", media: true), .inApp)
        XCTAssertEqual(policy("", "audio/aac", media: true), .inApp) // voice notes have no file name
    }

    func testMediaSuffixNeverTakesTheSendersExtension() {
        XCTAssertEqual(AttachmentPolicy.mediaSuffix(filename: "clip.mov", contentType: "video/quicktime", fallback: "mp4"), "mov")
        XCTAssertEqual(AttachmentPolicy.mediaSuffix(filename: "x.terminal", contentType: "video/mp4", fallback: "mp4"), "mp4")
        XCTAssertEqual(AttachmentPolicy.mediaSuffix(filename: "x.terminal", contentType: "text/plain", fallback: "m4a"), "m4a")
    }
}
