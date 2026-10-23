// Contact list order and Ctrl+Up / Ctrl+Down stepping.
#include "ConversationOrder.h"
#include "MessageInput.h"
#include "Model.h"
#include "Records.h"

#include <QAction>
#include <QApplication>
#include <QMainWindow>
#include <QTest>

using ConversationOrder::before;
using ConversationOrder::step;

class ConversationOrderTest : public QObject
{
    Q_OBJECT
private slots:
    void recentFirstByDefault()
    {
        const QDateTime older(QDate(2026, 10, 1), QTime(9, 0));
        const QDateTime newer(QDate(2026, 10, 2), QTime(9, 0));
        // "Zoe" wrote last, so she is above "Anna" although the name sorts later
        QVERIFY(before("Zoe", newer, "z", "Anna", older, "a", false));
        QVERIFY(!before("Anna", older, "a", "Zoe", newer, "z", false));
        // same time: by name
        QVERIFY(before("Anna", newer, "a", "Zoe", newer, "z", false));
        // chats without messages come last
        QVERIFY(before("Zoe", older, "z", "Anna", QDateTime(), "a", false));
    }

    void alphabeticalIgnoresActivity()
    {
        const QDateTime older(QDate(2026, 10, 1), QTime(9, 0));
        const QDateTime newer(QDate(2026, 10, 2), QTime(9, 0));
        QVERIFY(before("Anna", older, "a", "Zoe", newer, "z", true));
        QVERIFY(!before("Zoe", newer, "z", "Anna", older, "a", true));
        // not even an empty chat moves: its position depends on the name only
        QVERIFY(before("Anna", QDateTime(), "a", "Zoe", newer, "z", true));
        // identical names stay in a fixed order
        QVERIFY(before("Sam", older, "a", "Sam", older, "b", true));
        QVERIFY(!before("Sam", older, "b", "Sam", older, "a", true));
    }

    void alphabeticalIsCaseInsensitive()
    {
        QVERIFY(before("anna", QDateTime(), "1", "Bob", QDateTime(), "2", true));
        QVERIFY(before("Anna", QDateTime(), "1", "bob", QDateTime(), "2", true));
    }

    void senderClockDoesNotControlTimelineOrder()
    {
        Message previous;
        previous.time = QDateTime::fromMSecsSinceEpoch(4'000'000'000'000LL); // legacy message with a bad clock

        Message received;
        received.time = QDateTime::fromMSecsSinceEpoch(1'000); // sender clock is far in the past
        received.orderTs = previous.sortTs() + 1;

        QVERIFY(received.sortTs() > previous.sortTs());
        const QJsonObject record = Records::message("+49111", received);
        const Message restored = Records::messageFromRecord(record);
        QCOMPARE(restored.ts(), received.ts()); // protocol identity/time remains untouched
        QCOMPARE(restored.sortTs(), received.sortTs()); // local order survives history reload
    }

    void steppingMovesByOneAndDoesNotWrap()
    {
        QCOMPARE(step(2, 5, +1), 3);
        QCOMPARE(step(2, 5, -1), 1);
        QCOMPARE(step(0, 5, -1), -1); // already at the top
        QCOMPARE(step(4, 5, +1), -1); // already at the bottom
        QCOMPARE(step(0, 1, +1), -1);
    }

    void steppingWithoutCurrentRow()
    {
        QCOMPARE(step(-1, 5, +1), 0); // nothing selected: down starts at the top ...
        QCOMPARE(step(-1, 5, -1), 4); // ... and up at the bottom
        QCOMPARE(step(-1, 0, +1), -1);
        QCOMPARE(step(3, 0, -1), -1);
    }

    // The window-wide shortcut must also work while the message field has focus.
    void shortcutWorksInTheMessageField()
    {
        QMainWindow w;
        auto *input = new MessageInput;
        w.setCentralWidget(input);
        int up = 0, down = 0;
        for (const int delta : {-1, 1}) {
            auto *action = new QAction(&w);
            action->setShortcut(QKeySequence(Qt::CTRL | (delta < 0 ? Qt::Key_Up : Qt::Key_Down)));
            action->setShortcutContext(Qt::WindowShortcut);
            w.addAction(action);
            connect(action, &QAction::triggered, &w, [&, delta] { (delta < 0 ? up : down)++; });
        }
        w.show();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        input->setFocus();
        input->setPlainText(QStringLiteral("draft text\nsecond line"));

        QTest::keyClick(input, Qt::Key_Down, Qt::ControlModifier);
        QTest::keyClick(input, Qt::Key_Down, Qt::ControlModifier);
        QTest::keyClick(input, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(down, 2);
        QCOMPARE(up, 1);
        // the draft is untouched
        QCOMPARE(input->toPlainText(), QStringLiteral("draft text\nsecond line"));
        // plain arrow keys still move the cursor and do not switch chats
        QTest::keyClick(input, Qt::Key_Down);
        QCOMPARE(down, 2);
    }
};

QTEST_MAIN(ConversationOrderTest)
#include "conversation_order_test.moc"
