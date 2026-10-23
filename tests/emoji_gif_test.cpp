// The "GIF" tab of the emoji popup, against a local stand-in for the GIPHY API.
#include "EmojiPicker.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

namespace {

const QByteArray kGif = QByteArray::fromBase64(
    "R0lGODlhEAAQAPAAAP8AAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACwAAAAAEAAQAAACDoSP"
    "qcvtD6OctNqLsz4FACH5BAAKAAAALAAAAAAQABAAgAAA/wAAAAIOhI+py+0Po5y02ouzPgUAOw==");

// Answers trending/search with one GIF and serves the GIF itself.
class FakeGiphy : public QObject
{
public:
    FakeGiphy()
    {
        QVERIFY2(m_server.listen(QHostAddress::LocalHost), "cannot listen");
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections()) {
                QTcpSocket *socket = m_server.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { answer(socket); });
            }
        });
    }
    QUrl base() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort())); }
    QStringList requests;

private:
    void answer(QTcpSocket *socket)
    {
        const QByteArray request = socket->readAll();
        const QByteArray path = request.left(request.indexOf("\r\n")).split(' ').value(1);
        requests << QString::fromLatin1(path);
        QByteArray body;
        QByteArray type = "application/json";
        if (path.startsWith("/v1/gifs/")) {
            const QString b = base().toString();
            const QJsonObject gif{{"title", "local test gif"},
                                  {"images", QJsonObject{{"fixed_width_small", QJsonObject{{"url", b + "/preview.gif"}}},
                                                         {"original", QJsonObject{{"url", b + "/original.gif"}}}}}};
            body = QJsonDocument(QJsonObject{{"data", QJsonArray{gif}}}).toJson(QJsonDocument::Compact);
        } else if (path.startsWith("/preview.gif")) {
            body = kGif;
            type = "image/gif";
        }
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    QTcpServer m_server;
};

// The "Emoji | GIF" tab bar is the only 34 px high, full-width child.
QWidget *findTabBar(EmojiPicker &picker)
{
    QWidget *bar = nullptr;
    for (QWidget *w : picker.findChildren<QWidget *>())
        if (w->height() == 34 && w->width() > 200) bar = w;
    return bar;
}

void clickGifTab(EmojiPicker &picker)
{
    QWidget *bar = findTabBar(picker);
    QVERIFY(bar && bar->isVisible());
    QTest::mouseClick(bar, Qt::LeftButton, {}, QPoint(bar->width() * 3 / 4, bar->height() / 2));
}

} // namespace

class EmojiGifTest : public QObject
{
    Q_OBJECT
private slots:
    void noGifTabUnlessEnabled()
    {
        EmojiPicker picker;
        picker.popup(QPoint(10, 10));
        QVERIFY(!findTabBar(picker) || !findTabBar(picker)->isVisible());
        picker.showGifTab();
        QVERIFY(!picker.isGifTabActive());

        picker.setGifsAvailable(true);
        QVERIFY(findTabBar(picker) && findTabBar(picker)->isVisible());
        picker.setGifsAvailable(false);
        QVERIFY(!findTabBar(picker)->isVisible());
    }

    void gifTabLoadsAndSends()
    {
        FakeGiphy giphy;
        EmojiPicker picker;
        picker.setGiphyApiBase(giphy.base());
        picker.setGifsAvailable(true);
        QSignalSpy keyAsked(&picker, &EmojiPicker::giphyKeyRequested);
        QSignalSpy chosen(&picker, &EmojiPicker::gifSelected);
        connect(&picker, &EmojiPicker::giphyKeyRequested, &picker,
                [&picker] { picker.setGiphyApiKey(QStringLiteral("local-test-key")); });

        picker.popup(QPoint(10, 10));
        QVERIFY(!picker.isGifTabActive());
        clickGifTab(picker);
        QVERIFY(picker.isGifTabActive());
        QCOMPARE(keyAsked.size(), 1);

        QPushButton *button = nullptr;
        QTRY_VERIFY((button = picker.findChild<QPushButton *>(QStringLiteral("giphyResult0"))) && button->isEnabled());
        button->click();
        QCOMPARE(chosen.size(), 1);
        QCOMPARE(chosen.first().at(0).toUrl(), QUrl(giphy.base().toString() + QStringLiteral("/original.gif")));
        QVERIFY(!picker.isVisible()); // the popup closes
    }

    void searchFieldSearchesGiphyInGifTab()
    {
        FakeGiphy giphy;
        EmojiPicker picker;
        picker.setGiphyApiBase(giphy.base());
        picker.setGifsAvailable(true);
        connect(&picker, &EmojiPicker::giphyKeyRequested, &picker,
                [&picker] { picker.setGiphyApiKey(QStringLiteral("local-test-key")); });
        picker.popup(QPoint(10, 10));
        picker.showGifTab();
        QTRY_VERIFY(giphy.requests.value(0).startsWith(QStringLiteral("/v1/gifs/trending")));

        auto *edit = picker.findChild<QLineEdit *>();
        QVERIFY(edit);
        QCOMPARE(edit->placeholderText(), QStringLiteral("Search GIFs…"));
        edit->setText(QStringLiteral("cats"));
        QTRY_VERIFY(!giphy.requests.filter(QStringLiteral("/v1/gifs/search")).isEmpty());
        QVERIFY(giphy.requests.filter(QStringLiteral("/v1/gifs/search")).first().contains(QStringLiteral("q=cats")));

        // Back on the emoji tab the same field filters emojis again.
        QWidget *bar = findTabBar(picker);
        QTest::mouseClick(bar, Qt::LeftButton, {}, QPoint(bar->width() / 4, bar->height() / 2));
        QVERIFY(!picker.isGifTabActive());
        QCOMPARE(edit->placeholderText(), QStringLiteral("Search emojis…"));
        edit->setText(QStringLiteral("thumbs up"));
        QVERIFY(picker.visibleEmojis().contains(QStringLiteral("👍")));
        QVERIFY(picker.visibleEmojis().size() < 20);
    }

    void withoutKeyOffersConfiguration()
    {
        EmojiPicker picker;
        picker.setGifsAvailable(true);
        QSignalSpy configure(&picker, &EmojiPicker::giphyConfigureRequested);
        picker.popup(QPoint(10, 10));
        picker.showGifTab();
        QPushButton *button = nullptr;
        for (auto *b : picker.findChildren<QPushButton *>())
            if (b->text().startsWith(QStringLiteral("Configure API key"))) button = b;
        QVERIFY(button && button->isVisible());
        button->click();
        QCOMPARE(configure.size(), 1);
        QVERIFY(!picker.isVisible());
    }

    void keyStoreErrorIsShown()
    {
        EmojiPicker picker;
        picker.setGifsAvailable(true);
        connect(&picker, &EmojiPicker::giphyKeyRequested, &picker,
                [&picker] { picker.setGiphyApiKey(QString(), QStringLiteral("Key store unavailable")); });
        picker.popup(QPoint(10, 10));
        picker.showGifTab();
        bool shown = false;
        for (auto *l : picker.findChildren<QLabel *>())
            if (l->isVisible() && l->text() == QStringLiteral("Key store unavailable")) shown = true;
        QVERIFY(shown);
    }

    void reactionsLoseTheGifTab()
    {
        EmojiPicker picker;
        picker.setGifsAvailable(true);
        picker.popup(QPoint(10, 10));
        picker.showGifTab();
        QVERIFY(picker.isGifTabActive());
        picker.setGifsAvailable(false); // opened for a reaction
        QVERIFY(!picker.isGifTabActive());
        picker.popup(QPoint(10, 10));
        QVERIFY(!picker.isGifTabActive());
    }
};

QTEST_MAIN(EmojiGifTest)
#include "emoji_gif_test.moc"
