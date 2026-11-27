#include "ChatView.h"

#include "ApngMovie.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QLabel>
#include <QKeyEvent>
#include <QLocale>
#include <QMenu>
#include <QMovie>
#include <QBuffer>
#include <QMouseEvent>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QVector>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolTip>
#include <QTextDocument>
#include <QElapsedTimer>
#include <QTimer>
#include <QToolButton>
#include <QVideoWidget>
#include <algorithm>
#include <numeric>

#include "Attachments.h"
#include "EmojiPicker.h"

namespace {
constexpr int kMargin = 12; // distance of bubbles to the view edge
constexpr int kPadX = 12;
constexpr int kPadY = 8;
constexpr int kGap = 4;
constexpr int kMaxImage = 300;
constexpr int kMaxCachedConversations = 3;
constexpr int kMaxCachedConversationItems = 2500;
// Chats with at least this many messages are opened lazily (see beginLazyLayout()). Below it a
// full layout costs a few milliseconds and is simpler.
constexpr int kLazyOpenMinMessages = 100;

QFont scaled(const QFont &f, qreal factor, bool bold = false)
{
    QFont r = f;
    if (f.pointSizeF() > 0)
        r.setPointSizeF(f.pointSizeF() * factor);
    else if (f.pixelSize() > 0)
        r.setPixelSize(qMax(6, int(f.pixelSize() * factor)));
    r.setBold(bold);
    return r;
}

QColor alpha(QColor c, int a)
{
    c.setAlpha(a);
    return c;
}

// Text of the chat area's secondary elements (date labels, reaction pills, ...).
QColor mutedOn(const QColor &bg)
{
    return bg.lightness() < 128 ? QColor(0xb5, 0xb5, 0xb8) : QColor(0x6b, 0x6b, 0x70);
}

// Placeholder shown instead of a message that was deleted for everyone (wording as in Signal Desktop).
QString deletedText(const Message &m)
{
    return m.outgoing ? QCoreApplication::translate("ChatView", "You deleted this message.")
                      : QCoreApplication::translate("ChatView", "This message was deleted.");
}

QString linkify(const QString &text)
{
    QString esc = text.toHtmlEscaped();
    static const QRegularExpression re(QStringLiteral(R"((https?://[^\s<>"]*[^\s<>".,;:!?)\]]))"));
    esc.replace(re, QStringLiteral("<a href=\"\\1\">\\1</a>"));
    esc.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return esc;
}

QString styledMessageHtml(const QString &text, const QList<TextStyleRange> &styles,
                          const QList<MentionRange> &mentions, const QColor &fg,
                          const std::function<QString(const QString &)> &nameResolver)
{
    if (text.isEmpty()) return QString();
    const int n = text.size();
    QVector<QSet<QString>> active(n);
    for (const TextStyleRange &r : styles) {
        const int a = qMax(0, r.start);
        const int b = qMin(n, r.start + r.length);
        for (int i = a; i < b; ++i) active[i].insert(r.style);
    }

    QList<MentionRange> sortedMentions = mentions;
    std::sort(sortedMentions.begin(), sortedMentions.end(), [](const MentionRange &a, const MentionRange &b) {
        return a.start < b.start;
    });

    auto runHtml = [&](int a, int b) {
        if (a >= b) return QString();
        QString css;
        const QSet<QString> &set = active[a];
        if (set.contains(QStringLiteral("BOLD"))) css += QStringLiteral("font-weight:600;");
        if (set.contains(QStringLiteral("ITALIC"))) css += QStringLiteral("font-style:italic;");
        if (set.contains(QStringLiteral("STRIKETHROUGH"))) css += QStringLiteral("text-decoration:line-through;");
        if (set.contains(QStringLiteral("MONOSPACE"))) css += QStringLiteral("font-family:monospace;");
        if (set.contains(QStringLiteral("SPOILER")))
            css += QStringLiteral("background-color:%1;color:%1;").arg(fg.name());
        const QString piece = linkify(text.mid(a, b - a));
        return css.isEmpty() ? piece : QStringLiteral("<span style=\"%1\">%2</span>").arg(css, piece);
    };

    QString html;
    int i = 0;
    int mentionIndex = 0;
    while (i < n) {
        while (mentionIndex < sortedMentions.size() &&
               (sortedMentions.at(mentionIndex).start < i || sortedMentions.at(mentionIndex).length <= 0))
            ++mentionIndex;
        if (mentionIndex < sortedMentions.size() && sortedMentions.at(mentionIndex).start == i) {
            const MentionRange range = sortedMentions.at(mentionIndex++);
            const int consume = text.at(i) == QChar(0xfffc) ? 1 : qMin(range.length, n - i);
            QString label = range.uuid.isEmpty() ? QString() : (nameResolver ? nameResolver(range.uuid) : range.uuid);
            if (label.trimmed().isEmpty() || label == range.uuid) {
                // Avoid exposing a raw UUID as the UI text if signal-cli could not resolve it.
                const bool looksLikeUuid = range.uuid.count(QLatin1Char('-')) >= 4;
                label = (range.uuid.isEmpty() || looksLikeUuid)
                            ? QCoreApplication::translate("ChatView", "mentioned member") : range.uuid;
            }
            html += QStringLiteral("<span style=\"color:%1;font-weight:600;\">@%2</span>")
                        .arg(fg.lighter(125).name(), label.toHtmlEscaped());
            i += qMax(1, consume);
            continue;
        }
        int j = i + 1;
        while (j < n && active[j] == active[i] &&
               (mentionIndex >= sortedMentions.size() || sortedMentions.at(mentionIndex).start != j))
            ++j;
        html += runHtml(i, j);
        i = j;
    }
    return html;
}

// Shorten `text` so it fits into at most `maxLines` lines of the given width.
QString fitLines(const QFontMetrics &fm, const QString &in, int width, int maxLines)
{
    const QString text = in.simplified();
    if (text.isEmpty() || width <= 0) return text;
    const int maxH = fm.lineSpacing() * maxLines;
    auto fits = [&](const QString &t) {
        return fm.boundingRect(QRect(0, 0, width, 100000), Qt::TextWordWrap, t).height() <= maxH;
    };
    if (fits(text)) return text;
    int lo = 0, hi = text.size();
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (fits(text.left(mid) + QStringLiteral("…"))) lo = mid; else hi = mid - 1;
    }
    return text.left(lo).trimmed() + QStringLiteral("…");
}

QString videoTime(qint64 milliseconds)
{
    const qint64 totalSeconds = qMax<qint64>(0, milliseconds) / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0)
        return QStringLiteral("%1:%2:%3").arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(totalSeconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

class SeekSlider final : public QSlider
{
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || width() <= 0) {
            QSlider::mousePressEvent(event);
            return;
        }
        QStyleOptionSlider option;
        initStyleOption(&option);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option,
                                                      QStyle::SC_SliderGroove, this);
        const int handleLength = style()->pixelMetric(QStyle::PM_SliderLength, &option, this);
        const int span = qMax(1, groove.width() - handleLength);
        const int position = event->pos().x() - groove.left() - handleLength / 2;
        setSliderDown(true);
        setSliderPosition(QStyle::sliderValueFromPosition(minimum(), maximum(), position,
                                                           span, option.upsideDown));
        event->accept();
    }
};

void updateVideoTimes(QLabel *elapsed, QLabel *remaining, qint64 position, qint64 duration)
{
    if (!elapsed || !remaining) return;
    elapsed->setText(videoTime(position));
    remaining->setText(QStringLiteral("-%1").arg(videoTime(qMax<qint64>(0, duration - position))));
}
} // namespace

// ---------------------------------------------------------------------------
// The round "scroll to the newest message" button, with a badge for the messages
// that arrived while the view was scrolled up (like in Signal Desktop).
// ---------------------------------------------------------------------------
class ScrollToLatestButton : public QWidget
{
public:
    std::function<void()> onClicked;

    explicit ScrollToLatestButton(QWidget *parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("scrollToLatest"));
        setFixedSize(kWidth, kHeight);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);
        setAccessibleName(QObject::tr("Scroll to latest message"));
        setToolTip(QObject::tr("Scroll to latest message"));
        hide();
    }

    void setMissed(int count)
    {
        if (count == m_missed) return;
        m_missed = count;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF circle(kInset, kHeight - kDiameter - kInset, kDiameter, kDiameter);

        // soft shadow
        p.setPen(Qt::NoPen);
        for (int i = 3; i >= 1; --i) {
            p.setBrush(QColor(0, 0, 0, 14));
            p.drawEllipse(circle.adjusted(-i, -i + 2, i, i + 2));
        }
        QColor fill = palette().color(QPalette::Button);
        if (m_hover) fill = fill.lighter(m_pressed ? 90 : 115);
        p.setBrush(fill);
        p.setPen(QPen(QColor(128, 128, 128, 90), 1));
        p.drawEllipse(circle);

        // chevron pointing down
        QPen pen(palette().color(QPalette::ButtonText), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const QPointF c = circle.center();
        QPainterPath chevron;
        chevron.moveTo(c + QPointF(-7, -3));
        chevron.lineTo(c + QPointF(0, 4));
        chevron.lineTo(c + QPointF(7, -3));
        p.drawPath(chevron);

        if (m_missed > 0) {
            const QString text = m_missed > 99 ? QStringLiteral("99+") : QString::number(m_missed);
            QFont f = font();
            f.setBold(true);
            f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.5));
            p.setFont(f);
            const int w = qMax(kBadge, QFontMetrics(f).horizontalAdvance(text) + 10);
            const QRectF badge(kWidth - w, 0, w, kBadge);
            p.setPen(Qt::NoPen);
            p.setBrush(palette().color(QPalette::Highlight));
            p.drawRoundedRect(badge, kBadge / 2.0, kBadge / 2.0);
            p.setPen(palette().color(QPalette::HighlightedText));
            p.drawText(badge, Qt::AlignCenter, text);
        }
    }

    void enterEvent(QEnterEvent *) override { setHover(true); }
    void leaveEvent(QEvent *) override
    {
        m_pressed = false;
        setHover(false);
    }
    // Accepted so that they do not fall through to the chat underneath.
    void mouseMoveEvent(QMouseEvent *e) override { e->accept(); }
    void contextMenuEvent(QContextMenuEvent *e) override { e->accept(); }
    void mouseDoubleClickEvent(QMouseEvent *e) override { e->accept(); }
    void mousePressEvent(QMouseEvent *e) override
    {
        e->accept();
        if (e->button() != Qt::LeftButton) return;
        m_pressed = true;
        update();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        e->accept();
        const bool click = e->button() == Qt::LeftButton && m_pressed && rect().contains(e->position().toPoint());
        m_pressed = false;
        update();
        if (click && onClicked) onClicked();
    }

private:
    static constexpr int kDiameter = 40;
    static constexpr int kInset = 2;
    static constexpr int kBadge = 20;
    static constexpr int kWidth = kDiameter + 2 * kInset + 10;  // room for the badge on the right
    static constexpr int kHeight = kDiameter + 2 * kInset + 10; // ... and above

    void setHover(bool on)
    {
        if (m_hover == on) return;
        m_hover = on;
        update();
    }

    int m_missed = 0;
    bool m_hover = false;
    bool m_pressed = false;
};

ChatView::ChatView(QWidget *parent) : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setMouseTracking(true);
    viewport()->setAutoFillBackground(false);
    verticalScrollBar()->setSingleStep(40);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::StrongFocus);
    m_ap = Appearance::defaults(palette());
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        updateVideoGeometry();
        updateToLatestButton();
        // The user scrolled while the off-screen rows are still being re-laid out: hold the
        // new position instead of the one captured when the resize started.
        if (m_relayoutActive && m_progScroll == 0) captureResizeAnchor();
    });
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] { updateToLatestButton(); });
    m_toLatest = new ScrollToLatestButton(viewport());
    m_toLatest->onClicked = [this] { verticalScrollBar()->setValue(verticalScrollBar()->maximum()); };
    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    // Every resize event re-lays out only the rows on screen (relayoutWindow()), so the text
    // re-wraps live while the window edge is dragged. Once the drag pauses, the rest of the
    // conversation is brought up to date in short slices that never block the UI.
    m_resizeTimer->setInterval(40);
    connect(m_resizeTimer, &QTimer::timeout, this, [this] {
        if (!m_relayoutActive) return;
        m_bgStarted = false;
        relayoutSlice();
    });
    m_relayoutTimer = new QTimer(this);
    m_relayoutTimer->setSingleShot(true);
    m_relayoutTimer->setInterval(0);
    connect(m_relayoutTimer, &QTimer::timeout, this, &ChatView::relayoutSlice);
    // Keep parsed text documents available across short pauses while the user drags
    // the window edge. The normal renderer still releases off-screen paint resources.
    m_resizeCleanupTimer = new QTimer(this);
    m_resizeCleanupTimer->setSingleShot(true);
    m_resizeCleanupTimer->setInterval(300);
    connect(m_resizeCleanupTimer, &QTimer::timeout, this, [this] {
        for (Item &item : m_items) item.measureDoc.reset();
    });
    m_autoScrollTimer = new QTimer(this);
    m_autoScrollTimer->setInterval(16);
    connect(m_autoScrollTimer, &QTimer::timeout, this, [this] {
        if (!m_autoScrolling) return;
        const int offset = m_autoScrollPos.y() - m_autoScrollAnchor.y();
        if (qAbs(offset) < 8) return;
        const int speed = qBound(1, qAbs(offset) / 4, 60);
        verticalScrollBar()->setValue(verticalScrollBar()->value() + (offset > 0 ? speed : -speed));
    });
}

ChatView::~ChatView() = default;

void ChatView::setNameResolver(std::function<QString(const QString &)> fn)
{
    invalidateAllCachedConversations();
    m_nameOf = std::move(fn);
    for (Item &item : m_items) {
        item.doc.reset();
        item.measureDoc.reset();
        item.renderResourcesBuilt = false;
    }
}

void ChatView::disposeItemResources(QList<Item> &items)
{
    for (Item &item : items) {
        for (QMovie *movie : item.movies)
            delete movie;
        for (ApngMovie *movie : item.apngMovies)
            delete movie;
        for (int i = 0; i < item.movies.size(); ++i)
            item.movies[i] = nullptr;
        for (int i = 0; i < item.apngMovies.size(); ++i)
            item.apngMovies[i] = nullptr;
        for (int i = 0; i < item.animationRequested.size(); ++i)
            item.animationRequested[i] = false;

        // Keep the measured geometry, but drop heavyweight paint resources. They are
        // recreated only for messages near the viewport when this chat is shown again.
        item.doc.reset();
        item.measureDoc.reset();
        item.pix.clear();
        item.linkPreviewPixmap = QPixmap();
        item.contactAvatarPixmap = QPixmap();
        item.renderResourcesBuilt = false;
    }
}

void ChatView::discardCachedConversation(const QString &key)
{
    auto it = m_conversationCache.find(key);
    if (it == m_conversationCache.end()) return;
    disposeItemResources(it->items);
    m_conversationCache.erase(it);
    m_conversationCacheOrder.removeAll(key);
}

void ChatView::invalidateConversation(const QString &key)
{
    if (key.isEmpty() || key == m_conversationKey) return;
    discardCachedConversation(key);
}

void ChatView::updateMessageReceipt(const QString &conversationKey, int messageIndex, int receiptStatus)
{
    const auto update = [this, messageIndex, receiptStatus](QList<Item> &items) -> QRect {
        if (messageIndex < 0 || messageIndex >= items.size()) return {};
        Item &item = items[messageIndex];
        if (!item.built) return {};

        // Reserve the width of two ticks in layoutItem(), so changing from sent to
        // delivered/read never changes bubble geometry or requires a relayout.
        item.statusText = m_showDeliveryStatus
            ? (receiptStatus >= 1 ? QStringLiteral("✓✓") : QStringLiteral("✓"))
            : QString();
        item.statusRead = m_showReadReceipts && receiptStatus >= 2;
        return item.statusRect;
    };

    if (conversationKey == m_conversationKey) {
        const QRect statusRect = update(m_items);
        if (!statusRect.isEmpty()) {
            const int y = m_items[messageIndex].y - verticalScrollBar()->value();
            viewport()->update(statusRect.translated(0, y).adjusted(-2, -2, 2, 2));
        }
        return;
    }

    auto cached = m_conversationCache.find(conversationKey);
    if (cached != m_conversationCache.end())
        update(cached->items);
}

void ChatView::invalidateAllCachedConversations()
{
    const QStringList keys = m_conversationCache.keys();
    for (const QString &key : keys)
        discardCachedConversation(key);
}

void ChatView::cacheCurrentConversation()
{
    if (m_conversationKey.isEmpty()) return;

    stopVideoPlayback();
    clearSelection();
    if (m_relayoutActive) {
        // Not every row has been laid out yet. Finishing that here would stall the switch, and
        // such a chat reopens immediately anyway (beginLazyLayout()), so it is not cached.
        cancelRelayout();
        disposeItemResources(m_items);
        discardCachedConversation(m_conversationKey);
        m_msgs.clear();
        m_items.clear();
        m_renderFirst = m_renderLast = -1;
        m_layoutWidth = -1;
        return;
    }
    CachedConversation cached;
    cached.items = std::move(m_items);
    cached.pins = m_pins;
    cached.messageCount = m_msgs.size();
    cached.lastSortTs = m_msgs.isEmpty() ? 0 : m_msgs.last().sortTs();
    cached.group = m_group;
    cached.bannerCycle = m_bannerCycle;
    cached.missed = m_missed;
    cached.scrollValue = verticalScrollBar()->value();
    cached.layoutWidth = m_layoutWidth;
    disposeItemResources(cached.items);

    discardCachedConversation(m_conversationKey);
    m_conversationCache.insert(m_conversationKey, std::move(cached));
    m_conversationCacheOrder.append(m_conversationKey);
    const auto cachedItemCount = [this] {
        qsizetype count = 0;
        for (auto it = m_conversationCache.cbegin(); it != m_conversationCache.cend(); ++it)
            count += it.value().items.size();
        return count;
    };
    while (!m_conversationCacheOrder.isEmpty() &&
           (m_conversationCacheOrder.size() > kMaxCachedConversations ||
            cachedItemCount() > kMaxCachedConversationItems)) {
        discardCachedConversation(m_conversationCacheOrder.first());
    }

    m_msgs.clear();
    m_items.clear();
    m_renderFirst = m_renderLast = -1;
    m_layoutWidth = -1;
}

QString ChatView::nameOf(const QString &key) const
{
    if (key.isEmpty()) return tr("Unknown");
    if (!m_self.isEmpty() && key == m_self) return tr("You");
    return m_nameOf ? m_nameOf(key) : key;
}

bool ChatView::atBottom() const
{
    return verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 4;
}

// Shows the arrow once the view is clearly away from the newest message (or messages
// arrived below the visible part) and keeps it in the bottom right corner.
void ChatView::updateToLatestButton()
{
    if (!m_toLatest) return;
    if (atBottom()) m_missed = 0;
    const int distance = verticalScrollBar()->maximum() - verticalScrollBar()->value();
    const int threshold = qMax(160, viewport()->height() / 3);
    const bool show = !m_items.isEmpty() && (distance > threshold || (m_missed > 0 && !atBottom()));
    m_toLatest->setMissed(m_missed);
    m_toLatest->move(viewport()->width() - m_toLatest->width() - 6, viewport()->height() - m_toLatest->height() - 6);
    m_toLatest->setVisible(show);
    if (show) m_toLatest->raise();
}

// ------------------------------------------------------------ public API

void ChatView::setGroupMode(bool group)
{
    if (m_group == group) return;
    m_group = group;
    if (!m_items.isEmpty()) relayoutAll();
}

void ChatView::setReadReceiptsEnabled(bool enabled)
{
    if (m_showReadReceipts == enabled) return;
    invalidateAllCachedConversations();
    m_showReadReceipts = enabled;
    for (int i = 0; i < m_items.size() && i < m_msgs.size(); ++i)
        m_items[i].statusRead = m_showReadReceipts && m_msgs[i].receiptStatus >= 2;
    viewport()->update();
}

void ChatView::setDeliveryStatusVisible(bool visible)
{
    if (m_showDeliveryStatus == visible) return;
    invalidateAllCachedConversations();
    m_showDeliveryStatus = visible;
    // This is a local display preference only. Keep receipt processing and stored receipt data
    // intact so enabling the indicators again immediately restores each message's current state.
    for (int i = 0; i < m_items.size() && i < m_msgs.size(); ++i) {
        m_items[i].statusText.clear();
        if (m_showDeliveryStatus && m_msgs[i].outgoing)
            m_items[i].statusText = m_msgs[i].receiptStatus >= 1 ? QStringLiteral("✓✓") : QStringLiteral("✓");
    }
    relayoutAll();
}

void ChatView::setAppearance(const Appearance &a)
{
    invalidateAllCachedConversations();
    m_ap = a;
    for (Item &it : m_items) { // colours of links are baked into the documents
        it.built = false;
        it.doc.reset();
        it.measureDoc.reset();
        it.pix.clear();
        it.renderResourcesBuilt = false;
    }
    m_renderFirst = m_renderLast = -1;
    const bool bottom = atBottom();
    const int old = verticalScrollBar()->value();
    relayoutAll();
    verticalScrollBar()->setValue(bottom ? verticalScrollBar()->maximum() : old);
}

void ChatView::clearSelection()
{
    m_selItem = -1;
    m_selCursor = QTextCursor();
    m_dragging = false;
    m_wordSelected = false;
}

// Double-click on a message bubble: select the whole word under the cursor,
// like Signal/WhatsApp do. The selection survives the click/release sequence
// (see mousePressEvent / mouseReleaseEvent) so it can be copied with Ctrl+C.
void ChatView::selectWordAt(int msg, int textPos)
{
    if (msg < 0 || msg >= m_items.size() || !m_items[msg].doc || textPos < 0) return;
    m_selItem = msg;
    m_selCursor = QTextCursor(m_items[msg].doc.get());
    m_selCursor.setPosition(textPos);
    m_selCursor.select(QTextCursor::WordUnderCursor);
    m_dragging = false;
    m_wordSelected = true;
    viewport()->update();
}

void ChatView::clearView()
{
    cancelRelayout();
    stopVideoPlayback();
    disposeItemResources(m_items);
    invalidateAllCachedConversations();
    m_conversationKey.clear();
    m_msgs.clear();
    m_items.clear();
    m_pins.clear();
    m_bannerCycle = 0;
    m_bannerOffset = 0;
    m_renderFirst = m_renderLast = -1;
    clearSelection();
    m_flash = -1;
    updateScrollRange();
    verticalScrollBar()->setValue(0);
    m_missed = 0;
    updateToLatestButton();
    viewport()->update();
}

void ChatView::setPins(const QList<PinInfo> &pins)
{
    if (pins == m_pins) return;
    m_pins = pins;
    m_bannerOffset = m_pins.isEmpty() ? 0 : kBannerHeight;
    m_bannerCycle = m_pins.isEmpty() ? 0 : qBound(0, m_bannerCycle, m_pins.size() - 1);
    relayoutAll();
}

void ChatView::showMessages(const QList<Message> &msgs, bool keepScroll)
{
    showMessages(msgs, keepScroll, QString(), m_group, m_pins);
}

void ChatView::showMessages(const QList<Message> &msgs, bool keepScroll, const QString &conversationKey,
                            bool group, const QList<PinInfo> &pins)
{
    const bool bottom = atBottom();
    const int old = verticalScrollBar()->value();

    if (!conversationKey.isEmpty() && conversationKey != m_conversationKey)
        cacheCurrentConversation();

    m_group = group;
    m_pins = pins;
    m_bannerOffset = m_pins.isEmpty() ? 0 : kBannerHeight;
    m_bannerCycle = 0; // a new conversation starts at its oldest pin; cached views restore their own cycle

    if (!conversationKey.isEmpty()) {
        auto cachedIt = m_conversationCache.find(conversationKey);
        if (cachedIt != m_conversationCache.end()) {
            CachedConversation cached = std::move(cachedIt.value());
            m_conversationCache.erase(cachedIt);
            m_conversationCacheOrder.removeAll(conversationKey);

            const qint64 lastSortTs = msgs.isEmpty() ? 0 : msgs.last().sortTs();
            const bool valid = cached.messageCount == msgs.size() &&
                               cached.lastSortTs == lastSortTs &&
                               cached.group == group &&
                               cached.pins == pins &&
                               cached.layoutWidth == viewport()->width();
            if (valid) {
                m_msgs = msgs;
                cancelRelayout();
                m_items = std::move(cached.items);
                m_conversationKey = conversationKey;
                m_bannerCycle = cached.bannerCycle;
                m_missed = cached.missed;
                m_layoutWidth = cached.layoutWidth;
                m_renderFirst = m_renderLast = -1;
                m_flash = -1;
                clearSelection();
                updateScrollRange();
                verticalScrollBar()->setValue(cached.scrollValue);
                updateToLatestButton();
                updateVideoGeometry();
                viewport()->update();
                return;
            }
            disposeItemResources(cached.items);
        }
    }

    clearSelection();
    disposeItemResources(m_items);
    m_msgs = msgs;
    m_items = QList<Item>(msgs.size());
    m_conversationKey = conversationKey;
    m_renderFirst = m_renderLast = -1;
    m_layoutWidth = -1;
    if (msgs.size() >= kLazyOpenMinMessages && (bottom || !keepScroll)) {
        // Opens at the newest message: only what is on screen has to be laid out before the
        // chat can be shown; the (long) history above is measured in the background.
        cancelRelayout();
        beginLazyLayout();
    } else {
        relayoutAll();
        verticalScrollBar()->setValue((bottom || !keepScroll) ? verticalScrollBar()->maximum() : old);
    }
    if (!keepScroll) m_missed = 0; // another conversation: nothing is "new" here
    updateToLatestButton();
    updateVideoGeometry();
}

void ChatView::appendMessage(const Message &m)
{
    const bool bottom = atBottom();
    m_msgs.append(m);
    m_items.append(Item());
    const int i = m_msgs.size() - 1;
    layoutItem(i, i > 0 ? m_items[i - 1].y + m_items[i - 1].height : m_bannerOffset);
    if (!bottom && !m.outgoing) ++m_missed; // arrived below the visible part
    updateScrollRange();
    if (bottom || m.outgoing) verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    updateToLatestButton();
    updateVideoGeometry();
    viewport()->update();
}

void ChatView::replaceLastMessage(const Message &m)
{
    if (m_msgs.isEmpty() || m_items.size() != m_msgs.size()) return;

    const bool bottom = atBottom();
    const int oldScroll = verticalScrollBar()->value();
    const int i = m_msgs.size() - 1;

    clearSelection();
    for (QMovie *movie : m_items[i].movies)
        delete movie;
    for (ApngMovie *movie : m_items[i].apngMovies)
        delete movie;
    m_items[i] = Item();
    m_msgs[i] = m;
    m_flash = -1;
    const int y = i > 0 ? m_items[i - 1].y + m_items[i - 1].height : m_bannerOffset;
    layoutItem(i, y);

    updateScrollRange();
    verticalScrollBar()->setValue(bottom ? verticalScrollBar()->maximum() : oldScroll);
    updateToLatestButton();
    updateVideoGeometry();
    viewport()->update();
}

void ChatView::scrollToMessage(int idx)
{
    if (idx < 0 || idx >= m_items.size()) return;
    finishRelayout(); // the target row's position must not move afterwards
    const Item &it = m_items[idx];
    verticalScrollBar()->setValue(qBound(0, it.y + it.bubble.top() - 24, verticalScrollBar()->maximum()));
    m_flash = idx;
    viewport()->update();
    QTimer::singleShot(1200, this, [this, idx] {
        if (m_flash == idx) {
            m_flash = -1;
            viewport()->update();
        }
    });
}

QRect ChatView::bubbleRect(int idx) const
{
    if (idx < 0 || idx >= m_items.size()) return {};
    return m_items[idx].bubble.translated(0, m_items[idx].y - verticalScrollBar()->value());
}

QRect ChatView::quoteRect(int idx) const
{
    if (idx < 0 || idx >= m_items.size()) return {};
    return m_items[idx].quoteBox.translated(0, m_items[idx].y - verticalScrollBar()->value());
}

QRect ChatView::attachmentRect(int idx, int att) const
{
    if (idx < 0 || idx >= m_items.size() || att < 0 || att >= m_items[idx].attRects.size()) return {};
    return m_items[idx].attRects[att].translated(0, m_items[idx].y - verticalScrollBar()->value());
}

QRect ChatView::textRect(int idx) const
{
    if (idx < 0 || idx >= m_items.size()) return {};
    return m_items[idx].textRect.translated(0, m_items[idx].y - verticalScrollBar()->value());
}

void ChatView::setAudioPlayback(int messageIndex, int attachmentIndex, bool loading, bool playing,
                               qint64 positionMs, qint64 durationMs)
{
    m_audioMessage = messageIndex;
    m_audioAttachment = attachmentIndex;
    m_audioLoading = loading;
    m_audioPlaying = playing;
    m_audioPositionMs = qMax<qint64>(0, positionMs);
    m_audioDurationMs = qMax<qint64>(0, durationMs);
    if (messageIndex >= 0 && messageIndex < m_items.size()) {
        viewport()->update(m_items[messageIndex].attRects.value(attachmentIndex)
                               .translated(0, m_items[messageIndex].y - verticalScrollBar()->value()));
    }
}

void ChatView::setVideoLoading(int messageIndex, int attachmentIndex, bool loading)
{
    if (messageIndex < 0 || messageIndex >= m_msgs.size() || attachmentIndex < 0 ||
        attachmentIndex >= m_msgs[messageIndex].attachments.size()) return;
    if (loading) {
        m_videoLoadingTs = m_msgs[messageIndex].ts();
        m_videoLoadingAttachment = attachmentIndex;
    } else if (m_videoLoadingTs == m_msgs[messageIndex].ts() && m_videoLoadingAttachment == attachmentIndex) {
        m_videoLoadingTs = 0;
        m_videoLoadingAttachment = -1;
    }
    viewport()->update(m_items[messageIndex].attRects.value(attachmentIndex)
                           .translated(0, m_items[messageIndex].y - verticalScrollBar()->value()));
}

bool ChatView::isVideoActive(int messageIndex, int attachmentIndex) const
{
    return messageIndex >= 0 && messageIndex < m_msgs.size() && attachmentIndex >= 0 &&
           attachmentIndex < m_msgs[messageIndex].attachments.size() &&
           m_videoMessageTs == m_msgs[messageIndex].ts() && m_videoAttachment == attachmentIndex;
}

bool ChatView::isVideoLoading(int messageIndex, int attachmentIndex) const
{
    return messageIndex >= 0 && messageIndex < m_msgs.size() && attachmentIndex >= 0 &&
           attachmentIndex < m_msgs[messageIndex].attachments.size() &&
           m_videoLoadingTs == m_msgs[messageIndex].ts() && m_videoLoadingAttachment == attachmentIndex;
}

void ChatView::playVideoAttachment(int messageIndex, int attachmentIndex, const QString &path)
{
    if (messageIndex < 0 || messageIndex >= m_msgs.size() || attachmentIndex < 0 ||
        attachmentIndex >= m_msgs[messageIndex].attachments.size()) return;
    stopVideoPlayback();
    m_videoPlayer = new QMediaPlayer(this);
    m_videoAudioOutput = new QAudioOutput(this);
    m_videoWidget = new QVideoWidget(viewport());
    m_videoToggle = new QToolButton(viewport());
    m_videoProgress = new SeekSlider(Qt::Horizontal, viewport());
    m_videoElapsed = new QLabel(QStringLiteral("00:00"), viewport());
    m_videoRemaining = new QLabel(QStringLiteral("-00:00"), viewport());
    m_videoToggle->setText(QStringLiteral("Ⅱ"));
    m_videoToggle->setObjectName(QStringLiteral("videoPlayPause"));
    m_videoToggle->setToolTip(tr("Pause video"));
    m_videoToggle->setAccessibleName(tr("Pause video"));
    m_videoToggle->setFixedSize(32, 28);
    m_videoToggle->setStyleSheet(QStringLiteral("QToolButton { background: rgba(0,0,0,180); color: white; border: 0; }"));
    m_videoProgress->setObjectName(QStringLiteral("videoProgress"));
    m_videoProgress->setRange(0, 1000);
    m_videoProgress->setToolTip(tr("Video position"));
    m_videoProgress->setAccessibleName(tr("Video position"));
    m_videoProgress->setFixedHeight(24);
    m_videoProgress->setStyleSheet(QStringLiteral(
        "QSlider { background: rgba(0,0,0,180); padding: 0 8px; }"
        "QSlider::groove:horizontal { height: 4px; background: rgba(255,255,255,100); }"
        "QSlider::sub-page:horizontal { background: white; }"
        "QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px; background: white; }"));
    for (QLabel *timeLabel : {m_videoElapsed, m_videoRemaining}) {
        timeLabel->setStyleSheet(QStringLiteral("QLabel { background: rgba(0,0,0,180); color: white; padding: 0 3px; }"));
        timeLabel->setFixedSize(50, 16);
        timeLabel->setAlignment(Qt::AlignCenter);
    }
    m_videoElapsed->setObjectName(QStringLiteral("videoElapsed"));
    m_videoRemaining->setObjectName(QStringLiteral("videoRemaining"));
    // Keep frames in the multimedia backend: converting every frame to QImage
    // forced a GPU-to-CPU copy before painting. The native surface is only shown
    // while the whole attachment is inside the viewport (see updateVideoGeometry),
    // so it cannot spill over the composer or the scroll area's edges.
    m_videoWidget->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_videoWidget->setAspectRatioMode(Qt::KeepAspectRatio);
    m_videoPlayer->setAudioOutput(m_videoAudioOutput);
    m_videoPlayer->setVideoOutput(m_videoWidget);
    connect(m_videoToggle, &QToolButton::clicked, this, &ChatView::toggleVideoPlayback);
    connect(m_videoProgress, &QSlider::sliderMoved, this, [this](int value) {
        if (m_videoPlayer && m_videoPlayer->duration() > 0)
            m_videoPlayer->setPosition(qint64(value) * m_videoPlayer->duration() / 1000);
    });
    connect(m_videoPlayer, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (!m_videoPlayer || !m_videoProgress) return;
        const qint64 duration = m_videoPlayer->duration();
        if (!m_videoProgress->isSliderDown() && duration > 0)
            m_videoProgress->setValue(int(position * 1000 / duration));
        updateVideoTimes(m_videoElapsed, m_videoRemaining, position, duration);
    });
    connect(m_videoPlayer, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        if (!m_videoPlayer || !m_videoProgress) return;
        const qint64 position = m_videoPlayer->position();
        if (duration > 0)
            m_videoProgress->setValue(int(position * 1000 / duration));
        updateVideoTimes(m_videoElapsed, m_videoRemaining, position, duration);
    });
    connect(m_videoPlayer, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        if (!m_videoToggle) return;
        const bool playing = state == QMediaPlayer::PlayingState;
        m_videoToggle->setText(playing ? QStringLiteral("Ⅱ") : QStringLiteral("▶"));
        m_videoToggle->setToolTip(playing ? tr("Pause video") : tr("Play video"));
        m_videoToggle->setAccessibleName(m_videoToggle->toolTip());
    });
    connect(m_videoPlayer, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &error) { emit videoPlaybackError(error); });
    m_videoMessageTs = m_msgs[messageIndex].ts();
    m_videoAttachment = attachmentIndex;
    m_videoLoadingTs = 0;
    m_videoLoadingAttachment = -1;
    m_videoToggle->show();
    m_videoProgress->show();
    m_videoElapsed->show();
    m_videoRemaining->show();
    updateVideoGeometry();
    m_videoPlayer->setSource(QUrl::fromLocalFile(path));
    m_videoPlayer->play();
}

void ChatView::toggleVideoPlayback()
{
    if (!m_videoPlayer) return;
    if (m_videoPlayer->playbackState() == QMediaPlayer::PlayingState)
        m_videoPlayer->pause();
    else {
        if (m_videoPlayer->duration() > 0 && m_videoPlayer->position() >= m_videoPlayer->duration())
            m_videoPlayer->setPosition(0);
        m_videoPlayer->play();
    }
}

void ChatView::stopVideoPlayback()
{
    if (m_videoPlayer) m_videoPlayer->stop();
    if (m_videoPlayer) {
        m_videoPlayer->setVideoOutput(static_cast<QVideoWidget *>(nullptr));
        m_videoPlayer->setAudioOutput(static_cast<QAudioOutput *>(nullptr));
    }
    delete m_videoToggle;
    delete m_videoProgress;
    delete m_videoElapsed;
    delete m_videoRemaining;
    delete m_videoPlayer;
    delete m_videoAudioOutput;
    delete m_videoWidget;
    m_videoToggle = nullptr;
    m_videoProgress = nullptr;
    m_videoElapsed = nullptr;
    m_videoRemaining = nullptr;
    m_videoWidget = nullptr;
    m_videoPlayer = nullptr;
    m_videoAudioOutput = nullptr;
    m_videoMessageTs = 0;
    m_videoAttachment = -1;
    m_videoLoadingTs = 0;
    m_videoLoadingAttachment = -1;
}

QRect ChatView::attachmentRectForVideo() const
{
    if (m_videoAttachment < 0) return {};
    for (int i = 0; i < m_msgs.size() && i < m_items.size(); ++i) {
        if (m_msgs[i].ts() != m_videoMessageTs) continue;
        if (m_videoAttachment >= m_items[i].attRects.size()) return {};
        return m_items[i].attRects[m_videoAttachment]
            .translated(0, m_items[i].y - verticalScrollBar()->value());
    }
    return {};
}

void ChatView::updateVideoGeometry()
{
    if (!m_videoWidget || !m_videoToggle || !m_videoProgress || !m_videoElapsed || !m_videoRemaining) return;
    int index = -1;
    for (int i = 0; i < m_msgs.size(); ++i) {
        if (m_msgs[i].ts() == m_videoMessageTs) { index = i; break; }
    }
    if (index < 0 || m_videoAttachment < 0 || m_videoAttachment >= m_items[index].attRects.size()) {
        stopVideoPlayback();
        return;
    }
    const QRect rect = attachmentRectForVideo();
    viewport()->update();

    // QVideoWidget may use a native video surface that is not clipped like an
    // ordinary child widget. Only expose it when the complete video rectangle
    // is inside the viewport; otherwise hide it before it can overlap the
    // composer or the area outside the scroll view.
    const QRect viewRect = viewport()->rect();
    const bool fullyVisible = viewRect.contains(rect);
    m_videoWidget->setGeometry(rect);
    m_videoWidget->setVisible(fullyVisible);
    if (fullyVisible) m_videoWidget->lower();

    const auto place = [&viewRect, fullyVisible](QWidget *w, const QRect &geometry) {
        w->setGeometry(geometry);
        const bool inside = fullyVisible && viewRect.contains(geometry);
        w->setVisible(inside);
        if (inside) w->raise();
    };
    const int controlsY = rect.bottom() - 46;
    place(m_videoToggle, QRect(QPoint(rect.left() + 6, controlsY), m_videoToggle->size()));
    place(m_videoProgress, QRect(rect.left() + 42, controlsY + 2, qMax(40, rect.width() - 50), 24));
    place(m_videoElapsed, QRect(QPoint(rect.left() + 6, controlsY + 27), m_videoElapsed->size()));
    place(m_videoRemaining, QRect(QPoint(rect.right() - m_videoRemaining->width() - 6, controlsY + 27),
                                  m_videoRemaining->size()));
}

void ChatView::setAnimatedAttachment(int messageIndex, int attachmentIndex, const QByteArray &data)
{
    if (messageIndex < 0 || messageIndex >= m_items.size() || attachmentIndex < 0 ||
        attachmentIndex >= m_msgs[messageIndex].attachments.size() ||
        !(m_msgs[messageIndex].attachments[attachmentIndex].isGif() ||
          m_msgs[messageIndex].attachments[attachmentIndex].isSticker()))
        return;

    Item &item = m_items[messageIndex];
    if (attachmentIndex >= item.movies.size() || item.movies[attachmentIndex] ||
        item.apngMovies[attachmentIndex])
        return;

    // Signal's animated stickers are APNG, and Qt ships no APNG image plugin:
    // a QMovie would freeze on frame one. Drive those frames with ApngMovie.
    if (ApngMovie *apng = ApngMovie::fromData(data, this)) {
        item.apngMovies[attachmentIndex] = apng;
        connect(apng, &ApngMovie::updated, this, [this] { viewport()->update(); });
        apng->start();
        viewport()->update();
        return;
    }

    auto *movie = new QMovie(this);
    auto *buffer = new QBuffer(movie);
    buffer->setData(data);
    buffer->open(QIODevice::ReadOnly);
    movie->setDevice(buffer);
    movie->setCacheMode(QMovie::CacheNone);
    if (!movie->isValid()) {
        delete movie;
        return;
    }
    item.movies[attachmentIndex] = movie;
    connect(movie, &QMovie::frameChanged, this, [this] { viewport()->update(); });
    movie->start();
    viewport()->update();
}

bool ChatView::hasAnimatedAttachment(int messageIndex, int attachmentIndex) const
{
    return messageIndex >= 0 && messageIndex < m_items.size() && attachmentIndex >= 0 &&
           attachmentIndex < m_items[messageIndex].movies.size() &&
           (m_items[messageIndex].movies[attachmentIndex] ||
            m_items[messageIndex].apngMovies[attachmentIndex]);
}

QString ChatView::selectedText() const
{
    if (m_selItem < 0 || !m_selCursor.hasSelection()) return {};
    QString t = m_selCursor.selectedText();
    t.replace(QChar::LineSeparator, QLatin1Char('\n'));
    t.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return t;
}

QString ChatView::plainText() const
{
    QStringList lines;
    for (const Message &m : m_msgs) {
        const QString who = m.outgoing ? tr("You") : (m.sender.isEmpty() ? nameOf(m.authorKey) : m.sender);
        if (m.viewOnce) {
            QString once = m.viewOnceExpired ? tr("View Once media expired")
                           : m.viewOnceViewed ? tr("Viewed")
                           : m.outgoing ? tr("View Once photo sent") : tr("View Once photo");
            lines << (who + QStringLiteral(": ") + once);
            continue;
        }
        if (m.quote.valid) lines << QStringLiteral("> %1: %2").arg(nameOf(m.quote.authorKey), m.quote.text);
        for (const Attachment &a : m.attachments) {
            const QString n = a.filename.isEmpty() ? a.contentType : a.filename;
            if (a.isImage())
                lines << (a.thumb.isEmpty() ? QStringLiteral("[loading image: %1]") : QStringLiteral("[image: %1]")).arg(n);
            else if (a.isVideo())
                lines << QStringLiteral("[video: %1]").arg(n);
            else
                lines << QStringLiteral("[file: %1]").arg(n);
        }
        if (m.deleted) {
            lines << (who + QStringLiteral(": ") + deletedText(m));
            continue;
        }
        if (m.hasPoll) {
            // poll line + one row per option with the current vote count
            const QList<int> counts = m.poll.optionCounts();
            lines << (who + QStringLiteral(": ") + QStringLiteral("[Poll] ") + m.poll.question);
            for (int o = 0; o < m.poll.options.size(); ++o)
                lines << QStringLiteral("  %1 (%2)").arg(m.poll.options.at(o)).arg(counts.value(o, 0));
        } else if (m.hasContact) {
            lines << (who + QStringLiteral(": ") + QStringLiteral("[Contact] ") + m.contactName);
        } else {
            lines << (who + QStringLiteral(": ") + m.text + (m.isEdited() ? tr(" (edited)") : QString()));
        }
        if (m.linkPreview.isValid())
            lines << QStringLiteral("[link preview: %1 — %2]").arg(m.linkPreview.title, m.linkPreview.url);
        if (!m.reactions.isEmpty()) {
            QStringList e = m.reactions.values();
            e.sort();
            lines << QStringLiteral("reactions: ") + e.join(QLatin1Char(' '));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------- layout

void ChatView::buildItem(int i)
{
    Item &it = m_items[i];
    if (it.built) return;
    for (QMovie *movie : it.movies)
        delete movie;
    for (ApngMovie *movie : it.apngMovies)
        delete movie;
    it.movies.clear();
    it.apngMovies.clear();
    it.animationRequested.clear();
    it.built = true;
    const Message &m = m_msgs[i];

    it.timeText = QLocale().toString(m.time.time(), QLocale::ShortFormat);
    if (m.isEdited() && !m.deleted) it.timeText = tr("edited") + QStringLiteral(" · ") + it.timeText;
    it.statusText.clear();
    it.statusRead = m_showReadReceipts && m.receiptStatus >= 2;
    if (m_showDeliveryStatus && m.outgoing)
        it.statusText = m.receiptStatus >= 1 ? QStringLiteral("✓✓") : QStringLiteral("✓");
    if (!m.outgoing) it.sender = m.sender.isEmpty() ? nameOf(m.authorKey) : m.sender;
    if (m.quote.valid) {
        it.quoteAuthor = nameOf(m.quote.authorKey);
        it.quoteText = m.quote.text;
        if (it.quoteText.isEmpty() && !m.quote.attachmentNote.isEmpty())
            it.quoteText = QStringLiteral("📎 ") + m.quote.attachmentNote;
    }

    for (const Attachment &a : m.attachments) {
        it.movies << nullptr;
        it.apngMovies << nullptr;
        it.animationRequested << false;
        QSize imageSize(a.width, a.height);
        if ((imageSize.width() <= 0 || imageSize.height() <= 0) && a.isImage() && !a.thumb.isEmpty()) {
            const QImage image = QImage::fromData(a.thumb);
            imageSize = image.size();
        }
        it.attachmentImageSizes << imageSize;
    }
}

void ChatView::ensureItemRenderResources(int i, bool includePixmaps)
{
    Item &it = m_items[i];
    if (it.renderResourcesBuilt) return;

    const Message &m = m_msgs[i];
    // A resize measurement document may already contain the parsed HTML. Reuse it
    // when painting instead of parsing the same message a second time.
    if (!it.doc && it.measureDoc) {
        it.doc = it.measureDoc;
        if (it.textRect.width() > 0) it.doc->setTextWidth(it.textRect.width());
    }
    if (!it.doc && m.deleted) {
        // "This message was deleted." / "You deleted this message." in a muted italic style
        const QColor fg = m.outgoing ? m_ap.outText : m_ap.inText;
        const QColor bg = m.outgoing ? m_ap.outBg : m_ap.inBg;
        const QColor muted((fg.red() * 7 + bg.red() * 3) / 10, (fg.green() * 7 + bg.green() * 3) / 10,
                           (fg.blue() * 7 + bg.blue() * 3) / 10);
        it.doc = std::make_shared<QTextDocument>();
        it.doc->setDocumentMargin(0);
        it.doc->setDefaultFont(font());
        it.doc->setHtml(QStringLiteral("<i style=\"color: %1\">%2</i>").arg(muted.name(), deletedText(m).toHtmlEscaped()));
        if (it.textRect.width() > 0) it.doc->setTextWidth(it.textRect.width());
    } else if (!it.doc && !m.text.isEmpty()) {
        const QColor fg = m.outgoing ? m_ap.outText : m_ap.inText;
        it.doc = std::make_shared<QTextDocument>();
        it.doc->setDocumentMargin(0);
        it.doc->setDefaultFont(font());
        it.doc->setDefaultStyleSheet(QStringLiteral("a { color: %1; text-decoration: underline; }").arg(fg.name()));
        it.doc->setHtml(styledMessageHtml(m.text, m.textStyles, m.mentions, fg, m_nameOf));
        if (it.textRect.width() > 0) it.doc->setTextWidth(it.textRect.width());
    }
    it.pix.clear();
    if (includePixmaps) {
        for (const Attachment &a : m.attachments)
            it.pix << ((a.isImage() && !a.thumb.isEmpty()) ? QPixmap::fromImage(QImage::fromData(a.thumb)) : QPixmap());
        it.linkPreviewPixmap = m.linkPreview.imageThumb.isEmpty()
                                   ? QPixmap()
                                   : QPixmap::fromImage(QImage::fromData(m.linkPreview.imageThumb));
        if (!m.contactAvatar.isEmpty()) {
            const QImage avatar = QImage::fromData(QByteArray::fromBase64(m.contactAvatar.toLatin1()));
            if (!avatar.isNull()) {
                // center-cropped square, like the official clients' contact tile
                QImage square = avatar.scaled(96, 96, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                if (square.width() > square.height())
                    square = square.copy((square.width() - 96) / 2, 0, 96, 96);
                else if (square.height() > square.width())
                    square = square.copy(0, (square.height() - 96) / 2, 96, 96);
                it.contactAvatarPixmap = QPixmap::fromImage(square);
            }
        }
    }
    it.renderResourcesBuilt = true;
}

void ChatView::releaseItemRenderResources(int i)
{
    Item &it = m_items[i];
    it.doc.reset();
    it.pix.clear();
    it.linkPreviewPixmap = QPixmap();
    it.renderResourcesBuilt = false;
}

size_t ChatView::textMeasureKey(const Message &m, int innerMax) const
{
    // The size of the laid-out text depends on the text, its styles, the wrap width and the font.
    // Colours do not matter. Mentions are resolved to display names that can change, so those
    // messages are always measured.
    if (!m.mentions.isEmpty()) return 0;
    // font().key() builds a string, so hash the font once and only redo it when font or DPI change.
    const int dpi = logicalDpiX() * 100003 + logicalDpiY() * 101 + int(devicePixelRatioF() * 100);
    if (m_measureFontHash == 0 || m_measureDpi != dpi || m_measureFont != font()) {
        m_measureFont = font();
        m_measureDpi = dpi;
        m_measureFontHash = qHashMulti(size_t(0x9e3779b9), m_measureFont.key(), dpi);
    }
    size_t h = qHashMulti(m_measureFontHash, innerMax, int(m.deleted));
    if (m.deleted) {
        h = qHashMulti(h, int(m.outgoing));
    } else {
        h = qHashMulti(h, m.text);
        for (const TextStyleRange &r : m.textStyles) h = qHashMulti(h, r.start, r.length, r.style);
    }
    return h ? h : 1;
}

void ChatView::layoutItem(int i, int y, bool cacheMeasureDoc)
{
    buildItem(i);
    // The text document is only built when the size is not cached (see below).
    Item &it = m_items[i];
    const Message &m = m_msgs[i];
    const int W = viewport()->width();
    it.resizePreviewShiftX = 0;
    it.layoutW = W;
    if (!cacheMeasureDoc) it.measureDoc.reset();

    const QFont base = font();
    const QFont small = scaled(base, 0.85);
    const QFont smallBold = scaled(base, 0.9, true);
    const QFontMetrics fm(base), fms(small), fmb(smallBold);

    it.y = y;
    it.dateRect = it.bubble = it.senderRect = it.quoteBox = it.quoteAuthorRect = it.quoteTextRect = QRect();
    it.textRect = it.timeRect = it.statusRect = it.pillsRect = QRect();
    it.linkPreviewRect = it.linkPreviewImageRect = it.linkPreviewTitleRect =
        it.linkPreviewDescriptionRect = it.linkPreviewHostRect = QRect();
    it.pollRect = it.pollStatusRect = it.pollQuestionRect = it.pollFooterRect = QRect();
    it.pollOptionRects.clear();
    it.pollOptionTexts.clear();
    it.contactRect = it.contactButtonRect = QRect();
    it.contactAvatarPixmap = QPixmap();
    it.attRects.clear();
    it.pills.clear();
    it.dateLabel.clear();

    int cy = 0;
    const bool newDay = i == 0 || m_msgs[i - 1].time.date() != m.time.date();
    if (newDay) {
        const QDate d = m.time.date();
        if (d == QDate::currentDate()) it.dateLabel = tr("Today");
        else if (d == QDate::currentDate().addDays(-1)) it.dateLabel = tr("Yesterday");
        else it.dateLabel = QLocale().toString(d, QLocale::LongFormat);
        it.dateRect = QRect(0, cy + 8, W, fms.height() + 6);
        cy += 8 + fms.height() + 6 + 6;
    }
    const bool grouped = i > 0 && !newDay && m_msgs[i - 1].outgoing == m.outgoing &&
                         (m.outgoing || m_msgs[i - 1].sender == m.sender) &&
                         qAbs(m_msgs[i - 1].time.secsTo(m.time)) < 180;
    it.showSender = m_group && !m.outgoing && !grouped && !it.sender.isEmpty();
    cy += (i == 0 || newDay) ? 4 : (grouped ? 2 : 10);

    const int maxBubble = qMax(160, qMin(int(W * 0.75), 560));
    const int innerMax = maxBubble - 2 * kPadX;

    // ---- pass 1: sizes
    const int statusW = (m_showDeliveryStatus && m.outgoing)
        ? fms.horizontalAdvance(QStringLiteral("✓✓")) : 0;
    int contentW = fms.horizontalAdvance(it.timeText);
    if (statusW > 0) contentW += 5 + statusW;
    int senderW = 0, senderH = 0;
    QString senderShown;
    if (it.showSender) {
        senderShown = fmb.elidedText(it.sender, Qt::ElideRight, innerMax);
        senderW = fmb.horizontalAdvance(senderShown);
        senderH = fmb.height();
        contentW = qMax(contentW, senderW);
    }
    int quoteH = 0, quoteTextH = 0, quoteAuthW = 0;
    QString quoteAuthShown;
    if (m.quote.valid) {
        quoteAuthShown = fmb.elidedText(it.quoteAuthor, Qt::ElideRight, innerMax - 16);
        quoteAuthW = fmb.horizontalAdvance(quoteAuthShown);
        it.quoteText = fitLines(fms, it.quoteText, innerMax - 16, 2);
        const QRect br = fms.boundingRect(QRect(0, 0, innerMax - 16, 100000), Qt::TextWordWrap, it.quoteText);
        quoteTextH = it.quoteText.isEmpty() ? 0 : br.height();
        quoteH = 6 + fmb.height() + (quoteTextH ? 2 + quoteTextH : 0) + 6;
        contentW = qMax(contentW, qMin(innerMax, qMax(quoteAuthW, br.width()) + 16));
    }
    QList<QSize> attSizes;
    if (m.viewOnce) {
        const QString label = (m.viewOnceViewed || m.viewOnceExpired)
                                  ? (m.viewOnceExpired ? tr("View Once media expired") : tr("Viewed"))
                                  : (m.outgoing ? tr("View Once photo sent") : tr("View Once photo · Tap to view"));
        attSizes << QSize(qMin(innerMax, qMax(210, fmb.horizontalAdvance(label) + 58)), 54);
        contentW = qMax(contentW, attSizes.last().width());
    } else {
        for (int a = 0; a < m.attachments.size(); ++a) {
            const Attachment &att = m.attachments[a];
            QSize sz;
            const QSize imageSize = it.attachmentImageSizes[a];
            if (att.isImage() && !att.thumb.isEmpty() && imageSize.width() > 0 && imageSize.height() > 0) {
                const int maxDim = att.isSticker() ? 180 : kMaxImage;
                const int maxHeight = att.isSticker() ? 180 : 360;
                int pw = qMin(qMin(maxDim, innerMax), imageSize.width());
                int ph = imageSize.height() * pw / qMax(1, imageSize.width());
                if (ph > maxHeight) {
                    pw = pw * maxHeight / ph;
                    ph = maxHeight;
                }
                sz = QSize(qMax(att.isSticker() ? 1 : 40, pw), qMax(att.isSticker() ? 1 : 30, ph));
            } else if (att.isVideo()) {
                const int width = qMin(320, innerMax);
                sz = QSize(width, qMax(76, width * 9 / 16));
            } else {
                const QString name = att.filename.isEmpty() ? att.contentType : att.filename;
                const int tw = qMax(fm.horizontalAdvance(fm.elidedText(name, Qt::ElideMiddle, innerMax - 44)),
                                    fms.horizontalAdvance(AttachmentUtil::humanSize(att.size)));
                sz = QSize(qMin(innerMax, qMax(170, tw + (att.isAudio() ? 64 : 44))), att.isAudio() ? 56 : 40);
            }
            attSizes << sz;
            contentW = qMax(contentW, sz.width());
        }
    }
    QSize textSz;
    const bool hasText = m.deleted || !m.text.isEmpty();
    if (hasText) {
        const size_t cacheKey = textMeasureKey(m, innerMax);
        const auto cached = cacheKey ? m_textSizeCache.constFind(cacheKey) : m_textSizeCache.constEnd();
        if (cached != m_textSizeCache.constEnd()) {
            textSz = cached.value();
        } else {
            ensureItemRenderResources(i, false);
            if (it.doc) {
                it.doc->setTextWidth(innerMax);
                const int ideal = int(std::ceil(it.doc->idealWidth()));
                it.doc->setTextWidth(qMin(innerMax, ideal + 1));
                textSz = QSize(qMin(innerMax, ideal + 1), int(std::ceil(it.doc->size().height())));
            }
            if (cacheKey && it.doc) {
                if (m_textSizeCache.size() >= 50000) m_textSizeCache.clear(); // bound the memory
                m_textSizeCache.insert(cacheKey, textSz);
            }
        }
        if (cacheMeasureDoc && it.doc) it.measureDoc = it.doc;
        contentW = qMax(contentW, textSz.width());
    }

    // Signal poll (like the official clients): status line, question, one row per option
    // with a vote bar, and a total at the bottom. Rows are clickable to vote.
    int pollStatusH = 0, pollQuestionH = 0, pollFooterH = 0;
    int pollW = 0;
    if (!m.deleted && m.hasPoll) {
        pollW = qMin(innerMax, 320);
        const int textWidth = qMax(1, pollW - 16);
        pollStatusH = fms.height();
        const QFont questionFont = scaled(base, 1.05, true);
        const QFontMetrics fmq(questionFont);
        pollQuestionH = fmq.boundingRect(QRect(0, 0, textWidth, 10000), Qt::TextWordWrap, m.poll.question).height();
        const QFont optionFont = scaled(base, 0.95, true);
        const QFontMetrics fbo(optionFont);
        for (int o = 0; o < m.poll.options.size(); ++o) {
            const QString fitted = fitLines(fbo, m.poll.options.at(o), textWidth - 44, 2);
            const int textH = fbo.boundingRect(QRect(0, 0, textWidth - 44, 10000), Qt::TextWordWrap, fitted).height();
            // one row: option text lines + the vote bar below
            it.pollOptionRects << QRect(0, 0, pollW, 6 + textH + 6 + 4 + 6);
            it.pollOptionTexts << fitted;
        }
        pollFooterH = fms.height();
        contentW = qMax(contentW, pollW);
    }

    // Contact card (like the official clients): avatar left, name and phone number right,
    // and the "Message" button below that opens the chat with that contact.
    int contactH = 0, contactW = 0;
    if (!m.deleted && m.hasContact) {
        contactW = qMin(innerMax, 280);
        const QFont contactNameFont = scaled(base, 1.0, true);
        const QFontMetrics fmn(contactNameFont);
        const int nameH = fmn.boundingRect(QRect(0, 0, contactW - 72, 10000), Qt::TextWordWrap, m.contactName).height();
        const int textH = nameH + (m.contactNumber.isEmpty() ? 0 : fms.height() + 2);
        // Like the official clients the "Message" button is only shown when the contact
        // can actually be messaged: the card carries a phone number or a known serviceId.
        const bool contactHasButton = !m.contactNumber.isEmpty() || !m.contactUuid.isEmpty();
        const int contactButtonH = contactHasButton ? fms.height() + 12 : 0;
        contactH = 12 + qMax(48, textH) + (contactHasButton ? 10 + contactButtonH : 0) + 12;
        contentW = qMax(contentW, contactW);
    }

    int linkPreviewW = 0, linkPreviewH = 0, linkPreviewImageH = 0;
    int linkPreviewTitleH = 0, linkPreviewDescriptionH = 0, linkPreviewHostH = 0;
    if (!m.deleted && !m.viewOnce && m.linkPreview.isValid()) {
        linkPreviewW = qMin(innerMax, 320);
        const int textWidth = qMax(1, linkPreviewW - 16);
        it.linkPreviewTitle = fitLines(fmb, m.linkPreview.title, textWidth, 2);
        it.linkPreviewDescription = fitLines(fms, m.linkPreview.description, textWidth, 2);
        it.linkPreviewHost = QUrl(m.linkPreview.url).host();
        linkPreviewTitleH = fmb.boundingRect(QRect(0, 0, textWidth, 10000), Qt::TextWordWrap, it.linkPreviewTitle).height();
        linkPreviewDescriptionH = it.linkPreviewDescription.isEmpty() ? 0
            : fms.boundingRect(QRect(0, 0, textWidth, 10000), Qt::TextWordWrap, it.linkPreviewDescription).height();
        linkPreviewHostH = fms.height();
        if (!m.linkPreview.imageThumb.isEmpty()) linkPreviewImageH = qBound(76, linkPreviewW * 9 / 16, 150);
        linkPreviewH = 8 + linkPreviewImageH + (linkPreviewImageH > 0 ? 6 : 0) + linkPreviewTitleH +
                       (linkPreviewDescriptionH > 0 ? 4 + linkPreviewDescriptionH : 0) + 2 + linkPreviewHostH + 8;
        contentW = qMax(contentW, linkPreviewW);
    }

    const int bubbleW = contentW + 2 * kPadX;
    const int bubbleX = m.outgoing ? W - kMargin - bubbleW : kMargin;
    const int x0 = bubbleX + kPadX;

    // ---- pass 2: positions
    int yb = cy + kPadY;
    if (it.showSender) {
        it.senderRect = QRect(x0, yb, senderW, senderH);
        it.sender = senderShown; // what is drawn
        yb += senderH + 2;
    }
    if (m.quote.valid) {
        it.quoteBox = QRect(x0, yb, contentW, quoteH);
        it.quoteAuthorRect = QRect(x0 + 10, yb + 6, contentW - 16, fmb.height());
        it.quoteTextRect = QRect(x0 + 10, yb + 6 + fmb.height() + 2, contentW - 16, quoteTextH);
        it.quoteAuthor = quoteAuthShown;
        yb += quoteH + kGap;
    }
    if (!m.deleted && m.hasPoll) {
        int py = yb;
        it.pollStatusRect = QRect(x0, py, pollW, pollStatusH);
        py += pollStatusH + 4;
        it.pollQuestionRect = QRect(x0, py, pollW, pollQuestionH);
        py += pollQuestionH + 8;
        for (QRect &row : it.pollOptionRects) {
            row = QRect(x0, py, pollW, row.height());
            py += row.height() + 6;
        }
        it.pollFooterRect = QRect(x0, py, pollW, pollFooterH);
        py += pollFooterH;
        it.pollRect = QRect(x0, yb, pollW, py - yb);
        yb = py + kGap;
    }
    if (!m.deleted && m.hasContact) {
        it.contactRect = QRect(x0, yb, contactW, contactH);
        if (!m.contactNumber.isEmpty() || !m.contactUuid.isEmpty()) {
            const int contactButtonH = fms.height() + 12;
            // the full-width "Message" pill below the card content, like the official clients
            it.contactButtonRect = QRect(x0 + 12, yb + contactH - 12 - contactButtonH,
                                         contactW - 24, contactButtonH);
        }
        yb += contactH + kGap;
    }
    for (const QSize &sz : attSizes) {
        it.attRects << QRect(x0, yb, sz.width(), sz.height());
        yb += sz.height() + kGap;
    }
    if (hasText) {
        it.textRect = QRect(x0, yb, textSz.width(), textSz.height());
        yb += textSz.height() + 2;
    }
    if (linkPreviewH > 0) {
        it.linkPreviewRect = QRect(x0, yb, linkPreviewW, linkPreviewH);
        int py = yb + 8;
        if (linkPreviewImageH > 0) {
            it.linkPreviewImageRect = QRect(x0, py, linkPreviewW, linkPreviewImageH);
            py += linkPreviewImageH + 6;
        }
        it.linkPreviewTitleRect = QRect(x0 + 8, py, linkPreviewW - 16, linkPreviewTitleH);
        py += linkPreviewTitleH;
        if (linkPreviewDescriptionH > 0) {
            py += 4;
            it.linkPreviewDescriptionRect = QRect(x0 + 8, py, linkPreviewW - 16, linkPreviewDescriptionH);
            py += linkPreviewDescriptionH;
        }
        py += 2;
        it.linkPreviewHostRect = QRect(x0 + 8, py, linkPreviewW - 16, linkPreviewHostH);
        yb += linkPreviewH + kGap;
    }
    const int timeW = fms.horizontalAdvance(it.timeText);
    if (statusW > 0) {
        it.statusRect = QRect(bubbleX + bubbleW - kPadX - statusW, yb, statusW, fms.height());
        it.timeRect = QRect(it.statusRect.left() - 5 - timeW, yb, timeW, fms.height());
    } else {
        it.timeRect = QRect(bubbleX + bubbleW - kPadX - timeW, yb, timeW, fms.height());
    }
    yb += fms.height();
    // Reaction pills overlap the lower edge of the bubble: reserve room so they never cover the time.
    const int pillH = fms.height() + 8;
    const int reactionRoom = m.reactions.isEmpty() ? 0 : pillH / 2 + 2;
    it.bubble = QRect(bubbleX, cy, bubbleW, yb + kPadY - cy + reactionRoom);

    int height = it.bubble.bottom() + 1;
    if (!m.reactions.isEmpty()) {
        QHash<QString, int> counts;
        for (auto r = m.reactions.cbegin(); r != m.reactions.cend(); ++r) counts[r.value()]++;
        QStringList order = counts.keys();
        std::sort(order.begin(), order.end(), [&](const QString &a, const QString &b) {
            return counts[a] != counts[b] ? counts[a] > counts[b] : a < b;
        });
        const int py = it.bubble.bottom() - pillH / 2;
        int total = 0;
        for (const QString &e : order) {
            const QString text = counts[e] > 1 ? QStringLiteral("%1 %2").arg(e).arg(counts[e]) : e;
            total += fm.horizontalAdvance(text) + 16 + 4;
        }
        int px = m.outgoing ? it.bubble.right() - 10 - total + 4 : it.bubble.left() + 10;
        for (const QString &e : order) {
            Pill p;
            p.text = counts[e] > 1 ? QStringLiteral("%1 %2").arg(e).arg(counts[e]) : e;
            p.own = !m_self.isEmpty() && m.reactions.value(m_self) == e;
            p.rect = QRect(px, py, fm.horizontalAdvance(p.text) + 16, pillH);
            px += p.rect.width() + 4;
            it.pills << p;
            it.pillsRect = it.pillsRect.united(p.rect);
        }
        height = qMax(height, py + pillH + 4);
    }
    it.height = height + 2;
    releaseItemRenderResources(i);
}

void ChatView::relayoutAll(bool cacheMeasureDocs)
{
    cancelRelayout(); // everything below is laid out from scratch
    m_layoutWidth = viewport()->width();
    m_resizePreviewWidth = m_layoutWidth;

    // QTextDocuments are comparatively expensive and can be large. Retain them only
    // for rows the renderer is about to paint (plus the same eight-row safety margin),
    // not for every message in a long conversation.
    int measureFirst = 0;
    int measureLast = -1;
    if (cacheMeasureDocs && !m_items.isEmpty()) {
        const int top = verticalScrollBar()->value();
        const int bottom = top + viewport()->height();
        measureFirst = itemAtY(top);
        if (measureFirst < 0) measureFirst = 0;
        measureLast = measureFirst;
        while (measureLast + 1 < m_items.size() && m_items[measureLast + 1].y < bottom)
            ++measureLast;
        measureFirst = qMax(0, measureFirst - 8);
        measureLast = qMin(m_items.size() - 1, measureLast + 8);
    }

    int y = m_bannerOffset; // room for the pinned-message banner above the first bubble
    for (int i = 0; i < m_items.size(); ++i) {
        const bool keepMeasureDoc = cacheMeasureDocs && i >= measureFirst && i <= measureLast;
        layoutItem(i, y, keepMeasureDoc);
        y += m_items[i].height;
    }
    updateScrollRange();
    viewport()->update();
    updateVideoGeometry();
}

void ChatView::previewResize(int width)
{
    if (m_resizePreviewWidth < 0) m_resizePreviewWidth = m_layoutWidth;
    if (width == m_resizePreviewWidth) return;
    m_resizePreviewWidth = width;
    if (m_layoutWidth <= 0 || m_items.isEmpty()) return;

    // Only touch rows near the viewport in the resize-event path. Newly visible rows
    // are adjusted lazily by paintEvent(), avoiding an O(total messages) mouse-move loop.
    const int top = verticalScrollBar()->value();
    const int bottom = top + viewport()->height();
    int first = itemAtY(top);
    if (first < 0) first = 0;
    int last = first;
    while (last + 1 < m_items.size() && m_items[last + 1].y < bottom)
        ++last;
    previewResizeItems(qMax(0, first - 8), qMin(m_items.size() - 1, last + 8));
    viewport()->update();
}

void ChatView::previewResizeItems(int first, int last)
{
    if (m_layoutWidth <= 0 || m_resizePreviewWidth < 0 || m_items.isEmpty()) return;
    first = qMax(0, first);
    last = qMin(m_items.size() - 1, last);
    if (first > last) return;

    const auto shift = [](QRect &rect, int dx) {
        if (!rect.isNull()) rect.translate(dx, 0);
    };
    for (int i = first; i <= last; ++i) {
        Item &it = m_items[i];
        // Rows that are still waiting for the background relayout were laid out for an older
        // width, so every row is shifted relative to its own layout width.
        if (it.layoutW <= 0) continue;
        const int targetShift = m_resizePreviewWidth - it.layoutW;
        const int dx = targetShift - it.resizePreviewShiftX;
        if (dx != 0 && i < m_msgs.size() && m_msgs[i].outgoing) {
            shift(it.bubble, dx);
            shift(it.senderRect, dx);
            shift(it.quoteBox, dx);
            shift(it.quoteAuthorRect, dx);
            shift(it.quoteTextRect, dx);
            shift(it.textRect, dx);
            shift(it.timeRect, dx);
            shift(it.statusRect, dx);
            shift(it.pillsRect, dx);
            shift(it.linkPreviewRect, dx);
            shift(it.linkPreviewImageRect, dx);
            shift(it.linkPreviewTitleRect, dx);
            shift(it.linkPreviewDescriptionRect, dx);
            shift(it.linkPreviewHostRect, dx);
            shift(it.pollRect, dx);
            shift(it.pollStatusRect, dx);
            shift(it.pollQuestionRect, dx);
            shift(it.pollFooterRect, dx);
            shift(it.contactRect, dx);
            shift(it.contactButtonRect, dx);
            for (QRect &rect : it.attRects) shift(rect, dx);
            for (QRect &rect : it.pollOptionRects) shift(rect, dx);
            for (Pill &pill : it.pills) shift(pill.rect, dx);
        }
        if (!it.dateRect.isNull()) it.dateRect.setWidth(m_resizePreviewWidth);
        it.resizePreviewShiftX = targetShift;
    }
}

void ChatView::updateScrollRange()
{
    verticalScrollBar()->setRange(0, qMax(0, totalHeight() - viewport()->height() + 6));
    verticalScrollBar()->setPageStep(viewport()->height());
}

void ChatView::scrollContentsBy(int dx, int dy)
{
    // When rows off screen change height the anchor moves the scroll bar by exactly as much as
    // the content moved, so the viewport shows the same picture: nothing to repaint.
    if (m_quietScroll) return;
    QAbstractScrollArea::scrollContentsBy(dx, dy);
}

void ChatView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    const int width = viewport()->width();
    const bool widthChanged = width != m_layoutWidth;
    const bool previewNeedsReset = m_resizePreviewWidth >= 0 && width != m_resizePreviewWidth;
    if (widthChanged || previewNeedsReset) {
        if (m_items.isEmpty() || m_layoutWidth <= 0) {
            // Nothing has been laid out yet.
            m_layoutWidth = m_resizePreviewWidth = width;
            updateScrollRange();
        } else {
            if (!m_relayoutActive) {
                captureResizeAnchor();
                m_relayoutActive = true;
            }
            // Slices of the background pass belong to the previous width.
            m_relayoutTimer->stop();
            m_bgStarted = false;
            m_layoutWidth = m_resizePreviewWidth = width;
            relayoutWindow();
            m_resizeTimer->start();
            m_resizeCleanupTimer->start();
        }
    } else {
        const bool bottom = atBottom();
        const int old = verticalScrollBar()->value();
        updateScrollRange();
        verticalScrollBar()->setValue(bottom ? verticalScrollBar()->maximum() : old);
    }
    updateToLatestButton();
    updateVideoGeometry();
}

// ------------------------------------------------- incremental relayout (resize)
//
// A width change invalidates the wrapping of every message, but only the rows on screen need
// to be right immediately. relayoutWindow() lays those out (a few milliseconds, so the text
// re-wraps live while the edge is dragged); relayoutSlice() then catches up with the rest of
// the conversation in short time boxes. Rows still waiting are drawn as before the resize,
// shifted to the new right edge (previewResizeItems()). Item heights change as rows are
// brought up to date, so a scroll anchor keeps what the user is looking at in place.

void ChatView::reflowPositions()
{
    // Cheap (one addition per row): the expensive part of a layout is measuring the text.
    int y = m_bannerOffset;
    for (Item &it : m_items) {
        it.y = y;
        y += it.height;
    }
}

void ChatView::captureResizeAnchor()
{
    m_anchorBottom = m_items.isEmpty() || atBottom();
    if (m_anchorBottom) return;
    const int value = verticalScrollBar()->value();
    int idx = itemAtY(value);
    if (idx < 0) idx = m_items.size() - 1;
    m_anchorIndex = idx;
    m_anchorOffset = m_items[idx].y - value;
}

void ChatView::applyResizeAnchor()
{
    QScrollBar *bar = verticalScrollBar();
    int target = bar->maximum();
    if (!m_anchorBottom && m_anchorIndex >= 0 && m_anchorIndex < m_items.size())
        target = m_items[m_anchorIndex].y - m_anchorOffset;
    ++m_progScroll;
    bar->setValue(qBound(0, target, bar->maximum()));
    --m_progScroll;
}

void ChatView::commitRelayout(bool repaint)
{
    // updateScrollRange() may clamp the value and so emit valueChanged(); that is not the
    // user scrolling and must not move the anchor.
    m_quietScroll = !repaint;
    ++m_progScroll;
    reflowPositions();
    updateScrollRange();
    applyResizeAnchor();
    --m_progScroll;
    m_quietScroll = false;
    updateVideoGeometry();
    updateToLatestButton();
    // Rows above or below the screen changing height moves nothing that is visible (the
    // anchor sees to that), so the background pass does not repaint.
    if (repaint) viewport()->update();
}

bool ChatView::relayoutVisibleStale()
{
    const int count = m_items.size();
    if (count == 0) return false;
    const int top = verticalScrollBar()->value();
    const int bottom = top + viewport()->height();
    int first = itemAtY(top);
    if (first < 0) first = count - 1;
    int last = first;
    while (last + 1 < count && m_items[last + 1].y < bottom) ++last;
    first = qMax(0, first - 2);
    last = qMin(count - 1, last + 2);
    bool any = false;
    for (int i = first; i <= last; ++i) {
        if (!isStale(i)) continue;
        layoutItem(i, m_items[i].y, true);
        any = true;
    }
    return any;
}

void ChatView::relayoutWindow()
{
    const int count = m_items.size();
    if (count == 0) return;

    // The part of the document that will be on screen, in the coordinates of the layout that
    // is still in place (y and height are consistent with each other there).
    const int viewH = viewport()->height();
    const int margin = 160;
    int docTop;
    if (m_anchorBottom || m_anchorIndex < 0 || m_anchorIndex >= count)
        docTop = qMax(0, totalHeight() + 6 - viewH);
    else
        docTop = m_items[m_anchorIndex].y - m_anchorOffset;
    int first = itemAtY(qMax(0, docTop - margin));
    if (first < 0) first = count - 1; // below the last row; never "everything"
    int last = first;
    const int docBottom = docTop + viewH + margin;
    while (last + 1 < count && m_items[last + 1].y < docBottom) ++last;
    if (m_anchorBottom) last = count - 1;
    first = qMax(0, first - 1);
    last = qMin(count - 1, last + 1);

    for (int i = first; i <= last; ++i)
        if (isStale(i)) layoutItem(i, m_items[i].y, true); // rows that are current stay as they are
    m_windowFirst = first;
    m_windowLast = last;
    commitRelayout(true);

    // New heights can pull rows into view that were outside the window.
    for (int guard = 0; guard < 12 && relayoutVisibleStale(); ++guard)
        commitRelayout(true);
}

void ChatView::beginLazyLayout()
{
    const int count = m_items.size();
    if (count == 0) return;
    m_layoutWidth = m_resizePreviewWidth = viewport()->width();

    // Rows that have not been measured yet (layoutW == -1) get a height estimated from the
    // newest ones, so the scroll bar is plausible from the start.
    const int sample = qMin(count, 12);
    for (Item &it : m_items) it.height = 64;
    reflowPositions();
    int sum = 0;
    for (int i = count - sample; i < count; ++i) {
        layoutItem(i, m_items[i].y, true);
        sum += m_items[i].height;
    }
    const int estimate = qMax(24, sum / sample);
    for (int i = 0; i < count - sample; ++i) m_items[i].height = estimate;
    reflowPositions(); // y and height must agree before the window is chosen from them

    m_anchorBottom = true;
    m_relayoutActive = true;
    m_bgStarted = false;
    relayoutWindow();
    m_resizeCleanupTimer->start(); // drop the measuring documents again once things have settled
    m_relayoutTimer->start();
}

void ChatView::relayoutSlice()
{
    if (!m_relayoutActive) return;
    constexpr int kSliceMs = 4; // well below a frame: input and painting stay responsive
    QElapsedTimer clock;
    clock.start();

    // The user may have scrolled into rows that are still waiting.
    const bool visibleChanged = relayoutVisibleStale();
    bool changed = visibleChanged;

    const int count = m_items.size();
    if (!m_bgStarted) {
        m_bgStarted = true;
        m_bgUp = m_windowFirst - 1;
        m_bgDown = m_windowLast + 1;
    }
    const auto nextDown = [&] {
        while (m_bgDown < count && !isStale(m_bgDown)) ++m_bgDown;
        return m_bgDown < count ? m_bgDown++ : -1;
    };
    const auto nextUp = [&] {
        while (m_bgUp >= 0 && m_bgUp < count && !isStale(m_bgUp)) --m_bgUp;
        return (m_bgUp >= 0 && m_bgUp < count) ? m_bgUp-- : -1;
    };
    bool exhausted = false;
    bool downTurn = true;
    while (true) {
        int i = downTurn ? nextDown() : nextUp();
        if (i < 0) i = downTurn ? nextUp() : nextDown();
        if (i < 0) {
            exhausted = true;
            break;
        }
        downTurn = !downTurn;
        layoutItem(i, m_items[i].y, false);
        changed = true;
        if (clock.elapsed() >= kSliceMs) break;
    }
    if (changed) commitRelayout(visibleChanged);

    if (exhausted) {
        m_relayoutActive = false;
        m_bgStarted = false;
    } else {
        m_relayoutTimer->start();
    }
}

void ChatView::finishRelayout()
{
    if (!m_relayoutActive) return;
    m_resizeTimer->stop();
    m_relayoutTimer->stop();
    for (int i = 0; i < m_items.size(); ++i)
        if (isStale(i)) layoutItem(i, m_items[i].y, false);
    commitRelayout(true);
    m_relayoutActive = false;
    m_bgStarted = false;
}

void ChatView::cancelRelayout()
{
    if (m_resizeTimer) m_resizeTimer->stop();
    if (m_relayoutTimer) m_relayoutTimer->stop();
    m_relayoutActive = false;
    m_bgStarted = false;
}

int ChatView::itemAtY(int docY) const
{
    if (m_items.isEmpty() || docY < 0) return m_items.isEmpty() ? -1 : (docY < 0 ? 0 : -1);
    int lo = 0, hi = m_items.size() - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (m_items[mid].y <= docY) lo = mid; else hi = mid - 1;
    }
    return docY < m_items[lo].y + m_items[lo].height ? lo : -1;
}

// -------------------------------------------------------------- painting

void ChatView::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    p.fillRect(viewport()->rect(), m_ap.background);
    if (m_items.isEmpty()) {
        drawPinBanner(p); // a pinned message can outlive the messages shown (e.g. after a delete)
        return;
    }

    const int top = verticalScrollBar()->value();
    const int bottom = top + viewport()->height();
    int first = itemAtY(top);
    if (first < 0) first = 0;
    int last = first;
    while (last + 1 < m_items.size() && m_items[last + 1].y < bottom) ++last;

    const int resourceFirst = qMax(0, first - 8);
    const int resourceLast = qMin(m_items.size() - 1, last + 8);
    for (int i = m_renderFirst; i >= 0 && i <= m_renderLast; ++i)
        if (i < resourceFirst || i > resourceLast) releaseItemRenderResources(i);
    m_renderFirst = resourceFirst;
    m_renderLast = resourceLast;
    previewResizeItems(resourceFirst, resourceLast);

    for (int i = first; i <= last; ++i)
        if (m_items[i].layoutW > 0) ensureItemRenderResources(i);
    for (int i = first; i <= last; ++i) {
        if (m_items[i].layoutW <= 0) continue; // still waiting for the background layout
        p.save();
        p.translate(0, m_items[i].y - top);
        paintItem(p, i);
        p.restore();
    }
    // Last, so bubbles scrolling underneath never show through the fixed banner.
    drawPinBanner(p);
}

void ChatView::paintItem(QPainter &p, int i)
{
    const Item &it = m_items[i];
    const Message &m = m_msgs[i];
    const QColor bg = m.outgoing ? m_ap.outBg : m_ap.inBg;
    const QColor fg = m.outgoing ? m_ap.outText : m_ap.inText;
    const QFont base = font();
    const QFont small = scaled(base, 0.85);
    const QFont smallBold = scaled(base, 0.9, true);

    if (!it.dateLabel.isEmpty()) {
        p.setFont(small);
        p.setPen(mutedOn(m_ap.background));
        p.drawText(it.dateRect, Qt::AlignCenter, it.dateLabel);
    }

    // bubble
    const qreal r = qMin<qreal>(m_ap.radius, qMin(it.bubble.height(), it.bubble.width()) / 2.0);
    QPainterPath path;
    path.addRoundedRect(QRectF(it.bubble), r, r);
    p.fillPath(path, bg);
    if (m_flash == i) {
        p.setPen(QPen(alpha(m_ap.outBg, 200), 2));
        p.drawPath(path);
    }

    if (it.showSender) {
        const int hue = int(qHash(m.sender) % 360);
        p.setFont(smallBold);
        p.setPen(QColor::fromHsv(hue, 170, bg.lightness() < 128 ? 235 : 150));
        p.drawText(it.senderRect, Qt::AlignLeft | Qt::AlignVCenter, it.sender);
    }

    if (m.quote.valid) {
        QPainterPath qp;
        const qreal qr = qMin<qreal>(10, qMin(it.quoteBox.height(), it.quoteBox.width()) / 2.0);
        qp.addRoundedRect(QRectF(it.quoteBox), qr, qr);
        p.fillPath(qp, alpha(fg, 34));
        p.save();
        p.setClipPath(qp);
        p.fillRect(QRect(it.quoteBox.left(), it.quoteBox.top(), 4, it.quoteBox.height()), alpha(fg, 210));
        p.restore();
        p.setFont(smallBold);
        p.setPen(fg);
        p.drawText(it.quoteAuthorRect, Qt::AlignLeft | Qt::AlignVCenter, it.quoteAuthor);
        if (!it.quoteText.isEmpty()) {
            p.setFont(small);
            p.setPen(alpha(fg, 225));
            p.drawText(it.quoteTextRect, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, it.quoteText);
        }
    }

    if (!m.deleted && m.hasPoll) {
        // status line ("Poll · Select one", ...) and the question, like the official clients
        p.setFont(small);
        p.setPen(alpha(fg, 170));
        p.drawText(it.pollStatusRect, Qt::AlignLeft | Qt::AlignVCenter,
                   m.poll.terminated ? tr("Poll · Final results")
                                     : m.poll.allowMultiple ? tr("Poll · Select one or more")
                                                            : tr("Poll · Select one"));
        p.setFont(scaled(base, 1.05, true));
        p.setPen(fg);
        p.drawText(it.pollQuestionRect, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, m.poll.question);

        const QList<int> counts = m.poll.optionCounts();
        const int totalVotes = std::accumulate(counts.cbegin(), counts.cend(), 0);
        const QList<int> ownVote = m.poll.voteOf(m_self);
        const QFont countFont = scaled(base, 0.85);
        const QFont optionFontPaint = scaled(base, 0.95, true);
        const QFont optionMuted = scaled(base, 0.95);
        const int top = m.poll.topCount();
        for (int o = 0; o < it.pollOptionRects.size() && o < m.poll.options.size(); ++o) {
            const QRect row = it.pollOptionRects.at(o);
            const bool own = ownVote.contains(o);
            const int count = counts.value(o, 0);
            const int pct = totalVotes > 0 ? qRound(100.0 * count / totalVotes) : 0;
            const bool leading = m.poll.terminated && top > 0 && count == top;

            // the row itself
            QPainterPath rp;
            rp.addRoundedRect(QRectF(row), 8, 8);
            p.fillPath(rp, alpha(fg, 26));
            if (own) {
                p.setPen(QPen(alpha(fg, 200), 1));
                p.setBrush(Qt::NoBrush);
                p.drawPath(rp);
            }
            // vote bar along the lower edge of the row
            const QRect bar(row.left() + 8, row.bottom() - 9, row.width() - 16, 4);
            p.setPen(Qt::NoPen);
            p.setBrush(alpha(fg, 40));
            p.drawRoundedRect(bar, 2, 2);
            if (count > 0 && totalVotes > 0) {
                QRect fill = bar;
                fill.setWidth(qMax(4, qRound(bar.width() * qBound<qreal>(0.0, qreal(count) / totalVotes, 1.0))));
                p.setBrush(alpha(m.outgoing ? m_ap.outText : m_ap.inText, leading ? 255 : 130));
                p.drawRoundedRect(fill, 2, 2);
            }
            // check mark of our own vote, then the option text, then the percentage
            if (own) {
                p.setBrush(alpha(m.outgoing ? m_ap.outText : m_ap.inText, 230));
                p.setPen(Qt::NoPen);
                p.drawEllipse(QRect(row.left() + 8, row.top() + 6, 14, 14));
                p.setFont(scaled(base, 0.75, true));
                p.setPen(m.outgoing ? m_ap.outBg : m_ap.inBg);
                p.drawText(QRect(row.left() + 8, row.top() + 6, 14, 14), Qt::AlignCenter, QStringLiteral("✓"));
            }
            p.setFont(own ? optionFontPaint : optionMuted);
            p.setPen(alpha(fg, own ? 255 : 215));
            const int textLeft = row.left() + (own ? 28 : 8);
            p.drawText(QRect(textLeft, row.top() + 6, row.width() - (textLeft - row.left()) - 40, row.height() - 22),
                       Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, it.pollOptionTexts.value(o));
            p.setFont(countFont);
            p.setPen(alpha(fg, 190));
            p.drawText(QRect(row.right() - 40, row.top() + 6, 34, row.height() - 22),
                       Qt::AlignRight | Qt::AlignTop, QStringLiteral("%1%").arg(pct));
        }

        // vote total at the bottom ("No votes" like in the official clients)
        p.setFont(small);
        p.setPen(alpha(fg, 170));
        p.drawText(it.pollFooterRect, Qt::AlignLeft | Qt::AlignVCenter,
                   totalVotes == 0 ? tr("No votes")
                                   : tr("%n vote(s)", "", totalVotes));
    }

    if (!m.deleted && m.hasContact) {
        // the card itself: rounded tile, avatar left, name + phone number right
        QPainterPath cardPath;
        cardPath.addRoundedRect(QRectF(it.contactRect), 10, 10);
        p.fillPath(cardPath, alpha(fg, 26));

        const QRect avatarRect(it.contactRect.left() + 12, it.contactRect.top() + 12, 48, 48);
        if (!it.contactAvatarPixmap.isNull()) {
            QPainterPath avatarClip;
            avatarClip.addEllipse(QRectF(avatarRect));
            p.save();
            p.setClipPath(avatarClip);
            p.drawPixmap(avatarRect, it.contactAvatarPixmap);
            p.restore();
        } else {
            // placeholder: initial letter on a tinted circle
            p.setPen(Qt::NoPen);
            p.setBrush(alpha(m.outgoing ? m_ap.outText : m_ap.inText, 60));
            p.drawEllipse(avatarRect);
            p.setFont(scaled(base, 1.1, true));
            p.setPen(fg);
            p.drawText(avatarRect, Qt::AlignCenter, m.contactName.left(1).toUpper());
        }

        const QFontMetrics fnum(small);
        const bool contactHasButton = !it.contactButtonRect.isEmpty();
        const int contactButtonH = contactHasButton ? fnum.height() + 12 : 0;
        const QRect textRect(it.contactRect.left() + 72, it.contactRect.top() + 12,
                             it.contactRect.width() - 84,
                             it.contactRect.height() - 24 - (contactHasButton ? 10 + contactButtonH : 0));
        const QFont nameFont = scaled(base, 1.0, true);
        const QFontMetrics fmn(nameFont);
        const QString nameFitted = fmn.elidedText(m.contactName, Qt::ElideRight, textRect.width());
        const int nameH = fmn.boundingRect(QRect(0, 0, textRect.width(), 10000),
                                           Qt::TextWordWrap, nameFitted).height();
        const int numH = m.contactNumber.isEmpty() ? 0 : fnum.height();
        const int blockTop = textRect.top() + qMax(0, (textRect.height() - nameH - (numH > 0 ? 2 + numH : 0)) / 2);
        p.setFont(nameFont);
        p.setPen(fg);
        p.drawText(QRect(textRect.left(), blockTop, textRect.width(), nameH),
                   Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, nameFitted);
        if (numH > 0) {
            p.setFont(small);
            p.setPen(alpha(fg, 170));
            p.drawText(QRect(textRect.left(), blockTop + nameH + 2, textRect.width(), numH),
                       Qt::AlignLeft | Qt::AlignVCenter, m.contactNumber);
        }

        if (contactHasButton) {
            // the "Message" pill below the card content, like the official clients
            QPainterPath buttonPath;
            const qreal buttonRadius = it.contactButtonRect.height() / 2.0;
            buttonPath.addRoundedRect(QRectF(it.contactButtonRect), buttonRadius, buttonRadius);
            p.setPen(Qt::NoPen);
            p.setBrush(m_ap.inText);
            p.drawPath(buttonPath);
            p.setFont(small);
            p.setPen(m_ap.inBg);
            p.drawText(it.contactButtonRect, Qt::AlignCenter, tr("Message"));
        }
    }

    if (m.viewOnce && !it.attRects.isEmpty()) {
        const QRect rc = it.attRects.first();
        QPainterPath oncePath;
        oncePath.addRoundedRect(QRectF(rc), 10, 10);
        p.fillPath(oncePath, alpha(fg, 30));
        p.setPen(fg);
        QFont iconFont = base;
        iconFont.setBold(true);
        p.setFont(iconFont);
        p.drawText(QRect(rc.left() + 10, rc.top(), 38, rc.height()), Qt::AlignCenter, QStringLiteral("1×"));
        p.setFont(smallBold);
        QString onceLabel;
        if (m.viewOnceExpired) onceLabel = tr("View Once media expired");
        else if (m.viewOnceViewed) onceLabel = tr("Viewed");
        else if (m.outgoing) onceLabel = tr("View Once photo sent");
        else onceLabel = tr("View Once photo · Tap to view");
        p.drawText(QRect(rc.left() + 48, rc.top() + 3, rc.width() - 56, rc.height() - 6),
                   Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(smallBold).elidedText(onceLabel, Qt::ElideRight, rc.width() - 56));
    } else for (int a = 0; a < m.attachments.size() && a < it.attRects.size(); ++a) {
        const QRect rc = it.attRects[a];
        const Attachment &att = m.attachments[a];
        if ((att.isGif() || att.isSticker()) && !it.movies[a] && !it.apngMovies[a] &&
            !it.animationRequested[a]) {
            it.animationRequested[a] = true;
            emit animatedAttachmentRequested(i, a);
        }
        if (it.movies[a] || it.apngMovies[a]) {
            QPixmap frame = it.movies[a] ? it.movies[a]->currentPixmap()
                                         : it.apngMovies[a]->currentPixmap();
            if (!frame.isNull()) {
                if (att.isSticker()) {
                    // Stickers preserve their transparent edges, unlike regular image
                    // attachments — draw them without the bubble rounding.
                    p.drawPixmap(rc, frame);
                } else {
                    QPainterPath ip;
                    const qreal ir = qMin<qreal>(qMin(m_ap.radius, 14), qMin(rc.width(), rc.height()) / 2.0);
                    ip.addRoundedRect(QRectF(rc), ir, ir);
                    p.save();
                    p.setClipPath(ip);
                    p.drawPixmap(rc, frame);
                    p.restore();
                }
            }
        } else if (!it.pix[a].isNull()) {
            p.save();
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            if (att.isSticker()) {
                // Stickers preserve their transparent edges, unlike regular image attachments.
                p.drawPixmap(rc, it.pix[a]);
            } else {
                QPainterPath ip;
                const qreal ir = qMin<qreal>(qMin(m_ap.radius, 14), qMin(rc.width(), rc.height()) / 2.0);
                ip.addRoundedRect(QRectF(rc), ir, ir);
                p.setClipPath(ip);
                p.drawPixmap(rc, it.pix[a]);
            }
            p.restore();
        } else {
            QPainterPath fp;
            fp.addRoundedRect(QRectF(rc), 10, 10);
            p.fillPath(fp, alpha(fg, 30));
            const QRect icon(rc.left() + 6, rc.top() + 6, 28, 28);
            p.setBrush(alpha(fg, 50));
            p.setPen(Qt::NoPen);
            p.drawEllipse(icon);
            p.setFont(base);
            p.setPen(fg);
            const bool audio = att.isAudio();
            const bool video = att.isVideo();
            const bool active = audio && m_audioMessage == i && m_audioAttachment == a;
            p.drawText(icon, Qt::AlignCenter, att.isImage() ? QStringLiteral("🖼")
                                                            : ((audio || video) ? QStringLiteral("▶")
                                                                     : QStringLiteral("📎")));
            const QString name = att.filename.isEmpty() ? att.contentType : att.filename;
            const QFontMetrics fm(base);
            if (video) {
                const bool loading = m_videoLoadingTs == m.ts() && m_videoLoadingAttachment == a;
                p.setFont(scaled(base, 1.8, true));
                p.setPen(fg);
                p.drawText(rc, Qt::AlignCenter, loading ? QStringLiteral("…") : QStringLiteral("▶"));
                p.setFont(smallBold);
                p.drawText(QRect(rc.left() + 10, rc.bottom() - 38, rc.width() - 20, 18),
                           Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(name, Qt::ElideMiddle, rc.width() - 20));
                p.setFont(small);
                p.setPen(alpha(fg, 190));
                const QString info = loading ? tr("Loading video …")
                                             : tr("Video") + (att.size > 0 ? QStringLiteral(" · ") + AttachmentUtil::humanSize(att.size) : QString());
                p.drawText(QRect(rc.left() + 10, rc.bottom() - 20, rc.width() - 20, 16),
                           Qt::AlignLeft | Qt::AlignVCenter, info);
                continue;
            }
            const int textLeft = (audio || video) ? 42 : 40;
            const int textWidth = rc.width() - textLeft - 6;
            if (!video)
                p.drawText(QRect(rc.left() + textLeft, rc.top() + 3, textWidth, 18), Qt::AlignLeft | Qt::AlignVCenter,
                           fm.elidedText(name, Qt::ElideMiddle, textWidth));
            p.setFont(small);
            p.setPen(alpha(fg, 190));
            QString info = att.size > 0 ? AttachmentUtil::humanSize(att.size) : att.contentType;
            if (att.isImage()) info += tr(" – loading …");
            if (video && m_videoLoadingTs == m.ts() && m_videoLoadingAttachment == a)
                info = tr("Loading video …");
            else if (video)
                info = tr("Video") + (att.size > 0 ? QStringLiteral(" · ") + AttachmentUtil::humanSize(att.size) : QString());
            if (audio) {
                const qint64 shownMs = active ? m_audioPositionMs : 0;
                const qint64 totalMs = active && m_audioDurationMs > 0 ? m_audioDurationMs : 0;
                const auto formatTime = [](qint64 ms) {
                    const qint64 seconds = ms / 1000;
                    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
                };
                const QString clock = formatTime(shownMs) + QStringLiteral(" / ") +
                                      (totalMs > 0 ? formatTime(totalMs) : QStringLiteral("--:--"));
                const QRect bar(rc.left() + textLeft, rc.bottom() - 13, textWidth, 3);
                p.setPen(Qt::NoPen);
                p.setBrush(alpha(fg, 65));
                p.drawRoundedRect(bar, 1, 1);
                if (active && totalMs > 0) {
                    QRect progress = bar;
                    progress.setWidth(qRound(bar.width() * qBound<qreal>(0.0, qreal(shownMs) / totalMs, 1.0)));
                    p.setBrush(m.outgoing ? m_ap.outText : m_ap.inText);
                    p.drawRoundedRect(progress, 1, 1);
                }
                p.setPen(alpha(fg, 190));
                p.drawText(QRect(rc.left() + textLeft, rc.top() + 22, textWidth, 16), Qt::AlignLeft | Qt::AlignVCenter,
                           m_audioLoading && active ? tr("Loading audio …") : clock);
            } else {
                p.drawText(QRect(rc.left() + textLeft, rc.top() + 20, textWidth, 16), Qt::AlignLeft | Qt::AlignVCenter, info);
            }
        }
    }

    if (it.doc) {
        p.save();
        p.translate(it.textRect.topLeft());
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, fg);
        if (m_selItem == i && m_selCursor.hasSelection()) {
            QAbstractTextDocumentLayout::Selection sel;
            sel.cursor = m_selCursor;
            sel.format.setBackground(alpha(fg, 90));
            ctx.selections.append(sel);
        }
        it.doc->documentLayout()->draw(&p, ctx);
        p.restore();
    }

    if (!it.linkPreviewRect.isEmpty()) {
        QPainterPath previewPath;
        previewPath.addRoundedRect(QRectF(it.linkPreviewRect), 8, 8);
        p.save();
        p.setClipPath(previewPath);
        p.fillRect(it.linkPreviewRect, alpha(fg, 12));
        if (!it.linkPreviewImageRect.isEmpty() && !it.linkPreviewPixmap.isNull()) {
            QPixmap scaled = it.linkPreviewPixmap.scaled(it.linkPreviewImageRect.size(), Qt::KeepAspectRatioByExpanding,
                                                         Qt::SmoothTransformation);
            QRect src(QPoint(0, 0), it.linkPreviewImageRect.size());
            src.moveCenter(QRect(QPoint(0, 0), scaled.size()).center());
            p.drawPixmap(it.linkPreviewImageRect, scaled, src);
        }
        p.restore();
        p.setPen(QPen(alpha(fg, 65), 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(previewPath);
        p.setFont(smallBold);
        p.setPen(fg);
        p.drawText(it.linkPreviewTitleRect, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, it.linkPreviewTitle);
        if (!it.linkPreviewDescriptionRect.isEmpty()) {
            p.setFont(small);
            p.setPen(alpha(fg, 220));
            p.drawText(it.linkPreviewDescriptionRect, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
                       it.linkPreviewDescription);
        }
        p.setFont(small);
        p.setPen(alpha(fg, 170));
        p.drawText(it.linkPreviewHostRect, Qt::AlignLeft | Qt::AlignVCenter, it.linkPreviewHost);
    }

    p.setFont(small);
    p.setPen(alpha(fg, 175));
    p.drawText(it.timeRect, Qt::AlignRight | Qt::AlignVCenter, it.timeText);
    if (!it.statusText.isEmpty()) {
        p.setPen(it.statusRead ? QColor(0x2e, 0xa0, 0x66) : alpha(fg, 175));
        p.drawText(it.statusRect, Qt::AlignRight | Qt::AlignVCenter, it.statusText);
    }

    // reaction pills overlapping the bubble's lower edge
    for (const Pill &pill : it.pills) {
        const bool dark = m_ap.background.lightness() < 128;
        const QColor pillBg = dark ? QColor(0x2a, 0x2a, 0x2d) : QColor(0xff, 0xff, 0xff);
        p.setPen(QPen(pill.own ? m_ap.outBg : alpha(mutedOn(m_ap.background), 90), pill.own ? 2 : 1));
        p.setBrush(pillBg);
        p.drawRoundedRect(QRectF(pill.rect), pill.rect.height() / 2.0, pill.rect.height() / 2.0);
        p.setFont(base);
        p.setPen(dark ? QColor(0xee, 0xee, 0xee) : QColor(0x20, 0x20, 0x20));
        p.drawText(pill.rect, Qt::AlignCenter, pill.text);
    }
}

// -------------------------------------------------------------- pinned messages

QRect ChatView::pinBannerRect() const
{
    return QRect(0, 0, viewport()->width(), kBannerHeight);
}

QRect ChatView::pinBannerMenuRect() const
{
    return QRect(pinBannerRect().right() - 40, 0, 40, kBannerHeight);
}

// The pin target is the identity of one revision of the message (original ts or an edit ts).
int ChatView::pinMessageIndex(const PinInfo &pin) const
{
    for (int i = 0; i < m_msgs.size(); ++i) {
        if (m_msgs[i].ts() != pin.ts && !(m_msgs[i].editTs > 0 && m_msgs[i].editTs == pin.ts)) continue;
        if (!pin.authorKey.isEmpty() && !m_msgs[i].authorKey.isEmpty() && m_msgs[i].authorKey != pin.authorKey)
            continue;
        return i;
    }
    return -1;
}

// Short banner description of a message: text if there is one, otherwise the media kind
// (wording like the official clients).
QString ChatView::pinSnippet(int idx) const
{
    const Message &m = m_msgs[idx];
    if (m.deleted) return deletedText(m);
    if (m.hasPoll) return tr("Poll: %1").arg(m.poll.question);
    if (m.hasContact) return tr("Contact: %1").arg(m.contactName);
    if (!m.text.isEmpty()) return m.text.simplified();
    if (!m.attachments.isEmpty()) {
        if (m.attachments.first().isSticker()) return tr("Sticker");
        if (m.attachments.first().isGif()) return tr("GIF");
        if (m.attachments.first().isImage()) return tr("Photo");
        if (m.attachments.first().isVideo()) return tr("Video");
        if (m.attachments.first().isAudio()) return tr("Audio");
        return tr("File");
    }
    if (m.quote.valid) return tr("Reply");
    return tr("Message");
}

void ChatView::drawPinBanner(QPainter &p)
{
    if (m_pins.isEmpty()) return;
    const QRect r = pinBannerRect();
    const QColor fg = m_ap.inText;
    const QColor muted = mutedOn(m_ap.inBg);
    p.fillRect(r, m_ap.inBg);
    p.setPen(QPen(mutedOn(m_ap.background), 1));
    p.drawLine(r.left(), r.bottom(), r.right(), r.bottom());

    const int shown = qBound(0, m_bannerCycle, m_pins.size() - 1);
    const PinInfo &pin = m_pins[shown];
    const int mi = pinMessageIndex(pin);

    const QFont base = font();
    const QFont small = scaled(base, 0.8);
    const QFont label = scaled(base, 0.9, true);
    const QFontMetrics fm(label);

    p.setFont(base);
    p.setPen(fg);
    p.drawText(QRect(r.left() + 4, 0, 32, kBannerHeight), Qt::AlignCenter, QStringLiteral("📌"));

    p.setFont(small);
    p.setPen(muted);
    p.drawText(QRect(r.left() + 40, 4, r.width() - 40 - 92, 16), Qt::AlignLeft | Qt::AlignVCenter,
               tr("Pinned message %1/%2").arg(shown + 1).arg(m_pins.size()));

    QString text = mi >= 0 ? pinSnippet(mi) : tr("Message not available");
    const QString author = mi >= 0 && !m_msgs[mi].authorKey.isEmpty() ? nameOf(m_msgs[mi].authorKey) : QString();
    if (!author.isEmpty()) text = author + QStringLiteral(": ") + text;
    const int textLeft = r.left() + 40;
    const int textRight = pinBannerMenuRect().left() - 8;
    p.setFont(label);
    p.setPen(fg);
    p.drawText(QRect(textLeft, 20, textRight - textLeft, 20), Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(text, Qt::ElideRight, textRight - textLeft));

    p.setFont(base);
    p.setPen(fg);
    p.drawText(pinBannerMenuRect(), Qt::AlignCenter, QStringLiteral("⋯"));
}

// ----------------------------------------------------------- interaction

ChatView::Hit ChatView::hitTest(const QPoint &vp) const
{
    Hit h;
    const int docY = vp.y() + verticalScrollBar()->value();
    const int i = itemAtY(docY);
    if (i < 0) return h;
    const Item &it = m_items[i];
    const QPoint p(vp.x(), docY - it.y);
    if (!(it.bubble.contains(p) || it.pillsRect.contains(p))) return h;
    h.kind = Hit::Bubble;
    h.msg = i;
    if (it.quoteBox.contains(p)) {
        h.kind = Hit::Quote;
        return h;
    }
    if (m_msgs[i].viewOnce) {
        if (!m_msgs[i].outgoing && !m_msgs[i].viewOnceViewed && !m_msgs[i].viewOnceExpired &&
            !it.attRects.isEmpty() && it.attRects.first().contains(p)) {
            h.kind = Hit::Attachment;
            h.index = 0;
            return h;
        }
    } else {
        for (int a = 0; a < it.attRects.size(); ++a)
            if (it.attRects[a].contains(p)) {
                h.kind = Hit::Attachment;
                h.index = a;
                return h;
            }
    }
    if (!it.linkPreviewRect.isEmpty() && it.linkPreviewRect.contains(p)) {
        h.kind = Hit::Link;
        h.href = m_msgs[i].linkPreview.url;
        return h;
    }
    for (int o = 0; o < it.pollOptionRects.size(); ++o)
        if (it.pollOptionRects[o].contains(p)) {
            h.kind = Hit::PollOption;
            h.index = o;
            return h;
        }
    if (!it.contactRect.isEmpty() && it.contactRect.contains(p)) {
        h.kind = Hit::ContactCard;
        return h;
    }
    if (it.doc && it.textRect.adjusted(0, 0, 0, 2).contains(p)) {
        const QPointF local = QPointF(p - it.textRect.topLeft());
        h.textPos = it.doc->documentLayout()->hitTest(local, Qt::FuzzyHit);
        h.href = it.doc->documentLayout()->anchorAt(local);
        h.kind = h.href.isEmpty() ? Hit::Text : Hit::Link;
    }
    return h;
}

void ChatView::updateCursorShape(const QPoint &vp)
{
    if (m_autoScrolling) {
        viewport()->setCursor(Qt::SizeVerCursor);
        return;
    }
    if (!m_pins.isEmpty() && vp.y() <= kBannerHeight) {
        viewport()->setCursor(Qt::PointingHandCursor);
        return;
    }
    const Hit h = hitTest(vp);
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (h.kind == Hit::Text) shape = Qt::IBeamCursor;
    else if (h.kind == Hit::Link || h.kind == Hit::Quote || h.kind == Hit::Attachment || h.kind == Hit::PollOption
             || h.kind == Hit::ContactCard)
        shape = Qt::PointingHandCursor;
    viewport()->setCursor(shape);
}

void ChatView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::MiddleButton) {
        if (m_autoScrolling) {
            stopAutoScroll();
        } else {
            m_autoScrollAnchor = e->pos();
            m_autoScrollPos = e->pos();
            m_autoScrolling = true;
            viewport()->setCursor(Qt::SizeVerCursor);
            m_autoScrollTimer->start();
        }
        e->accept();
        return;
    }
    if (m_autoScrolling) stopAutoScroll();
    if (e->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(e);
        return;
    }
    // Left click on the pinned-message banner: the "…" opens the options menu, any other
    // spot cycles through the pins like in the official clients.
    if (!m_pins.isEmpty() && e->pos().y() <= kBannerHeight) {
        e->accept();
        if (pinBannerMenuRect().contains(e->pos())) {
            emit pinBannerMenuRequested(e->globalPosition().toPoint());
        } else {
            m_bannerCycle = (m_bannerCycle + 1) % m_pins.size();
            const int i = pinMessageIndex(m_pins[m_bannerCycle]);
            if (i >= 0) scrollToMessage(i);
        }
        viewport()->update();
        return;
    }
    setFocus();
    m_pressPos = e->pos();
    m_pressHit = hitTest(e->pos());
    // Qt delivers a double click as press, release, DoubleClick, release. The
    // second press must not wipe the word selection made by the double click;
    // any other press starts fresh. (m_dblClick stands in for clickCount(),
    // which not every Qt 6 exposes.)
    if (m_wordSelected && m_dblClick && m_pressHit.msg == m_selItem) {
        m_dragging = false;
        viewport()->update();
        return;
    }
    clearSelection();
    if ((m_pressHit.kind == Hit::Text || m_pressHit.kind == Hit::Link) && m_pressHit.textPos >= 0) {
        m_selItem = m_pressHit.msg;
        m_selCursor = QTextCursor(m_items[m_selItem].doc.get());
        m_selCursor.setPosition(m_pressHit.textPos);
        m_dragging = true;
    }
    viewport()->update();
}

void ChatView::mouseMoveEvent(QMouseEvent *e)
{
    if (m_autoScrolling) {
        m_autoScrollPos = e->pos();
        e->accept();
        return;
    }
    if (m_dragging && (e->buttons() & Qt::LeftButton) && m_selItem >= 0 && m_selItem < m_items.size()) {
        const Item &it = m_items[m_selItem];
        const QPointF local(e->pos().x() - it.textRect.left(),
                            e->pos().y() + verticalScrollBar()->value() - it.y - it.textRect.top());
        const int pos = it.doc->documentLayout()->hitTest(local, Qt::FuzzyHit);
        if (pos >= 0) {
            // leaving a double-click word selection only when the pointer really
            // moved away from the click point (extend by drag instead of losing it)
            if (m_wordSelected && (e->pos() - m_pressPos).manhattanLength() < 4) return;
            m_wordSelected = false;
            m_selCursor.setPosition(pos, QTextCursor::KeepAnchor);
        }
        viewport()->update();
        return;
    }
    updateCursorShape(e->pos());
}

void ChatView::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::MiddleButton && m_autoScrolling) {
        e->accept();
        return;
    }
    if (e->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(e);
        return;
    }
    // The banner press handled the click itself; do not let the stale press hit
    // turn the release into an action on some message.
    if (!m_pins.isEmpty() && e->pos().y() <= kBannerHeight) {
        e->accept();
        return;
    }
    m_dragging = false;
    // The double-click flag is consumed by the release that ends the double click,
    // even when that release leaves the drag threshold (flag must not leak).
    const bool dbl = m_dblClick;
    m_dblClick = false;
    const bool click = (e->pos() - m_pressPos).manhattanLength() < 4;
    if (!click) return;
    // keep a word selection made by double clicking until the next real click
    if (m_wordSelected && dbl) {
        viewport()->update();
        return;
    }
    const Hit h = hitTest(e->pos());
    if (h.msg != m_pressHit.msg || h.kind != m_pressHit.kind) return;
    switch (h.kind) {
    case Hit::Link:
        clearSelection();
        emit linkClicked(h.href);
        break;
    case Hit::PollOption:
        // like the official clients: one click on an option votes (or changes the vote)
        emit pollOptionClicked(h.msg, h.index);
        break;
    case Hit::ContactCard:
        // like the official clients: clicking a contact card opens the chat with that contact
        emit contactCardActivated(h.msg);
        break;
    case Hit::Attachment:
        emit attachmentActivated(h.msg, h.index);
        break;
    case Hit::Quote: {
        const qint64 id = m_msgs[h.msg].quote.id;
        for (int i = 0; i < m_msgs.size(); ++i)
            if (m_msgs[i].ts() == id || (id > 0 && m_msgs[i].editTs == id)) {
                scrollToMessage(i);
                break;
            }
        break;
    }
    case Hit::Text:
        clearSelection(); // a plain click just removes the selection
        break;
    default:
        break;
    }
    viewport()->update();
}

void ChatView::stopAutoScroll()
{
    m_autoScrolling = false;
    m_autoScrollTimer->stop();
    updateCursorShape(m_autoScrollPos);
}

void ChatView::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseDoubleClickEvent(e);
        return;
    }
    const Hit h = hitTest(e->pos());
    // double click on a word in a message bubble selects that word, as known
    // from other chat apps (works for plain text and the text of links too)
    if ((h.kind == Hit::Text || h.kind == Hit::Link) && h.textPos >= 0) {
        selectWordAt(h.msg, h.textPos);
        m_dblClick = true; // keeps the word selection through the release that ends the double click
        e->accept();
        return;
    }
    QAbstractScrollArea::mouseDoubleClickEvent(e);
}

void ChatView::keyPressEvent(QKeyEvent *e)
{
    QScrollBar *sb = verticalScrollBar();
    if (e->matches(QKeySequence::Copy)) {
        const QString t = selectedText();
        if (!t.isEmpty()) QApplication::clipboard()->setText(t);
    } else if (e->key() == Qt::Key_PageUp) {
        sb->setValue(sb->value() - sb->pageStep());
    } else if (e->key() == Qt::Key_PageDown) {
        sb->setValue(sb->value() + sb->pageStep());
    } else if (e->key() == Qt::Key_Home) {
        sb->setValue(0);
    } else if (e->key() == Qt::Key_End) {
        sb->setValue(sb->maximum());
    } else {
        QAbstractScrollArea::keyPressEvent(e);
    }
}

int ChatView::messageAt(const QPoint &viewportPos) const
{
    return hitTest(viewportPos).msg;
}

void ChatView::contextMenuEvent(QContextMenuEvent *e)
{
    // Right-click on the pinned-message banner opens the same options menu as the "…" button.
    if (!m_pins.isEmpty() && e->pos().y() <= kBannerHeight) {
        emit pinBannerMenuRequested(e->globalPos());
        e->accept();
        return;
    }
    const int idx = hitTest(e->pos()).msg;
    QMenu menu(this);
    const QString sel = selectedText();
    if (!sel.isEmpty())
        menu.addAction(tr("Copy"), this, [sel] { QApplication::clipboard()->setText(sel); });
    if (idx < 0 || idx >= m_msgs.size()) {
        if (!menu.isEmpty()) menu.exec(e->globalPos());
        return;
    }
    const Message m = m_msgs[idx];
    if (!menu.isEmpty()) menu.addSeparator();
    if (!m.deleted)
        menu.addAction(tr("Message details…"), this, [this, idx] { emit messageDetailsRequested(idx); });
    if (m.deleted) { // placeholder: only removing it from this device is left
        menu.addAction(tr("Delete message …"), this, [this, idx] { emit deleteRequested(idx); });
        menu.exec(e->globalPos());
        return;
    }
    if (m.viewOnce) {
        menu.addAction(tr("Delete message …"), this, [this, idx] { emit deleteRequested(idx); });
        menu.exec(e->globalPos());
        return;
    }
    if (m.hasPoll) {
        // Polls: the details show who voted for what; the author can end the poll early.
        menu.addAction(tr("View votes…"), this, [this, idx] { emit pollVotesRequested(idx); });
        if (m.outgoing && !m.poll.terminated)
            menu.addAction(tr("End poll"), this, [this, idx] { emit pollTerminateRequested(idx); });
        menu.addAction(tr("Reply"), this, [this, idx] { emit replyRequested(idx); });
        menu.addAction(tr("Forward…"), this, [this, idx] { emit forwardRequested(idx); });
        menu.addAction(tr("Delete message …"), this, [this, idx] { emit deleteRequested(idx); });
        menu.exec(e->globalPos());
        return;
    }
    menu.addAction(tr("Reply"), this, [this, idx] { emit replyRequested(idx); });
    menu.addAction(tr("Forward…"), this, [this, idx] { emit forwardRequested(idx); });
    QMenu *react = menu.addMenu(tr("React"));
    const QString own = m.reactions.value(m_self);
    for (const QString &emoji : EmojiPicker::quickReactions()) {
        QAction *a = react->addAction(own == emoji ? emoji + QStringLiteral(" ✓") : emoji);
        connect(a, &QAction::triggered, this, [this, idx, emoji] { emit reactionChosen(idx, emoji); });
    }
    const QPoint gp = e->globalPos();
    react->addAction(tr("More …"), this, [this, idx, gp] { emit moreReactionsRequested(idx, gp); });
    // Pin / unpin like in the official clients: a pin replaces the oldest of the three pins.
    bool alreadyPinned = false;
    for (const PinInfo &pin : m_pins) {
        if (pin.ts == m.ts() || pin.ts == m.latestTs()) {
            alreadyPinned = true;
            break;
        }
    }
    if (alreadyPinned) {
        menu.addAction(tr("Unpin"), this, [this, idx] { emit unpinRequested(idx); });
    } else {
        QMenu *pinMenu = menu.addMenu(tr("Pin…"));
        pinMenu->addAction(tr("24 hours"), this, [this, idx] { emit pinRequested(idx, 24 * 60 * 60); });
        pinMenu->addAction(tr("7 days"), this, [this, idx] { emit pinRequested(idx, 7 * 24 * 60 * 60); });
        pinMenu->addAction(tr("30 days"), this, [this, idx] { emit pinRequested(idx, 30 * 24 * 60 * 60); });
        pinMenu->addAction(tr("Forever"), this, [this, idx] { emit pinRequested(idx, qint64(-1)); });
    }
    if (m.canEdit(QDateTime::currentMSecsSinceEpoch()))
        menu.addAction(tr("Edit"), this, [this, idx] { emit editRequested(idx); });
    menu.addAction(tr("Delete message …"), this, [this, idx] { emit deleteRequested(idx); });
    if (!m.text.isEmpty())
        menu.addAction(tr("Copy message text"), this, [m] { QApplication::clipboard()->setText(m.text); });
    for (int a = 0; a < m.attachments.size(); ++a) {
        const Attachment &att = m.attachments[a];
        const QString name = AttachmentUtil::displayFileName(att, m.time);
        menu.addAction(tr("Save “%1” …").arg(name), this, [this, idx, a] { emit attachmentSaveRequested(idx, a); });
    }
    menu.exec(gp);
}
