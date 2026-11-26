import AppKit
import AVFoundation
import Observation
import CorvusCore
import SwiftUI

/// Unsent composer state of one conversation.
struct Draft {
    var text = ""
    var reply = Quote()
    var files: [String] = []
    var gifAnalyticsUrl: URL?
    var mentions: [ComposerMention] = []
    var styles: [TextStyleRange] = []
    var isEmpty: Bool { text.isEmpty && !reply.valid && files.isEmpty }
}

/// Group dialogs shown as sheets on the main window.
enum GroupSheet: Identifiable, Equatable {
    case create
    case settings(groupId: String)
    case invitations

    var id: String {
        switch self {
        case .create: return "create"
        case .settings(let g): return "settings:" + g
        case .invitations: return "invitations"
        }
    }
}

/// Dialogs about the open chat or one of its messages, shown as sheets on the main window.
enum ChatSheet: Identifiable, Equatable {
    case messageDetails(String) // Message.id
    case safetyNumber(String)   // conversation key (1:1)
    case forward(String)        // Message.id in the open chat

    var id: String {
        switch self {
        case .messageDetails(let m): return "details:" + m
        case .forward(let m): return "forward:" + m
        case .safetyNumber(let k): return "safety:" + k
        }
    }
}

/// An unacknowledged change of a contact's safety number (shown as a bar in the chat).
struct SafetyChange: Equatable {
    var recipient: String
    var digits: String
    var blocked: Bool // corvus-cli refuses to send until the new key is verified
}

/// A person who can be added to a group: key (number / UUID / username) and display name.
struct GroupContact: Identifiable, Hashable {
    var id: String
    var name: String
}

struct UpdateHint: Equatable {
    var backend: String             // "corvus-cli" / "signal-cli"
    var version: String
    var url: URL
}

/// Everything the main window shows, plus the logic of src/MainWindow.cpp that is not pure model
/// (that part lives in CorvusCore.ChatState).
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
    var version = ""                // signal-cli version (for corvus-cli: the one it is compatible with); feature checks use it
    var backend = BackendInfo()     // which backend runs: corvus-cli (recommended) or signal-cli
    var historyProtectionMode = ""  // "keychain" | "passphrase"
    var historyReady = false

    var appearance = Appearance.load()
    var notify = NotificationSettings.load()
    var privacy = ChatPrivacy.load()

    // typing indicators of others: conversation key -> sender key -> expiry (ms)
    var typing: [String: [String: Int64]] = [:]
    // own typing indicator: the chat it was last sent to and when
    @ObservationIgnored private var typingKey: String?
    @ObservationIgnored private var typingSentMs: Int64 = 0
    @ObservationIgnored private var typingStopTask: Task<Void, Never>?
    // incoming messages whose read receipt is due once their chat is read: conversation key -> (author, ts)
    @ObservationIgnored private var unsentReads: [String: [(author: String, ts: Int64)]] = [:]

    // composer (for the current conversation; other conversations keep a Draft)
    var composerText = "" { didSet { if composerText != oldValue { updateDraftLinkPreview() } } }
    // link preview of the composer's first HTTPS link
    var draftLinkPreview = LinkPreview()
    var linkPreviewLoading = false
    @ObservationIgnored private var previewRequestedUrl = ""
    @ObservationIgnored private var dismissedPreviewUrl = ""
    @ObservationIgnored private var linkPreviewTask: Task<Void, Never>?
    @ObservationIgnored private var sendAfterLinkPreview: String?  // composer text when Send waited for the preview
    var replyQuote = Quote()
    var pendingFiles: [String] = []
    var pendingGifAnalyticsUrl: URL?
    var editKey = ""                // message being edited (conversation); empty = not editing
    var editTs: Int64 = 0
    private var editDraft = ""
    private var editDraftStyles: [TextStyleRange] = []
    var composerFocusRequest = 0    // incremented to put the caret into the composer
    // voice messages
    var recording = false           // a voice message is being recorded (the record bar replaces the field)
    var recordingMs: Int64 = 0
    var voiceFiles: [String: Int64] = [:]   // recorded files (pending or in a draft) -> duration (ms)
    @ObservationIgnored private var recordKey: String?
    @ObservationIgnored private var recordSendWhenDone = false
    @ObservationIgnored private let recorder = VoiceRecorder()
    var composerCursorToEnd = 0
    private var drafts: [String: Draft] = [:]

    // @mentions in the message field (groups)
    var mentionSuggestions: [GroupContact] = []
    var mentionSelection = 0
    @ObservationIgnored var mentionTrigger: (start: Int, end: Int)?  // "@query" being replaced (UTF-16)
    @ObservationIgnored var composerMentions: [ComposerMention] = []  // kept up to date by the message field
    @ObservationIgnored var composerStyles: [TextStyleRange] = []     // bold, italic, … (kept up to date by the message field)
    @ObservationIgnored private var groupMembers: [String: [String]] = [:] // "g:<id>" -> member keys (without us)

    // chat list search (⌘K) and keyboard navigation
    var chatFilter = ""
    var chatSearchFocusRequest = 0          // incremented to put the caret into the chat list's search field
    @ObservationIgnored var chatSearchFocused = false

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

    // stickers
    var stickerPacks: [StickerPack] = []
    var stickerPacksLoaded = false
    var stickerStatus: (text: String, error: Bool)?
    var stickerImages: [String: Data] = [:]          // "packId:stickerId" -> image (WebP / APNG / PNG)
    @ObservationIgnored private var stickerPacksLoading = false
    @ObservationIgnored private var stickerQueue: [(pack: String, id: Int)] = []
    @ObservationIgnored private var stickerRequested: Set<String> = []
    @ObservationIgnored private var stickerLoadsInFlight = 0

    // groups
    var groupSheet: GroupSheet?
    /// Contacts corvus-cli marks as hidden: the senders of message requests (key -> name).
    var hiddenContacts: [String: String] = [:]
    var messageRequestsShown = false
    var safetyChanged: [String: SafetyChange] = [:]   // conversation key -> unacknowledged change
    @ObservationIgnored private var knownSafety: [String: String] = [:] // recipient -> accepted digits
    @ObservationIgnored private var knownSafetyAccount = ""
    @ObservationIgnored private var identityCheckedMs: [String: Int64] = [:]
    var chatSheet: ChatSheet?
    var groupInvitations: [GroupInfo] = []  // groups we are invited to (not yet a member)

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
        cli.onAttachmentDownloaded = { [weak self] id in self?.onAttachmentDownloaded(id) }
        cli.onStopped = { [weak self] reason in self?.setConnected(false, info: reason) }
        cli.onLogLine = { [weak self] line in self?.showFlash(line, seconds: 5) }
        notifier.onActivate = { [weak self] key in self?.activateConversation(key) }
        recorder.onDuration = { [weak self] ms in self?.recordingMs = ms }
        recorder.onFinished = { [weak self] path, ms in self?.onVoiceRecorded(path, ms) }
        recorder.onFailed = { [weak self] error in
            guard let self else { return }
            recording = false
            recordKey = nil
            recordSendWhenDone = false
            Task { await Dialogs.warn("Voice message", "The recording failed: \(error)") }
        }
    }

    // MARK: - Start

    func start() {
        guard !started else { return }
        started = true
        notifier.setUp()
        timers.append(Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.expireMessages()
                self?.expireTyping()
            }
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
        cancelVoiceRecording() // the temporary folder with the recording goes away below
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

    // MARK: - Signal backend (corvus-cli)

    /// "corvus-cli" or "signal-cli": the connected backend, else what the chosen executable looks like.
    var backendName: String { backend.version.isEmpty ? cli.backendName : backend.name }

    func ensureExecutable() async -> Bool {
        var exe = UserDefaults.standard.string(forKey: "executable") ?? ""
        if exe.isEmpty || !FileManager.default.fileExists(atPath: exe) { exe = SignalCli.autodetect() }
        while exe.isEmpty {
            let r = await Dialogs.ask("Signal backend not found",
                                      "corvus-cli was not found.\n\nThis app needs corvus-cli as its Signal backend:\n\n"
                                          + "brew tap ada-h00k/corvus\nbrew install --cask corvus-cli",
                                      buttons: ["Choose path …", "Get corvus-cli", "Close"], style: .warning)
            switch r {
            case 0: exe = pickExecutable() ?? ""
            case 1: NSWorkspace.shared.open(URL(string: "https://github.com/ada-h00k/corvus-cli/releases/latest")!)
            default: return false
            }
        }
        UserDefaults.standard.set(exe, forKey: "executable")
        cli.executable = exe
        return true
    }

    private func pickExecutable() -> String? {
        let panel = NSOpenPanel()
        panel.title = "Select corvus-cli"
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

    /// Everything here is asynchronous so the window never freezes while the backend starts.
    /// The version is read on every (re)connect, so an update of corvus-cli is noticed without restarting.
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
            await showCliProblem("\(cli.backendName) could not be run.", vr)
            return
        }
        backend = BackendInfo.parse(versionLine: versionLine)
        version = backend.signalVersion

        let (ar, accounts) = await cli.queryAccounts()
        guard gen == connectGen else { return }
        // A failing command is NOT "no account": report what actually went wrong.
        if !ar.ok && accounts.isEmpty {
            await showCliProblem("The accounts could not be listed.", ar)
            return
        }
        if accounts.isEmpty {
            setConnected(false, info: "no account linked")
            if await Dialogs.confirm("No account", "\(backendName) reports that no account is set up yet.\nLink this device with your phone now?",
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
        chats.ensureNoteToSelf()
        cli.start(account: acc)
        guard cli.isRunning else { return }
        setConnected(true)
        loadContactsAndGroups()
        checkForUpdates(silent: true) // silent: only a hint in the status bar
    }

    private func showCliProblem(_ title: String, _ r: CliResult) async {
        setConnected(false, info: title)
        let info = r.describe() + "\n\nCommand: " + SignalCli.maskNumbers(r.command)
            + "\n\nThis does not mean that no account is linked. Check that the command works in a terminal."
        switch await Dialogs.ask(title, info, buttons: ["Retry", "Choose backend …", "Close"], style: .warning) {
        case 0: await connectAccount()
        case 1: chooseExecutable()
        default: break
        }
    }

    func setConnected(_ on: Bool, info: String = "") {
        connected = on
        if on {
            statusText = "Connected · \(backend.description)"
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
        resetStickers() // another account may have other packs
        avatarTried.removeAll() // ask signal-cli again, avatars may have changed
        avatarBusy.removeAll()
        avatarInFlight = 0
        Task {
            do {
                let res = try await cli.call("listContacts")
                var hidden: [String: String] = [:]
                defer { hiddenContacts = hidden }
                for cv in jsonArray(res) {
                    let c = jsonObject(cv)
                    let number = jsonString(c["number"]), uuid = jsonString(c["uuid"])
                    let key = !number.isEmpty ? number : uuid
                    if key.isEmpty { continue }
                    if !number.isEmpty && !uuid.isEmpty { chats.alias[uuid] = number }
                    if chats.isNoteToSelf(key) { continue } // fixed title, managed locally

                    var title = jsonString(c["name"])
                    if title.isEmpty { title = jsonString(c["profileName"]) }
                    if !title.isEmpty { chats.contactNames[key] = title }

                    // signal-cli lists every known recipient (address book, group members, …), not just people
                    // we have chatted with. Only show a chat once there is one: local history, a new message,
                    // or an archived chat on the phone.
                    let sig = jsonBool(c["isArchived"])
                    // Hidden contacts are the message requests (like the official client): offered in
                    // the "Message requests" dialog until accepted, declined or blocked.
                    if jsonBool(c["isHidden"]) {
                        hidden[key] = title.isEmpty ? key : title
                        continue
                    }
                    if chats.convs[key] == nil && !sig { continue }

                    chats.ensure(key, title: key)
                    let blocked = jsonBool(c["isBlocked"])
                    if blocked != chats.convs[key]!.blocked { // e.g. blocked on the phone: follow corvus-cli
                        chats.convs[key]!.blocked = blocked
                        persist(Records.blocked(key, blocked))
                    }
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
                checkIdentities() // any safety number that changed while Corvus was not running
            } catch {
                showFlash("Could not load contacts: \(error.localizedDescription)", seconds: 8)
            }
        }
        Task { await refreshGroups() }
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
                let secret: String
                do { secret = try HistoryKeychain.generateSecret() } catch {
                    await Dialogs.error("Chat history", "Could not generate the history key:\n\(error.localizedDescription)")
                    return false
                }
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
        chats.ensureNoteToSelf()
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
                let newSecret: String
                do { newSecret = try HistoryKeychain.generateSecret() } catch {
                    await Dialogs.error("Chat history", "Could not generate the history key:\n\(error.localizedDescription)")
                    return
                }
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
        if chats.isNoteToSelf(key) { return } // the chat with ourselves keeps its fixed title
        chats.ensure(key)
        let c = chats.convs[key]!
        if !c.storedName.isEmpty || c.isGroup { return }
        if c.title != c.key && c.title != name && c.localNickname.isEmpty { return } // a saved contact name wins
        chats.convs[key]!.storedName = name
        if c.localNickname.isEmpty { chats.convs[key]!.title = name }
        persist(Records.name(key, name))
    }

    func setNickname(_ key: String, _ nickname: String) {
        if chats.isNoteToSelf(key) { return } // nicknames are for contacts, not for ourselves
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

    // MARK: - Sending a prepared message

    /// Shows `preview` in chat `key` right away and sends `params`; the preview is confirmed (and written to
    /// the history) when the send succeeds, removed when it fails. Used by forwarding, polls and contact cards.
    func sendWithPreview(_ key: String, params: JSON, preview template: Message, failure: String,
                         cleanup: URL? = nil, done: ((Int64?) -> Void)? = nil) {
        guard chats.convs[key] != nil else { return }
        var preview = template
        preview.ts = nowMs()
        while chats.findMessage(chats.convs[key]!, ts: preview.ts, authors: []) != nil { preview.ts += 1 }
        preview.orderTs = max(preview.ts, (chats.convs[key]!.msgs.last?.sortTs ?? 0) + 1)
        preview.outgoing = true
        preview.authorKey = account
        preview.expiresInSeconds = chats.convs[key]!.expirationSeconds
        let pendingId = UUID().uuidString
        var pending = PendingSend()
        pending.conversationKey = key
        pending.temporaryTimestamp = preview.ts
        pending.message = preview
        chats.pendingSends[pendingId] = pending
        chats.convs[key]!.insertSorted(preview)
        if key == current { scrollToMessage(index: chats.convs[key]!.msgs.firstIndex { $0.id == preview.id } ?? 0) }
        listChanged()

        Task { [params] in
            var result: Any?
            var err: String?
            do { result = try await cli.call("send", params) } catch { err = error.localizedDescription }
            checkSendOutcome(key, result: result, error: err)
            if let cleanup { try? FileManager.default.removeItem(at: cleanup) }
            guard let pendingNow = chats.pendingSends[pendingId] else { return }
            if let err, !pendingNow.echoReceived {
                chats.removePendingPreview(pendingNow)
                chats.pendingSends.removeValue(forKey: pendingId)
                showFlash("\(failure): \(err)", seconds: 9)
                listChanged()
                done?(nil)
                return
            }
            var ts = jsonInt64(jsonObject(result)["timestamp"])
            if pendingNow.echoReceived {
                chats.pendingSends.removeValue(forKey: pendingId)
            } else {
                if ts <= 0 { ts = nowMs() }
                persist(chats.finalizePendingSend(pendingId, timestamp: ts))
                listChanged()
            }
            done?(ts)
        }
    }

    // MARK: - Forwarding

    /// Chats a message can be forwarded to (not blocked ones), by name.
    var forwardDestinations: [Conversation] {
        chats.convs.values.filter { !$0.blocked }.sorted {
            let c = $0.title.localizedCaseInsensitiveCompare($1.title)
            return c == .orderedSame ? $0.key < $1.key : c == .orderedAscending
        }
    }

    static let maxForwardTargets = 5

    func canForward(_ m: Message) -> Bool {
        // A View Once item is never re-uploaded from the desktop (as in the Qt app).
        !m.deleted && !m.viewOnce && !m.hasPoll && (!m.text.isEmpty || !m.attachments.isEmpty)
    }

    /// Forwards a message of the open chat to up to five chats, with an optional note above it (port of the
    /// Qt app's forwardMessage): attachments are fetched once and sent again as new uploads.
    func forward(_ id: String, to targets: [String], note: String) async {
        guard let sourceKey = current, let (_, source) = message(withId: id), canForward(source),
              !targets.isEmpty, targets.count <= AppModel.maxForwardTargets else { return }
        let note = note.trimmingCharacters(in: .whitespacesAndNewlines)
        let directSticker = source.text.isEmpty && source.attachments.count == 1 && source.attachments[0].isValidSticker && note.isEmpty
        let stage = FileManager.default.temporaryDirectory.appendingPathComponent("corvus-forward-" + UUID().uuidString)
        var files: [String] = []
        var datas: [Data] = []
        if !directSticker {
            do {
                try FileManager.default.createDirectory(at: stage, withIntermediateDirectories: true)
                var total: Int64 = 0
                for (i, att) in source.attachments.enumerated() {
                    if att.size > Media.maxAttachment { throw RpcError("The attachment is too large to forward.") }
                    let data = try await fetchAttachmentData(sourceKey, att)
                    total += Int64(data.count)
                    if data.isEmpty || total > Media.maxAttachment { throw RpcError("The attachment could not be loaded.") }
                    var name = (att.filename as NSString).lastPathComponent
                    if att.isSticker { name = "sticker.webp" }
                    if name.isEmpty || name == "." || name == ".." { name = "attachment" }
                    let dir = stage.appendingPathComponent("named-\(i)")
                    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                    let file = dir.appendingPathComponent(name)
                    try data.write(to: file)
                    files.append(file.path)
                    datas.append(data)
                }
            } catch {
                try? FileManager.default.removeItem(at: stage)
                showFlash("Forwarding failed: \(error.localizedDescription)", seconds: 9)
                return
            }
        }
        let shown = displayText(source)
        let prefix = note.isEmpty ? "" : note + "\n\n"
        let prefixLength = (prefix as NSString).length
        let text = prefix + shown.text
        let styles = shown.styles.map { TextStyleRange(start: $0.start + prefixLength, length: $0.length, style: $0.style) }

        var remaining = targets.filter { chats.convs[$0].map { !$0.blocked } ?? false }
        if remaining.isEmpty { try? FileManager.default.removeItem(at: stage); return }
        for key in remaining {
            // Every destination gets its own staged copy: corvus-cli may move the files it uploads.
            let staged = directSticker || files.isEmpty ? (paths: [String](), dir: stage) : Media.stageForSignalCli(files)
            var p: JSON = [:]
            if !text.isEmpty {
                p["message"] = text
                if !styles.isEmpty { p["textStyle"] = TextStyles.cliArguments(styles) }
            }
            if directSticker {
                p["sticker"] = "\(source.attachments[0].stickerPackId):\(source.attachments[0].stickerId)"
            } else if !staged.paths.isEmpty {
                p["attachments"] = staged.paths
            }
            addRecipient(&p, chats.convs[key]!)
            var preview = Message()
            preview.text = text
            preview.textStyles = styles
            preview.attachments = source.attachments.enumerated().map { i, a in
                var copy = a
                copy.id = ""
                copy.path = directSticker ? "" : (i < files.count ? files[i] : "")
                if i < datas.count {
                    copy.size = Int64(datas[i].count)
                    if copy.isImage && copy.thumb.isEmpty {
                        if let (w, h) = Media.pixelSize(datas[i]) { copy.width = w; copy.height = h }
                        copy.thumb = Media.makeThumbnail(datas[i])
                    }
                }
                if !directSticker && copy.isSticker { copy.filename = "sticker.webp" }
                return copy
            }
            sendWithPreview(key, params: p, preview: preview, failure: "Forwarding failed",
                            cleanup: staged.paths.isEmpty ? nil : staged.dir) { _ in
                remaining.removeAll { $0 == key }
                if remaining.isEmpty { try? FileManager.default.removeItem(at: stage) }
            }
        }
        showFlash(targets.count == 1 ? "Forwarded." : "Forwarded to \(targets.count) chats.", seconds: 4)
    }

    // MARK: - Safety numbers

    private func loadKnownSafetyNumbers() {
        guard knownSafetyAccount != account else { return }
        knownSafetyAccount = account
        knownSafety = UserDefaults.standard.dictionary(forKey: "knownSafetyNumbers." + account) as? [String: String] ?? [:]
    }

    private func setKnownSafetyNumber(_ recipient: String, _ digits: String) {
        guard !recipient.isEmpty, !digits.isEmpty, !account.isEmpty else { return }
        loadKnownSafetyNumbers()
        knownSafety[recipient] = digits
        UserDefaults.standard.set(knownSafety, forKey: "knownSafetyNumbers." + account)
    }

    /// Compares the contacts' safety numbers with the ones accepted before (all contacts, or one chat).
    func checkIdentities(_ key: String = "", force: Bool = false) {
        guard cli.isRunning, !account.isEmpty else { return }
        loadKnownSafetyNumbers()
        var p: JSON = [:]
        if !key.isEmpty {
            guard let c = chats.convs[key], !c.isGroup, !chats.isNoteToSelf(key) else { return }
            let now = nowMs()
            if !force && now - (identityCheckedMs[key] ?? 0) < 20_000 { return }
            identityCheckedMs[key] = now
            p["number"] = chats.resolveKey(key)
        }
        Task {
            guard let res = try? await cli.call("listIdentities", p) else { return } // unknown contact: nothing to compare
            handleIdentities(SafetyNumber.parse(res), force: force)
        }
    }

    private func handleIdentities(_ all: [SafetyNumber.Identity], force: Bool) {
        let byRecipient = Dictionary(grouping: all.filter { !$0.recipient.isEmpty && $0.recipient != account && $0.uuid != account },
                                     by: \.recipient)
        for (recipient, ids) in byRecipient {
            guard let id = SafetyNumber.pick(ids, recipient: recipient) else { continue }
            let stored = knownSafety[recipient] ?? ""
            if SafetyNumber.changed(stored: stored, now: id) || (force && id.isUntrusted) {
                guard let key = [id.number, id.uuid].first(where: { !$0.isEmpty && chats.convs[$0].map { !$0.isGroup } ?? false })
                else { continue }
                let change = SafetyChange(recipient: recipient, digits: id.digits, blocked: id.isUntrusted)
                if safetyChanged[key] != change { safetyChanged[key] = change }
            } else if stored.isEmpty {
                setKnownSafetyNumber(recipient, id.digits) // first sight: this is the baseline
            }
        }
    }

    /// Accepted from the bar: only a further change warns again.
    func dismissSafetyChange(_ key: String) {
        guard let change = safetyChanged.removeValue(forKey: key) else { return }
        setKnownSafetyNumber(change.recipient, change.digits)
    }

    func safetyVerified(_ key: String, recipient: String, digits: String) {
        setKnownSafetyNumber(recipient, digits)
        safetyChanged.removeValue(forKey: key)
    }

    /// A send was refused for a changed key, or a group send reported recipients whose key changed.
    private func checkSendOutcome(_ key: String, result: Any?, error: String?) {
        if let error, SafetyNumber.isUntrustedIdentityError(error) { checkIdentities(key, force: true) }
        for who in SafetyNumber.identityFailures(in: result) { checkIdentities(chats.resolveKey(who), force: true) }
    }

    // MARK: - Blocking, message requests

    /// Blocks or unblocks a contact on the backend (synced to the phone); the chat follows on success.
    func setBlocked(_ key: String, _ blocked: Bool) {
        guard !chats.isNoteToSelf(key), let c = chats.convs[key], !c.isGroup, c.blocked != blocked else { return }
        let recipient = chats.resolveKey(key)
        Task {
            do {
                _ = try await cli.call(blocked ? "block" : "unblock", ["recipient": [recipient]])
                guard chats.convs[key] != nil, chats.convs[key]!.blocked != blocked else { return }
                chats.convs[key]!.blocked = blocked
                persist(Records.blocked(key, blocked))
                if blocked && typingKey == key { stopTyping() }
                listChanged()
            } catch {
                await Dialogs.warn(blocked ? "Block contact" : "Unblock contact",
                                   "Could not \(blocked ? "block" : "unblock") the contact: \(error.localizedDescription)")
            }
        }
    }

    func blockContact(_ key: String) async {
        guard let c = chats.convs[key], !c.isGroup, !c.blocked, !chats.isNoteToSelf(key) else { return }
        guard await Dialogs.confirm("Block contact",
                                    "Block \(c.title)? You will no longer receive messages or calls from this contact.",
                                    ok: "Block", destructive: true) else { return }
        setBlocked(key, true)
    }

    /// Open message requests: hidden contacts that wrote to us, not yet declined or blocked.
    var messageRequests: [(key: String, title: String)] {
        hiddenContacts.compactMap { key, name in
            guard !chats.declined.contains(key), let c = chats.convs[key], !c.blocked else { return nil }
            return (key, c.title.isEmpty || c.title == key ? name : c.title)
        }.sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
    }

    /// Accept = clear the hidden state (corvus-cli "removeContact" without options): the messages and
    /// the session stay, the request's parked attachments are fetched now.
    func acceptRequest(_ key: String) async throws {
        _ = try await cli.call("removeContact", ["recipient": key])
        hiddenContacts.removeValue(forKey: key)
        if chats.declined.remove(key) != nil { _ = rewriteHistory("update history") } // drop the stored "declined" record
        if let c = chats.convs[key] {
            for m in c.msgs where !m.attachments.isEmpty { fetchThumbnails(key, ts: m.ts, author: chats.effectiveAuthor(c, m)) }
            showArchived = false
            openConversation(key)
        }
        listChanged()
    }

    /// Decline is local only: the contact stays hidden on the backend, Corvus stops offering it.
    func declineRequest(_ key: String) {
        chats.declined.insert(key)
        persist(Records.requestDeclined(key))
        listChanged()
    }

    func blockRequest(_ key: String) async throws {
        _ = try await cli.call("block", ["recipient": [key]])
        hiddenContacts.removeValue(forKey: key)
        chats.declined.insert(key)
        persist(Records.requestDeclined(key))
        if chats.convs[key] != nil {
            chats.convs[key]!.blocked = true
            persist(Records.blocked(key, true))
        }
        listChanged()
    }

    func deleteConversation(_ key: String) async {
        guard let c = chats.convs[key], !chats.isNoteToSelf(key) else { return } // like the official client
        guard await Dialogs.confirm("Delete conversation", "Delete “\(c.title)” and its stored messages from this computer?",
                                    ok: "Delete", destructive: true) else { return }
        if key == editKey { cancelEdit() }
        if key == typingKey { stopTyping() }
        // The chat is gone: the backend's stored copies of its attachments go too (F1).
        chats.convs[key]?.msgs.forEach { deleteAttachmentFiles($0.attachments) }
        chats.convs.removeValue(forKey: key)
        drafts.removeValue(forKey: key)
        unsentReads.removeValue(forKey: key)
        typing.removeValue(forKey: key)
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
            stopTyping()
            saveDraft()
            dismissedPreviewUrl = ""
            current = key
            loadDraft(key)
            closeSearch()
        }
        chats.convs[key]!.unread = 0
        sendReadReceipts(key)
        persist(chats.startIncomingExpirationTimers(key, now: nowMs()))
        notifier.close(key) // the notification for this chat is obsolete now
        composerFocusRequest += 1
        listChanged()
        checkIdentities(key)
    }

    private func saveDraft() {
        guard let key = current else { return }
        let d = Draft(text: composerText, reply: replyQuote, files: pendingFiles, gifAnalyticsUrl: pendingGifAnalyticsUrl,
                      mentions: MentionCompose.valid(composerMentions, in: composerText), styles: composerStyles)
        if d.isEmpty { drafts.removeValue(forKey: key) } else { drafts[key] = d }
    }

    private func loadDraft(_ key: String) {
        let d = drafts.removeValue(forKey: key) ?? Draft() // the composer owns it while the chat is open
        composerText = d.text
        composerMentions = d.mentions
        composerStyles = d.styles
        hideMentionSuggestions()
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
            sendReadReceipts(key)
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
        let now = nowMs()
        guard historyReady, chats.hasExpiredMessages(now: now) else { return }
        let removed = chats.expireMessages(now: now)
        if removed.isEmpty { return }
        deleteAttachmentFiles(removed.flatMap(\.attachments)) // the backend's stored copies too (F1)
        rewriteHistory("remove expired messages")
        if let key = current, chats.convs[key] != nil { updateSearch() }
    }

    // MARK: - @mentions

    /// How a mentioned person is written ("@Name"): the same for sent and received mentions, so our own message
    /// reads the same before and after signal-cli echoes it. "" if nothing better than the id is known.
    func mentionName(_ id: String) -> String {
        let key = chats.resolveKey(id)
        if key.isEmpty { return "" }
        if let c = chats.convs[key], !c.isGroup, c.title != key { return c.title }
        if let n = chats.contactNames[key], !n.isEmpty { return n }
        return displayName(key) == key ? "" : displayName(key)
    }

    /// Follows the caret in the message field: suggests group members while an "@query" is being typed
    /// (only in groups, not while editing). Same rules as the Qt app.
    func updateMentionSuggestions(text: String, cursor: Int) {
        guard let key = current, let c = chats.convs[key], c.isGroup, editKey.isEmpty, connected,
              let (start, query) = MentionCompose.query(in: text, cursor: cursor),
              !composerMentions.contains(where: { $0.start - 1 == start }) // the caret is in a finished mention
        else {
            hideMentionSuggestions()
            return
        }
        var found: [GroupContact] = []
        for id in groupMembers[key] ?? [] {
            var name = mentionName(id).trimmingCharacters(in: .whitespaces)
            if name.isEmpty { name = id }
            if !query.isEmpty && !name.localizedCaseInsensitiveContains(query) && !id.localizedCaseInsensitiveContains(query) {
                continue
            }
            found.append(GroupContact(id: id, name: name))
        }
        found.sort { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
        found = Array(found.prefix(8))
        guard !found.isEmpty else {
            hideMentionSuggestions()
            return
        }
        mentionTrigger = (start, cursor)
        if found != mentionSuggestions {
            mentionSuggestions = found
            mentionSelection = 0
        }
    }

    func hideMentionSuggestions() {
        mentionTrigger = nil
        if !mentionSuggestions.isEmpty { mentionSuggestions = [] }
        mentionSelection = 0
    }

    func moveMentionSelection(_ delta: Int) {
        let n = mentionSuggestions.count
        guard n > 0 else { return }
        mentionSelection = (mentionSelection + (delta > 0 ? 1 : n - 1)) % n
    }

    // MARK: - Chat list navigation

    /// The chats as the list shows them (folder, sort order, search).
    var visibleConversations: [Conversation] {
        chats.visibleConversations(archived: showArchived, alphabetical: appearance.sortAlphabetically, query: chatFilter)
    }

    /// ⌘1 … ⌘9
    func openConversation(atListIndex i: Int) {
        let list = visibleConversations
        if i >= 0 && i < list.count { openConversation(list[i].key) }
    }

    /// ⌥⌘↑ / ⌥⌘↓: previous / next chat in the list (no wrap-around).
    func stepConversation(_ delta: Int) {
        let list = visibleConversations
        let cur = current.flatMap { k in list.firstIndex { $0.key == k } }
        guard let t = ChatState.step(current: cur, count: list.count, delta: delta) else { return }
        openConversation(list[t].key)
    }

    /// ⌘K: into the chat list's search field; again (or Esc there): back to the message field.
    func toggleChatSearch() {
        if chatSearchFocused { endChatSearch() } else { chatSearchFocusRequest += 1 }
    }

    func endChatSearch() {
        chatFilter = ""
        chatSearchFocused = false
        composerFocusRequest += 1
    }

    /// Return in the chat search: open the first match.
    func openFirstChatMatch() {
        guard let first = visibleConversations.first else { return }
        let key = first.key
        chatFilter = ""
        chatSearchFocused = false
        openConversation(key)
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

    // MARK: - Groups

    /// Reloads the groups from signal-cli: titles and timers of our groups, and open invitations.
    func refreshGroups() async {
        guard let res = try? await cli.call("listGroups") else { return }
        var invitations: [GroupInfo] = []
        for gv in jsonArray(res) {
            let g = jsonObject(gv)
            let info = GroupInfo.parseOne(g)
            if !info.isMember {
                if info.isInvitation(for: account) { invitations.append(info) }
                continue
            }
            if info.id.isEmpty { continue }
            let title = info.name.isEmpty ? "Group" : info.name
            let key = "g:" + info.id
            var members: [String] = []
            for m in info.members where !m.isPerson(account) {
                let k = chats.resolveKey(m.id)
                if !k.isEmpty && k != account && !members.contains(k) { members.append(k) }
            }
            groupMembers[key] = members
            chats.ensure(key, title: title, group: true)
            chats.convs[key]!.title = title
            if chats.convs[key]!.expirationSeconds != info.expiration {
                chats.convs[key]!.expirationSeconds = info.expiration
                persist(Records.expiration(key, info.expiration))
            }
        }
        for g in invitations where !groupInvitations.contains(where: { $0.id == g.id }) {
            showFlash("You were invited to the group “\(g.name.isEmpty ? "Group" : g.name)”.", seconds: 10)
        }
        groupInvitations = invitations
        listChanged()
    }

    /// One group as signal-cli knows it now.
    func loadGroup(_ groupId: String) async throws -> GroupInfo {
        let res = try await cli.call("listGroups", ["groupId": [groupId]])
        guard let info = GroupInfo.find(res, id: groupId) else { throw RpcError("\(backendName) does not know this group.") }
        return info
    }

    /// "updateGroup" for an existing group (`groupId` set) or a new one; returns signal-cli's result.
    @discardableResult
    func updateGroup(_ groupId: String?, _ params: JSON) async throws -> Any {
        var p = params
        if let groupId { p["groupId"] = groupId }
        return try await cli.call("updateGroup", p)
    }

    /// Leaves a group; `successor` names the new admin when we are the only one.
    func quitGroup(_ groupId: String, successor: String? = nil) async throws {
        var p: JSON = ["groupId": groupId]
        if let successor { p["admin"] = [successor] }
        try await cli.call("quitGroup", p)
    }

    /// People that can be added to a group: contacts plus 1:1 chats, without ourselves, by name.
    func groupContacts() -> [GroupContact] {
        var people: [String: String] = chats.contactNames
        for c in chats.convs.values where !c.isGroup { people[c.key] = c.title.isEmpty ? c.key : c.title }
        return people
            .filter { $0.key != account && chats.resolveKey($0.key) != account }
            .map { GroupContact(id: $0.key, name: $0.value) }
            .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    /// "You", the person's name, or the id.
    func memberName(_ id: String) -> String {
        let key = chats.resolveKey(id)
        if !account.isEmpty && (id == account || key == account) { return "You" }
        if let c = chats.convs[key], !c.isGroup, c.title != key { return c.title }
        return chats.contactNames[key] ?? id
    }

    private var canTalkToCli: Bool {
        if !connected { showFlash("Not connected to \(backendName).", seconds: 5) }
        return connected
    }

    func newGroup() {
        guard canTalkToCli else { return }
        groupSheet = .create
    }

    func openGroupSettings(_ key: String) {
        guard let c = chats.convs[key], c.isGroup, canTalkToCli else { return }
        groupSheet = .settings(groupId: String(key.dropFirst(2)))
    }

    func openGroupInvitations() {
        guard !groupInvitations.isEmpty, canTalkToCli else { return }
        groupSheet = .invitations
    }

    /// After creating / joining: load the group and open its chat.
    func showGroup(_ groupId: String) async {
        await refreshGroups()
        let key = "g:" + groupId
        guard !groupId.isEmpty, chats.convs[key] != nil else { return }
        showArchived = chats.convs[key]!.archived
        openConversation(key)
    }

    func groupLeft(_ groupId: String) async {
        let key = "g:" + groupId
        showFlash("You left “\(chats.convs[key]?.title ?? "the group")”.", seconds: 8)
        await refreshGroups()
    }

    func joinGroupByLink() async {
        guard canTalkToCli,
              let uri = await Dialogs.text("Join group", "Group invite link (https://signal.group/#…):")?
                .trimmingCharacters(in: .whitespacesAndNewlines), !uri.isEmpty else { return }
        do {
            let o = jsonObject(try await cli.call("joinGroup", ["uri": uri]))
            if jsonBool(o["onlyRequested"]) {
                showFlash("Join request sent. An admin has to approve it.", seconds: 10)
            } else {
                showFlash("Joined the group.", seconds: 8)
            }
            await showGroup(jsonString(o["groupId"]))
        } catch {
            await Dialogs.warn("Join group", "Could not join the group: \(error.localizedDescription)")
        }
    }

    /// Text and styles as shown: messages written by the Qt app keep their mention placeholders (with the
    /// mentions stored next to them), those are shown as "@Name" like the ones received here.
    func displayText(_ m: Message) -> (text: String, styles: [TextStyleRange]) {
        guard !m.mentions.isEmpty else { return (m.text, m.textStyles) }
        return TextStyles.resolveMentions(m.text, m.mentions.map { r in
            let n = r.uuid.isEmpty ? "" : mentionName(r.uuid)
            return (r.start, r.length, n.isEmpty || n == r.uuid ? "mentioned member" : n)
        }, styles: m.textStyles)
    }

    // MARK: - Receiving

    private func onEnvelope(_ env: JSON) {
        var pe = parseEnvelope(env)
        if pe.kind == .none { return }
        if !pe.mentions.isEmpty {
            // "@Name" in place of each placeholder; the text styles move with the text.
            let resolved = TextStyles.resolveMentions(pe.msg.text, pe.mentions.map { m in
                let n = mentionName(m.number.isEmpty ? m.uuid : m.number)
                return (m.start, m.length, n.isEmpty ? (m.name.isEmpty ? m.number : m.name) : n)
            }, styles: pe.msg.textStyles)
            pe.msg.text = resolved.text
            pe.msg.textStyles = resolved.styles
        }

        let group = !pe.groupId.isEmpty
        // A message straight from our own number is a note to ourselves: treat it as our own send
        // (no unread badge, no notification), like the Qt app and the official client.
        if !pe.outgoing && !group && chats.isNoteToSelf(pe.src) {
            pe.outgoing = true
            if pe.destination.isEmpty { pe.destination = account }
        }
        let srcKey = chats.resolveKey(pe.src)
        let key = group ? "g:" + pe.groupId : (pe.outgoing ? chats.resolveKey(pe.destination) : srcKey)
        if key.isEmpty { return }
        if !pe.outgoing {
            // Nothing from a blocked contact; in a group a blocked member's messages, receipts and
            // typing are dropped too (their block state lives in their direct chat), as in the Qt app (L4).
            if !group, chats.convs[key]?.blocked == true { return }
            if let sender = chats.convs[srcKey], !sender.isGroup, sender.blocked { return }
        }

        // corvus-cli saw an identity key it could not verify (L6): a warning until the
        // contact is checked (listIdentities / trust) or the flash times out.
        if !pe.outgoing && pe.kind == .message && !group {
            checkIdentities(srcKey, force: pe.untrustedIdentity) // a new key becomes known with its first message
        }
        if pe.untrustedIdentity && !pe.outgoing && pe.kind == .message {
            showFlash("The safety number with \(displayName(key)) changed and is not verified. Check it before you rely on this chat.", seconds: 15)
        }

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
            if let rec = chats.applyEdit(key, targetTs: pe.targetTs, authors: [author], text: pe.msg.text, newTs: pe.msg.ts,
                                         styles: pe.msg.textStyles) {
                persist(rec)
            }
            return
        case .remoteDelete:
            // Like the official clients: only the author can delete a message for everyone.
            applyDelete(key, ts: pe.targetTs, authors: [pe.outgoing ? account : srcKey])
            return
        case .receipt:
            var recs: [JSON] = []
            // Like the Qt app (and Signal): without sending read receipts, nobody's read state is shown.
            if pe.receiptLevel == .read && !privacy.sendReadReceipts { return }
            for ts in pe.receiptTimestamps {
                if let rec = chats.applyReceipt(ts: ts, level: pe.receiptLevel, preferredKey: key,
                                                recipient: srcKey, timeMs: pe.receiptTimeMs) { recs.append(rec) }
            }
            persist(recs)
            return
        case .typing:
            guard chats.convs[key] != nil, !srcKey.isEmpty, srcKey != account else { return }
            if pe.typingStarted {
                typing[key, default: [:]][srcKey] = nowMs() + 15_000 // Signal's timeout without a refresh
            } else if typing[key]?[srcKey] != nil {
                typing[key]!.removeValue(forKey: srcKey)
                if typing[key]!.isEmpty { typing.removeValue(forKey: key) }
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
        if !m.outgoing {
            // The message ends the sender's typing indicator.
            if typing[key]?[m.authorKey] != nil {
                typing[key]!.removeValue(forKey: m.authorKey)
                if typing[key]!.isEmpty { typing.removeValue(forKey: key) }
            }
            if !m.authorKey.isEmpty { unsentReads[key, default: []].append((m.authorKey, m.ts)) }
        }
        // A message in the selected chat is only read while the user looks at it.
        if !m.outgoing {
            if key == current && userIsLooking() { sendReadReceipts(key) } else { chats.convs[key]!.unread += 1 }
        }
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
            if preview.isEmpty, let a = m.attachments.first { preview = a.isSticker ? "[Sticker]" : a.isImage ? "[Image]" : "[File] \(a.filename)" }
            var title = c.title, body: String
            switch notify.content {
            case .hidden:
                title = "Corvus"
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

    /// Drops typing indicators that were not refreshed in time (Signal clients refresh every few seconds).
    private func expireTyping() {
        let now = nowMs()
        guard typing.values.contains(where: { $0.values.contains { $0 <= now } }) else { return } // no observed change
        for key in typing.keys {
            typing[key] = typing[key]!.filter { $0.value > now }
            if typing[key]!.isEmpty { typing.removeValue(forKey: key) }
        }
    }

    /// Display names of the people typing in `key`, in a stable order.
    func typingNames(_ key: String) -> [String] {
        (typing[key] ?? [:]).keys.sorted().map { displayName($0) }
    }

    /// Read receipts for the incoming messages of `key` that the user has now seen, one call per author.
    private func sendReadReceipts(_ key: String) {
        guard let due = unsentReads.removeValue(forKey: key), !due.isEmpty else { return }
        guard privacy.sendReadReceipts, connected else { return }
        var byAuthor: [String: [Int64]] = [:]
        for (author, ts) in due { byAuthor[author, default: []].append(ts) }
        for (author, tss) in byAuthor where author != account {
            let p: JSON = ["recipient": author, "targetTimestamp": tss, "type": "read"]
            Task { _ = try? await cli.call("sendReceipt", p) }
        }

        let readSyncs: [[String: Any]] = due.compactMap { item in
            guard item.0 != account else { return nil }
            return ["sender": item.0, "timestamp": item.1]
        }
        if !readSyncs.isEmpty {
            Task {
                do {
                    _ = try await cli.call("syncReadMessages", ["reads": readSyncs])
                } catch {
                    // Retry on the next activation/chat open if the phone did not receive the sync.
                    unsentReads[key, default: []].append(contentsOf: due.filter { $0.0 != account })
                    NSLog("Could not synchronize read messages to linked devices: %@", error.localizedDescription)
                }
            }
        }
    }

    // MARK: - Sending

    /// The user changed the composer text (typed, pasted, deleted): drives our own typing indicator.
    /// Like Signal Desktop: STARTED again every 10 s while typing, STOPPED after a 3 s pause or when emptied.
    func composerEdited() {
        guard privacy.sendTypingIndicators, connected, editKey.isEmpty, let key = current, chats.convs[key] != nil else {
            stopTyping()
            return
        }
        if composerText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            stopTyping()
            return
        }
        let now = nowMs()
        if typingKey != key || now - typingSentMs >= 10_000 {
            if let old = typingKey, old != key { sendTyping(old, stop: true) }
            typingKey = key
            typingSentMs = now
            sendTyping(key, stop: false)
        }
        typingStopTask?.cancel()
        typingStopTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(3))
            guard !Task.isCancelled else { return }
            self?.stopTyping()
        }
    }

    /// Ends our typing indicator, if one is showing on the other side.
    func stopTyping() {
        typingStopTask?.cancel()
        typingStopTask = nil
        guard let key = typingKey else { return }
        typingKey = nil
        typingSentMs = 0
        sendTyping(key, stop: true)
    }

    private func sendTyping(_ key: String, stop: Bool) {
        guard connected, let c = chats.convs[key], stop || !c.blocked else { return }
        var p: JSON = [:]
        addRecipient(&p, c)
        if stop { p["stop"] = true }
        Task { _ = try? await cli.call("sendTyping", p) }
    }

    private func addRecipient(_ params: inout JSON, _ c: Conversation) {
        if c.isGroup { params["groupId"] = String(c.key.dropFirst(2)) } else { params["recipient"] = [c.key] }
    }

    /// Recipients of an outgoing message for Message details: the chat partner or the group's members,
    /// plus anyone who sent a receipt for it; sorted by name. Same rules as the Qt app.
    func detailRecipients(_ m: Message, in c: Conversation) -> [String] {
        var keys = c.isGroup ? (groupMembers[c.key] ?? []) : [c.key]
        for k in m.recipientReceipts.keys.sorted() where !keys.contains(k) { keys.append(k) }
        keys.removeAll { $0 == account || chats.resolveKey($0) == account }
        var seen = Set<String>()
        keys = keys.filter { seen.insert($0).inserted }
        return keys.sorted { displayName($0).localizedStandardCompare(displayName($1)) == .orderedAscending }
    }

    func message(withId id: String) -> (index: Int, message: Message)? {
        guard let c = currentConversation, let i = c.msgs.firstIndex(where: { $0.id == id }) else { return nil }
        return (i, c.msgs[i])
    }

    func startReply(_ id: String) {
        guard let c = currentConversation, let (_, m) = message(withId: id), !m.deleted else { return }
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
            sendEdit(composerText.trimmingCharacters(in: .whitespacesAndNewlines),
                     styles: TextStyles.map(composerStyles, in: composerText))
            return
        }
        let text = composerText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let key = current, let c = chats.convs[key], !(text.isEmpty && pendingFiles.isEmpty) else { return }
        guard connected else {
            showFlash("Not connected to \(backendName).", seconds: 5)
            return
        }
        if c.blocked {
            showFlash("You blocked \(c.title). Unblock the contact to send messages.", seconds: 6)
            return
        }
        // A preview for the link in this text is still being made: send once it is there (or failed).
        let pendingPreviewUrl = LinkPreviewRules.firstHttpsLink(composerText)
        if privacy.generateLinkPreviews && !pendingPreviewUrl.isEmpty && pendingPreviewUrl != dismissedPreviewUrl
            && linkPreviewLoading && previewRequestedUrl == pendingPreviewUrl {
            sendAfterLinkPreview = composerText
            showFlash("Generating link preview …", seconds: 5)
            return
        }
        var previewForSend = LinkPreview()
        let textPreviewUrl = LinkPreviewRules.firstHttpsLink(text)
        if privacy.generateLinkPreviews && !textPreviewUrl.isEmpty && textPreviewUrl != dismissedPreviewUrl
            && draftLinkPreview.isValid && draftLinkPreview.url == textPreviewUrl {
            previewForSend = draftLinkPreview
        }
        let files = pendingFiles, quote = replyQuote, gifAnalytics = pendingGifAnalyticsUrl
        stopTyping()

        var p: JSON = [:]
        if !text.isEmpty {
            // @mentions: signal-cli gets the text without the "@" in front of each name, plus their positions.
            let prepared = c.isGroup ? MentionCompose.prepare(composerText, mentions: composerMentions) : (text: text, mentions: [ComposerMention]())
            p["message"] = prepared.text
            if !prepared.mentions.isEmpty { p["mention"] = MentionCompose.cliArguments(prepared.mentions) }
            // Formatting: the offsets of what signal-cli gets (trimmed, without the "@" of each mention).
            let removeAt = MentionCompose.removedAtSigns(composerText, mentions: c.isGroup ? composerMentions : [])
            let cliStyles = TextStyles.map(composerStyles, in: composerText, removeAt: removeAt)
            if !cliStyles.isEmpty { p["textStyle"] = TextStyles.cliArguments(cliStyles) }
        }
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
        let previewImage = previewForSend.imagePath.isEmpty || !FileManager.default.fileExists(atPath: previewForSend.imagePath)
            ? [] : [previewForSend.imagePath]
        let staged = Media.stageForSignalCli(files + previewImage) // the preview image, if any, is the last one
        if !files.isEmpty { p["attachments"] = Array(staged.paths.prefix(files.count)) }
        if previewForSend.isValid {
            p["previewUrl"] = previewForSend.url
            p["previewTitle"] = previewForSend.title
            if !previewForSend.description.isEmpty { p["previewDescription"] = previewForSend.description }
            if !previewImage.isEmpty, let img = staged.paths.last { p["previewImage"] = img }
        }
        // Recorded voice messages go out as voice notes (shown inline by the Signal apps) when this
        // signal-cli can do that and nothing but recordings is attached.
        if !files.isEmpty && files.allSatisfy({ voiceFiles[$0] != nil })
            && VoiceNote.supportsVoiceNote(signalCliVersion: version) {
            p["voiceNote"] = true
        }
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
        preview.orderTs = max(preview.ts, (chats.convs[key]!.msgs.last?.sortTs ?? 0) + 1)
        preview.text = text
        preview.textStyles = TextStyles.map(composerStyles, in: composerText) // offsets of the stored (trimmed) text
        preview.outgoing = true
        preview.authorKey = account
        preview.expiresInSeconds = c.expirationSeconds
        preview.quote = quote
        preview.attachments = atts
        preview.linkPreview = previewForSend
        preview.linkPreview.imagePath = ""

        let localStyles = preview.textStyles
        let pendingId = UUID().uuidString
        var pending = PendingSend()
        pending.conversationKey = key
        pending.temporaryTimestamp = preview.ts
        pending.message = preview
        chats.pendingSends[pendingId] = pending
        chats.convs[key]!.insertSorted(preview)

        composerText = ""
        composerMentions = []
        composerStyles = []
        hideMentionSuggestions()
        dismissedPreviewUrl = ""
        pendingFiles = []
        replyQuote = Quote()
        if !files.isEmpty { showFlash("Sending …", seconds: 120) }
        scrollToMessage(index: chats.convs[key]!.msgs.firstIndex { $0.id == preview.id } ?? 0)

        Task { [p] in
            var result: Any?
            var err: String?
            do { result = try await cli.call("send", p) } catch { err = error.localizedDescription }
            checkSendOutcome(key, result: result, error: err)
            try? FileManager.default.removeItem(at: staged.dir)
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
                    composerStyles = localStyles
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

    // MARK: Link previews

    /// Follows the composer: makes (or drops) the preview of its first HTTPS link. Like the Qt app, but the
    /// page is only fetched once typing pauses for a moment, so half-typed URLs are not requested.
    private func updateDraftLinkPreview() {
        let raw = composerText
        let url = LinkPreviewRules.firstHttpsLink(raw)
        if let waiting = sendAfterLinkPreview, waiting != raw { sendAfterLinkPreview = nil }
        if !privacy.generateLinkPreviews || !editKey.isEmpty || url.isEmpty {
            if url.isEmpty { dismissedPreviewUrl = "" }
            cancelLinkPreview()
            return
        }
        if url == dismissedPreviewUrl { return }
        if draftLinkPreview.isValid && draftLinkPreview.url == url { return }
        if url == previewRequestedUrl { return } // loading, or tried already without a result
        linkPreviewTask?.cancel()
        draftLinkPreview = LinkPreview()
        previewRequestedUrl = url
        linkPreviewLoading = true
        linkPreviewTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(400))
            if Task.isCancelled { return }
            let result = await LinkPreviewFetcher.fetch(url)
            guard let self, !Task.isCancelled, self.previewRequestedUrl == url else { return }
            self.draftLinkPreview = result ?? LinkPreview()
            self.linkPreviewLoading = false
            self.finishPendingLinkPreviewSend()
        }
    }

    private func cancelLinkPreview() {
        linkPreviewTask?.cancel()
        linkPreviewTask = nil
        linkPreviewLoading = false
        previewRequestedUrl = ""
        if draftLinkPreview != LinkPreview() { draftLinkPreview = LinkPreview() }
        finishPendingLinkPreviewSend()
    }

    /// ✕ on the preview: this link is sent without one.
    func dismissLinkPreview() {
        dismissedPreviewUrl = draftLinkPreview.url.isEmpty ? previewRequestedUrl : draftLinkPreview.url
        cancelLinkPreview()
    }

    private func finishPendingLinkPreviewSend() {
        guard let text = sendAfterLinkPreview else { return }
        sendAfterLinkPreview = nil
        Task { if composerText == text { sendMessage() } }
    }

    /// The image of a received link preview (signal-cli may still be downloading it: a few retries).
    private func fetchLinkPreviewThumbnail(_ key: String, ts: Int64, author: String, attempt: Int = 0) {
        guard let c = chats.convs[key], let mi = chats.findMessage(c, ts: ts, authors: [author]) else { return }
        let lp = c.msgs[mi].linkPreview
        guard lp.isValid, !lp.imageId.isEmpty, lp.imageThumb.isEmpty, lp.imageSize <= Media.maxThumbSource else { return }
        let token = "lp|\(key)|\(ts)|\(author)"
        guard !thumbPending.contains(token) else { return }
        thumbPending.insert(token)
        var p: JSON = ["id": lp.imageId]
        addRecipient(&p, c)
        Task { [p] in
            defer { thumbPending.remove(token) }
            let data: Data
            do {
                data = Data(base64Encoded: jsonString(jsonObject(try await cli.call("getAttachment", p))["data"])) ?? Data()
            } catch {
                if attempt < 3 {
                    try? await Task.sleep(for: .seconds(2 * (attempt + 1)))
                    thumbPending.remove(token)
                    fetchLinkPreviewThumbnail(key, ts: ts, author: author, attempt: attempt + 1)
                }
                return
            }
            guard NSImage(data: data) != nil, let c2 = chats.convs[key], let i = chats.findMessage(c2, ts: ts, authors: [author]),
                  c2.msgs[i].linkPreview.url == lp.url else { return }
            let thumb = Media.makeThumbnail(data)
            chats.convs[key]!.msgs[i].linkPreview.imageThumb = thumb.isEmpty ? data : thumb
            persist(Records.linkPreviewUpdate(key, chats.convs[key]!.msgs[i]))
        }
    }

    // MARK: Stickers

    private func resetStickers() {
        stickerPacks = []
        stickerPacksLoaded = false
        stickerPacksLoading = false
        stickerImages = [:]
        stickerQueue = []
        stickerRequested = []
        stickerStatus = nil
    }

    /// The image of one sticker (signal-cli downloads the pack on first use).
    func fetchSticker(_ packId: String, _ stickerId: Int) async throws -> Data {
        let res = try await cli.call("getSticker", ["packId": packId, "stickerId": stickerId])
        var encoded = jsonString(jsonObject(res)["data"])
        if encoded.isEmpty { encoded = jsonString(jsonObject(res)["base64"]) }
        if encoded.isEmpty { encoded = jsonString(res) }
        guard let data = Data(base64Encoded: encoded), !data.isEmpty else { throw RpcError("The sticker could not be loaded.") }
        return data
    }

    /// Installed packs; loaded once per connection (reopening the picker uses the copy in memory).
    func loadStickerPacks(force: Bool = false) {
        guard connected else {
            if !stickerPacksLoaded { stickerStatus = ("Connect to Signal to load sticker packs.", true) }
            return
        }
        if (stickerPacksLoaded && !force) || stickerPacksLoading { return }
        stickerPacksLoading = true
        stickerStatus = ("Loading installed sticker packs …", false)
        Task {
            defer { stickerPacksLoading = false }
            do {
                stickerPacks = StickerPack.parse(try await cli.call("listStickerPacks"))
                stickerPacksLoaded = true
                stickerStatus = nil
            } catch {
                stickerStatus = ("Could not load sticker packs: \(error.localizedDescription)", true)
            }
        }
    }

    func addStickerPack() async {
        guard let uri = await Dialogs.text("Add sticker pack", "Paste a Signal sticker-pack link (https://signal.art/addstickers/#…):")?
            .trimmingCharacters(in: .whitespacesAndNewlines), !uri.isEmpty else { return }
        guard StickerPack.isValidPackUrl(uri) else {
            stickerStatus = ("Enter a valid Signal sticker-pack link.", true)
            return
        }
        guard connected else {
            stickerStatus = ("Connect to Signal before installing a sticker pack.", true)
            return
        }
        stickerStatus = ("Adding sticker pack …", false)
        do {
            try await cli.call("addStickerPack", ["uri": uri])
            stickerPacksLoaded = false
            loadStickerPacks()
        } catch {
            stickerStatus = ("Could not add sticker pack: \(error.localizedDescription)", true)
        }
    }

    static func stickerKey(_ packId: String, _ id: Int) -> String { "\(packId):\(id)" }

    /// Asks for a sticker image for the picker; at most two loads run at once (like the Qt app),
    /// so opening a large collection does not flood signal-cli.
    func requestStickerImage(_ packId: String, _ id: Int) {
        let k = AppModel.stickerKey(packId, id)
        guard stickerImages[k] == nil, !stickerRequested.contains(k) else { return }
        stickerRequested.insert(k)
        stickerQueue.append((packId, id))
        pumpStickerQueue()
    }

    private func pumpStickerQueue() {
        while stickerLoadsInFlight < 2, !stickerQueue.isEmpty {
            let (pack, id) = stickerQueue.removeFirst()
            stickerLoadsInFlight += 1
            Task {
                defer {
                    stickerLoadsInFlight -= 1
                    pumpStickerQueue()
                }
                if let data = try? await fetchSticker(pack, id) { stickerImages[AppModel.stickerKey(pack, id)] = data }
            }
        }
    }

    /// Sends a sticker by reference (a real Signal sticker, not an image upload), with the reply quote if any.
    func sendSticker(packId: String, stickerId: Int) {
        guard let key = current, let c = chats.convs[key], !packId.isEmpty, stickerId >= 0 else { return }
        guard editKey.isEmpty else {
            showFlash("Send a sticker after finishing the current edit.", seconds: 5)
            return
        }
        guard connected else {
            showFlash("Not connected to \(backendName).", seconds: 5)
            return
        }
        let quote = replyQuote
        // A standalone sticker message: no (even empty) message body next to the sticker reference.
        var p: JSON = ["sticker": "\(packId):\(stickerId)"]
        if quote.valid && !quote.authorKey.isEmpty {
            p["quoteTimestamp"] = quote.id
            p["quoteAuthor"] = quote.authorKey
            p["quoteMessage"] = quote.text
        }
        addRecipient(&p, c)

        var att = Attachment.sticker(packId: packId, stickerId: stickerId)
        if let data = stickerImages[AppModel.stickerKey(packId, stickerId)] {
            if let (w, h) = Media.pixelSize(data) { att.width = w; att.height = h }
            att.thumb = Media.makeThumbnail(data)
        }
        var preview = Message()
        preview.ts = nowMs()
        while chats.findMessage(chats.convs[key]!, ts: preview.ts, authors: []) != nil { preview.ts += 1 }
        preview.orderTs = max(preview.ts, (chats.convs[key]!.msgs.last?.sortTs ?? 0) + 1)
        preview.outgoing = true
        preview.authorKey = account
        preview.expiresInSeconds = c.expirationSeconds
        preview.quote = quote
        preview.attachments = [att]

        let pendingId = UUID().uuidString
        var pending = PendingSend()
        pending.conversationKey = key
        pending.temporaryTimestamp = preview.ts
        pending.message = preview
        chats.pendingSends[pendingId] = pending
        chats.convs[key]!.insertSorted(preview)
        replyQuote = Quote()
        stopTyping()
        scrollToMessage(index: chats.convs[key]!.msgs.firstIndex { $0.id == preview.id } ?? 0)

        Task { [p] in
            var result: Any?
            var err: String?
            do { result = try await cli.call("send", p) } catch { err = error.localizedDescription }
            checkSendOutcome(key, result: result, error: err)
            guard let pendingNow = chats.pendingSends[pendingId] else { return }
            if let err, !pendingNow.echoReceived {
                chats.removePendingPreview(pendingNow)
                chats.pendingSends.removeValue(forKey: pendingId)
                showFlash("Sticker could not be sent: \(err)", seconds: 9)
                if quote.valid {
                    if key == current { if !replyQuote.valid { replyQuote = quote } }
                    else if !(drafts[key]?.reply.valid ?? false) { var d = drafts[key] ?? Draft(); d.reply = quote; drafts[key] = d }
                }
                return
            }
            if pendingNow.echoReceived {
                chats.pendingSends.removeValue(forKey: pendingId)
            } else {
                var ts = jsonInt64(jsonObject(result)["timestamp"])
                if ts <= 0 { ts = nowMs() }
                persist(chats.finalizePendingSend(pendingId, timestamp: ts))
                listChanged()
                if att.thumb.isEmpty { fetchThumbnails(key, ts: ts, author: account) } // picker had no image yet
            }
        }
    }

    // MARK: Voice messages

    func startVoiceRecording() {
        guard connected, let key = current, chats.convs[key] != nil, !recorder.isRecording, !recording else { return }
        guard editKey.isEmpty else {
            showFlash("Finish editing the message before recording.", seconds: 5)
            return
        }
        recording = true // the bar shows at once; the microphone prompt may come first
        recordingMs = 0
        recordKey = key
        recordSendWhenDone = false
        stopTyping()
        Task {
            if let why = await VoiceRecorder.prepare() {
                recording = false
                recordKey = nil
                await Dialogs.warn("Voice message", why)
                return
            }
            guard recording, recordKey != nil else { return } // discarded while asking
            recorder.start(directory: AppPaths.tempDir.appendingPathComponent("voice", isDirectory: true),
                           baseName: VoiceNote.baseName())
        }
    }

    /// Stop and attach the recording (and send it right away with `send`).
    func stopVoiceRecording(send: Bool) {
        guard recording else { return }
        recordSendWhenDone = send
        if recorder.isRecording { recorder.stop() } else { cancelVoiceRecording() } // onVoiceRecorded() follows
    }

    func cancelVoiceRecording() {
        guard recording else { return }
        recording = false
        recordKey = nil
        recordSendWhenDone = false
        recorder.cancel()
    }

    private func onVoiceRecorded(_ path: String, _ ms: Int64) {
        let send = recordSendWhenDone, key = recordKey
        recording = false
        recordKey = nil
        recordSendWhenDone = false
        guard let key, chats.convs[key] != nil else {
            try? FileManager.default.removeItem(atPath: path)
            return
        }
        if ms < VoiceNote.minDurationMs { // an accidental click
            try? FileManager.default.removeItem(atPath: path)
            showFlash("The recording was too short and was discarded.", seconds: 5)
            return
        }
        voiceFiles[path] = ms
        if key == current {
            pendingFiles.append(path)
            if send { sendMessage() }
        } else {
            var d = drafts[key] ?? Draft()
            if !d.files.contains(path) { d.files.append(path) }
            drafts[key] = d
            showFlash("The voice message was kept in the draft of “\(chats.convs[key]!.title)”.", seconds: 8)
        }
    }

    /// Removes a queued attachment; a discarded recording is deleted.
    func removePendingFile(_ f: String) {
        pendingFiles.removeAll { $0 == f }
        if voiceFiles.removeValue(forKey: f) != nil { try? FileManager.default.removeItem(atPath: f) }
        if pendingFiles.isEmpty { pendingGifAnalyticsUrl = nil }
    }

    // MARK: Editing

    func startEdit(_ id: String) {
        guard let c = currentConversation, let (_, m) = message(withId: id) else { return }
        guard m.canEdit(nowMs: nowMs()) else {
            showFlash("This message can no longer be edited (3 hours / 10 edits).", seconds: 6)
            return
        }
        stopTyping()
        if editKey.isEmpty { // keep an already running edit's draft
            editDraft = composerText
            editDraftStyles = composerStyles
        }
        editKey = c.key
        editTs = m.ts
        composerText = m.text
        composerStyles = m.textStyles
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
            composerStyles = editDraftStyles
            composerCursorToEnd += 1
        }
        editDraft = ""
        editDraftStyles = []
    }

    private func sendEdit(_ text: String, styles: [TextStyleRange]) {
        guard let c = chats.convs[editKey], let i = chats.findMessage(c, ts: editTs, authors: [account]) else {
            cancelEdit()
            return
        }
        let m = c.msgs[i]
        if text.isEmpty {
            showFlash("A message cannot be empty.", seconds: 6)
            return
        }
        if text == m.text && styles == m.textStyles { // nothing changed
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
        if !styles.isEmpty { p["textStyle"] = TextStyles.cliArguments(styles) }
        if m.quote.valid && !m.quote.authorKey.isEmpty { // an edit keeps the reply it belongs to
            p["quoteTimestamp"] = m.quote.id
            p["quoteAuthor"] = m.quote.authorKey
            p["quoteMessage"] = m.quote.text
        }
        addRecipient(&p, c)
        let key = c.key, origTs = m.ts
        // Don't keep the composer locked in edit mode while signal-cli responds.
        // The request below only updates the original message; any new draft stays untouched.
        cancelEdit(restoreDraft: true)
        Task { [p] in
            do {
                let res = try await cli.call("send", p)
                checkSendOutcome(key, result: res, error: nil)
                clearFlash()
                var ts = jsonInt64(jsonObject(res)["timestamp"])
                if ts <= 0 { ts = nowMs() }
                if let rec = chats.applyEdit(key, targetTs: origTs, authors: [account], text: text, newTs: ts,
                                             styles: styles) { persist(rec) }
            } catch {
                checkSendOutcome(key, result: nil, error: error.localizedDescription)
                showFlash("Editing failed: \(error.localizedDescription)")
            }
        }
    }

    // MARK: Reactions, deletion

    func sendReaction(_ id: String, _ emoji: String) {
        guard let c = currentConversation, let (_, m) = message(withId: id), !m.deleted else { return }
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
        let forEveryone = m.outgoing && !m.deleted && age >= 0 && age <= 24 * 60 * 60 * 1000
        let r = await Dialogs.ask("Delete message",
                                  forEveryone ? "Delete this message from this device or for everyone?" : "Delete this message from this device?",
                                  buttons: forEveryone ? ["Delete for me", "Delete for everyone"] : ["Delete for me"],
                                  style: .warning, cancel: true, destructive: forEveryone ? [0, 1] : [0])
        let key = c.key, ts = m.ts
        if r == 0 {
            deleteAttachmentFiles(m.attachments) // deleted for me: the backend's copy goes too (F1)
            removeMessage(key, ts: ts)
            return
        }
        guard r == 1 else { return }
        var p: JSON = ["targetTimestamp": ts]
        if c.isGroup { p["groupIds"] = [String(key.dropFirst(2))] } else { p["recipients"] = [chats.resolveKey(key)] }
        do {
            try await cli.call("remoteDelete", p)
            // Like the official clients: a placeholder stays where the message was.
            if applyDelete(key, ts: ts, authors: []) { showFlash("Message deleted for everyone.", seconds: 5) }
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

    /// Delete for everyone (ours or the author's): the message turns into a placeholder.
    @discardableResult
    private func applyDelete(_ key: String, ts: Int64, authors: [String]) -> Bool {
        guard let c = chats.convs[key], let i = chats.findMessage(c, ts: ts, authors: authors) else { return false }
        if editKey == key && editTs == c.msgs[i].ts && !c.msgs[i].deleted { cancelEdit() }
        guard let rec = chats.applyDelete(key, targetTs: ts, authors: authors) else { return false }
        // Rewrite instead of append: the old ciphertext of the message must not stay in the file.
        do {
            try store.rewrite(chats.allRecords())
        } catch {
            showFlash("Could not remove message from history: \(error.localizedDescription)")
            persist(rec) // at least keep the placeholder after a restart
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

    /// The backend downloaded an attachment in its background worker (F4): the thumbnails
    /// that gave up on it can try again — the file is there now, getAttachment serves it.
    private func onAttachmentDownloaded(_ id: String) {
        guard !id.isEmpty else { return }
        for c in chats.convs.values {
            for m in c.msgs {
                if m.attachments.contains(where: { $0.id == id && $0.isImage && $0.thumb.isEmpty }) {
                    fetchThumbnails(c.key, ts: m.ts, author: m.authorKey)
                }
            }
        }
    }

    /// The backend's stored copy of an attachment is removed (F1): expired, deleted for me,
    /// chat deleted. Best-effort, like the local copies the app removes itself; a failure
    /// only means the file stays.
    private func deleteAttachmentFiles(_ atts: [Attachment]) {
        for att in atts where !att.id.isEmpty {
            Task { [weak self, id = att.id] in
                _ = try? await self?.cli.call("deleteAttachment", ["id": id])
            }
        }
    }

    private func fetchThumbnails(_ key: String, ts: Int64, author: String, attempt: Int = 0) {
        if attempt == 0 { fetchLinkPreviewThumbnail(key, ts: ts, author: author) } // retries on its own
        guard let c = chats.convs[key], let mi = chats.findMessage(c, ts: ts, authors: [author]) else { return }
        let m = c.msgs[mi]
        for (a, att) in m.attachments.enumerated() {
            if !att.isImage || !att.thumb.isEmpty || att.size > Media.maxThumbSource { continue }
            // A sticker is fetched by attachment id (corvus-cli 1.6 reports it, F5) or, when the
            // backend reports none, by its pack reference — something fetchable must exist.
            if att.id.isEmpty && (!att.isSticker || !att.isValidSticker) { continue }
            let token = "\(key)|\(ts)|\(author)|\(a)"
            if thumbPending.contains(token) { continue }
            thumbPending.insert(token)
            var p: JSON = ["id": att.id]
            addRecipient(&p, c)
            Task { [p] in
                defer { thumbPending.remove(token) }
                let data: Data
                do {
                    if att.isSticker && att.id.isEmpty {
                        data = try await fetchSticker(att.stickerPackId, att.stickerId)
                    } else {
                        let res = try await cli.call("getAttachment", p)
                        data = Data(base64Encoded: jsonString(jsonObject(res)["data"])) ?? Data()
                    }
                } catch {
                    // The backend may have parked the file (F4: a sender it does not count as
                    // accepted): ask it to download it. A sticker without a pointer here (sent
                    // from another device) is served by an installed pack. Otherwise the
                    // backend may still be downloading it: retry a few times.
                    if attempt == 0, !att.id.isEmpty, let d = try? await downloadParked(att.id, key: key) {
                        data = d
                    } else if att.isSticker && att.isValidSticker, let d = try? await fetchSticker(att.stickerPackId, att.stickerId) {
                        data = d
                    } else {
                        if attempt < 3 {
                            try? await Task.sleep(for: .seconds(2 * (attempt + 1)))
                            thumbPending.remove(token)
                            fetchThumbnails(key, ts: ts, author: author, attempt: attempt + 1)
                        }
                        return
                    }
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
        // A received sticker with an attachment id (corvus-cli 1.6, F5) is fetched like any
        // other attachment: getAttachment serves exactly the image the sender sent. getSticker
        // only knows installed packs; it stays the path for backends that report no id.
        if att.isSticker && att.id.isEmpty {
            guard att.isValidSticker else { throw RpcError("This sticker reference is invalid.") }
            return try await fetchSticker(att.stickerPackId, att.stickerId)
        }
        guard !att.id.isEmpty, let c = chats.convs[key] else { throw RpcError("The attachment is no longer available.") }
        var p: JSON = ["id": att.id]
        addRecipient(&p, c)
        do {
            let res = try await cli.call("getAttachment", p)
            return Data(base64Encoded: jsonString(jsonObject(res)["data"])) ?? Data()
        } catch {
            // The backend may have parked the file (F4): ask it to download it first.
            if let d = try? await downloadParked(att.id, key: key) { return d }
            // A sticker without a pointer here (sent from another device, or swept): an
            // installed pack serves it instead.
            if att.isSticker && att.isValidSticker { return try await fetchSticker(att.stickerPackId, att.stickerId) }
            throw error
        }
    }

    /// The backend parks the attachments (and sticker images) of senders it does not count as
    /// accepted (F4), and its contact sets can miss long-standing chat partners: nothing else
    /// would ever fetch those pointers. An open message request is left alone: accepting it
    /// fetches its attachments (as in the Qt app).
    private func downloadParked(_ id: String, key: String) async throws -> Data {
        if id.isEmpty || hiddenContacts[key] != nil { throw RpcError("The attachment is no longer available.") }
        let res = try await cli.call("downloadAttachment", ["id": id])
        guard let d = Data(base64Encoded: jsonString(jsonObject(res)["data"])), !d.isEmpty else {
            throw RpcError("The attachment is no longer available.")
        }
        return d
    }

    /// Animated GIFs and stickers: the stored preview is a still image, so the original is fetched on demand.
    func loadAnimatedGif(_ m: Message, _ i: Int) {
        guard let key = current, i < m.attachments.count else { return }
        let att = m.attachments[i]
        let token = attachmentToken(m, i)
        guard att.isGif || att.isValidSticker, att.size <= Media.maxThumbSource, animatedGifs[token] == nil, !gifPending.contains(token) else { return }
        gifPending.insert(token)
        Task {
            defer { gifPending.remove(token) }
            if let data = try? await fetchAttachmentData(key, att), !data.isEmpty { animatedGifs[token] = data }
        }
    }

    /// A click on an attachment, following `AttachmentPolicy`: audio, video and images are shown by the app,
    /// files that can run code are never opened (only saved), everything else is opened after confirmation.
    func openAttachment(_ m: Message, _ i: Int) {
        guard let key = current, i < m.attachments.count else { return }
        let att = m.attachments[i]
        if (att.isAudio || att.isVideo)
            && AttachmentPolicy.classify(filename: att.filename, contentType: att.contentType, isMedia: true) == .risky {
            Task { await confirmAttachment(m, i, att, policy: .risky, data: nil) }
            return
        }
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
                    let suffix = AttachmentPolicy.mediaSuffix(filename: att.filename, contentType: att.contentType, fallback: "mp4")
                    videoURLs[token] = try Media.writeAttachmentTemp(data, name: "video." + suffix)
                } catch {
                    showFlash("Video playback failed: \(error.localizedDescription)")
                }
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
            let name = attachmentFileName(att)
            let decoded = att.isImage && NSImage(data: data) != nil
            let policy = AttachmentPolicy.classify(filename: name, contentType: att.contentType, isMedia: decoded)
            if policy == .inApp {
                ImageViewerWindow.show(data: data, title: att.filename)
                return
            }
            clearFlash()
            await confirmAttachment(m, i, att, policy: policy, data: data)
        }
    }

    /// The name a file attachment is written under (the sender's name without any directories).
    private func attachmentFileName(_ att: Attachment) -> String {
        let name = (att.filename as NSString).lastPathComponent
        return name.isEmpty ? "attachment." + Media.preferredSuffix(forMime: att.contentType) : name
    }

    /// The dialog in front of every file that is not shown by the app itself. Risky files only offer "Save …".
    private func confirmAttachment(_ m: Message, _ i: Int, _ att: Attachment, policy: AttachmentPolicy, data: Data?) async {
        let name = attachmentFileName(att)
        let save = { [self] in
            if let data { presentSavePanel(data: data, filename: name) } else { saveAttachment(m, i) }
        }
        if policy == .risky {
            let r = await Dialogs.ask("“\(name)” can run programs on your Mac",
                                      "For your safety, Corvus never opens files of this type. You can save it and check it first.",
                                      buttons: ["Save …"], style: .warning, cancel: true)
            if r == 0 { save() }
            return
        }
        let r = await Dialogs.ask("Open “\(name)”?",
                                  "Files from others can contain malware. Only open files you expected to receive.",
                                  buttons: ["Open", "Save …"], cancel: true)
        switch r {
        case 0:
            guard let data else { return }
            do {
                NSWorkspace.shared.open(try Media.writeAttachmentTemp(data, name: name))
            } catch {
                await Dialogs.warn("Attachment", "The file could not be written.")
            }
        case 1: save()
        default: break
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
            presentSavePanel(data: data, filename: (att.filename as NSString).lastPathComponent)
        }
    }

    /// Saved files are marked as downloaded, so Gatekeeper checks them when they are opened later.
    private func presentSavePanel(data: Data, filename: String) {
        let panel = NSSavePanel()
        panel.title = "Save attachment"
        panel.nameFieldStringValue = filename
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try data.write(to: url)
            Media.quarantine(url)
        } catch {
            Task { await Dialogs.warn("Attachment", "The file could not be written.") }
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
                let suffix = AttachmentPolicy.mediaSuffix(filename: att.filename, contentType: att.contentType, fallback: "m4a")
                let url = try Media.writeAttachmentTemp(data, name: "audio." + suffix)
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
        if chats.isNoteToSelf(c.key) { return Media.noteToSelfAvatar } // fixed glyph, never a profile photo
        return avatars[c.key] ?? Media.placeholderAvatar(key: c.key, title: c.title)
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
            if avatarTried.contains(key) || avatarBusy.contains(key) || chats.isNoteToSelf(key) { continue }
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

    func applyPrivacy(_ p: ChatPrivacy) {
        privacy = p
        p.save()
        if !p.sendTypingIndicators { stopTyping() }
        if !p.sendReadReceipts { unsentReads.removeAll() } // seen while switched off: never reported later
        dismissedPreviewUrl = ""
        updateDraftLinkPreview() // switched on: preview the link already typed; off: drop it
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
        guard connected else { return reportProfileStatus("Connect to \(backendName) before editing your profile.", error: true) }
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
        guard connected else { return reportProfileStatus("Connect to \(backendName) before editing your profile.", error: true) }
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
            req.setValue("corvus", forHTTPHeaderField: "User-Agent")
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
        req.setValue("corvus", forHTTPHeaderField: "User-Agent")
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

    private func github(_ path: String) async throws -> JSON {
        var req = URLRequest(url: URL(string: "https://api.github.com/repos/" + path)!)
        req.setValue("corvus", forHTTPHeaderField: "User-Agent")
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        let (data, resp) = try await URLSession.shared.data(for: req)
        if let code = (resp as? HTTPURLResponse)?.statusCode, code != 200 { throw RpcError("GitHub answered with HTTP \(code).") }
        return (try JSONSerialization.jsonObject(with: data)) as? JSON ?? [:]
    }

    /// corvus-cli publishes one release per commit (tag "commit-<sha>"), so it is compared by commit: the installed build reports its commit in --version,
    /// and GitHub's compare API says whether the latest build is ahead of it. (Same as the Qt app.)
    func checkForUpdates(silent: Bool) {
        let info = backend
        let name = info.name
        let repo = "ada-h00k/corvus-cli"
        Task {
            if info.version.isEmpty {
                if !silent { await Dialogs.info("Update check", "The installed backend version is not known yet. Connect first.") }
                return
            }
            var newer = false, unknown = false
            var latest = "", url = URL(string: "https://github.com/\(repo)/releases/latest")!
            do {
                let o = try await github(repo + "/releases/latest")
                latest = jsonString(o["tag_name"])
                if let u = URL(string: jsonString(o["html_url"])) { url = u }
                if latest.hasPrefix("commit-") { latest.removeFirst(7) }
                let sha = latest.lowercased()
                latest = "(\(sha.prefix(7)))"
                if info.build.isEmpty {
                    unknown = true
                } else if !(sha.hasPrefix(info.build) || info.build.hasPrefix(sha)) {
                    // Is the latest build ahead of the installed one? (It may also be older or on another branch.)
                    newer = jsonString(try await github(repo + "/compare/\(info.build)...\(sha)")["status"]) == "ahead"
                }
            } catch {
                if !silent { await Dialogs.warn("Update check", "Could not reach GitHub:\n\(error.localizedDescription)") }
                return
            }
            updateHint = newer ? UpdateHint(backend: name, version: latest, url: url) : nil
            if silent { return }
            let installed = info.description
            if newer {
                let how = "Update corvus-cli the way you installed it (e.g. brew upgrade --cask corvus-cli) or download it from the release page. "
                let r = await Dialogs.ask("Update available",
                                          "Installed: \(installed)\nLatest version: \(name) \(latest)\n\n\(how)Afterwards choose “File → Reconnect”.",
                                          buttons: ["Open release page", "Close"])
                if r == 0 { NSWorkspace.shared.open(url) }
            } else if unknown {
                let r = await Dialogs.ask("Update check",
                                          "Installed: \(installed)\nLatest build: \(name) \(latest)\n\nThis corvus-cli does not report which build "
                                              + "it is, so Corvus cannot tell whether it is up to date. Builds that do report it are compared automatically.",
                                          buttons: ["Open release page", "Close"])
                if r == 0 { NSWorkspace.shared.open(url) }
            } else {
                await Dialogs.info("Update check", "\(installed) is up to date.")
            }
        }
    }

    func showAbout() {
        let credits = NSMutableAttributedString(
            string: "Graphical front end for Signal. It uses corvus-cli as its backend.\n"
                + "The backend is used as a standalone program and is not part of this app.\n\n"
                + "Backend: \(backend.version.isEmpty ? backendName : backend.description)\nPath: \(cli.executable)"
                + "\nCompatible with signal-cli \(version)"
                + "\n\nChat history (encrypted): \(AppPaths.historyPath)",
            attributes: [.font: NSFont.systemFont(ofSize: 11), .foregroundColor: NSColor.labelColor])
        NSApp.orderFrontStandardAboutPanel(options: [.credits: credits, .applicationName: "Corvus"])
        NSApp.activate(ignoringOtherApps: true)
    }
}
