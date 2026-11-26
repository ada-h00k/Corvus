import CorvusCore
import SwiftUI

/// The dialogs of `ChatSheet`.
struct ChatSheetView: View {
    @Environment(AppModel.self) private var model
    let sheet: ChatSheet

    var body: some View {
        switch sheet {
        case .messageDetails(let id): MessageDetailsView(messageId: id)
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
