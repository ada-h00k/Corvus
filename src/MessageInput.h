#pragma once

#include <QTextEdit>
#include <functional>
#include <QJsonArray>
#include <QVector>

#include "Model.h"

class QEvent;
class QMimeData;
class QMenu;
class QAction;
class QMouseEvent;
class QMoveEvent;
class QResizeEvent;
class QFrame;
class QToolButton;
class SpellChecker;
class SpellHighlighter;

// Message input that supports a compact "Enter to send" mode and an expanded
// multi-line mode. It also lets the application take over paste when the
// clipboard holds an image or files (screenshots!).
class MessageInput : public QTextEdit
{
    Q_OBJECT
public:
    using PasteHook = std::function<bool(const QMimeData *)>;

    explicit MessageInput(QWidget *parent = nullptr);

    void setPasteHook(PasteHook hook) { m_hook = std::move(hook); }
    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded);
    void setCursorPosition(int position);
    void clear();
    void setText(const QString &text);
    QString text() const;
    QJsonArray textStyles() const;
    QList<MentionRange> mentionRanges() const;
    void insertMention(int start, int end, const QString &recipientId, const QString &displayName);
    void applyMentionFormat(int start, int length, const QString &recipientId);
    QString richText() const { return toHtml(); }
    void setRichText(const QString &html);

    // Spell checking: misspelled words get a wavy underline, the context menu offers
    // suggestions. The checker is owned by the caller; null or `false` switches it off.
    void setSpellChecker(SpellChecker *checker);
    void setSpellCheckEnabled(bool enabled);
    bool spellCheckEnabled() const;

signals:
    void escapePressed(); // used to cancel editing
    void upPressed();
    void returnPressed(); // Enter in compact mode
    void expandedChanged(bool expanded);
    void formattingChanged();
    // Chosen in the "Spelling" part of the context menu; the owner stores the setting.
    void spellCheckEnableRequested(bool enable);
    void spellLanguageRequested(const QString &code); // "" = automatic

protected:
    void changeEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void moveEvent(QMoveEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    enum Style { Bold, Italic, Strike, Monospace, Spoiler };
    void showFormattingBar();
    void hideFormattingBar();
    void applyStyle(Style style);
    bool selectionHasStyle(Style style) const;
    void positionFormattingBar();
    void updateFormattingBarTheme();
    QString styleName(Style style) const;
    QFrame *m_formatBar = nullptr;
    QVector<QToolButton *> m_formatButtons;
    void addSpellingActions(QMenu *menu, QAction *before, const QPoint &viewportPos);
    SpellChecker *m_spell = nullptr;
    SpellHighlighter *m_highlighter = nullptr;
    PasteHook m_hook;
    bool m_expanded = false;
    bool m_handlingKeyPress = false;
};
