#include "ChatView.h"

#include "ApngMovie.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
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
#include <QTimer>
#include <QToolButton>
#include <QVideoFrame>
#include <QVideoSink>
#include <algorithm>

#include "Attachments.h"
#include "EmojiPicker.h"

namespace {
constexpr int kMargin = 12; // distance of bubbles to the view edge
constexpr int kPadX = 12;
constexpr int kPadY = 8;
constexpr int kGap = 4;
constexpr int kMaxImage = 300;

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

QString styledMessageHtml(const QString &text, const QList<TextStyleRange> &styles, const QColor &fg)
{
    if (text.isEmpty()) return QString();
    const int n = text.size();
    QVector<QSet<QString>> active(n);
    for (const TextStyleRange &r : styles) {
        const int a = qMax(0, r.start);
        const int b = qMin(n, r.start + r.length);
        for (int i = a; i < b; ++i) active[i].insert(r.style);
    }

    QString html;
    int runStart = 0;
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

    for (int i = 1; i <= n; ++i) {
        if (i == n || active[i] != active[runStart]) {
            html += runHtml(runStart, i);
            runStart = i;
        }
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
    });
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this] { updateToLatestButton(); });
    m_toLatest = new ScrollToLatestButton(viewport());
    m_toLatest->onClicked = [this] { verticalScrollBar()->setValue(verticalScrollBar()->maximum()); };
    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(70);
    connect(m_resizeTimer, &QTimer::timeout, this, [this] {
        relayoutAll();
        verticalScrollBar()->setValue(m_resizeAtBottom ? verticalScrollBar()->maximum() : m_resizeScrollValue);
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

void ChatView::setAppearance(const Appearance &a)
{
    m_ap = a;
    for (Item &it : m_items) { // colours of links are baked into the documents
        it.built = false;
        it.doc.reset();
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
}

void ChatView::clearView()
{
    stopVideoPlayback();
    for (Item &item : m_items) {
        for (QMovie *movie : item.movies)
            delete movie;
        for (ApngMovie *movie : item.apngMovies)
            delete movie;
    }
    m_msgs.clear();
    m_items.clear();
    m_renderFirst = m_renderLast = -1;
    clearSelection();
    m_flash = -1;
    updateScrollRange();
    verticalScrollBar()->setValue(0);
    m_missed = 0;
    updateToLatestButton();
    viewport()->update();
}

void ChatView::showMessages(const QList<Message> &msgs, bool keepScroll)
{
    const bool bottom = atBottom();
    const int old = verticalScrollBar()->value();
    for (Item &item : m_items) {
        for (QMovie *movie : item.movies)
            delete movie;
        for (ApngMovie *movie : item.apngMovies)
            delete movie;
    }
    m_msgs = msgs;
    m_items = QList<Item>(msgs.size());
    m_renderFirst = m_renderLast = -1;
    clearSelection();
    relayoutAll();
    verticalScrollBar()->setValue((bottom || !keepScroll) ? verticalScrollBar()->maximum() : old);
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
    layoutItem(i, i > 0 ? m_items[i - 1].y + m_items[i - 1].height : 0);
    if (!bottom && !m.outgoing) ++m_missed; // arrived below the visible part
    updateScrollRange();
    if (bottom || m.outgoing) verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    updateToLatestButton();
    updateVideoGeometry();
    viewport()->update();
}

void ChatView::scrollToMessage(int idx)
{
    if (idx < 0 || idx >= m_items.size()) return;
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
    m_videoSink = new QVideoSink(this);
    m_videoFrame = QImage();
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
    m_videoPlayer->setAudioOutput(m_videoAudioOutput);
    m_videoPlayer->setVideoOutput(m_videoSink);
    // Frames are painted by the viewport itself (see paintEvent) instead of a
    // QVideoWidget child: a native video surface is not clipped to the
    // viewport and would poke out over the composer when scrolling.
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        if (!m_videoSink) return;
        m_videoFrame = frame.isValid() ? frame.toImage() : QImage();
        const QRect rect = attachmentRectForVideo();
        if (rect.isValid()) viewport()->update(rect);
    });
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
        m_videoPlayer->setVideoOutput(static_cast<QVideoSink *>(nullptr));
        m_videoPlayer->setAudioOutput(static_cast<QAudioOutput *>(nullptr));
    }
    delete m_videoToggle;
    delete m_videoProgress;
    delete m_videoElapsed;
    delete m_videoRemaining;
    delete m_videoPlayer;
    delete m_videoAudioOutput;
    delete m_videoSink;
    m_videoToggle = nullptr;
    m_videoProgress = nullptr;
    m_videoElapsed = nullptr;
    m_videoRemaining = nullptr;
    m_videoSink = nullptr;
    m_videoFrame = QImage();
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
    if (!m_videoSink || !m_videoToggle || !m_videoProgress || !m_videoElapsed || !m_videoRemaining) return;
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

    // The video itself is painted in paintEvent and clipped by the viewport.
    // The small overlay controls are shown only while fully inside it.
    const QRect viewRect = viewport()->rect();
    const auto place = [&viewRect](QWidget *w, const QRect &geometry) {
        w->setGeometry(geometry);
        const bool inside = viewRect.contains(geometry);
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
        lines << (who + QStringLiteral(": ") + m.text + (m.isEdited() ? tr(" (edited)") : QString()));
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
    if (m.deleted) {
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
    } else if (!m.text.isEmpty()) {
        const QColor fg = m.outgoing ? m_ap.outText : m_ap.inText;
        it.doc = std::make_shared<QTextDocument>();
        it.doc->setDocumentMargin(0);
        it.doc->setDefaultFont(font());
        it.doc->setDefaultStyleSheet(QStringLiteral("a { color: %1; text-decoration: underline; }").arg(fg.name()));
        it.doc->setHtml(styledMessageHtml(m.text, m.textStyles, fg));
        if (it.textRect.width() > 0) it.doc->setTextWidth(it.textRect.width());
    }
    it.pix.clear();
    if (includePixmaps) {
        for (const Attachment &a : m.attachments)
            it.pix << ((a.isImage() && !a.thumb.isEmpty()) ? QPixmap::fromImage(QImage::fromData(a.thumb)) : QPixmap());
    }
    it.renderResourcesBuilt = true;
}

void ChatView::releaseItemRenderResources(int i)
{
    Item &it = m_items[i];
    it.doc.reset();
    it.pix.clear();
    it.renderResourcesBuilt = false;
}

void ChatView::layoutItem(int i, int y)
{
    buildItem(i);
    ensureItemRenderResources(i, false);
    Item &it = m_items[i];
    const Message &m = m_msgs[i];
    const int W = viewport()->width();

    const QFont base = font();
    const QFont small = scaled(base, 0.85);
    const QFont smallBold = scaled(base, 0.9, true);
    const QFontMetrics fm(base), fms(small), fmb(smallBold);

    it.y = y;
    it.dateRect = it.bubble = it.senderRect = it.quoteBox = it.quoteAuthorRect = it.quoteTextRect = QRect();
    it.textRect = it.timeRect = it.pillsRect = QRect();
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
    int contentW = fms.horizontalAdvance(it.timeText);
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
    QSize textSz;
    if (it.doc) {
        it.doc->setTextWidth(innerMax);
        const int ideal = int(std::ceil(it.doc->idealWidth()));
        it.doc->setTextWidth(qMin(innerMax, ideal + 1));
        textSz = QSize(qMin(innerMax, ideal + 1), int(std::ceil(it.doc->size().height())));
        contentW = qMax(contentW, textSz.width());
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
    for (const QSize &sz : attSizes) {
        it.attRects << QRect(x0, yb, sz.width(), sz.height());
        yb += sz.height() + kGap;
    }
    if (it.doc) {
        it.textRect = QRect(x0, yb, textSz.width(), textSz.height());
        yb += textSz.height() + 2;
    }
    const int timeW = fms.horizontalAdvance(it.timeText);
    it.timeRect = QRect(bubbleX + bubbleW - kPadX - timeW, yb, timeW, fms.height());
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

void ChatView::relayoutAll()
{
    m_layoutWidth = viewport()->width();
    int y = 0;
    for (int i = 0; i < m_items.size(); ++i) {
        layoutItem(i, y);
        y += m_items[i].height;
    }
    updateScrollRange();
    viewport()->update();
    updateVideoGeometry();
}

void ChatView::updateScrollRange()
{
    verticalScrollBar()->setRange(0, qMax(0, totalHeight() - viewport()->height() + 6));
    verticalScrollBar()->setPageStep(viewport()->height());
}

void ChatView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    if (viewport()->width() != m_layoutWidth) {
        if (!m_resizeTimer->isActive()) {
            m_resizeAtBottom = atBottom();
            m_resizeScrollValue = verticalScrollBar()->value();
        }
        updateScrollRange();
        verticalScrollBar()->setValue(m_resizeAtBottom ? verticalScrollBar()->maximum() : m_resizeScrollValue);
        m_resizeTimer->start();
    } else {
        const bool bottom = atBottom();
        const int old = verticalScrollBar()->value();
        updateScrollRange();
        verticalScrollBar()->setValue(bottom ? verticalScrollBar()->maximum() : old);
    }
    updateToLatestButton();
    updateVideoGeometry();
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
    if (m_items.isEmpty()) return;

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

    for (int i = first; i <= last; ++i) ensureItemRenderResources(i);
    for (int i = first; i <= last; ++i) {
        p.save();
        p.translate(0, m_items[i].y - top);
        paintItem(p, i);
        p.restore();
    }
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

    for (int a = 0; a < m.attachments.size() && a < it.attRects.size(); ++a) {
        const QRect rc = it.attRects[a];
        const Attachment &att = m.attachments[a];
        if (att.isVideo() && m_videoSink && m_videoMessageTs == m.ts() && m_videoAttachment == a) {
            QPainterPath vp;
            const qreal vr = qMin<qreal>(qMin(m_ap.radius, 14), qMin(rc.width(), rc.height()) / 2.0);
            vp.addRoundedRect(QRectF(rc), vr, vr);
            p.save();
            p.setClipPath(vp);
            p.fillRect(rc, Qt::black);
            if (!m_videoFrame.isNull()) {
                QSize target = m_videoFrame.size();
                target.scale(rc.size(), Qt::KeepAspectRatio);
                QRect dst(QPoint(0, 0), target);
                dst.moveCenter(rc.center());
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                p.drawImage(dst, m_videoFrame);
            }
            p.restore();
            continue;
        }
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

    p.setFont(small);
    p.setPen(alpha(fg, 175));
    p.drawText(it.timeRect, Qt::AlignRight | Qt::AlignVCenter, it.timeText);

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
    for (int a = 0; a < it.attRects.size(); ++a)
        if (it.attRects[a].contains(p)) {
            h.kind = Hit::Attachment;
            h.index = a;
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
    const Hit h = hitTest(vp);
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (h.kind == Hit::Text) shape = Qt::IBeamCursor;
    else if (h.kind == Hit::Link || h.kind == Hit::Quote || h.kind == Hit::Attachment) shape = Qt::PointingHandCursor;
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
    setFocus();
    m_pressPos = e->pos();
    m_pressHit = hitTest(e->pos());
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
        if (pos >= 0) m_selCursor.setPosition(pos, QTextCursor::KeepAnchor);
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
    m_dragging = false;
    const bool click = (e->pos() - m_pressPos).manhattanLength() < 4;
    if (!click) return;
    const Hit h = hitTest(e->pos());
    if (h.msg != m_pressHit.msg || h.kind != m_pressHit.kind) return;
    switch (h.kind) {
    case Hit::Link:
        clearSelection();
        emit linkClicked(h.href);
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
    const Hit h = hitTest(e->pos());
    if ((h.kind == Hit::Text || h.kind == Hit::Link) && h.textPos >= 0) {
        m_selItem = h.msg;
        m_selCursor = QTextCursor(m_items[m_selItem].doc.get());
        m_selCursor.setPosition(h.textPos);
        m_selCursor.select(QTextCursor::WordUnderCursor);
        m_dragging = false;
        viewport()->update();
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
    if (m.deleted) { // placeholder: only removing it from this device is left
        menu.addAction(tr("Delete message …"), this, [this, idx] { emit deleteRequested(idx); });
        menu.exec(e->globalPos());
        return;
    }
    menu.addAction(tr("Reply"), this, [this, idx] { emit replyRequested(idx); });
    QMenu *react = menu.addMenu(tr("React"));
    const QString own = m.reactions.value(m_self);
    for (const QString &emoji : EmojiPicker::quickReactions()) {
        QAction *a = react->addAction(own == emoji ? emoji + QStringLiteral(" ✓") : emoji);
        connect(a, &QAction::triggered, this, [this, idx, emoji] { emit reactionChosen(idx, emoji); });
    }
    const QPoint gp = e->globalPos();
    react->addAction(tr("More …"), this, [this, idx, gp] { emit moreReactionsRequested(idx, gp); });
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
