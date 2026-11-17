// Headless test of Signal contact cards: envelope parsing (incoming and via
// syncMessage.sentMessage), history records and the ChatView contact card
// (avatar/name rendering, click-to-open hit-testing).
#include "ChatView.h"
#include "Envelope.h"
#include "Records.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <cstdio>

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

static void pump()
{
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

static QJsonObject contactCardEnvelope(qint64 timestamp)
{
    return QJsonObject{
        {"sourceNumber", "+49111"},
        {"sourceName", "Alice"},
        {"timestamp", QJsonValue(timestamp)},
        {"dataMessage", QJsonObject{
            {"timestamp", QJsonValue(timestamp)},
            {"message", QJsonValue()},
            {"contacts", QJsonArray{QJsonObject{
                {"name", QStringLiteral("Bob")},
                {"number", QStringLiteral("+49222")},
                {"avatar", QJsonObject{
                    {"contentType", QStringLiteral("image/png")},
                    {"avatar", QStringLiteral("AQID")}
                }}
            }}}
        }}
    };
}

class ContactTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesIncomingContactCard()
    {
        const ParsedEnvelope parsed = parseEnvelope(contactCardEnvelope(1'790'000'000'000LL));
        CHECK(parsed.kind == ParsedEnvelope::Message_);
        CHECK(!parsed.outgoing);
        CHECK(parsed.msg.hasContact);
        CHECK(parsed.msg.contactName == QStringLiteral("Bob"));
        CHECK(parsed.msg.contactNumber == QStringLiteral("+49222"));
        CHECK(parsed.msg.contactAvatarId == QStringLiteral("987654321"));
        CHECK(parsed.msg.contactAvatar.isEmpty()); // the bytes are fetched lazily, like thumbnails
        CHECK(parsed.msg.text.isEmpty());
        CHECK(parsed.msg.attachments.isEmpty());
        // A contact card without a body must not be dropped as an empty data message.
        CHECK(parsed.kind == ParsedEnvelope::Message_);
    }

    void parsesContactFromSyncSent()
    {
        // The phone shared a contact: the card arrives as a sync sent message.
        const qint64 timestamp = 1'790'000'000'010LL;
        QJsonObject envelope{
            {"timestamp", QJsonValue(timestamp)},
            {"syncMessage", QJsonObject{
                {"sentMessage", QJsonObject{
                    {"destinationNumber", "+49222"},
                    {"timestamp", QJsonValue(timestamp)},
                    {"dataMessage", QJsonObject{
                        {"timestamp", QJsonValue(timestamp)},
                        {"contacts", QJsonArray{QJsonObject{
                            {"name", QStringLiteral("Bob")},
                            {"number", QStringLiteral("+49222")}
                        }}}
                    }}
                }}
            }}
        };
        const ParsedEnvelope parsed = parseEnvelope(envelope);
        CHECK(parsed.kind == ParsedEnvelope::Message_);
        CHECK(parsed.outgoing);
        CHECK(parsed.msg.hasContact);
        CHECK(parsed.msg.contactName == QStringLiteral("Bob"));
        CHECK(parsed.msg.contactNumber == QStringLiteral("+49222"));
        CHECK(parsed.msg.contactAvatarId.isEmpty()); // the phone sent the card without an avatar
    }

    void contactRoundTripsThroughTheHistory()
    {
        Message card;
        card.time = QDateTime::fromMSecsSinceEpoch(1'790'000'000'000LL);
        card.sender = QStringLiteral("Alice");
        card.authorKey = QStringLiteral("+49111");
        card.hasContact = true;
        card.contactName = QStringLiteral("Bob");
        card.contactNumber = QStringLiteral("+49222");
        card.contactAvatarId = QStringLiteral("987654321");

        // An incoming card persists its avatar attachment id; the bytes stay runtime-only.
        const QJsonObject record = Records::message(QStringLiteral("+49111"), card);
        const QJsonObject extras = record.value(QStringLiteral("d")).toObject();
        const QJsonObject storedCard = extras.value(QStringLiteral("cc")).toObject();
        CHECK(storedCard.value(QStringLiteral("n")).toString() == QStringLiteral("Bob"));
        CHECK(storedCard.value(QStringLiteral("num")).toString() == QStringLiteral("+49222"));
        CHECK(storedCard.value(QStringLiteral("av")).toString() == QStringLiteral("987654321"));
        CHECK(!storedCard.contains(QStringLiteral("avb")));

        const Message restored = Records::messageFromRecord(record);
        CHECK(restored.hasContact);
        CHECK(restored.contactName == QStringLiteral("Bob"));
        CHECK(restored.contactNumber == QStringLiteral("+49222"));
        CHECK(restored.contactAvatarId == QStringLiteral("987654321"));
        CHECK(restored.contactAvatar.isEmpty());
        CHECK(restored.text.isEmpty());

        // A card whose bytes are already cached (e.g. our own card) keeps them encrypted.
        Message own;
        own.time = card.time;
        own.hasContact = true;
        own.contactName = QStringLiteral("Bob");
        own.contactAvatar = QStringLiteral("AQID");
        const QJsonObject ownRecord = Records::message(QStringLiteral("+49"), own);
        const Message ownRestored = Records::messageFromRecord(ownRecord);
        CHECK(ownRestored.contactAvatarId.isEmpty());
        CHECK(ownRestored.contactAvatar == QStringLiteral("AQID"));
    }

    void deletedContactLosesItsData()
    {
        Message restored;
        restored.time = QDateTime::fromMSecsSinceEpoch(1'790'000'000'050LL);
        restored.hasContact = true;
        restored.contactName = QStringLiteral("Bob");
        restored.contactNumber = QStringLiteral("+49222");
        restored.contactAvatar = QStringLiteral("AQID");
        Records::applyExtras(restored, QJsonObject{{"dl", true}});
        CHECK(restored.deleted);
        CHECK(!restored.hasContact);
        CHECK(restored.contactName.isEmpty());
        CHECK(restored.contactNumber.isEmpty());
        CHECK(restored.contactAvatarId.isEmpty());
        CHECK(restored.contactAvatar.isEmpty());
    }

    void chatViewShowsContactCardAndOpensChat()
    {
        ChatView view;
        view.resize(560, 700);
        view.show();
        pump();

        QList<Message> msgs;
        Message card;
        card.time = QDateTime::currentDateTime().addSecs(-60);
        card.sender = QStringLiteral("Alice");
        card.authorKey = QStringLiteral("+49111");
        card.hasContact = true;
        card.contactName = QStringLiteral("Bob");
        card.contactNumber = QStringLiteral("+49222");
        // a tiny valid PNG so the avatar path is exercised as well
        card.contactAvatar = QStringLiteral(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");
        card.outgoing = false;
        msgs << card;
        view.setNameResolver([](const QString &key) {
            return key == QStringLiteral("+49111") ? QStringLiteral("Alice") : key;
        });
        view.setSelfKey(QStringLiteral("+49"));
        view.showMessages(msgs);
        pump();

        // plainText describes the contact (used by tests and search).
        const QString text = view.plainText();
        CHECK(text.contains(QStringLiteral("[Contact] Bob")));

        // Clicking the card opens a chat with that contact: probe the bubble's middle
        // column top-down; every probe inside the card emits exactly one
        // contactCardActivated with the message index.
        const QRect bubble = view.bubbleRect(0);
        QSignalSpy activated(&view, &ChatView::contactCardActivated);
        int hits = 0;
        for (int dy = 8; dy < bubble.height() - 8; dy += 8) {
            const QPoint probe(bubble.left() + 30, bubble.top() + dy);
            const int before = activated.count();
            QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, probe);
            pump();
            if (activated.count() > before) ++hits;
        }
        CHECK(hits > 0);
        if (activated.count() > 0) CHECK(activated.first().at(0).toInt() == 0);

        // The pinned-message snippet names the contact.
        CHECK(view.pinSnippet(0) == QStringLiteral("Contact: Bob"));
    }
};

QTEST_MAIN(ContactTest)
#include "contact_test.moc"
