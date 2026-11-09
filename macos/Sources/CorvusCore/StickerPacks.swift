import Foundation

/// An installed sticker pack as signal-cli's "listStickerPacks" reports it.
public struct StickerPack: Identifiable, Equatable, Sendable {
    public struct Sticker: Equatable, Hashable, Sendable {
        public var id: Int
        public var emoji: String
        public init(id: Int, emoji: String = "") {
            self.id = id
            self.emoji = emoji
        }
    }

    public var id = ""
    public var title = ""
    public var author = ""
    public var stickers: [Sticker] = []

    public init(id: String = "", title: String = "", author: String = "", stickers: [Sticker] = []) {
        self.id = id
        self.title = title
        self.author = author
        self.stickers = stickers
    }

    /// Title, author or a sticker's emoji contains `query` (empty matches everything).
    public func matches(_ query: String) -> Bool {
        let q = query.trimmingCharacters(in: .whitespaces)
        if q.isEmpty { return true }
        return title.localizedCaseInsensitiveContains(q) || author.localizedCaseInsensitiveContains(q)
            || stickers.contains { $0.emoji.localizedCaseInsensitiveContains(q) }
    }

    private static func str(_ o: JSON, _ keys: [String]) -> String {
        keys.lazy.map { jsonString(o[$0]).trimmingCharacters(in: .whitespaces) }.first { !$0.isEmpty } ?? ""
    }

    private static func int(_ o: JSON, _ keys: [String], fallback: Int) -> Int {
        for k in keys {
            if let n = o[k] as? NSNumber { return n.intValue }
            if let s = o[k] as? String, let n = Int(s) { return n }
        }
        return fallback
    }

    /// Parses "listStickerPacks" (an array, or an object holding "packs" / "stickerPacks" / packs by id),
    /// sorted by title. Same tolerance as the Qt app (src/StickerPicker.cpp).
    public static func parse(_ value: Any?) -> [StickerPack] {
        var raw: [Any] = []
        if let a = value as? [Any] {
            raw = a
        } else if let o = value as? JSON {
            if let a = o["packs"] as? [Any] { raw = a }
            else if let a = o["stickerPacks"] as? [Any] { raw = a }
            else { raw = o.keys.sorted().compactMap { o[$0] as? JSON } }
        }
        var packs: [StickerPack] = []
        for v in raw {
            var o = jsonObject(v)
            if let inner = o["pack"] as? JSON { o = inner }
            var p = StickerPack()
            p.id = str(o, ["packId", "packID", "pack_id", "id"])
            if p.id.isEmpty { continue }
            p.title = str(o, ["title", "name"])
            p.author = str(o, ["author", "creator"])
            var list = jsonArray(o["stickers"])
            if list.isEmpty { list = jsonArray(jsonObject(o["manifest"])["stickers"]) }
            for (i, sv) in list.enumerated() {
                let so = jsonObject(sv)
                let id = int(so, ["stickerId", "sticker_id", "id"], fallback: i)
                if id >= 0 { p.stickers.append(Sticker(id: id, emoji: str(so, ["emoji", "emojiString", "emoji_string"]))) }
            }
            if p.title.isEmpty { p.title = "Sticker pack" }
            packs.append(p)
        }
        return packs.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
    }

    /// https://signal.art/addstickers/#pack_id=<32 hex>&pack_key=<64 hex> or sgnl://addstickers/?pack_id=…&pack_key=…
    public static func isValidPackUrl(_ raw: String) -> Bool {
        guard let url = URLComponents(string: raw.trimmingCharacters(in: .whitespacesAndNewlines)) else { return false }
        let scheme = url.scheme?.lowercased() ?? ""
        let host = url.host?.lowercased() ?? ""
        let https = scheme == "https" && host == "signal.art" && url.path.lowercased().hasPrefix("/addstickers")
        let sgnl = scheme == "sgnl" && (host == "addstickers" || url.path.lowercased().contains("addstickers"))
        guard https || sgnl else { return false }
        var items = url.queryItems ?? []
        if let fragment = url.fragment, let f = URLComponents(string: "?" + fragment) { items += f.queryItems ?? [] }
        func value(_ name: String) -> String { items.first { $0.name == name && !($0.value ?? "").isEmpty }?.value ?? "" }
        func hex(_ s: String, _ n: Int) -> Bool { s.count == n && s.allSatisfy(\.isHexDigit) }
        return hex(value("pack_id"), 32) && hex(value("pack_key"), 64)
    }
}
