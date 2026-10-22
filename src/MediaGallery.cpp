#include "MediaGallery.h"

#include "Attachments.h"
#include "ImageViewer.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QAudioOutput>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QLocale>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <algorithm>
#include <climits>

namespace MediaGallery {

QStringList extractLinks(const QString &text)
{
    static const QRegularExpression re(QStringLiteral(R"((https?://[^\s<>"]*[^\s<>".,;:!?)\]]))"));
    QStringList out;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QString url = it.next().captured(1);
        if (!out.contains(url)) out << url;
    }
    return out;
}

QString monthTitle(const QDate &date)
{
    QString s = QLocale().toString(date, QStringLiteral("MMMM yyyy"));
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    return s;
}

Collection collect(const QList<Message> &messages)
{
    Collection c;
    // Newest message first; inside one message the original order is kept.
    for (int mi = int(messages.size()) - 1; mi >= 0; --mi) {
        const Message &m = messages[mi];
        if (m.deleted) continue;
        for (int i = 0; i < m.attachments.size(); ++i) {
            const Attachment &a = m.attachments[i];
            MediaItem item;
            item.ts = m.ts();
            item.attIndex = i;
            item.time = m.time;
            item.outgoing = m.outgoing;
            item.att = a;
            // Same precedence as the chat: audio wins over video for ambiguous
            // containers such as .webm without a content type.
            if (a.isImage()) c.media.append(item);
            else if (a.isAudio()) c.audio.append(item);
            else if (a.isVideo()) c.media.append(item);
            else c.files.append(item);
        }
        for (const QString &url : extractLinks(m.text)) {
            LinkItem l;
            l.ts = m.ts();
            l.time = m.time;
            l.outgoing = m.outgoing;
            l.url = url;
            c.links.append(l);
        }
    }
    // Messages are normally sorted already; the stable sort only guards against
    // a list that is not, and keeps the order inside one message.
    const auto newer = [](const auto &a, const auto &b) { return a.time > b.time; };
    std::stable_sort(c.media.begin(), c.media.end(), newer);
    std::stable_sort(c.audio.begin(), c.audio.end(), newer);
    std::stable_sort(c.files.begin(), c.files.end(), newer);
    std::stable_sort(c.links.begin(), c.links.end(), newer);
    return c;
}

} // namespace MediaGallery

using namespace MediaGallery;

namespace {

constexpr int kMargin = 12;
constexpr int kGap = 4;
constexpr int kTargetTile = 128;
constexpr int kHeaderHeight = 38;
constexpr int kRowHeight = 60;
constexpr int kSectionGap = 6;
constexpr int kThumbSide = 256;
constexpr int kThumbConcurrency = 3;
constexpr qint64 kMaxThumbSource = 30LL * 1024 * 1024;

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

QString formatTime(qint64 ms)
{
    const qint64 s = qMax<qint64>(0, ms / 1000);
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString shortDate(const QDateTime &t)
{
    return QLocale().toString(t.date(), QLocale::ShortFormat);
}

// Small player window for videos opened from the gallery.
class VideoViewer : public QDialog
{
public:
    VideoViewer(const QString &path, const QString &title, QWidget *parent) : QDialog(parent)
    {
        setWindowTitle(title);
        setAttribute(Qt::WA_DeleteOnClose);
        resize(820, 560);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 8);
        lay->setSpacing(6);
        auto *video = new QVideoWidget;
        video->setMinimumSize(320, 200);
        lay->addWidget(video, 1);

        auto *controls = new QHBoxLayout;
        controls->setContentsMargins(10, 0, 10, 0);
        auto *toggle = new QPushButton(QStringLiteral("⏸"));
        toggle->setFixedWidth(40);
        auto *slider = new QSlider(Qt::Horizontal);
        auto *time = new QLabel(QStringLiteral("0:00 / 0:00"));
        controls->addWidget(toggle);
        controls->addWidget(slider, 1);
        controls->addWidget(time);
        lay->addLayout(controls);

        m_player = new QMediaPlayer(this);
        auto *out = new QAudioOutput(this);
        m_player->setAudioOutput(out);
        m_player->setVideoOutput(video);

        connect(m_player, &QMediaPlayer::durationChanged, this, [slider](qint64 d) {
            slider->setRange(0, int(qMin<qint64>(d, INT_MAX)));
        });
        connect(m_player, &QMediaPlayer::positionChanged, this, [this, slider, time](qint64 p) {
            if (!slider->isSliderDown()) slider->setValue(int(qMin<qint64>(p, INT_MAX)));
            time->setText(formatTime(p) + QStringLiteral(" / ") + formatTime(m_player->duration()));
        });
        connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this, toggle] {
            toggle->setText(m_player->playbackState() == QMediaPlayer::PlayingState ? QStringLiteral("⏸")
                                                                                     : QStringLiteral("▶"));
        });
        connect(slider, &QSlider::sliderMoved, m_player, &QMediaPlayer::setPosition);
        connect(toggle, &QPushButton::clicked, this, [this] { togglePlayback(); });
        connect(m_player, &QMediaPlayer::errorOccurred, this, [this, time](QMediaPlayer::Error, const QString &e) {
            time->setText(e);
        });

        m_player->setSource(QUrl::fromLocalFile(path));
        m_player->play();
    }

    ~VideoViewer() override
    {
        if (m_player) m_player->stop();
    }

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Space) {
            togglePlayback();
            return;
        }
        QDialog::keyPressEvent(e);
    }

private:
    void togglePlayback()
    {
        if (m_player->playbackState() == QMediaPlayer::PlayingState) {
            m_player->pause();
        } else {
            if (m_player->duration() > 0 && m_player->position() >= m_player->duration()) m_player->setPosition(0);
            m_player->play();
        }
    }

    QMediaPlayer *m_player = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// View: one tab. Paints either a grid of square tiles or a list of rows, both
// grouped under month headings.
// ---------------------------------------------------------------------------
class MediaGalleryDialog::View : public QAbstractScrollArea
{
public:
    enum Mode { Grid, Rows };

    struct Cell {
        int id = -1; // index into the dialog's collection
        QString title;
        QString subtitle;
        QString trailing;
        QString glyph;
        QString tooltip;
        bool video = false;
        bool active = false;
    };
    struct Section {
        QString title;
        QVector<Cell> cells;
    };

    std::function<void(int)> onActivated;
    std::function<void(int, const QPoint &)> onContext;
    std::function<QPixmap(int)> thumbnail; // Grid only; may return a null pixmap
    QString emptyText;

    View(Mode mode, QWidget *parent) : QAbstractScrollArea(parent), m_mode(mode)
    {
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        viewport()->setMouseTracking(true);
        viewport()->setAutoFillBackground(false);
        verticalScrollBar()->setSingleStep(mode == Grid ? 40 : kRowHeight);
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
            updateHover(viewport()->mapFromGlobal(QCursor::pos()));
        });
    }

    void setSections(const QVector<Section> &sections)
    {
        m_sections = sections;
        m_total = 0;
        for (const Section &s : m_sections) m_total += s.cells.size();
        relayout();
        viewport()->update();
    }

    int total() const { return m_total; }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QAbstractScrollArea::resizeEvent(e);
        relayout();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRect view = viewport()->rect();
        const int scroll = verticalScrollBar()->value();

        if (m_total == 0) {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(view, Qt::AlignCenter, emptyText);
            return;
        }

        QFont headerFont = font();
        headerFont.setBold(true);
        headerFont.setPointSizeF(headerFont.pointSizeF() + 0.5);

        for (int s = 0; s < m_sections.size(); ++s) {
            const Section &sec = m_sections[s];
            const int top = m_tops[s] - scroll;
            if (top + kHeaderHeight + bodyHeight(sec) < 0 || top > view.height()) continue;

            p.setFont(headerFont);
            p.setPen(palette().color(QPalette::Text));
            p.drawText(QRect(kMargin, top, view.width() - 2 * kMargin, kHeaderHeight),
                       Qt::AlignLeft | Qt::AlignVCenter, sec.title);
            p.setFont(font());

            for (int i = 0; i < sec.cells.size(); ++i) {
                const QRect r = cellRect(s, i).translated(0, -scroll);
                if (r.bottom() < 0 || r.top() > view.height()) continue;
                const bool hovered = m_hover.s == s && m_hover.i == i;
                if (m_mode == Grid) paintTile(p, sec.cells[i], r, hovered);
                else paintRow(p, sec.cells[i], r, hovered);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override { updateHover(e->position().toPoint()); }

    void leaveEvent(QEvent *) override
    {
        m_hover = {};
        viewport()->update();
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) m_pressed = posAt(e->position().toPoint());
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const Pos p = posAt(e->position().toPoint());
        const bool click = p.valid() && p == m_pressed;
        m_pressed = {};
        if (click && onActivated) {
            const int id = m_sections[p.s].cells[p.i].id; // copy: onActivated may rebuild
            onActivated(id);
        }
    }

    void contextMenuEvent(QContextMenuEvent *e) override
    {
        const Pos p = posAt(e->pos());
        if (!p.valid() || !onContext) return;
        const int id = m_sections[p.s].cells[p.i].id;
        onContext(id, e->globalPos());
    }

    bool viewportEvent(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const Pos p = posAt(he->pos());
            if (p.valid() && !m_sections[p.s].cells[p.i].tooltip.isEmpty()) {
                QToolTip::showText(he->globalPos(), m_sections[p.s].cells[p.i].tooltip, viewport());
            } else {
                QToolTip::hideText();
                e->ignore();
            }
            return true;
        }
        return QAbstractScrollArea::viewportEvent(e);
    }

private:
    struct Pos {
        int s = -1;
        int i = -1;
        bool valid() const { return s >= 0 && i >= 0; }
        bool operator==(const Pos &o) const { return s == o.s && i == o.i; }
    };

    int bodyHeight(const Section &s) const
    {
        if (m_mode == Rows) return s.cells.size() * kRowHeight;
        const int rows = (s.cells.size() + m_cols - 1) / m_cols;
        return rows * (m_tile + kGap);
    }

    void relayout()
    {
        const int w = viewport()->width();
        if (m_mode == Grid) {
            m_cols = qMax(3, (w - 2 * kMargin + kGap) / (kTargetTile + kGap));
            m_tile = qMax(48, (w - 2 * kMargin - kGap * (m_cols - 1)) / m_cols);
        }
        m_tops.clear();
        int y = 0;
        for (const Section &s : m_sections) {
            m_tops.append(y);
            y += kHeaderHeight + bodyHeight(s) + kSectionGap;
        }
        verticalScrollBar()->setRange(0, qMax(0, y - viewport()->height()));
        verticalScrollBar()->setPageStep(viewport()->height());
    }

    // In content coordinates (not scrolled).
    QRect cellRect(int s, int i) const
    {
        const int top = m_tops[s] + kHeaderHeight;
        if (m_mode == Rows)
            return QRect(kMargin - 6, top + i * kRowHeight, viewport()->width() - 2 * kMargin + 12, kRowHeight);
        const int row = i / m_cols;
        const int col = i % m_cols;
        return QRect(kMargin + col * (m_tile + kGap), top + row * (m_tile + kGap), m_tile, m_tile);
    }

    Pos posAt(const QPoint &pt) const
    {
        const int y = pt.y() + verticalScrollBar()->value();
        for (int s = 0; s < m_sections.size(); ++s) {
            const int top = m_tops[s] + kHeaderHeight;
            if (y < top || y >= top + bodyHeight(m_sections[s])) continue;
            int i = -1;
            if (m_mode == Rows) {
                i = (y - top) / kRowHeight;
            } else {
                if (pt.x() < kMargin) return {};
                const int col = (pt.x() - kMargin) / (m_tile + kGap);
                const int row = (y - top) / (m_tile + kGap);
                if (col >= m_cols) return {};
                i = row * m_cols + col;
            }
            if (i < 0 || i >= m_sections[s].cells.size()) return {};
            if (!cellRect(s, i).translated(0, -verticalScrollBar()->value()).contains(pt)) return {};
            return {s, i};
        }
        return {};
    }

    void updateHover(const QPoint &pt)
    {
        const Pos p = posAt(pt);
        if (p == m_hover) return;
        m_hover = p;
        viewport()->setCursor(p.valid() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        viewport()->update();
    }

    void paintTile(QPainter &p, const Cell &c, const QRect &r, bool hovered)
    {
        QPainterPath clip;
        clip.addRoundedRect(QRectF(r), 8, 8);
        p.save();
        p.setClipPath(clip);
        const QPixmap pm = thumbnail ? thumbnail(c.id) : QPixmap();
        if (!pm.isNull()) {
            p.drawPixmap(r, pm);
        } else {
            p.fillRect(r, withAlpha(palette().color(QPalette::Text), 26));
            QFont f = font();
            f.setPointSizeF(22);
            p.setFont(f);
            p.setPen(withAlpha(palette().color(QPalette::Text), 130));
            p.drawText(r, Qt::AlignCenter, c.video ? QStringLiteral("🎬") : QStringLiteral("🖼"));
            p.setFont(font());
        }
        if (c.video) {
            const QPoint center = r.center();
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            p.drawEllipse(center, 18, 18);
            QPainterPath tri;
            tri.moveTo(center + QPoint(-5, -9));
            tri.lineTo(center + QPoint(10, 0));
            tri.lineTo(center + QPoint(-5, 9));
            tri.closeSubpath();
            p.setBrush(Qt::white);
            p.drawPath(tri);
            if (!c.subtitle.isEmpty()) {
                QFont f = font();
                f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.5));
                p.setFont(f);
                const QRect tag(r.left() + 6, r.bottom() - 22, r.width() - 12, 18);
                p.setPen(Qt::white);
                p.drawText(tag, Qt::AlignRight | Qt::AlignVCenter, c.subtitle);
                p.setFont(font());
            }
        }
        if (hovered) p.fillRect(r, QColor(255, 255, 255, 36));
        p.restore();
        if (hovered) {
            p.setPen(QPen(palette().color(QPalette::Highlight), 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(r).adjusted(1, 1, -1, -1), 7, 7);
        }
    }

    void paintRow(QPainter &p, const Cell &c, const QRect &r, bool hovered)
    {
        if (hovered || c.active) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(palette().color(c.active ? QPalette::Highlight : QPalette::Text),
                                 c.active ? 40 : 24));
            p.drawRoundedRect(r, 8, 8);
        }
        const QRect icon(r.left() + 8, r.top() + (r.height() - 40) / 2, 40, 40);
        p.setPen(Qt::NoPen);
        p.setBrush(c.active ? palette().color(QPalette::Highlight)
                            : withAlpha(palette().color(QPalette::Text), 28));
        p.drawRoundedRect(icon, 10, 10);
        QFont gf = font();
        gf.setPointSizeF(15);
        p.setFont(gf);
        p.setPen(c.active ? palette().color(QPalette::HighlightedText) : palette().color(QPalette::Text));
        p.drawText(icon, Qt::AlignCenter, c.glyph);
        p.setFont(font());

        QFontMetrics fm(font());
        const int trailingW = c.trailing.isEmpty() ? 0 : fm.horizontalAdvance(c.trailing) + 16;
        const int textLeft = icon.right() + 12;
        const int textW = qMax(40, r.right() - 10 - trailingW - textLeft);

        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        p.setPen(palette().color(QPalette::Text));
        p.drawText(QRect(textLeft, r.top() + 8, textW, 22), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(bold).elidedText(c.title, Qt::ElideMiddle, textW));
        p.setFont(font());
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(QRect(textLeft, r.top() + 30, textW, 20), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(c.subtitle, Qt::ElideMiddle, textW));
        if (!c.trailing.isEmpty())
            p.drawText(QRect(r.right() - 10 - trailingW + 8, r.top(), trailingW, r.height()),
                       Qt::AlignRight | Qt::AlignVCenter, c.trailing);
    }

    Mode m_mode;
    QVector<Section> m_sections;
    QVector<int> m_tops;
    int m_total = 0;
    int m_cols = 3;
    int m_tile = kTargetTile;
    Pos m_hover;
    Pos m_pressed;
};

// ---------------------------------------------------------------------------
// MediaGalleryDialog
// ---------------------------------------------------------------------------
MediaGalleryDialog::MediaGalleryDialog(const QString &conversationTitle, MediaGalleryHost host, QWidget *parent)
    : QDialog(parent), m_host(std::move(host)), m_title(conversationTitle)
{
    setWindowTitle(tr("Media gallery – %1").arg(conversationTitle));
    setAttribute(Qt::WA_DeleteOnClose);
    setSizeGripEnabled(true);
    resize(780, 620);
    m_thumbs.setMaxCost(48 * 1024); // KiB

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    lay->addWidget(m_tabs, 1);

    m_media = new View(View::Grid, this);
    m_audio = new View(View::Rows, this);
    m_links = new View(View::Rows, this);
    m_files = new View(View::Rows, this);
    m_media->emptyText = tr("No media");
    m_audio->emptyText = tr("No audio files");
    m_links->emptyText = tr("No links");
    m_files->emptyText = tr("No files");
    m_tabs->addTab(m_media, tr("Media"));
    m_tabs->addTab(m_audio, tr("Audio"));
    m_tabs->addTab(m_links, tr("Links"));
    m_tabs->addTab(m_files, tr("Files"));

    m_status = new QLabel;
    m_status->setContentsMargins(12, 4, 12, 6);
    m_status->setForegroundRole(QPalette::PlaceholderText);
    m_status->hide();
    lay->addWidget(m_status);

    m_media->thumbnail = [this](int id) { return thumbnailFor(id); };
    m_media->onActivated = [this](int id) { activateMedia(id); };
    m_audio->onActivated = [this](int id) { activateAudio(id); };
    m_links->onActivated = [this](int id) { activateLink(id); };
    m_files->onActivated = [this](int id) { activateFile(id); };
    m_media->onContext = [this](int id, const QPoint &g) { showContextMenu(TabMedia, id, g); };
    m_audio->onContext = [this](int id, const QPoint &g) { showContextMenu(TabAudio, id, g); };
    m_links->onContext = [this](int id, const QPoint &g) { showContextMenu(TabLinks, id, g); };
    m_files->onContext = [this](int id, const QPoint &g) { showContextMenu(TabFiles, id, g); };

    m_audioOut = new QAudioOutput(this);
    m_player = new QMediaPlayer(this);
    m_player->setAudioOutput(m_audioOut);
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 p) {
        m_audioPos = p;
        rebuildAudio();
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 d) {
        m_audioDur = d;
        rebuildAudio();
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this] { rebuildAudio(); });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &e) {
        m_audioLoading = false;
        showStatus(tr("Audio playback failed: %1").arg(e), 10000);
        rebuildAudio();
    });

    reload();
}

MediaGalleryDialog::~MediaGalleryDialog()
{
    if (m_player) m_player->stop();
}

int MediaGalleryDialog::count(int tab) const
{
    switch (tab) {
    case TabMedia: return m_media->total();
    case TabAudio: return m_audio->total();
    case TabLinks: return m_links->total();
    case TabFiles: return m_files->total();
    }
    return 0;
}

bool MediaGalleryDialog::event(QEvent *e)
{
    if (e->type() == QEvent::WindowActivate) reload();
    return QDialog::event(e);
}

void MediaGalleryDialog::closeEvent(QCloseEvent *e)
{
    if (m_player) m_player->stop();
    QDialog::closeEvent(e);
}

void MediaGalleryDialog::reload()
{
    m_col = collect(m_host.messages ? m_host.messages() : QList<Message>());
    // The playing clip may have moved in the list: find it again by identity.
    m_audioId = -1;
    if (m_audioTs > 0) {
        for (int i = 0; i < m_col.audio.size(); ++i)
            if (m_col.audio[i].ts == m_audioTs && m_col.audio[i].attIndex == m_audioAttIndex) m_audioId = i;
    }
    rebuildMedia();
    rebuildAudio();
    rebuildLinks();
    rebuildFiles();
}

void MediaGalleryDialog::showStatus(const QString &text, int timeoutMs)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
    if (timeoutMs > 0) {
        QPointer<MediaGalleryDialog> self(this);
        QTimer::singleShot(timeoutMs, this, [self, text] {
            if (self && self->m_status->text() == text) self->showStatus(QString());
        });
    }
}

// --------------------------------------------------------------- building

static QString detailsTip(const MediaItem &it, const QString &name)
{
    return QStringLiteral("%1\n%2 · %3")
        .arg(name, QLocale().toString(it.time, QLocale::ShortFormat), AttachmentUtil::humanSize(it.att.size));
}

void MediaGalleryDialog::rebuildMedia()
{
    QVector<View::Section> sections;
    QString lastMonth;
    for (int i = 0; i < m_col.media.size(); ++i) {
        const MediaItem &it = m_col.media[i];
        const QString month = monthTitle(it.time.date());
        if (month != lastMonth) {
            sections.append({month, {}});
            lastMonth = month;
        }
        View::Cell c;
        c.id = i;
        c.video = !it.att.isImage();
        c.subtitle = c.video ? AttachmentUtil::humanSize(it.att.size) : QString();
        c.tooltip = detailsTip(it, AttachmentUtil::displayFileName(it.att, it.time));
        sections.last().cells.append(c);
    }
    m_media->setSections(sections);
}

void MediaGalleryDialog::rebuildAudio()
{
    QVector<View::Section> sections;
    QString lastMonth;
    const bool playing = m_player && m_player->playbackState() == QMediaPlayer::PlayingState;
    for (int i = 0; i < m_col.audio.size(); ++i) {
        const MediaItem &it = m_col.audio[i];
        const QString month = monthTitle(it.time.date());
        if (month != lastMonth) {
            sections.append({month, {}});
            lastMonth = month;
        }
        const QString name = AttachmentUtil::displayFileName(it.att, it.time);
        View::Cell c;
        c.id = i;
        c.title = name;
        c.trailing = shortDate(it.time);
        c.tooltip = detailsTip(it, name);
        const bool current = i == m_audioId;
        c.active = current && (playing || m_audioLoading || (m_player && m_player->playbackState() == QMediaPlayer::PausedState));
        c.glyph = current && playing ? QStringLiteral("⏸") : QStringLiteral("▶");
        if (current && m_audioLoading)
            c.subtitle = tr("Loading …");
        else if (current && m_audioDur > 0)
            c.subtitle = formatTime(m_audioPos) + QStringLiteral(" / ") + formatTime(m_audioDur);
        else
            c.subtitle = AttachmentUtil::humanSize(it.att.size);
        sections.last().cells.append(c);
    }
    m_audio->setSections(sections);
}

void MediaGalleryDialog::rebuildLinks()
{
    QVector<View::Section> sections;
    QString lastMonth;
    for (int i = 0; i < m_col.links.size(); ++i) {
        const LinkItem &it = m_col.links[i];
        const QString month = monthTitle(it.time.date());
        if (month != lastMonth) {
            sections.append({month, {}});
            lastMonth = month;
        }
        View::Cell c;
        c.id = i;
        const QUrl url(it.url);
        c.title = url.host().isEmpty() ? it.url : url.host();
        c.subtitle = it.url;
        c.trailing = shortDate(it.time);
        c.glyph = QStringLiteral("🔗");
        c.tooltip = it.url;
        sections.last().cells.append(c);
    }
    m_links->setSections(sections);
}

void MediaGalleryDialog::rebuildFiles()
{
    QVector<View::Section> sections;
    QString lastMonth;
    for (int i = 0; i < m_col.files.size(); ++i) {
        const MediaItem &it = m_col.files[i];
        const QString month = monthTitle(it.time.date());
        if (month != lastMonth) {
            sections.append({month, {}});
            lastMonth = month;
        }
        const QString name = AttachmentUtil::displayFileName(it.att, it.time);
        View::Cell c;
        c.id = i;
        c.title = name;
        const QString ext = QFileInfo(name).suffix().toUpper();
        c.subtitle = ext.isEmpty() ? AttachmentUtil::humanSize(it.att.size)
                                   : ext + QStringLiteral(" · ") + AttachmentUtil::humanSize(it.att.size);
        c.trailing = shortDate(it.time);
        c.glyph = QStringLiteral("📄");
        c.tooltip = detailsTip(it, name);
        sections.last().cells.append(c);
    }
    m_files->setSections(sections);
}

// -------------------------------------------------------------- thumbnails

QPixmap MediaGalleryDialog::thumbnailFor(int id)
{
    if (id < 0 || id >= m_col.media.size()) return {};
    const MediaItem &it = m_col.media[id];
    if (!it.att.isImage()) return {};
    const QString key = QString::number(it.ts) + QLatin1Char('/') + QString::number(it.attIndex);
    if (const QPixmap *cached = m_thumbs.object(key)) return *cached;

    const auto store = [this, &key](const QImage &img) {
        if (img.isNull()) return QPixmap();
        // Square, cropped like the tiles of the official client.
        const QImage scaled = img.scaled(kThumbSide, kThumbSide, Qt::KeepAspectRatioByExpanding,
                                         Qt::SmoothTransformation);
        const QRect crop((scaled.width() - kThumbSide) / 2, (scaled.height() - kThumbSide) / 2, kThumbSide,
                         kThumbSide);
        auto *pm = new QPixmap(QPixmap::fromImage(scaled.copy(crop)));
        const QPixmap result = *pm;
        m_thumbs.insert(key, pm, kThumbSide * kThumbSide * 4 / 1024);
        return result;
    };

    if (!it.att.thumb.isEmpty()) return store(AttachmentUtil::imageFromData(it.att.thumb));

    // No stored preview yet: load the image itself, a few at a time.
    if (!m_thumbFailed.contains(key) && !m_thumbQueued.contains(key) && m_host.fetch && it.att.size <= kMaxThumbSource) {
        m_thumbQueued.insert(key);
        m_thumbQueue.append(key);
        if (!m_pumpScheduled) {
            m_pumpScheduled = true;
            QTimer::singleShot(0, this, [this] {
                m_pumpScheduled = false;
                pumpThumbnails();
            });
        }
    }
    return {};
}

void MediaGalleryDialog::pumpThumbnails()
{
    while (m_thumbInFlight < kThumbConcurrency && !m_thumbQueue.isEmpty()) {
        const QString key = m_thumbQueue.takeFirst();
        // Find the item again (the list may have been rebuilt).
        const MediaItem *found = nullptr;
        for (const MediaItem &it : std::as_const(m_col.media)) {
            if (QString::number(it.ts) + QLatin1Char('/') + QString::number(it.attIndex) == key) {
                found = &it;
                break;
            }
        }
        if (!found) {
            m_thumbQueued.remove(key);
            continue;
        }
        ++m_thumbInFlight;
        QPointer<MediaGalleryDialog> self(this);
        m_host.fetch(found->att, [self, key](const QByteArray &data, const QString &error) {
            if (!self) return;
            --self->m_thumbInFlight;
            self->m_thumbQueued.remove(key);
            const QImage img = error.isEmpty() ? AttachmentUtil::imageFromData(data) : QImage();
            if (img.isNull()) {
                self->m_thumbFailed.insert(key);
            } else {
                const QImage scaled = img.scaled(kThumbSide, kThumbSide, Qt::KeepAspectRatioByExpanding,
                                                 Qt::SmoothTransformation);
                const QRect crop((scaled.width() - kThumbSide) / 2, (scaled.height() - kThumbSide) / 2,
                                 kThumbSide, kThumbSide);
                self->m_thumbs.insert(key, new QPixmap(QPixmap::fromImage(scaled.copy(crop))),
                                      kThumbSide * kThumbSide * 4 / 1024);
                self->m_media->viewport()->update();
            }
            QTimer::singleShot(0, self, [self] {
                if (self) self->pumpThumbnails();
            });
        });
    }
}

// ----------------------------------------------------------------- actions

QString MediaGalleryDialog::writeTempFile(const QByteArray &data, const QString &prefix, const Attachment &att)
{
    const QString dir = m_host.tempDir ? m_host.tempDir() : QString();
    if (dir.isEmpty()) return {};
    QString suffix = QFileInfo(att.filename).suffix();
    if (suffix.isEmpty()) suffix = QMimeDatabase().mimeTypeForName(att.contentType).preferredSuffix();
    const QString path = dir + QLatin1Char('/') + prefix + QUuid::createUuid().toString(QUuid::WithoutBraces) +
                         (suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) return {};
    return path;
}

void MediaGalleryDialog::activateMedia(int id)
{
    if (id < 0 || id >= m_col.media.size()) return;
    const MediaItem item = m_col.media[id];
    if (item.att.isImage()) openImage(item);
    else openVideo(item);
}

void MediaGalleryDialog::openImage(const MediaItem &item)
{
    if (!m_host.fetch) return;
    showStatus(tr("Loading …"));
    QPointer<MediaGalleryDialog> self(this);
    m_host.fetch(item.att, [self, item](const QByteArray &data, const QString &error) {
        if (!self) return;
        self->showStatus(QString());
        const QString name = AttachmentUtil::displayFileName(item.att, item.time);
        QByteArray bytes = data;
        if (!error.isEmpty()) {
            if (item.att.thumb.isEmpty()) { // nothing to show at all
                QMessageBox::warning(self, tr("Attachment"), error);
                return;
            }
            bytes = item.att.thumb; // fall back to the stored preview
        }
        auto *viewer = new ImageViewer(bytes, name, self);
        if (!viewer->isValid()) {
            delete viewer;
            QMessageBox::warning(self, tr("Attachment"), tr("This image cannot be displayed."));
            return;
        }
        QObject::connect(viewer, &ImageViewer::saved, self, [self](const QString &path) {
            if (self->m_host.saved) self->m_host.saved(path);
        });
        viewer->setAttribute(Qt::WA_DeleteOnClose);
        viewer->show();
        viewer->raise();
        viewer->activateWindow();
    });
}

void MediaGalleryDialog::openVideo(const MediaItem &item)
{
    if (!m_host.fetch) return;
    showStatus(tr("Loading …"));
    QPointer<MediaGalleryDialog> self(this);
    m_host.fetch(item.att, [self, item](const QByteArray &data, const QString &error) {
        if (!self) return;
        self->showStatus(QString());
        if (!error.isEmpty() || data.isEmpty()) {
            self->showStatus(error.isEmpty() ? tr("The video file is empty.") : error, 10000);
            return;
        }
        const QString path = self->writeTempFile(data, QStringLiteral("gallery-video-"), item.att);
        if (path.isEmpty()) {
            self->showStatus(tr("The video file could not be prepared for playback."), 10000);
            return;
        }
        auto *viewer = new VideoViewer(path, AttachmentUtil::displayFileName(item.att, item.time), self);
        viewer->show();
        viewer->raise();
        viewer->activateWindow();
    });
}

void MediaGalleryDialog::activateAudio(int id)
{
    if (id < 0 || id >= m_col.audio.size() || !m_host.fetch) return;
    const MediaItem item = m_col.audio[id];

    const bool same = id == m_audioId;
    if (same && m_audioLoading) return;
    if (same && m_player->source().isValid()) {
        if (m_player->playbackState() == QMediaPlayer::PlayingState) {
            m_player->pause();
        } else {
            if (m_audioDur > 0 && m_audioPos >= m_audioDur) m_player->setPosition(0);
            m_player->play();
        }
        return;
    }

    m_player->stop();
    m_player->setSource(QUrl());
    m_audioId = id;
    m_audioTs = item.ts;
    m_audioAttIndex = item.attIndex;
    m_audioPos = 0;
    m_audioDur = 0;
    m_audioLoading = true;
    rebuildAudio();

    QPointer<MediaGalleryDialog> self(this);
    m_host.fetch(item.att, [self, item](const QByteArray &data, const QString &error) {
        if (!self || self->m_audioTs != item.ts || self->m_audioAttIndex != item.attIndex) return;
        self->m_audioLoading = false;
        if (!error.isEmpty() || data.isEmpty()) {
            self->showStatus(error.isEmpty() ? tr("The audio file is empty.") : error, 10000);
            self->rebuildAudio();
            return;
        }
        const QString path = self->writeTempFile(data, QStringLiteral("gallery-audio-"), item.att);
        if (path.isEmpty()) {
            self->showStatus(tr("The audio file could not be prepared for playback."), 10000);
            self->rebuildAudio();
            return;
        }
        self->m_player->setSource(QUrl::fromLocalFile(path));
        self->m_player->play();
        self->rebuildAudio();
    });
}

void MediaGalleryDialog::activateFile(int id)
{
    if (id < 0 || id >= m_col.files.size() || !m_host.fetch) return;
    const MediaItem item = m_col.files[id];
    if (AttachmentUtil::isRiskyFileName(item.att.filename)) {
        QMessageBox::information(this, tr("Attachment"),
                                 tr("For safety, “%1” is not opened directly. Use “Save as …” from the right-click menu instead.")
                                     .arg(item.att.filename));
        return;
    }
    showStatus(tr("Loading …"));
    QPointer<MediaGalleryDialog> self(this);
    m_host.fetch(item.att, [self, item](const QByteArray &data, const QString &error) {
        if (!self) return;
        self->showStatus(QString());
        if (!error.isEmpty()) {
            QMessageBox::warning(self, tr("Attachment"), error);
            return;
        }
        const QString dir = self->m_host.tempDir ? self->m_host.tempDir() : QString();
        if (dir.isEmpty()) {
            QMessageBox::warning(self, tr("Attachment"), tr("No temporary directory available."));
            return;
        }
        QString name = QFileInfo(item.att.filename).fileName();
        if (name.isEmpty()) name = AttachmentUtil::displayFileName(item.att, item.time);
        const QString folder = dir + QLatin1String("/gallery-") + QString::number(++self->m_tempCounter);
        QDir().mkpath(folder);
        const QString path = folder + QLatin1Char('/') + name;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
            QMessageBox::warning(self, tr("Attachment"), tr("The file could not be written."));
            return;
        }
        f.close();
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
}

void MediaGalleryDialog::activateLink(int id)
{
    if (id < 0 || id >= m_col.links.size()) return;
    const QUrl url(m_col.links[id].url);
    if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(url);
}

void MediaGalleryDialog::saveItem(const MediaItem &item)
{
    if (!m_host.fetch) return;
    showStatus(tr("Loading …"));
    QPointer<MediaGalleryDialog> self(this);
    m_host.fetch(item.att, [self, item](const QByteArray &data, const QString &error) {
        if (!self) return;
        self->showStatus(QString());
        if (!error.isEmpty()) {
            QMessageBox::warning(self, tr("Attachment"), error);
            return;
        }
        const QString path = QFileDialog::getSaveFileName(self, tr("Save attachment"),
                                                          AttachmentUtil::displayFileName(item.att, item.time));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
            QMessageBox::warning(self, tr("Attachment"), tr("The file could not be written."));
            return;
        }
        f.close();
        if (self->m_host.saved) self->m_host.saved(path);
    });
}

void MediaGalleryDialog::showContextMenu(Tab tab, int id, const QPoint &globalPos)
{
    const QVector<MediaItem> *items = tab == TabMedia   ? &m_col.media
                                      : tab == TabAudio ? &m_col.audio
                                      : tab == TabFiles ? &m_col.files
                                                        : nullptr;
    qint64 ts = 0;
    QMenu menu(this);
    if (tab == TabLinks) {
        if (id < 0 || id >= m_col.links.size()) return;
        const QString url = m_col.links[id].url;
        ts = m_col.links[id].ts;
        menu.addAction(tr("Open link"), this, [this, id] { activateLink(id); });
        menu.addAction(tr("Copy link"), this, [url] { QApplication::clipboard()->setText(url); });
    } else {
        if (id < 0 || id >= items->size()) return;
        const MediaItem item = (*items)[id];
        ts = item.ts;
        if (tab == TabMedia) menu.addAction(item.att.isImage() ? tr("View") : tr("Play"), this, [this, id] { activateMedia(id); });
        else if (tab == TabAudio) menu.addAction(tr("Play / pause"), this, [this, id] { activateAudio(id); });
        else menu.addAction(tr("Open"), this, [this, id] { activateFile(id); });
        menu.addAction(tr("Save as …"), this, [this, item] { saveItem(item); });
    }
    if (m_host.showInChat) {
        menu.addSeparator();
        menu.addAction(tr("Show in chat"), this, [this, ts] {
            if (m_host.showInChat) m_host.showInChat(ts);
            close();
        });
    }
    menu.exec(globalPos);
}
