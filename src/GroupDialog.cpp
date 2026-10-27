#include "GroupDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace {
enum Category { CatMember = 0, CatPending = 1, CatRequesting = 2, CatBanned = 3 };
constexpr int kIdRole = Qt::UserRole;
constexpr int kCategoryRole = Qt::UserRole + 1;
constexpr int kAdminRole = Qt::UserRole + 2;

QString permissionLabel(bool everyone) { return everyone ? QObject::tr("Every member") : QObject::tr("Only admins"); }

void fillPermission(QComboBox *box)
{
    box->addItem(permissionLabel(true), QStringLiteral("EVERY_MEMBER"));
    box->addItem(permissionLabel(false), QStringLiteral("ONLY_ADMINS"));
}

void selectData(QComboBox *box, const QString &data)
{
    const int i = box->findData(data);
    box->setCurrentIndex(i < 0 ? 0 : i);
}
} // namespace

// Checkable list of known contacts plus a field to add someone by number or username.
class MemberPicker : public QWidget
{
public:
    MemberPicker(const QList<GroupDialog::Contact> &contacts, const QStringList &exclude, QWidget *parent)
        : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        m_filter = new QLineEdit;
        m_filter->setPlaceholderText(QObject::tr("Search contacts"));
        m_filter->setClearButtonEnabled(true);
        lay->addWidget(m_filter);
        m_list = new QListWidget;
        m_list->setObjectName(QStringLiteral("memberPickerList"));
        for (const GroupDialog::Contact &c : contacts) {
            if (exclude.contains(c.id)) continue;
            addItem(c.id, c.name == c.id ? c.id : QStringLiteral("%1  (%2)").arg(c.name, c.id), false);
        }
        lay->addWidget(m_list, 1);
        auto *row = new QHBoxLayout;
        m_manual = new QLineEdit;
        m_manual->setObjectName(QStringLiteral("memberPickerManual"));
        m_manual->setPlaceholderText(QObject::tr("Phone number (+49…) or username"));
        auto *add = new QPushButton(QObject::tr("Add"));
        add->setObjectName(QStringLiteral("memberPickerAdd"));
        row->addWidget(m_manual, 1);
        row->addWidget(add);
        lay->addLayout(row);
        QObject::connect(add, &QPushButton::clicked, this, [this] { addManual(); });
        QObject::connect(m_manual, &QLineEdit::returnPressed, this, [this] { addManual(); });
        QObject::connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
            for (int i = 0; i < m_list->count(); ++i)
                m_list->item(i)->setHidden(!text.isEmpty() && !m_list->item(i)->text().contains(text, Qt::CaseInsensitive));
        });
        m_exclude = exclude;
    }

    QStringList selected() const
    {
        QStringList ids;
        for (int i = 0; i < m_list->count(); ++i)
            if (m_list->item(i)->checkState() == Qt::Checked) ids << m_list->item(i)->data(kIdRole).toString();
        return ids;
    }

private:
    void addItem(const QString &id, const QString &label, bool checked)
    {
        auto *item = new QListWidgetItem(label, m_list);
        item->setData(kIdRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }

    void addManual()
    {
        const QString id = m_manual->text().trimmed();
        if (id.isEmpty() || m_exclude.contains(id)) return;
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->data(kIdRole).toString() == id) {
                m_list->item(i)->setCheckState(Qt::Checked);
                m_manual->clear();
                return;
            }
        }
        addItem(id, id, true);
        m_manual->clear();
    }

    QLineEdit *m_filter;
    QListWidget *m_list;
    QLineEdit *m_manual;
    QStringList m_exclude;
};

GroupDialog::GroupDialog(SignalCli &cli, const QString &account, const QString &groupId,
                         const QList<Contact> &contacts, NameFn nameFor, QWidget *parent)
    : QDialog(parent), m_cli(cli), m_account(account), m_groupId(groupId), m_contacts(contacts),
      m_nameFor(std::move(nameFor))
{
    setMinimumWidth(520);
    m_status = new QLabel;
    m_status->setWordWrap(true);
    if (m_groupId.isEmpty()) {
        setWindowTitle(tr("New group"));
        buildCreateUi();
    } else {
        setWindowTitle(tr("Group settings"));
        buildSettingsUi();
        reload();
    }
}

QString GroupDialog::labelFor(const QString &id) const
{
    if (isSelf(id)) return tr("You");
    const QString name = m_nameFor ? m_nameFor(id) : id;
    return name.isEmpty() || name == id ? id : QStringLiteral("%1  (%2)").arg(name, id);
}

bool GroupDialog::isSelf(const QString &id) const
{
    if (id.isEmpty()) return false;
    if (id == m_account) return true;
    const GroupInfo::Member *self = m_info.member(m_account);
    return self && self->is(id);
}

void GroupDialog::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
}

void GroupDialog::setBusy(bool busy)
{
    m_busy = busy;
    if (m_createButton) m_createButton->setEnabled(!busy);
    if (m_tabs) m_tabs->setEnabled(!busy);
    if (m_leaveButton) m_leaveButton->setEnabled(!busy && m_loaded && m_info.isMember);
    if (!busy && m_tabs) {
        updateMemberButtons();
        updateSaveState();
    }
}

// ------------------------------------------------------------------ New group

void GroupDialog::buildCreateUi()
{
    auto *lay = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_name = new QLineEdit;
    m_name->setObjectName(QStringLiteral("groupName"));
    m_name->setPlaceholderText(tr("Group name"));
    form->addRow(tr("Name:"), m_name);
    m_description = new QPlainTextEdit;
    m_description->setObjectName(QStringLiteral("groupDescription"));
    m_description->setPlaceholderText(tr("Description (optional)"));
    m_description->setMaximumHeight(70);
    form->addRow(tr("Description:"), m_description);
    auto *avatarRow = new QHBoxLayout;
    m_avatarButton = new QPushButton(tr("Choose picture …"));
    m_avatarLabel = new QLabel(tr("No picture"));
    avatarRow->addWidget(m_avatarButton);
    avatarRow->addWidget(m_avatarLabel, 1);
    form->addRow(tr("Picture:"), avatarRow);
    lay->addLayout(form);

    lay->addWidget(new QLabel(tr("Members:")));
    m_picker = new MemberPicker(m_contacts, {m_account}, this);
    lay->addWidget(m_picker, 1);
    lay->addWidget(m_status);

    auto *box = new QDialogButtonBox;
    m_createButton = box->addButton(tr("Create group"), QDialogButtonBox::AcceptRole);
    m_createButton->setObjectName(QStringLiteral("createGroupButton"));
    m_createButton->setEnabled(false);
    box->addButton(QDialogButtonBox::Cancel);
    lay->addWidget(box);

    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_createButton, &QPushButton::clicked, this, &GroupDialog::create);
    connect(m_avatarButton, &QPushButton::clicked, this, &GroupDialog::chooseAvatar);
    connect(m_name, &QLineEdit::textChanged, this,
            [this](const QString &text) { m_createButton->setEnabled(!m_busy && !text.trimmed().isEmpty()); });
}

void GroupDialog::create()
{
    const QString name = m_name->text().trimmed();
    if (name.isEmpty() || m_busy) return;
    setBusy(true);
    setStatus(tr("Creating the group …"), false);
    const QJsonObject params = GroupInfo::createParams(name, m_description->toPlainText().trimmed(),
                                                       m_picker->selected(), m_avatarPath);
    QPointer<GroupDialog> self(this);
    m_cli.call(QStringLiteral("updateGroup"), params, [self](const QJsonValue &res, const QString &err) {
        if (!self) return;
        self->setBusy(false);
        const QString id = res.toObject().value(QStringLiteral("groupId")).toString();
        if (!err.isEmpty() || id.isEmpty()) {
            self->setStatus(tr("Could not create the group: %1").arg(err.isEmpty() ? tr("no group id returned") : err), true);
            return;
        }
        emit self->groupCreated(id);
        self->accept();
    });
}

void GroupDialog::chooseAvatar()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Group picture"), QString(),
                                                      tr("Images (*.png *.jpg *.jpeg *.webp *.gif)"));
    if (path.isEmpty()) return;
    m_avatarPath = path;
    m_avatarLabel->setText(QFileInfo(path).fileName());
    if (m_tabs) updateSaveState();
}

// ------------------------------------------------------------------ Settings

void GroupDialog::buildSettingsUi()
{
    auto *lay = new QVBoxLayout(this);
    m_tabs = new QTabWidget;
    lay->addWidget(m_tabs, 1);

    // --- Details
    auto *details = new QWidget;
    auto *form = new QFormLayout(details);
    m_name = new QLineEdit;
    m_name->setObjectName(QStringLiteral("groupName"));
    form->addRow(tr("Name:"), m_name);
    m_description = new QPlainTextEdit;
    m_description->setObjectName(QStringLiteral("groupDescription"));
    m_description->setMaximumHeight(90);
    form->addRow(tr("Description:"), m_description);
    auto *avatarRow = new QHBoxLayout;
    m_avatarButton = new QPushButton(tr("Change picture …"));
    m_avatarLabel = new QLabel(tr("Unchanged"));
    avatarRow->addWidget(m_avatarButton);
    avatarRow->addWidget(m_avatarLabel, 1);
    form->addRow(tr("Picture:"), avatarRow);
    m_tabs->addTab(details, tr("Details"));

    // --- Members
    auto *members = new QWidget;
    auto *mlay = new QVBoxLayout(members);
    m_members = new QListWidget;
    m_members->setObjectName(QStringLiteral("groupMembers"));
    mlay->addWidget(m_members, 1);
    auto *buttons = new QHBoxLayout;
    auto makeButton = [&](const QString &text, const char *name) {
        auto *b = new QPushButton(text);
        b->setObjectName(QLatin1String(name));
        buttons->addWidget(b);
        return b;
    };
    m_addButton = makeButton(tr("Add members …"), "addMembersButton");
    m_removeButton = makeButton(tr("Remove"), "removeMemberButton");
    m_adminButton = makeButton(tr("Make admin"), "toggleAdminButton");
    m_banButton = makeButton(tr("Ban"), "banMemberButton");
    m_approveButton = makeButton(tr("Approve"), "approveRequestButton");
    m_denyButton = makeButton(tr("Deny"), "denyRequestButton");
    m_unbanButton = makeButton(tr("Unban"), "unbanMemberButton");
    buttons->addStretch(1);
    mlay->addLayout(buttons);
    m_tabs->addTab(members, tr("Members"));

    // --- Permissions and link
    auto *perms = new QWidget;
    auto *pform = new QFormLayout(perms);
    m_permAdd = new QComboBox;
    m_permAdd->setObjectName(QStringLiteral("permAddMember"));
    m_permEdit = new QComboBox;
    m_permEdit->setObjectName(QStringLiteral("permEditDetails"));
    m_permSend = new QComboBox;
    m_permSend->setObjectName(QStringLiteral("permSendMessage"));
    for (QComboBox *box : {m_permAdd, m_permEdit, m_permSend}) fillPermission(box);
    pform->addRow(tr("Who can add members:"), m_permAdd);
    pform->addRow(tr("Who can edit name, picture and description:"), m_permEdit);
    pform->addRow(tr("Who can send messages:"), m_permSend);
    m_link = new QComboBox;
    m_link->setObjectName(QStringLiteral("linkState"));
    m_link->addItem(tr("Off"), QStringLiteral("disabled"));
    m_link->addItem(tr("On"), QStringLiteral("enabled"));
    m_link->addItem(tr("On, admins approve requests"), QStringLiteral("enabled-with-approval"));
    m_link->setToolTip(tr("signal-cli does not report whether admin approval is required. "
                          "The setting is only sent when you change it here."));
    pform->addRow(tr("Invite link:"), m_link);
    m_linkUrl = new QLabel;
    m_linkUrl->setObjectName(QStringLiteral("inviteLink"));
    m_linkUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_linkUrl->setWordWrap(true);
    auto *linkRow = new QHBoxLayout;
    linkRow->addWidget(m_linkUrl, 1);
    m_copyLink = new QPushButton(tr("Copy"));
    m_resetLink = new QPushButton(tr("Reset link"));
    m_resetLink->setObjectName(QStringLiteral("resetLinkButton"));
    linkRow->addWidget(m_copyLink);
    linkRow->addWidget(m_resetLink);
    pform->addRow(QString(), linkRow);
    m_tabs->addTab(perms, tr("Permissions and link"));

    lay->addWidget(m_status);
    auto *bottom = new QHBoxLayout;
    m_leaveButton = new QPushButton(tr("Leave group …"));
    m_leaveButton->setObjectName(QStringLiteral("leaveGroupButton"));
    bottom->addWidget(m_leaveButton);
    bottom->addStretch(1);
    m_save = new QPushButton(tr("Save changes"));
    m_save->setObjectName(QStringLiteral("saveGroupButton"));
    m_save->setEnabled(false);
    auto *close = new QPushButton(tr("Close"));
    bottom->addWidget(m_save);
    bottom->addWidget(close);
    lay->addLayout(bottom);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_avatarButton, &QPushButton::clicked, this, &GroupDialog::chooseAvatar);
    connect(m_save, &QPushButton::clicked, this, [this] {
        const QJsonObject params = GroupInfo::changes(m_info, currentEdit());
        if (params.isEmpty()) return;
        const bool avatarSent = params.contains(QStringLiteral("avatar"));
        run(params, tr("Could not save the changes: %1"), [this, avatarSent] {
            if (avatarSent) {
                m_avatarPath.clear();
                m_avatarLabel->setText(tr("Unchanged"));
            }
            m_resetRequested = false;
            m_linkTouched = false;
            setStatus(tr("Saved."), false);
        });
    });
    connect(m_leaveButton, &QPushButton::clicked, this, &GroupDialog::leave);
    connect(m_addButton, &QPushButton::clicked, this, &GroupDialog::addMembers);
    connect(m_copyLink, &QPushButton::clicked, this, [this] {
        if (!m_info.inviteLink.isEmpty()) QApplication::clipboard()->setText(m_info.inviteLink);
    });
    connect(m_resetLink, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Reset link"),
                                  tr("Create a new invite link? The old link will stop working."))
            != QMessageBox::Yes)
            return;
        run({{QStringLiteral("resetLink"), true}}, tr("Could not reset the link: %1"));
    });

    auto edited = [this] { if (!m_showing) updateSaveState(); };
    connect(m_name, &QLineEdit::textChanged, this, edited);
    connect(m_description, &QPlainTextEdit::textChanged, this, edited);
    for (QComboBox *box : {m_permAdd, m_permEdit, m_permSend})
        connect(box, &QComboBox::currentIndexChanged, this, edited);
    connect(m_link, &QComboBox::currentIndexChanged, this, [this] {
        if (m_showing) return;
        m_linkTouched = true;
        updateSaveState();
    });

    connect(m_members, &QListWidget::currentItemChanged, this, [this] { updateMemberButtons(); });

    auto memberAction = [this](QPushButton *button, std::function<void(const QString &id, int category, bool admin)> fn) {
        connect(button, &QPushButton::clicked, this, [this, fn] {
            QListWidgetItem *item = m_members->currentItem();
            if (!item || m_busy) return;
            fn(item->data(kIdRole).toString(), item->data(kCategoryRole).toInt(), item->data(kAdminRole).toBool());
        });
    };
    memberAction(m_removeButton, [this](const QString &id, int category, bool) {
        const QString text = category == CatPending ? tr("Withdraw the invitation for %1?") : tr("Remove %1 from the group?");
        if (QMessageBox::question(this, tr("Remove"), text.arg(labelFor(id))) != QMessageBox::Yes) return;
        run(GroupInfo::memberParams(QStringLiteral("removeMember"), {id}), tr("Could not remove the member: %1"));
    });
    memberAction(m_adminButton, [this](const QString &id, int, bool admin) {
        run(GroupInfo::memberParams(admin ? QStringLiteral("removeAdmin") : QStringLiteral("admin"), {id}),
            tr("Could not change admin rights: %1"));
    });
    memberAction(m_banButton, [this](const QString &id, int, bool) {
        if (QMessageBox::question(this, tr("Ban"),
                                  tr("Remove %1 from the group and ban them from rejoining?").arg(labelFor(id)))
            != QMessageBox::Yes)
            return;
        run(GroupInfo::memberParams(QStringLiteral("ban"), {id}), tr("Could not ban the member: %1"));
    });
    memberAction(m_approveButton, [this](const QString &id, int, bool) {
        run(GroupInfo::memberParams(QStringLiteral("member"), {id}), tr("Could not approve the request: %1"));
    });
    memberAction(m_denyButton, [this](const QString &id, int, bool) {
        run(GroupInfo::memberParams(QStringLiteral("removeMember"), {id}), tr("Could not deny the request: %1"));
    });
    memberAction(m_unbanButton, [this](const QString &id, int, bool) {
        run(GroupInfo::memberParams(QStringLiteral("unban"), {id}), tr("Could not unban: %1"));
    });

    setBusy(true);
    setStatus(tr("Loading …"), false);
}

void GroupDialog::reload()
{
    QPointer<GroupDialog> self(this);
    m_cli.call(QStringLiteral("listGroups"), {{QStringLiteral("groupId"), QJsonArray{m_groupId}}},
               [self](const QJsonValue &res, const QString &err) {
        if (!self) return;
        if (!err.isEmpty()) {
            self->setBusy(false);
            self->setStatus(tr("Could not load the group: %1").arg(err), true);
            return;
        }
        const auto info = GroupInfo::find(res, self->m_groupId);
        if (!info) {
            self->setBusy(false);
            self->setStatus(tr("signal-cli does not know this group."), true);
            return;
        }
        self->m_info = *info;
        self->m_loaded = true;
        self->showInfo();
        self->setBusy(false);
    });
}

void GroupDialog::showInfo()
{
    m_showing = true;
    setWindowTitle(tr("Group settings – %1").arg(m_info.name));
    m_name->setText(m_info.name);
    m_description->setPlainText(m_info.description);
    selectData(m_permAdd, m_info.permAddMember);
    selectData(m_permEdit, m_info.permEditDetails);
    selectData(m_permSend, m_info.permSendMessage);
    m_link->setCurrentIndex(m_info.inviteLink.isEmpty() ? 0 : 1);
    m_linkTouched = false;
    m_linkUrl->setText(m_info.inviteLink.isEmpty() ? tr("(no link)") : m_info.inviteLink);
    rebuildMembers();
    m_showing = false;

    const bool member = m_info.isMember && m_info.member(m_account);
    const bool admin = m_info.isAdmin(m_account);
    const bool edit = member && m_info.canEditDetails(m_account);
    m_name->setEnabled(edit);
    m_description->setEnabled(edit);
    m_avatarButton->setEnabled(edit);
    for (QWidget *w : {static_cast<QWidget *>(m_permAdd), static_cast<QWidget *>(m_permEdit),
                       static_cast<QWidget *>(m_permSend), static_cast<QWidget *>(m_link),
                       static_cast<QWidget *>(m_resetLink)})
        w->setEnabled(admin);
    m_copyLink->setEnabled(!m_info.inviteLink.isEmpty());
    m_resetLink->setEnabled(admin && !m_info.inviteLink.isEmpty());
    if (!m_info.isMember)
        setStatus(tr("You are not a member of this group."), false);
    else if (m_status->text() == tr("Loading …"))
        setStatus(QString(), false);
    updateMemberButtons();
    updateSaveState();
}

void GroupDialog::rebuildMembers()
{
    const QString keepId = m_members->currentItem() ? m_members->currentItem()->data(kIdRole).toString() : QString();
    m_members->clear();
    auto add = [&](const GroupInfo::Member &m, Category category) {
        QString text = labelFor(m.id());
        if (category == CatMember && m.admin) text += tr("  — admin");
        if (category == CatPending) text += tr("  — invited");
        if (category == CatRequesting) text += tr("  — wants to join");
        if (category == CatBanned) text += tr("  — banned");
        auto *item = new QListWidgetItem(text, m_members);
        item->setData(kIdRole, m.id());
        item->setData(kCategoryRole, int(category));
        item->setData(kAdminRole, m.admin);
        if (!keepId.isEmpty() && m.id() == keepId) m_members->setCurrentItem(item);
    };
    auto sorted = [this](QList<GroupInfo::Member> list) {
        std::sort(list.begin(), list.end(), [this](const GroupInfo::Member &a, const GroupInfo::Member &b) {
            return labelFor(a.id()).localeAwareCompare(labelFor(b.id())) < 0;
        });
        return list;
    };
    for (const auto &m : sorted(m_info.members)) add(m, CatMember);
    for (const auto &m : sorted(m_info.pending)) add(m, CatPending);
    for (const auto &m : sorted(m_info.requesting)) add(m, CatRequesting);
    for (const auto &m : sorted(m_info.banned)) add(m, CatBanned);
}

void GroupDialog::updateMemberButtons()
{
    const QListWidgetItem *item = m_members->currentItem();
    const int category = item ? item->data(kCategoryRole).toInt() : -1;
    const QString id = item ? item->data(kIdRole).toString() : QString();
    const bool admin = item && item->data(kAdminRole).toBool();
    const bool member = m_loaded && m_info.isMember && m_info.member(m_account);
    const bool iAmAdmin = member && m_info.isAdmin(m_account);
    const bool self = item && isSelf(id);

    m_addButton->setEnabled(m_loaded && member && m_info.canAddMembers(m_account));
    m_removeButton->setVisible(item && (category == CatMember || category == CatPending) && iAmAdmin && !self);
    m_adminButton->setVisible(item && category == CatMember && iAmAdmin && !self);
    m_adminButton->setText(admin ? tr("Remove admin") : tr("Make admin"));
    m_banButton->setVisible(item && category == CatMember && iAmAdmin && !self);
    m_approveButton->setVisible(item && category == CatRequesting && iAmAdmin);
    m_denyButton->setVisible(item && category == CatRequesting && iAmAdmin);
    m_unbanButton->setVisible(item && category == CatBanned && iAmAdmin);
}

GroupInfo::Edit GroupDialog::currentEdit() const
{
    GroupInfo::Edit edit;
    edit.name = m_name->text().trimmed();
    edit.description = m_description->toPlainText().trimmed();
    edit.avatarPath = m_avatarPath;
    edit.permAddMember = m_permAdd->currentData().toString();
    edit.permEditDetails = m_permEdit->currentData().toString();
    edit.permSendMessage = m_permSend->currentData().toString();
    if (m_linkTouched) edit.linkState = m_link->currentData().toString();
    return edit;
}

void GroupDialog::updateSaveState()
{
    if (!m_save) return;
    bool can = false;
    if (m_loaded && !m_busy && m_info.isMember) {
        GroupInfo::Edit edit = currentEdit();
        // Only fields the user may change count (a disabled field cannot differ, but be safe).
        if (!m_name->isEnabled()) { edit.name.reset(); edit.description.reset(); edit.avatarPath.clear(); }
        if (!m_permAdd->isEnabled()) { edit.permAddMember.reset(); edit.permEditDetails.reset(); edit.permSendMessage.reset(); edit.linkState.reset(); }
        if (edit.name && edit.name->isEmpty()) edit.name.reset(); // a group needs a name
        can = !GroupInfo::changes(m_info, edit).isEmpty();
    }
    m_save->setEnabled(can);
}

void GroupDialog::run(const QJsonObject &extra, const QString &failure, std::function<void()> then)
{
    if (m_busy) return;
    QJsonObject params = extra;
    params[QStringLiteral("groupId")] = m_groupId;
    setBusy(true);
    setStatus(tr("Saving …"), false);
    QPointer<GroupDialog> self(this);
    m_cli.call(QStringLiteral("updateGroup"), params, [self, failure, then](const QJsonValue &, const QString &err) {
        if (!self) return;
        if (!err.isEmpty()) {
            self->setBusy(false);
            self->setStatus(failure.arg(err), true);
            return;
        }
        self->setStatus(QString(), false);
        if (then) then();
        emit self->groupChanged(self->m_groupId);
        self->reload();
    });
}

void GroupDialog::addMembers()
{
    QStringList exclude{m_account};
    for (const auto &list : {m_info.members, m_info.pending, m_info.banned})
        for (const GroupInfo::Member &m : list) {
            exclude << m.number << m.uuid;
        }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add members"));
    dialog.setMinimumSize(420, 420);
    auto *lay = new QVBoxLayout(&dialog);
    auto *picker = new MemberPicker(m_contacts, exclude, &dialog);
    lay->addWidget(picker, 1);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Ok)->setText(tr("Add"));
    lay->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const QStringList ids = picker->selected();
    if (ids.isEmpty()) return;
    run(GroupInfo::memberParams(QStringLiteral("member"), ids), tr("Could not add the members: %1"));
}

void GroupDialog::leave()
{
    if (!m_loaded || m_busy || !m_info.isMember) return;
    if (QMessageBox::question(this, tr("Leave group"), tr("Leave “%1”? You will stop receiving its messages.").arg(m_info.name))
        != QMessageBox::Yes)
        return;

    QJsonObject params{{QStringLiteral("groupId"), m_groupId}};
    // signal-cli refuses to let the only admin go without naming a successor.
    if (m_info.isAdmin(m_account) && m_info.adminCount() == 1 && m_info.members.size() > 1) {
        QStringList labels;
        QStringList ids;
        for (const GroupInfo::Member &m : m_info.members) {
            if (isSelf(m.id())) continue;
            labels << labelFor(m.id());
            ids << m.id();
        }
        bool ok = false;
        const QString chosen = QInputDialog::getItem(this, tr("Choose a new admin"),
                                                      tr("You are the only admin. Who should take over?"), labels, 0,
                                                      false, &ok);
        if (!ok) return;
        params[QStringLiteral("admin")] = QJsonArray{ids.value(labels.indexOf(chosen))};
    }

    setBusy(true);
    setStatus(tr("Leaving …"), false);
    QPointer<GroupDialog> self(this);
    m_cli.call(QStringLiteral("quitGroup"), params, [self](const QJsonValue &, const QString &err) {
        if (!self) return;
        if (!err.isEmpty()) {
            self->setBusy(false);
            self->setStatus(tr("Could not leave the group: %1").arg(err), true);
            return;
        }
        emit self->groupLeft(self->m_groupId);
        self->accept();
    });
}
