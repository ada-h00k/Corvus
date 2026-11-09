import AppKit
import CorvusCore
import SwiftUI
import UniformTypeIdentifiers

/// The group sheets: New group, Group settings and Group invitations (port of the Qt app's
/// src/GroupDialog.cpp and src/GroupInvitesDialog.cpp).
struct GroupSheetView: View {
    let sheet: GroupSheet

    var body: some View {
        switch sheet {
        case .create: NewGroupView()
        case .settings(let id): GroupSettingsView(groupId: id)
        case .invitations: GroupInvitesView()
        }
    }
}

// MARK: - Shared pieces

/// Red for errors, secondary otherwise.
private struct StatusLine: View {
    let status: (text: String, error: Bool)?
    var body: some View {
        if let s = status, !s.text.isEmpty {
            Text(s.text)
                .font(.callout)
                .foregroundStyle(s.error ? Color.red : Color.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

@MainActor
private func chooseGroupPicture() -> String? {
    let panel = NSOpenPanel()
    panel.title = "Group picture"
    panel.allowedContentTypes = [.png, .jpeg, .webP, .gif]
    panel.allowsMultipleSelection = false
    return panel.runModal() == .OK ? panel.url?.path : nil
}

/// Checkable list of known contacts plus a field to add someone by number or username.
private struct MemberPicker: View {
    let contacts: [GroupContact]
    let exclude: Set<String>
    @Binding var selected: [String]
    @State private var filter = ""
    @State private var manual = ""
    @State private var extra: [GroupContact] = []  // added by hand

    var body: some View {
        let q = filter.trimmingCharacters(in: .whitespaces)
        let rows = (contacts + extra).filter { !exclude.contains($0.id) }.filter {
            q.isEmpty || $0.name.localizedCaseInsensitiveContains(q) || $0.id.localizedCaseInsensitiveContains(q)
        }
        VStack(spacing: 8) {
            TextField("Search contacts", text: $filter).textFieldStyle(.roundedBorder)
            List(rows) { c in
                Toggle(isOn: Binding(get: { selected.contains(c.id) }, set: { on in
                    if on { if !selected.contains(c.id) { selected.append(c.id) } } else { selected.removeAll { $0 == c.id } }
                })) {
                    if c.name == c.id { Text(c.id) } else {
                        Text(c.name) + Text("  \(c.id)").foregroundColor(.secondary)
                    }
                }
                .toggleStyle(.checkbox)
            }
            .listStyle(.bordered(alternatesRowBackgrounds: true))
            .overlay {
                if rows.isEmpty { Text("No contacts").foregroundStyle(.secondary) }
            }
            HStack {
                TextField("Phone number (+49…) or username", text: $manual)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit(addManual)
                Button("Add", action: addManual).disabled(manual.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
    }

    private func addManual() {
        let id = manual.trimmingCharacters(in: .whitespaces)
        guard !id.isEmpty, !exclude.contains(id) else { return }
        if !contacts.contains(where: { $0.id == id }) && !extra.contains(where: { $0.id == id }) {
            extra.append(GroupContact(id: id, name: id))
        }
        if !selected.contains(id) { selected.append(id) }
        manual = ""
    }
}

// MARK: - New group

private struct NewGroupView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var description = ""
    @State private var avatarPath = ""
    @State private var members: [String] = []
    @State private var busy = false
    @State private var status: (text: String, error: Bool)?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("New group").font(.title3.weight(.semibold))
            Form {
                TextField("Name:", text: $name, prompt: Text("Group name"))
                TextField("Description:", text: $description, prompt: Text("Description (optional)"), axis: .vertical)
                    .lineLimit(2...4)
                LabeledContent("Picture:") {
                    HStack {
                        Button("Choose picture …") { if let p = chooseGroupPicture() { avatarPath = p } }
                        Text(avatarPath.isEmpty ? "No picture" : (avatarPath as NSString).lastPathComponent)
                            .foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    }
                }
            }
            Text("Members:").font(.headline)
            MemberPicker(contacts: model.groupContacts(), exclude: [model.account], selected: $members)
            StatusLine(status: status)
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Create group") { Task { await create() } }
                    .keyboardShortcut(.defaultAction)
                    .disabled(busy || name.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .padding(20)
        .frame(width: 520, height: 580)
        .disabled(busy)
    }

    private func create() async {
        let n = name.trimmingCharacters(in: .whitespaces)
        guard !n.isEmpty, !busy else { return }
        busy = true
        status = ("Creating the group …", false)
        let params = GroupInfo.createParams(name: n, description: description.trimmingCharacters(in: .whitespacesAndNewlines),
                                            members: members, avatarPath: avatarPath)
        do {
            let id = jsonString(jsonObject(try await model.updateGroup(nil, params))["groupId"])
            busy = false
            guard !id.isEmpty else {
                status = ("Could not create the group: no group id returned", true)
                return
            }
            dismiss()
            await model.showGroup(id)
        } catch {
            busy = false
            status = ("Could not create the group: \(error.localizedDescription)", true)
        }
    }
}

// MARK: - Group settings

private struct GroupSettingsView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let groupId: String

    @State private var info = GroupInfo()
    @State private var loaded = false
    @State private var busy = true
    @State private var status: (text: String, error: Bool)? = ("Loading …", false)

    // edits
    @State private var name = ""
    @State private var description = ""
    @State private var avatarPath = ""
    @State private var permAdd = GroupPermission.everyMember
    @State private var permEdit = GroupPermission.everyMember
    @State private var permSend = GroupPermission.everyMember
    @State private var link = GroupLinkState.disabled
    @State private var linkTouched = false

    @State private var selection: String?   // "<category>|<id>"
    @State private var addingMembers = false

    private var me: String { model.account }
    private var isMember: Bool { loaded && info.isMember && info.member(me) != nil }
    private var iAmAdmin: Bool { isMember && info.isAdmin(me) }
    private var canEdit: Bool { isMember && info.canEditDetails(me) }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(info.name.isEmpty ? "Group settings" : "Group settings – \(info.name)")
                .font(.title3.weight(.semibold))
                .lineLimit(1)
            TabView {
                details.tabItem { Text("Details") }
                membersTab.tabItem { Text("Members") }
                permissions.tabItem { Text("Permissions and link") }
            }
            .disabled(busy)
            StatusLine(status: status)
            HStack {
                Button("Leave group …", role: .destructive) { Task { await leave() } }
                    .disabled(busy || !loaded || !info.isMember)
                Spacer()
                Button("Save changes") { Task { await save() } }
                    .disabled(!canSave)
                Button("Close") { dismiss() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding(20)
        .frame(width: 600, height: 540)
        .task { await reload() }
        .sheet(isPresented: $addingMembers) {
            AddMembersView(exclude: excludedFromAdding) { ids in
                Task { await run(GroupInfo.memberParams("member", ids), failure: "Could not add the members") }
            }
        }
    }

    // MARK: Tabs

    private var details: some View {
        Form {
            TextField("Name:", text: $name)
            TextField("Description:", text: $description, axis: .vertical).lineLimit(3...6)
            LabeledContent("Picture:") {
                HStack {
                    Button("Change picture …") { if let p = chooseGroupPicture() { avatarPath = p } }
                    Text(avatarPath.isEmpty ? "Unchanged" : (avatarPath as NSString).lastPathComponent)
                        .foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                }
            }
        }
        .disabled(!canEdit)
        .padding(8)
    }

    private enum Category: String { case member, pending, requesting, banned }

    private struct Row: Identifiable {
        var category: Category
        var member: GroupMember
        var id: String { category.rawValue + "|" + member.id }
    }

    private var rows: [Row] {
        func sorted(_ l: [GroupMember]) -> [GroupMember] {
            l.sorted { label($0.id).localizedStandardCompare(label($1.id)) == .orderedAscending }
        }
        return sorted(info.members).map { Row(category: .member, member: $0) }
            + sorted(info.pending).map { Row(category: .pending, member: $0) }
            + sorted(info.requesting).map { Row(category: .requesting, member: $0) }
            + sorted(info.banned).map { Row(category: .banned, member: $0) }
    }

    private var selectedRow: Row? { rows.first { $0.id == selection } }

    private var membersTab: some View {
        VStack(alignment: .leading, spacing: 8) {
            List(rows, selection: $selection) { r in
                HStack {
                    Text(label(r.member.id)).lineLimit(1)
                    Spacer()
                    switch r.category {
                    case .member: if r.member.admin { tag("Admin") }
                    case .pending: tag("Invited")
                    case .requesting: tag("Wants to join")
                    case .banned: tag("Banned")
                    }
                }
                .tag(r.id)
            }
            .listStyle(.bordered(alternatesRowBackgrounds: true))
            memberButtons
        }
        .padding(8)
    }

    private func tag(_ t: String) -> some View {
        Text(t).font(.caption).foregroundStyle(.secondary)
            .padding(.horizontal, 6).padding(.vertical, 1)
            .background(Capsule().fill(Theme.field))
    }

    @ViewBuilder private var memberButtons: some View {
        let r = selectedRow
        let isSelf = r.map { isSelfId($0.member.id) } ?? false
        HStack {
            Button("Add members …") { addingMembers = true }
                .disabled(!(isMember && info.canAddMembers(me)))
            if let r, iAmAdmin {
                switch r.category {
                case .member where !isSelf:
                    Button("Remove") { Task { await removeMember(r) } }
                    Button(r.member.admin ? "Remove admin" : "Make admin") {
                        Task {
                            await run(GroupInfo.memberParams(r.member.admin ? "removeAdmin" : "admin", [r.member.id]),
                                      failure: "Could not change admin rights")
                        }
                    }
                    Button("Ban") { Task { await ban(r) } }
                case .pending:
                    Button("Withdraw invitation") { Task { await removeMember(r) } }
                case .requesting:
                    Button("Approve") {
                        Task { await run(GroupInfo.memberParams("member", [r.member.id]), failure: "Could not approve the request") }
                    }
                    Button("Deny") {
                        Task { await run(GroupInfo.memberParams("removeMember", [r.member.id]), failure: "Could not deny the request") }
                    }
                case .banned:
                    Button("Unban") {
                        Task { await run(GroupInfo.memberParams("unban", [r.member.id]), failure: "Could not unban") }
                    }
                default:
                    EmptyView()
                }
            }
            Spacer()
        }
    }

    private var permissions: some View {
        Form {
            Picker("Who can add members:", selection: $permAdd) { permissionItems }
            Picker("Who can edit name, picture and description:", selection: $permEdit) { permissionItems }
            Picker("Who can send messages:", selection: $permSend) { permissionItems }
            Picker("Invite link:", selection: Binding(get: { link }, set: { link = $0; linkTouched = true })) {
                ForEach(GroupLinkState.allCases, id: \.self) { Text($0.label).tag($0) }
            }
            .help("signal-cli does not report whether admin approval is required. The setting is only sent when you change it here.")
            LabeledContent("") {
                HStack {
                    Text(info.inviteLink.isEmpty ? "(no link)" : info.inviteLink)
                        .textSelection(.enabled)
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                        .truncationMode(.middle)
                    Spacer()
                    Button("Copy") {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(info.inviteLink, forType: .string)
                        status = ("Link copied.", false)
                    }
                    .disabled(info.inviteLink.isEmpty)
                    Button("Reset link") { Task { await resetLink() } }
                        .disabled(!iAmAdmin || info.inviteLink.isEmpty)
                }
            }
        }
        .disabled(!iAmAdmin)
        .padding(8)
    }

    @ViewBuilder private var permissionItems: some View {
        ForEach(GroupPermission.allCases, id: \.self) { Text($0.label).tag($0) }
    }

    // MARK: Logic

    private func isSelfId(_ id: String) -> Bool {
        if id.isEmpty { return false }
        if id == me { return true }
        return info.member(me)?.isPerson(id) ?? false
    }

    /// "You", "Name  (id)" or the id.
    private func label(_ id: String) -> String {
        if isSelfId(id) { return "You" }
        let n = model.memberName(id)
        return n.isEmpty || n == id ? id : "\(n)  (\(id))"
    }

    private var excludedFromAdding: Set<String> {
        var s: Set<String> = [me]
        for m in info.members + info.pending + info.banned { s.insert(m.number); s.insert(m.uuid) }
        s.remove("")
        return s
    }

    private var currentEdit: GroupInfo.Edit {
        var e = GroupInfo.Edit()
        if canEdit {
            let n = name.trimmingCharacters(in: .whitespaces)
            if !n.isEmpty { e.name = n } // a group needs a name
            e.description = description.trimmingCharacters(in: .whitespacesAndNewlines)
            e.avatarPath = avatarPath
        }
        if iAmAdmin {
            e.permAddMember = permAdd
            e.permEditDetails = permEdit
            e.permSendMessage = permSend
            if linkTouched { e.linkState = link }
        }
        return e
    }

    private var canSave: Bool { loaded && !busy && info.isMember && !info.changes(currentEdit).isEmpty }

    private func reload() async {
        do {
            let i = try await model.loadGroup(groupId)
            info = i
            loaded = true
            name = i.name
            description = i.description
            permAdd = i.permAddMember
            permEdit = i.permEditDetails
            permSend = i.permSendMessage
            link = i.inviteLink.isEmpty ? .disabled : .enabled
            linkTouched = false
            if selection != nil && selectedRow == nil { selection = nil }
            if !i.isMember { status = ("You are not a member of this group.", false) }
            else if status?.text == "Loading …" { status = nil }
        } catch {
            status = ("Could not load the group: \(error.localizedDescription)", true)
        }
        busy = false
    }

    /// One "updateGroup" call, then reload; `failure` prefixes the error text.
    private func run(_ params: JSON, failure: String, then: (() -> Void)? = nil) async {
        guard !busy else { return }
        busy = true
        status = ("Saving …", false)
        do {
            try await model.updateGroup(groupId, params)
            status = nil
            then?()
            Task { await model.refreshGroups() }
            await reload()
        } catch {
            busy = false
            status = ("\(failure): \(error.localizedDescription)", true)
        }
    }

    private func save() async {
        let params = info.changes(currentEdit)
        guard !params.isEmpty else { return }
        let avatarSent = params["avatar"] != nil
        await run(params, failure: "Could not save the changes") {
            if avatarSent { avatarPath = "" }
            linkTouched = false
        }
        if status == nil { status = ("Saved.", false) }
    }

    private func removeMember(_ r: Row) async {
        let text = r.category == .pending ? "Withdraw the invitation for \(label(r.member.id))?"
            : "Remove \(label(r.member.id)) from the group?"
        guard await Dialogs.confirm("Remove", text, ok: "Remove", destructive: true) else { return }
        await run(GroupInfo.memberParams("removeMember", [r.member.id]), failure: "Could not remove the member")
    }

    private func ban(_ r: Row) async {
        guard await Dialogs.confirm("Ban", "Remove \(label(r.member.id)) from the group and ban them from rejoining?",
                                    ok: "Ban", destructive: true) else { return }
        await run(GroupInfo.memberParams("ban", [r.member.id]), failure: "Could not ban the member")
    }

    private func resetLink() async {
        guard await Dialogs.confirm("Reset link", "Create a new invite link? The old link will stop working.",
                                    ok: "Reset link") else { return }
        await run(["resetLink": true], failure: "Could not reset the link")
    }

    private func leave() async {
        guard loaded, !busy, info.isMember else { return }
        guard await Dialogs.confirm("Leave group", "Leave “\(info.name)”? You will stop receiving its messages.",
                                    ok: "Leave", destructive: true) else { return }
        // signal-cli refuses to let the only admin go without naming a successor.
        var successor: String?
        if info.isAdmin(me) && info.adminCount == 1 && info.members.count > 1 {
            let others = info.members.filter { !isSelfId($0.id) }
            guard let i = await Dialogs.choose("Choose a new admin", "You are the only admin. Who should take over?",
                                               options: others.map { label($0.id) }) else { return }
            successor = others[i].id
        }
        busy = true
        status = ("Leaving …", false)
        do {
            try await model.quitGroup(groupId, successor: successor)
            dismiss()
            await model.groupLeft(groupId)
        } catch {
            busy = false
            status = ("Could not leave the group: \(error.localizedDescription)", true)
        }
    }
}

private struct AddMembersView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    let exclude: Set<String>
    let add: ([String]) -> Void
    @State private var selected: [String] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Add members").font(.title3.weight(.semibold))
            MemberPicker(contacts: model.groupContacts(), exclude: exclude, selected: $selected)
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Add") {
                    dismiss()
                    if !selected.isEmpty { add(selected) }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(selected.isEmpty)
            }
        }
        .padding(20)
        .frame(width: 440, height: 460)
    }
}

// MARK: - Invitations

private struct GroupInvitesView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var selection: String?
    @State private var busy = false
    @State private var status: (text: String, error: Bool)?

    var body: some View {
        let invites = model.groupInvitations
        VStack(alignment: .leading, spacing: 12) {
            Text("Group invitations").font(.title3.weight(.semibold))
            Text("You have been invited to these groups. Accept to join, or decline the invitation.")
                .foregroundStyle(.secondary)
            List(invites, id: \.id, selection: $selection) { g in
                HStack {
                    Text(displayName(g))
                    Spacer()
                    Text("\(g.members.count) member\(g.members.count == 1 ? "" : "s")").foregroundStyle(.secondary)
                }
                .tag(g.id)
            }
            .listStyle(.bordered(alternatesRowBackgrounds: true))
            .overlay { if invites.isEmpty { Text("No open invitations").foregroundStyle(.secondary) } }
            .disabled(busy)
            StatusLine(status: status)
            HStack {
                Button("Accept") { Task { await act(accept: true) } }
                Button("Decline") { Task { await act(accept: false) } }
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.cancelAction)
            }
            .disabled(busy)
        }
        .padding(20)
        .frame(width: 460, height: 380)
        .onAppear { if selection == nil { selection = invites.first?.id } }
    }

    private func displayName(_ g: GroupInfo) -> String { g.name.isEmpty ? "Group" : g.name }

    private func act(accept: Bool) async {
        guard !busy, let id = selection, let g = model.groupInvitations.first(where: { $0.id == id }) else { return }
        let name = displayName(g)
        if !accept, !(await Dialogs.confirm("Decline invitation", "Decline the invitation to “\(name)”?", ok: "Decline",
                                             destructive: true)) {
            return
        }
        busy = true
        status = (accept ? "Joining …" : "Declining …", false)
        do {
            // Accepting = "updateGroup" on a group we are invited to; declining = "quitGroup".
            if accept { try await model.updateGroup(id, [:]) } else { try await model.quitGroup(id) }
            busy = false
            status = (accept ? "Joined “\(name)”." : "Declined the invitation to “\(name)”.", false)
            if accept { await model.showGroup(id) } else { await model.refreshGroups() }
            selection = model.groupInvitations.first?.id
        } catch {
            busy = false
            status = ((accept ? "Could not join the group" : "Could not decline the invitation")
                      + ": \(error.localizedDescription)", true)
        }
    }
}
