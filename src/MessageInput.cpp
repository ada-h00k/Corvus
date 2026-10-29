#include "MessageInput.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QFrame>
#include <QHBoxLayout>
#include <QToolButton>
#include <QFontDatabase>
#include <QPalette>
#include <QSignalBlocker>
#include <QResizeEvent>
#include <QMoveEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextCharFormat>
#include <QEvent>

namespace {
constexpr int kCompactHeight = 40;
constexpr int kExpandedHeight = 140;
constexpr int kMentionUuidProperty = QTextFormat::UserProperty + 2;
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
    setMouseTracking(true);
    connect(this, &QTextEdit::selectionChanged, this, [this] {
        if (textCursor().hasSelection()) showFormattingBar();
        else hideFormattingBar();
        emit formattingChanged();
    });
    connect(this, &QTextEdit::cursorPositionChanged, this, [this] {
        // Formatting belongs to the selected range only. When the caret moves
        // away from a selection, reset the insertion format so typing beside a
        // formatted range does not inherit its style. Do not do this mid-key
        // event: replacing a selected range should use that range's format for
        // the replacement text.
        if (!m_handlingKeyPress && !textCursor().hasSelection())
            setCurrentCharFormat(QTextCharFormat());
        emit formattingChanged();
    });

    m_formatBar = new QFrame(nullptr);
    m_formatBar->setObjectName(QStringLiteral("messageFormattingBar"));
    m_formatBar->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    m_formatBar->setAttribute(Qt::WA_ShowWithoutActivating);
    m_formatBar->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    auto *layout = new QHBoxLayout(m_formatBar);
    layout->setContentsMargins(3, 2, 3, 2);
    layout->setSpacing(1);
    const QStringList labels = {QStringLiteral("B"), QStringLiteral("I"), QStringLiteral("S"),
                                QStringLiteral("M"), QStringLiteral("◩")};
    for (int i = 0; i < labels.size(); ++i) {
        auto *button = new QToolButton(m_formatBar);
        button->setText(labels.at(i));
        QFont buttonFont = button->font();
        if (i == Bold) buttonFont.setBold(true);
        if (i == Italic) buttonFont.setItalic(true);
        if (i == Strike) buttonFont.setStrikeOut(true);
        if (i == Monospace) buttonFont.setFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
        button->setFont(buttonFont);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(QStringList{tr("Bold"), tr("Italic"), tr("Strikethrough"),
                                       tr("Monospace"), tr("Spoiler")}.at(i));
        layout->addWidget(button);
        m_formatButtons.append(button);
        connect(button, &QToolButton::clicked, this, [this, i] { applyStyle(static_cast<Style>(i)); });
    }
    updateFormattingBarTheme();
    m_formatBar->hide();
    connect(this, &QObject::destroyed, m_formatBar, &QObject::deleteLater);
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
    if (!cursor.hasSelection()) setCurrentCharFormat(QTextCharFormat());
}

void MessageInput::clear()
{
    // QTextEdit::clear() can retain the current insertion format if the cursor
    // was not at the start of the document. Reset it so the next message starts
    // unformatted, regardless of the previous message's selected styles.
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::Start);
    setTextCursor(cursor);
    QTextEdit::clear();
    setCurrentCharFormat(QTextCharFormat());
    for (QToolButton *button : m_formatButtons)
        button->setChecked(false);
    hideFormattingBar();
    emit formattingChanged();
}

void MessageInput::setText(const QString &text)
{
    // Loading a plain-text draft or an edited message must not inherit a style
    // from whatever was selected in the previous composer contents.
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::Start);
    setTextCursor(cursor);
    setPlainText(text);
    setCurrentCharFormat(QTextCharFormat());
}

QString MessageInput::text() const
{
    return toPlainText();
}

void MessageInput::setRichText(const QString &html)
{
    {
        QSignalBlocker blocker(this);
        if (html.isEmpty()) setPlainText(QString());
        else setHtml(html);
    }
    emit formattingChanged();
}

QString MessageInput::styleName(Style style) const
{
    switch (style) {
    case Bold: return QStringLiteral("BOLD");
    case Italic: return QStringLiteral("ITALIC");
    case Strike: return QStringLiteral("STRIKETHROUGH");
    case Monospace: return QStringLiteral("MONOSPACE");
    case Spoiler: return QStringLiteral("SPOILER");
    }
    return QString();
}

QJsonArray MessageInput::textStyles() const
{
    QJsonArray result;
    QTextDocument *doc = document();
    const int end = toPlainText().size();
    if (end == 0) return result;

    auto hasStyle = [](const QTextCharFormat &f, Style style) {
        switch (style) {
        case Bold: return f.fontWeight() >= QFont::DemiBold;
        case Italic: return f.fontItalic();
        case Strike: return f.fontStrikeOut();
        case Monospace: return f.fontFixedPitch() || f.font().family() == QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
        case Spoiler: return f.hasProperty(QTextFormat::UserProperty + 1) ||
                               (f.hasProperty(QTextFormat::BackgroundBrush) && f.hasProperty(QTextFormat::ForegroundBrush) &&
                                f.background().color() == f.foreground().color());
        }
        return false;
    };

    for (int style = Bold; style <= Spoiler; ++style) {
        const QString name = styleName(static_cast<Style>(style));
        int runStart = -1;
        for (int pos = 0; pos <= end; ++pos) {
            bool active = false;
            if (pos < end) {
                QTextCursor c(doc);
                c.setPosition(pos);
                c.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 1);
                active = hasStyle(c.charFormat(), static_cast<Style>(style));
            }
            if (active && runStart < 0) runStart = pos;
            if ((!active || pos == end) && runStart >= 0) {
                result.append(QStringLiteral("%1:%2:%3").arg(runStart).arg(pos - runStart).arg(name));
                runStart = -1;
            }
        }
    }
    return result;
}

QList<MentionRange> MessageInput::mentionRanges() const
{
    QList<MentionRange> result;
    const QString plain = toPlainText();
    int runStart = -1;
    QString runUuid;
    for (int pos = 0; pos <= plain.size(); ++pos) {
        QString uuid;
        if (pos < plain.size()) {
            QTextCursor c(document());
            c.setPosition(pos);
            c.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 1);
            uuid = c.charFormat().property(kMentionUuidProperty).toString();
        }
        if (uuid == runUuid) continue;
        if (runStart >= 0 && !runUuid.isEmpty())
            result.append({runStart, pos - runStart, runUuid});
        runStart = uuid.isEmpty() ? -1 : pos;
        runUuid = uuid;
    }
    return result;
}

void MessageInput::insertMention(int start, int end, const QString &recipientId, const QString &displayName)
{
    if (recipientId.trimmed().isEmpty() || displayName.trimmed().isEmpty()) return;
    const QString plain = toPlainText();
    start = qBound(0, start, plain.size());
    end = qBound(start, end, plain.size());

    QTextCursor cursor(document());
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.insertText(QStringLiteral("@"), QTextCharFormat());

    QTextCharFormat mentionFormat;
    mentionFormat.setProperty(kMentionUuidProperty, recipientId);
    mentionFormat.setForeground(palette().color(QPalette::Link));
    mentionFormat.setFontWeight(QFont::DemiBold);
    cursor.insertText(displayName, mentionFormat);
    if (cursor.position() >= toPlainText().size() ||
        (cursor.position() < toPlainText().size() && !toPlainText().at(cursor.position()).isSpace()))
        cursor.insertText(QStringLiteral(" "), QTextCharFormat());
    setTextCursor(cursor);
    setCurrentCharFormat(QTextCharFormat());
    emit formattingChanged();
}

void MessageInput::applyMentionFormat(int start, int length, const QString &recipientId)
{
    if (recipientId.trimmed().isEmpty() || length <= 0) return;
    const int textLength = toPlainText().size();
    start = qBound(0, start, textLength);
    const int end = qBound(start, start + length, textLength);
    if (end <= start) return;

    QTextCursor cursor(document());
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    QTextCharFormat format;
    format.setProperty(kMentionUuidProperty, recipientId);
    format.setForeground(palette().color(QPalette::Link));
    format.setFontWeight(QFont::DemiBold);
    cursor.mergeCharFormat(format);
}

bool MessageInput::selectionHasStyle(Style style) const
{
    QTextCursor selected = textCursor();
    if (!selected.hasSelection()) return false;
    const QString name = styleName(style);
    const QJsonArray styles = textStyles();
    const int start = selected.selectionStart();
    const int end = selected.selectionEnd();
    for (const QJsonValue &v : styles) {
        const QStringList p = v.toString().split(QLatin1Char(':'));
        if (p.size() != 3 || p.at(2) != name) continue;
        const int s = p.at(0).toInt(), e = s + p.at(1).toInt();
        if (s <= start && e >= end) return true;
    }
    return false;
}

void MessageInput::applyStyle(Style style)
{
    QTextCursor cursor = textCursor();
    if (!cursor.hasSelection()) return;
    const bool remove = selectionHasStyle(style);
    QTextCharFormat fmt;
    switch (style) {
    case Bold: fmt.setFontWeight(remove ? QFont::Normal : QFont::Bold); break;
    case Italic: fmt.setFontItalic(!remove); break;
    case Strike: fmt.setFontStrikeOut(!remove); break;
    case Monospace: {
        fmt.setFontFixedPitch(!remove);
        if (remove) fmt.setFontFamilies(QStringList());
        else fmt.setFontFamilies({QFontDatabase::systemFont(QFontDatabase::FixedFont).family()});
        break;
    }
    case Spoiler: {
        if (remove) {
            fmt.clearProperty(QTextFormat::UserProperty + 1);
            fmt.clearProperty(QTextFormat::BackgroundBrush);
            fmt.clearProperty(QTextFormat::ForegroundBrush);
        } else {
            fmt.setProperty(QTextFormat::UserProperty + 1, true);
            const QColor c = palette().text().color();
            fmt.setBackground(c);
            fmt.setForeground(c);
        }
        break;
    }
    }
    cursor.mergeCharFormat(fmt);
    setTextCursor(cursor);
    m_formatButtons.at(static_cast<int>(style))->setChecked(!remove);
    emit formattingChanged();
    positionFormattingBar();
}

void MessageInput::updateFormattingBarTheme()
{
    if (!m_formatBar) return;

    // Derive the toolbar colours from this input's active palette rather than
    // using fixed light/dark colours. This respects both system themes and any
    // application palette overrides.
    const QPalette p = palette();
    QColor surface = p.color(QPalette::Button);
    if (!surface.isValid()) surface = p.color(QPalette::Window);
    const QColor text = p.color(QPalette::ButtonText);
    const QColor border = p.color(QPalette::Mid);
    const QColor accent = p.color(QPalette::Highlight);
    const QColor accentText = p.color(QPalette::HighlightedText);

    auto rgba = [](QColor color, int alpha) {
        color.setAlpha(alpha);
        return QStringLiteral("rgba(%1, %2, %3, %4)")
            .arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
    };

    m_formatBar->setPalette(p);
    m_formatBar->setStyleSheet(QStringLiteral(
        "#messageFormattingBar { background-color: %1; border: 1px solid %2; border-radius: 7px; padding: 2px; }"
        "#messageFormattingBar QToolButton { color: %3; background: transparent; border: none;"
        " border-radius: 5px; min-width: 28px; max-width: 28px; min-height: 28px; max-height: 28px;"
        " padding: 0px; font-size: 16px; font-weight: 600; }"
        "#messageFormattingBar QToolButton:hover { background-color: %4; }"
        "#messageFormattingBar QToolButton:checked { background-color: %5; color: %6; }")
        .arg(surface.name(), border.name(), text.name(), rgba(accent, 38), accent.name(), accentText.name()));
}

void MessageInput::changeEvent(QEvent *event)
{
    QTextEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        updateFormattingBarTheme();
        if (m_formatBar && m_formatBar->isVisible()) {
            m_formatBar->update();
        }
    }
}

void MessageInput::showFormattingBar()
{
    if (!m_formatBar || !textCursor().hasSelection()) return;
    updateFormattingBarTheme();
    positionFormattingBar();
    m_formatBar->show();
    m_formatBar->raise();
    for (int i = 0; i < m_formatButtons.size(); ++i)
        m_formatButtons.at(i)->setChecked(selectionHasStyle(static_cast<Style>(i)));
}

void MessageInput::hideFormattingBar()
{
    if (m_formatBar) m_formatBar->hide();
}

void MessageInput::positionFormattingBar()
{
    if (!m_formatBar || !textCursor().hasSelection()) return;

    QWidget *host = parentWidget();
    if (!host) return;

    // Keep the toolbar as a child overlay of the chat panel rather than a
    // separate Qt::Tool window. Window managers (notably Wayland compositors)
    // may adjust the position of top-level tool windows, causing them to land
    // over the selection even when given global coordinates.
    if (m_formatBar->parentWidget() != host || m_formatBar->isWindow()) {
        m_formatBar->hide();
        m_formatBar->setParent(host, Qt::Widget);
        m_formatBar->setAttribute(Qt::WA_ShowWithoutActivating, true);
    }

    m_formatBar->adjustSize();
    const QSize size = m_formatBar->sizeHint();
    const QPoint fieldTop = mapTo(host, QPoint(0, 0));

    // Keep the toolbar above the input field, but horizontally align it with
    // the selected text instead of centering it over the whole field. QTextEdit
    // reports cursor rectangles in viewport coordinates, so map them into the
    // toolbar host before calculating the anchor point.
    QTextCursor selection = textCursor();
    QTextCursor selectionStart = selection;
    selectionStart.setPosition(selection.selectionStart());
    QTextCursor selectionEnd = selection;
    selectionEnd.setPosition(selection.selectionEnd());
    const QRect startRect = cursorRect(selectionStart);
    const QRect endRect = cursorRect(selectionEnd);
    const QPoint startPoint = viewport()->mapTo(host, startRect.center());
    const QPoint endPoint = viewport()->mapTo(host, endRect.center());

    // For a single-line selection, center the toolbar over the selected range.
    // For a wrapped/multi-line selection, anchor it at the start of the range,
    // which is stable and avoids placing it at an unrelated x-coordinate.
    const int anchorX = (startRect.center().y() == endRect.center().y())
        ? (startPoint.x() + endPoint.x()) / 2
        : startPoint.x();

    // The lower edge stays above the input widget with an 8 px gap. Clamp the
    // x coordinate to the input's width first, so the toolbar stays visually
    // attached to the text field while following the selected text.
    QPoint pos(anchorX - size.width() / 2,
               fieldTop.y() - size.height() - 8);
    const int fieldMinX = fieldTop.x() + 4;
    const int fieldMaxX = fieldTop.x() + width() - size.width() - 4;
    if (fieldMaxX >= fieldMinX) {
        pos.setX(qBound(fieldMinX, pos.x(), fieldMaxX));
    } else {
        const QRect bounds = host->rect();
        const int minX = bounds.left() + 4;
        const int maxX = qMax(minX, bounds.right() - size.width() - 4);
        pos.setX(qBound(minX, pos.x(), maxX));
    }
    m_formatBar->move(pos);
}

void MessageInput::moveEvent(QMoveEvent *event)
{
    QTextEdit::moveEvent(event);
    if (m_formatBar && m_formatBar->isVisible()) positionFormattingBar();
}

void MessageInput::resizeEvent(QResizeEvent *event)
{
    QTextEdit::resizeEvent(event);
    if (m_formatBar && m_formatBar->isVisible()) positionFormattingBar();
}

void MessageInput::keyPressEvent(QKeyEvent *e)
{
    QScopedValueRollback<bool> keyPressGuard(m_handlingKeyPress, true);
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
            setCurrentCharFormat(QTextCharFormat());
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
        setCurrentCharFormat(QTextCharFormat());
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
        if (!textCursor().hasSelection()) setCurrentCharFormat(QTextCharFormat());
        return;
    }

    // Ctrl+V, Shift+Insert, Cmd+V ...
    if (e->matches(QKeySequence::Paste) && m_hook && m_hook(QApplication::clipboard()->mimeData())) {
        if (!textCursor().hasSelection()) setCurrentCharFormat(QTextCharFormat());
        e->accept();
        return;
    }

    const bool insertingText = !e->text().isEmpty()
        && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        && e->key() != Qt::Key_Backspace && e->key() != Qt::Key_Delete
        && e->key() != Qt::Key_Return && e->key() != Qt::Key_Enter
        && e->key() != Qt::Key_Tab;
    QTextEdit::keyPressEvent(e);
    if (!insertingText && !textCursor().hasSelection())
        setCurrentCharFormat(QTextCharFormat());
}

void MessageInput::mousePressEvent(QMouseEvent *e)
{
    QTextEdit::mousePressEvent(e);
    if (!textCursor().hasSelection())
        setCurrentCharFormat(QTextCharFormat());
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
