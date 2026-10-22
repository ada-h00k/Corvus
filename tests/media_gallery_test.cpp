// Tests for the media gallery: sorting of attachments/links into the four tabs
// and the dialog built on top of it.
#include "ImageViewer.h"
#include "MediaGallery.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QBuffer>
#include <QImage>
#include <QLocale>
#include <QPainter>
#include <QTabWidget>
#include <QTest>

namespace {

QByteArray pngBytes(const QColor &color, int w = 300, int h = 200)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(color);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

Attachment attachment(const QString &type, const QString &name, qint64 size = 1000)
{
    Attachment a;
    a.id = name;
    a.contentType = type;
    a.filename = name;
    a.size = size;
    return a;
}

Message message(const QDateTime &time, const QString &text = {}, const QList<Attachment> &atts = {})
{
    Message m;
    m.time = time;
    m.text = text;
    m.attachments = atts;
    return m;
}

QList<Message> sampleConversation()
{
    QList<Message> msgs;
    msgs << message(QDateTime(QDate(2026, 8, 3), QTime(10, 0)), QStringLiteral("old https://example.org/a."),
                    {attachment("application/pdf", "report.pdf")});
    msgs << message(QDateTime(QDate(2026, 9, 12), QTime(9, 30)), {},
                    {attachment("image/png", "a.png"), attachment("video/mp4", "clip.mp4"),
                     attachment("audio/aac", "voice.aac")});
    msgs << message(QDateTime(QDate(2026, 10, 5), QTime(18, 0)),
                    QStringLiteral("see https://signal.org/ and (https://example.com/x?y=1) and https://signal.org/"),
                    {attachment("application/octet-stream", "notes.webm"), attachment("image/gif", "fun.gif")});
    Message deleted = message(QDateTime(QDate(2026, 10, 6), QTime(8, 0)), QStringLiteral("https://gone.example/"),
                              {attachment("image/png", "gone.png")});
    deleted.deleted = true;
    msgs << deleted;
    return msgs;
}

} // namespace

class MediaGalleryTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates)); }

    void extractsLinks()
    {
        QCOMPARE(MediaGallery::extractLinks(QStringLiteral("no link here")), QStringList());
        QCOMPARE(MediaGallery::extractLinks(QStringLiteral("a https://a.org/x. b (http://b.org/y?z=1) c")),
                 (QStringList{"https://a.org/x", "http://b.org/y?z=1"}));
        // duplicates inside a message are listed once, other schemes are ignored
        QCOMPARE(MediaGallery::extractLinks(QStringLiteral("https://a.org https://a.org ftp://c.org")),
                 (QStringList{"https://a.org"}));
    }

    void sortsIntoCategories()
    {
        const MediaGallery::Collection c = MediaGallery::collect(sampleConversation());
        // image, video, gif; the deleted message is ignored
        QCOMPARE(c.media.size(), 3);
        // .webm without content type counts as audio, like in the chat
        QCOMPARE(c.audio.size(), 2);
        QCOMPARE(c.files.size(), 1);
        QCOMPARE(c.files[0].att.filename, QStringLiteral("report.pdf"));
        // one link from the first message, two distinct ones from the third
        QCOMPARE(c.links.size(), 3);
    }

    void newestFirst()
    {
        const MediaGallery::Collection c = MediaGallery::collect(sampleConversation());
        for (int i = 1; i < c.media.size(); ++i) QVERIFY(c.media[i - 1].time >= c.media[i].time);
        QCOMPARE(c.media.first().att.filename, QStringLiteral("fun.gif"));
        QCOMPARE(c.links.first().url, QStringLiteral("https://signal.org/"));
        QCOMPARE(c.links.last().url, QStringLiteral("https://example.org/a"));
        // order inside one message is kept
        QCOMPARE(c.media[1].att.filename, QStringLiteral("a.png"));
        QCOMPARE(c.media[2].att.filename, QStringLiteral("clip.mp4"));
    }

    void monthTitles()
    {
        QCOMPARE(MediaGallery::monthTitle(QDate(2026, 10, 22)), QStringLiteral("October 2026"));
    }

    void dialogShowsTabs()
    {
        QList<Message> msgs = sampleConversation();
        MediaGalleryHost host;
        host.messages = [&msgs] { return msgs; };
        host.fetch = [](const Attachment &, std::function<void(const QByteArray &, const QString &)> cb) {
            cb(pngBytes(Qt::darkCyan), QString());
        };
        MediaGalleryDialog dlg(QStringLiteral("Dylan"), host);
        dlg.show();
        QCOMPARE(dlg.count(MediaGalleryDialog::TabMedia), 3);
        QCOMPARE(dlg.count(MediaGalleryDialog::TabAudio), 2);
        QCOMPARE(dlg.count(MediaGalleryDialog::TabLinks), 3);
        QCOMPARE(dlg.count(MediaGalleryDialog::TabFiles), 1);

        auto *tabs = dlg.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 4);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Media"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Audio"));
        QCOMPARE(tabs->tabText(2), QStringLiteral("Links"));
        QCOMPARE(tabs->tabText(3), QStringLiteral("Files"));

        // new messages show up on reload
        msgs << message(QDateTime(QDate(2026, 10, 20), QTime(12, 0)), {}, {attachment("image/jpeg", "new.jpg")});
        dlg.reload();
        QCOMPARE(dlg.count(MediaGalleryDialog::TabMedia), 4);
    }

    void clickingAnImageOpensViewer()
    {
        QList<Message> msgs;
        msgs << message(QDateTime(QDate(2026, 10, 5), QTime(18, 0)), {}, {attachment("image/png", "a.png")});
        int fetches = 0;
        MediaGalleryHost host;
        host.messages = [&msgs] { return msgs; };
        host.fetch = [&fetches](const Attachment &, std::function<void(const QByteArray &, const QString &)> cb) {
            ++fetches;
            cb(pngBytes(Qt::darkMagenta), QString());
        };
        MediaGalleryDialog dlg(QStringLiteral("Dylan"), host);
        dlg.resize(780, 620);
        dlg.show();
        // no stored preview: the tile loads the image by itself
        QTRY_VERIFY(fetches >= 1);
        // the grid of the visible "Media" tab
        QAbstractScrollArea *view = nullptr;
        for (auto *candidate : dlg.findChildren<QAbstractScrollArea *>())
            if (candidate->isVisible()) view = candidate;
        QVERIFY(view);
        const int before = fetches;
        // first tile: below the month heading
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, QPoint(12 + 20, 38 + 20));
        QTRY_VERIFY(fetches > before);
        ImageViewer *viewer = nullptr;
        QTRY_VERIFY(([&] {
            for (QWidget *w : QApplication::topLevelWidgets())
                if (auto *v = qobject_cast<ImageViewer *>(w)) viewer = v;
            return viewer != nullptr;
        })());
        viewer->close();
    }

    void emptyConversationShowsNothing()
    {
        MediaGalleryHost host;
        host.messages = [] { return QList<Message>(); };
        MediaGalleryDialog dlg(QStringLiteral("Nobody"), host);
        for (int t = 0; t < 4; ++t) QCOMPARE(dlg.count(t), 0);
    }
};

QTEST_MAIN(MediaGalleryTest)
#include "media_gallery_test.moc"
