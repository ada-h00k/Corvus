#if DEBUG
import AppKit
import SignalCliCore

/// Debug builds only: a tiny command channel for unattended UI checks against tests/mock-signal-cli.py.
/// Set SIGNAL_CLI_MAC_DEBUG_COMMANDS=<file>; every line appended to it is executed:
///   open <conversation key> | type <text> | send | reply-last | react-last <emoji> | edit-last <text>
///   search <text> | archived on|off | snapshot <png path> | quit
@MainActor
enum DebugHooks {
    private static var offset: UInt64 = 0
    private static var timer: Timer?

    static func start(_ model: AppModel) {
        guard let path = ProcessInfo.processInfo.environment["SIGNAL_CLI_MAC_DEBUG_COMMANDS"] else { return }
        timer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { _ in
            MainActor.assumeIsolated { poll(path, model) }
        }
    }

    private static func poll(_ path: String, _ model: AppModel) {
        guard let h = FileHandle(forReadingAtPath: path) else { return }
        defer { try? h.close() }
        try? h.seek(toOffset: offset)
        let data = h.readDataToEndOfFile()
        guard let nl = data.lastIndex(of: 0x0A) else { return }
        let chunk = data[data.startIndex...nl]
        offset += UInt64(chunk.count)
        for line in String(decoding: chunk, as: UTF8.self).split(separator: "\n") {
            run(String(line), model)
        }
    }

    private static func run(_ line: String, _ model: AppModel) {
        let parts = line.split(separator: " ", maxSplits: 1).map(String.init)
        let arg = parts.count > 1 ? parts[1] : ""
        let last = model.currentConversation?.msgs.last
        FileHandle.standardError.write(Data("debug: \(line)\n".utf8))
        switch parts.first ?? "" {
        case "open": model.openConversation(arg)
        case "type": model.composerText = arg
        case "send": model.sendMessage()
        case "reply-last": if let last { model.startReply(last.id) }
        case "react-last": if let last { model.sendReaction(last.id, arg) }
        case "edit-last":
            if model.editLastMessage() {
                model.composerText = arg
                model.sendMessage()
            }
        case "search":
            model.openSearch()
            model.searchText = arg
        case "archived": model.showArchived = arg == "on"
        case "snapshot": snapshot(to: arg)
        case "quit": NSApp.terminate(nil)
        default: break
        }
    }

    private static func snapshot(to path: String) {
        guard let window = MainWindowTracker.shared.window, let view = window.contentView?.superview ?? window.contentView,
              let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
    }
}
#endif
