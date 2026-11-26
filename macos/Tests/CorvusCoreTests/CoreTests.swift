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

        XCTAssertEqual(parseEnvelope(["sourceNumber": "+1", "receiptMessage": ["isRead": true]]).kind, .none) // no timestamps
        let read = parseEnvelope(["sourceNumber": "+1", "receiptMessage": ["isRead": true, "timestamps": [5, 6]] as JSON])
        XCTAssertEqual(read.kind, .receipt)
        XCTAssertEqual(read.receiptLevel, .read)
        XCTAssertEqual(read.receiptTimestamps, [5, 6])
        let viewed = parseEnvelope(["sourceNumber": "+1", "receiptMessage": ["isViewed": true, "timestamps": [5]] as JSON])
        XCTAssertEqual(viewed.receiptLevel, .read)
        let delivered = parseEnvelope(["sourceNumber": "+1", "receiptMessage": ["isDelivery": true, "timestamps": [5]] as JSON])
        XCTAssertEqual(delivered.kind, .receipt)
        XCTAssertEqual(delivered.receiptLevel, .delivered)
        XCTAssertEqual(parseEnvelope(["sourceNumber": "+1", "dataMessage": ["timestamp": 1]]).kind, .none)
        XCTAssertEqual(parseEnvelope(["sourceNumber": "+1", "dataMessage": ["isExpirationUpdate": true, "expiresInSeconds": 60]]).kind,
                       .expirationUpdate)
    }
}

final class TypingEnvelopeTests: XCTestCase {
    func testTyping() {
        let started = parseEnvelope(["sourceNumber": "+1", "typingMessage": ["action": "STARTED", "timestamp": 9] as JSON])
        XCTAssertEqual(started.kind, .typing)
        XCTAssertTrue(started.typingStarted)
        XCTAssertEqual(started.src, "+1")
        XCTAssertEqual(started.groupId, "")

        let stopped = parseEnvelope(["sourceNumber": "+1", "typingMessage": ["action": "STOPPED", "groupId": "G1"] as JSON])
        XCTAssertEqual(stopped.kind, .typing)
        XCTAssertFalse(stopped.typingStarted)
        XCTAssertEqual(stopped.groupId, "G1")

        XCTAssertEqual(parseEnvelope(["sourceNumber": "+1", "typingMessage": ["action": "WHATEVER"] as JSON]).kind, .none)
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
        XCTAssertTrue(s.expireMessages(now: 1_000 + 29_999).isEmpty)
        XCTAssertTrue(s.hasExpiredMessages(now: 1_000 + 30_000))
        XCTAssertFalse(s.expireMessages(now: 1_000 + 30_000).isEmpty)
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

    private func outgoing(_ ts: Int64, _ text: String = "hi") -> Message {
        var m = Message()
        m.ts = ts
        m.text = text
        m.outgoing = true
        m.authorKey = "+49me"
        return m
    }

    func testReceiptsOnlyGoUpAndPersist() {
        var s = makeState()
        s.addMessage("+49bob", outgoing(100))
        s.addMessage("g:G1", outgoing(200), group: true)

        let delivered = s.applyReceipt(ts: 100, level: .delivered, preferredKey: "+49bob")
        XCTAssertEqual(jsonString(delivered?["t"]), "u")
        XCTAssertEqual(s.convs["+49bob"]!.msgs[0].receipt, .delivered)
        XCTAssertNotNil(s.applyReceipt(ts: 100, level: .read, preferredKey: "+49bob"))
        XCTAssertNil(s.applyReceipt(ts: 100, level: .delivered, preferredKey: "+49bob")) // never back down
        XCTAssertEqual(s.convs["+49bob"]!.msgs[0].receipt, .read)

        // Group messages are found although the receipt names only the member's 1:1 chat.
        XCTAssertNotNil(s.applyReceipt(ts: 200, level: .delivered, preferredKey: "+49carol"))
        XCTAssertEqual(s.convs["g:G1"]!.msgs[0].receipt, .delivered)

        // Message record and update record both survive a reload.
        let rec = Records.message("+49bob", s.convs["+49bob"]!.msgs[0])
        XCTAssertEqual(Records.messageFromRecord(rec).receipt, .read)
        var r = makeState()
        r.loadHistory([Records.message("g:G1", outgoing(200)),
                       Records.receiptUpdate("g:G1", s.convs["g:G1"]!.msgs[0])])
        XCTAssertEqual(r.convs["g:G1"]!.msgs[0].receipt, .delivered)
        // Nothing written for a plain sent message.
        XCTAssertNil(jsonObject(Records.message("+49bob", outgoing(1))["d"])["rs"])
    }

    func testNoteToSelfIsPinnedWithFixedTitle() {
        var s = makeState()
        s.alias["uuid-me"] = s.account
        s.addMessage("+49bob", outgoing(500))
        s.ensureNoteToSelf()
        XCTAssertEqual(s.convs[s.account]?.title, "Note to Self")
        XCTAssertTrue(s.isNoteToSelf("uuid-me"))
        XCTAssertFalse(s.isNoteToSelf("+49bob"))
        // pinned on top although Bob's chat is newer, also when sorted by name
        XCTAssertEqual(s.sortedConversations(archived: false).first?.key, s.account)
        XCTAssertEqual(s.sortedConversations(archived: false, alphabetical: true).first?.key, s.account)
        // an archived Note to Self lives in the folder like any other chat
        s.convs[s.account]!.archived = true
        XCTAssertEqual(s.sortedConversations(archived: false).map(\.key), ["+49bob"])
        XCTAssertEqual(s.sortedConversations(archived: true).map(\.key), [s.account])
    }

    func testViewOnceIsConsumedOnceAndExpires() {
        var s = makeState()
        var m = Message()
        m.ts = 1000
        m.authorKey = "+49alice"
        m.viewOnce = true
        m.text = "caption"
        var a = Attachment()
        a.id = "vo"
        a.contentType = "image/jpeg"
        m.attachments = [a]
        m.sanitizeViewOnce()
        XCTAssertEqual(m.text, "", "a View Once item has no caption")
        XCTAssertEqual(m.attachments.count, 1)
        s.addMessage("+49alice", m)
        // the stored record keeps only the opaque reference
        let rec = Records.message("+49alice", s.convs["+49alice"]!.msgs[0])
        XCTAssertEqual(jsonBool(jsonObject(rec["d"])["vo"]), true)

        let consumed = s.consumeViewOnce("+49alice", ts: 1000, author: "+49alice", expired: false)
        XCTAssertEqual(consumed?.attachments.map(\.id), ["vo"])
        XCTAssertNil(s.consumeViewOnce("+49alice", ts: 1000, author: "+49alice", expired: false), "only once")
        XCTAssertTrue(s.convs["+49alice"]!.msgs[0].viewOnceViewed)
        var r = makeState()
        r.loadHistory([rec, consumed!.record])
        XCTAssertTrue(r.convs["+49alice"]!.msgs[0].viewOnceViewed)
        XCTAssertTrue(r.convs["+49alice"]!.msgs[0].attachments.isEmpty)

        // an unopened one runs out after 45 days
        var e = makeState()
        e.addMessage("+49alice", m)
        XCTAssertFalse(e.hasExpiredViewOnce(now: 1000 + Message.viewOnceLifetimeMs - 1))
        XCTAssertEqual(e.expireViewOnce(now: 1000 + Message.viewOnceLifetimeMs).count, 1)
        XCTAssertTrue(e.convs["+49alice"]!.msgs[0].viewOnceExpired)
    }

    func testSentViewOnceKeepsNothing() {
        var s = makeState()
        s.ensure("+49bob")
        var preview = outgoing(100)
        preview.viewOnce = true
        var a = Attachment()
        a.path = "/tmp/x.jpg"
        a.thumb = Data([1])
        preview.attachments = [a]
        s.convs["+49bob"]!.insertSorted(preview)
        var p = PendingSend()
        p.conversationKey = "+49bob"
        p.temporaryTimestamp = 100
        p.message = preview
        s.pendingSends["id"] = p
        _ = s.finalizePendingSend("id", timestamp: 150)
        XCTAssertTrue(s.convs["+49bob"]!.msgs[0].viewOnce)
        XCTAssertTrue(s.convs["+49bob"]!.msgs[0].attachments.isEmpty)
    }

    func testPinsFromEnvelopesAndLimit() {
        let env: JSON = ["sourceNumber": "+49alice", "timestamp": 50,
                         "dataMessage": ["timestamp": 50, "pinMessage": ["targetAuthorNumber": "+49me", "targetSentTimestamp": 100,
                                                                         "pinDurationSeconds": 0] as JSON] as JSON]
        let pe = parseEnvelope(env)
        XCTAssertEqual(pe.kind, .pin)
        XCTAssertEqual(pe.targetTs, 100)
        XCTAssertEqual(pe.pinDurationSeconds, -1, "unspecified = forever")
        let unpin: JSON = ["sourceNumber": "+49alice", "timestamp": 60,
                           "dataMessage": ["timestamp": 60, "unpinMessage": ["targetAuthorNumber": "+49me", "targetSentTimestamp": 100] as JSON] as JSON]
        XCTAssertEqual(parseEnvelope(unpin).kind, .unpin)

        var s = makeState()
        for ts in [100, 200, 300, 400] as [Int64] { s.addMessage("+49alice", outgoing(ts)) }
        XCTAssertNil(s.applyPin("+49alice", targetTs: 999, authors: ["+49me"], by: "+49alice", durationSeconds: -1, now: 1))
        for ts in [100, 200, 300, 400] as [Int64] {
            XCTAssertNotNil(s.applyPin("+49alice", targetTs: ts, authors: ["+49me"], by: "+49alice", durationSeconds: -1, now: 1))
        }
        XCTAssertEqual(s.convs["+49alice"]!.pins.map(\.ts), [200, 300, 400], "a fourth pin replaces the oldest")
        // re-pinning moves it to the end instead of showing it twice
        _ = s.applyPin("+49alice", targetTs: 200, authors: ["+49me"], by: "+49me", durationSeconds: 86400, now: 1000)
        XCTAssertEqual(s.convs["+49alice"]!.pins.map(\.ts), [300, 400, 200])
        XCTAssertNotNil(s.applyUnpin("+49alice", targetTs: 300, authors: ["+49me"]))
        XCTAssertNil(s.applyUnpin("+49alice", targetTs: 300, authors: ["+49me"]))
        XCTAssertEqual(s.pruneExpiredPins(now: 1000 + 86_400_000), ["+49alice"])
        XCTAssertEqual(s.convs["+49alice"]!.pins.map(\.ts), [400])
    }

    func testPerRecipientReceipts() {
        var s = makeState()
        s.addMessage("g:G1", outgoing(200), group: true)
        XCTAssertNotNil(s.applyReceipt(ts: 200, level: .delivered, recipient: "+49alice", timeMs: 210))
        XCTAssertNotNil(s.applyReceipt(ts: 200, level: .delivered, recipient: "+49bob", timeMs: 215))
        // The aggregate is already "delivered", but Bob's read state is new.
        XCTAssertNotNil(s.applyReceipt(ts: 200, level: .read, recipient: "+49bob", timeMs: 230))
        XCTAssertNil(s.applyReceipt(ts: 200, level: .delivered, recipient: "+49bob", timeMs: 240)) // nothing new
        let m = s.convs["g:G1"]!.msgs[0]
        XCTAssertEqual(m.receipt, .read)
        XCTAssertEqual(m.recipientReceipts["+49alice"], RecipientReceipt(status: 1, deliveredAtMs: 210, readAtMs: 0))
        XCTAssertEqual(m.recipientReceipts["+49bob"], RecipientReceipt(status: 2, deliveredAtMs: 215, readAtMs: 230))
        // Survives the update record and a reload.
        var r = makeState()
        r.loadHistory([Records.message("g:G1", outgoing(200)), Records.receiptUpdate("g:G1", m)])
        XCTAssertEqual(r.convs["g:G1"]!.msgs[0].recipientReceipts, m.recipientReceipts)
    }

    func testEarlyReceiptAppliedOnConfirmation() {
        var s = makeState()
        s.ensure("+49bob")
        let preview = outgoing(100)
        s.convs["+49bob"]!.insertSorted(preview)
        var p = PendingSend()
        p.conversationKey = "+49bob"
        p.temporaryTimestamp = 100
        p.message = preview
        s.pendingSends["id1"] = p

        // The preview's temporary timestamp is not the real one: the receipt waits.
        XCTAssertNil(s.applyReceipt(ts: 150, level: .delivered, preferredKey: "+49bob"))
        XCTAssertNil(s.applyReceipt(ts: 100, level: .read, preferredKey: "+49bob"))
        XCTAssertEqual(s.convs["+49bob"]!.msgs[0].receipt, .sent)

        let recs = s.finalizePendingSend("id1", timestamp: 150)
        XCTAssertEqual(s.convs["+49bob"]!.msgs[0].receipt, .delivered)
        XCTAssertEqual(jsonInt(jsonObject(recs.first?["d"])["rs"]), 1)
        XCTAssertNil(s.earlyReceipts[150])
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
        XCTAssertEqual(version, "corvus-cli 1.6.0 (0123456) compatible with signal-cli 0.14.9")
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

    func testSafetyNumberAndTrustAgainstMock() async throws {
        try XCTSkipUnless(FileManager.default.isExecutableFile(atPath: "/usr/bin/python3"), "python3 missing")
        let cli = SignalCli()
        cli.executable = try mockPath()
        defer { try? FileManager.default.removeItem(atPath: cli.executable) }
        cli.start(account: "+491234567")
        defer { cli.stop() }
        let res = try await cli.call("listIdentities", ["number": "+49111"])
        let id = try XCTUnwrap(SafetyNumber.pick(SafetyNumber.parse(res), recipient: "+49111"))
        XCTAssertEqual(id.trustLevel, "TRUSTED_UNVERIFIED")
        XCTAssertFalse(SafetyNumber.scannableBytes(id.scannable).isEmpty)
        _ = try await cli.call("trust", ["recipient": "+49111", "verifiedSafetyNumber": id.digits])
        let after = SafetyNumber.pick(SafetyNumber.parse(try await cli.call("listIdentities", ["number": "+49111"])), recipient: "+49111")
        XCTAssertEqual(after?.isVerified, true)
    }

    /// The calls behind Block contact and Message requests, with the parameters the app sends.
    func testBlockAndAcceptRequestAgainstMock() async throws {
        try XCTSkipUnless(FileManager.default.isExecutableFile(atPath: "/usr/bin/python3"), "python3 missing")
        let cli = SignalCli()
        cli.executable = try mockPath()
        defer { try? FileManager.default.removeItem(atPath: cli.executable) }
        cli.start(account: "+491234567")
        defer { cli.stop() }
        func contact(_ number: String) async throws -> JSON {
            jsonArray(try await cli.call("listContacts")).map { jsonObject($0) }.first { jsonString($0["number"]) == number } ?? [:]
        }
        var dave = try await contact("+49444")
        XCTAssertTrue(jsonBool(dave["isHidden"])) // an open request
        _ = try await cli.call("removeContact", ["recipient": "+49444"])
        dave = try await contact("+49444")
        XCTAssertFalse(jsonBool(dave["isHidden"]))
        _ = try await cli.call("block", ["recipient": ["+49111"]])
        var alice = try await contact("+49111")
        XCTAssertTrue(jsonBool(alice["isBlocked"]))
        _ = try await cli.call("unblock", ["recipient": ["+49111"]])
        alice = try await contact("+49111")
        XCTAssertFalse(jsonBool(alice["isBlocked"]))
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

final class VoiceNoteTests: XCTestCase {
    func testDurationText() {
        XCTAssertEqual(VoiceNote.durationText(0), "0:00")
        XCTAssertEqual(VoiceNote.durationText(7_400), "0:07")
        XCTAssertEqual(VoiceNote.durationText(723_000), "12:03")
        XCTAssertEqual(VoiceNote.durationText(3_723_000), "1:02:03")
        XCTAssertEqual(VoiceNote.durationText(-5), "0:00")
    }

    func testVoiceNoteNeedsSignalCli0142() {
        XCTAssertFalse(VoiceNote.supportsVoiceNote(signalCliVersion: "0.13.12"))
        XCTAssertFalse(VoiceNote.supportsVoiceNote(signalCliVersion: "0.14.1"))
        XCTAssertTrue(VoiceNote.supportsVoiceNote(signalCliVersion: "0.14.2"))
        XCTAssertTrue(VoiceNote.supportsVoiceNote(signalCliVersion: "signal-cli 0.15.0"))
        XCTAssertTrue(VoiceNote.supportsVoiceNote(signalCliVersion: "1.0"))
        XCTAssertFalse(VoiceNote.supportsVoiceNote(signalCliVersion: ""))
        XCTAssertFalse(VoiceNote.supportsVoiceNote(signalCliVersion: "unknown"))
    }

    func testBaseName() {
        var c = DateComponents()
        (c.year, c.month, c.day, c.hour, c.minute, c.second) = (2026, 10, 27, 14, 15, 23)
        let d = Calendar.current.date(from: c)!
        XCTAssertEqual(VoiceNote.baseName(d), "voice-message-2026-10-27-14-15-23")
    }
}

final class GroupInfoTests: XCTestCase {
    let sample: JSON = [
        "id": "G1", "name": "Hikers", "description": "Weekend trips", "isMember": true,
        "messageExpirationTime": 3600,
        "members": [["number": "+49me", "uuid": "u-me", "isAdmin": true] as JSON,
                    ["number": "+49bob", "uuid": "u-bob", "isAdmin": false] as JSON],
        "pendingMembers": [["uuid": "u-carol"] as JSON],
        "requestingMembers": [["number": "+49dave"] as JSON],
        "banned": [["number": "+49eve"] as JSON],
        "permissionAddMember": "ONLY_ADMINS", "permissionEditDetails": "every-member",
        "groupInviteLink": "https://signal.group/#abc",
    ]

    func testParse() {
        let g = GroupInfo.parseOne(sample)
        XCTAssertEqual(g.id, "G1")
        XCTAssertEqual(g.expiration, 3600)
        XCTAssertEqual(g.members.map(\.id), ["+49me", "+49bob"])
        XCTAssertEqual(g.pending.map(\.id), ["u-carol"])
        XCTAssertEqual(g.requesting.map(\.id), ["+49dave"])
        XCTAssertEqual(g.banned.map(\.id), ["+49eve"])
        XCTAssertEqual(g.permAddMember, .onlyAdmins)
        XCTAssertEqual(g.permEditDetails, .everyMember)
        XCTAssertEqual(g.permSendMessage, .everyMember) // missing = every member
        XCTAssertTrue(g.isAdmin("u-me"))
        XCTAssertFalse(g.isAdmin("+49bob"))
        XCTAssertTrue(g.canAddMembers("+49me"))
        XCTAssertFalse(g.canAddMembers("+49bob"))     // only admins
        XCTAssertTrue(g.canEditDetails("+49bob"))
        XCTAssertFalse(g.canEditDetails("+49stranger"))
        XCTAssertEqual(GroupInfo.find([sample, ["id": "G2"] as JSON], id: "G2")?.id, "G2")
        XCTAssertNil(GroupInfo.find([sample], id: "nope"))
    }

    func testOlderSignalCliListsAdminsSeparately() {
        let g = GroupInfo.parseOne(["id": "G", "members": [["number": "+1"] as JSON, ["number": "+2"] as JSON],
                                    "admins": [["number": "+2"] as JSON]])
        XCTAssertEqual(g.members.filter(\.admin).map(\.id), ["+2"])
    }

    func testInvitation() {
        var g = GroupInfo.parseOne(["id": "G", "isMember": false, "pendingMembers": [["number": "+49me"] as JSON]])
        XCTAssertTrue(g.isInvitation(for: "+49me"))
        XCTAssertFalse(g.isInvitation(for: "+49other"))
        g.isTerminated = true
        XCTAssertFalse(g.isInvitation(for: "+49me"))
    }

    func testChangesAndParams() {
        let g = GroupInfo.parseOne(sample)
        var e = GroupInfo.Edit()
        e.name = "Hikers"                 // unchanged
        e.description = "Mountains"
        e.permAddMember = .onlyAdmins     // unchanged
        e.permSendMessage = .onlyAdmins
        e.linkState = .enabledWithApproval
        let p = g.changes(e)
        XCTAssertNil(p["name"])
        XCTAssertEqual(jsonString(p["description"]), "Mountains")
        XCTAssertNil(p["setPermissionAddMember"])
        XCTAssertEqual(jsonString(p["setPermissionSendMessages"]), "only-admins")
        XCTAssertEqual(jsonString(p["link"]), "enabled-with-approval")
        XCTAssertTrue(g.changes(GroupInfo.Edit()).isEmpty)

        let c = GroupInfo.createParams(name: "New", description: "", members: ["+1", "+2"], avatarPath: "")
        XCTAssertEqual(jsonString(c["name"]), "New")
        XCTAssertNil(c["description"])
        XCTAssertEqual(c["member"] as? [String], ["+1", "+2"])
        XCTAssertEqual(GroupInfo.memberParams("ban", ["+3"])["ban"] as? [String], ["+3"])
    }
}

final class StickerTests: XCTestCase {
    let pack = "0123456789abcdef0123456789abcdef"
    let key = String(repeating: "ab", count: 32)

    func testIncomingSticker() {
        // corvus-cli 1.6 reports where a received sticker's image is stored (attachmentId, F5).
        let pe = parseEnvelope(["sourceNumber": "+1", "dataMessage": [
            "timestamp": 5, "sticker": ["packId": pack, "stickerId": 3, "attachmentId": "att-1"] as JSON,
        ] as JSON])
        XCTAssertEqual(pe.kind, .message) // a sticker alone is a message
        XCTAssertEqual(pe.msg.attachments.count, 1)
        let a = pe.msg.attachments[0]
        XCTAssertTrue(a.isSticker)
        XCTAssertTrue(a.isImage)
        XCTAssertEqual(a.stickerPackId, pack)
        XCTAssertEqual(a.stickerId, 3)
        XCTAssertEqual(a.id, "att-1") // fetched with getAttachment; getSticker serves installed packs only
        XCTAssertEqual(a.filename, "signal-sticker:\(pack):3.webp") // same convention as the Qt app

        // Other spellings signal-cli has used, and a string id.
        let alt = parseEnvelope(["sourceNumber": "+1", "dataMessage": [
            "timestamp": 6, "sticker": ["pack_id": "p", "sticker_id": "7"] as JSON,
        ] as JSON])
        XCTAssertEqual(alt.msg.attachments.first?.stickerId, 7)
        // No attachmentId reported: the pack-reference path stays, the id stays empty.
        XCTAssertTrue(alt.msg.attachments.first?.id.isEmpty ?? false)
        // Broken references are dropped.
        XCTAssertEqual(parseEnvelope(["sourceNumber": "+1", "dataMessage": [
            "timestamp": 7, "sticker": ["packId": "", "stickerId": 1] as JSON,
        ] as JSON]).kind, .none)
    }

    func testStickerAttachmentFields() {
        var a = Attachment()
        a.filename = "photo.webp"
        XCTAssertFalse(a.isSticker)
        XCTAssertEqual(a.stickerId, -1)
        a.filename = "signal-sticker:p:x.webp"
        XCTAssertTrue(a.isSticker)
        XCTAssertFalse(a.isValidSticker)
        // A sticker survives a trip through the history records.
        var m = Message()
        m.ts = 1
        m.attachments = [Attachment.sticker(packId: "p", stickerId: 2)]
        XCTAssertEqual(Records.messageFromRecord(Records.message("c", m)).attachments.first?.stickerId, 2)
    }

    func testParsePacks() {
        let packs = StickerPack.parse([
            ["packId": "b", "title": "Zebras", "stickers": [["id": 0, "emoji": "🦓"] as JSON, ["id": 1] as JSON]] as JSON,
            ["pack": ["pack_id": "a", "name": "Ants", "manifest": ["stickers": [["emoji": "🐜"] as JSON]] as JSON] as JSON] as JSON,
            ["title": "no id"] as JSON,
        ])
        XCTAssertEqual(packs.map(\.id), ["a", "b"]) // sorted by title, packs without id dropped
        XCTAssertEqual(packs[0].stickers, [StickerPack.Sticker(id: 0, emoji: "🐜")]) // index as id fallback
        XCTAssertTrue(packs[1].matches("🦓"))
        XCTAssertTrue(packs[1].matches("zeb"))
        XCTAssertFalse(packs[1].matches("ants"))
        XCTAssertEqual(StickerPack.parse(["packs": [["packId": "x"] as JSON]] as JSON).first?.title, "Sticker pack")
    }

    func testPackUrl() {
        XCTAssertTrue(StickerPack.isValidPackUrl("https://signal.art/addstickers/#pack_id=\(pack)&pack_key=\(key)"))
        XCTAssertTrue(StickerPack.isValidPackUrl("sgnl://addstickers/?pack_id=\(pack)&pack_key=\(key)"))
        XCTAssertFalse(StickerPack.isValidPackUrl("https://evil.example/addstickers/#pack_id=\(pack)&pack_key=\(key)"))
        XCTAssertFalse(StickerPack.isValidPackUrl("https://signal.art/addstickers/#pack_id=\(pack)"))
        XCTAssertFalse(StickerPack.isValidPackUrl("https://signal.art/addstickers/#pack_id=xyz&pack_key=\(key)"))
        XCTAssertFalse(StickerPack.isValidPackUrl("not a url"))
    }
}

final class BackendTests: XCTestCase {
    func testParseVersionLine() {
        let c = BackendInfo.parse(versionLine: "corvus-cli 0.1.0 (2AB8C86) compatible with signal-cli 0.14.9\n")
        XCTAssertTrue(c.isCorvusCli)
        XCTAssertEqual(c.version, "0.1.0")
        XCTAssertEqual(c.build, "2ab8c86")
        XCTAssertEqual(c.signalVersion, "0.14.9") // feature checks (e.g. voice notes) use this
        XCTAssertEqual(c.description, "corvus-cli 0.1.0 (2ab8c86)")

        let noBuild = BackendInfo.parse(versionLine: "corvus-cli 0.2.0 compatible with signal-cli 0.15.1")
        XCTAssertEqual(noBuild.build, "")
        XCTAssertEqual(noBuild.signalVersion, "0.15.1")

        let s = BackendInfo.parse(versionLine: "signal-cli 0.14.10")
        XCTAssertFalse(s.isCorvusCli)
        XCTAssertEqual(s.version, "0.14.10")
        XCTAssertEqual(s.signalVersion, "0.14.10")
        XCTAssertEqual(s.description, "signal-cli 0.14.10")
    }

    func testNameForExecutable() {
        XCTAssertEqual(BackendInfo.name(forExecutable: "/opt/homebrew/bin/corvus-cli"), "corvus-cli")
        XCTAssertEqual(BackendInfo.name(forExecutable: "/opt/homebrew/Caskroom/corvus-cli/x/corvus-cli-macos-aarch64"), "corvus-cli")
        XCTAssertEqual(BackendInfo.name(forExecutable: "/opt/homebrew/bin/signal-cli"), "signal-cli")
        var r = CliResult()
        r.backend = "corvus-cli"
        r.timedOut = true
        XCTAssertEqual(r.describe(), "corvus-cli did not answer in time.")
    }

    func testAutodetectFindsOnlyCorvusCli() throws {
        let fm = FileManager.default
        let a = fm.temporaryDirectory.appendingPathComponent("corvus-autodetect-\(UUID().uuidString)")
        let b = a.appendingPathComponent("second")
        try fm.createDirectory(at: b, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: a) }
        func exe(_ dir: URL, _ name: String) throws {
            let p = dir.appendingPathComponent(name).path
            fm.createFile(atPath: p, contents: Data("#!/bin/sh\n".utf8), attributes: [.posixPermissions: 0o755])
        }
        // A signal-cli on PATH is ignored: only corvus-cli is a supported backend.
        try exe(a, "signal-cli")
        try exe(b, "corvus-cli")
        XCTAssertEqual(SignalCli.autodetect(environment: ["PATH": a.path + ":" + b.path], extraDirs: []), b.appendingPathComponent("corvus-cli").path)
        // Also when corvus-cli is only in an extra folder (Homebrew, not on a Finder app's PATH).
        XCTAssertEqual(SignalCli.autodetect(environment: ["PATH": a.path], extraDirs: [b.path]), b.appendingPathComponent("corvus-cli").path)
        try fm.removeItem(at: b.appendingPathComponent("corvus-cli"))
        XCTAssertEqual(SignalCli.autodetect(environment: ["PATH": a.path + ":" + b.path], extraDirs: []), "")
    }
}

final class LinkPreviewTests: XCTestCase {
    func testFirstHttpsLink() {
        XCTAssertEqual(LinkPreviewRules.firstHttpsLink("see https://example.com/a?b=1."), "https://example.com/a?b=1")
        XCTAssertEqual(LinkPreviewRules.firstHttpsLink("(https://en.wikipedia.org/wiki/Foo_(bar))"), "https://en.wikipedia.org/wiki/Foo_(bar)")
        XCTAssertEqual(LinkPreviewRules.firstHttpsLink("[https://example.com/x]"), "https://example.com/x")
        XCTAssertEqual(LinkPreviewRules.firstHttpsLink("http://example.com only"), "")
        XCTAssertEqual(LinkPreviewRules.firstHttpsLink("HTTPS://Example.com/Path"), "HTTPS://Example.com/Path") // spelling kept
        XCTAssertEqual(LinkPreviewRules.firstHttpsLink("no link"), "")
    }

    func testSafeToFetch() {
        XCTAssertTrue(LinkPreviewRules.isSafeToFetch(URL(string: "https://example.com/page")))
        for bad in ["http://example.com", "https://localhost/x", "https://printer.local", "https://intranet",
                    "https://svc.internal", "https://a.test", "https://127.0.0.1/", "https://10.0.0.1",
                    "https://[::1]/", "https://user:pw@example.com", "ftp://example.com"] {
            XCTAssertFalse(LinkPreviewRules.isSafeToFetch(URL(string: bad)), bad)
        }
        XCTAssertFalse(LinkPreviewRules.isSafeToFetch(nil))
    }

    func testParseHtml() {
        let html = """
        <html><head><title>Fallback &amp; title</title>
        <meta property="og:title" content="Caf&eacute; &quot;Raven&quot; &#8211; &#x1F426;">
        <meta name='description' content='Weekly &lt;news&gt;'>
        <meta content="/img/card.png" property="og:image">
        </head></html>
        """
        let (lp, image) = LinkPreviewRules.parse(html: html, url: "https://example.com/post")
        XCTAssertEqual(lp.title, "Caf&eacute; \"Raven\" – 🐦") // unknown named entities stay as they are
        XCTAssertEqual(lp.description, "Weekly <news>")
        XCTAssertEqual(image, "/img/card.png")
        XCTAssertTrue(lp.isValid)
        XCTAssertEqual(lp.host, "example.com")

        let plain = LinkPreviewRules.parse(html: "<TITLE>\n  Just   a\ttitle </TITLE>", url: "https://e.org")
        XCTAssertEqual(plain.preview.title, "Just a title")
        XCTAssertFalse(LinkPreviewRules.parse(html: "<p>nothing</p>", url: "https://e.org").preview.isValid)
        XCTAssertEqual(LinkPreviewRules.imageSuffix(contentType: "image/JPEG; charset=x"), "jpg")
        XCTAssertEqual(LinkPreviewRules.imageSuffix(contentType: "image/svg+xml"), "")
    }

    func testIncomingStoredEditedDeleted() {
        let pe = parseEnvelope(["sourceNumber": "+49alice", "dataMessage": [
            "timestamp": 10, "message": "look https://example.com",
            "previews": [["url": "http://insecure.example", "title": "no"] as JSON,
                         ["url": "https://example.com", "title": "Example", "description": "Desc",
                          "image": ["id": "img1", "contentType": "image/png", "size": 99] as JSON] as JSON],
        ] as JSON])
        XCTAssertEqual(pe.msg.linkPreview.url, "https://example.com") // the first valid (HTTPS) one
        XCTAssertEqual(pe.msg.linkPreview.imageId, "img1")

        var s = ChatState()
        s.account = "+49me"
        var m = pe.msg
        m.authorKey = "+49alice"
        s.addMessage("+49alice", m)
        // Stored with the message, and a late image as an update record (same keys as the Qt app).
        var withThumb = s.convs["+49alice"]!.msgs[0]
        withThumb.linkPreview.imageThumb = Data([1, 2, 3])
        var r = ChatState()
        r.loadHistory([Records.message("+49alice", m), Records.linkPreviewUpdate("+49alice", withThumb)])
        XCTAssertEqual(r.convs["+49alice"]!.msgs[0].linkPreview.title, "Example")
        XCTAssertEqual(r.convs["+49alice"]!.msgs[0].linkPreview.imageThumb, Data([1, 2, 3]))
        XCTAssertNil(jsonObject(Records.message("c", Message())["d"])["lp"]) // nothing written without a preview

        // An edit drops the preview (it belonged to the old text), in memory and in the history.
        let edit = s.applyEdit("+49alice", targetTs: 10, authors: ["+49alice"], text: "changed", newTs: 20)!
        XCTAssertFalse(s.convs["+49alice"]!.msgs[0].linkPreview.isValid)
        r.loadHistory([edit])
        XCTAssertFalse(r.convs["+49alice"]!.msgs[0].linkPreview.isValid)

        var d = m
        d.markDeleted()
        XCTAssertFalse(d.linkPreview.isValid)
    }
}

final class ChatOrderTests: XCTestCase {
    func testNewestFirstOrAlphabetical() {
        var s = ChatState()
        func add(_ key: String, _ title: String, _ order: Int64, archived: Bool = false) {
            var c = Conversation(key: key, title: title)
            var m = Message()
            m.ts = order
            m.orderTs = order
            c.msgs = [m]
            c.archived = archived
            s.convs[key] = c
        }
        add("+1", "zoe", 300)
        add("+2", "Ärger", 100)
        add("+3", "anna", 200)
        add("+4", "Bob", 400, archived: true)
        add("+5", "anna", 50)   // same name: the key keeps the order stable
        add("+6", "Chat 10", 10)
        add("+7", "Chat 9", 20)

        XCTAssertEqual(s.sortedConversations(archived: false).map(\.key), ["+1", "+3", "+2", "+5", "+7", "+6"])
        // Case and accents do not matter, numbers in names sort as numbers; the archive stays separate.
        XCTAssertEqual(s.sortedConversations(archived: false, alphabetical: true).map(\.title),
                       ["anna", "anna", "Ärger", "Chat 9", "Chat 10", "zoe"])
        XCTAssertEqual(s.sortedConversations(archived: false, alphabetical: true).prefix(2).map(\.key), ["+3", "+5"])
        XCTAssertEqual(s.sortedConversations(archived: true, alphabetical: true).map(\.key), ["+4"])

        // A new message moves a chat to the top only when sorting by newest.
        s.convs["+2"]!.msgs[0].orderTs = 999
        XCTAssertEqual(s.sortedConversations(archived: false).first?.key, "+2")
        XCTAssertEqual(s.sortedConversations(archived: false, alphabetical: true).first?.key, "+3")
    }
}

final class ChatNavigationTests: XCTestCase {
    func testStep() {
        XCTAssertEqual(ChatState.step(current: nil, count: 3, delta: 1), 0)   // nothing selected: first
        XCTAssertEqual(ChatState.step(current: nil, count: 3, delta: -1), 2)  // … or last
        XCTAssertEqual(ChatState.step(current: 0, count: 3, delta: 1), 1)
        XCTAssertEqual(ChatState.step(current: 2, count: 3, delta: -1), 1)
        XCTAssertNil(ChatState.step(current: 2, count: 3, delta: 1))          // no wrap-around
        XCTAssertNil(ChatState.step(current: 0, count: 3, delta: -1))
        XCTAssertNil(ChatState.step(current: 0, count: 0, delta: 1))
        XCTAssertEqual(ChatState.step(current: 7, count: 3, delta: 1), 0)     // stale index: like none
    }

    func testVisibleConversationsFilter() {
        var s = ChatState()
        for (k, t, o) in [("+1", "Zoe", Int64(3)), ("+2", "anna", 2), ("+493", "Annika", 1)] {
            var c = Conversation(key: k, title: t)
            var m = Message()
            m.orderTs = o
            c.msgs = [m]
            s.convs[k] = c
        }
        XCTAssertEqual(s.visibleConversations(archived: false, alphabetical: false, query: "").map(\.key), ["+1", "+2", "+493"])
        XCTAssertEqual(s.visibleConversations(archived: false, alphabetical: true, query: " ann ").map(\.key), ["+2", "+493"])
        XCTAssertEqual(s.visibleConversations(archived: false, alphabetical: false, query: "+49").map(\.key), ["+493"]) // by number too
    }
}

final class MentionComposeTests: XCTestCase {
    func testQuery() {
        XCTAssertEqual(MentionCompose.query(in: "hi @an", cursor: 6)?.start, 3)
        XCTAssertEqual(MentionCompose.query(in: "hi @an", cursor: 6)?.query, "an")
        XCTAssertEqual(MentionCompose.query(in: "@", cursor: 1)?.query, "")         // just typed "@": everybody
        XCTAssertNil(MentionCompose.query(in: "mail@host", cursor: 9))              // e-mail address, not a mention
        XCTAssertNil(MentionCompose.query(in: "a_@b", cursor: 4))
        XCTAssertNil(MentionCompose.query(in: "@an na", cursor: 6))                 // a space ends it
        XCTAssertNil(MentionCompose.query(in: "no at", cursor: 5))
        XCTAssertEqual(MentionCompose.query(in: "😀 @x", cursor: 5)?.start, 3)      // UTF-16 offsets
        XCTAssertNil(MentionCompose.query(in: "@" + String(repeating: "a", count: 49), cursor: 50))
    }

    func testAdjust() {
        let m = ComposerMention(start: 4, length: 5, id: "+1", name: "Alice") // "hi @Alice …"
        // typing in front (also right before the "@") moves it
        XCTAssertEqual(MentionCompose.adjust([m], editing: NSRange(location: 0, length: 0), replacementLength: 2).first?.start, 6)
        XCTAssertEqual(MentionCompose.adjust([m], editing: NSRange(location: 3, length: 0), replacementLength: 1).first?.start, 5)
        // typing after it leaves it alone
        XCTAssertEqual(MentionCompose.adjust([m], editing: NSRange(location: 9, length: 0), replacementLength: 3), [m])
        // touching the name or its "@" makes it plain text
        XCTAssertTrue(MentionCompose.adjust([m], editing: NSRange(location: 6, length: 1), replacementLength: 0).isEmpty)
        XCTAssertTrue(MentionCompose.adjust([m], editing: NSRange(location: 3, length: 1), replacementLength: 0).isEmpty)
    }

    func testPrepareLikeQt() {
        // "  Hi @Alice and @Bob!  " -> "Hi Alice and Bob!" with the names at their new places
        let raw = "  Hi @Alice and @Bob!  "
        let mentions = [ComposerMention(start: 6, length: 5, id: "+1", name: "Alice"),
                        ComposerMention(start: 17, length: 3, id: "u-2", name: "Bob")]
        let p = MentionCompose.prepare(raw, mentions: mentions)
        XCTAssertEqual(p.text, "Hi Alice and Bob!")
        XCTAssertEqual(MentionCompose.cliArguments(p.mentions), ["3:5:+1", "13:3:u-2"])

        // A mention whose text was changed in the meantime is sent as plain text.
        let stale = MentionCompose.prepare("Hi @Alicia", mentions: [ComposerMention(start: 4, length: 5, id: "+1", name: "Alice")])
        XCTAssertEqual(stale.text, "Hi @Alicia")
        XCTAssertTrue(stale.mentions.isEmpty)

        // Emoji before a mention: offsets stay in UTF-16 units.
        let emoji = MentionCompose.prepare("😀 @Bo", mentions: [ComposerMention(start: 4, length: 2, id: "+2", name: "Bo")])
        XCTAssertEqual(emoji.text, "😀 Bo")
        XCTAssertEqual(MentionCompose.cliArguments(emoji.mentions), ["3:2:+2"])
        XCTAssertEqual(MentionCompose.prepare("   ", mentions: []).text, "")
    }
}

/// The Swift app shares history.corvus with the Qt app: records written by src/Records.cpp
/// must survive a load + compaction here unchanged.
final class QtRecordParityTests: XCTestCase {
    /// Records exactly as the Qt app writes them (tags from src/Records.cpp).
    func qtRecords() -> [JSON] {
        [
            ["t": "b", "c": "+49eve", "v": true],
            ["t": "m", "c": "+49eve", "ts": 50, "s": "Eve", "x": "hi", "o": false, "au": "+49eve"],
            ["t": "m", "c": "g:G1", "ts": 100, "s": "Alice", "x": "bold \u{FFFC}", "o": false, "au": "+49alice",
             "d": ["st": ["0:4:BOLD", "5:1:SPOILER"], "mn": [["s": 5, "l": 1, "u": "uuid-bob"]]] as JSON],
            ["t": "m", "c": "g:G1", "ts": 200, "s": "You", "x": "", "o": true, "au": "+49me",
             "d": ["pl": ["q": "Lunch?", "m": true, "o": ["Pizza", "Sushi", "Salad"]] as JSON,
                   "rs": 2,
                   "rr": ["+49alice": ["s": 2, "d": 210, "r": 220] as JSON, "+49bob": ["s": 1, "d": 215, "r": 0] as JSON] as JSON] as JSON],
            ["t": "pv", "c": "g:G1", "ts": 200, "au": "+49me", "by": "+49alice", "oi": [0, 2], "vc": 1, "vt": 230],
            ["t": "pv", "c": "g:G1", "ts": 200, "au": "+49me", "by": "+49alice", "oi": [1], "vc": 2, "vt": 240],
            ["t": "pv", "c": "g:G1", "ts": 200, "au": "+49me", "by": "+49bob", "oi": [0], "vc": 1, "vt": 235],
            ["t": "u", "c": "g:G1", "ts": 200, "au": "+49me", "d": ["pt": true] as JSON],
            ["t": "m", "c": "g:G1", "ts": 300, "s": "Bob", "x": "", "o": false, "au": "+49bob",
             "d": ["cc": ["n": "Carol", "num": "+49carol", "u": "uuid-carol", "av": "avatar1"] as JSON] as JSON],
            ["t": "m", "c": "g:G1", "ts": 400, "s": "Bob", "x": "", "o": false, "au": "+49bob",
             "d": ["vo": true, "at": [["id": "vo1", "ct": "image/jpeg"] as JSON]] as JSON],
            ["t": "m", "c": "g:G1", "ts": 500, "s": "Bob", "x": "", "o": false, "au": "+49bob",
             "d": ["vo": true, "at": [["id": "vo2", "ct": "image/jpeg"] as JSON]] as JSON],
            ["t": "u", "c": "g:G1", "ts": 500, "au": "+49bob", "d": ["vv": true, "ve": false, "at": [Any]()] as JSON],
            ["t": "pi", "c": "g:G1", "ts": 100, "au": "+49alice", "by": "+49bob", "at": nowMs(), "d": -1],
            ["t": "pi", "c": "g:G1", "ts": 300, "au": "+49bob", "by": "+49me", "at": 1, "d": 86400], // long expired
            ["t": "j", "c": "+49stranger"],
        ]
    }

    func testLoadQtRecords() {
        var s = ChatState()
        s.account = "+49me"
        s.loadHistory(qtRecords())

        XCTAssertTrue(s.convs["+49eve"]!.blocked)
        XCTAssertEqual(s.declined, ["+49stranger"])
        XCTAssertNil(s.convs["+49stranger"])

        let g = s.convs["g:G1"]!
        let styled = g.msgs[0]
        XCTAssertEqual(styled.textStyles, [TextStyleRange(start: 0, length: 4, style: "BOLD"),
                                           TextStyleRange(start: 5, length: 1, style: "SPOILER")])
        XCTAssertEqual(styled.mentions, [MentionRange(start: 5, length: 1, uuid: "uuid-bob")])

        let poll = g.msgs[1]
        XCTAssertTrue(poll.hasPoll)
        XCTAssertEqual(poll.poll.options, ["Pizza", "Sushi", "Salad"])
        XCTAssertTrue(poll.poll.allowMultiple)
        XCTAssertTrue(poll.poll.terminated)
        XCTAssertEqual(poll.poll.voteOf("+49alice"), [1]) // the later vote (vc 2) wins
        XCTAssertEqual(poll.poll.optionCounts, [1, 1, 0])
        XCTAssertEqual(poll.recipientReceipts["+49alice"], RecipientReceipt(status: 2, deliveredAtMs: 210, readAtMs: 220))
        XCTAssertEqual(poll.receipt, .read)

        let card = g.msgs[2]
        XCTAssertTrue(card.hasContact)
        XCTAssertEqual(card.contact.name, "Carol")
        XCTAssertEqual(card.contact.number, "+49carol")
        XCTAssertEqual(card.contact.avatarId, "avatar1")

        XCTAssertTrue(g.msgs[3].viewOnce)
        XCTAssertEqual(g.msgs[3].attachments.map(\.id), ["vo1"])
        XCTAssertTrue(g.msgs[4].viewOnceViewed)
        XCTAssertTrue(g.msgs[4].attachments.isEmpty, "an opened View Once item keeps no attachment")

        XCTAssertEqual(g.pins.map(\.ts), [100], "the expired 24 h pin is dropped")
    }

    func testCompactionKeepsQtFields() {
        var s = ChatState()
        s.account = "+49me"
        s.loadHistory(qtRecords())
        let compacted = s.allRecords()

        var r = ChatState()
        r.account = "+49me"
        r.loadHistory(compacted)
        XCTAssertEqual(r.convs["g:G1"]!.msgs, s.convs["g:G1"]!.msgs)
        XCTAssertEqual(r.convs["g:G1"]!.pins, s.convs["g:G1"]!.pins)
        XCTAssertTrue(r.convs["+49eve"]!.blocked)
        XCTAssertEqual(r.declined, ["+49stranger"])

        // Same tags as the Qt writer.
        let pollRec = compacted.first { jsonString($0["t"]) == "m" && jsonInt64($0["ts"]) == 200 }!
        let d = jsonObject(pollRec["d"])
        XCTAssertEqual(jsonBool(d["pt"]), true)
        XCTAssertEqual(jsonObject(d["pl"])["q"] as? String, "Lunch?")
        XCTAssertNotNil(jsonObject(d["rr"])["+49bob"])
        XCTAssertEqual(compacted.filter { jsonString($0["t"]) == "pv" }.count, 2)
        XCTAssertEqual(compacted.filter { jsonString($0["t"]) == "pi" }.count, 1)
        let styled = jsonObject(compacted.first { jsonInt64($0["ts"]) == 100 && jsonString($0["t"]) == "m" }!["d"])
        XCTAssertEqual(styled["st"] as? [String], ["0:4:BOLD", "5:1:SPOILER"])
        // A viewed View Once item is written without its attachment reference.
        let viewed = jsonObject(compacted.first { jsonInt64($0["ts"]) == 500 && jsonString($0["t"]) == "m" }!["d"])
        XCTAssertNil(viewed["at"])
        XCTAssertEqual(jsonBool(viewed["vv"]), true)
    }

    /// The Rust store (history-store) must accept every Qt record shape the Swift app now writes.
    func testRustStoreAcceptsQtRecords() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let path = dir.appendingPathComponent("history.corvus").path
        var s = ChatState()
        s.account = "+49me"
        s.loadHistory(qtRecords())
        let records = s.allRecords()
        let store = HistoryStore()
        try store.create(path: path, passphrase: "pw")
        for r in records { try store.append(r) }
        try store.rewrite(records)
        store.close()
        var r = ChatState()
        r.account = "+49me"
        r.loadHistory(try HistoryStore().open(path: path, passphrase: "pw"))
        XCTAssertEqual(r.convs["g:G1"]!.msgs, s.convs["g:G1"]!.msgs)
        XCTAssertEqual(r.convs["g:G1"]!.pins, s.convs["g:G1"]!.pins)
        XCTAssertEqual(r.declined, s.declined)
        XCTAssertTrue(r.convs["+49eve"]!.blocked)
    }

    func testDeletionClearsQtFields() {
        var m = Message()
        m.hasPoll = true
        m.poll.options = ["a", "b"]
        m.hasContact = true
        m.textStyles = [TextStyleRange(start: 0, length: 1, style: "BOLD")]
        Records.applyExtras(&m, ["dl": true])
        XCTAssertFalse(m.hasPoll)
        XCTAssertFalse(m.hasContact)
        XCTAssertTrue(m.textStyles.isEmpty)
    }

    func testThumbnailsAreCappedPerRecord() {
        var m = Message()
        for i in 0..<3 {
            var a = Attachment()
            a.id = "a\(i)"
            a.thumb = Data(count: 4 * 1024 * 1024) // ~5.3 MiB as base64
            m.attachments.append(a)
        }
        let at = jsonArray(jsonObject(Records.message("+49x", m)["d"])["at"]).map { jsonObject($0) }
        XCTAssertEqual(at.count, 3)
        XCTAssertNotNil(at[0]["th"])
        XCTAssertNil(at[1]["th"])
        XCTAssertNil(at[2]["th"])
    }
}

final class TextStyleTests: XCTestCase {
    let bold = { (s: Int, l: Int) in TextStyleRange(start: s, length: l, style: "BOLD") }

    func testEnvelopeStylesForMessagesAndEdits() {
        let msg: JSON = ["sourceNumber": "+49alice", "timestamp": 10,
                         "dataMessage": ["timestamp": 10, "message": "hello world",
                                         "textStyle": ["0:5:BOLD", "6:5:SPOILER", "bad"]] as JSON]
        XCTAssertEqual(parseEnvelope(msg).msg.textStyles,
                       [bold(0, 5), TextStyleRange(start: 6, length: 5, style: "SPOILER")])
        let edit: JSON = ["sourceNumber": "+49alice", "timestamp": 20,
                          "editMessage": ["targetSentTimestamp": 10,
                                          "dataMessage": ["timestamp": 20, "message": "hi",
                                                          "textStyle": ["0:2:ITALIC"]] as JSON] as JSON]
        let pe = parseEnvelope(edit)
        XCTAssertEqual(pe.kind, .edit)
        XCTAssertEqual(pe.msg.textStyles, [TextStyleRange(start: 0, length: 2, style: "ITALIC")])
    }

    func testAdjustFollowsTheQtRules() {
        let styles = [bold(2, 3)] // "ab[cde]f"
        // typing in front of or right after the run does not take its style
        XCTAssertEqual(TextStyles.adjust(styles, editing: NSRange(location: 2, length: 0), replacementLength: 1), [bold(3, 3)])
        XCTAssertEqual(TextStyles.adjust(styles, editing: NSRange(location: 5, length: 0), replacementLength: 1), [bold(2, 3)])
        // typing inside extends it
        XCTAssertEqual(TextStyles.adjust(styles, editing: NSRange(location: 3, length: 0), replacementLength: 2), [bold(2, 5)])
        // deleting part of it shrinks it, deleting all of it drops it
        XCTAssertEqual(TextStyles.adjust(styles, editing: NSRange(location: 1, length: 2), replacementLength: 0), [bold(1, 2)])
        XCTAssertEqual(TextStyles.adjust(styles, editing: NSRange(location: 2, length: 3), replacementLength: 0), [])
        // replacing selected styled text keeps the style for the new text
        XCTAssertEqual(TextStyles.adjust(styles, editing: NSRange(location: 2, length: 3), replacementLength: 1), [bold(2, 1)])
        // edits before it move it
        XCTAssertEqual(TextStyles.adjust(styles, editing: NSRange(location: 0, length: 1), replacementLength: 0), [bold(1, 3)])
    }

    func testToggle() {
        var s = TextStyles.toggle([], NSRange(location: 0, length: 4), style: "BOLD")
        XCTAssertEqual(s, [bold(0, 4)])
        s = TextStyles.toggle(s, NSRange(location: 2, length: 4), style: "BOLD") // partly bold -> all bold, merged
        XCTAssertEqual(s, [bold(0, 6)])
        s = TextStyles.toggle(s, NSRange(location: 2, length: 2), style: "BOLD") // fully bold -> off, split
        XCTAssertEqual(s, [bold(0, 2), bold(4, 2)])
        s = TextStyles.toggle(s, NSRange(location: 0, length: 6), style: "ITALIC")
        XCTAssertEqual(s.filter { $0.style == "ITALIC" }, [TextStyleRange(start: 0, length: 6, style: "ITALIC")])
        XCTAssertTrue(TextStyles.covers(s, NSRange(location: 1, length: 1), style: "BOLD"))
        XCTAssertFalse(TextStyles.covers(s, NSRange(location: 1, length: 3), style: "BOLD"))
    }

    func testMapTrimsAndDropsMentionAtSigns() {
        // "  hi @Bob bold  " with "bold" bold and a mention of Bob
        let raw = "  hi @Bob bold  "
        let mention = ComposerMention(start: 6, length: 3, id: "uuid-bob", name: "Bob")
        let styles = [bold(10, 4)]
        XCTAssertEqual(TextStyles.map(styles, in: raw), [bold(8, 4)]) // stored text: "hi @Bob bold"
        let removeAt = MentionCompose.removedAtSigns(raw, mentions: [mention])
        XCTAssertEqual(removeAt, [5])
        let prepared = MentionCompose.prepare(raw, mentions: [mention])
        let cli = TextStyles.map(styles, in: raw, removeAt: removeAt)
        XCTAssertEqual(prepared.text, "hi Bob bold")
        XCTAssertEqual((prepared.text as NSString).substring(with: NSRange(location: cli[0].start, length: cli[0].length)), "bold")
    }

    func testResolveMentionsMovesStyles() {
        // "hey \u{FFFC} you": "\u{FFFC} you" bold
        let r = TextStyles.resolveMentions("hey \u{FFFC} you", [(start: 4, length: 1, name: "Alice")], styles: [bold(4, 5)])
        XCTAssertEqual(r.text, "hey @Alice you")
        XCTAssertEqual(r.styles, [bold(4, 10)])
        let after = TextStyles.resolveMentions("\u{FFFC} x", [(start: 0, length: 1, name: "Al")], styles: [bold(2, 1)])
        XCTAssertEqual(after.text, "@Al x")
        XCTAssertEqual(after.styles, [bold(4, 1)])
    }
}

final class SafetyNumberTests: XCTestCase {
    let sixty = "123456789012345678901234567890123456789012345678901234567890"

    func identity(_ digits: String, trust: String = "TRUSTED_UNVERIFIED", number: String = "+49a", added: Int64 = 1) -> SafetyNumber.Identity {
        var id = SafetyNumber.Identity()
        id.number = number
        id.safetyNumber = digits
        id.trustLevel = trust
        id.addedMs = added
        return id
    }

    func testFormatAndMatch() {
        XCTAssertEqual(SafetyNumber.format(sixty),
                       "12345 67890 12345 67890\n12345 67890 12345 67890\n12345 67890 12345 67890")
        XCTAssertEqual(SafetyNumber.format("123"), "123")
        XCTAssertTrue(SafetyNumber.matches(SafetyNumber.format(sixty), sixty))
        XCTAssertFalse(SafetyNumber.matches(String(sixty.dropLast()), String(sixty.dropLast())))
    }

    func testChangeDetection() {
        XCTAssertFalse(SafetyNumber.changed(stored: "", now: identity(sixty)))          // first sight: baseline
        XCTAssertTrue(SafetyNumber.changed(stored: "", now: identity(sixty, trust: "UNTRUSTED")))
        XCTAssertFalse(SafetyNumber.changed(stored: sixty, now: identity(sixty)))
        let other = String(sixty.reversed())
        XCTAssertTrue(SafetyNumber.changed(stored: sixty, now: identity(other)))
        XCTAssertFalse(SafetyNumber.changed(stored: sixty, now: identity("12")))         // no usable number
    }

    func testPickPrefersOwnNewestIdentity() {
        let other = String(sixty.reversed())
        let list = [identity(sixty, number: "+49x", added: 9), identity(sixty, added: 1), identity(other, added: 5)]
        XCTAssertEqual(SafetyNumber.pick(list, recipient: "+49a")?.digits, other)
        XCTAssertNil(SafetyNumber.pick([identity("1")], recipient: "+49a"))
    }

    func testSendFailures() {
        XCTAssertTrue(SafetyNumber.isUntrustedIdentityError("Failed to send message: Untrusted Identity for \"+49\""))
        let result: JSON = ["timestamp": 1, "results": [
            ["type": "SUCCESS", "recipientAddress": ["number": "+49a"]] as JSON,
            ["type": "IDENTITY_FAILURE", "recipientAddress": ["uuid": "u-b"]] as JSON,
        ]]
        XCTAssertEqual(SafetyNumber.identityFailures(in: result), ["u-b"])
    }
}

final class GalleryTests: XCTestCase {
    func testItemsByTab() {
        func att(_ name: String, _ ct: String) -> Attachment { var a = Attachment(); a.filename = name; a.contentType = ct; return a }
        var m1 = Message(); m1.ts = 1; m1.text = "see https://example.com/a and http://x.org"
        m1.attachments = [att("a.jpg", "image/jpeg"), att("v.mp4", "video/mp4"), att("s.mp3", "audio/mpeg"), att("doc.pdf", "application/pdf")]
        var m2 = Message(); m2.ts = 2; m2.attachments = [Attachment.sticker(packId: "p", stickerId: 1)]
        var m3 = Message(); m3.ts = 3; m3.viewOnce = true; m3.attachments = [att("o.jpg", "image/jpeg")]
        var m4 = Message(); m4.ts = 4; m4.linkPreview = LinkPreview(url: "https://p.example", title: "P")
        let items = Gallery.items([m1, m2, m3, m4])
        XCTAssertEqual(items.filter { $0.tab == .media }.map(\.attachment.filename), ["a.jpg", "v.mp4"])
        XCTAssertEqual(items.filter { $0.tab == .audio }.count, 1)
        XCTAssertEqual(items.filter { $0.tab == .files }.map(\.attachment.filename), ["doc.pdf"])
        XCTAssertEqual(items.filter { $0.tab == .links }.map(\.url), ["https://p.example", "https://example.com/a", "http://x.org"])
        XCTAssertEqual(items.first { $0.tab == .links }?.title, "P")
    }
}
