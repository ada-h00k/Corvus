import Foundation

/// Signal text formatting (bodyRanges: BOLD, ITALIC, STRIKETHROUGH, MONOSPACE, SPOILER), port of the
/// Qt app's formatting options. All offsets are UTF-16 units, like signal-cli.
public enum TextStyles {
    /// signal-cli's "textStyle" values / history "st": ["start:length:STYLE", …]
    public static func parse(_ v: Any?) -> [TextStyleRange] { Records.textStylesFromJson(v) }
    public static func cliArguments(_ styles: [TextStyleRange]) -> [String] { Records.textStylesToJson(styles) }

    /// Styles after the text in `range` was replaced by `replacementLength` units. Like the Qt app, formatting
    /// belongs to the selected text only: typing right before or after a styled run does not take its style,
    /// typing inside it or over a selection in it does.
    public static func adjust(_ styles: [TextStyleRange], editing range: NSRange, replacementLength: Int) -> [TextStyleRange] {
        let delta = replacementLength - range.length
        let loc = range.location, end = range.location + range.length
        return styles.compactMap { st in
            var start = st.start, stop = st.start + st.length
            if start == loc && range.length == 0 { start += delta }   // typed right in front: not styled
            else if start >= end { start += delta }                   // entirely after the change
            else if start > loc { start = loc }                       // starts inside the replaced text
            if stop == loc && range.length == 0 { }                   // typed right after it: not styled (as in Qt)
            else if stop >= end { stop += delta }
            else if stop > loc { stop = loc }                         // ends inside the replaced text
            return stop > start ? TextStyleRange(start: start, length: stop - start, style: st.style) : nil
        }
    }

    /// Whether every unit of `range` carries `style`.
    public static func covers(_ styles: [TextStyleRange], _ range: NSRange, style: String) -> Bool {
        guard range.length > 0 else { return false }
        var covered = IndexSet()
        for s in styles where s.style == style { covered.insert(integersIn: s.start..<(s.start + s.length)) }
        return covered.contains(integersIn: range.location..<(range.location + range.length))
    }

    /// Turns `style` on for `range`, or off if the whole range already has it. Runs of one style are merged.
    public static func toggle(_ styles: [TextStyleRange], _ range: NSRange, style: String) -> [TextStyleRange] {
        guard range.length > 0 else { return styles }
        var units = IndexSet()
        for s in styles where s.style == style { units.insert(integersIn: s.start..<(s.start + s.length)) }
        let r = range.location..<(range.location + range.length)
        if units.contains(integersIn: r) { units.remove(integersIn: r) } else { units.insert(integersIn: r) }
        let others = styles.filter { $0.style != style }
        let merged = units.rangeView.map { TextStyleRange(start: $0.lowerBound, length: $0.count, style: style) }
        return (others + merged).sorted { ($0.start, $0.style) < ($1.start, $1.style) }
    }

    /// The styles of `raw` for the text that is kept after trimming whitespace and deleting the units at
    /// `removeAt` (the "@" in front of mentions). Same mapping as the Qt app's prepareComposerMessage.
    public static func map(_ styles: [TextStyleRange], in raw: String, removeAt: [Int] = []) -> [TextStyleRange] {
        let s = raw as NSString
        var lead = 0, end = s.length
        let ws = CharacterSet.whitespacesAndNewlines
        while lead < end, let u = UnicodeScalar(s.character(at: lead)), ws.contains(u) { lead += 1 }
        while end > lead, let u = UnicodeScalar(s.character(at: end - 1)), ws.contains(u) { end -= 1 }
        if end <= lead { return [] }
        func removed(before offset: Int) -> Int { removeAt.filter { $0 < offset }.count }
        return styles.compactMap { st in
            let a = max(st.start, lead), b = min(st.start + st.length, end)
            guard st.length > 0, a < b else { return nil }
            let ma = a - lead - removed(before: a), mb = b - lead - removed(before: b)
            return ma < mb ? TextStyleRange(start: ma, length: mb - ma, style: st.style) : nil
        }
    }

    /// Replaces mention placeholders (U+FFFC) in `text` with "@" + name and moves the styles with the text.
    public static func resolveMentions(_ text: String, _ mentions: [(start: Int, length: Int, name: String)],
                                       styles: [TextStyleRange]) -> (text: String, styles: [TextStyleRange]) {
        let s = text as NSString
        var valid: [(start: Int, length: Int, name: String)] = []
        for m in mentions.sorted(by: { $0.start < $1.start })
        where m.start >= 0 && m.length > 0 && m.start + m.length <= s.length && m.start >= (valid.last.map { $0.start + $0.length } ?? 0) {
            valid.append(m)
        }
        guard !valid.isEmpty else { return (text, styles) }
        let out = NSMutableString()
        var map = [Int](repeating: 0, count: s.length + 1) // old offset -> new offset
        var pos = 0
        for m in valid {
            for i in pos..<m.start { map[i] = out.length + (i - pos) }
            out.append(s.substring(with: NSRange(location: pos, length: m.start - pos)))
            for i in m.start..<(m.start + m.length) { map[i] = out.length }
            out.append("@" + m.name)
            pos = m.start + m.length
        }
        for i in pos..<s.length { map[i] = out.length + (i - pos) }
        out.append(s.substring(from: pos))
        map[s.length] = out.length
        let mapped: [TextStyleRange] = styles.compactMap { st in
            let a = min(max(st.start, 0), s.length), b = min(max(st.start + st.length, a), s.length)
            let na = map[a], nb = map[b]
            return nb > na ? TextStyleRange(start: na, length: nb - na, style: st.style) : nil
        }
        return (out as String, mapped)
    }
}
