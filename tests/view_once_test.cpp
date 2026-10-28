#include "Envelope.h"
#include "Records.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

class ViewOnceTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesIncomingViewOncePhoto()
    {
        const qint64 timestamp = 1'790'000'000'000LL;
        const QJsonObject envelope{
            {"sourceNumber", "+49111"},
            {"sourceName", "Alice"},
            {"timestamp", QJsonValue(timestamp)},
            {"dataMessage", QJsonObject{
                {"timestamp", QJsonValue(timestamp)},
                {"message", ""},
                {"viewOnce", true},
                {"attachments", QJsonArray{QJsonObject{
                    {"id", "one-time-attachment"},
                    {"contentType", "image/jpeg"},
                    {"filename", "signal-attachment.jpg"},
                    {"size", 1280}
                }}}
            }}
        };

        const ParsedEnvelope parsed = parseEnvelope(envelope);
        QCOMPARE(parsed.kind, ParsedEnvelope::Message_);
        QVERIFY(!parsed.outgoing);
        QVERIFY(parsed.msg.viewOnce);
        QCOMPARE(parsed.msg.attachments.size(), 1);
        QCOMPARE(parsed.msg.attachments.first().id, QStringLiteral("one-time-attachment"));
    }

    void parsesViewOnceSyncFromOwnDevice()
    {
        const qint64 timestamp = 1'790'000'000'001LL;
        const QJsonObject envelope{
            {"timestamp", QJsonValue(timestamp)},
            {"syncMessage", QJsonObject{
                {"sentMessage", QJsonObject{
                    {"destinationNumber", "+49111"},
                    {"timestamp", QJsonValue(timestamp)},
                    {"message", ""},
                    {"viewOnce", true},
                    {"attachments", QJsonArray{QJsonObject{
                        {"id", "synced-one-time-attachment"},
                        {"contentType", "image/png"},
                        {"filename", "signal-attachment.png"},
                        {"size", 1024}
                    }}}
                }}
            }}
        };

        const ParsedEnvelope parsed = parseEnvelope(envelope);
        QCOMPARE(parsed.kind, ParsedEnvelope::Message_);
        QVERIFY(parsed.outgoing);
        QVERIFY(parsed.msg.viewOnce);
        QCOMPARE(parsed.msg.attachments.size(), 1);
    }

    void unopenedIncomingPhotoPersistsOnlyAReference()
    {
        Message message;
        message.time = QDateTime::fromMSecsSinceEpoch(1'790'000'000'010LL);
        message.sender = QStringLiteral("Alice");
        message.authorKey = QStringLiteral("+49111");
        message.viewOnce = true;

        Attachment attachment;
        attachment.id = QStringLiteral("opaque-signal-id");
        attachment.contentType = QStringLiteral("image/jpeg");
        attachment.filename = QStringLiteral("signal-attachment.jpg");
        attachment.path = QStringLiteral("/tmp/should-not-be-stored.jpg");
        attachment.thumb = QByteArrayLiteral("cached-preview");
        attachment.size = 42;
        message.attachments.append(attachment);

        const QJsonObject record = Records::message(QStringLiteral("+49111"), message);
        const QJsonObject extras = record.value(QStringLiteral("d")).toObject();
        QVERIFY(extras.value(QStringLiteral("vo")).toBool());
        const QJsonArray storedAttachments = extras.value(QStringLiteral("at")).toArray();
        QCOMPARE(storedAttachments.size(), 1);
        const QJsonObject stored = storedAttachments.first().toObject();
        QCOMPARE(stored.value(QStringLiteral("id")).toString(), QStringLiteral("opaque-signal-id"));
        QVERIFY(!stored.contains(QStringLiteral("p")));
        QVERIFY(!stored.contains(QStringLiteral("th")));

        const Message restored = Records::messageFromRecord(record);
        QVERIFY(restored.viewOnce);
        QCOMPARE(restored.attachments.size(), 1);
        QCOMPARE(restored.attachments.first().id, QStringLiteral("opaque-signal-id"));
        QVERIFY(restored.attachments.first().path.isEmpty());
        QVERIFY(restored.attachments.first().thumb.isEmpty());
    }

    void sentAndConsumedMediaAreNotPersisted()
    {
        Message sent;
        sent.time = QDateTime::fromMSecsSinceEpoch(1'790'000'000'020LL);
        sent.outgoing = true;
        sent.authorKey = QStringLiteral("+491234567");
        sent.viewOnce = true;
        Attachment local;
        local.id = QStringLiteral("outgoing-id");
        local.path = QStringLiteral("/home/test/photo.png");
        local.thumb = QByteArrayLiteral("preview");
        sent.attachments.append(local);

        QJsonObject sentRecord = Records::message(QStringLiteral("+49111"), sent);
        QVERIFY(sentRecord.value(QStringLiteral("d")).toObject().value(QStringLiteral("vo")).toBool());
        QVERIFY(!sentRecord.value(QStringLiteral("d")).toObject().contains(QStringLiteral("at")));
        QVERIFY(Records::messageFromRecord(sentRecord).attachments.isEmpty());

        Message consumed = sent;
        consumed.outgoing = false;
        consumed.viewOnceViewed = true;
        QJsonObject consumedRecord = Records::message(QStringLiteral("+49111"), consumed);
        const QJsonObject consumedExtras = consumedRecord.value(QStringLiteral("d")).toObject();
        QVERIFY(consumedExtras.value(QStringLiteral("vv")).toBool());
        QVERIFY(!consumedExtras.contains(QStringLiteral("at")));
    }

    void consumedUpdateClearsAnAlreadyStoredReference()
    {
        Message message;
        message.time = QDateTime::fromMSecsSinceEpoch(1'790'000'000'030LL);
        message.authorKey = QStringLiteral("+49111");
        message.viewOnce = true;
        Attachment attachment;
        attachment.id = QStringLiteral("one-time-id");
        attachment.contentType = QStringLiteral("image/png");
        message.attachments.append(attachment);

        message.viewOnceViewed = true;
        message.attachments.clear();
        const QJsonObject update = Records::viewOnceUpdate(QStringLiteral("+49111"), message);
        const QJsonObject extras = update.value(QStringLiteral("d")).toObject();
        QVERIFY(extras.value(QStringLiteral("vv")).toBool());
        QVERIFY(extras.value(QStringLiteral("at")).toArray().isEmpty());

        Message restored;
        restored.time = message.time;
        restored.authorKey = message.authorKey;
        restored.viewOnce = true;
        restored.attachments.append(attachment);
        Records::applyExtras(restored, extras);
        QVERIFY(restored.viewOnceViewed);
        QVERIFY(restored.attachments.isEmpty());
    }

    void deletedViewOnceIsNoLongerTreatedAsViewOnce()
    {
        Message restored;
        restored.time = QDateTime::fromMSecsSinceEpoch(1'790'000'000'040LL);
        restored.viewOnce = true;
        restored.viewOnceViewed = true;
        Records::applyExtras(restored, QJsonObject{{"dl", true}});
        QVERIFY(restored.deleted);
        QVERIFY(!restored.viewOnce);
        QVERIFY(!restored.viewOnceViewed);
        QVERIFY(restored.attachments.isEmpty());
    }
};

QTEST_MAIN(ViewOnceTest)
#include "view_once_test.moc"
