// Monospace styling must only be applied and detected when the user
// explicitly formatted the text. Regression test for the report that whole
// messages were sent as monospace when the system font itself is a
// fixed-pitch font: unformatted text inherits the system font, so the old
// detection compared the inherited family against the platform fixed font
// and matched every character. It also made the M button impossible to
// untoggle, because selectionHasStyle() kept reporting the style as present.
#include "MessageInput.h"

#include <QApplication>
#include <QFontDatabase>
#include <QJsonArray>
#include <QMainWindow>
#include <QTest>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QToolButton>

namespace {

QStringList styleStrings(const MessageInput &input)
{
    QStringList result;
    const QJsonArray styles = input.textStyles();
    for (const QJsonValue &v : styles)
        result.append(v.toString());
    return result;
}

bool hasMonospaceRange(const MessageInput &input, const QString &expected = QString())
{
    const QStringList styles = styleStrings(input);
    for (const QString &s : styles) {
        if (!s.endsWith(QLatin1String(":MONOSPACE"))) continue;
        if (expected.isEmpty() || s == expected) return true;
    }
    return false;
}

// Applies/removes monospace exactly like MessageInput::applyStyle() does
// (that method is private to the widget).
void setMonospace(MessageInput &input, bool on)
{
    QTextCursor cursor = input.textCursor();
    QTextCharFormat fmt;
    fmt.setFontFixedPitch(on);
    if (on) fmt.setFontFamilies({QFontDatabase::systemFont(QFontDatabase::FixedFont).family()});
    else fmt.setFontFamilies(QStringList());
    cursor.mergeCharFormat(fmt);
    input.setTextCursor(cursor);
}

} // namespace

class MessageStylesTest : public QObject
{
    Q_OBJECT

    QFont m_normalAppFont;

private slots:
    void initTestCase()
    {
        // Reproduce the reported environment: the application font is a
        // fixed-pitch font, so unformatted text's inherited family equals the
        // platform fixed font. Property-less char formats resolve their font
        // against the application font (QFont's default constructor), which
        // is what made the old detection match every character.
        m_normalAppFont = qApp->font();
        qApp->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    }

    void cleanupTestCase()
    {
        qApp->setFont(m_normalAppFont);
    }

    // The core regression: plain, unformatted text must not be sent with
    // MONOSPACE ranges, no matter what the system font is.
    void unformattedTextIsNotMonospace()
    {
        MessageInput input;
        input.setPlainText(QStringLiteral("ganz normale Nachricht"));
        QVERIFY2(!hasMonospaceRange(input),
                 qPrintable(QStringLiteral("unexpected styles: %1").arg(styleStrings(input).join(QLatin1Char(',')))
                            + QStringLiteral(" — plain text must not carry MONOSPACE")));
    }

    // Sanity check that textStyles() works at all: bold must still be picked up.
    void boldIsStillDetected()
    {
        MessageInput input;
        input.setPlainText(QStringLiteral("heavy text"));
        QTextCursor cursor = input.textCursor();
        cursor.setPosition(0);
        cursor.setPosition(5, QTextCursor::KeepAnchor);
        QTextCharFormat fmt;
        fmt.setFontWeight(QFont::Bold);
        cursor.mergeCharFormat(fmt);
        input.setTextCursor(cursor);

        QVERIFY(styleStrings(input).contains(QStringLiteral("0:5:BOLD")));
    }

    // Only the explicitly formatted range carries MONOSPACE, and removing the
    // style really removes it (this used to be stuck on fixed-pitch systems).
    void explicitMonospaceIsDetectedAndRemovable()
    {
        MessageInput input;
        input.setPlainText(QStringLiteral("code and prose"));
        QTextCursor cursor = input.textCursor();
        cursor.setPosition(0);
        cursor.setPosition(4, QTextCursor::KeepAnchor);
        input.setTextCursor(cursor);

        setMonospace(input, true);
        QVERIFY(hasMonospaceRange(input, QStringLiteral("0:4:MONOSPACE")));
        QVERIFY(!hasMonospaceRange(input, QStringLiteral("4:10:MONOSPACE")));

        setMonospace(input, false);
        QVERIFY2(!hasMonospaceRange(input),
                 qPrintable(QStringLiteral("styles after removal: %1").arg(styleStrings(input).join(QLatin1Char(',')))));
    }

    // Drafts and edited messages round-trip through HTML. The fixed-pitch
    // flag does not survive that trip, but the explicit font family does —
    // and it must still count as monospace.
    void monospaceSurvivesHtmlRoundTrip()
    {
        // The round-trip needs a default font whose family differs from the
        // platform fixed font by string, otherwise Qt's HTML exporter cannot
        // distinguish the monospace span from the document default and drops
        // the font-family declaration (inherent to toHtml(); only relevant on
        // systems whose default font is already fixed-pitch).
        qApp->setFont(m_normalAppFont);
        MessageInput input;
        QFont documentFont = input.font();
        if (documentFont.family() == QFontDatabase::systemFont(QFontDatabase::FixedFont).family())
            documentFont.setFamily(documentFont.family() + QStringLiteral(" Proportional"));
        input.setFont(documentFont);
        input.setPlainText(QStringLiteral("code and prose"));
        QTextCursor cursor = input.textCursor();
        cursor.setPosition(0);
        cursor.setPosition(4, QTextCursor::KeepAnchor);
        input.setTextCursor(cursor);
        setMonospace(input, true);

        input.setRichText(input.richText());
        QVERIFY(hasMonospaceRange(input, QStringLiteral("0:4:MONOSPACE")));
    }

    // End-to-end through the formatting bar: select text, click the M button,
    // verify the style is applied; click again to remove it. On systems with
    // a fixed-pitch system font the second click used to be a no-op because
    // the style was considered present even without any formatting.
    void formattingBarTogglesMonospace()
    {
        // The HTML round-trip test restored the regular font; this test needs
        // the fixed-pitch environment again (widgets pick it up on creation).
        qApp->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

        QMainWindow window;
        auto *input = new MessageInput;
        window.setCentralWidget(input);
        window.show();
        QVERIFY(QTest::qWaitForWindowActive(&window));

        input->setPlainText(QStringLiteral("select me"));
        QTextCursor cursor = input->textCursor();
        cursor.select(QTextCursor::Document);
        input->setTextCursor(cursor); // shows the formatting bar

        QToolButton *monoButton = nullptr;
        for (QToolButton *button : window.findChildren<QToolButton *>()) {
            if (button->text() == QLatin1String("M")) monoButton = button;
        }
        QVERIFY(monoButton);
        QVERIFY(monoButton->isVisible());

        QTest::mouseClick(monoButton, Qt::LeftButton);
        QVERIFY(hasMonospaceRange(*input, QStringLiteral("0:10:MONOSPACE")));

        QTest::mouseClick(monoButton, Qt::LeftButton);
        QVERIFY2(!hasMonospaceRange(*input),
                 qPrintable(QStringLiteral("styles after untoggling: %1").arg(styleStrings(*input).join(QLatin1Char(',')))));
    }
};

QTEST_MAIN(MessageStylesTest)
#include "message_styles_test.moc"
