#pragma once

#include <QTextEdit>
#include <functional>

class QMimeData;

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
    void setText(const QString &text);
    QString text() const;

signals:
    void escapePressed(); // used to cancel editing
    void upPressed();
    void returnPressed(); // Enter in compact mode
    void expandedChanged(bool expanded);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    PasteHook m_hook;
    bool m_expanded = false;
};
