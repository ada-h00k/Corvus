// Group management: signal-cli's group JSON, the parameters we send, and the dialog against the mock.
#include "GroupDialog.h"
#include "GroupInfo.h"
#include "SignalCli.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {

QJsonObject member(const QString &number, const QString &uuid, bool admin = false)
{
    return {{"number", number}, {"uuid", uuid}, {"isAdmin", admin}};
}

QJsonObject sampleGroup()
{
    return {{"id", "R1JPVVA="},
            {"name", "Team"},
            {"description", "the team"},
            {"isMember", true},
            {"messageExpirationTime", 3600},
            {"members", QJsonArray{member("+49000", "self-uuid", true), member("+49111", "u-alice"),
                                   member(QString(), "u-nonumber")}},
            {"pendingMembers", QJsonArray{QJsonObject{{"number", "+49444"}, {"uuid", "u-4"}}}},
            {"requestingMembers", QJsonArray{QJsonObject{{"number", "+49333"}, {"uuid", "u-carol"}}}},
            {"banned", QJsonArray{QJsonObject{{"number", "+49666"}, {"uuid", "u-6"}}}},
            {"permissionAddMember", "ONLY_ADMINS"},
            {"permissionEditDetails", "EVERY_MEMBER"},
            {"permissionSendMessage", "EVERY_MEMBER"},
            {"groupInviteLink", "https://signal.group/#abc"}};
}

// Answers the modal dialogs the group dialog opens (confirmations, successor choice, member picker).
QString g_addMember;
void answerModals()
{
    QWidget *w = QApplication::activeModalWidget();
    if (!w) return;
    if (auto *box = qobject_cast<QMessageBox *>(w)) {
        if (QAbstractButton *yes = box->button(QMessageBox::Yes)) yes->click();
        else box->accept();
    } else if (auto *input = qobject_cast<QInputDialog *>(w)) {
        input->accept();
    } else if (auto *dialog = qobject_cast<QDialog *>(w)) {
        if (dialog->windowTitle() == QStringLiteral("Add members")) {
            if (auto *manual = dialog->findChild<QLineEdit *>("memberPickerManual")) manual->setText(g_addMember);
            if (auto *add = dialog->findChild<QPushButton *>("memberPickerAdd")) add->click();
            dialog->accept();
        }
    }
}

bool selectMember(QListWidget *list, const QString &text)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text().contains(text)) {
            list->setCurrentRow(i);
            return true;
        }
    return false;
}

bool listHas(QListWidget *list, const QString &text)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text().contains(text)) return true;
    return false;
}

QString nameOf(const QString &id)
{
    if (id == "+49111") return QStringLiteral("Alice");
    if (id == "+49222") return QStringLiteral("Bob");
    if (id == "+49333") return QStringLiteral("Carol");
    return id;
}

} // namespace

class GroupManagementTest : public QObject
{
    Q_OBJECT
    SignalCli *m_cli = nullptr;
    QTimer m_modals;
    QList<GroupDialog::Contact> m_contacts{{"+49111", "Alice"}, {"+49222", "Bob"}, {"+49333", "Carol"}};

    // tab: 0 details, 1 members, 2 permissions and link. Widgets of a hidden tab are never "visible".
    GroupDialog *openSettings(const QString &groupId, int tab = 0)
    {
        auto *d = new GroupDialog(*m_cli, QStringLiteral("+49000"), groupId, m_contacts, nameOf);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->show();
        if (auto *tabs = d->findChild<QTabWidget *>()) tabs->setCurrentIndex(tab);
        return d;
    }
    template <typename T> T *child(QWidget *w, const char *name) { return w->findChild<T *>(QLatin1String(name)); }

private slots:
    void initTestCase()
    {
        qputenv("MOCK_EXTRA_GROUPS", "1");
        m_cli = new SignalCli;
        m_cli->setExecutable(QStringLiteral(MOCK_CLI));
        QSignalSpy started(m_cli, &SignalCli::started);
        m_cli->start(QStringLiteral("+49000"));
        QTRY_VERIFY_WITH_TIMEOUT(started.count() == 1, 10000);
        connect(&m_modals, &QTimer::timeout, answerModals);
        m_modals.start(20);
    }
    void cleanupTestCase() { delete m_cli; }

    // ---------------------------------------------------------------- data

    void parsesGroupJson()
    {
        const auto list = GroupInfo::parse(QJsonArray{sampleGroup(), QJsonObject()});
        QCOMPARE(list.size(), 1); // the entry without an id is dropped
        const GroupInfo::Info &g = list.first();
        QCOMPARE(g.name, QStringLiteral("Team"));
        QCOMPARE(g.expiration, 3600);
        QCOMPARE(g.members.size(), 3);
        QVERIFY(g.isAdmin("+49000"));
        QVERIFY(g.isAdmin("self-uuid"));       // by UUID too
        QVERIFY(!g.isAdmin("+49111"));
        QCOMPARE(g.members.at(2).id(), QStringLiteral("u-nonumber")); // no number: the UUID identifies
        QCOMPARE(g.pending.first().id(), QStringLiteral("+49444"));
        QCOMPARE(g.requesting.first().id(), QStringLiteral("+49333"));
        QCOMPARE(g.banned.first().id(), QStringLiteral("+49666"));
        QCOMPARE(g.inviteLink, QStringLiteral("https://signal.group/#abc"));
        QVERIFY(!g.canAddMembers("+49111"));   // only admins may add
        QVERIFY(g.canAddMembers("+49000"));
        QVERIFY(g.canEditDetails("+49111"));   // everyone may edit
        QVERIFY(!g.canEditDetails("+49999"));  // not a member
        QCOMPARE(g.adminCount(), 1);
        QVERIFY(GroupInfo::find(QJsonArray{sampleGroup()}, "R1JPVVA=").has_value());
        QVERIFY(!GroupInfo::find(QJsonArray{sampleGroup()}, "other").has_value());
    }

    void oldSignalCliListsAdminsSeparately()
    {
        QJsonObject g = sampleGroup();
        QJsonArray plain;
        for (const QJsonValue &m : g["members"].toArray())
            plain << QJsonObject{{"number", m.toObject()["number"]}, {"uuid", m.toObject()["uuid"]}};
        g["members"] = plain;
        g["admins"] = QJsonArray{QJsonObject{{"number", "+49111"}, {"uuid", "u-alice"}}};
        const auto info = GroupInfo::parseOne(g);
        QVERIFY(info.isAdmin("+49111"));
        QVERIFY(!info.isAdmin("+49000"));
    }

    void buildsParameters()
    {
        const GroupInfo::Info g = GroupInfo::parseOne(sampleGroup());
        GroupInfo::Edit edit;
        QVERIFY(GroupInfo::changes(g, edit).isEmpty());             // nothing set
        edit.name = "Team";
        edit.description = "the team";
        edit.permAddMember = "ONLY_ADMINS";
        QVERIFY(GroupInfo::changes(g, edit).isEmpty());             // same values: nothing to send

        edit.name = "Crew";
        edit.permSendMessage = "ONLY_ADMINS";
        edit.permAddMember = "EVERY_MEMBER";
        edit.linkState = "enabled-with-approval";
        edit.resetLink = true;
        edit.avatarPath = "/tmp/a.png";
        const QJsonObject p = GroupInfo::changes(g, edit);
        QCOMPARE(p["name"].toString(), QStringLiteral("Crew"));
        QVERIFY(!p.contains("description"));
        QCOMPARE(p["setPermissionSendMessages"].toString(), QStringLiteral("only-admins"));
        QCOMPARE(p["setPermissionAddMember"].toString(), QStringLiteral("every-member"));
        QVERIFY(!p.contains("setPermissionEditDetails"));
        QCOMPARE(p["link"].toString(), QStringLiteral("enabled-with-approval"));
        QVERIFY(p["resetLink"].toBool());
        QCOMPARE(p["avatar"].toString(), QStringLiteral("/tmp/a.png"));

        const QJsonObject c = GroupInfo::createParams("New", QString(), {"+49111", "+49222"}, QString());
        QCOMPARE(c["name"].toString(), QStringLiteral("New"));
        QCOMPARE(c["member"].toArray().size(), 2);
        QVERIFY(!c.contains("description"));
        QVERIFY(!c.contains("avatar"));
        QVERIFY(!c.contains("groupId")); // no group id: signal-cli creates a new group
        QCOMPARE(GroupInfo::memberParams("ban", {"+49111"})["ban"].toArray().first().toString(), QStringLiteral("+49111"));
    }

    // -------------------------------------------------------------- dialog

    void adminManagesMembers()
    {
        GroupDialog *d = openSettings("R1JPVVA=", 1);
        auto *members = child<QListWidget>(d, "groupMembers");
        QVERIFY(members);
        QTRY_VERIFY_WITH_TIMEOUT(listHas(members, "Alice"), 10000);
        QVERIFY(listHas(members, "You"));
        QVERIFY(listHas(members, "Bob") && members->item(0) != nullptr);
        QVERIFY(listHas(members, "invited"));
        QVERIFY(listHas(members, "wants to join"));

        // make Alice an admin
        QVERIFY(selectMember(members, "Alice"));
        auto *toggle = child<QPushButton>(d, "toggleAdminButton");
        QTRY_VERIFY(toggle->isVisible());
        QCOMPARE(toggle->text(), QStringLiteral("Make admin"));
        toggle->click();
        QTRY_VERIFY_WITH_TIMEOUT(selectMember(members, "Alice") && members->currentItem()->text().contains("admin"), 10000);
        QTRY_COMPARE(toggle->text(), QStringLiteral("Remove admin"));

        // approve Carol's join request
        QVERIFY(selectMember(members, "wants to join"));
        auto *approve = child<QPushButton>(d, "approveRequestButton");
        QTRY_VERIFY(approve->isVisible());
        QVERIFY(!child<QPushButton>(d, "removeMemberButton")->isVisible());
        approve->click();
        QTRY_VERIFY_WITH_TIMEOUT(!listHas(members, "wants to join") && listHas(members, "Carol"), 10000);

        // withdraw the invitation of +49444
        QVERIFY(selectMember(members, "invited"));
        auto *remove = child<QPushButton>(d, "removeMemberButton");
        QTRY_VERIFY(remove->isVisible());
        remove->click(); // confirmed by answerModals()
        QTRY_VERIFY_WITH_TIMEOUT(!listHas(members, "invited"), 10000);

        // ban Bob: removed from the members and listed as banned, then unbanned
        QVERIFY(selectMember(members, "Bob"));
        child<QPushButton>(d, "banMemberButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(listHas(members, "banned"), 10000);
        QVERIFY(selectMember(members, "banned"));
        auto *unban = child<QPushButton>(d, "unbanMemberButton");
        QTRY_VERIFY(unban->isVisible());
        unban->click();
        QTRY_VERIFY_WITH_TIMEOUT(!listHas(members, "banned"), 10000);

        // yourself cannot be removed or demoted
        QVERIFY(selectMember(members, "You"));
        QVERIFY(!child<QPushButton>(d, "removeMemberButton")->isVisible());
        QVERIFY(!toggle->isVisible());

        // add a member by number
        g_addMember = QStringLiteral("+49555");
        auto *add = child<QPushButton>(d, "addMembersButton");
        QVERIFY(add->isEnabled());
        add->click();
        QTRY_VERIFY_WITH_TIMEOUT(listHas(members, "+49555"), 10000);
        d->close();
    }

    void saveChangesNameAndPermissions()
    {
        GroupDialog *d = openSettings("R1JPVVA=");
        auto *name = child<QLineEdit>(d, "groupName");
        auto *save = child<QPushButton>(d, "saveGroupButton");
        QTRY_COMPARE_WITH_TIMEOUT(name->text(), QStringLiteral("Team"), 10000);
        QTRY_VERIFY(!save->isEnabled()); // nothing changed yet

        name->setText("Crew");
        child<QComboBox>(d, "permSendMessage")->setCurrentIndex(1); // only admins may send
        child<QComboBox>(d, "linkState")->setCurrentIndex(1);       // link on
        QTRY_VERIFY(save->isEnabled());
        save->click();

        auto *link = child<QLabel>(d, "inviteLink");
        QTRY_VERIFY_WITH_TIMEOUT(link->text().contains("MOCK-LINK"), 10000);
        QTRY_VERIFY(!save->isEnabled());
        QCOMPARE(child<QComboBox>(d, "permSendMessage")->currentData().toString(), QStringLiteral("ONLY_ADMINS"));
        QVERIFY(d->windowTitle().contains("Crew"));

        child<QPushButton>(d, "resetLinkButton")->click(); // confirmed by answerModals()
        QTRY_VERIFY_WITH_TIMEOUT(link->text().contains("MOCK-LINK-2"), 10000);

        name->setText("   "); // an empty name is not a change
        QTRY_VERIFY(!save->isEnabled());
        d->close();
    }

    void plainMemberCannotChangeAnything()
    {
        GroupDialog *d = openSettings("Q0xVQg==", 1);
        auto *members = child<QListWidget>(d, "groupMembers");
        QTRY_VERIFY_WITH_TIMEOUT(listHas(members, "Alice"), 10000);
        QVERIFY(!child<QLineEdit>(d, "groupName")->isEnabled());           // only admins edit details
        QVERIFY(!child<QPlainTextEdit>(d, "groupDescription")->isEnabled());
        QVERIFY(!child<QComboBox>(d, "permAddMember")->isEnabled());
        QVERIFY(!child<QComboBox>(d, "linkState")->isEnabled());
        QVERIFY(!child<QPushButton>(d, "addMembersButton")->isEnabled());   // only admins add
        QVERIFY(selectMember(members, "Alice"));
        QVERIFY(!child<QPushButton>(d, "removeMemberButton")->isVisible());
        QVERIFY(!child<QPushButton>(d, "toggleAdminButton")->isVisible());
        QVERIFY(!child<QPushButton>(d, "banMemberButton")->isVisible());
        QVERIFY(!child<QPushButton>(d, "saveGroupButton")->isEnabled());
        QVERIFY(child<QPushButton>(d, "leaveGroupButton")->isEnabled());    // but anyone may leave
        d->close();
    }

    void createsGroupAndSoleAdminNamesSuccessor()
    {
        auto *create = new GroupDialog(*m_cli, QStringLiteral("+49000"), QString(), m_contacts, nameOf);
        create->setAttribute(Qt::WA_DeleteOnClose);
        create->show();
        auto *button = child<QPushButton>(create, "createGroupButton");
        QVERIFY(!button->isEnabled()); // a name is required
        child<QLineEdit>(create, "groupName")->setText("Weekend");
        QVERIFY(button->isEnabled());
        auto *list = child<QListWidget>(create, "memberPickerList");
        QVERIFY(list && list->count() == 3);
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text().contains("Alice") || list->item(i)->text().contains("Bob"))
                list->item(i)->setCheckState(Qt::Checked);
        QSignalSpy created(create, &GroupDialog::groupCreated);
        button->click();
        QTRY_COMPARE_WITH_TIMEOUT(created.count(), 1, 10000);
        const QString id = created.first().first().toString();
        QVERIFY(!id.isEmpty());

        // we are the only admin of the new group: leaving needs a successor (the first one is chosen)
        GroupDialog *d = openSettings(id, 1);
        auto *members = child<QListWidget>(d, "groupMembers");
        QTRY_VERIFY_WITH_TIMEOUT(listHas(members, "Alice") && listHas(members, "Bob"), 10000);
        QSignalSpy left(d, &GroupDialog::groupLeft);
        child<QPushButton>(d, "leaveGroupButton")->click();
        QTRY_COMPARE_WITH_TIMEOUT(left.count(), 1, 10000);
    }

    void failuresAreShown()
    {
        // a group signal-cli does not know
        GroupDialog *d = openSettings("bm9wZQ==");
        QTRY_VERIFY_WITH_TIMEOUT(!d->findChildren<QLabel *>().isEmpty(), 10000);
        auto statusText = [d] {
            QString t;
            for (QLabel *l : d->findChildren<QLabel *>()) t += l->text() + "\n";
            return t;
        };
        QTRY_VERIFY_WITH_TIMEOUT(statusText().contains("does not know"), 10000);
        QVERIFY(!child<QPushButton>(d, "saveGroupButton")->isEnabled());
        d->close();
    }
};

QTEST_MAIN(GroupManagementTest)
#include "group_management_test.moc"
