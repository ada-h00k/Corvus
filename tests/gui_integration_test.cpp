// Headless end-to-end test: real MainWindow + encrypted Rust store + mock signal-cli.
#include "ChatView.h"
#include "EmojiPicker.h"
#include "GiphyPicker.h"
#include "HistoryKeychain.h"
#include "ImageViewer.h"
#include "MainWindow.h"
#include "PassphraseDialog.h"

#include <QApplication>
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
#include <QMimeData>
#include <QMovie>
#include <QMediaPlayer>
#include <QPainter>
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

static bool loggedAttachmentFetch(const QString &log, const QString &attachmentId)
{
    for (const QJsonObject &request : readLog(log))
        if (request["method"].toString() == QStringLiteral("getAttachment") &&
            request["params"].toObject()["id"].toString() == attachmentId)
            return true;
    return false;
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
    QApplication::setOrganizationName(QStringLiteral("signal-cli-qt"));
    QApplication::setApplicationName(QStringLiteral("signal-cli-qt"));

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
        auto *input = w.findChild<QLineEdit *>();
        CHECK(list && view && input);
        QToolButton *attachBtn = nullptr;
        QToolButton *emojiBtn = nullptr;
        QToolButton *gifBtn = nullptr;
        for (QToolButton *button : w.findChildren<QToolButton *>()) {
            if (button->toolTip() == QStringLiteral("Attach files")) attachBtn = button;
            else if (button->toolTip() == QStringLiteral("Emoji")) emojiBtn = button;
            else if (button->toolTip() == QStringLiteral("Search GIPHY GIFs")) gifBtn = button;
        }
        CHECK(attachBtn && emojiBtn && gifBtn);
        if (attachBtn && emojiBtn && gifBtn && input) {
            CHECK(attachBtn->size() == emojiBtn->size());
            CHECK(gifBtn->size() == emojiBtn->size());
            CHECK(attachBtn->mapTo(&w, QPoint()).x() >= input->mapTo(&w, QPoint()).x() + input->width());
        }

        CHECK(waitFor([&] { return listHas(list, "Archived chats") && listHas(list, "Team"); }));
        CHECK(listHas(list, "Archived chats (2)")); // Bob + Carol archived on the "phone"
        CHECK(!listHas(list, "Alice")); // active contacts appear when a chat exists
        CHECK(!listHas(list, "Bob"));
        CHECK(!listHas(list, "Carol"));
        CHECK(listHas(list, "Team"));
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
        }

        // --- emoji picker emits what was clicked
        EmojiPicker picker;
        QString got;
        QObject::connect(&picker, &EmojiPicker::emojiSelected, [&](const QString &e) { got = e; });
        auto buttons = picker.findChildren<QToolButton *>();
        CHECK(buttons.size() > 100);
        buttons.first()->click();
        CHECK(!got.isEmpty());

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

        CHECK(chooseChatNickname(w, list, "Bob", "Bobby"));
        CHECK(listHas(list, "Bobby"));

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
        CHECK(waitFor([&] { return view->messageCount() == 5; }));
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
        QFile f(tmp.path() + "/data/signal-cli-qt/signal-cli-qt/history.scq");
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
