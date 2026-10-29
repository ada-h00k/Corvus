#include "Envelope.h"
#include "Records.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

class MentionsTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesIncomingNativeMention()
    {
        const qint64 timestamp = 1'790'000'100'000LL;
        const QString placeholder(QChar(0xfffc));
        const QJsonObject envelope{
            {"sourceNumber", "+49111"},
            {"timestamp", QJsonValue(timestamp)},
            {"dataMessage", QJsonObject{
                {"timestamp", QJsonValue(timestamp)},
                {"message", placeholder + QStringLiteral(" hello")},
                {"mentions", QJsonArray{QJsonObject{
                    {"start", 0}, {"length", 1}, {"uuid", "00000000-0000-4000-8000-000000000001"}
                }}}
            }}
        };

        const ParsedEnvelope parsed = parseEnvelope(envelope);
        QCOMPARE(parsed.kind, ParsedEnvelope::Message_);
        QCOMPARE(parsed.msg.mentions.size(), 1);
        QCOMPARE(parsed.msg.mentions.first().start, 0);
        QCOMPARE(parsed.msg.mentions.first().length, 1);
        QCOMPARE(parsed.msg.mentions.first().uuid, QStringLiteral("00000000-0000-4000-8000-000000000001"));
    }

    void keepsPlaceholderWhenSignalCliOmitsMentionMetadata()
    {
        const qint64 timestamp = 1'790'000'100'001LL;
        const QString placeholder(QChar(0xfffc));
        const QJsonObject envelope{
            {"sourceNumber", "+49111"},
            {"timestamp", QJsonValue(timestamp)},
            {"dataMessage", QJsonObject{
                {"timestamp", QJsonValue(timestamp)},
                {"message", QStringLiteral("Hi ") + placeholder}
            }}
        };

        const ParsedEnvelope parsed = parseEnvelope(envelope);
        QCOMPARE(parsed.msg.mentions.size(), 1);
        QCOMPARE(parsed.msg.mentions.first().start, 3);
        QCOMPARE(parsed.msg.mentions.first().length, 1);
        QVERIFY(parsed.msg.mentions.first().uuid.isEmpty());
    }

    void mentionMetadataRoundTripsThroughEncryptedHistoryRecord()
    {
        Message message;
        message.time = QDateTime::fromMSecsSinceEpoch(1'790'000'100'002LL);
        message.sender = QStringLiteral("Me");
        message.authorKey = QStringLiteral("+49111");
        message.outgoing = true;
        message.text = QStringLiteral("Ada is here");
        message.mentions.append({0, 3, QStringLiteral("00000000-0000-4000-8000-000000000001")});

        const Message restored = Records::messageFromRecord(Records::message(QStringLiteral("g:group"), message));
        QCOMPARE(restored.mentions.size(), 1);
        QCOMPARE(restored.mentions.first().start, 0);
        QCOMPARE(restored.mentions.first().length, 3);
        QCOMPARE(restored.mentions.first().uuid, message.mentions.first().uuid);
    }

    void editRecordRetainsMentionMetadata()
    {
        Message message;
        message.time = QDateTime::fromMSecsSinceEpoch(1'790'000'100'003LL);
        message.authorKey = QStringLiteral("+49111");
        message.outgoing = true;
        message.text = QStringLiteral("Ada");
        message.editTs = message.ts() + 100;
        message.editCount = 1;
        message.mentions.append({0, 3, QStringLiteral("00000000-0000-4000-8000-000000000002")});

        Message restored = Records::messageFromRecord(Records::message(QStringLiteral("g:group"), message));
        // A follow-up edit record updates the current body and its mentions.
        Records::applyExtras(restored, Records::edit(QStringLiteral("g:group"), message).value(QStringLiteral("d")).toObject());
        QCOMPARE(restored.mentions.size(), 1);
        QCOMPARE(restored.mentions.first().uuid, message.mentions.first().uuid);
    }
};

QTEST_MAIN(MentionsTest)
#include "mentions_test.moc"
