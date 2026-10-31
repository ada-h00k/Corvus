// End-to-end: group management in the real MainWindow (menu actions, dialogs, chat list) against the mock.
#include "GroupDialog.h"
#include "GroupInvitesDialog.h"
#include "MainWindow.h"
#include "PassphraseDialog.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

static QString g_inputText;
static QString g_lastBox;

static bool waitFor(const std::function<bool()> &cond, int ms = 15000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

static void autoDialogs()
{
    QWidget *w = QApplication::activeModalWidget();
    if (!w) return;
    if (auto *pd = qobject_cast<PassphraseDialog *>(w)) {
        for (QLineEdit *e : pd->findChildren<QLineEdit *>()) e->setText(QStringLiteral("test-passphrase-1"));
        pd->accept();
    } else if (auto *mb = qobject_cast<QMessageBox *>(w)) {
        if (mb->windowTitle() == QStringLiteral("Protect chat history")) {
            for (QAbstractButton *button : mb->buttons())
                if (button->text() == QStringLiteral("Use a passphrase")) button->click();
            return;
        }
        g_lastBox = mb->text();
        if (QAbstractButton *yes = mb->button(QMessageBox::Yes)) yes->click();
        else mb->accept();
    } else if (auto *input = qobject_cast<QInputDialog *>(w)) {
        input->setTextValue(g_inputText);
        input->accept();
    }
}

static bool listHas(QListWidget *l, const QString &needle)
{
    for (int i = 0; i < l->count(); ++i)
        if (l->item(i)->text().contains(needle)) return true;
    return false;
}

static void selectItem(QListWidget *l, const QString &needle)
{
    for (int i = 0; i < l->count(); ++i)
        if (l->item(i)->text().contains(needle)) {
            l->setCurrentRow(i);
            return;
        }
}

static QAction *action(MainWindow &w, const QString &text)
{
    for (QAction *a : w.menuBar()->findChildren<QAction *>())
        if (a->text() == text) return a;
    return nullptr;
}

static GroupDialog *dialogWith(MainWindow &w, const char *childName)
{
    for (GroupDialog *d : w.findChildren<GroupDialog *>())
        if (d->isVisible() && d->findChild<QWidget *>(QLatin1String(childName))) return d;
    return nullptr;
}

static bool logHas(const QString &file, const QString &needle)
{
    QFile f(file);
    return f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()).contains(needle);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("corvus"));
    QApplication::setApplicationName(QStringLiteral("corvus"));

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp.path() + "/config");
    QSettings().setValue("executable", QStringLiteral(MOCK_CLI));
    QSettings().setValue("historyProtectionMode", QStringLiteral("passphrase"));
    const QString logFile = tmp.path() + "/rpc.log";
    qputenv("MOCK_LOG", logFile.toUtf8());
    qputenv("MOCK_EXTRA_GROUPS", "1");

    QTimer dialogTimer;
    QObject::connect(&dialogTimer, &QTimer::timeout, autoDialogs);
    dialogTimer.start(30);

    MainWindow w;
    w.show();
    auto *list = w.findChild<QListWidget *>("conversationList");
    CHECK(list);
    if (!list) return 1;
    CHECK(waitFor([&] { return listHas(list, "Team") && listHas(list, "Club"); }));

    QAction *settings = action(w, "Group settings …");
    QAction *newGroup = action(w, "New group …");
    QAction *join = action(w, "Join group via link …");
    CHECK(settings && newGroup && join);
    if (!settings || !newGroup || !join) return 1;

    std::printf("0: open invitations are listed apart from the chats\n");
    QAction *invites = action(w, "Group invitations …");
    CHECK(invites);
    if (!invites) return 1;
    CHECK(waitFor([&] { return listHas(list, "Group invitations (2)"); }));
    CHECK(!listHas(list, "Book club") && !listHas(list, "Hiking"));
    CHECK(invites->isEnabled());

    std::printf("1: group settings of the selected group, rename\n");
    selectItem(list, "Team");
    CHECK(waitFor([&] { return settings->isEnabled(); }));
    settings->trigger();
    GroupDialog *dialog = nullptr;
    CHECK(waitFor([&] { return (dialog = dialogWith(w, "groupName")) != nullptr; }));
    if (dialog) {
        auto *name = dialog->findChild<QLineEdit *>("groupName");
        CHECK(waitFor([&] { return name->text() == "Team"; }));
        name->setText("Team Renamed");
        auto *save = dialog->findChild<QPushButton *>("saveGroupButton");
        CHECK(waitFor([&] { return save->isEnabled(); }));
        save->click();
        CHECK(waitFor([&] { return listHas(list, "Team Renamed"); }));

        std::printf("2: leaving the group\n");
        dialog->findChild<QPushButton *>("leaveGroupButton")->click(); // confirmed automatically
        CHECK(waitFor([&] { return logHas(logFile, "quitGroup"); }));
        CHECK(waitFor([&] { return w.statusBar()->currentMessage().contains("You left"); }));
    }

    std::printf("3: new group with Alice -> listed and opened\n");
    newGroup->trigger();
    GroupDialog *create = nullptr;
    CHECK(waitFor([&] { return (create = dialogWith(w, "createGroupButton")) != nullptr; }));
    if (create) {
        create->findChild<QLineEdit *>("groupName")->setText("Weekend");
        // Alice is not a chat yet; she is added by number
        create->findChild<QLineEdit *>("memberPickerManual")->setText("+49111");
        create->findChild<QPushButton *>("memberPickerAdd")->click();
        create->findChild<QPushButton *>("createGroupButton")->click();
        CHECK(waitFor([&] { return listHas(list, "Weekend"); }));
        CHECK(waitFor([&] {
            for (QLabel *l : w.findChildren<QLabel *>())
                if (l->isVisible() && l->text() == "Weekend") return true;
            return false;
        }));
        CHECK(waitFor([&] { return settings->isEnabled(); })); // the new group is the current chat
    }

    std::printf("4: join by link\n");
    g_inputText = "https://signal.group/#abc";
    join->trigger();
    CHECK(waitFor([&] { return listHas(list, "Joined group"); }));

    g_inputText = "https://example.com/not-a-group";
    g_lastBox.clear();
    join->trigger();
    CHECK(waitFor([&] { return g_lastBox.contains("Group link is invalid"); }));

    g_inputText = "https://signal.group/#abcAPPROVAL"; // needs approval: only requested
    join->trigger();
    CHECK(waitFor([&] { return w.statusBar()->currentMessage().contains("Join request sent"); }));

    std::printf("5: accept one invitation, decline the other\n");
    invites->trigger();
    GroupInvitesDialog *inv = nullptr;
    CHECK(waitFor([&] {
        for (GroupInvitesDialog *d : w.findChildren<GroupInvitesDialog *>())
            if (d->isVisible()) inv = d;
        return inv != nullptr;
    }));
    if (inv) {
        auto *invList = inv->findChild<QListWidget *>("invitationList");
        CHECK(invList && invList->count() == 2);
        auto pick = [&](const QString &text) {
            for (int i = 0; i < invList->count(); ++i)
                if (invList->item(i)->text().contains(text)) invList->setCurrentRow(i);
        };
        pick("Book club");
        inv->findChild<QPushButton *>("acceptInviteButton")->click();
        CHECK(waitFor([&] { return listHas(list, "Book club") && listHas(list, "Group invitations (1)"); }));
        CHECK(invList->count() == 1);

        pick("Hiking");
        inv->findChild<QPushButton *>("declineInviteButton")->click(); // confirmed automatically
        CHECK(waitFor([&] { return !listHas(list, "Group invitations"); }));
        CHECK(!listHas(list, "Hiking"));
        CHECK(waitFor([&] { return !invites->isEnabled(); }));
        CHECK(invList->count() == 0);
        inv->close();
    }

    w.close();
    std::printf(g_failed ? "FAILED (%d)\n" : "OK\n", g_failed);
    return g_failed ? 1 : 0;
}
