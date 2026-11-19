#include <QtTest>

#include "SpellChecker.h"

// The word splitting does not need a dictionary, so this runs everywhere. It makes sure that
// only prose is handed to Hunspell: no addresses, mentions, identifiers or abbreviations.
class SpellCheckerTest : public QObject
{
    Q_OBJECT
private:
    static QStringList words(const QString &text)
    {
        QStringList out;
        for (const auto &w : SpellChecker::checkableWords(text))
            out << text.mid(w.first, w.second);
        return out;
    }

private slots:
    void plainWords()
    {
        QCOMPARE(words(QStringLiteral("Das ist ein Test.")),
                 QStringList({QStringLiteral("Das"), QStringLiteral("ist"), QStringLiteral("ein"), QStringLiteral("Test")}));
    }

    void umlautsAndApostrophes()
    {
        QCOMPARE(words(QStringLiteral("Größe don't it’s E-Mail")),
                 QStringList({QStringLiteral("Größe"), QStringLiteral("don't"), QStringLiteral("it’s"), QStringLiteral("E-Mail")}));
        // Quotes around a word are not part of it.
        QCOMPARE(words(QStringLiteral("'quoted' -dash-")),
                 QStringList({QStringLiteral("quoted"), QStringLiteral("dash")}));
    }

    void notProse()
    {
        QCOMPARE(words(QStringLiteral("https://example.org/path www.example.org a.b@example.org")), QStringList());
        QCOMPARE(words(QStringLiteral("abc123 123abc snake_case @name #tag a/b file.txt")), QStringList());
        QCOMPARE(words(QStringLiteral("USB HTTP x")), QStringList()); // abbreviations, single letters
    }

    void sentenceBeforeUrl()
    {
        QCOMPARE(words(QStringLiteral("Schau hier: https://example.org danke")),
                 QStringList({QStringLiteral("Schau"), QStringLiteral("hier"), QStringLiteral("danke")}));
    }

    void wordAtPosition()
    {
        const QString text = QStringLiteral("Hallo schöne Welt");
        int start = -1, length = -1;
        QVERIFY(SpellChecker::wordAt(text, 8, &start, &length));
        QCOMPARE(text.mid(start, length), QStringLiteral("schöne"));
        QVERIFY(SpellChecker::wordAt(text, 12, &start, &length)); // right behind the word
        QCOMPARE(text.mid(start, length), QStringLiteral("schöne"));
        QVERIFY(!SpellChecker::wordAt(QStringLiteral("   "), 1, &start, &length));
    }

    void withoutDictionaryEverythingIsCorrect()
    {
        SpellChecker checker;
        checker.setLanguage(QStringLiteral("xx_XX")); // does not exist
        if (!checker.isReady()) {
            QVERIFY(checker.isCorrect(QStringLiteral("Qwertzuiop")));
            QVERIFY(checker.suggestions(QStringLiteral("Qwertzuiop")).isEmpty());
        }
    }
};

QTEST_MAIN(SpellCheckerTest)
#include "spellchecker_test.moc"
