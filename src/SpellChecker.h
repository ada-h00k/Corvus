#pragma once

#include <QObject>
#include <QStringList>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include <functional>
#include <memory>

// Spell checking for the message field, based on Hunspell.
//
// Hunspell is optional: without it (or without any installed dictionary)
// available() / isReady() are false and every word counts as correct, so the
// rest of the application does not need to care.
//
// Dictionaries are the usual Hunspell pairs (<lang>.dic + <lang>.aff). They are
// looked up in DICPATH, in the "dictionaries" folder of Corvus' data directory
// (a place for the user's own dictionaries, also inside the Flatpak sandbox), in
// <XDG data dirs>/hunspell|myspell and in the usual system folders.
class SpellChecker : public QObject
{
    Q_OBJECT
public:
    explicit SpellChecker(QObject *parent = nullptr);
    ~SpellChecker() override;

    // Built with Hunspell support?
    static bool available();
    // Codes ("de_DE", "en_US", …) of all installed dictionaries, sorted by title.
    static QStringList availableLanguages();
    // "Deutsch (Deutschland)" – the name of a dictionary in its own language.
    static QString languageTitle(const QString &code);

    // The dictionary to use; empty = pick one that matches the system language.
    void setLanguage(const QString &code);
    // The dictionary that is actually loaded ("" if none).
    QString language() const;
    // What was asked for ("" = automatic).
    QString requestedLanguage() const;
    bool isReady() const;

    bool isCorrect(const QString &word) const;
    QStringList suggestions(const QString &word, int max = 6) const;

    // Words the user added ("Add to dictionary") or ignores until restart.
    void addToDictionary(const QString &word);
    void ignoreWord(const QString &word);

    // Finds the word that contains `position` in `text`. Returns false if there is none.
    static bool wordAt(const QString &text, int position, int *start, int *length);
    // Splits `text` into the words that should be checked (start, length).
    static QList<QPair<int, int>> checkableWords(const QString &text);

signals:
    // The dictionary or the word lists changed: re-check what is shown.
    void changed();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Underlines misspelled words with a red wavy line. The marks are display-only
// (QTextLayout formats), they never change the text or its formatting.
class SpellHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
public:
    // `skip` tells for a character format whether the text must not be checked
    // (mentions, monospace text).
    using SkipFormat = std::function<bool(const QTextCharFormat &)>;

    SpellHighlighter(QTextDocument *document, SpellChecker *checker);

    void setSkipFormat(SkipFormat skip) { m_skip = std::move(skip); }
    void setActive(bool active);
    bool isActive() const { return m_active; }
    // The word that ends at the cursor is still being typed: do not mark it yet.
    void setCursorPosition(int position);

protected:
    void highlightBlock(const QString &text) override;

private:
    SpellChecker *m_checker;
    SkipFormat m_skip;
    bool m_active = false;
    int m_cursor = -1;
};
