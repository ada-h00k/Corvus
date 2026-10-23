#include "EmojiPicker.h"

#include "GiphyPicker.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCursor>
#include <QFile>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace emojipicker {

struct Entry {
    QString emoji;
    QString name;
    QString nameFolded;
    QStringList tones;
};

struct Section {
    QString key;   // category key / title shown above the emojis
    QString icon;  // icon in the category bar
    QString title; // empty: no header (search results)
    QVector<Entry> entries;
};

namespace {

struct Category {
    const char *key;
    const char *icon;
};

const Category kCategories[] = {
    {"Smileys & People", "😀"},
    {"Animals & Nature", "🐻"},
    {"Food & Drink", "🍕"},
    {"Travel & Places", "🚗"},
    {"Activities", "⚽"},
    {"Objects", "💡"},
    {"Symbols", "🔣"},
    {"Flags", "🏁"},
};

constexpr int kRecentLimit = 16;
constexpr int kColumns = 8;
constexpr int kCellSize = 40;
constexpr int kHeaderHeight = 30;
constexpr int kSectionGap = 6;

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

QFont emojiFont(const QFont &base)
{
    QFont f = base;
    f.setPointSizeF(19);
    return f;
}

} // namespace

// ---------------------------------------------------------------------------
// Grid: all sections in one scrollable, custom painted view
// ---------------------------------------------------------------------------
class Grid : public QAbstractScrollArea
{
public:
    std::function<void(const QString &)> onPick;
    std::function<void(const Entry &, const QRect &)> onTones;
    std::function<void(int)> onSectionChanged;

    explicit Grid(QWidget *parent = nullptr) : QAbstractScrollArea(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setFocusPolicy(Qt::NoFocus);
        viewport()->setAutoFillBackground(false);
        viewport()->setMouseTracking(true);
        verticalScrollBar()->setSingleStep(kCellSize);

        m_holdTimer.setSingleShot(true);
        m_holdTimer.setInterval(450);
        QObject::connect(&m_holdTimer, &QTimer::timeout, this, [this] {
            if (!m_pressed.valid()) return;
            const Entry *e = entryAt(m_pressed);
            if (e && !e->tones.isEmpty()) {
                m_pressSuppressed = true;
                openTones(m_pressed);
            }
        });
        QObject::connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
            updateHover(viewport()->mapFromGlobal(QCursor::pos()));
            notifySection();
        });
    }

    void setSections(const QVector<Section> &sections, bool resetScroll)
    {
        m_sections = sections;
        m_hover = {};
        m_pressed = {};
        relayout();
        if (resetScroll) verticalScrollBar()->setValue(0);
        viewport()->update();
        notifySection();
    }

    const QVector<Section> &sections() const { return m_sections; }

    void scrollToSection(int index)
    {
        if (index < 0 || index >= m_sectionTop.size()) return;
        verticalScrollBar()->setValue(m_sectionTop[index]);
    }

    int currentSection() const
    {
        const int y = verticalScrollBar()->value() + kHeaderHeight / 2;
        int current = 0;
        for (int i = 0; i < m_sectionTop.size(); ++i)
            if (m_sectionTop[i] <= y) current = i;
        return current;
    }

    QStringList emojis() const
    {
        QStringList out;
        for (const Section &s : m_sections)
            for (const Entry &e : s.entries) out << e.emoji;
        return out;
    }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QAbstractScrollArea::resizeEvent(e);
        relayout();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(viewport());
        p.setRenderHint(QPainter::TextAntialiasing);
        const int scroll = verticalScrollBar()->value();
        const QRect view = viewport()->rect();

        if (m_total == 0) {
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(view, Qt::AlignCenter, tr("No emojis found"));
            return;
        }

        QFont headerFont = font();
        headerFont.setBold(true);
        headerFont.setPointSizeF(qMax(8.0, headerFont.pointSizeF() - 0.5));
        const QFont eFont = emojiFont(font());
        const int cw = cellWidth();

        for (int s = 0; s < m_sections.size(); ++s) {
            const Section &sec = m_sections[s];
            const int top = m_sectionTop[s] - scroll;
            const int rows = (sec.entries.size() + kColumns - 1) / kColumns;
            const int bottom = top + headerPx(sec) + rows * kCellSize;
            if (bottom < 0 || top > view.height()) continue;

            if (!sec.title.isEmpty()) {
                p.setFont(headerFont);
                p.setPen(palette().color(QPalette::PlaceholderText));
                p.drawText(QRect(8, top, view.width() - 16, kHeaderHeight),
                           Qt::AlignLeft | Qt::AlignBottom, sec.title);
            }

            p.setFont(eFont);
            const int gridTop = top + headerPx(sec);
            const int firstRow = qMax(0, (-gridTop) / kCellSize);
            const int lastRow = qMin(rows - 1, (view.height() - gridTop) / kCellSize);
            for (int row = firstRow; row <= lastRow; ++row) {
                for (int col = 0; col < kColumns; ++col) {
                    const int i = row * kColumns + col;
                    if (i >= sec.entries.size()) break;
                    const QRect cell(col * cw, gridTop + row * kCellSize, cw, kCellSize);
                    const Entry &e = sec.entries[i];
                    const bool hovered = m_hover.section == s && m_hover.index == i;
                    if (hovered) {
                        p.setPen(Qt::NoPen);
                        p.setBrush(withAlpha(palette().color(QPalette::Text), 32));
                        p.drawRoundedRect(cell.adjusted(2, 2, -2, -2), 8, 8);
                    }
                    p.setPen(palette().color(QPalette::Text));
                    p.drawText(cell, Qt::AlignCenter, e.emoji);

                    if (!e.tones.isEmpty()) {
                        // Small corner marker: "has skin tone variants".
                        const QPoint br = cell.bottomRight() - QPoint(6, 6);
                        QPainterPath tri;
                        tri.moveTo(br);
                        tri.lineTo(br - QPoint(5, 0));
                        tri.lineTo(br - QPoint(0, 5));
                        tri.closeSubpath();
                        p.setPen(Qt::NoPen);
                        p.setBrush(withAlpha(palette().color(QPalette::Text), hovered ? 150 : 70));
                        p.drawPath(tri);
                    }
                }
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        updateHover(e->position().toPoint());
        if (m_pressed.valid() && !(m_hover == m_pressed)) m_holdTimer.stop();
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = {};
        viewport()->update();
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        const Pos pos = posAt(e->position().toPoint());
        if (e->button() == Qt::RightButton) {
            const Entry *en = entryAt(pos);
            if (en && !en->tones.isEmpty()) openTones(pos);
            return;
        }
        if (e->button() != Qt::LeftButton) return;
        m_pressed = pos;
        m_pressSuppressed = false;
        m_holdTimer.start();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        m_holdTimer.stop();
        const Pos pos = posAt(e->position().toPoint());
        const bool click = pos.valid() && pos == m_pressed && !m_pressSuppressed;
        m_pressed = {};
        if (!click) return;
        if (const Entry *en = entryAt(pos); en && onPick) {
            // Copy first: onPick rebuilds the sections, which frees `*en`.
            const QString emoji = en->emoji;
            onPick(emoji);
        }
    }

    bool viewportEvent(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const Entry *en = entryAt(posAt(he->pos()));
            if (en && !en->name.isEmpty()) {
                QToolTip::showText(he->globalPos(), en->name, viewport());
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
        int section = -1;
        int index = -1;
        bool valid() const { return section >= 0 && index >= 0; }
        bool operator==(const Pos &o) const { return section == o.section && index == o.index; }
    };

    int cellWidth() const { return qMax(kCellSize / 2, viewport()->width() / kColumns); }
    static int headerPx(const Section &s) { return s.title.isEmpty() ? 0 : kHeaderHeight; }

    void relayout()
    {
        m_sectionTop.clear();
        int y = 0;
        m_total = 0;
        for (const Section &s : m_sections) {
            m_sectionTop.append(y);
            const int rows = (s.entries.size() + kColumns - 1) / kColumns;
            y += headerPx(s) + rows * kCellSize + kSectionGap;
            m_total += s.entries.size();
        }
        m_contentHeight = y;
        verticalScrollBar()->setRange(0, qMax(0, m_contentHeight - viewport()->height()));
        verticalScrollBar()->setPageStep(viewport()->height());
    }

    Pos posAt(const QPoint &pt) const
    {
        const int y = pt.y() + verticalScrollBar()->value();
        const int col = pt.x() / cellWidth();
        if (col < 0 || col >= kColumns) return {};
        for (int s = 0; s < m_sections.size(); ++s) {
            const Section &sec = m_sections[s];
            const int gridTop = m_sectionTop[s] + headerPx(sec);
            const int rows = (sec.entries.size() + kColumns - 1) / kColumns;
            if (y < gridTop || y >= gridTop + rows * kCellSize) continue;
            const int idx = ((y - gridTop) / kCellSize) * kColumns + col;
            if (idx < sec.entries.size()) return {s, idx};
            return {};
        }
        return {};
    }

    const Entry *entryAt(const Pos &p) const
    {
        if (!p.valid()) return nullptr;
        return &m_sections[p.section].entries[p.index];
    }

    QRect cellRect(const Pos &p) const
    {
        const Section &sec = m_sections[p.section];
        const int row = p.index / kColumns;
        const int col = p.index % kColumns;
        const int cw = cellWidth();
        return QRect(col * cw,
                     m_sectionTop[p.section] + headerPx(sec) + row * kCellSize
                         - verticalScrollBar()->value(),
                     cw, kCellSize);
    }

    void openTones(const Pos &p)
    {
        const Entry *en = entryAt(p);
        if (!en || !onTones) return;
        QRect r = cellRect(p);
        r.moveTopLeft(viewport()->mapToGlobal(r.topLeft()));
        onTones(*en, r);
    }

    void updateHover(const QPoint &pt)
    {
        const Pos p = posAt(pt);
        if (p == m_hover) return;
        m_hover = p;
        setCursor(p.valid() ? Qt::PointingHandCursor : Qt::ArrowCursor);
        viewport()->update();
    }

    void notifySection()
    {
        if (!onSectionChanged || m_sections.isEmpty()) return;
        onSectionChanged(currentSection());
    }

    QVector<Section> m_sections;
    QVector<int> m_sectionTop;
    int m_contentHeight = 0;
    int m_total = 0;
    Pos m_hover;
    Pos m_pressed;
    bool m_pressSuppressed = false;
    QTimer m_holdTimer;
};

// ---------------------------------------------------------------------------
// Category bar: one icon per section, highlights the visible one
// ---------------------------------------------------------------------------
class CategoryBar : public QWidget
{
public:
    std::function<void(int)> onSelected;

    explicit CategoryBar(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(40);
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);
    }

    void setItems(const QStringList &icons, const QStringList &names)
    {
        m_icons = icons;
        m_names = names;
        if (m_current >= m_icons.size()) m_current = 0;
        m_hover = -1;
        update();
    }

    void setCurrent(int index)
    {
        if (index == m_current) return;
        m_current = index;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(withAlpha(palette().color(QPalette::Text), 40));
        p.drawLine(0, 0, width(), 0);
        if (m_icons.isEmpty()) return;

        QFont f = emojiFont(font());
        f.setPointSizeF(15);
        p.setFont(f);
        for (int i = 0; i < m_icons.size(); ++i) {
            const QRect r = itemRect(i);
            const bool current = i == m_current;
            if (i == m_hover && !current) {
                p.setPen(Qt::NoPen);
                p.setBrush(withAlpha(palette().color(QPalette::Text), 28));
                p.drawRoundedRect(r.adjusted(2, 4, -2, -6), 8, 8);
            }
            p.setOpacity(current || i == m_hover ? 1.0 : 0.55);
            p.setPen(palette().color(QPalette::Text));
            p.drawText(r.adjusted(0, 0, 0, -3), Qt::AlignCenter, m_icons[i]);
            p.setOpacity(1.0);
            if (current) {
                p.setPen(Qt::NoPen);
                p.setBrush(palette().color(QPalette::Highlight));
                p.drawRoundedRect(QRect(r.center().x() - 9, height() - 5, 18, 3), 1.5, 1.5);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override { setHover(indexAt(e->position().toPoint())); }
    void leaveEvent(QEvent *) override { setHover(-1); }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int i = indexAt(e->position().toPoint());
        if (i < 0) return;
        m_current = i;
        update();
        if (onSelected) onSelected(i);
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const int i = indexAt(he->pos());
            if (i >= 0 && i < m_names.size()) {
                QToolTip::showText(he->globalPos(), m_names[i], this);
            } else {
                QToolTip::hideText();
                e->ignore();
            }
            return true;
        }
        return QWidget::event(e);
    }

private:
    QRect itemRect(int i) const
    {
        const int w = width() / qMax(1, int(m_icons.size()));
        return QRect(i * w, 0, w, height());
    }

    int indexAt(const QPoint &pt) const
    {
        for (int i = 0; i < m_icons.size(); ++i)
            if (itemRect(i).contains(pt)) return i;
        return -1;
    }

    void setHover(int i)
    {
        if (i == m_hover) return;
        m_hover = i;
        update();
    }

    QStringList m_icons;
    QStringList m_names;
    int m_current = 0;
    int m_hover = -1;
};

// ---------------------------------------------------------------------------
// Mode bar: "Emoji | GIF" tabs above the search field
// ---------------------------------------------------------------------------
class ModeBar : public QWidget
{
public:
    std::function<void(int)> onSelected;

    ModeBar(const QStringList &labels, QWidget *parent = nullptr) : QWidget(parent), m_labels(labels)
    {
        setFixedHeight(34);
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);
    }

    int current() const { return m_current; }
    void setCurrent(int index)
    {
        if (index == m_current) return;
        m_current = index;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // thin separator under the whole bar
        p.setPen(withAlpha(palette().color(QPalette::Text), 40));
        p.drawLine(0, height() - 1, width(), height() - 1);

        for (int i = 0; i < m_labels.size(); ++i) {
            const QRect r = itemRect(i);
            const bool current = i == m_current;
            if (i == m_hover && !current) {
                p.setPen(Qt::NoPen);
                p.setBrush(withAlpha(palette().color(QPalette::Text), 24));
                p.drawRoundedRect(r.adjusted(4, 3, -4, -5), 7, 7);
            }
            QFont f = font();
            f.setBold(current);
            p.setFont(f);
            p.setOpacity(current || i == m_hover ? 1.0 : 0.65);
            p.setPen(palette().color(QPalette::Text));
            p.drawText(r.adjusted(0, 0, 0, -2), Qt::AlignCenter, m_labels[i]);
            p.setOpacity(1.0);
            if (current) {
                p.setPen(Qt::NoPen);
                p.setBrush(palette().color(QPalette::Highlight));
                p.drawRoundedRect(QRect(r.left() + 10, height() - 3, r.width() - 20, 3), 1.5, 1.5);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override { setHover(indexAt(e->position().toPoint())); }
    void leaveEvent(QEvent *) override { setHover(-1); }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int i = indexAt(e->position().toPoint());
        if (i < 0) return;
        if (onSelected) onSelected(i);
    }

private:
    QRect itemRect(int i) const
    {
        const int w = width() / qMax(1, int(m_labels.size()));
        return QRect(i * w, 0, w, height());
    }
    int indexAt(const QPoint &pt) const
    {
        for (int i = 0; i < m_labels.size(); ++i)
            if (itemRect(i).contains(pt)) return i;
        return -1;
    }
    void setHover(int i)
    {
        if (i == m_hover) return;
        m_hover = i;
        update();
    }

    QStringList m_labels;
    int m_current = 0;
    int m_hover = -1;
};

// ---------------------------------------------------------------------------
// Skin tone popup (opened from the grid)
// ---------------------------------------------------------------------------
class TonePopup : public QFrame
{
public:
    TonePopup(const Entry &entry, QWidget *parent, std::function<void(const QString &)> pick)
        : QFrame(parent, Qt::Popup)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);
        auto *grid = new QGridLayout(this);
        grid->setContentsMargins(4, 4, 4, 4);
        grid->setSpacing(2);

        QFont f = emojiFont(font());
        QStringList variants;
        variants << entry.emoji << entry.tones; // base emoji first
        const int columns = entry.tones.size() > 5 ? 5 : int(variants.size());
        for (int i = 0; i < variants.size(); ++i) {
            const QString v = variants[i];
            auto *b = new QToolButton;
            b->setText(v);
            b->setFont(f);
            b->setAutoRaise(true);
            b->setFixedSize(kCellSize, kCellSize);
            b->setCursor(Qt::PointingHandCursor);
            QObject::connect(b, &QToolButton::clicked, this, [this, v, pick] {
                close();
                pick(v);
            });
            grid->addWidget(b, i / columns, i % columns);
        }
    }
};

} // namespace emojipicker

using namespace emojipicker;

QStringList EmojiPicker::quickReactions()
{
    return {QStringLiteral("👍"), QStringLiteral("❤️"), QStringLiteral("😂"),
            QStringLiteral("😮"), QStringLiteral("😢"), QStringLiteral("🙏")};
}

EmojiPicker::EmojiPicker(QWidget *parent)
    : QFrame(parent, Qt::Popup),
      m_browse(new QVector<Section>),
      m_all(new QVector<Entry>)
{
    setFrameShape(QFrame::StyledPanel);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 0);
    lay->setSpacing(6);

    // "Emoji | GIF" tabs, only in the message composer.
    m_modeBar = new ModeBar({QStringLiteral("Emoji"), QStringLiteral("GIF")});
    m_modeBar->hide();
    lay->addWidget(m_modeBar);

    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText(QStringLiteral("Search emojis…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setToolTip(QStringLiteral("Search by emoji name"));
    m_searchEdit->setMinimumHeight(32);
    lay->addWidget(m_searchEdit);

    m_pages = new QStackedWidget;
    lay->addWidget(m_pages, 1);

    // Page 1: emojis (grid + category bar)
    auto *emojiPage = new QWidget;
    auto *emojiLay = new QVBoxLayout(emojiPage);
    emojiLay->setContentsMargins(0, 0, 0, 0);
    emojiLay->setSpacing(6);
    m_grid = new Grid;
    emojiLay->addWidget(m_grid, 1);
    m_bar = new CategoryBar;
    emojiLay->addWidget(m_bar);
    m_pages->addWidget(emojiPage);

    // Page 2: GIFs
    m_gif = new GiphyPicker;
    m_pages->addWidget(m_gif);

    // --- data
    QFile catalogFile(QStringLiteral(":/emoji/catalog.json"));
    if (!catalogFile.open(QIODevice::ReadOnly))
        qFatal("Could not open embedded emoji catalog");
    const QJsonObject catalog = QJsonDocument::fromJson(catalogFile.readAll()).object();

    QFile namesFile(QStringLiteral(":/emoji/names.json"));
    if (!namesFile.open(QIODevice::ReadOnly))
        qFatal("Could not open embedded emoji name index");
    const QJsonObject names = QJsonDocument::fromJson(namesFile.readAll()).object();

    for (const Category &cat : kCategories) {
        Section sec;
        sec.key = QString::fromUtf8(cat.key);
        sec.title = sec.key;
        sec.icon = QString::fromUtf8(cat.icon);
        const QJsonArray list = catalog.value(sec.key).toArray();
        for (const QJsonValue &item : list) {
            const QJsonObject obj = item.toObject();
            Entry e;
            e.emoji = obj.value(QStringLiteral("emoji")).toString();
            e.name = names.value(e.emoji).toString();
            e.nameFolded = e.name.toCaseFolded();
            for (const QJsonValue &t : obj.value(QStringLiteral("tones")).toArray())
                e.tones << t.toString();
            sec.entries.append(e);
            m_index.insert(e.emoji, int(m_all->size()));
            m_all->append(e);
        }
        m_browse->append(sec);
    }

    // --- wiring
    m_grid->onPick = [this](const QString &emoji) { activateEmoji(emoji); };
    m_grid->onTones = [this](const Entry &e, const QRect &r) { showTonePopup(e, r); };
    m_grid->onSectionChanged = [this](int i) { m_bar->setCurrent(i); };
    m_bar->onSelected = [this](int i) { m_grid->scrollToSection(i); };

    m_modeBar->onSelected = [this](int i) { setMode(i == 1 ? Mode::Gif : Mode::Emoji); };
    connect(m_gif, &GiphyPicker::gifSelected, this, [this](const QUrl &url, const QUrl &analytics) {
        hide();
        emit gifSelected(url, analytics);
    });
    connect(m_gif, &GiphyPicker::configureRequested, this, [this] {
        hide();
        emit giphyConfigureRequested();
    });

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_mode == Mode::Gif) m_gif->setQuery(text);
        else updateSearchResults(text);
    });
    // Enter picks the first emoji, or searches GIFs right away.
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this] {
        if (m_mode == Mode::Gif) {
            m_gif->searchNow();
            return;
        }
        const QStringList shown = m_grid->emojis();
        if (!m_searchEdit->text().trimmed().isEmpty() && !shown.isEmpty())
            activateEmoji(shown.first());
    });

    rebuildSections();
    setFixedSize(kColumns * kCellSize + 16 + 24, 460);
}

EmojiPicker::~EmojiPicker()
{
    delete m_browse;
    delete m_all;
}

int EmojiPicker::emojiCount() const { return int(m_all->size()); }

QStringList EmojiPicker::toneVariants(const QString &emoji) const
{
    const auto it = m_index.constFind(emoji);
    return it == m_index.constEnd() ? QStringList() : (*m_all)[it.value()].tones;
}

QStringList EmojiPicker::visibleEmojis() const { return m_grid->emojis(); }

void EmojiPicker::activateEmoji(const QString &emojiRef)
{
    // `emojiRef` may point into the grid's own data, which rebuildSections()
    // replaces below, so work on a copy.
    const QString emoji = emojiRef;
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("emojiPicker/recent")).toStringList();
    recent.removeAll(emoji);
    recent.prepend(emoji);
    while (recent.size() > kRecentLimit)
        recent.removeLast();
    settings.setValue(QStringLiteral("emojiPicker/recent"), recent);

    rebuildSections();
    hide();
    emit emojiSelected(emoji);
}

// Builds the browse view: "Recently used" (if any) followed by all categories.
void EmojiPicker::rebuildSections(bool resetScroll)
{
    QVector<Section> sections;

    QSettings settings;
    const QStringList recent = settings.value(QStringLiteral("emojiPicker/recent")).toStringList();
    if (!recent.isEmpty()) {
        Section sec;
        sec.key = QStringLiteral("recent");
        sec.title = QStringLiteral("Recently used");
        sec.icon = QStringLiteral("🕒");
        for (const QString &emoji : recent) {
            const auto it = m_index.constFind(emoji);
            if (it != m_index.constEnd()) {
                sec.entries.append((*m_all)[it.value()]);
            } else {
                // A skin tone variant: shown as is, without further variants.
                Entry e;
                e.emoji = emoji;
                sec.entries.append(e);
            }
        }
        sections.append(sec);
    }
    sections += *m_browse;

    QStringList icons, titles;
    for (const Section &s : sections) {
        icons << s.icon;
        titles << s.title;
    }
    m_bar->setItems(icons, titles);

    if (m_searchEdit && !m_searchEdit->text().trimmed().isEmpty())
        return; // keep showing search results
    m_grid->setSections(sections, resetScroll);
    m_bar->setVisible(true);
}

void EmojiPicker::updateSearchResults(const QString &query)
{
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        rebuildSections(true);
        return;
    }

    const QStringList tokens = trimmed.toCaseFolded().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    struct Hit {
        int rank;
        int order;
        const Entry *entry;
    };
    QVector<Hit> hits;
    for (int i = 0; i < m_all->size(); ++i) {
        const Entry &e = (*m_all)[i];
        bool all = true;
        int rank = 2;
        for (const QString &t : tokens) {
            const int pos = e.nameFolded.indexOf(t);
            if (pos < 0) {
                all = false;
                break;
            }
            if (pos == 0)
                rank = qMin(rank, 0);
            else if (e.nameFolded.at(pos - 1) == QLatin1Char(' '))
                rank = qMin(rank, 1);
        }
        if (!all && e.emoji != trimmed) continue;
        hits.append({e.emoji == trimmed ? -1 : rank, i, &e});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        return a.rank < b.rank;
    });

    Section sec;
    sec.key = QStringLiteral("results");
    for (const Hit &h : hits) sec.entries.append(*h.entry);
    m_grid->setSections({sec}, true);
    m_bar->setVisible(false);
}

void EmojiPicker::showTonePopup(const Entry &entry, const QRect &globalCellRect)
{
    auto *tones = new TonePopup(entry, this, [this](const QString &v) { activateEmoji(v); });
    tones->adjustSize();
    QPoint p(globalCellRect.center().x() - tones->width() / 2,
             globalCellRect.top() - tones->height() - 2);
    if (const QScreen *s = QGuiApplication::screenAt(globalCellRect.center())) {
        const QRect r = s->availableGeometry();
        if (p.y() < r.top()) p.setY(globalCellRect.bottom() + 2);
        p.setX(qBound(r.left(), p.x(), r.right() - tones->width()));
    }
    tones->move(p);
    tones->show();
}

void EmojiPicker::setGifsAvailable(bool available)
{
    m_gifsAvailable = available;
    m_modeBar->setVisible(available);
    if (!available && m_mode == Mode::Gif) setMode(Mode::Emoji);
}

void EmojiPicker::setGiphyApiKey(const QString &apiKey, const QString &error)
{
    m_gif->setApiKey(apiKey, error);
}

void EmojiPicker::setGiphyApiBase(const QUrl &apiBase)
{
    m_gif->setApiBase(apiBase);
}

bool EmojiPicker::isGifTabActive() const
{
    return m_mode == Mode::Gif;
}

void EmojiPicker::showGifTab()
{
    if (m_gifsAvailable) setMode(Mode::Gif);
}

// Switches between the emoji and the GIF page; both share the search field.
void EmojiPicker::setMode(Mode mode)
{
    if (mode == Mode::Gif && !m_gifsAvailable) mode = Mode::Emoji;
    m_mode = mode;
    m_modeBar->setCurrent(mode == Mode::Gif ? 1 : 0);

    // Each tab starts with an empty search.
    m_searchEdit->blockSignals(true);
    m_searchEdit->clear();
    m_searchEdit->blockSignals(false);

    if (mode == Mode::Gif) {
        m_searchEdit->setPlaceholderText(QStringLiteral("Search GIFs…"));
        m_searchEdit->setToolTip(QStringLiteral("Search GIPHY"));
        m_pages->setCurrentWidget(m_gif);
        emit giphyKeyRequested(); // answered synchronously via setGiphyApiKey()
        m_gif->setQuery(QString()); // no-op if the panel is already showing trending GIFs
    } else {
        m_searchEdit->setPlaceholderText(QStringLiteral("Search emojis…"));
        m_searchEdit->setToolTip(QStringLiteral("Search by emoji name"));
        m_pages->setCurrentIndex(0);
        rebuildSections(true);
    }
    m_searchEdit->setFocus();
}

void EmojiPicker::popup(const QPoint &globalPos)
{
    // Always start fresh: no old query, back at the top.
    if (m_mode == Mode::Gif && m_gifsAvailable) {
        setMode(Mode::Gif);
    } else {
        m_mode = Mode::Emoji;
        m_modeBar->setCurrent(0);
        m_pages->setCurrentIndex(0);
        m_searchEdit->setPlaceholderText(QStringLiteral("Search emojis…"));
        m_searchEdit->clear();
        rebuildSections(true);
    }

    QPoint p = globalPos;
    if (const QScreen *s = QGuiApplication::screenAt(globalPos)) {
        const QRect r = s->availableGeometry();
        p.setX(qBound(r.left(), p.x(), r.right() - width()));
        p.setY(qBound(r.top(), p.y(), r.bottom() - height()));
    }
    move(p);
    show();
    m_searchEdit->setFocus();
}
