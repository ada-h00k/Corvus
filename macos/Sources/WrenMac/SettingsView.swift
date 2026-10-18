import AppKit
import WrenCore
import SwiftUI
import UniformTypeIdentifiers

struct SettingsView: View {
    var body: some View {
        TabView {
            GeneralSettings().tabItem { Label("General", systemImage: "gearshape") }
            AppearanceSettings().tabItem { Label("Appearance", systemImage: "paintpalette") }
            NotificationSettingsView().tabItem { Label("Notifications", systemImage: "bell.badge") }
            ProfileSettings().tabItem { Label("Profile", systemImage: "person.crop.circle") }
        }
        .frame(width: 520)
    }
}

// MARK: - General

private struct GeneralSettings: View {
    @Environment(AppModel.self) private var model
    @AppStorage(MessageInput.spellCheckKey) private var spellCheck = false

    var body: some View {
        Form {
            Section("signal-cli") {
                LabeledContent("Path") {
                    Text(model.cli.executable.isEmpty ? "not found" : model.cli.executable)
                        .textSelection(.enabled)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                LabeledContent("Version", value: model.version.isEmpty ? "–" : model.version)
                HStack {
                    Button("Choose signal-cli …") { model.chooseExecutable() }
                    Button("Reconnect") { model.reconnect() }
                    Button("Check for update") { model.checkForUpdates(silent: false) }
                }
            }
            Section("Chat history") {
                LabeledContent("File") {
                    Text(AppPaths.historyPath).textSelection(.enabled).lineLimit(1).truncationMode(.middle)
                }
                LabeledContent("Protection", value: model.historyProtectionMode == "keychain" ? "System keychain" : "Passphrase")
                Button("Change history protection …") { Task { await model.changeHistoryProtection() } }
                    .disabled(!model.historyReady)
            }
            Section("Message field") {
                Toggle("Check spelling while typing", isOn: $spellCheck)
            }
            Section("GIPHY") {
                Button("GIPHY API key …") { Task { await model.configureGiphyApiKey() } }
            }
        }
        .formStyle(.grouped)
        .frame(height: 400)
    }
}

// MARK: - Appearance

private struct AppearanceSettings: View {
    @Environment(AppModel.self) private var model
    @State private var a = Appearance()
    @State private var loaded = false

    var body: some View {
        Form {
            Section("Bubbles") {
                colorRow("Your messages", \.outBg, a.outBgColor)
                colorRow("Text of your messages", \.outText, a.outTextColor)
                colorRow("Received messages", \.inBg, a.inBgColor)
                colorRow("Text of received messages", \.inText, a.inTextColor)
                colorRow("Chat background", \.background, a.backgroundColor)
                Stepper("Corner radius: \(a.radius) pt", value: $a.radius, in: 0...28)
            }
            Section("Chat list") {
                Toggle("Show contact avatars", isOn: $a.showAvatars)
            }
            Section("Preview") {
                VStack(spacing: 6) {
                    previewBubble("Hi! How are you?", outgoing: false)
                    previewBubble("Great, thanks – and you?", outgoing: true)
                }
                .padding(10)
                .frame(maxWidth: .infinity)
                .background(a.backgroundColor)
            }
            HStack {
                Button("Reset to defaults") { a = Appearance() }
                Spacer()
                Text("Untouched colours follow the light or dark system appearance.").font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .frame(height: 520)
        .onAppear {
            if !loaded { a = model.appearance; loaded = true }
        }
        .onChange(of: a) { _, new in model.applyAppearance(new) } // live
    }

    private func colorRow(_ title: String, _ kp: WritableKeyPath<Appearance, String?>, _ current: Color) -> some View {
        HStack {
            ColorPicker(title, selection: Binding(get: { current }, set: { a[keyPath: kp] = Appearance.hex($0) }), supportsOpacity: false)
            if a[keyPath: kp] != nil {
                Button { a[keyPath: kp] = nil } label: { Image(systemName: "arrow.uturn.backward") }
                    .buttonStyle(.borderless)
                    .help("Back to the default colour")
            }
        }
    }

    private func previewBubble(_ text: String, outgoing: Bool) -> some View {
        HStack {
            if outgoing { Spacer() }
            Text(text)
                .foregroundStyle(outgoing ? a.outTextColor : a.inTextColor)
                .padding(.horizontal, 10)
                .padding(.vertical, 7)
                .background(RoundedRectangle(cornerRadius: CGFloat(a.radius), style: .continuous)
                    .fill(outgoing ? a.outBgColor : a.inBgColor))
            if !outgoing { Spacer() }
        }
    }
}

// MARK: - Notifications

private struct NotificationSettingsView: View {
    @Environment(AppModel.self) private var model
    @State private var n = NotificationSettings()
    @State private var loaded = false
    @State private var soundError: String?

    var body: some View {
        Form {
            Section("Notifications") {
                Toggle("Show notifications for new messages", isOn: $n.notifications)
                Picker("Notifications show", selection: $n.content) {
                    Text("Name and message").tag(NotificationSettings.Content.nameAndText)
                    Text("Name only").tag(NotificationSettings.Content.nameOnly)
                    Text("Nothing (just “New message”)").tag(NotificationSettings.Content.hidden)
                }
                .disabled(!n.notifications)
                if !model.notifier.available {
                    Text("Notifications need the app bundle (run it via scripts/build-app.sh, not swift run).")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Button("Notification settings in System Settings …") {
                    NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.Notifications-Settings.extension")!)
                }
            }
            Section("Sound") {
                Toggle("Play a sound for new messages", isOn: $n.sound)
                Picker("Sound", selection: $n.soundId) {
                    ForEach(SoundPlayer.builtins, id: \.id) { Text($0.title).tag($0.id) }
                    Text("Custom file").tag("custom")
                }
                .disabled(!n.sound)
                if n.soundId == "custom" {
                    HStack {
                        Text(n.soundFile.isEmpty ? "No file chosen" : (n.soundFile as NSString).lastPathComponent)
                            .foregroundStyle(.secondary)
                        Spacer()
                        Button("Choose …") { chooseSound() }
                    }
                }
                HStack {
                    Button("Test") { soundError = model.sound.play(n) }
                    if let soundError { Text(soundError).font(.caption).foregroundStyle(.red) }
                }
                Text("Muted chats (right-click a chat) and the chat you are looking at never alert.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Menu bar") {
                Toggle("Show icon in the menu bar", isOn: $n.trayIcon)
                Toggle("Keep running in the menu bar when the window is closed", isOn: $n.closeToTray)
                    .disabled(!n.trayIcon)
                Toggle("Start with the window hidden", isOn: $n.startMinimizedInTray)
                    .disabled(!n.trayIcon)
            }
        }
        .formStyle(.grouped)
        .frame(height: 560)
        .onAppear {
            if !loaded { n = model.notify; loaded = true }
        }
        .onChange(of: n) { _, new in model.applyNotifications(new) }
    }

    private func chooseSound() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.audio]
        if panel.runModal() == .OK, let url = panel.url { n.soundFile = url.path }
    }
}

// MARK: - Profile

private struct ProfileSettings: View {
    @Environment(AppModel.self) private var model
    @State private var username = ""
    @State private var displayName = ""

    var body: some View {
        Form {
            if !model.connected {
                Text("Connect to signal-cli before editing your profile.").foregroundStyle(.secondary)
            }
            Section("Signal username") {
                LabeledContent("Current", value: model.profileUsername.isEmpty ? "…" : model.profileUsername)
                HStack {
                    TextField("New username (e.g. name.42 or just name)", text: $username)
                    Button("Set") { model.applySignalUsername(username.trimmingCharacters(in: .whitespaces)) }
                        .disabled(username.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
            Section("Display name") {
                HStack {
                    TextField("Name shown to your contacts", text: $displayName)
                    Button("Save") {
                        model.sendProfileUpdate(["givenName": displayName.trimmingCharacters(in: .whitespaces), "familyName": "",
                                                 "removeAvatar": false], success: "Signal display name updated.")
                    }
                    .disabled(displayName.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
            Section("Avatar") {
                HStack {
                    Button("Choose image …") { chooseAvatar() }
                    Button("Remove avatar", role: .destructive) {
                        model.sendProfileUpdate(["removeAvatar": true], success: "Signal avatar removed.")
                    }
                }
            }
            if let s = model.profileStatus {
                Text(s.text).foregroundStyle(s.error ? .red : .secondary)
            }
        }
        .formStyle(.grouped)
        .disabled(!model.connected)
        .frame(height: 380)
        .onAppear {
            model.profileStatus = nil
            model.loadSignalUsername()
        }
    }

    private func chooseAvatar() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.png, .jpeg, .gif, .webP, .heic]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        // The copy stays in the temporary directory until Wren quits.
        let avatar = Media.stageForSignalCli([url.path]).paths[0]
        model.sendProfileUpdate(["avatar": avatar, "removeAvatar": false], success: "Signal avatar updated.")
    }
}
