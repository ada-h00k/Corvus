#pragma once

#include "GroupInfo.h"
#include "SignalCli.h"

#include <QDialog>
#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class MemberPicker;

// Create a group, or show / change the settings of one: name, description, picture, members
// (add, remove, admins, bans, join requests), permissions and the invite link. Which controls
// are usable depends on whether you are an admin and on the group's permissions.
class GroupDialog : public QDialog
{
    Q_OBJECT
public:
    struct Contact {
        QString id;   // phone number or UUID, as signal-cli knows it
        QString name;
    };
    using NameFn = std::function<QString(const QString &id)>;

    // groupId empty: dialog for a new group. Otherwise: settings of that group (base64 id).
    GroupDialog(SignalCli &cli, const QString &account, const QString &groupId, const QList<Contact> &contacts,
                NameFn nameFor, QWidget *parent = nullptr);

signals:
    void groupCreated(const QString &groupId);
    void groupChanged(const QString &groupId);
    void groupLeft(const QString &groupId);

private:
    void buildCreateUi();
    void buildSettingsUi();
    void reload();
    void showInfo();
    void rebuildMembers();
    void updateMemberButtons();
    void updateSaveState();
    GroupInfo::Edit currentEdit() const;
    // One "updateGroup" call; `then` runs after it succeeded, before the group is reloaded.
    void run(const QJsonObject &params, const QString &failure, std::function<void()> then = {});
    void create();
    void leave();
    void addMembers();
    void chooseAvatar();
    QString labelFor(const QString &id) const;
    bool isSelf(const QString &id) const;
    void setStatus(const QString &text, bool error);
    void setBusy(bool busy);

    SignalCli &m_cli;
    QString m_account;
    QString m_groupId;
    QList<Contact> m_contacts;
    NameFn m_nameFor;
    GroupInfo::Info m_info;
    bool m_loaded = false;
    bool m_busy = false;
    bool m_showing = false; // showInfo() is filling the widgets: not a user edit
    QString m_avatarPath;

    QLabel *m_status = nullptr;
    QLineEdit *m_name = nullptr;
    QPlainTextEdit *m_description = nullptr;
    QLabel *m_avatarLabel = nullptr;
    QPushButton *m_avatarButton = nullptr;
    // create
    MemberPicker *m_picker = nullptr;
    QPushButton *m_createButton = nullptr;
    // settings
    QTabWidget *m_tabs = nullptr;
    QListWidget *m_members = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_adminButton = nullptr;
    QPushButton *m_banButton = nullptr;
    QPushButton *m_approveButton = nullptr;
    QPushButton *m_denyButton = nullptr;
    QPushButton *m_unbanButton = nullptr;
    QComboBox *m_permAdd = nullptr;
    QComboBox *m_permEdit = nullptr;
    QComboBox *m_permSend = nullptr;
    QComboBox *m_link = nullptr;
    QLabel *m_linkUrl = nullptr;
    QPushButton *m_copyLink = nullptr;
    QPushButton *m_resetLink = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_leaveButton = nullptr;
    bool m_linkTouched = false;
    bool m_resetRequested = false;
};
