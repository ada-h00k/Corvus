import AppKit
import SignalCliCore
import SwiftUI

/// Resources of this target: inside the .app they are copied to Contents/Resources,
/// with `swift run` they are next to the executable (Bundle.module).
enum AppResources {
    static let bundle: Bundle = {
        if let u = Bundle.main.url(forResource: "SignalCliMac_SignalCliMac", withExtension: "bundle"), let b = Bundle(url: u) {
            return b
        }
        return Bundle.module
    }()

    /// The emoji data is shared with the Qt app (resources/ in the repository): scripts/build-app.sh copies it
    /// into Contents/Resources, development builds read it from the source tree.
    private static let sharedResources = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().appendingPathComponent("../../../resources").standardized

    static func url(_ name: String, _ ext: String) -> URL? {
        if let u = bundle.url(forResource: name, withExtension: ext) ?? Bundle.main.url(forResource: name, withExtension: ext) {
            return u
        }
        let dev = sharedResources.appendingPathComponent("\(name).\(ext)")
        return FileManager.default.fileExists(atPath: dev.path) ? dev : nil
    }
}

/// Only one instance may use the chat history: two processes appending to history.scq (or compacting it)
/// at the same time would corrupt it.
enum SingleInstance {
    private static var fd: Int32 = -1

    static func acquire() -> Bool {
        fd = open(AppPaths.lockPath, O_RDWR | O_CREAT, 0o600)
        guard fd >= 0 else { return true } // cannot lock (permissions): let the store report problems
        if flock(fd, LOCK_EX | LOCK_NB) == 0 {
            ftruncate(fd, 0)
            let pid = "\(ProcessInfo.processInfo.processIdentifier)\n"
            _ = pid.withCString { write(fd, $0, strlen($0)) }
            return true
        }
        return false
    }

    /// Another instance owns the history: bring it to the front.
    static func activateOther() -> Bool {
        let me = ProcessInfo.processInfo.processIdentifier
        let others = Bundle.main.bundleIdentifier.map { NSRunningApplication.runningApplications(withBundleIdentifier: $0) } ?? []
        guard let other = others.first(where: { $0.processIdentifier != me }) else { return false }
        return other.activate()
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    let model = AppModel()
    private var inputMonitor: Any?

    func applicationWillFinishLaunching(_ notification: Notification) {
        guard SingleInstance.acquire() else {
            if !SingleInstance.activateOther() {
                let alert = NSAlert()
                alert.messageText = "signal-cli Mac is already running"
                alert.informativeText = "Close the other instance first. Two instances would damage the chat history."
                alert.runModal()
            }
            exit(0)
        }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Remembers when the user last used keyboard or mouse in the main window: a key window can still
        // be covered by another app's window, so activity is the hint that somebody is really looking.
        inputMonitor = NSEvent.addLocalMonitorForEvents(matching: [.mouseMoved, .leftMouseDown, .keyDown, .scrollWheel]) { [weak self] e in
            if e.window != nil && e.window === MainWindowTracker.shared.window { self?.model.lastInputMs = nowMs() }
            return e
        }
        if model.notify.startMinimizedInTray && model.notify.trayIcon {
            DispatchQueue.main.async {
                MainWindowTracker.shared.window?.close()
            }
        }
        model.start()
        #if DEBUG
        DebugHooks.start(model)
        #endif
    }

    /// Closing the window keeps the app running in the menu bar only if that icon is shown and enabled.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        !(model.notify.trayIcon && model.notify.closeToTray)
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag { model.openMainWindow?() }
        return true
    }

    func applicationWillTerminate(_ notification: Notification) {
        model.shutdown()
    }
}

@main
struct SignalCliMacApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        let model = delegate.model
        Window("signal-cli Mac", id: "main") {
            ContentView()
                .environment(model)
                .frame(minWidth: 720, minHeight: 460)
        }
        .defaultSize(width: 1020, height: 680)
        .commands { AppCommands(model: model) }

        Settings {
            SettingsView()
                .environment(model)
        }

        MenuBarExtra(isInserted: Binding(get: { model.notify.trayIcon }, set: { _ in })) {
            MenuBarContent().environment(model)
        } label: {
            MenuBarLabel(unread: model.totalUnread)
        }
    }
}

private struct MenuBarLabel: View {
    let unread: Int
    var body: some View {
        if unread > 0 {
            Label("\(unread)", systemImage: "bubble.left.and.text.bubble.right.fill").labelStyle(.titleAndIcon)
        } else {
            Image(systemName: "bubble.left.and.bubble.right")
        }
    }
}

private struct MenuBarContent: View {
    @Environment(AppModel.self) private var model
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        let unread = model.totalUnread
        Text(unread > 0 ? "\(unread) unread message\(unread == 1 ? "" : "s")" : "No unread messages")
        Divider()
        Button("Show window") {
            openWindow(id: "main")
            NSApp.activate(ignoringOtherApps: true)
        }
        Divider()
        Button("Quit signal-cli Mac") { NSApp.terminate(nil) }
            .keyboardShortcut("q")
    }
}

struct AppCommands: Commands {
    let model: AppModel

    var body: some Commands {
        CommandGroup(replacing: .appInfo) {
            Button("About signal-cli Mac") { model.showAbout() }
        }
        CommandGroup(replacing: .newItem) {
            Button("New conversation …") { Task { await model.newConversation() } }
                .keyboardShortcut("n")
            Button("Link device …") { Task { await model.linkDevice() } }
            Button("Reconnect") { model.reconnect() }
                .keyboardShortcut("r")
        }
        CommandGroup(after: .textEditing) {
            Button("Search this conversation") { model.openSearch() }
                .keyboardShortcut("f")
                .disabled(model.current == nil)
        }
        CommandMenu("Conversation") {
            ForEach(1..<10) { n in
                Button("Conversation \(n)") {
                    let list = model.chats.sortedConversations(archived: model.showArchived)
                    if n - 1 < list.count { model.openConversation(list[n - 1].key) }
                }
                .keyboardShortcut(KeyEquivalent(Character("\(n)")))
            }
        }
        CommandGroup(after: .appSettings) {
            Button("History protection …") { Task { await model.changeHistoryProtection() } }
            Button("GIPHY API key …") { Task { await model.configureGiphyApiKey() } }
            Button("Path to signal-cli …") { model.chooseExecutable() }
        }
        CommandGroup(replacing: .help) {
            Button("Check for signal-cli update") { model.checkForUpdates(silent: false) }
            Button("signal-cli on GitHub") { NSWorkspace.shared.open(URL(string: "https://github.com/AsamK/signal-cli")!) }
        }
    }
}
