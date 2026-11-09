import Foundation

/// Group data as reported by signal-cli ("listGroups") and the parameters of the commands that
/// change it ("updateGroup", "quitGroup", …). Port of the Qt app's src/GroupInfo.cpp.
public enum GroupPermission: String, CaseIterable, Sendable {
    case everyMember = "EVERY_MEMBER"
    case onlyAdmins = "ONLY_ADMINS"

    /// signal-cli reports "EVERY_MEMBER" / "every-member"; anything empty or unknown counts as every member.
    public init(reported v: Any?) {
        let s = jsonString(v).trimmingCharacters(in: .whitespaces).uppercased().replacingOccurrences(of: "-", with: "_")
        self = GroupPermission(rawValue: s) ?? .everyMember
    }

    /// Parameter value for "updateGroup": every-member | only-admins
    public var param: String { self == .onlyAdmins ? "only-admins" : "every-member" }
    public var label: String { self == .onlyAdmins ? "Only admins" : "Every member" }
}

/// Invite link setting as "updateGroup" takes it ("link").
public enum GroupLinkState: String, CaseIterable, Sendable {
    case disabled
    case enabled
    case enabledWithApproval = "enabled-with-approval"

    public var label: String {
        switch self {
        case .disabled: return "Off"
        case .enabled: return "On"
        case .enabledWithApproval: return "On, admins approve requests"
        }
    }
}

public struct GroupMember: Equatable, Hashable, Sendable {
    public var number = ""
    public var uuid = ""
    public var admin = false

    public init(number: String = "", uuid: String = "", admin: Bool = false) {
        self.number = number
        self.uuid = uuid
        self.admin = admin
    }

    public var id: String { number.isEmpty ? uuid : number }
    public func isPerson(_ who: String) -> Bool { !who.isEmpty && (number == who || uuid == who) }
}

public struct GroupInfo: Equatable, Sendable {
    public var id = ""                 // base64, as used in "updateGroup" (Corvus keys the chat "g:<id>")
    public var name = ""
    public var description = ""
    public var isMember = true
    public var isBlocked = false
    public var isTerminated = false
    public var expiration = 0
    public var members: [GroupMember] = []    // with their admin flag
    public var pending: [GroupMember] = []    // invited, not yet joined
    public var requesting: [GroupMember] = [] // asked to join through the invite link
    public var banned: [GroupMember] = []
    public var permAddMember = GroupPermission.everyMember
    public var permEditDetails = GroupPermission.everyMember
    public var permSendMessage = GroupPermission.everyMember
    public var inviteLink = ""                // empty: link disabled

    public init() {}

    public func member(_ who: String) -> GroupMember? { members.first { $0.isPerson(who) } }
    public func isAdmin(_ who: String) -> Bool { member(who)?.admin ?? false }
    public var adminCount: Int { members.filter(\.admin).count }
    /// Admin, or everybody may.
    public func canEditDetails(_ who: String) -> Bool { member(who) != nil && (isAdmin(who) || permEditDetails != .onlyAdmins) }
    public func canAddMembers(_ who: String) -> Bool { member(who) != nil && (isAdmin(who) || permAddMember != .onlyAdmins) }
    /// Not a member, but on the list of invited people: an open invitation.
    public func isInvitation(for account: String) -> Bool {
        !isMember && !isTerminated && !id.isEmpty && pending.contains { $0.isPerson(account) }
    }

    // MARK: Parsing

    private static func parseMembers(_ v: Any?, withAdmin: Bool) -> [GroupMember] {
        jsonArray(v).compactMap { mv in
            let o = jsonObject(mv)
            let m = GroupMember(number: jsonString(o["number"]), uuid: jsonString(o["uuid"]),
                                admin: withAdmin && jsonBool(o["isAdmin"]))
            return m.number.isEmpty && m.uuid.isEmpty ? nil : m
        }
    }

    public static func parseOne(_ g: JSON) -> GroupInfo {
        var info = GroupInfo()
        info.id = jsonString(g["id"])
        info.name = jsonString(g["name"])
        info.description = jsonString(g["description"])
        info.isMember = g["isMember"] == nil || jsonBool(g["isMember"])
        info.isBlocked = jsonBool(g["isBlocked"])
        info.isTerminated = jsonBool(g["isTerminated"])
        info.expiration = jsonInt(g["messageExpirationTime"])
        info.members = parseMembers(g["members"], withAdmin: true)
        info.pending = parseMembers(g["pendingMembers"], withAdmin: false)
        info.requesting = parseMembers(g["requestingMembers"], withAdmin: false)
        info.banned = parseMembers(g["banned"], withAdmin: false)
        // Older signal-cli versions only list the admins separately.
        if g["members"] == nil || info.adminCount == 0 {
            for a in parseMembers(g["admins"], withAdmin: false) {
                for i in info.members.indices where info.members[i].isPerson(a.number) || info.members[i].isPerson(a.uuid) {
                    info.members[i].admin = true
                }
            }
        }
        info.permAddMember = GroupPermission(reported: g["permissionAddMember"])
        info.permEditDetails = GroupPermission(reported: g["permissionEditDetails"])
        info.permSendMessage = GroupPermission(reported: g["permissionSendMessage"])
        info.inviteLink = jsonString(g["groupInviteLink"])
        return info
    }

    public static func parse(_ listGroupsResult: Any?) -> [GroupInfo] {
        jsonArray(listGroupsResult).map { parseOne(jsonObject($0)) }.filter { !$0.id.isEmpty }
    }

    /// The group with this id in a "listGroups" result, if present.
    public static func find(_ listGroupsResult: Any?, id: String) -> GroupInfo? {
        parse(listGroupsResult).first { $0.id == id }
    }

    // MARK: Commands

    /// What the user changed in the settings dialog. Unset fields stay as they are.
    public struct Edit: Sendable {
        public var name: String?
        public var description: String?
        public var avatarPath = ""                  // empty: keep the picture
        public var permAddMember: GroupPermission?
        public var permEditDetails: GroupPermission?
        public var permSendMessage: GroupPermission?
        public var linkState: GroupLinkState?
        public var resetLink = false
        public init() {}
    }

    /// "updateGroup" parameters (without groupId) for what differs from `self`; empty if nothing does.
    public func changes(_ edit: Edit) -> JSON {
        var p: JSON = [:]
        if let n = edit.name, n != name { p["name"] = n }
        if let d = edit.description, d != description { p["description"] = d }
        if !edit.avatarPath.isEmpty { p["avatar"] = edit.avatarPath }
        if let v = edit.permAddMember, v != permAddMember { p["setPermissionAddMember"] = v.param }
        if let v = edit.permEditDetails, v != permEditDetails { p["setPermissionEditDetails"] = v.param }
        if let v = edit.permSendMessage, v != permSendMessage { p["setPermissionSendMessages"] = v.param }
        if let l = edit.linkState { p["link"] = l.rawValue }
        if edit.resetLink { p["resetLink"] = true }
        return p
    }

    /// "updateGroup" parameters that create a group (no groupId).
    public static func createParams(name: String, description: String, members: [String], avatarPath: String) -> JSON {
        var p: JSON = ["name": name]
        if !description.isEmpty { p["description"] = description }
        if !members.isEmpty { p["member"] = members }
        if !avatarPath.isEmpty { p["avatar"] = avatarPath }
        return p
    }

    /// {key: [ids…]}, e.g. member / removeMember / admin / removeAdmin / ban / unban
    public static func memberParams(_ key: String, _ ids: [String]) -> JSON { [key: ids] }
}
