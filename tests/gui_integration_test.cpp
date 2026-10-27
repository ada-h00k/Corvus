// Headless end-to-end test: real MainWindow + encrypted Rust store + mock signal-cli.
#include "ApngMovie.h"
#include "ChatView.h"
#include "EmojiPicker.h"
#include "GiphyPicker.h"
#include "HistoryKeychain.h"
#include "ImageViewer.h"
#include "MainWindow.h"
#include "MessageRequestsDialog.h"
#include "PassphraseDialog.h"
#include "ProfilePage.h"
#include "SettingsDialog.h"

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QBuffer>
#include <QInputDialog>
#include <QDateTime>
#include <QtTest/QTest>
#include <QDir>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QThread>
#include <QDropEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMovie>
#include <QMediaPlayer>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <cstdio>
#include <cstdlib>

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

static const char *kPass = "test-passphrase-1";
static QString g_unexpectedBox;
static QString g_deleteChoice;

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

// Answers the passphrase dialog; records any unexpected message box.
static void autoDialogs()
{
    QWidget *w = QApplication::activeModalWidget();
    if (!w) return;
    if (auto *pd = qobject_cast<PassphraseDialog *>(w)) {
        for (QLineEdit *e : pd->findChildren<QLineEdit *>()) e->setText(QString::fromLatin1(kPass));
        pd->accept();
    } else if (auto *mb = qobject_cast<QMessageBox *>(w)) {
        if (mb->windowTitle() == QStringLiteral("Protect chat history")) {
            QPushButton *keychain = nullptr;
            QPushButton *passphrase = nullptr;
            for (QAbstractButton *button : mb->buttons()) {
                if (button->text().contains(QStringLiteral("System keychain")))
                    keychain = qobject_cast<QPushButton *>(button);
                if (button->text() == QStringLiteral("Use a passphrase"))
                    passphrase = qobject_cast<QPushButton *>(button);
            }
            CHECK(passphrase != nullptr);
            if (keychain) CHECK(mb->defaultButton() == keychain);
            if (passphrase) passphrase->click();
            return;
        }
        if (mb->windowTitle() == QStringLiteral("Delete message") && !g_deleteChoice.isEmpty()) {
            for (QAbstractButton *button : mb->buttons()) {
                if (button->text() == g_deleteChoice) {
                    g_deleteChoice.clear();
                    button->click();
                    return;
                }
            }
        }
        g_unexpectedBox = mb->text();
        std::printf("  [unexpected message box] %s\n", qPrintable(mb->text()));
        std::fflush(stdout);
        mb->reject();
    }
}

static QStringList listTexts(QListWidget *l)
{
    QStringList out;
    for (int i = 0; i < l->count(); ++i) out << l->item(i)->text();
    return out;
}

static bool listHas(QListWidget *l, const QString &needle)
{
    for (const QString &t : listTexts(l))
        if (t.contains(needle)) return true;
    return false;
}

// Like listHas, but only counts rows that are actually visible (the contact search hides rows).
static bool listVisibleHas(QListWidget *l, const QString &needle)
{
    for (int i = 0; i < l->count(); ++i) {
        QListWidgetItem *it = l->item(i);
        if (!it->isHidden() && it->text().contains(needle)) return true;
    }
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

static bool chooseChatExpiration(MainWindow &window, QListWidget *list, const QString &needle, int choice)
{
    QListWidgetItem *item = nullptr;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->text().contains(needle)) {
            item = list->item(i);
            break;
        }
    }
    if (!item) return false;

    const QPoint pos = list->visualItemRect(item).center();
    QTimer::singleShot(0, &window, [choice] {
        for (QWidget *top : QApplication::topLevelWidgets()) {
            auto *menu = qobject_cast<QMenu *>(top);
            if (!menu) continue;
            for (QAction *action : menu->actions()) {
                if (!action->text().contains("Disappearing messages")) continue;
                QTimer::singleShot(100, [choice] {
                    auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                    if (!dialog) return;
                    auto *combo = dialog->findChild<QComboBox *>();
                    if (!combo || choice < 0 || choice >= combo->count()) {
                        dialog->reject();
                        return;
                    }
                    combo->setCurrentIndex(choice);
                    dialog->accept();
                });
                action->trigger();
                return;
            }
        }
    });
    QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, pos));
    return waitFor([] { return QApplication::activeModalWidget() == nullptr; }, 3000);
}

static bool chooseChatNickname(MainWindow &window, QListWidget *list, const QString &needle, const QString &nickname)
{
    QListWidgetItem *item = nullptr;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->text().contains(needle)) {
            item = list->item(i);
            break;
        }
    }
    if (!item) return false;

    const QPoint pos = list->visualItemRect(item).center();
    bool answered = false;
    QTimer::singleShot(0, &window, [nickname, &answered] {
        for (QWidget *top : QApplication::topLevelWidgets()) {
            auto *menu = qobject_cast<QMenu *>(top);
            if (!menu) continue;
            for (QAction *action : menu->actions()) {
                if (!action->text().contains("nickname")) continue;
                QTimer::singleShot(100, [nickname, &answered] {
                    auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                    if (!dialog) return;
                    auto *edit = dialog->findChild<QLineEdit *>();
                    if (!edit) {
                        dialog->reject();
                        answered = true;
                        return;
                    }
                    edit->setText(nickname);
                    answered = true;
                    dialog->accept();
                });
                action->trigger();
                return;
            }
        }
    });
    QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, pos));
    return waitFor([&] { return answered && QApplication::activeModalWidget() == nullptr; }, 3000);
}

// Opens the conversation list context menu on the row matching `needle` and
// triggers the first action whose text contains `actionText` (e.g. "Archive").
// With `confirmText` set, the same text is clicked away in the confirmation box
// the action opens (Block).
static bool chooseChatAction(MainWindow &window, QListWidget *list, const QString &needle, const QString &actionText,
                             const QString &confirmText = QString())
{
    QListWidgetItem *item = nullptr;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->text().contains(needle)) {
            item = list->item(i);
            break;
        }
    }
    if (!item) return false;

    const QPoint pos = list->visualItemRect(item).center();
    bool triggered = false;
    QTimer::singleShot(0, &window, [actionText, confirmText, &triggered] {
        QAction *action = nullptr;
        QList<QMenu *> menus;
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (auto *menu = qobject_cast<QMenu *>(top)) {
                menus.append(menu);
                for (QAction *candidate : menu->actions()) {
                    if (candidate->text().contains(actionText)) { action = candidate; break; }
                }
                if (action) break;
            }
        }
        if (action) {
            triggered = true;
            action->trigger();
            if (!confirmText.isEmpty()) {
                // the action opened a confirmation box: click the requested button
                QTimer::singleShot(100, [confirmText] {
                    for (QWidget *top : QApplication::topLevelWidgets()) {
                        if (auto *box = qobject_cast<QMessageBox *>(top)) {
                            for (QAbstractButton *button : box->buttons())
                                if (button->text().contains(confirmText)) { button->click(); return; }
                        }
                    }
                });
            }
        } else {
            for (QMenu *menu : menus) menu->close(); // nothing matched: don't leave it open
        }
    });
    QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, pos));
    return waitFor([&] { return triggered && QApplication::activeModalWidget() == nullptr; }, 3000);
}

static bool startConversation(MainWindow &window, const QString &recipient)
{
    QMenu *fileMenu = nullptr;
    for (QAction *menuAction : window.menuBar()->actions()) {
        if (menuAction->text().contains(QStringLiteral("File"))) {
            fileMenu = menuAction->menu();
            break;
        }
    }
    if (!fileMenu) return false;

    QAction *newConversationAction = nullptr;
    for (QAction *action : fileMenu->actions()) {
        if (action->text().contains(QStringLiteral("New conversation"))) {
            newConversationAction = action;
            break;
        }
    }
    if (!newConversationAction) return false;

    bool answered = false;
    QTimer::singleShot(100, &window, [&] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        CHECK(dialog->labelText().contains(QStringLiteral("Signal username")));
        auto *edit = dialog->findChild<QLineEdit *>();
        if (!edit) {
            dialog->reject();
            answered = true;
            return;
        }
        edit->setText(recipient);
        answered = true;
        dialog->accept();
    });
    newConversationAction->trigger();
    return waitFor([&] { return answered && QApplication::activeModalWidget() == nullptr; }, 3000);
}

static QList<QJsonObject> readLog(const QString &path)
{
    QList<QJsonObject> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    for (const QByteArray &l : f.readAll().split('\n'))
        if (!l.trimmed().isEmpty()) out << QJsonDocument::fromJson(l).object();
    return out;
}

static QJsonObject findCall(const QString &log, const QString &method, int nth = 0)
{
    int n = 0;
    for (const QJsonObject &o : readLog(log))
        if (o["method"].toString() == method && n++ == nth) return o;
    return {};
}

static QJsonObject lastCall(const QString &log, const QString &method)
{
    QJsonObject last;
    for (const QJsonObject &o : readLog(log))
        if (o["method"].toString() == method) last = o;
    return last;
}

// Settings → "Configure Corvus …" opens the settings window; the Profile page lives in it.
static QAction *settingsAction(MainWindow &window)
{
    for (QAction *menuAction : window.menuBar()->actions()) {
        if (!menuAction->text().contains(QStringLiteral("Settings"))) continue;
        for (QAction *action : menuAction->menu()->actions())
            if (action->text().contains(QStringLiteral("Configure Corvus"))) return action;
    }
    return nullptr;
}

// The settings dialog is modal (exec), so the test drives it from a timer that fires inside its event loop.
static SettingsDialog *findSettingsDialog()
{
    SettingsDialog *dialog = nullptr;
    waitFor([&] {
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *d = qobject_cast<SettingsDialog *>(w)) { dialog = d; return d->isVisible(); }
        return false;
    }, 3000);
    return dialog;
}

static bool loggedAttachmentFetch(const QString &log, const QString &attachmentId)
{
    for (const QJsonObject &request : readLog(log))
        if (request["method"].toString() == QStringLiteral("getAttachment") &&
            request["params"].toObject()["id"].toString() == attachmentId)
            return true;
    return false;
}

static bool changeSignalUsername(ProfilePage *dialog, const QString &username, const QString &logFile)
{
    auto *label = dialog->findChild<QLabel *>(QStringLiteral("currentUsernameLabel"));
    auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("usernameEdit"));
    auto *apply = dialog->findChild<QPushButton *>(QStringLiteral("usernameApply"));
    CHECK(label && edit && apply);
    if (!label || !edit || !apply) return false;
    const bool showedCurrent = waitFor([&] { return label->text().contains(QStringLiteral("oldname.1234")); }, 5000);
    CHECK(showedCurrent);
    edit->setText(username);
    apply->click();
    const bool updated = waitFor([&] {
        const QJsonObject call = lastCall(logFile, "updateAccount");
        return call["params"].toObject()["username"].toString() == username;
    }, 5000);
    CHECK(updated);
    return showedCurrent && updated;
}

static bool changeProfileDisplayName(ProfilePage *dialog, const QString &displayName, const QString &logFile)
{
    auto *edit = dialog->findChild<QLineEdit *>(QStringLiteral("displayNameEdit"));
    auto *apply = dialog->findChild<QPushButton *>(QStringLiteral("displayNameApply"));
    CHECK(edit && apply);
    if (!edit || !apply) return false;
    edit->setText(displayName);
    apply->click();
    const bool updated = waitFor([&] {
        const QJsonObject params = lastCall(logFile, "updateProfile")["params"].toObject();
        return params["givenName"].toString() == displayName && params["familyName"].toString().isEmpty();
    }, 5000);
    CHECK(updated);
    return updated;
}

static bool removeProfileAvatar(ProfilePage *dialog, const QString &logFile)
{
    auto *remove = dialog->findChild<QPushButton *>(QStringLiteral("avatarRemove"));
    CHECK(remove != nullptr);
    if (!remove) return false;
    remove->click();
    const bool removed = waitFor([&] {
        return lastCall(logFile, "updateProfile")["params"].toObject()["removeAvatar"].toBool();
    }, 5000);
    CHECK(removed);
    return removed;
}

static void inject(const QString &file, const QJsonObject &env)
{
    QFile f(file);
    f.open(QIODevice::Append);
    f.write(QJsonDocument(env).toJson(QJsonDocument::Compact) + "\n");
}

static QString html(ChatView *v) { return v->plainText(); }

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("corvus"));
    QApplication::setApplicationName(QStringLiteral("corvus"));

    const QString firstHistorySecret = HistoryKeychain::generateSecret();
    const QString secondHistorySecret = HistoryKeychain::generateSecret();
    CHECK(firstHistorySecret.size() == 64);
    CHECK(firstHistorySecret != secondHistorySecret);
    for (const QChar ch : firstHistorySecret) CHECK(ch.isDigit() || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')));

    QTemporaryDir tmp;
    CHECK(tmp.isValid());

    {
        QTcpServer server;
        CHECK(server.listen(QHostAddress::LocalHost));
        const QByteArray previewData = QByteArray::fromBase64(
            "R0lGODlhEAAQAPAAAP8AAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACwAAAAAEAAQAAACDoSP"
            "qcvtD6OctNqLsz4FACH5BAAKAAAALAAAAAAQABAAgAAA/wAAAAIOhI+py+0Po5y02ouzPgUAOw==");
        const QString base = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, base, previewData] {
            while (server.hasPendingConnections()) {
                QTcpSocket *socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, base, previewData] {
                    const QByteArray request = socket->readAll();
                    const QByteArray path = request.left(request.indexOf("\r\n")).split(' ').value(1);
                    QByteArray body;
                    QByteArray type = "application/json";
                    if (path.startsWith("/v1/gifs/trending")) {
                        if (path.contains("rating=")) {
                            body = QJsonDocument(QJsonObject{{"data", QJsonArray{}}}).toJson(QJsonDocument::Compact);
                            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                            socket->disconnectFromHost();
                            return;
                        }
                        const QJsonObject gif{
                            {"title", "local test gif"},
                            {"images", QJsonObject{
                                {"fixed_width_small", QJsonObject{{"url", base + "/preview.gif"}}},
                                {"original", QJsonObject{{"url", base + "/original.gif"}}}}}};
                        body = QJsonDocument(QJsonObject{{"data", QJsonArray{gif}}}).toJson(QJsonDocument::Compact);
                    } else if (path.startsWith("/preview.gif")) {
                        body = previewData;
                        type = "image/gif";
                    }
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });

        GiphyPicker picker(QStringLiteral("local-test-key"), nullptr, QUrl(base));
        QSignalSpy selected(&picker, &GiphyPicker::gifSelected);
        picker.show();
        CHECK(waitFor([&] {
            auto *button = picker.findChild<QPushButton *>(QStringLiteral("giphyResult0"));
            return button && button->isEnabled();
        }));
        auto *resultButton = picker.findChild<QPushButton *>(QStringLiteral("giphyResult0"));
        auto *movie = resultButton ? resultButton->findChild<QMovie *>() : nullptr;
        CHECK(movie && movie->isValid());
        CHECK(waitFor([&] { return movie && movie->state() == QMovie::Running; }));
        if (auto *button = picker.findChild<QPushButton *>(QStringLiteral("giphyResult0"))) button->click();
        CHECK(selected.size() == 1);
        if (!selected.isEmpty())
            CHECK(selected.first().at(0).toUrl() == QUrl(base + QStringLiteral("/original.gif")));
        picker.close();
    }

    {
        QImage source(2000, 1500, QImage::Format_RGB32);
        source.fill(Qt::darkCyan);
        {
            QPainter painter(&source);
            painter.fillRect(QRect(1400, 600, 400, 300), Qt::magenta);
        }
        QByteArray encoded;
        QBuffer buffer(&encoded);
        CHECK(buffer.open(QIODevice::WriteOnly));
        CHECK(source.save(&buffer, "PNG"));

        ImageViewer viewer(encoded, QStringLiteral("large.png"));
        CHECK(viewer.isValid());
        viewer.show();
        QCoreApplication::processEvents();
        auto *scroll = viewer.findChild<QScrollArea *>();
        auto *label = scroll ? qobject_cast<QLabel *>(scroll->widget()) : nullptr;
        CHECK(scroll && label);
        if (scroll && label) {
            auto magentaPixels = [label] {
                const QImage rendered = label->grab().toImage().convertToFormat(QImage::Format_RGB32);
                int count = 0;
                for (int y = 0; y < rendered.height(); ++y)
                    for (int x = 0; x < rendered.width(); ++x)
                        if (rendered.pixel(x, y) == qRgb(255, 0, 255)) ++count;
                return count;
            };
            const int initialDetailPixels = magentaPixels();
            CHECK(initialDetailPixels > 0);
            viewer.resize(320, 240);
            QCoreApplication::processEvents();
            const int smallDetailPixels = magentaPixels();
            CHECK(smallDetailPixels > 0 && smallDetailPixels < initialDetailPixels);
            viewer.resize(900, 700);
            QCoreApplication::processEvents();
            CHECK(magentaPixels() > smallDetailPixels);
        }
        viewer.close();
    }

    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, tmp.path() + "/config");
    QSettings().setValue("executable", QStringLiteral(MOCK_CLI));
    QSettings().setValue("historyProtectionMode", QStringLiteral("passphrase"));

    const QString logFile = tmp.path() + "/rpc.log";
    const QString injectFile = tmp.path() + "/inject.jsonl";
    qputenv("MOCK_LOG", logFile.toUtf8());
    qputenv("MOCK_INJECT", injectFile.toUtf8());

    QTimer dialogTimer;
    QObject::connect(&dialogTimer, &QTimer::timeout, autoDialogs);
    dialogTimer.start(30);

    // ---------------------------------------------------------------- run 1
    std::printf("run 1: first start, create encrypted history\n");
    {
        MainWindow w;
        w.show();
        auto *list = w.findChild<QListWidget *>();
        auto *view = w.findChild<ChatView *>();
        // The composer is a MessageInput (a QTextEdit subclass). Finding it by its type instead of
        // "the first QLineEdit" keeps the test independent of the other line edits of the window
        // (contact search, message search).
        auto *input = w.findChild<MessageInput *>();
        CHECK(list && view && input);

        // A light / dark switch while running re-colours the chat without a restart.
        if (view) {
            const QPalette before = QApplication::palette();
            QPalette flipped = before;
            const bool wasDark = before.color(QPalette::Window).lightness() < 128;
            flipped.setColor(QPalette::Window, wasDark ? QColor(0xef, 0xf0, 0xf1) : QColor(0x20, 0x23, 0x26));
            flipped.setColor(QPalette::Base, wasDark ? QColor(0xff, 0xff, 0xff) : QColor(0x14, 0x16, 0x18));
            QApplication::setPalette(flipped);
            CHECK(waitFor([&] { return view->appearance().background == flipped.color(QPalette::Base); }));
            CHECK(view->appearance().inBg == Appearance::defaults(flipped).inBg);
            QApplication::setPalette(before);
            CHECK(waitFor([&] { return view->appearance().background == before.color(QPalette::Base); }));
        }

        QToolButton *attachBtn = nullptr;
        QToolButton *emojiBtn = nullptr;
        for (QToolButton *button : w.findChildren<QToolButton *>()) {
            if (button->toolTip() == QStringLiteral("Attach files")) attachBtn = button;
            else if (button->toolTip() == QStringLiteral("Emoji and GIFs")) emojiBtn = button;
        }
        CHECK(attachBtn && emojiBtn);
        if (attachBtn && emojiBtn && input) {
            CHECK(attachBtn->size() == emojiBtn->size());
            CHECK(attachBtn->mapTo(&w, QPoint()).x() >= input->mapTo(&w, QPoint()).x() + input->width());
        }

        CHECK(waitFor([&] { return listHas(list, "Archived chats") && listHas(list, "Team"); }));
        CHECK(listHas(list, "Archived chats (2)")); // Bob + Carol archived on the "phone"
        CHECK(!listHas(list, "Alice")); // active contacts appear when a chat exists
        CHECK(!listHas(list, "Bob"));
        CHECK(!listHas(list, "Carol"));
        CHECK(listHas(list, "Team"));
        {
            // avatars: requested from signal-cli (contact first, group by id) and shown next to the names
            auto rpcLogHas = [&](const QString &needle) {
                QFile f(logFile);
                return f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()).contains(needle);
            };
            CHECK(waitFor([&] { return rpcLogHas("getAvatar"); }));
            QListWidgetItem *team = nullptr;
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->text().contains("Team")) team = list->item(i);
            CHECK(team && !team->icon().isNull());
        }
        {
            bool found = false;
            for (QLabel *l : w.findChildren<QLabel *>()) {
                if (l->text().contains("Connected")) {
                    found = true;
                    CHECK(!l->text().contains("+49"));      // no phone number in the status bar
                    CHECK(l->text().contains("0.13.99"));   // but the signal-cli version
                }
            }
            CHECK(found);
        }
        QAction *settingsAct = settingsAction(w);
        CHECK(settingsAct != nullptr);
        bool settingsDriven = false;
        QTimer::singleShot(300, &w, [&] {
            SettingsDialog *settingsDlg = findSettingsDialog();
            CHECK(settingsDlg != nullptr);
            if (!settingsDlg) return;
            settingsDlg->showPage(SettingsDialog::Profile);
            ProfilePage *profile = settingsDlg->profilePage();
            CHECK(profile != nullptr);
            CHECK(settingsDlg->windowTitle().contains(QStringLiteral("Settings")));
            CHECK(profile->findChild<QPushButton *>(QStringLiteral("avatarChoose")) != nullptr);
            // the page shows the current display name and the current avatar
            auto *nameEdit = profile->findChild<QLineEdit *>(QStringLiteral("displayNameEdit"));
            auto *avatarView = profile->findChild<QLabel *>(QStringLiteral("avatarView"));
            CHECK(nameEdit && avatarView);
            if (nameEdit && avatarView) {
                CHECK(waitFor([&] { return nameEdit->text() == QStringLiteral("Old Name"); }, 5000));
                CHECK(waitFor([&] { return !avatarView->pixmap(Qt::ReturnByValue).isNull(); }, 5000));
            }
            CHECK(changeSignalUsername(profile, QStringLiteral("newname"), logFile));
            CHECK(changeProfileDisplayName(profile, QStringLiteral("New Display Name"), logFile));
            CHECK(removeProfileAvatar(profile, logFile));
            settingsDriven = true;
            settingsDlg->reject();
        });
        if (settingsAct) settingsAct->trigger(); // returns when the dialog is closed
        CHECK(settingsDriven);

        CHECK(chooseChatExpiration(w, list, "Team", 4)); // 1 hour
        CHECK(waitFor([&] { return !findCall(logFile, "updateGroup").isEmpty(); }));
        QJsonObject groupTimer = findCall(logFile, "updateGroup")["params"].toObject();
        CHECK(groupTimer["groupId"].toString() == "R1JPVVA=");
        CHECK(groupTimer["expiration"].toInt() == 3600);

        // --- incoming message with quote and image attachment
        inject(injectFile, QJsonObject{
            {"sourceNumber", "+49111"}, {"sourceUuid", "u-alice"}, {"sourceName", "Alice"},
            {"timestamp", 1700000000000LL},
            {"dataMessage", QJsonObject{
                {"timestamp", 1700000000000LL}, {"message", "look at this 📷"},
                {"quote", QJsonObject{{"id", 1699999999000LL}, {"authorNumber", "+491234567"}, {"text", "earlier question"}}},
                {"attachments", QJsonArray{QJsonObject{{"contentType", "image/png"}, {"filename", "pic.png"},
                                                       {"id", "att1"}, {"size", 6321}}}}}}});
        CHECK(waitFor([&] { return listHas(list, "Alice (1)"); }));
        selectItem(list, "Alice");
        CHECK(waitFor([&] { return view->messageCount() == 1; }));
        CHECK(waitFor([&] { return html(view).contains("[image: pic.png]"); }));  // thumbnail fetched via getAttachment
        CHECK(html(view).contains("look at this"));
        CHECK(html(view).contains("earlier question"));              // quote rendered
        CHECK(html(view).contains("You"));                           // quote author is us
        CHECK(!findCall(logFile, "getAttachment").isEmpty());
        CHECK(findCall(logFile, "getAttachment")["params"].toObject()["id"].toString() == "att1");
        CHECK(view->messageAt(view->bubbleRect(0).center()) == 0);

        // --- archived contact receives a message -> leaves the folder
        inject(injectFile, QJsonObject{
            {"sourceNumber", "+49222"}, {"sourceUuid", "u-bob"}, {"sourceName", "Bob"},
            {"timestamp", 1700000050000LL},
            {"dataMessage", QJsonObject{{"timestamp", 1700000050000LL}, {"message", "hi from bob"}}}});
        CHECK(waitFor([&] { return listHas(list, "Bob (1)"); }));
        CHECK(listHas(list, "Archived chats (1)"));

        // --- reply with quote + attachment (dropped file)
        QImage img(40, 30, QImage::Format_RGB32);
        img.fill(Qt::red);
        const QString imgPath = tmp.path() + "/photo.png";
        CHECK(img.save(imgPath, "PNG"));
        emit view->replyRequested(0);
        w.addPendingFiles({imgPath});
        input->setText("my reply 😀");
        QTest::keyClick(input, Qt::Key_Return);
        CHECK(waitFor([&] { return !findCall(logFile, "send").isEmpty(); }));
        const QJsonObject sendParams = findCall(logFile, "send")["params"].toObject();
        CHECK(sendParams["message"].toString() == "my reply 😀");
        CHECK(sendParams["quoteTimestamp"].toVariant().toLongLong() == 1700000000000LL);
        CHECK(sendParams["quoteAuthor"].toString() == "+49111");
        CHECK(sendParams["quoteMessage"].toString().contains("look at this"));
        CHECK(sendParams["recipient"].toArray().first().toString() == "+49111");
        CHECK(sendParams["attachments"].toArray().first().toString() == imgPath);
        CHECK(waitFor([&] { return view->messageCount() == 2; }));
        CHECK(html(view).contains("my reply"));

        // --- Note to Self: pinned above every chat (only folder rows above it) and sends to our own account
        {
            int selfRow = -1;
            for (int i = 0; i < list->count(); ++i) {
                const QString text = list->item(i)->text();
                if (text == QStringLiteral("Note to Self")) { selfRow = i; break; }
                CHECK(text.contains(QStringLiteral("✉")) || text.contains(QStringLiteral("🗄")) ||
                      text.startsWith(QStringLiteral("←"))); // only special rows sit above it
            }
            CHECK(selfRow >= 0);
            selectItem(list, "Note to Self");
            input->setText("hello me");
            QTest::keyClick(input, Qt::Key_Return);
            CHECK(waitFor([&] { return view->messageCount() == 1; }));
            const QJsonObject selfSend = lastCall(logFile, "send")["params"].toObject();
            CHECK(selfSend["message"].toString() == "hello me");
            CHECK(selfSend["recipient"].toArray().first().toString() == "+491234567");
            selectItem(list, "Alice"); // the following tests expect Alice's chat to be open
        }

        // --- Note to Self can be archived (like the official client): the row
        //     moves into the "Archived chats" folder and comes back via the menu.
        {
            CHECK(chooseChatAction(w, list, "Note to Self", "Archive"));
            CHECK(waitFor([&] { return listHas(list, "Archived chats (2)"); })); // Carol + Note to Self
            CHECK(!listHas(list, "Note to Self")); // gone from the main list

            // Inside the folder the chat shows its notepad glyph and unarchives from there.
            QListWidgetItem *folder = nullptr;
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->text().contains("Archived chats")) { folder = list->item(i); break; }
            CHECK(folder);
            if (folder) {
                QMetaObject::invokeMethod(list, "itemClicked", Qt::DirectConnection,
                                          Q_ARG(QListWidgetItem *, folder)); // open the folder
                CHECK(waitFor([&] { return listHas(list, "Note to Self"); }));
                CHECK(listHas(list, "Back to chats"));
                CHECK(chooseChatAction(w, list, "Note to Self", "Unarchive"));
                CHECK(waitFor([&] { return !listHas(list, "Note to Self"); })); // left the folder
                QListWidgetItem *back = nullptr;
                for (int i = 0; i < list->count(); ++i)
                    if (list->item(i)->text().contains("Back to chats")) { back = list->item(i); break; }
                CHECK(back);
                if (back) {
                    QMetaObject::invokeMethod(list, "itemClicked", Qt::DirectConnection,
                                              Q_ARG(QListWidgetItem *, back)); // back to the main list
                }
                CHECK(waitFor([&] { return listHas(list, "Note to Self"); })); // pinned again
                CHECK(listHas(list, "Archived chats (1)")); // only Carol remains archived
                selectItem(list, "Alice"); // the following tests expect Alice's chat to be open
            }
        }

        // --- contact management: search, message requests, block
        {
            // the chat search field is hidden by default; ONLY the File menu entry
            // ("Search chats …", Ctrl+K) reveals it, and pressing it again — like Esc —
            // closes it and resets the filter
            auto *searchField = w.findChild<QLineEdit *>(QStringLiteral("contactSearch"));
            CHECK(searchField);
            CHECK(!w.findChild<QToolButton *>(QStringLiteral("contactSearchToggle"))); // no button on purpose
            QAction *searchAction = nullptr;
            const auto actions = w.findChildren<QAction *>();
            for (QAction *a : actions)
                if (a->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_K)) { searchAction = a; break; }
            CHECK(searchAction);
            if (searchField && searchAction) {
                CHECK(!searchField->isVisible());
                searchAction->trigger(); // what pressing Ctrl+K runs
                CHECK(searchField->isVisible());
                CHECK(QApplication::focusWidget() == searchField);
                CHECK(searchAction->text() == MainWindow::tr("Hide chat search")); // menu now offers hiding
                searchField->setText(QStringLiteral("ali"));
                CHECK(waitFor([&] { return listVisibleHas(list, "Alice"); }));
                CHECK(!listVisibleHas(list, "Note to Self")); // the pinned row hides while searching
                searchAction->trigger(); // Ctrl+K / the menu entry again: hide + reset
                CHECK(!searchField->isVisible());
                CHECK(searchField->text().isEmpty());
                CHECK(listVisibleHas(list, "Note to Self")); // filter reset: the row is back
                CHECK(searchAction->text() == MainWindow::tr("Search chats …")); // label back
                searchAction->trigger(); // and the field comes back
                CHECK(searchField->isVisible());
                QTest::keyClick(searchField, Qt::Key_Escape);
                CHECK(!searchField->isVisible());
                CHECK(searchField->text().isEmpty());
                CHECK(listVisibleHas(list, "Note to Self"));
            }

            // the search filters the chat list and finds address book contacts without a chat
            auto *searchList = w.findChild<QLineEdit *>(QStringLiteral("contactSearch"));
            CHECK(searchList);
            if (searchList) {
                QAction *searchAction = nullptr;
                const auto actions = w.findChildren<QAction *>();
                for (QAction *a : actions)
                    if (a->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_K)) { searchAction = a; break; }
                if (searchAction && !searchList->isVisible()) searchAction->trigger(); // open it like a user
                searchList->setText(QStringLiteral("erin"));
                CHECK(waitFor([&] { return listVisibleHas(list, "Erin"); })); // contact hit from the address book
                QListWidgetItem *erin = nullptr;
                for (int i = 0; i < list->count(); ++i)
                    if (list->item(i)->text().contains("Erin")) { erin = list->item(i); break; }
                CHECK(erin);
                if (erin) {
                    QMetaObject::invokeMethod(list, "itemClicked", Qt::DirectConnection, Q_ARG(QListWidgetItem *, erin));
                    CHECK(waitFor([&] { // the hit turned into a plain (unfiltered) chat row
                        for (int i = 0; i < list->count(); ++i) {
                            QListWidgetItem *row = list->item(i);
                            if (row->text().contains("Erin")) return !row->isHidden() && !row->text().contains("👤");
                        }
                        return false;
                    }));
                }
                searchList->clear();
                QTest::keyClick(searchList, Qt::Key_Escape); // close it again
            }

            // a message from a hidden contact shows up as a message request
            inject(injectFile, QJsonObject{
                {"sourceNumber", "+49444"}, {"sourceUuid", "u-dave"}, {"sourceName", "Dave"},
                {"timestamp", 1700000500000LL},
                {"dataMessage", QJsonObject{{"timestamp", 1700000500000LL}, {"message", "dave here"}}}});
            CHECK(waitFor([&] { return listVisibleHas(list, "Message requests (1)"); }));
            QListWidgetItem *requestsRow = nullptr;
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->text().contains("Message requests")) { requestsRow = list->item(i); break; }
            CHECK(requestsRow);
            if (requestsRow) {
                QTimer::singleShot(0, &w, [&] {
                    for (QWidget *top : QApplication::topLevelWidgets()) {
                        if (auto *dlg = qobject_cast<MessageRequestsDialog *>(top)) {
                            const auto buttons = dlg->findChildren<QPushButton *>(QStringLiteral("requestAccept"));
                            if (!buttons.isEmpty()) buttons.first()->click();
                            return;
                        }
                    }
                });
                QMetaObject::invokeMethod(list, "itemClicked", Qt::DirectConnection, Q_ARG(QListWidgetItem *, requestsRow));
                CHECK(waitFor([&] { return !findCall(logFile, "removeContact").isEmpty(); }));
                CHECK(findCall(logFile, "removeContact")["params"].toObject()["recipient"].toString() == "u-dave");
                CHECK(waitFor([&] { return listVisibleHas(list, "Dave"); })); // accepted: a normal chat row
                bool requestsLeft = false;
                for (int i = 0; i < list->count(); ++i)
                    if (list->item(i)->text().contains("Message requests")) requestsLeft = true;
                CHECK(!requestsLeft);
            }

            // block a contact from the context menu: her row is marked, her messages are dropped
            selectItem(list, "Alice");
            const int aliceBefore = view->messageCount();
            CHECK(chooseChatAction(w, list, "Alice", "Block contact", "Yes"));
            CHECK(waitFor([&] { return listVisibleHas(list, "🚫"); }));
            CHECK(findCall(logFile, "block")["params"].toObject()["recipient"].toArray().first().toString() == "+49111");
            inject(injectFile, QJsonObject{
                {"sourceNumber", "+49111"}, {"sourceUuid", "u-alice"}, {"timestamp", 1700000600000LL},
                {"dataMessage", QJsonObject{{"timestamp", 1700000600000LL}, {"message", "should not arrive"}}}});
            CHECK(!waitFor([&] { return view->messageCount() != aliceBefore; }, 700));
            CHECK(chooseChatAction(w, list, "Alice", "Unblock contact"));
            CHECK(waitFor([&] { return !listVisibleHas(list, "🚫"); }));
            CHECK(!findCall(logFile, "unblock").isEmpty());

            selectItem(list, "Alice"); // the following tests expect Alice's chat to be open
        }

        auto *search = w.findChild<QLineEdit *>(QStringLiteral("messageSearchInput"));
        auto *searchCount = w.findChild<QLabel *>(QStringLiteral("messageSearchCount"));
        CHECK(search && searchCount);
        if (search && searchCount) {
            input->setFocus();
            QTest::keyClick(input, Qt::Key_F, Qt::ControlModifier);
            CHECK(waitFor([&] { return search->isVisible(); }));
            search->setText(QStringLiteral("e"));
            CHECK(searchCount->text() == QStringLiteral("1 / 2"));
            QTest::keyClick(search, Qt::Key_Return);
            CHECK(searchCount->text() == QStringLiteral("2 / 2"));
            QTest::keyClick(search, Qt::Key_Return);
            CHECK(searchCount->text() == QStringLiteral("1 / 2"));
            QTest::keyClick(search, Qt::Key_Escape);
            CHECK(!search->isVisible());
        }

        // --- react (and take the reaction back by choosing the same emoji again)
        emit view->reactionChosen(0, QStringLiteral("👍"));
        CHECK(waitFor([&] { return !findCall(logFile, "sendReaction").isEmpty(); }));
        QJsonObject r1 = findCall(logFile, "sendReaction")["params"].toObject();
        CHECK(r1["emoji"].toString() == "👍");
        CHECK(r1["targetAuthor"].toString() == "+49111");
        CHECK(r1["targetTimestamp"].toVariant().toLongLong() == 1700000000000LL);
        CHECK(!r1["remove"].toBool());
        CHECK(waitFor([&] { return html(view).contains("👍"); }));
        emit view->reactionChosen(0, QStringLiteral("👍"));
        CHECK(waitFor([&] { return findCall(logFile, "sendReaction", 1)["params"].toObject()["remove"].toBool(); }));
        CHECK(waitFor([&] { return !html(view).contains("👍"); }));

        // --- Alice reacts to our reply (target = our number + the timestamp mock returned)
        inject(injectFile, QJsonObject{
            {"sourceNumber", "+49111"}, {"sourceUuid", "u-alice"}, {"timestamp", 1700000200000LL},
            {"dataMessage", QJsonObject{{"timestamp", 1700000200000LL},
                {"reaction", QJsonObject{{"emoji", "❤️"}, {"targetAuthorNumber", "+491234567"},
                                         {"targetSentTimestamp", 1700000100001LL}, {"isRemove", false}}}}}});
        CHECK(waitFor([&] { return html(view).contains("❤"); }));

        // --- message sent from the phone (sync) with a group id
        inject(injectFile, QJsonObject{
            {"sourceNumber", "+491234567"}, {"timestamp", 1700000300000LL},
            {"syncMessage", QJsonObject{{"sentMessage", QJsonObject{
                {"destinationNumber", "+49111"}, {"timestamp", 1700000300000LL}, {"message", "sent from phone"}}}}}});
        CHECK(waitFor([&] { return view->messageCount() == 3; }));

        // --- unread attachment-only message in the group
        inject(injectFile, QJsonObject{
            {"sourceNumber", "+49111"}, {"sourceUuid", "u-alice"}, {"sourceName", "Alice"}, {"timestamp", 1700000400000LL},
            {"dataMessage", QJsonObject{{"timestamp", 1700000400000LL},
                {"groupInfo", QJsonObject{{"groupId", "R1JPVVA="}}},
                {"attachments", QJsonArray{QJsonObject{{"contentType", "application/pdf"}, {"filename", "doc.pdf"},
                                                       {"id", "att2"}, {"size", 2048}}}}}}});
        CHECK(waitFor([&] { return listHas(list, "Team (1)"); }));

        // --- pasting with Ctrl+V: a screenshot in the clipboard becomes an attachment
        {
            auto labelWith = [&](const QString &needle) {
                for (QLabel *l : w.findChildren<QLabel *>())
                    if (l->isVisible() && l->text().contains(needle)) return true;
                return false;
            };
            auto clearPending = [&] {
                for (QToolButton *b : w.findChildren<QToolButton *>())
                    if (b->text() == "✕" && b->parentWidget()->isVisible()) b->click();
            };
            input->clear();
            input->setFocus();
            QImage screenshot(300, 200, QImage::Format_RGB32);
            screenshot.fill(Qt::darkGreen);

            QApplication::clipboard()->setImage(screenshot);
            QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
            CHECK(waitFor([&] { return labelWith("pasted-"); }, 3000));
            CHECK(input->text().isEmpty());
            clearPending();
            CHECK(!labelWith("pasted-"));

            QApplication::clipboard()->setImage(screenshot); // Shift+Insert is "paste" as well
            QTest::keyClick(input, Qt::Key_Insert, Qt::ShiftModifier);
            CHECK(waitFor([&] { return labelWith("pasted-"); }, 3000));
            clearPending();

            QApplication::clipboard()->setText("pasted text"); // plain text must still paste normally
            QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
            CHECK(input->text() == "pasted text");
            CHECK(!labelWith("pasted-"));
            input->clear();

            { // files copied in a file manager
                auto *md = new QMimeData;
                md->setUrls({QUrl::fromLocalFile(tmp.path() + "/photo.png")});
                QApplication::clipboard()->setMimeData(md);
                QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
                CHECK(waitFor([&] { return labelWith("photo.png"); }, 3000));
                clearPending();
            }
            { // image *and* text on the clipboard (e.g. copied from a document): the text wins
                auto *md = new QMimeData;
                md->setImageData(screenshot);
                md->setText("some text");
                QApplication::clipboard()->setMimeData(md);
                QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
                CHECK(input->text() == "some text");
                CHECK(!labelWith("pasted-"));
                input->clear();
            }
            { // paste a screenshot and actually send it (to Bob)
                selectItem(list, "Bob");
                CHECK(waitFor([&] { return view->messageCount() == 1; }));
                input->setFocus();
                QApplication::clipboard()->setImage(screenshot);
                QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
                CHECK(waitFor([&] { return labelWith("pasted-"); }, 3000));
                QTest::keyClick(input, Qt::Key_Return);
                CHECK(waitFor([&] { return !findCall(logFile, "send", 1).isEmpty(); }));
                const QJsonObject sp = findCall(logFile, "send", 1)["params"].toObject();
                CHECK(sp["recipient"].toArray().first().toString() == "+49222");
                const QString sent = sp["attachments"].toArray().first().toString();
                CHECK(sent.contains("pasted-") && sent.endsWith(".png"));
                CHECK(waitFor([&] { return view->messageCount() == 2; }));
                CHECK(html(view).contains("[image: pasted-"));
                CHECK(!labelWith("pasted-")); // attachment bar is cleared after sending
            }
        }

        // --- editing messages
        {
            auto labelWith = [&](const QString &needle) {
                for (QLabel *l : w.findChildren<QLabel *>())
                    if (l->isVisible() && l->text().contains(needle)) return true;
                return false;
            };
            // Only own messages can be edited; received ones are refused.
            input->clear();
            emit view->editRequested(0);
            CHECK(!labelWith("Editing message"));
            CHECK(input->text().isEmpty());

            // A message we sent from the phone, just now.
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            inject(injectFile, QJsonObject{
                {"sourceNumber", "+491234567"}, {"timestamp", now},
                {"syncMessage", QJsonObject{{"sentMessage", QJsonObject{
                    {"destinationNumber", "+49222"}, {"timestamp", now}, {"message", "hello wrold"}}}}}});
            CHECK(waitFor([&] { return view->messageCount() == 3; }));
            const int idx = 2;

            input->clear();
            QTest::keyClick(input, Qt::Key_Up);
            CHECK(input->text() == "hello wrold");
            CHECK(labelWith("Editing message"));
            QTest::keyClick(input, Qt::Key_Escape);
            CHECK(input->text().isEmpty());

            input->setText("draft in progress");
            input->setCursorPosition(5);
            QTest::keyClick(input, Qt::Key_Up);
            CHECK(input->cursorPosition() == 0);
            CHECK(input->text() == "draft in progress");
            CHECK(!labelWith("Editing message"));
            QTest::keyClick(input, Qt::Key_Down);
            CHECK(input->cursorPosition() == input->text().size());
            QTest::keyClick(input, Qt::Key_Up, Qt::ShiftModifier);
            CHECK(input->selectedText() == "draft in progress");
            QTest::keyClick(input, Qt::Key_Up);
            CHECK(input->text() == "draft in progress");
            CHECK(!labelWith("Editing message"));

            // Edit mode: text is loaded, Esc cancels and restores what was typed before
            emit view->editRequested(idx);
            CHECK(input->text() == "hello wrold");
            CHECK(labelWith("Editing message"));
            QTest::keyClick(input, Qt::Key_Escape);
            CHECK(input->text() == "draft in progress");
            CHECK(!labelWith("Editing message"));
            input->clear();

            // Edit and send: signal-cli `send` with editTimestamp = the message being edited
            emit view->editRequested(idx);
            input->setText("hello world");
            QTest::keyClick(input, Qt::Key_Return);
            CHECK(waitFor([&] { return !findCall(logFile, "send", 2).isEmpty(); }));
            const QJsonObject ep = findCall(logFile, "send", 2)["params"].toObject();
            CHECK(ep["message"].toString() == "hello world");
            CHECK(ep["editTimestamp"].toVariant().toLongLong() == now);
            CHECK(ep["recipient"].toArray().first().toString() == "+49222");
            CHECK(!ep.contains("attachments"));
            CHECK(waitFor([&] { return html(view).contains("You: hello world (edited)"); }));
            CHECK(!html(view).contains("wrold"));
            CHECK(view->messageCount() == 3);        // edited in place, not added
            CHECK(!labelWith("Editing message"));
            CHECK(input->text().isEmpty());

            // Second edit targets the latest revision (what the recipient knows now)
            emit view->editRequested(idx);
            input->setText("hello world!");
            QTest::keyClick(input, Qt::Key_Return);
            CHECK(waitFor([&] { return !findCall(logFile, "send", 3).isEmpty(); }));
            CHECK(findCall(logFile, "send", 3)["params"].toObject()["editTimestamp"].toVariant().toLongLong() == now + 1000);
            CHECK(waitFor([&] { return html(view).contains("You: hello world! (edited)"); }));

            // Empty and unchanged edits send nothing
            emit view->editRequested(idx);
            input->setText("   ");
            QTest::keyClick(input, Qt::Key_Return);
            QTest::qWait(100);
            CHECK(labelWith("Editing message"));
            CHECK(findCall(logFile, "send", 4).isEmpty());
            input->setText("hello world!");
            QTest::keyClick(input, Qt::Key_Return);
            QTest::qWait(100);
            CHECK(!labelWith("Editing message"));
            CHECK(findCall(logFile, "send", 4).isEmpty());

            // Bob edits his message (incoming edit)
            inject(injectFile, QJsonObject{
                {"sourceNumber", "+49222"}, {"sourceUuid", "u-bob"}, {"timestamp", 1700000060000LL},
                {"editMessage", QJsonObject{{"targetSentTimestamp", 1700000050000LL},
                    {"dataMessage", QJsonObject{{"timestamp", 1700000060000LL}, {"message", "hi from bob, corrected"}}}}}});
            CHECK(waitFor([&] { return html(view).contains("Bob: hi from bob, corrected (edited)"); }));
            CHECK(view->messageCount() == 3);
            // the same envelope again (e.g. redelivery) changes nothing
            const QString before = html(view);
            inject(injectFile, QJsonObject{
                {"sourceNumber", "+49222"}, {"sourceUuid", "u-bob"}, {"timestamp", 1700000060000LL},
                {"editMessage", QJsonObject{{"targetSentTimestamp", 1700000050000LL},
                    {"dataMessage", QJsonObject{{"timestamp", 1700000060000LL}, {"message", "hi from bob, corrected"}}}}}});
            QTest::qWait(200);
            CHECK(html(view) == before);

            // Bob cannot edit *our* message
            inject(injectFile, QJsonObject{
                {"sourceNumber", "+49222"}, {"sourceUuid", "u-bob"}, {"timestamp", now + 9000},
                {"editMessage", QJsonObject{{"targetSentTimestamp", now},
                    {"dataMessage", QJsonObject{{"timestamp", now + 9000}, {"message", "forged"}}}}}});
            QTest::qWait(300);
            CHECK(html(view).contains("You: hello world! (edited)"));
            CHECK(!html(view).contains("forged"));
            CHECK(view->messageCount() == 3);

            // Edit made on another of our devices (sync), addressed to the latest revision ...
            inject(injectFile, QJsonObject{
                {"sourceNumber", "+491234567"}, {"timestamp", now + 5000},
                {"syncMessage", QJsonObject{{"sentMessage", QJsonObject{
                    {"destinationNumber", "+49222"}, {"timestamp", now + 5000},
                    {"editMessage", QJsonObject{{"targetSentTimestamp", now + 2000},
                        {"dataMessage", QJsonObject{{"timestamp", now + 5000}, {"message", "edited on the phone"}}}}}}}}}});
            CHECK(waitFor([&] { return html(view).contains("You: edited on the phone (edited)"); }));
            // ... or to the original timestamp
            inject(injectFile, QJsonObject{
                {"sourceNumber", "+491234567"}, {"timestamp", now + 6000},
                {"syncMessage", QJsonObject{{"sentMessage", QJsonObject{
                    {"destinationNumber", "+49222"}, {"timestamp", now + 6000},
                    {"editMessage", QJsonObject{{"targetSentTimestamp", now},
                        {"dataMessage", QJsonObject{{"timestamp", now + 6000}, {"message", "phone, via original ts"}}}}}}}}}});
            CHECK(waitFor([&] { return html(view).contains("You: phone, via original ts (edited)"); }));
            CHECK(view->messageCount() == 3);

            // Reactions and replies address the latest revision
            emit view->reactionChosen(idx, QStringLiteral("👍"));
            CHECK(waitFor([&] { return !findCall(logFile, "sendReaction", 2).isEmpty(); }));
            CHECK(findCall(logFile, "sendReaction", 2)["params"].toObject()["targetTimestamp"].toVariant().toLongLong() == now + 6000);

            // Limits: 3 hours and 10 edits; received and attachment-only messages never
            Message lim;
            lim.outgoing = true;
            lim.text = "x";
            lim.time = QDateTime::fromMSecsSinceEpoch(now);
            CHECK(lim.canEdit(now + 2LL * 3600 * 1000));
            CHECK(!lim.canEdit(now + 3LL * 3600 * 1000 + 1));
            lim.editCount = Message::kMaxEdits;
            CHECK(!lim.canEdit(now));
            lim.editCount = 0;
            lim.outgoing = false;
            CHECK(!lim.canEdit(now));
            lim.outgoing = true;
            lim.text.clear();
            CHECK(!lim.canEdit(now));

            // Delete for everyone leaves a placeholder where the message was.
            // Bob cannot delete *our* message ...
            inject(injectFile, QJsonObject{
                {"sourceNumber", "+49222"}, {"sourceUuid", "u-bob"}, {"timestamp", now + 7000},
                {"dataMessage", QJsonObject{{"timestamp", now + 7000},
                    {"remoteDelete", QJsonObject{{"timestamp", now}}}}}});
            QTest::qWait(300);
            CHECK(!html(view).contains("You deleted this message."));
            CHECK(html(view).contains("phone, via original ts"));
            // ... but our phone can (sync message).
            inject(injectFile, QJsonObject{
                {"sourceNumber", "+491234567"}, {"timestamp", now + 8000},
                {"syncMessage", QJsonObject{{"sentMessage", QJsonObject{
                    {"destinationNumber", "+49222"}, {"timestamp", now + 8000},
                    {"remoteDelete", QJsonObject{{"timestamp", now}}}}}}}});
            CHECK(waitFor([&] { return html(view).contains("You deleted this message."); }));
            CHECK(!html(view).contains("phone, via original ts"));
            CHECK(view->messageCount() == 3); // placeholder, not removed
        }

        // --- emoji picker emits what was clicked
        EmojiPicker picker;
        QString got;
        QObject::connect(&picker, &EmojiPicker::emojiSelected, [&](const QString &e) { got = e; });
        CHECK(picker.emojiCount() > 1500);
        const QStringList shown = picker.visibleEmojis();
        CHECK(shown.contains(QStringLiteral("🇩🇪")));
        CHECK(shown.contains(QStringLiteral("👍")));
        CHECK(picker.toneVariants(QStringLiteral("🇩🇪")).isEmpty());
        CHECK(picker.toneVariants(QStringLiteral("👍")).contains(QStringLiteral("👍🏿")));
        picker.activateEmoji(QStringLiteral("👍"));
        CHECK(got == QStringLiteral("👍"));
        picker.activateEmoji(QStringLiteral("👍🏿"));
        CHECK(got == QStringLiteral("👍🏿"));
        // Used emojis are listed first as "recent"; searching narrows the list.
        CHECK(picker.visibleEmojis().first() == QStringLiteral("👍🏿"));
        if (auto *search = picker.findChild<QLineEdit *>()) {
            search->setText(QStringLiteral("thumbs up"));
            const QStringList found = picker.visibleEmojis();
            CHECK(found.contains(QStringLiteral("👍")) && found.size() < 20);
            search->clear();
            CHECK(picker.visibleEmojis().size() > 1500);
        } else {
            CHECK(false);
        }

        // A message-specific timer begins when the visible chat receives it and removes
        // the message from both the view and the encrypted history.
        selectItem(list, "Alice");
        CHECK(waitFor([&] { return view->messageCount() == 3; }));
        CHECK(chooseChatExpiration(w, list, "Alice", 3)); // 5 minutes
        CHECK(waitFor([&] { return !findCall(logFile, "updateContact").isEmpty(); }));
        QJsonObject contactTimer = findCall(logFile, "updateContact")["params"].toObject();
        CHECK(contactTimer["recipient"].toString() == "+49111");
        CHECK(contactTimer["expiration"].toInt() == 300);
        const qint64 expiringTs = QDateTime::currentMSecsSinceEpoch();
        inject(injectFile, QJsonObject{
            {"sourceNumber", "+49111"}, {"sourceUuid", "u-alice"}, {"sourceName", "Alice"},
            {"timestamp", expiringTs},
            {"dataMessage", QJsonObject{{"timestamp", expiringTs}, {"message", "vanish locally"},
                                         {"expiresInSeconds", 3}}}});
        CHECK(waitFor([&] { return view->messageCount() == 4 && html(view).contains("vanish locally"); }));
        CHECK(waitFor([&] { return view->messageCount() == 3 && !html(view).contains("vanish locally"); }, 7000));

        const qint64 remoteDeleteTs = QDateTime::currentMSecsSinceEpoch();
        inject(injectFile, QJsonObject{
            {"sourceNumber", "+491234567"}, {"timestamp", remoteDeleteTs},
            {"syncMessage", QJsonObject{{"sentMessage", QJsonObject{
                {"destinationNumber", "+49111"}, {"timestamp", remoteDeleteTs},
                {"message", "delete for everyone"}}}}}});
        CHECK(waitFor([&] { return view->messageCount() == 4 && html(view).contains("delete for everyone"); }));
        g_deleteChoice = QStringLiteral("Delete for everyone");
        emit view->deleteRequested(3);
        CHECK(waitFor([&] { return !findCall(logFile, "remoteDelete").isEmpty(); }));
        const QJsonObject deleteParams = findCall(logFile, "remoteDelete")["params"].toObject();
        CHECK(deleteParams["targetTimestamp"].toVariant().toLongLong() == remoteDeleteTs);
        CHECK(deleteParams["recipients"].toArray().first().toString() == "+49111");
        CHECK(waitFor([&] { return view->messageCount() == 3 && !html(view).contains("delete for everyone"); }));

        const qint64 localDeleteTs = QDateTime::currentMSecsSinceEpoch();
        inject(injectFile, QJsonObject{
            {"sourceNumber", "+49111"}, {"sourceUuid", "u-alice"}, {"sourceName", "Alice"},
            {"timestamp", localDeleteTs},
            {"dataMessage", QJsonObject{{"timestamp", localDeleteTs}, {"message", "delete only here"}}}});
        CHECK(waitFor([&] { return view->messageCount() == 4 && html(view).contains("delete only here"); }));
        g_deleteChoice = QStringLiteral("Delete for me");
        emit view->deleteRequested(3);
        CHECK(waitFor([&] { return view->messageCount() == 3 && !html(view).contains("delete only here"); }));
        CHECK(findCall(logFile, "remoteDelete")["params"].toObject()["targetTimestamp"].toVariant().toLongLong() == remoteDeleteTs);

        const qint64 audioTs = QDateTime::currentMSecsSinceEpoch();
        inject(injectFile, QJsonObject{
            {"sourceNumber", "+49111"}, {"sourceUuid", "u-alice"}, {"sourceName", "Alice"},
            {"timestamp", audioTs},
            {"dataMessage", QJsonObject{{"timestamp", audioTs}, {"attachments", QJsonArray{
                QJsonObject{{"id", "voice1"}, {"contentType", "audio/wav"},
                             {"filename", "voice.wav"}, {"size", 32044}}}}}}});
        CHECK(waitFor([&] { return view->messageCount() == 4; }));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->attachmentRect(3, 0).center());
        CHECK(waitFor([&] { return loggedAttachmentFetch(logFile, QStringLiteral("voice1")); }));
        auto *audioPlayer = w.findChild<QMediaPlayer *>(QStringLiteral("chatAudioPlayer"));
        CHECK(audioPlayer != nullptr);
        CHECK(waitFor([&] { return audioPlayer && audioPlayer->source().isLocalFile() && audioPlayer->duration() > 0; }, 5000));
        CHECK(waitFor([&] { return audioPlayer && audioPlayer->playbackState() == QMediaPlayer::PlayingState; }, 3000));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->attachmentRect(3, 0).center());
        CHECK(waitFor([&] { return audioPlayer && audioPlayer->playbackState() == QMediaPlayer::PausedState; }));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->attachmentRect(3, 0).center());
        CHECK(waitFor([&] { return audioPlayer && audioPlayer->playbackState() == QMediaPlayer::PlayingState; }));

        const qint64 gifTs = QDateTime::currentMSecsSinceEpoch();
        inject(injectFile, QJsonObject{
            {"sourceNumber", "+49111"}, {"sourceUuid", "u-alice"}, {"sourceName", "Alice"},
            {"timestamp", gifTs},
            {"dataMessage", QJsonObject{{"timestamp", gifTs}, {"attachments", QJsonArray{
                QJsonObject{{"id", "chatgif"}, {"contentType", "image/gif"},
                             {"filename", "animated.gif"}, {"size", 1000}}}}}}});
        CHECK(waitFor([&] { return view->messageCount() == 5; }));
        CHECK(waitFor([&] { return view->hasAnimatedAttachment(4, 0); }, 5000));
        bool chatGifPlaying = false;
        for (QMovie *movie : view->findChildren<QMovie *>())
            if (movie->state() == QMovie::Running) chatGifPlaying = true;
        CHECK(chatGifPlaying);

        // stickers play like gifs: the chat animation is built from the
        // full-size sticker data, not from the static first-frame thumbnail.
        // Sticker 0 of the mock pack is an APNG — Signal's real animated
        // sticker format, which QMovie cannot play — so this exercises the
        // ApngMovie path; sticker 1 stays an animated WebP for QMovie.
        const qint64 stickerTs = QDateTime::currentMSecsSinceEpoch();
        inject(injectFile, QJsonObject{
            {"sourceNumber", "+49111"}, {"sourceUuid", "u-alice"}, {"sourceName", "Alice"},
            {"timestamp", stickerTs},
            {"dataMessage", QJsonObject{{"timestamp", stickerTs},
                                        {"sticker", QJsonObject{{"packId", "corvusanim"}, {"stickerId", 0}}}}}});
        CHECK(waitFor([&] { return view->messageCount() == 6; }));
        CHECK(waitFor([&] { return view->hasAnimatedAttachment(5, 0); }, 5000));
        CHECK(!view->findChildren<ApngMovie *>().isEmpty());
        bool stickerPlaying = false;
        for (QMovie *movie : view->findChildren<QMovie *>())
            if (movie->state() == QMovie::Running) stickerPlaying = true;
        for (ApngMovie *apng : view->findChildren<ApngMovie *>())
            if (apng->isRunning()) stickerPlaying = true;
        CHECK(stickerPlaying);

        CHECK(chooseChatNickname(w, list, "Bob", "Bobby"));
        CHECK(listHas(list, "Bobby"));

        CHECK(startConversation(w, QStringLiteral("alice.1234")));
                CHECK(listHas(list, "alice.1234"));

        CHECK(g_unexpectedBox.isEmpty());
        w.close();
    }

    // ---------------------------------------------------------------- run 2
    std::printf("run 2: restart, history must come back from the encrypted file\n");
    {
        QFile::remove(injectFile);
        MainWindow w;
        w.show();
        auto *list = w.findChild<QListWidget *>();
        auto *view = w.findChild<ChatView *>();
        CHECK(waitFor([&] { return listHas(list, "Alice"); }));
        CHECK(listHas(list, "Archived chats (1)")); // Carol only: Bob stays unarchived (phone state unchanged)
        CHECK(waitFor([&] { return listHas(list, "Bobby"); }));
        CHECK(waitFor([&] { return w.findChild<QLabel *>() != nullptr; }));
        selectItem(list, "Alice");
        CHECK(waitFor([&] { return view->messageCount() == 6; })); // incl. Alice's animated sticker
        const QString h = html(view);
        CHECK(h.contains("look at this"));
        CHECK(h.contains("earlier question"));
        CHECK(h.contains("my reply"));
        CHECK(h.contains("sent from phone"));
        CHECK(h.contains("[file: voice.wav]"));
        CHECK(h.contains("[image: animated.gif]"));
        CHECK(!h.contains("delete only here"));
        CHECK(!h.contains("delete for everyone"));
        CHECK(h.contains("❤"));
        CHECK(h.count("[image:") >= 2);       // incoming thumbnail + own thumbnail, restored without signal-cli
        CHECK(!h.contains("👍"));          // removed reaction stays removed
        CHECK(waitFor([&] { return listHas(list, "Team"); })); // title arrives with listGroups
        // edits survive a restart (stored as records of the encrypted history)
        selectItem(list, "Bobby");
        CHECK(waitFor([&] { return view->messageCount() == 3; }));
        CHECK(html(view).contains("Bob: hi from bob, corrected (edited)"));
        CHECK(html(view).contains("You: phone, via original ts (edited)"));
        CHECK(!html(view).contains("wrold"));
        selectItem(list, "Team");
        CHECK(waitFor([&] { return view->messageCount() == 1; }));
        CHECK(html(view).contains("doc.pdf"));
        w.close();
    }

    // The history file must not contain any message text.
    {
        QFile f(tmp.path() + "/data/corvus/corvus/history.corvus");
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray raw = f.readAll();
        CHECK(raw.size() > 1000);
        CHECK(!raw.contains("look at this"));
        CHECK(!raw.contains("Alice"));
        CHECK(!raw.contains("PNG"));
    }

    CHECK(g_unexpectedBox.isEmpty());
    if (!g_unexpectedBox.isEmpty()) std::printf("  unexpected message box: %s\n", qPrintable(g_unexpectedBox));
    std::printf(g_failed ? "FAILED (%d checks)\n" : "ALL OK\n", g_failed);
    return g_failed ? 1 : 0;
}
