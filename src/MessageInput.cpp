#include "MessageInput.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QTextCursor>
#include <QTextDocument>

namespace {
constexpr int kCompactHeight = 40;
constexpr int kExpandedHeight = 140;
}

MessageInput::MessageInput(QWidget *parent)
    : QTextEdit(parent)
{
    setAcceptRichText(false);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMinimumHeight(kCompactHeight);
    setMaximumHeight(kCompactHeight);
}

void MessageInput::setExpanded(bool expanded)
{
    if (m_expanded == expanded) return;
    m_expanded = expanded;

    if (m_expanded) {
        setMinimumHeight(kExpandedHeight);
        setMaximumHeight(kExpandedHeight);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    } else {
        setMinimumHeight(kCompactHeight);
        setMaximumHeight(kCompactHeight);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    emit expandedChanged(m_expanded);
    updateGeometry();
}

void MessageInput::setCursorPosition(int position)
{
    QTextCursor cursor = textCursor();
    cursor.setPosition(qBound(0, position, toPlainText().size()));
    setTextCursor(cursor);
}

void MessageInput::setText(const QString &text)
{
    setPlainText(text);
}

QString MessageInput::text() const
{
    return toPlainText();
}

void MessageInput::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Up && e->modifiers() == Qt::ShiftModifier) {
        selectAll();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Up && e->modifiers() == Qt::NoModifier) {
        if (!toPlainText().isEmpty()) {
            QTextCursor cursor = textCursor();
            cursor.movePosition(QTextCursor::Start);
            setTextCursor(cursor);
            e->accept();
            return;
        }
        emit upPressed();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Down && e->modifiers() == Qt::NoModifier && !toPlainText().isEmpty()) {
        QTextCursor cursor = textCursor();
        cursor.movePosition(QTextCursor::End);
        setTextCursor(cursor);
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        emit escapePressed();
        e->accept();
        return;
    }

    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        && !m_expanded
        && e->modifiers() == Qt::NoModifier) {
        emit returnPressed();
        e->accept();
        return;
    }

    // In expanded mode Enter inserts a newline. In compact mode Shift+Enter
    // also inserts a newline, matching the usual chat composer convention.
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        && !m_expanded
        && e->modifiers() == Qt::ShiftModifier) {
        QTextEdit::keyPressEvent(e);
        return;
    }

    // Ctrl+V, Shift+Insert, Cmd+V ...
    if (e->matches(QKeySequence::Paste) && m_hook && m_hook(QApplication::clipboard()->mimeData())) {
        e->accept();
        return;
    }

    QTextEdit::keyPressEvent(e);
}

void MessageInput::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu *menu = createStandardContextMenu();
    const QMimeData *md = QApplication::clipboard()->mimeData();
    if (m_hook && md && (md->hasImage() || md->hasUrls())) {
        QAction *first = menu->actions().isEmpty() ? nullptr : menu->actions().first();
        QAction *attach = new QAction(tr("Attach from clipboard"), menu);
        connect(attach, &QAction::triggered, this, [this] {
            m_hook(QApplication::clipboard()->mimeData());
        });
        if (first) {
            menu->insertAction(first, attach);
            menu->insertSeparator(first);
        } else {
            menu->addAction(attach);
        }
    }
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(e->globalPos());
}
