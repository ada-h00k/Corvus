#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// Group data as reported by signal-cli ("listGroups") and the parameters of the commands that
// change it ("updateGroup", "quitGroup", …).
namespace GroupInfo {

struct Member {
    QString number;
    QString uuid;
    bool admin = false;

    QString id() const { return number.isEmpty() ? uuid : number; }
    bool is(const QString &who) const { return !who.isEmpty() && (number == who || uuid == who); }
};

struct Info {
    QString id; // base64, as used in "updateGroup" (Corvus keys the chat "g:<id>")
    QString name;
    QString description;
    bool isMember = true;
    bool isBlocked = false;
    bool isTerminated = false;
    int expiration = 0;
    QList<Member> members;    // with their admin flag
    QList<Member> pending;    // invited, not yet joined
    QList<Member> requesting; // asked to join through the invite link
    QList<Member> banned;
    QString permAddMember = QStringLiteral("EVERY_MEMBER");     // EVERY_MEMBER | ONLY_ADMINS
    QString permEditDetails = QStringLiteral("EVERY_MEMBER");
    QString permSendMessage = QStringLiteral("EVERY_MEMBER");
    QString inviteLink;       // empty: link disabled

    const Member *member(const QString &who) const;
    bool isAdmin(const QString &who) const;
    int adminCount() const;
    bool canEditDetails(const QString &who) const; // admin, or everybody may
    bool canAddMembers(const QString &who) const;
};

Info parseOne(const QJsonObject &group);
QList<Info> parse(const QJsonValue &listGroupsResult);
// The group with this id in a "listGroups" result, if present.
std::optional<Info> find(const QJsonValue &listGroupsResult, const QString &groupId);

// What the user changed in the settings dialog. Unset fields stay as they are.
struct Edit {
    std::optional<QString> name;
    std::optional<QString> description;
    QString avatarPath;                         // empty: keep the picture
    std::optional<QString> permAddMember;       // EVERY_MEMBER | ONLY_ADMINS
    std::optional<QString> permEditDetails;
    std::optional<QString> permSendMessage;
    std::optional<QString> linkState;           // disabled | enabled | enabled-with-approval
    bool resetLink = false;
};
// "updateGroup" parameters (without groupId) for what differs from `before`; empty if nothing does.
QJsonObject changes(const Info &before, const Edit &edit);

// "updateGroup" parameters that create a group (no groupId).
QJsonObject createParams(const QString &name, const QString &description, const QStringList &members,
                         const QString &avatarPath);
// {key: [ids…]}, e.g. member / removeMember / admin / removeAdmin / ban / unban
QJsonObject memberParams(const QString &key, const QStringList &ids);

// EVERY_MEMBER -> every-member, ONLY_ADMINS -> only-admins
QString permissionParam(const QString &permission);

} // namespace GroupInfo
