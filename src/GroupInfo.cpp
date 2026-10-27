#include "GroupInfo.h"

#include <QJsonArray>

namespace GroupInfo {

namespace {

Member parseMember(const QJsonObject &o, bool withAdmin)
{
    Member m;
    m.number = o.value(QStringLiteral("number")).toString();
    m.uuid = o.value(QStringLiteral("uuid")).toString();
    if (withAdmin) m.admin = o.value(QStringLiteral("isAdmin")).toBool();
    return m;
}

QList<Member> parseMembers(const QJsonValue &v, bool withAdmin)
{
    QList<Member> out;
    for (const QJsonValue &mv : v.toArray()) {
        const Member m = parseMember(mv.toObject(), withAdmin);
        if (!m.number.isEmpty() || !m.uuid.isEmpty()) out << m;
    }
    return out;
}

QString permission(const QJsonValue &v)
{
    const QString s = v.toString().trimmed().toUpper().replace(QLatin1Char('-'), QLatin1Char('_'));
    return s.isEmpty() ? QStringLiteral("EVERY_MEMBER") : s;
}

} // namespace

const Member *Info::member(const QString &who) const
{
    for (const Member &m : members)
        if (m.is(who)) return &m;
    return nullptr;
}

bool Info::isAdmin(const QString &who) const
{
    const Member *m = member(who);
    return m && m->admin;
}

int Info::adminCount() const
{
    int n = 0;
    for (const Member &m : members)
        if (m.admin) ++n;
    return n;
}

bool Info::canEditDetails(const QString &who) const
{
    return member(who) && (isAdmin(who) || permEditDetails != QLatin1String("ONLY_ADMINS"));
}

bool Info::canAddMembers(const QString &who) const
{
    return member(who) && (isAdmin(who) || permAddMember != QLatin1String("ONLY_ADMINS"));
}

Info parseOne(const QJsonObject &g)
{
    Info info;
    info.id = g.value(QStringLiteral("id")).toString();
    info.name = g.value(QStringLiteral("name")).toString();
    info.description = g.value(QStringLiteral("description")).toString();
    info.isMember = !g.contains(QStringLiteral("isMember")) || g.value(QStringLiteral("isMember")).toBool();
    info.isBlocked = g.value(QStringLiteral("isBlocked")).toBool();
    info.isTerminated = g.value(QStringLiteral("isTerminated")).toBool();
    info.expiration = g.value(QStringLiteral("messageExpirationTime")).toInt();
    info.members = parseMembers(g.value(QStringLiteral("members")), true);
    info.pending = parseMembers(g.value(QStringLiteral("pendingMembers")), false);
    info.requesting = parseMembers(g.value(QStringLiteral("requestingMembers")), false);
    info.banned = parseMembers(g.value(QStringLiteral("banned")), false);
    // Older signal-cli versions only list the admins separately.
    if (!g.contains(QStringLiteral("members")) || info.adminCount() == 0) {
        for (const Member &a : parseMembers(g.value(QStringLiteral("admins")), false))
            for (Member &m : info.members)
                if (m.is(a.number) || m.is(a.uuid)) m.admin = true;
    }
    info.permAddMember = permission(g.value(QStringLiteral("permissionAddMember")));
    info.permEditDetails = permission(g.value(QStringLiteral("permissionEditDetails")));
    info.permSendMessage = permission(g.value(QStringLiteral("permissionSendMessage")));
    info.inviteLink = g.value(QStringLiteral("groupInviteLink")).toString();
    return info;
}

QList<Info> parse(const QJsonValue &result)
{
    QList<Info> out;
    for (const QJsonValue &gv : result.toArray()) {
        const Info info = parseOne(gv.toObject());
        if (!info.id.isEmpty()) out << info;
    }
    return out;
}

std::optional<Info> find(const QJsonValue &result, const QString &groupId)
{
    for (const Info &info : parse(result))
        if (info.id == groupId) return info;
    return std::nullopt;
}

QString permissionParam(const QString &p)
{
    return p == QLatin1String("ONLY_ADMINS") ? QStringLiteral("only-admins") : QStringLiteral("every-member");
}

QJsonObject changes(const Info &before, const Edit &edit)
{
    QJsonObject p;
    if (edit.name && *edit.name != before.name) p[QStringLiteral("name")] = *edit.name;
    if (edit.description && *edit.description != before.description)
        p[QStringLiteral("description")] = *edit.description;
    if (!edit.avatarPath.isEmpty()) p[QStringLiteral("avatar")] = edit.avatarPath;
    if (edit.permAddMember && *edit.permAddMember != before.permAddMember)
        p[QStringLiteral("setPermissionAddMember")] = permissionParam(*edit.permAddMember);
    if (edit.permEditDetails && *edit.permEditDetails != before.permEditDetails)
        p[QStringLiteral("setPermissionEditDetails")] = permissionParam(*edit.permEditDetails);
    if (edit.permSendMessage && *edit.permSendMessage != before.permSendMessage)
        p[QStringLiteral("setPermissionSendMessages")] = permissionParam(*edit.permSendMessage);
    if (edit.linkState) p[QStringLiteral("link")] = *edit.linkState;
    if (edit.resetLink) p[QStringLiteral("resetLink")] = true;
    return p;
}

QJsonObject createParams(const QString &name, const QString &description, const QStringList &members,
                         const QString &avatarPath)
{
    QJsonObject p{{QStringLiteral("name"), name}};
    if (!description.isEmpty()) p[QStringLiteral("description")] = description;
    if (!members.isEmpty()) p[QStringLiteral("member")] = QJsonArray::fromStringList(members);
    if (!avatarPath.isEmpty()) p[QStringLiteral("avatar")] = avatarPath;
    return p;
}

QJsonObject memberParams(const QString &key, const QStringList &ids)
{
    return {{key, QJsonArray::fromStringList(ids)}};
}

} // namespace GroupInfo
