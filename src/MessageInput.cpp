#include "MessageInput.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>

void MessageInput::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Up && e->modifiers() == Qt::ShiftModifier) {
        selectAll();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Up && e->modifiers() == Qt::NoModifier) {
        emit upPressed();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        emit escapePressed();
        e->accept();
        return;
    }
    // Ctrl+V, Shift+Insert, Cmd+V ...
    if (e->matches(QKeySequence::Paste) && m_hook && m_hook(QApplication::clipboard()->mimeData())) {
        e->accept();
        return;
    }
    QLineEdit::keyPressEvent(e);
}

void MessageInput::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu *menu = createStandardContextMenu();
    const QMimeData *md = QApplication::clipboard()->mimeData();
    if (m_hook && md && (md->hasImage() || md->hasUrls())) {
        QAction *first = menu->actions().isEmpty() ? nullptr : menu->actions().first();
        QAction *attach = new QAction(tr("Attach from clipboard"), menu);
        connect(attach, &QAction::triggered, this, [this] { m_hook(QApplication::clipboard()->mimeData()); });
        menu->insertAction(first, attach);
        menu->insertSeparator(first);
    }
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(e->globalPos());
}
