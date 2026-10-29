#include "Envelope.h"
#include "Records.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

class LinkPreviewTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesIncomingHttpsPreview()
    {
        const qint64 timestamp = 1'790'000'100'000LL;
        const QJsonObject envelope{
            {"sourceNumber", "+49111"},
            {"sourceName", "Alice"},
            {"timestamp", QJsonValue(timestamp)},
            {"dataMessage", QJsonObject{
                {"timestamp", QJsonValue(timestamp)},
                {"message", "A useful page: https://example.org/article"},
                {"previews", QJsonArray{QJsonObject{
                    {"url", "https://example.org/article"},
                    {"title", "Example article"},
                    {"description", "A short description"},
                    {"image", QJsonObject{
                        {"id", "preview-image-id"},
                        {"contentType", "image/jpeg"},
                        {"filename", "preview.jpg"},
                        {"size", 2048}
                    }}
                }}}
            }}
        };

        const ParsedEnvelope parsed = parseEnvelope(envelope);
        QCOMPARE(parsed.kind, ParsedEnvelope::Message_);
        QVERIFY(parsed.msg.linkPreview.isValid());
        QCOMPARE(parsed.msg.linkPreview.url, QStringLiteral("https://example.org/article"));
        QCOMPARE(parsed.msg.linkPreview.title, QStringLiteral("Example article"));
        QCOMPARE(parsed.msg.linkPreview.description, QStringLiteral("A short description"));
        QCOMPARE(parsed.msg.linkPreview.imageId, QStringLiteral("preview-image-id"));
        QCOMPARE(parsed.msg.linkPreview.imageContentType, QStringLiteral("image/jpeg"));
        QCOMPARE(parsed.msg.linkPreview.imageSize, qint64(2048));
    }

    void rejectsNonHttpsPreview()
    {
        const qint64 timestamp = 1'790'000'100'001LL;
        const QJsonObject envelope{
            {"sourceNumber", "+49111"},
            {"timestamp", QJsonValue(timestamp)},
            {"dataMessage", QJsonObject{
                {"timestamp", QJsonValue(timestamp)},
                {"message", "http://example.org/article"},
                {"previews", QJsonArray{QJsonObject{
                    {"url", "http://example.org/article"},
                    {"title", "Must not be rendered"}
                }}}
            }}
        };

        const ParsedEnvelope parsed = parseEnvelope(envelope);
        QCOMPARE(parsed.kind, ParsedEnvelope::Message_);
        QVERIFY(!parsed.msg.linkPreview.isValid());
        QVERIFY(parsed.msg.linkPreview.title.isEmpty());
    }

    void persistsPreviewMetadataAndThumbnail()
    {
        Message message;
        message.time = QDateTime::fromMSecsSinceEpoch(1'790'000'100'002LL);
        message.authorKey = QStringLiteral("+49111");
        message.text = QStringLiteral("https://example.org/article");
        message.linkPreview.url = QStringLiteral("https://example.org/article");
        message.linkPreview.title = QStringLiteral("Example article");
        message.linkPreview.description = QStringLiteral("Description");
        message.linkPreview.imageId = QStringLiteral("preview-image-id");
        message.linkPreview.imageContentType = QStringLiteral("image/jpeg");
        message.linkPreview.imageFilename = QStringLiteral("preview.jpg");
        message.linkPreview.imageSize = 2048;
        message.linkPreview.imageThumb = QByteArrayLiteral("thumbnail-bytes");

        const QJsonObject record = Records::message(QStringLiteral("+49111"), message);
        const Message restored = Records::messageFromRecord(record);
        QVERIFY(restored.linkPreview.isValid());
        QCOMPARE(restored.linkPreview.url, message.linkPreview.url);
        QCOMPARE(restored.linkPreview.title, message.linkPreview.title);
        QCOMPARE(restored.linkPreview.description, message.linkPreview.description);
        QCOMPARE(restored.linkPreview.imageId, message.linkPreview.imageId);
        QCOMPARE(restored.linkPreview.imageThumb, message.linkPreview.imageThumb);
        QVERIFY(restored.linkPreview.imagePath.isEmpty());

        Message updated = restored;
        updated.linkPreview.imageThumb = QByteArrayLiteral("new-thumbnail");
        const QJsonObject update = Records::linkPreviewUpdate(QStringLiteral("+49111"), updated);
        Message applied = restored;
        Records::applyExtras(applied, update.value(QStringLiteral("d")).toObject());
        QCOMPARE(applied.linkPreview.imageThumb, QByteArrayLiteral("new-thumbnail"));
    }

    void editUpdateClearsOldPreview()
    {
        Message message;
        message.time = QDateTime::fromMSecsSinceEpoch(1'790'000'100'004LL);
        message.authorKey = QStringLiteral("+49111");
        message.linkPreview.url = QStringLiteral("https://example.org/old");
        message.linkPreview.title = QStringLiteral("Old preview");
        const QJsonObject update = Records::edit(QStringLiteral("+49111"), message);
        Message restored = message;
        Records::applyExtras(restored, update.value(QStringLiteral("d")).toObject());
        QVERIFY(!restored.linkPreview.isValid());
    }

    void viewOnceDoesNotKeepLinkPreview()
    {
        const qint64 timestamp = 1'790'000'100'003LL;
        const QJsonObject envelope{
            {"sourceNumber", "+49111"},
            {"timestamp", QJsonValue(timestamp)},
            {"dataMessage", QJsonObject{
                {"timestamp", QJsonValue(timestamp)},
                {"message", "https://example.org/article"},
                {"viewOnce", true},
                {"previews", QJsonArray{QJsonObject{
                    {"url", "https://example.org/article"},
                    {"title", "Example article"}
                }}},
                {"attachments", QJsonArray{QJsonObject{
                    {"id", "one-time-image"},
                    {"contentType", "image/jpeg"},
                    {"filename", "image.jpg"},
                    {"size", 10}
                }}}
            }}
        };

        const ParsedEnvelope parsed = parseEnvelope(envelope);
        QVERIFY(parsed.msg.viewOnce);
        QVERIFY(!parsed.msg.linkPreview.isValid());
    }
};

QTEST_MAIN(LinkPreviewTest)
#include "link_preview_test.moc"
