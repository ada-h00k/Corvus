import SwiftUI

/// resources/emoji-catalog.json ({category: [{emoji, tones}]}) and emoji-names.json ({emoji: name}).
@MainActor
enum EmojiCatalog {
    struct Entry: Hashable {
        let emoji: String
        let tones: [String]
    }

    static let quickReactions = ["👍", "❤️", "😂", "😮", "😢", "🙏"]

    static let categories: [(key: String, icon: String)] = [
        ("Smileys & People", "😀"), ("Animals & Nature", "🐻"), ("Food & Drink", "🍕"), ("Travel & Places", "🚗"),
        ("Activities", "⚽"), ("Objects", "💡"), ("Symbols", "🔣"), ("Flags", "🏁"),
    ]

    static let catalog: [String: [Entry]] = {
        guard let url = AppResources.url("emoji-catalog", "json"), let data = try? Data(contentsOf: url),
              let root = (try? JSONSerialization.jsonObject(with: data)) as? [String: [[String: Any]]] else { return [:] }
        return root.mapValues { list in
            list.compactMap { o in
                guard let e = o["emoji"] as? String else { return nil }
                return Entry(emoji: e, tones: o["tones"] as? [String] ?? [])
            }
        }
    }()

    static let names: [String: String] = {
        guard let url = AppResources.url("emoji-names", "json"), let data = try? Data(contentsOf: url) else { return [:] }
        return (try? JSONSerialization.jsonObject(with: data)) as? [String: String] ?? [:]
    }()

    private static let recentKey = "emoji.recent"

    static var recent: [String] { UserDefaults.standard.stringArray(forKey: recentKey) ?? [] }

    static func remember(_ e: String) {
        var r = recent.filter { $0 != e }
        r.insert(e, at: 0)
        UserDefaults.standard.set(Array(r.prefix(16)), forKey: recentKey)
    }
}

/// Recently used emojis, search by name and the categories, like the official Signal client.
struct EmojiPicker: View {
    let onPick: (String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var search = ""
    @State private var category = EmojiCatalog.categories[0].key
    @State private var recent = EmojiCatalog.recent

    private let columns = Array(repeating: GridItem(.fixed(34), spacing: 2), count: 9)

    var body: some View {
        VStack(spacing: 6) {
            TextField("Search emojis…", text: $search)
                .textFieldStyle(.roundedBorder)
            if !recent.isEmpty && search.isEmpty {
                HStack(spacing: 2) {
                    ForEach(recent.prefix(9), id: \.self) { e in button(EmojiCatalog.Entry(emoji: e, tones: [])) }
                    Spacer(minLength: 0)
                }
                Divider()
            }
            ScrollView {
                LazyVGrid(columns: columns, spacing: 2) {
                    ForEach(entries, id: \.self) { button($0) }
                }
            }
            .frame(height: 250)
            if search.isEmpty {
                HStack(spacing: 2) {
                    ForEach(EmojiCatalog.categories, id: \.key) { c in
                        Button { category = c.key } label: {
                            Text(c.icon).font(.title3).frame(width: 32, height: 28)
                                .background(RoundedRectangle(cornerRadius: 5).fill(category == c.key ? Color.accentColor.opacity(0.25) : .clear))
                        }
                        .buttonStyle(.plain)
                        .help(c.key)
                    }
                }
            }
        }
        .padding(8)
        .frame(width: 340)
    }

    private var entries: [EmojiCatalog.Entry] {
        let q = search.trimmingCharacters(in: .whitespaces).lowercased()
        if q.isEmpty { return EmojiCatalog.catalog[category] ?? [] }
        var out: [EmojiCatalog.Entry] = []
        for c in EmojiCatalog.categories {
            for e in EmojiCatalog.catalog[c.key] ?? [] where (EmojiCatalog.names[e.emoji] ?? "").lowercased().contains(q) {
                out.append(e)
            }
        }
        return out
    }

    private func button(_ e: EmojiCatalog.Entry) -> some View {
        Button {
            pick(e.emoji)
        } label: {
            Text(e.emoji).font(.system(size: 22)).frame(width: 34, height: 32)
        }
        .buttonStyle(.plain)
        .help(EmojiCatalog.names[e.emoji] ?? "")
        .contextMenu {
            ForEach(e.tones, id: \.self) { t in Button(t) { pick(t) } }
        }
    }

    private func pick(_ e: String) {
        EmojiCatalog.remember(e)
        recent = EmojiCatalog.recent
        onPick(e)
        dismiss()
    }
}
