import CoreImage.CIFilterBuiltins
import CorvusCore
import SwiftUI

/// The dialogs of `ChatSheet`.
struct ChatSheetView: View {
    @Environment(AppModel.self) private var model
    let sheet: ChatSheet

    var body: some View {
        switch sheet {
        case .messageDetails(let id): MessageDetailsView(messageId: id)
        case .safetyNumber(let key): SafetyNumberView(key: key)
        case .forward(let id): ForwardView(messageId: id)
        }
    }
}

/// "Message details": when a message was sent, and for your own messages when each recipient got and read it.
struct MessageDetailsView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let messageId: String

    private struct Row: Identifiable {
        let id: String
        let name: String
        let status: String
        let delivered: String
        let read: String
    }

    private static func when(_ ms: Int64) -> String {
        ms <= 0 ? "—" : Date(timeIntervalSince1970: TimeInterval(ms) / 1000).formatted(date: .long, time: .standard)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Message details").font(.headline)
            if let c = model.currentConversation, let (_, m) = model.message(withId: messageId) {
                Text("Message timestamp: \(m.time.formatted(date: .long, time: .standard))").textSelection(.enabled)
                if m.outgoing {
                    let readShown = model.privacy.sendReadReceipts
                    let rows = model.detailRecipients(m, in: c).map { key -> Row in
                        let r = m.recipientReceipts[key] ?? RecipientReceipt()
                        let status = r.status >= 2 && readShown ? "Read" : (r.status >= 1 ? "Delivered" : "Sent")
                        return Row(id: key, name: model.displayName(key), status: status,
                                   delivered: Self.when(r.deliveredAtMs), read: readShown ? Self.when(r.readAtMs) : "—")
                    }
                    if rows.isEmpty {
                        Text("No recipient status is available yet.").foregroundStyle(.secondary)
                    } else {
                        Table(rows) {
                            TableColumn("Recipient", value: \.name)
                            TableColumn("Status", value: \.status).width(min: 70, ideal: 80)
                            TableColumn("Delivered at", value: \.delivered)
                            TableColumn("Read at", value: \.read)
                        }
                        .frame(minHeight: min(260, CGFloat(30 + rows.count * 26)))
                    }
                    if !readShown {
                        Text("Read status is hidden because Send read receipts is turned off.")
                            .font(.callout).foregroundStyle(.secondary)
                    }
                } else {
                    Text("Received from: \(m.sender.isEmpty ? model.displayName(m.authorKey) : m.sender)")
                        .textSelection(.enabled)
                }
            } else {
                Text("This message is no longer available.").foregroundStyle(.secondary)
            }
            HStack {
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(minWidth: 560)
    }
}

/// "Message requests": people who wrote to you but are not accepted yet (port of the Qt dialog).
struct MessageRequestsView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var busy: Set<String> = []
    @State private var error = ""

    var body: some View {
        let requests = model.messageRequests
        VStack(alignment: .leading, spacing: 12) {
            Text("Message requests").font(.headline)
            Text("These people sent you a message but are not accepted yet. Accept to chat with them, decline to hide the request, or block them.")
                .fixedSize(horizontal: false, vertical: true)
                .foregroundStyle(.secondary)
            List(requests, id: \.key) { r in
                HStack {
                    Text(verbatim: r.title).lineLimit(1).help(r.key) // a stranger's profile name: plain text
                    Spacer()
                    Button("Accept") { run(r.key) { try await model.acceptRequest(r.key); dismiss() } }
                        .help("Accept the message request and show the conversation")
                    Button("Decline") { model.declineRequest(r.key) }
                        .help("Keep the conversation hidden and remove the request")
                    Button("Block …", role: .destructive) {
                        Task {
                            guard await Dialogs.confirm("Block contact", "Block \(r.title)? You will no longer receive messages or calls from this contact.",
                                                        ok: "Block", destructive: true) else { return }
                            run(r.key) { try await model.blockRequest(r.key) }
                        }
                    }
                    .help("Block this contact and remove the request")
                }
                .disabled(busy.contains(r.key) || !model.connected)
            }
            .frame(minHeight: 160)
            if !error.isEmpty { Text(error).foregroundStyle(.red).font(.callout) }
            HStack {
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding(20)
        .frame(minWidth: 480)
        .onChange(of: requests.isEmpty) { _, empty in if empty { dismiss() } } // nothing left to do
    }

    private func run(_ key: String, _ action: @escaping () async throws -> Void) {
        busy.insert(key)
        error = ""
        Task {
            do { try await action() } catch { self.error = "Could not complete the request: \(error.localizedDescription)" }
            busy.remove(key)
        }
    }
}

/// "Safety number with …": compare the 60 digits (or the QR code) with the contact, then mark them as verified.
struct SafetyNumberView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let key: String
    @State private var identity: SafetyNumber.Identity?
    @State private var status = "Loading …"
    @State private var statusIsError = false
    @State private var compare = ""
    @State private var busy = false

    private var name: String { model.displayName(key) }
    private var recipient: String { model.chats.resolveKey(key) }

    var body: some View {
        VStack(spacing: 12) {
            Text("Safety number with \(name)").font(.headline)
            Text("To check that your messages with \(name) are end-to-end encrypted, compare this number with the one on their device, in person or over a channel you trust. If the numbers match, mark the contact as verified.")
                .font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
            if let id = identity {
                if let qr = Self.qrImage(SafetyNumber.scannableBytes(id.scannable)) {
                    Image(nsImage: qr).interpolation(.none).resizable().frame(width: 180, height: 180)
                        .help("Scan this code with Signal on \(name)'s phone (conversation → View safety number → Scan).")
                }
                Text(SafetyNumber.format(id.safetyNumber))
                    .font(.system(.title3, design: .monospaced))
                    .multilineTextAlignment(.center)
                    .textSelection(.enabled)
                Text("Status: \(SafetyNumber.trustLabel(id.trustLevel))").foregroundStyle(id.isVerified ? .green : .secondary)
                VStack(alignment: .leading, spacing: 4) {
                    Text("Optional: paste the number from their device to compare it here:").font(.callout)
                    TextField("60 digits", text: $compare).textFieldStyle(.roundedBorder)
                    compareResult(id)
                }
            }
            if !status.isEmpty { Text(status).font(.callout).foregroundStyle(statusIsError ? .red : .secondary) }
            HStack {
                Button("Copy number") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(identity?.digits ?? "", forType: .string)
                }
                .disabled(identity == nil)
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.cancelAction)
                Button(identity?.isVerified == true ? "Verified ✓" : "Mark as verified") { Task { await verify() } }
                    .disabled(!canVerify)
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 460)
        .task { await reload() }
    }

    private var typed: String { SafetyNumber.digitsOnly(compare) }

    private var canVerify: Bool {
        guard let id = identity, !busy, !id.isVerified else { return false }
        return typed.isEmpty || SafetyNumber.matches(id.safetyNumber, typed)
    }

    @ViewBuilder private func compareResult(_ id: SafetyNumber.Identity) -> some View {
        if typed.isEmpty {
            EmptyView()
        } else if SafetyNumber.matches(id.safetyNumber, typed) {
            Text("✓ The numbers match.").foregroundStyle(.green)
        } else if typed.count < SafetyNumber.digitCount {
            Text("\(typed.count) of \(SafetyNumber.digitCount) digits entered.").foregroundStyle(.secondary)
        } else {
            Text("✗ The numbers do NOT match. Do not verify this contact.").bold().foregroundStyle(.red)
        }
    }

    private func reload() async {
        do {
            let res = try await model.cli.call("listIdentities", ["number": recipient])
            identity = SafetyNumber.pick(SafetyNumber.parse(res), recipient: recipient)
            status = identity == nil ? "No safety number is available for this contact yet. It appears after you have exchanged messages." : ""
            statusIsError = false
        } catch {
            status = "Could not load the safety number: \(error.localizedDescription)"
            statusIsError = true
        }
    }

    private func verify() async {
        guard let id = identity, canVerify else { return }
        // Without a pasted number the user compared by eye: ask once.
        if typed.isEmpty {
            guard await Dialogs.confirm("Mark as verified",
                                        "Did you compare this safety number with the one on \(name)'s device, and do both match?",
                                        ok: "Mark as verified") else { return }
        }
        busy = true
        status = "Saving …"
        defer { busy = false }
        do {
            _ = try await model.cli.call("trust", ["recipient": recipient, "verifiedSafetyNumber": id.digits])
            model.safetyVerified(key, recipient: recipient, digits: id.digits)
            await reload()
        } catch {
            status = "Could not mark the contact as verified: \(error.localizedDescription)"
            statusIsError = true
        }
    }

    /// The QR code of the official apps: the raw bytes, in byte mode (Core Image, no qrencode needed).
    static func qrImage(_ data: Data) -> NSImage? {
        guard !data.isEmpty else { return nil }
        let f = CIFilter.qrCodeGenerator()
        f.message = data
        f.correctionLevel = "L"
        guard let out = f.outputImage?.transformed(by: CGAffineTransform(scaleX: 6, y: 6)) else { return nil }
        let rep = NSCIImageRep(ciImage: out)
        let img = NSImage(size: rep.size)
        img.addRepresentation(rep)
        return img
    }
}

/// "Forward message": up to five chats and an optional note (port of the Qt dialog).
struct ForwardView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let messageId: String
    @State private var query = ""
    @State private var selected: [String] = []
    @State private var note = ""

    var body: some View {
        let q = query.trimmingCharacters(in: .whitespaces)
        let list = model.forwardDestinations.filter {
            q.isEmpty || $0.title.localizedCaseInsensitiveContains(q) || $0.key.localizedCaseInsensitiveContains(q)
        }
        VStack(alignment: .leading, spacing: 10) {
            Text("Forward message").font(.headline)
            Text("Choose up to \(AppModel.maxForwardTargets) chats to forward this message to.").foregroundStyle(.secondary)
            TextField("Search chats", text: $query).textFieldStyle(.roundedBorder)
            List(list) { c in
                let on = selected.contains(c.key)
                Button {
                    if on { selected.removeAll { $0 == c.key } }
                    else if selected.count < AppModel.maxForwardTargets { selected.append(c.key) }
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: on ? "checkmark.circle.fill" : "circle").foregroundStyle(on ? Theme.signalBlue : .secondary)
                        AvatarView(image: model.avatarImage(c), size: 24)
                        Text(verbatim: c.title.isEmpty ? c.key : c.title).lineLimit(1)
                        if c.isGroup { Image(systemName: "person.3").foregroundStyle(.secondary).font(.caption) }
                        Spacer()
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(!on && selected.count >= AppModel.maxForwardTargets)
                .help(c.key)
            }
            .frame(minHeight: 260)
            Text(selected.count >= AppModel.maxForwardTargets && selected.count > 0
                 ? "\(selected.count) of \(AppModel.maxForwardTargets) chats selected (the maximum)."
                 : "\(selected.count) of \(AppModel.maxForwardTargets) chats selected")
                .font(.callout).foregroundStyle(.secondary)
            TextField("Add a message (optional)", text: $note).textFieldStyle(.roundedBorder)
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Forward") {
                    let targets = selected, text = note, id = messageId
                    dismiss()
                    Task { await model.forward(id, to: targets, note: text) }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(selected.isEmpty || !model.connected)
            }
        }
        .padding(20)
        .frame(width: 440, height: 560)
    }
}
