#pragma once

#include <QLineEdit>
#include <functional>

class QMimeData;

// Message input line that lets the application take over "paste" when the
// clipboard holds an image or files (screenshots!).
//
// A plain QShortcut cannot do this: QLineEdit claims the standard Paste key
// sequences itself (ShortcutOverride), so the shortcut would never fire.
class MessageInput : public QLineEdit
{
    Q_OBJECT
public:
    // Returns true if the hook consumed the clipboard content.
    using PasteHook = std::function<bool(const QMimeData *)>;

    explicit MessageInput(QWidget *parent = nullptr) : QLineEdit(parent) {}
    void setPasteHook(PasteHook hook) { m_hook = std::move(hook); }

signals:
    void escapePressed(); // used to cancel editing

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    PasteHook m_hook;
};
