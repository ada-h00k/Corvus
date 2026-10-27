#pragma once

#include <QTextEdit>
#include <functional>
#include <QJsonArray>
#include <QVector>

class QEvent;
class QMimeData;
class QMouseEvent;
class QMoveEvent;
class QResizeEvent;
class QFrame;
class QToolButton;

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
    QString richText() const { return toHtml(); }
    void setRichText(const QString &html);

signals:
    void escapePressed(); // used to cancel editing
    void upPressed();
    void returnPressed(); // Enter in compact mode
    void expandedChanged(bool expanded);
    void formattingChanged();

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
    PasteHook m_hook;
    bool m_expanded = false;
    bool m_handlingKeyPress = false;
};
