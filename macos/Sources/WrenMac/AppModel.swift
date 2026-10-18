import AppKit
import AVFoundation
import Observation
import WrenCore
import SwiftUI

/// Unsent composer state of one conversation.
struct Draft {
    var text = ""
    var reply = Quote()
    var files: [String] = []
    var gifAnalyticsUrl: URL?
    var isEmpty: Bool { text.isEmpty && !reply.valid && files.isEmpty }
}

struct UpdateHint: Equatable {
    var version: String
    var url: URL
}

/// Everything the main window shows, plus the logic of src/MainWindow.cpp that is not pure model
/// (that part lives in WrenCore.ChatState).
@MainActor
@Observable
final class AppModel {
    // MARK: State shown by the views

    var chats = ChatState()
    var current: String?            // selected conversation
    var showArchived = false
    var connected = false
    var statusText = "Not connected"
    var flash: String?              // transient status bar message
    var updateHint: UpdateHint?
    var version = ""                // e.g. "0.13.12"
    var historyProtectionMode = ""  // "keychain" | "passphrase"
    var historyReady = false

    var appearance = Appearance.load()
    var notify = NotificationSettings.load()

    // composer (for the current conversation; other conversations keep a Draft)
    var composerText = ""
    var replyQuote = Quote()
    var pendingFiles: [String] = []
    var pendingGifAnalyticsUrl: URL?
    var editKey = ""                // message being edited (conversation); empty = not editing
    var editTs: Int64 = 0
    private var editDraft = ""
    var composerFocusRequest = 0    // incremented to put the caret into the composer
    var composerCursorToEnd = 0
    private var drafts: [String: Draft] = [:]

    // search in the current conversation
    var searchVisible = false
    var searchText = "" { didSet { updateSearch(selectFirst: true) } }
    var searchResults: [Int] = []
    var searchPosition = -1
    var scrollRequest: (id: String, serial: Int)?
    private var scrollSerial = 0

    // avatars (cached in memory only)
    var avatars: [String: NSImage] = [:]
    @ObservationIgnored private var avatarTried: Set<String> = []
    @ObservationIgnored private var avatarBusy: Set<String> = []
    @ObservationIgnored private var avatarInFlight = 0
    @ObservationIgnored private var avatarDigest: [String: Int] = [:]
    @ObservationIgnored private var avatarFetchedMs: [String: Int64] = [:]

    // attachments
    @ObservationIgnored private var thumbPending: Set<String> = []
    @ObservationIgnored private var gifPending: Set<String> = []
    var animatedGifs: [String: Data] = [:]      // attachment token -> GIF data
    var videoURLs: [String: URL] = [:]          // attachment token -> local file for playback
    var videoLoading: Set<String> = []

    // audio playback
    var audioToken: String?
    var audioLoading = false
    var audioPlaying = false
    var audioPosition: Double = 0
    var audioDuration: Double = 0
    @ObservationIgnored private var audioPlayer: AVPlayer?
    @ObservationIgnored private var audioObserver: Any?
    @ObservationIgnored private var audioEndObserver: NSObjectProtocol?

    // profile
    var profileUsername = ""
    var profileStatus: (text: String, error: Bool)?

    // sheets
    var linkSheetShown = false
    @ObservationIgnored private var linkContinuation: CheckedContinuation<Bool, Never>?
    var giphyApiKey: String?        // non-nil = GIF picker shown

    // MARK: Services

    @ObservationIgnored let cli = SignalCli()
    @ObservationIgnored let store = HistoryStore()
    @ObservationIgnored let notifier = Notifier()
    @ObservationIgnored let sound = SoundPlayer()
    @ObservationIgnored private var connectGen = 0
    @ObservationIgnored private var lastSoundMs: Int64 = 0
    @ObservationIgnored var lastInputMs: Int64 = 0
    @ObservationIgnored private var timers: [Timer] = []
    @ObservationIgnored private var flashTask: Task<Void, Never>?
    @ObservationIgnored private var started = false
    @ObservationIgnored var openMainWindow: (() -> Void)?

    var account: String { chats.account }

    init() {
        cli.onEnvelope = { [weak self] env in self?.onEnvelope(env) }
        cli.onStopped = { [weak self] reason in self?.setConnected(false, info: reason) }
        cli.onLogLine = { [weak self] line in self?.showFlash(line, seconds: 5) }
        notifier.onActivate = { [weak self] key in self?.activateConversation(key) }
    }

    // MARK: - Start

    func start() {
        guard !started else { return }
        started = true
        notifier.setUp()
        timers.append(Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.expireMessages() }
        })
        // Contacts change their picture at any time and signal-cli does not announce it: look again regularly.
        timers.append(Timer.scheduledTimer(withTimeInterval: 15 * 60, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.avatarTried.removeAll()
                self?.fetchMissingAvatars()
            }
        })
        Task {
            guard await unlockHistory() else {
                NSApp.terminate(nil)
                return
            }
            historyReady = true
            guard await ensureExecutable() else { return }
            await connectAccount()
        }
    }

    func shutdown() {
        cli.stop()
        store.close()
        try? FileManager.default.removeItem(at: AppPaths.tempDir)
    }

    func showFlash(_ text: String, seconds: Double = 10) {
        flash = text
        flashTask?.cancel()
        flashTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(seconds))
            if !Task.isCancelled { self?.flash = nil }
        }
    }

    func clearFlash() {
        flashTask?.cancel()
        flash = nil
    }

    // MARK: - signal-cli

    func ensureExecutable() async -> Bool {
        var exe = UserDefaults.standard.string(forKey: "executable") ?? ""
        if exe.isEmpty || !FileManager.default.fileExists(atPath: exe) { exe = SignalCli.autodetect() }
        while exe.isEmpty {
            let r = await Dialogs.ask("signal-cli not found",
                                      "signal-cli is not installed or not in PATH.\n\nThis app requires a separately installed signal-cli "
                                          + "(for example: brew install signal-cli).",
                                      buttons: ["Choose path …", "Open download page", "Close"], style: .warning)
            switch r {
            case 0: exe = pickExecutable() ?? ""
            case 1: NSWorkspace.shared.open(URL(string: "https://github.com/AsamK/signal-cli/releases")!)
            default: return false
            }
        }
        UserDefaults.standard.set(exe, forKey: "executable")
        cli.executable = exe
        return true
    }

    private func pickExecutable() -> String? {
        let panel = NSOpenPanel()
        panel.title = "Select signal-cli"
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.treatsFilePackagesAsDirectories = true
        if !cli.executable.isEmpty { panel.directoryURL = URL(fileURLWithPath: cli.executable).deletingLastPathComponent() }
        return panel.runModal() == .OK ? panel.url?.path : nil
    }

    func chooseExecutable() {
        guard let exe = pickExecutable() else { return }
        UserDefaults.standard.set(exe, forKey: "executable")
        cli.executable = exe
        Task { await connectAccount() }
    }

    func reconnect() {
        Task {
            if cli.executable.isEmpty { guard await ensureExecutable() else { return } }
            await connectAccount()
        }
    }

    /// Everything here is asynchronous so the window never freezes while the JVM behind signal-cli starts.
    /// The version is read on every (re)connect, so an update of signal-cli is noticed without restarting.
    func connectAccount() async {
        guard historyReady else { return }
        connectGen += 1
        let gen = connectGen
        cli.stop()
        setConnected(false)
        statusText = "Connecting …"

        let (vr, versionLine) = await cli.queryVersion()
        guard gen == connectGen else { return }
        if !vr.ok {
            await showCliProblem("signal-cli could not be run.", vr)
            return
        }
        version = versionLine.split(separator: " ").last.map(String.init) ?? versionLine

        let (ar, accounts) = await cli.queryAccounts()
        guard gen == connectGen else { return }
        // A failing command is NOT "no account": report what actually went wrong.
        if !ar.ok && accounts.isEmpty {
            await showCliProblem("The accounts could not be listed.", ar)
            return
        }
        if accounts.isEmpty {
            setConnected(false, info: "no account linked")
            if await Dialogs.confirm("No account", "signal-cli reports that no account is set up yet.\nLink this device with your phone now?",
                                     ok: "Link device …"), gen == connectGen {
                await linkDevice()
            }
            return
        }

        var acc = UserDefaults.standard.string(forKey: "account") ?? ""
        if !accounts.contains(acc) {
            if accounts.count == 1 {
                acc = accounts[0]
            } else {
                guard let i = await Dialogs.choose("Choose account", "Account:", options: accounts) else {
                    setConnected(false, info: "no account chosen")
                    return
                }
                acc = accounts[i]
            }
            UserDefaults.standard.set(acc, forKey: "account")
        }
        guard gen == connectGen else { return } // reconnect requested while a dialog was open

        chats.account = acc
        cli.start(account: acc)
        guard cli.isRunning else { return }
        setConnected(true)
        loadContactsAndGroups()
        checkForUpdates(silent: true) // silent: only a hint in the status bar
    }

    private func showCliProblem(_ title: String, _ r: CliResult) async {
        setConnected(false, info: title)
        let info = r.describe() + "\n\nCommand: " + SignalCli.maskNumbers(r.command)
            + "\n\nThis does not mean that no account is linked. Check that the command works in a terminal "
            + "(the non-native signal-cli build needs Java)."
        switch await Dialogs.ask(title, info, buttons: ["Retry", "Choose signal-cli …", "Close"], style: .warning) {
        case 0: await connectAccount()
        case 1: chooseExecutable()
        default: break
        }
    }

    func setConnected(_ on: Bool, info: String = "") {
        connected = on
        if on {
            statusText = "Connected · signal-cli \(version)"
        } else {
            statusText = info.isEmpty ? "Not connected" : "Disconnected: \(info)"
        }
    }

    func linkDevice() async {
        if cli.executable.isEmpty { guard await ensureExecutable() else { return } }
        connectGen += 1
        cli.stop()
        setConnected(false)
        openMainWindow?()
        let linked = await withCheckedContinuation { cont in
            linkContinuation = cont
            linkSheetShown = true
        }
        if linked {
            UserDefaults.standard.removeObject(forKey: "account")
            await connectAccount()
        }
    }

    func finishLinking(_ linked: Bool) {
        linkSheetShown = false
        linkContinuation?.resume(returning: linked)
        linkContinuation = nil
    }

    // MARK: - Contacts / groups

    private func loadContactsAndGroups() {
        avatarTried.removeAll() // ask signal-cli again, avatars may have changed
        avatarBusy.removeAll()
        avatarInFlight = 0
        Task {
            do {
                let res = try await cli.call("listContacts")
                for cv in jsonArray(res) {
                    let c = jsonObject(cv)
                    let number = jsonString(c["number"]), uuid = jsonString(c["uuid"])
                    let key = !number.isEmpty ? number : uuid
                    if key.isEmpty { continue }
                    if !number.isEmpty && !uuid.isEmpty { chats.alias[uuid] = number }

                    var title = jsonString(c["name"])
                    if title.isEmpty { title = jsonString(c["profileName"]) }
                    if !title.isEmpty { chats.contactNames[key] = title }

                    // signal-cli lists every known recipient (address book, group members, …), not just people
                    // we have chatted with. Only show a chat once there is one: local history, a new message,
                    // or an archived chat on the phone.
                    let sig = jsonBool(c["isArchived"])
                    if jsonBool(c["isHidden"]) { continue }
                    if chats.convs[key] == nil && !sig { continue }

                    chats.ensure(key, title: key)
                    let expiration = jsonInt(c["messageExpirationTime"])
                    if chats.convs[key]!.expirationSeconds != expiration {
                        chats.convs[key]!.expirationSeconds = expiration
                        persist(Records.expiration(key, expiration))
                    }
                    let stored = chats.convs[key]!.storedName
                    if title.isEmpty { title = stored.isEmpty ? key : stored }
                    let nick = chats.convs[key]!.localNickname
                    chats.convs[key]!.title = nick.isEmpty ? title : nick

                    // Archive state: follow signal-cli only when *it* changed (e.g. archived on the phone);
                    // otherwise keep the local state (e.g. auto-unarchived by a new message).
                    if sig != chats.convs[key]!.sigArchived {
                        chats.convs[key]!.sigArchived = sig
                        chats.convs[key]!.archived = sig
                        persist(Records.archive(key, archived: sig, sigArchived: sig))
                    }
                }
                listChanged()
            } catch {
                showFlash("Could not load contacts: \(error.localizedDescription)", seconds: 8)
            }
        }
        Task {
            guard let res = try? await cli.call("listGroups") else { return }
            for gv in jsonArray(res) {
                let g = jsonObject(gv)
                if g["isMember"] != nil && !jsonBool(g["isMember"]) { continue }
                let id = jsonString(g["id"])
                if id.isEmpty { continue }
                var title = jsonString(g["name"])
                if title.isEmpty { title = "Group" }
                let key = "g:" + id
                chats.ensure(key, title: title, group: true)
                chats.convs[key]!.title = title
                let expiration = jsonInt(g["messageExpirationTime"])
                if chats.convs[key]!.expirationSeconds != expiration {
                    chats.convs[key]!.expirationSeconds = expiration
                    persist(Records.expiration(key, expiration))
                }
            }
            listChanged()
        }
    }

    // MARK: - Encrypted history

    private func openStore(_ secret: String, create: Bool) async -> Result<[JSON], HistoryStoreError> {
        let store = self.store, path = AppPaths.historyPath
        statusText = create ? "Creating chat history …" : "Unlocking chat history …"
        defer { statusText = "Not connected" }
        return await Task.detached {
            do {
                if create {
                    try store.create(path: path, passphrase: secret)
                    return .success([])
                }
                return .success(try store.open(path: path, passphrase: secret))
            } catch let e as HistoryStoreError {
                return .failure(e)
            } catch {
                return .failure(HistoryStoreError(code: 4))
            }
        }.value
    }

    private func saveProtectionMode(_ mode: String) {
        historyProtectionMode = mode
        UserDefaults.standard.set(mode, forKey: "historyProtectionMode")
    }

    /// Opens (or creates) the history; keychain or passphrase protection like the Qt app.
    func unlockHistory() async -> Bool {
        let path = AppPaths.historyPath
        #if DEBUG
        // Debug builds only: unattended start for manual testing against tests/mock-signal-cli.py.
        if let pw = ProcessInfo.processInfo.environment["SIGNAL_CLI_MAC_TEST_PASSPHRASE"] {
            guard case .success(let records) = await openStore(pw, create: !HistoryStore.exists(path)) else { return false }
            historyProtectionMode = "passphrase"
            loadHistory(records)
            return true
        }
        #endif
        let exists = HistoryStore.exists(path)
        let saved = UserDefaults.standard.string(forKey: "historyProtectionMode") ?? ""
        historyProtectionMode = saved
        if exists && saved.isEmpty { historyProtectionMode = "passphrase" }
        if !exists && saved.isEmpty {
            historyProtectionMode = "keychain"
            switch await Dialogs.ask("Protect chat history", "Choose how to protect your new chat history.",
                                     buttons: ["System keychain (recommended)", "Use a passphrase"], cancel: true) {
            case 0: break
            case 1: historyProtectionMode = "passphrase"
            default: return false
            }
        }

        // An existing history without a saved mode: try the keychain once.
        if exists && saved.isEmpty, let secret = try? HistoryKeychain.load() {
            if case .success(let records) = await openStore(secret, create: false) {
                saveProtectionMode("keychain")
                loadHistory(records)
                return true
            }
            historyProtectionMode = "passphrase"
        }

        var error = ""
        if historyProtectionMode == "keychain" {
            if exists {
                let secret: String
                do {
                    guard let s = try HistoryKeychain.load() else {
                        throw RpcError("No history key was found in the keychain.")
                    }
                    secret = s
                } catch {
                    await Dialogs.error("Chat history", "Could not read the history key from the keychain:\n\(error.localizedDescription)")
                    return false
                }
                switch await openStore(secret, create: false) {
                case .success(let records):
                    loadHistory(records)
                    return true
                case .failure(let e) where e == .wrongPassphrase:
                    error = "The keychain key did not unlock this history. Try its previous passphrase if protection was just changed."
                    historyProtectionMode = "passphrase"
                case .failure(let e):
                    await Dialogs.error("Chat history", e.localizedDescription)
                    return false
                }
            } else {
                let secret = HistoryKeychain.generateSecret()
                do { try HistoryKeychain.save(secret) } catch {
                    await Dialogs.error("Chat history", "Could not save the history key in the keychain:\n\(error.localizedDescription)")
                    return false
                }
                if case .failure(let e) = await openStore(secret, create: true) {
                    try? HistoryKeychain.clear()
                    await Dialogs.error("Chat history", e.localizedDescription)
                    return false
                }
                saveProtectionMode("keychain")
                return true
            }
        }

        while true {
            guard let pw = await Dialogs.passphrase(exists ? .unlock : .create, error: error) else { return false }
            switch await openStore(pw.new, create: !exists) {
            case .success(let records):
                saveProtectionMode("passphrase")
                loadHistory(records)
                return true
            case .failure(let e) where e == .wrongPassphrase:
                error = e.localizedDescription
            case .failure(let e):
                await Dialogs.error("Chat history", "\(e.localizedDescription)\n\nFile: \(path)\n\nMove or delete the file to start with a new history.")
                return false
            }
        }
    }

    private func loadHistory(_ records: [JSON]) {
        chats.loadHistory(records)
        listChanged()
    }

    func persist(_ record: JSON) {
        do { try store.append(record) } catch {
            showFlash("Could not save message: \(error.localizedDescription)")
        }
    }

    func persist(_ records: [JSON]) { records.forEach(persist) }

    /// Compacts the file so the ciphertext of removed data is gone.
    @discardableResult
    private func rewriteHistory(_ what: String) -> Bool {
        do {
            try store.rewrite(chats.allRecords())
            return true
        } catch {
            showFlash("Could not \(what): \(error.localizedDescription)")
            return false
        }
    }

    private func changePassphrase(old: String, new: String) async -> HistoryStoreError? {
        let store = self.store
        return await Task.detached { () -> HistoryStoreError? in
            do { try store.changePassphrase(old: old, new: new); return nil } catch let e as HistoryStoreError { return e } catch {
                return HistoryStoreError(code: 5)
            }
        }.value
    }

    func changeHistoryProtection() async {
        guard store.isOpen else { return }
        let isKeychain = historyProtectionMode == "keychain"
        guard let choice = await Dialogs.ask("Chat history protection",
                                             "Choose how to protect the encrypted chat history. It is currently protected by "
                                                 + (isKeychain ? "the system keychain." : "a passphrase."),
                                             buttons: ["System keychain", isKeychain ? "Passphrase" : "Change passphrase"],
                                             cancel: true) else { return }
        let useKeychain = choice == 0
        if useKeychain && isKeychain { return }

        if useKeychain {
            var error = ""
            while true {
                guard let old = await Dialogs.passphrase(.unlock, error: error) else { return }
                let newSecret = HistoryKeychain.generateSecret()
                do { try HistoryKeychain.save(newSecret) } catch {
                    await Dialogs.error("Chat history", error.localizedDescription)
                    return
                }
                if let e = await changePassphrase(old: old.new, new: newSecret) {
                    try? HistoryKeychain.clear()
                    if e == .wrongPassphrase { error = e.localizedDescription; continue }
                    await Dialogs.error("Chat history", e.localizedDescription)
                    return
                }
                saveProtectionMode("keychain")
                await Dialogs.info("Chat history", "The history is now protected by the system keychain.")
                return
            }
        }

        if isKeychain {
            let oldSecret: String
            do {
                guard let s = try HistoryKeychain.load() else { throw RpcError("No history key was found in the keychain.") }
                oldSecret = s
            } catch {
                await Dialogs.error("Chat history", error.localizedDescription)
                return
            }
            guard let pw = await Dialogs.passphrase(.create) else { return }
            if let e = await changePassphrase(old: oldSecret, new: pw.new) {
                await Dialogs.error("Chat history", e.localizedDescription)
                return
            }
            saveProtectionMode("passphrase")
            do { try HistoryKeychain.clear() } catch { await Dialogs.warn("Chat history", error.localizedDescription) }
            await Dialogs.info("Chat history", "The history is now protected by your passphrase.")
            return
        }

        var error = ""
        while true {
            guard let pw = await Dialogs.passphrase(.change, error: error) else { return }
            guard let e = await changePassphrase(old: pw.old, new: pw.new) else {
                await Dialogs.info("Chat history", "The passphrase has been changed.")
                return
            }
            error = e.localizedDescription
            if e != .wrongPassphrase {
                await Dialogs.error("Chat history", error)
                return
            }
        }
    }

    // MARK: - Conversations

    var currentConversation: Conversation? { current.flatMap { chats.convs[$0] } }

    var totalUnread: Int { chats.convs.values.reduce(0) { $0 + $1.unread } }

    func displayName(_ key: String) -> String { chats.displayName(key) }

    /// Called after changes to the list (order, unread counters, …).
    private func listChanged() {
        if showArchived && !chats.convs.values.contains(where: \.archived) { showArchived = false } // folder became empty
        updateBadge()
        fetchMissingAvatars()
    }

    private func updateBadge() {
        let unread = totalUnread
        NSApp.dockTile.badgeLabel = unread > 0 ? "\(unread)" : nil
    }

    func learnName(_ key: String, _ name: String) {
        chats.ensure(key)
        let c = chats.convs[key]!
        if !c.storedName.isEmpty || c.isGroup { return }
        if c.title != c.key && c.title != name && c.localNickname.isEmpty { return } // a saved contact name wins
        chats.convs[key]!.storedName = name
        if c.localNickname.isEmpty { chats.convs[key]!.title = name }
        persist(Records.name(key, name))
    }

    func setNickname(_ key: String, _ nickname: String) {
        guard let c = chats.convs[key], !c.isGroup else { return }
        chats.convs[key]!.localNickname = nickname
        chats.convs[key]!.title = nickname.isEmpty
            ? (chats.contactNames[key] ?? (c.storedName.isEmpty ? key : c.storedName)) : nickname
        persist(Records.nickname(key, nickname))
    }

    func editNickname(_ key: String) async {
        guard let c = chats.convs[key] else { return }
        if let n = await Dialogs.text("Contact nickname", "Nickname for \(c.title):", initial: c.localNickname) {
            setNickname(key, n.trimmingCharacters(in: .whitespaces))
        }
    }

    func setArchived(_ key: String, _ archived: Bool) {
        guard let c = chats.convs[key], c.archived != archived else { return }
        chats.convs[key]!.archived = archived
        persist(Records.archive(key, archived: archived, sigArchived: c.sigArchived))
        listChanged()
    }

    /// Local per-chat mute (signal-cli does not report mute state): no notification, no sound.
    func setMuted(_ key: String, _ muted: Bool) {
        guard let c = chats.convs[key], c.muted != muted else { return }
        chats.convs[key]!.muted = muted
        persist(Records.mute(key, muted))
    }

    static let expirationChoices: [(String, Int)] = [
        ("Off", 0), ("30 seconds", 30), ("1 minute", 60), ("5 minutes", 300), ("1 hour", 3600), ("6 hours", 6 * 3600),
        ("12 hours", 12 * 3600), ("1 day", 86400), ("1 week", 7 * 86400), ("4 weeks", 28 * 86400),
    ]

    func setExpiration(_ key: String, _ seconds: Int) {
        guard let c = chats.convs[key], seconds >= 0 else { return }
        Task {
            do {
                if c.isGroup {
                    try await cli.call("updateGroup", ["groupId": String(key.dropFirst(2)), "expiration": seconds])
                } else {
                    try await cli.call("updateContact", ["recipient": chats.resolveKey(key), "expiration": seconds])
                }
                guard chats.convs[key] != nil else { return }
                chats.convs[key]!.expirationSeconds = seconds
                persist(Records.expiration(key, seconds))
                showFlash("Disappearing-message timer updated.", seconds: 5)
            } catch {
                showFlash("Could not change disappearing-message timer: \(error.localizedDescription)")
            }
        }
    }

    func deleteConversation(_ key: String) async {
        guard let c = chats.convs[key] else { return }
        guard await Dialogs.confirm("Delete conversation", "Delete “\(c.title)” and its stored messages from this computer?",
                                    ok: "Delete", destructive: true) else { return }
        if key == editKey { cancelEdit() }
        chats.convs.removeValue(forKey: key)
        drafts.removeValue(forKey: key)
        if key == current {
            current = nil
            composerText = ""
            replyQuote = Quote()
            pendingFiles = []
            pendingGifAnalyticsUrl = nil
        }
        rewriteHistory("update history")
        listChanged()
    }

    func newConversation() async {
        guard let r = await Dialogs.text("New conversation", "Phone number or Signal username (e.g. username.1234):")?
            .trimmingCharacters(in: .whitespaces), !r.isEmpty else { return }
        chats.ensure(r)
        if chats.convs[r]!.archived { setArchived(r, false) }
        showArchived = false
        openConversation(r)
    }

    /// Selecting a chat: stashes the old chat's draft, restores this chat's draft, marks it read.
    func openConversation(_ key: String) {
        guard chats.convs[key] != nil else { return }
        if key != current {
            cancelEdit() // an edit in progress is dropped and the text typed before it comes back
            saveDraft()
            current = key
            loadDraft(key)
            closeSearch()
        }
        chats.convs[key]!.unread = 0
        persist(chats.startIncomingExpirationTimers(key, now: nowMs()))
        notifier.close(key) // the notification for this chat is obsolete now
        composerFocusRequest += 1
        listChanged()
    }

    private func saveDraft() {
        guard let key = current else { return }
        let d = Draft(text: composerText, reply: replyQuote, files: pendingFiles, gifAnalyticsUrl: pendingGifAnalyticsUrl)
        if d.isEmpty { drafts.removeValue(forKey: key) } else { drafts[key] = d }
    }

    private func loadDraft(_ key: String) {
        let d = drafts.removeValue(forKey: key) ?? Draft() // the composer owns it while the chat is open
        composerText = d.text
        replyQuote = d.reply
        pendingFiles = d.files
        pendingGifAnalyticsUrl = d.gifAnalyticsUrl
        composerCursorToEnd += 1
    }

    /// Window became key: messages that arrived in the open chat while it was covered are read now.
    func windowActivated() {
        lastInputMs = nowMs()
        if let key = current, chats.convs[key] != nil {
            persist(chats.startIncomingExpirationTimers(key, now: nowMs()))
            if chats.convs[key]!.unread > 0 {
                chats.convs[key]!.unread = 0
                notifier.close(key)
                listChanged()
            }
        }
        if !searchVisible { composerFocusRequest += 1 }
    }

    func activateConversation(_ key: String) {
        openMainWindow?()
        NSApp.activate(ignoringOtherApps: true)
        guard let c = chats.convs[key] else { return }
        showArchived = c.archived
        openConversation(key)
    }

    /// Window really in front of the user (not just "key" while covered by another app's window).
    func userIsLooking() -> Bool {
        guard NSApp.isActive, let w = MainWindowTracker.shared.window, w.isVisible, !w.isMiniaturized, w.isKeyWindow
        else { return false }
        if w.frame.contains(NSEvent.mouseLocation) { return true }
        return nowMs() - lastInputMs < 30_000
    }

    private func expireMessages() {
        guard historyReady, chats.expireMessages(now: nowMs()) else { return }
        rewriteHistory("remove expired messages")
        if let key = current, chats.convs[key] != nil { updateSearch() }
    }

    // MARK: - Search

    func openSearch() {
        searchVisible = true
        updateSearch(selectFirst: true)
    }

    func closeSearch() {
        searchVisible = false
        if !searchText.isEmpty { searchText = "" }
        composerFocusRequest += 1
    }

    func updateSearch(selectFirst: Bool = false) {
        let old = searchPosition
        searchResults = []
        let needle = searchText.trimmingCharacters(in: .whitespaces)
        if !needle.isEmpty, let c = currentConversation {
            for (i, m) in c.msgs.enumerated() {
                var s = m.sender + "\n" + m.text + "\n" + m.quote.text
                for a in m.attachments { s += "\n" + a.filename }
                if s.localizedCaseInsensitiveContains(needle) { searchResults.append(i) }
            }
        }
        if searchResults.isEmpty {
            searchPosition = -1
            return
        }
        searchPosition = selectFirst || old < 0 ? 0 : min(old, searchResults.count - 1)
        if selectFirst { scrollToMessage(index: searchResults[searchPosition]) }
    }

    func navigateSearch(backwards: Bool) {
        guard !searchResults.isEmpty else { return }
        let n = searchResults.count
        searchPosition = (searchPosition + (backwards ? n - 1 : 1)) % n
        scrollToMessage(index: searchResults[searchPosition])
    }

    var searchMatchId: String? {
        guard searchVisible, searchPosition >= 0, searchPosition < searchResults.count, let c = currentConversation,
              searchResults[searchPosition] < c.msgs.count else { return nil }
        return c.msgs[searchResults[searchPosition]].id
    }

    func scrollToMessage(index: Int) {
        guard let c = currentConversation, index >= 0, index < c.msgs.count else { return }
        scrollSerial += 1
        scrollRequest = (c.msgs[index].id, scrollSerial)
    }

    /// Clicking a quote jumps to the original message.
    func jumpToQuote(_ q: Quote) {
        guard let c = currentConversation,
              let i = chats.findMessage(c, ts: q.id, authors: q.authorKey.isEmpty ? [] : [q.authorKey])
                ?? chats.findMessage(c, ts: q.id, authors: [])
        else {
            showFlash("The quoted message is not in this chat history.", seconds: 5)
            return
        }
        scrollToMessage(index: i)
    }

    // MARK: - Receiving

    private func onEnvelope(_ env: JSON) {
        let pe = parseEnvelope(env)
        if pe.kind == .none { return }

        let group = !pe.groupId.isEmpty
        let srcKey = chats.resolveKey(pe.src)
        let key = group ? "g:" + pe.groupId : (pe.outgoing ? chats.resolveKey(pe.destination) : srcKey)
        if key.isEmpty { return }

        switch pe.kind {
        case .expirationUpdate:
            if let c = chats.convs[key], c.expirationSeconds != pe.expirationSeconds {
                chats.convs[key]!.expirationSeconds = pe.expirationSeconds
                persist(Records.expiration(key, pe.expirationSeconds))
            }
            return
        case .reaction:
            let reactor = pe.outgoing ? account : srcKey
            if let rec = chats.applyReaction(key, ts: pe.targetTs, authors: [pe.targetNumber, pe.targetUuid],
                                             reactor: reactor, emoji: pe.remove ? "" : pe.emoji) {
                persist(rec)
            }
            return
        case .edit:
            // Like the official clients: an edit is only applied to a known message written by the same author.
            let author = pe.outgoing ? account : srcKey
            if let rec = chats.applyEdit(key, targetTs: pe.targetTs, authors: [author], text: pe.msg.text, newTs: pe.msg.ts) {
                persist(rec)
            }
            return
        default:
            break
        }

        var m = pe.msg
        m.outgoing = pe.outgoing
        m.authorKey = pe.outgoing ? account : srcKey
        if m.outgoing && m.expiresInSeconds > 0 { m.expirationStartMs = m.ts }
        if m.quote.valid { m.quote.authorKey = chats.resolveKey(m.quote.authorKey) }
        if !pe.outgoing {
            var display = chats.convs[srcKey]?.title ?? ""
            if display.isEmpty { display = pe.srcName.isEmpty ? pe.src : pe.srcName }
            m.sender = display
        }

        // signal-cli can publish our own message envelope before the send RPC returns.
        // Reconcile it with the visible preview so the chat never shows two bubbles.
        if pe.outgoing, let pendingId = chats.pendingSendMatching(echo: m, key: key) {
            persist(chats.finalizePendingSend(pendingId, timestamp: m.ts, echo: m))
            fetchThumbnails(key, ts: m.ts, author: m.authorKey)
            listChanged()
            return
        }

        let title = group ? "Group" : (pe.outgoing ? key : m.sender)
        guard addMessage(key, m, title: title, group: group) else { return }
        if !pe.outgoing && !group && !pe.srcName.isEmpty { learnName(key, pe.srcName) }
        if !pe.outgoing {
            alertIncoming(key, m)
            if !group { refreshAvatar(key) } // the contact may have changed the picture
        }
        fetchThumbnails(key, ts: m.ts, author: m.authorKey)
    }

    @discardableResult
    private func addMessage(_ key: String, _ m0: Message, title: String = "", group: Bool = false) -> Bool {
        var m = m0
        let looking = key == current && MainWindowTracker.shared.window?.isVisible == true
            && MainWindowTracker.shared.window?.isMiniaturized == false
        if !m.outgoing && m.expiresInSeconds > 0 && looking { m.expirationStartMs = nowMs() }
        guard let (_, recs) = chats.addMessage(key, m, title: title, group: group) else { return false } // duplicate
        persist(recs)
        // A message in the selected chat is only read while the user looks at it.
        if !m.outgoing && !(key == current && userIsLooking()) { chats.convs[key]!.unread += 1 }
        if !m.outgoing && !NSApp.isActive { NSApp.requestUserAttention(.informationalRequest) }
        if key == current { updateSearch() }
        listChanged()
        return true
    }

    private func alertIncoming(_ key: String, _ m: Message) {
        guard let c = chats.convs[key], !c.muted else { return }
        // Nothing to announce while the user is looking at this very conversation.
        if key == current && userIsLooking() { return }

        // Notification and sound are independent switches.
        if notify.notifications {
            var preview = m.text.split(whereSeparator: \.isWhitespace).joined(separator: " ")
            if preview.count > 240 { preview = String(preview.prefix(240)) + "…" }
            if preview.isEmpty, let a = m.attachments.first { preview = a.isImage ? "[Image]" : "[File] \(a.filename)" }
            var title = c.title, body: String
            switch notify.content {
            case .hidden:
                title = "Wren"
                body = "New message"
            case .nameOnly:
                body = "New message"
            case .nameAndText:
                body = c.isGroup && !m.sender.isEmpty ? m.sender + ": " + preview : preview
            }
            // The sender's picture instead of a generic icon; not when the notification hides who wrote.
            notifier.notify(key: key, title: title, body: body, icon: notify.content == .hidden ? nil : avatarImage(c))
        }
        if notify.sound {
            let now = nowMs()
            if now - lastSoundMs >= 700 { // a burst of messages should not become a burst of noise
                lastSoundMs = now
                sound.play(notify)
            }
        }
    }

    // MARK: - Sending

    private func addRecipient(_ params: inout JSON, _ c: Conversation) {
        if c.isGroup { params["groupId"] = String(c.key.dropFirst(2)) } else { params["recipient"] = [c.key] }
    }

    func message(withId id: String) -> (index: Int, message: Message)? {
        guard let c = currentConversation, let i = c.msgs.firstIndex(where: { $0.id == id }) else { return nil }
        return (i, c.msgs[i])
    }

    func startReply(_ id: String) {
        guard let c = currentConversation, let (_, m) = message(withId: id) else { return }
        replyQuote = Quote()
        replyQuote.valid = true
        replyQuote.id = m.latestTs // peers know an edited message by its latest timestamp
        replyQuote.authorKey = chats.effectiveAuthor(c, m)
        replyQuote.text = m.text
        if m.text.isEmpty, let a = m.attachments.first { replyQuote.attachmentNote = a.contentType }
        composerFocusRequest += 1
    }

    func sendMessage() {
        if !editKey.isEmpty {
            sendEdit(composerText.trimmingCharacters(in: .whitespacesAndNewlines))
            return
        }
        let text = composerText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let key = current, let c = chats.convs[key], !(text.isEmpty && pendingFiles.isEmpty) else { return }
        guard connected else {
            showFlash("Not connected to signal-cli.", seconds: 5)
            return
        }
        let files = pendingFiles, quote = replyQuote, gifAnalytics = pendingGifAnalyticsUrl

        var p: JSON = [:]
        if !text.isEmpty { p["message"] = text }
        var atts: [Attachment] = []
        for f in files {
            var a = Attachment()
            a.path = f
            a.filename = (f as NSString).lastPathComponent
            a.size = ((try? FileManager.default.attributesOfItem(atPath: f))?[.size] as? NSNumber)?.int64Value ?? 0
            a.contentType = Media.mimeType(forFile: f)
            if a.isImage, let data = FileManager.default.contents(atPath: f) {
                if let (w, h) = Media.pixelSize(data) {
                    a.width = w
                    a.height = h
                }
                a.thumb = Media.makeThumbnail(data)
            }
            atts.append(a)
        }
        if !files.isEmpty { p["attachments"] = files }
        if quote.valid && !quote.authorKey.isEmpty {
            p["quoteTimestamp"] = quote.id
            p["quoteAuthor"] = quote.authorKey
            p["quoteMessage"] = quote.text
        }
        addRecipient(&p, c)

        // Insert a temporary local message before waiting for signal-cli. It is kept out of
        // the encrypted history until the send succeeds, so a failed send leaves no stale record.
        var preview = Message()
        preview.ts = nowMs()
        while chats.findMessage(chats.convs[key]!, ts: preview.ts, authors: []) != nil { preview.ts += 1 }
        preview.text = text
        preview.outgoing = true
        preview.authorKey = account
        preview.expiresInSeconds = c.expirationSeconds
        preview.quote = quote
        preview.attachments = atts

        let pendingId = UUID().uuidString
        var pending = PendingSend()
        pending.conversationKey = key
        pending.temporaryTimestamp = preview.ts
        pending.message = preview
        chats.pendingSends[pendingId] = pending
        chats.convs[key]!.insertSorted(preview)

        composerText = ""
        pendingFiles = []
        replyQuote = Quote()
        if !files.isEmpty { showFlash("Sending …", seconds: 120) }
        scrollToMessage(index: chats.convs[key]!.msgs.firstIndex { $0.id == preview.id } ?? 0)

        Task { [p] in
            var result: Any?
            var err: String?
            do { result = try await cli.call("send", p) } catch { err = error.localizedDescription }
            guard let pendingNow = chats.pendingSends[pendingId] else { return }

            if let err, !pendingNow.echoReceived {
                chats.removePendingPreview(pendingNow)
                chats.pendingSends.removeValue(forKey: pendingId)
                showFlash("Sending failed: \(err)")
                if key != current {
                    // The user has moved on to another chat: put the unsent message back into that chat's
                    // draft, never into the composer of the chat that is open now.
                    var d = drafts[key] ?? Draft()
                    if d.isEmpty { d = Draft(text: text, reply: quote, files: files) }
                    drafts[key] = d.isEmpty ? nil : d
                    return
                }
                if composerText.isEmpty { // don't overwrite a newer draft
                    composerText = text
                    if pendingFiles.isEmpty { pendingFiles = files }
                    if !replyQuote.valid { replyQuote = quote }
                    composerCursorToEnd += 1
                }
                return
            }

            clearFlash()
            // An outgoing envelope is stronger evidence than a failed/late RPC response.
            // The preview is already confirmed and persisted in that case.
            if pendingNow.echoReceived {
                chats.pendingSends.removeValue(forKey: pendingId)
            } else {
                var ts = jsonInt64(jsonObject(result)["timestamp"])
                if ts <= 0 { ts = nowMs() }
                persist(chats.finalizePendingSend(pendingId, timestamp: ts))
                listChanged()
            }
            registerGiphyAnalytics(gifAnalytics)
            if pendingGifAnalyticsUrl == gifAnalytics { pendingGifAnalyticsUrl = nil }
        }
    }

    // MARK: Editing

    func startEdit(_ id: String) {
        guard let c = currentConversation, let (_, m) = message(withId: id) else { return }
        guard m.canEdit(nowMs: nowMs()) else {
            showFlash("This message can no longer be edited (3 hours / 10 edits).", seconds: 6)
            return
        }
        if editKey.isEmpty { editDraft = composerText } // keep an already running edit's draft
        editKey = c.key
        editTs = m.ts
        composerText = m.text
        composerCursorToEnd += 1
        composerFocusRequest += 1
    }

    /// ↑ in an empty composer edits the last own message.
    func editLastMessage() -> Bool {
        guard composerText.isEmpty, editKey.isEmpty, pendingFiles.isEmpty, let c = currentConversation else { return false }
        guard let m = c.msgs.last(where: { $0.outgoing && $0.authorKey == account && !chats.isPendingPreview(c.key, ts: $0.ts) })
        else { return false }
        startEdit(m.id)
        return true
    }

    func cancelEdit(restoreDraft: Bool = true) {
        guard !editKey.isEmpty else { return }
        editKey = ""
        editTs = 0
        if restoreDraft {
            composerText = editDraft
            composerCursorToEnd += 1
        }
        editDraft = ""
    }

    private func sendEdit(_ text: String) {
        guard let c = chats.convs[editKey], let i = chats.findMessage(c, ts: editTs, authors: [account]) else {
            cancelEdit()
            return
        }
        let m = c.msgs[i]
        if text.isEmpty {
            showFlash("A message cannot be empty.", seconds: 6)
            return
        }
        if text == m.text { // nothing changed
            cancelEdit()
            return
        }
        guard m.canEdit(nowMs: nowMs()) else {
            showFlash("This message can no longer be edited (3 hours / 10 edits).", seconds: 6)
            cancelEdit()
            return
        }
        // signal-cli: `send --edit-timestamp <ts>`; the target is the latest revision the peers know.
        var p: JSON = ["message": text, "editTimestamp": m.latestTs]
        if m.quote.valid && !m.quote.authorKey.isEmpty { // an edit keeps the reply it belongs to
            p["quoteTimestamp"] = m.quote.id
            p["quoteAuthor"] = m.quote.authorKey
            p["quoteMessage"] = m.quote.text
        }
        addRecipient(&p, c)
        let key = c.key, origTs = m.ts
        Task { [p] in
            do {
                let res = try await cli.call("send", p)
                clearFlash()
                var ts = jsonInt64(jsonObject(res)["timestamp"])
                if ts <= 0 { ts = nowMs() }
                if let rec = chats.applyEdit(key, targetTs: origTs, authors: [account], text: text, newTs: ts) { persist(rec) }
                if editKey == key && editTs == origTs { cancelEdit(restoreDraft: true) }
            } catch {
                // stay in edit mode with the typed text so nothing is lost
                showFlash("Editing failed: \(error.localizedDescription)")
            }
        }
    }

    // MARK: Reactions, deletion

    func sendReaction(_ id: String, _ emoji: String) {
        guard let c = currentConversation, let (_, m) = message(withId: id) else { return }
        let author = chats.effectiveAuthor(c, m)
        let remove = m.reactions[account] == emoji // same emoji again = take it back
        var p: JSON = ["emoji": emoji, "targetAuthor": author, "targetTimestamp": m.latestTs, "remove": remove]
        addRecipient(&p, c)
        let key = c.key, ts = m.ts
        Task { [p] in
            do {
                try await cli.call("sendReaction", p)
                if let rec = chats.applyReaction(key, ts: ts, authors: [author], reactor: account, emoji: remove ? "" : emoji) {
                    persist(rec)
                }
            } catch {
                showFlash("Reaction failed: \(error.localizedDescription)", seconds: 8)
            }
        }
    }

    func deleteMessage(_ id: String) async {
        guard let c = currentConversation, let (_, m) = message(withId: id) else { return }
        if chats.isPendingPreview(c.key, ts: m.ts) {
            showFlash("Wait until the message has finished sending before deleting it.", seconds: 6)
            return
        }
        let age = nowMs() - m.ts
        let forEveryone = m.outgoing && age >= 0 && age <= 24 * 60 * 60 * 1000
        let r = await Dialogs.ask("Delete message",
                                  forEveryone ? "Delete this message from this device or for everyone?" : "Delete this message from this device?",
                                  buttons: forEveryone ? ["Delete for me", "Delete for everyone"] : ["Delete for me"],
                                  style: .warning, cancel: true, destructive: forEveryone ? [0, 1] : [0])
        let key = c.key, ts = m.ts
        if r == 0 {
            removeMessage(key, ts: ts)
            return
        }
        guard r == 1 else { return }
        var p: JSON = ["targetTimestamp": ts]
        if c.isGroup { p["groupIds"] = [String(key.dropFirst(2))] } else { p["recipients"] = [chats.resolveKey(key)] }
        do {
            try await cli.call("remoteDelete", p)
            if removeMessage(key, ts: ts) { showFlash("Message deleted for everyone.", seconds: 5) }
        } catch {
            showFlash("Could not delete message for everyone: \(error.localizedDescription)")
        }
    }

    @discardableResult
    private func removeMessage(_ key: String, ts: Int64) -> Bool {
        guard let c = chats.convs[key], let i = chats.findMessage(c, ts: ts, authors: []) else { return false }
        let removed = chats.convs[key]!.msgs.remove(at: i)
        if editKey == key && editTs == removed.ts { cancelEdit() }
        do {
            try store.rewrite(chats.allRecords())
        } catch {
            chats.convs[key]!.insertSorted(removed)
            showFlash("Could not remove message from history: \(error.localizedDescription)")
            return false
        }
        updateSearch()
        return true
    }

    // MARK: - Attachments

    /// Queue files for the next message (attach button, drag & drop, paste).
    func addPendingFiles(_ paths: [String]) {
        for f in paths {
            var isDir: ObjCBool = false
            guard FileManager.default.fileExists(atPath: f, isDirectory: &isDir), !isDir.boolValue,
                  FileManager.default.isReadableFile(atPath: f) else { continue }
            let size = ((try? FileManager.default.attributesOfItem(atPath: f))?[.size] as? NSNumber)?.int64Value ?? 0
            if size > Media.maxAttachment {
                Task { await Dialogs.warn("Attach files", "“\((f as NSString).lastPathComponent)” is larger than 100 MB, the limit for Signal attachments.") }
                continue
            }
            let abs = (f as NSString).standardizingPath
            if !pendingFiles.contains(abs) { pendingFiles.append(abs) }
        }
        composerFocusRequest += 1
    }

    func chooseAttachments() {
        let panel = NSOpenPanel()
        panel.title = "Attach files"
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = false
        if panel.runModal() == .OK { addPendingFiles(panel.urls.map(\.path)) }
    }

    /// Files or an image (e.g. a screenshot) from drag & drop / the clipboard -> pending attachments.
    /// Returns false if there was nothing to attach (normal text paste continues then).
    func attach(from pb: NSPasteboard) -> Bool {
        // 1. files (copied in Finder, dropped, or a screenshot tool that saved a file)
        if let urls = pb.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL], !urls.isEmpty {
            addPendingFiles(urls.map(\.path))
            return true
        }
        // 2. raw image data (screenshot copied to the clipboard). If there is real text as well
        //    (e.g. copied from a document), the text wins.
        let text = pb.string(forType: .string)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        guard text.isEmpty, let image = NSImage(pasteboard: pb), let png = Media.pngData(image) else { return false }
        let f = DateFormatter()
        f.dateFormat = "yyyyMMdd-HHmmss"
        do {
            let url = try Media.writeTemp(png, name: "pasted-\(f.string(from: Date())).png")
            addPendingFiles([url.path])
            return true
        } catch {
            showFlash("The image from the clipboard could not be used.", seconds: 8)
            return false
        }
    }

    func attachmentToken(_ m: Message, _ i: Int) -> String { "\(m.id)#\(i)" }

    private func fetchThumbnails(_ key: String, ts: Int64, author: String, attempt: Int = 0) {
        guard let c = chats.convs[key], let mi = chats.findMessage(c, ts: ts, authors: [author]) else { return }
        let m = c.msgs[mi]
        for (a, att) in m.attachments.enumerated() {
            if !att.isImage || !att.thumb.isEmpty || att.id.isEmpty || att.size > Media.maxThumbSource { continue }
            let token = "\(key)|\(ts)|\(author)|\(a)"
            if thumbPending.contains(token) { continue }
            thumbPending.insert(token)
            var p: JSON = ["id": att.id]
            addRecipient(&p, c)
            Task { [p] in
                defer { thumbPending.remove(token) }
                let data: Data
                do {
                    let res = try await cli.call("getAttachment", p)
                    data = Data(base64Encoded: jsonString(jsonObject(res)["data"])) ?? Data()
                } catch {
                    // signal-cli may still be downloading it: retry a few times
                    if attempt < 3 {
                        try? await Task.sleep(for: .seconds(2 * (attempt + 1)))
                        thumbPending.remove(token)
                        fetchThumbnails(key, ts: ts, author: author, attempt: attempt + 1)
                    }
                    return
                }
                let thumb = Media.makeThumbnail(data)
                guard !thumb.isEmpty, let c2 = chats.convs[key], let i = chats.findMessage(c2, ts: ts, authors: [author]),
                      a < c2.msgs[i].attachments.count else { return }
                if let (w, h) = Media.pixelSize(data) {
                    chats.convs[key]!.msgs[i].attachments[a].width = w
                    chats.convs[key]!.msgs[i].attachments[a].height = h
                }
                chats.convs[key]!.msgs[i].attachments[a].thumb = thumb
                persist(Records.attachmentsUpdate(key, chats.convs[key]!.msgs[i]))
            }
        }
    }

    func fetchAttachmentData(_ key: String, _ att: Attachment) async throws -> Data {
        if !att.path.isEmpty, let d = FileManager.default.contents(atPath: att.path) { return d }
        guard !att.id.isEmpty, let c = chats.convs[key] else { throw RpcError("The attachment is no longer available.") }
        var p: JSON = ["id": att.id]
        addRecipient(&p, c)
        let res = try await cli.call("getAttachment", p)
        return Data(base64Encoded: jsonString(jsonObject(res)["data"])) ?? Data()
    }

    /// Animated GIFs: the stored preview is a still image, so the original is fetched on demand.
    func loadAnimatedGif(_ m: Message, _ i: Int) {
        guard let key = current, i < m.attachments.count else { return }
        let att = m.attachments[i]
        let token = attachmentToken(m, i)
        guard att.isGif, att.size <= Media.maxThumbSource, animatedGifs[token] == nil, !gifPending.contains(token) else { return }
        gifPending.insert(token)
        Task {
            defer { gifPending.remove(token) }
            if let data = try? await fetchAttachmentData(key, att), !data.isEmpty { animatedGifs[token] = data }
        }
    }

    func openAttachment(_ m: Message, _ i: Int) {
        guard let key = current, i < m.attachments.count else { return }
        let att = m.attachments[i]
        if att.isAudio {
            toggleAudio(m, i)
            return
        }
        if att.isVideo {
            let token = attachmentToken(m, i)
            if videoURLs[token] != nil || videoLoading.contains(token) { return }
            videoLoading.insert(token)
            Task {
                defer { videoLoading.remove(token) }
                do {
                    let data = try await fetchAttachmentData(key, att)
                    guard !data.isEmpty else { throw RpcError("The video file is empty.") }
                    var suffix = (att.filename as NSString).pathExtension
                    if suffix.isEmpty { suffix = Media.preferredSuffix(forMime: att.contentType) }
                    videoURLs[token] = try Media.writeTemp(data, name: "video." + (suffix.isEmpty ? "mp4" : suffix))
                } catch {
                    showFlash("Video playback failed: \(error.localizedDescription)")
                }
            }
            return
        }
        if !att.isImage && Media.isRisky(att.filename) {
            Task {
                await Dialogs.info("Attachment", "For safety, “\(att.filename)” is not opened directly. Use “Save …” from the message menu instead.")
            }
            return
        }
        showFlash("Loading attachment …", seconds: 60)
        Task {
            defer { clearFlash() }
            let data: Data
            do {
                data = try await fetchAttachmentData(key, att)
            } catch {
                if att.isImage && !att.thumb.isEmpty { // fall back to the stored preview
                    ImageViewerWindow.show(data: att.thumb, title: att.filename)
                } else {
                    await Dialogs.warn("Attachment", error.localizedDescription)
                }
                return
            }
            if att.isImage && NSImage(data: data) != nil {
                ImageViewerWindow.show(data: data, title: att.filename)
                return
            }
            var name = (att.filename as NSString).lastPathComponent
            if name.isEmpty { name = "attachment." + Media.preferredSuffix(forMime: att.contentType) }
            do {
                NSWorkspace.shared.open(try Media.writeTemp(data, name: name))
            } catch {
                await Dialogs.warn("Attachment", "The file could not be written.")
            }
        }
    }

    func saveAttachment(_ m: Message, _ i: Int) {
        guard let key = current, i < m.attachments.count else { return }
        let att = m.attachments[i]
        showFlash("Loading attachment …", seconds: 60)
        Task {
            let data: Data
            do { data = try await fetchAttachmentData(key, att) } catch {
                clearFlash()
                await Dialogs.warn("Attachment", error.localizedDescription)
                return
            }
            clearFlash()
            let panel = NSSavePanel()
            panel.title = "Save attachment"
            panel.nameFieldStringValue = (att.filename as NSString).lastPathComponent
            guard panel.runModal() == .OK, let url = panel.url else { return }
            do { try data.write(to: url) } catch { await Dialogs.warn("Attachment", "The file could not be written.") }
        }
    }

    // MARK: Audio

    func toggleAudio(_ m: Message, _ i: Int) {
        guard let key = current, i < m.attachments.count else { return }
        let token = attachmentToken(m, i)
        if audioToken == token {
            if audioLoading { return }
            if let player = audioPlayer {
                if audioPlaying {
                    player.pause()
                    audioPlaying = false
                } else {
                    if audioDuration > 0 && audioPosition >= audioDuration - 0.05 { player.seek(to: .zero) }
                    player.play()
                    audioPlaying = true
                }
                return
            }
        }
        stopAudio()
        audioToken = token
        audioLoading = true
        let att = m.attachments[i]
        Task {
            do {
                let data = try await fetchAttachmentData(key, att)
                guard audioToken == token else { return }
                guard !data.isEmpty else { throw RpcError("The audio file is empty.") }
                var suffix = (att.filename as NSString).pathExtension
                if suffix.isEmpty { suffix = Media.preferredSuffix(forMime: att.contentType) }
                let url = try Media.writeTemp(data, name: "audio." + (suffix.isEmpty ? "m4a" : suffix))
                let player = AVPlayer(url: url)
                audioPlayer = player
                audioObserver = player.addPeriodicTimeObserver(forInterval: CMTime(value: 1, timescale: 10), queue: .main) { [weak self] t in
                    MainActor.assumeIsolated {
                        guard let self else { return }
                        self.audioPosition = t.seconds
                        if let d = player.currentItem?.duration.seconds, d.isFinite { self.audioDuration = d }
                    }
                }
                audioEndObserver = NotificationCenter.default.addObserver(forName: .AVPlayerItemDidPlayToEndTime,
                                                                          object: player.currentItem, queue: .main) { [weak self] _ in
                    MainActor.assumeIsolated { self?.audioPlaying = false }
                }
                audioLoading = false
                player.play()
                audioPlaying = true
            } catch {
                audioLoading = false
                audioToken = nil
                showFlash("Audio playback failed: \(error.localizedDescription)")
            }
        }
    }

    private func stopAudio() {
        audioPlayer?.pause()
        if let o = audioObserver { audioPlayer?.removeTimeObserver(o) }
        if let o = audioEndObserver { NotificationCenter.default.removeObserver(o) }
        audioObserver = nil
        audioEndObserver = nil
        audioPlayer = nil
        audioPlaying = false
        audioLoading = false
        audioPosition = 0
        audioDuration = 0
        audioToken = nil
    }

    // MARK: - Avatars

    func avatarImage(_ c: Conversation) -> NSImage {
        avatars[c.key] ?? Media.placeholderAvatar(key: c.key, title: c.title)
    }

    /// Avatars are needed for the contact list and for notifications (unless those hide the sender).
    private var avatarsWanted: Bool {
        appearance.showAvatars || (notify.notifications && notify.content != .hidden)
    }

    func fetchMissingAvatars() {
        // Only while connected (otherwise the request would just fail and never be retried).
        guard avatarsWanted, cli.isRunning else { return }
        for key in chats.convs.keys {
            if avatarInFlight >= 4 { return } // continues when a request finishes
            if avatarTried.contains(key) || avatarBusy.contains(key) { continue }
            fetchAvatar(key)
        }
    }

    private func refreshAvatar(_ key: String) {
        guard avatarsWanted, cli.isRunning, !avatarBusy.contains(key) else { return }
        if nowMs() - (avatarFetchedMs[key] ?? 0) < 60_000 { return } // a burst of messages is one request
        fetchAvatar(key)
    }

    private func fetchAvatar(_ key: String) {
        guard let c = chats.convs[key] else { return }
        avatarTried.insert(key)
        avatarBusy.insert(key)
        avatarFetchedMs[key] = nowMs()
        avatarInFlight += 1
        let gen = connectGen
        Task {
            func dataOf(_ params: JSON) async -> Data {
                guard let res = try? await cli.call("getAvatar", params) else { return Data() }
                return Data(base64Encoded: jsonString(jsonObject(res)["data"])) ?? Data()
            }
            var data: Data
            if c.isGroup {
                data = await dataOf(["groupId": String(key.dropFirst(2))])
            } else {
                // Contact avatar first (set in the address book), then the person's own profile picture.
                data = await dataOf(["contact": key])
                if data.isEmpty { data = await dataOf(["profile": key]) }
            }
            guard gen == connectGen else { return } // reconnect in between: everything restarts anyway
            avatarInFlight = max(0, avatarInFlight - 1)
            avatarBusy.remove(key)
            if let img = data.isEmpty ? nil : NSImage(data: data) {
                let digest = data.hashValue
                if avatarDigest[key] != digest || avatars[key] == nil { // same picture: nothing to redraw
                    avatarDigest[key] = digest
                    avatars[key] = Media.roundAvatar(img)
                }
            } else if avatars[key] != nil {
                // The contact removed the picture: back to the letter placeholder.
                avatars.removeValue(forKey: key)
                avatarDigest.removeValue(forKey: key)
            }
            fetchMissingAvatars()
        }
    }

    // MARK: - Settings

    func applyAppearance(_ a: Appearance) {
        appearance = a
        a.save()
        fetchMissingAvatars()
    }

    func applyNotifications(_ n: NotificationSettings) {
        notify = n
        n.save()
        if n.notifications { Task { _ = await notifier.requestAuthorization() } }
    }

    // MARK: - Profile

    func loadSignalUsername() {
        guard connected else {
            profileUsername = "unavailable"
            return
        }
        Task {
            do {
                let res = try await cli.call("listContacts", ["recipients": [account]])
                let me = jsonArray(res).map(jsonObject).first { jsonString($0["number"]) == account || jsonString($0["uuid"]) == account }
                guard let me else {
                    profileUsername = "unavailable"
                    return
                }
                let u = jsonString(me["username"])
                profileUsername = u.isEmpty ? "not set" : u
            } catch {
                profileUsername = "unavailable"
                reportProfileStatus("Could not read your Signal username: \(error.localizedDescription)", error: true)
            }
        }
    }

    private func reportProfileStatus(_ text: String, error: Bool) {
        profileStatus = (text, error)
        showFlash(text)
    }

    func applySignalUsername(_ username: String) {
        guard connected else { return reportProfileStatus("Connect to signal-cli before editing your profile.", error: true) }
        Task {
            do {
                let res = try await cli.call("updateAccount", ["username": username, "deleteUsername": false])
                let updated = jsonString(jsonObject(res)["username"])
                if !updated.isEmpty { profileUsername = updated }
                reportProfileStatus(updated.isEmpty ? "Signal username updated." : "Signal username: \(updated)", error: false)
            } catch {
                reportProfileStatus("Could not change your Signal username: \(error.localizedDescription)", error: true)
            }
        }
    }

    func sendProfileUpdate(_ params: JSON, success: String) {
        guard connected else { return reportProfileStatus("Connect to signal-cli before editing your profile.", error: true) }
        Task {
            do {
                try await cli.call("updateProfile", params)
                reportProfileStatus(success, error: false)
            } catch {
                reportProfileStatus("Could not update your Signal profile: \(error.localizedDescription)", error: true)
            }
        }
    }

    // MARK: - GIPHY

    func searchGifs() async {
        guard current != nil else {
            showFlash("Open a conversation before searching for GIFs.", seconds: 5)
            return
        }
        var key: String
        do { key = try GiphyApiKeyStore.load() } catch {
            await Dialogs.warn("GIPHY API key", error.localizedDescription)
            return
        }
        if key.isEmpty {
            switch await Dialogs.ask("GIPHY API key required", "GIPHY requires an API key to search and send GIFs.",
                                     buttons: ["Configure API key …", "Get an API key"], cancel: true) {
            case 0:
                await configureGiphyApiKey()
                key = (try? GiphyApiKeyStore.load()) ?? ""
                if key.isEmpty { return }
            case 1:
                NSWorkspace.shared.open(URL(string: "https://developers.giphy.com/dashboard/")!)
                return
            default:
                return
            }
        }
        giphyApiKey = key
    }

    func configureGiphyApiKey() async {
        let currentKey: String
        do { currentKey = try GiphyApiKeyStore.load() } catch {
            await Dialogs.warn("GIPHY API key", error.localizedDescription)
            return
        }
        guard let k = await Dialogs.text("GIPHY API key", "Enter your GIPHY API key. Create one at developers.giphy.com/dashboard. "
                                            + "Leave it empty to remove the key.", initial: currentKey, secure: true) else { return }
        do { try GiphyApiKeyStore.save(k) } catch { await Dialogs.warn("GIPHY API key", error.localizedDescription) }
    }

    func sendGif(url: URL, analytics: URL?) {
        giphyApiKey = nil
        guard let key = current, url.scheme == "https" else {
            showFlash("The selected GIF could not be downloaded.", seconds: 8)
            return
        }
        showFlash("Downloading GIF …", seconds: 60)
        Task {
            var req = URLRequest(url: url)
            req.setValue("wren", forHTTPHeaderField: "User-Agent")
            do {
                let (data, resp) = try await URLSession.shared.data(for: req)
                guard (resp as? HTTPURLResponse)?.statusCode == 200, !data.isEmpty, Int64(data.count) <= Media.maxAttachment else {
                    throw RpcError("unexpected response")
                }
                guard current == key else {
                    showFlash("GIF sending cancelled because the active conversation changed.", seconds: 8)
                    return
                }
                let file = try Media.writeTemp(data, name: "giphy-\(UUID().uuidString).gif")
                clearFlash()
                pendingGifAnalyticsUrl = analytics
                addPendingFiles([file.path])
                sendMessage()
            } catch {
                showFlash("Could not download the selected GIF: \(error.localizedDescription)")
            }
        }
    }

    private func registerGiphyAnalytics(_ url: URL?) {
        guard let url, url.scheme == "https" else { return }
        var req = URLRequest(url: url)
        req.setValue("wren", forHTTPHeaderField: "User-Agent")
        URLSession.shared.dataTask(with: req).resume()
    }

    // MARK: - Update check, about

    static func compareVersions(_ a: String, _ b: String) -> ComparisonResult {
        let pa = a.split(separator: ".").map { Int($0.prefix { $0.isNumber }) ?? 0 }
        let pb = b.split(separator: ".").map { Int($0.prefix { $0.isNumber }) ?? 0 }
        for i in 0..<max(pa.count, pb.count) {
            let x = i < pa.count ? pa[i] : 0, y = i < pb.count ? pb[i] : 0
            if x != y { return x < y ? .orderedAscending : .orderedDescending }
        }
        return .orderedSame
    }

    func checkForUpdates(silent: Bool) {
        Task {
            var req = URLRequest(url: URL(string: "https://api.github.com/repos/AsamK/signal-cli/releases/latest")!)
            req.setValue("wren", forHTTPHeaderField: "User-Agent")
            req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
            let o: JSON
            do {
                let (data, _) = try await URLSession.shared.data(for: req)
                o = (try JSONSerialization.jsonObject(with: data)) as? JSON ?? [:]
            } catch {
                if !silent { await Dialogs.warn("Update check", "Could not reach GitHub:\n\(error.localizedDescription)") }
                return
            }
            var latest = jsonString(o["tag_name"])
            if latest.hasPrefix("v") { latest.removeFirst() }
            let url = URL(string: jsonString(o["html_url"])) ?? URL(string: "https://github.com/AsamK/signal-cli/releases")!
            let newer = !latest.isEmpty && AppModel.compareVersions(latest, version) == .orderedDescending
            updateHint = newer ? UpdateHint(version: latest, url: url) : nil
            if silent { return }
            if newer {
                let r = await Dialogs.ask("Update available",
                                          "Installed: signal-cli \(version)\nLatest version: signal-cli \(latest)\n\nUpdate signal-cli the way you "
                                              + "usually do (e.g. brew upgrade signal-cli) – this app does not need to be changed for that. "
                                              + "Afterwards choose “File → Reconnect”.",
                                          buttons: ["Open release page", "Close"])
                if r == 0 { NSWorkspace.shared.open(url) }
            } else {
                await Dialogs.info("Update check", version.isEmpty ? "The installed signal-cli version is not known yet."
                                       : "signal-cli \(version) is up to date.")
            }
        }
    }

    func showAbout() {
        let credits = NSMutableAttributedString(
            string: "Graphical front end for signal-cli.\nsignal-cli is used as a standalone program and is not part of this app.\n\n"
                + "Path: \(cli.executable)\nVersion: \(version)\n\nChat history (encrypted): \(AppPaths.historyPath)",
            attributes: [.font: NSFont.systemFont(ofSize: 11), .foregroundColor: NSColor.labelColor])
        NSApp.orderFrontStandardAboutPanel(options: [.credits: credits, .applicationName: "Wren"])
        NSApp.activate(ignoringOtherApps: true)
    }
}
