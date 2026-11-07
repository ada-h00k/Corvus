#include "StickerPicker.h"

#include "Attachments.h"
#include "ApngMovie.h"
#include "SignalCli.h"

#include <QBuffer>
#include <QFrame>
#include <QFont>
#include <QJsonArray>
#include <QMovie>
#include <QPixmap>
#include <QRegularExpression>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QPushButton>
#include <QToolButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>

namespace {
QString strValue(const QJsonObject &o, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QString value = o.value(key).toString().trimmed();
        if (!value.isEmpty()) return value;
    }
    return {};
}
int intValue(const QJsonObject &o, const QStringList &keys, int fallback = -1)
{
    for (const QString &key : keys) {
        const QJsonValue v = o.value(key);
        if (v.isDouble()) return v.toInt(fallback);
        if (v.isString()) {
            bool ok = false;
            const int n = v.toString().toInt(&ok);
            if (ok) return n;
        }
    }
    return fallback;
}
QString cacheKey(const QString &pack, int id) { return pack + QLatin1Char(':') + QString::number(id); }
}

StickerPicker::StickerPicker(SignalCli *cli, QWidget *parent)
    : QWidget(parent), m_cli(cli)
{
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(2, 2, 2, 2);
    root->setSpacing(4);

    // This field only filters the installed sticker packs.
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search sticker packs…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setToolTip(tr("Search installed sticker packs"));
    m_searchEdit->setMinimumHeight(32);
    root->addWidget(m_searchEdit);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    // Keep the side scrollbar visible when the sticker list is taller than the picker.
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_gridWidget = new QWidget(m_scroll);
    m_grid = new QGridLayout(m_gridWidget);
    m_grid->setContentsMargins(2, 2, 2, 2);
    m_grid->setSpacing(6);
    m_scroll->setWidget(m_gridWidget);
    root->addWidget(m_scroll, 1);

    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    m_message->setVisible(false);
    root->addWidget(m_message);

    // Pack icons act as category shortcuts, with the add button kept at the right edge.
    auto *categoryRow = new QHBoxLayout;
    categoryRow->setContentsMargins(0, 0, 0, 0);
    categoryRow->setSpacing(4);
    m_categoryScroll = new QScrollArea(this);
    m_categoryScroll->setFrameShape(QFrame::NoFrame);
    m_categoryScroll->setWidgetResizable(true);
    m_categoryScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_categoryScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_categoryScroll->setFixedHeight(52);
    m_categoryContents = new QWidget(m_categoryScroll);
    m_categoryLayout = new QHBoxLayout(m_categoryContents);
    m_categoryLayout->setContentsMargins(2, 2, 2, 2);
    m_categoryLayout->setSpacing(4);
    m_categoryLayout->addStretch(1);
    m_categoryScroll->setWidget(m_categoryContents);
    categoryRow->addWidget(m_categoryScroll, 1);

    m_addButton = new QToolButton(this);
    m_addButton->setText(QStringLiteral("+"));
    m_addButton->setToolTip(tr("Add sticker pack"));
    m_addButton->setAccessibleName(tr("Add sticker pack"));
    m_addButton->setFixedSize(32, 32);
    QFont addFont = m_addButton->font();
    addFont.setBold(true);
    addFont.setPointSize(addFont.pointSize() + 3);
    m_addButton->setFont(addFont);
    categoryRow->addWidget(m_addButton, 0, Qt::AlignVCenter);
    root->addLayout(categoryRow);

    connect(m_addButton, &QToolButton::clicked, this, &StickerPicker::addPack);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &) {
        setMessage(QString());
        m_scroll->verticalScrollBar()->setValue(0);
        // Keep the sticker list and bottom category shortcuts in sync with the search.
        showPacks();
    });
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        loadVisibleStickers();
        updateCategorySelection();
    });
    if (m_cli) refresh();
    else setMessage(tr("Connect to Signal to load sticker packs."), true);
}

void StickerPicker::setSignalCli(SignalCli *cli)
{
    if (m_cli == cli) return;
    m_cli = cli;
    ++m_cliGeneration;
    m_hasLoadedPacks = false;
    m_refreshInFlight = false;
    m_packList.clear();
    m_visiblePacks.clear();
    m_imageCache.clear();
    m_imagePending.clear();
    m_imageFailed.clear();
    m_sendWhenLoaded.clear();
    m_imageLoadsInFlight = 0;
    showPacks();
    refresh();
}

bool StickerPicker::isValidPackUrl(const QString &raw)
{
    const QUrl url(raw.trimmed());
    const QString scheme = url.scheme().toLower();
    const bool allowedHost = url.host().compare(QStringLiteral("signal.art"), Qt::CaseInsensitive) == 0;
    const bool allowedPath = url.path().startsWith(QStringLiteral("/addstickers"), Qt::CaseInsensitive);
    const bool signalScheme = scheme == QLatin1String("sgnl") &&
        (url.host().compare(QStringLiteral("addstickers"), Qt::CaseInsensitive) == 0 ||
         url.path().contains(QStringLiteral("addstickers"), Qt::CaseInsensitive));
    if (!((scheme == QLatin1String("https") && allowedHost && allowedPath) || signalScheme))
        return false;
    QUrlQuery query(url);
    QString packId = query.queryItemValue(QStringLiteral("pack_id"));
    QString packKey = query.queryItemValue(QStringLiteral("pack_key"));
    if (packId.isEmpty() || packKey.isEmpty()) {
        const QString fragment = url.fragment(QUrl::FullyDecoded);
        const QUrlQuery fragmentQuery(fragment);
        if (packId.isEmpty()) packId = fragmentQuery.queryItemValue(QStringLiteral("pack_id"));
        if (packKey.isEmpty()) packKey = fragmentQuery.queryItemValue(QStringLiteral("pack_key"));
    }
    static const QRegularExpression hexId(QStringLiteral("^[a-fA-F0-9]{32}$"));
    static const QRegularExpression hexKey(QStringLiteral("^[a-fA-F0-9]{64}$"));
    return hexId.match(packId).hasMatch() && hexKey.match(packKey).hasMatch();
}

void StickerPicker::refresh()
{
    if (!m_cli || !m_cli->isRunning()) {
        if (!m_hasLoadedPacks)
            setMessage(tr("Connect to Signal to load sticker packs."), true);
        return;
    }
    // The installed-pack list changes only when the user installs a pack or
    // switches Signal accounts. Reopening the popup should use the in-memory
    // copy instead of issuing another RPC and rebuilding every sticker widget.
    if (m_hasLoadedPacks || m_refreshInFlight) return;

    m_refreshInFlight = true;
    const quint64 generation = m_cliGeneration;
    setMessage(tr("Loading installed sticker packs…"));
    m_cli->call(QStringLiteral("listStickerPacks"), {}, [this, generation](const QJsonValue &value, const QString &error) {
        if (generation != m_cliGeneration) return;
        m_refreshInFlight = false;
        if (!error.isEmpty()) {
            setMessage(tr("Could not load sticker packs: %1").arg(error), true);
            return;
        }
        m_hasLoadedPacks = true;
        parsePacks(value);
    });
}

void StickerPicker::parsePacks(const QJsonValue &value)
{
    m_packList.clear();
    QJsonArray packs;
    if (value.isArray()) packs = value.toArray();
    else if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("packs")).isArray()) packs = object.value(QStringLiteral("packs")).toArray();
        else if (object.value(QStringLiteral("stickerPacks")).isArray()) packs = object.value(QStringLiteral("stickerPacks")).toArray();
        else {
            for (auto it = object.begin(); it != object.end(); ++it)
                if (it.value().isObject()) packs.append(it.value());
        }
    }

    for (const QJsonValue &v : packs) {
        QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("pack")).isObject()) o = o.value(QStringLiteral("pack")).toObject();
        Pack pack;
        pack.id = strValue(o, {QStringLiteral("packId"), QStringLiteral("packID"), QStringLiteral("pack_id"), QStringLiteral("id")});
        if (pack.id.isEmpty()) continue;
        pack.title = strValue(o, {QStringLiteral("title"), QStringLiteral("name")});
        pack.author = strValue(o, {QStringLiteral("author"), QStringLiteral("creator")});
        QJsonArray stickers = o.value(QStringLiteral("stickers")).toArray();
        if (stickers.isEmpty() && o.value(QStringLiteral("manifest")).isObject())
            stickers = o.value(QStringLiteral("manifest")).toObject().value(QStringLiteral("stickers")).toArray();
        for (int i = 0; i < stickers.size(); ++i) {
            const QJsonObject so = stickers.at(i).toObject();
            Sticker sticker;
            sticker.id = intValue(so, {QStringLiteral("stickerId"), QStringLiteral("sticker_id"), QStringLiteral("id")}, i);
            sticker.emoji = strValue(so, {QStringLiteral("emoji"), QStringLiteral("emojiString"), QStringLiteral("emoji_string")});
            if (sticker.id >= 0) pack.stickers.append(sticker);
        }
        if (pack.title.isEmpty()) pack.title = tr("Sticker pack");
        m_packList.append(pack);
    }
    std::sort(m_packList.begin(), m_packList.end(), [](const Pack &a, const Pack &b) {
        return a.title.toCaseFolded() < b.title.toCaseFolded();
    });

    setMessage(QString());
    showPacks();
}

// Every installed pack is shown as a section in one continuous scroll view.
// This avoids a pack-selection dropdown and lets users move between packs by scrolling.
void StickerPicker::showPacks()
{
    const quint64 generation = ++m_renderGeneration;
    while (QLayoutItem *item = m_grid->takeAt(0)) {
        if (QWidget *widget = item->widget()) widget->deleteLater();
        delete item;
    }
    while (m_categoryLayout->count()) {
        QLayoutItem *item = m_categoryLayout->takeAt(0);
        if (QWidget *widget = item->widget()) widget->deleteLater();
        delete item;
    }
    m_stickerButtons.clear();
    m_packHeadings.clear();
    m_categoryButtons.clear();
    m_visiblePacks.clear();

    const QString query = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    for (const Pack &pack : m_packList) {
        bool matches = query.isEmpty() || pack.title.contains(query, Qt::CaseInsensitive) ||
                       pack.author.contains(query, Qt::CaseInsensitive);
        if (!matches) {
            for (const Sticker &sticker : pack.stickers) {
                if (sticker.emoji.contains(query, Qt::CaseInsensitive)) {
                    matches = true;
                    break;
                }
            }
        }
        if (matches) m_visiblePacks.append(pack);
    }

    if (m_visiblePacks.isEmpty()) {
        auto *empty = new QLabel(m_gridWidget);
        empty->setWordWrap(true);
        empty->setContentsMargins(6, 8, 6, 8);
        if (m_packList.isEmpty())
            empty->setText(tr("No sticker packs installed."));
        else
            empty->setText(tr("No sticker packs match your search."));
        m_grid->addWidget(empty, 0, 0, 1, 4);
        m_categoryLayout->addStretch(1);
        m_categoryContents->adjustSize();
        setMessage(QString());
        return;
    }

    // Creating thousands of QPushButtons in one event-loop turn makes the
    // popup appear frozen for a moment. Add one small batch at a time so Qt
    // can paint and handle input while large sticker collections are built.
    m_renderPackIndex = 0;
    m_renderStickerIndex = 0;
    m_renderRow = 0;
    setMessage(QString());
    QTimer::singleShot(0, this, [this, generation] { appendStickerBatch(generation); });
}

void StickerPicker::appendStickerBatch(quint64 generation)
{
    if (generation != m_renderGeneration) return;

    if (m_renderPackIndex >= m_visiblePacks.size()) {
        m_categoryLayout->addStretch(1);
        for (int column = 0; column < 4; ++column) m_grid->setColumnStretch(column, 1);
        m_categoryContents->adjustSize();
        QTimer::singleShot(0, this, [this, generation] {
            if (generation != m_renderGeneration) return;
            loadVisibleStickers();
            updateCategorySelection();
        });
        return;
    }

    const Pack &pack = m_visiblePacks.at(m_renderPackIndex);
    const bool firstBatch = m_renderStickerIndex == 0;
    if (firstBatch) {
        auto *heading = new QLabel(m_gridWidget);
        QString headingText = pack.title;
        if (!pack.author.isEmpty()) headingText += QStringLiteral(" · ") + pack.author;
        heading->setText(headingText);
        QFont headingFont = heading->font();
        headingFont.setBold(true);
        heading->setFont(headingFont);
        heading->setWordWrap(true);
        heading->setContentsMargins(4, 8, 4, 2);
        m_grid->addWidget(heading, m_renderRow++, 0, 1, 4);
        m_packHeadings.insert(pack.id, heading);

        auto *category = new QToolButton(m_categoryContents);
        category->setFixedSize(32, 32);
        category->setIconSize(QSize(24, 24));
        category->setAutoRaise(true);
        category->setCheckable(true);
        category->setToolButtonStyle(Qt::ToolButtonIconOnly);
        category->setToolTip(headingText);
        category->setAccessibleName(headingText);
        category->setProperty("stickerPackId", pack.id);
        category->setStyleSheet(QStringLiteral(
            "QToolButton { border: 1px solid transparent; border-radius: 7px; background: transparent; padding: 1px; }"
            "QToolButton:checked { border-color: palette(highlight); background: transparent; }"));

        const Sticker *firstSticker = pack.stickers.isEmpty() ? nullptr : &pack.stickers.first();
        if (firstSticker) {
            const QByteArray cached = m_imageCache.value(cacheKey(pack.id, firstSticker->id));
            const QImage image = QImage::fromData(cached);
            if (!image.isNull()) {
                category->setIcon(QPixmap::fromImage(image));
            } else if (!firstSticker->emoji.isEmpty()) {
                category->setText(firstSticker->emoji);
                category->setToolButtonStyle(Qt::ToolButtonTextOnly);
            } else {
                category->setText(pack.title.left(1).toUpper());
                category->setToolButtonStyle(Qt::ToolButtonTextOnly);
            }
        } else {
            category->setText(pack.title.left(1).toUpper());
            category->setToolButtonStyle(Qt::ToolButtonTextOnly);
        }
        m_categoryLayout->addWidget(category);
        m_categoryButtons.insert(pack.id, category);
        connect(category, &QToolButton::clicked, this, [this, packId = pack.id] {
            const QPointer<QLabel> packHeading = m_packHeadings.value(packId);
            if (!packHeading || !m_scroll || !m_gridWidget) return;
            const int targetY = packHeading->mapTo(m_gridWidget, QPoint(0, 0)).y();
            QScrollBar *bar = m_scroll->verticalScrollBar();
            bar->setValue(qBound(bar->minimum(), targetY, bar->maximum()));
            updateCategorySelection();
        });

        if (pack.stickers.isEmpty()) {
            auto *empty = new QLabel(tr("No stickers available for this pack."), m_gridWidget);
            empty->setContentsMargins(4, 0, 4, 6);
            m_grid->addWidget(empty, m_renderRow++, 0, 1, 4);
            ++m_renderPackIndex;
            m_renderStickerIndex = 0;
            QTimer::singleShot(0, this, [this, generation] { appendStickerBatch(generation); });
            return;
        }
    }

    constexpr int kStickerWidgetsPerBatch = 24;
    const int count = pack.stickers.size();
    const int first = m_renderStickerIndex;
    const int last = qMin(first + kStickerWidgetsPerBatch, count);
    for (int i = first; i < last; ++i) {
        const Sticker sticker = pack.stickers.at(i);
        auto *button = new QPushButton(m_gridWidget);
        button->setFixedSize(78, 78);
        button->setIconSize(QSize(56, 56));
        button->setFlat(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(sticker.emoji.isEmpty() ? pack.title : sticker.emoji);
        if (!sticker.emoji.isEmpty()) button->setText(sticker.emoji);
        button->setProperty("stickerPackId", pack.id);
        button->setProperty("stickerId", sticker.id);
        const int buttonRow = m_renderRow + i / 4;
        m_grid->addWidget(button, buttonRow, i % 4);

        const QString key = cacheKey(pack.id, sticker.id);
        m_stickerButtons.insert(key, button);
        // Sticker animations are initialized lazily by loadVisibleStickers
        // only when this button approaches the viewport.
        connect(button, &QPushButton::clicked, this, [this, packId = pack.id, stickerId = sticker.id] {
            const QString key = cacheKey(packId, stickerId);
            const QByteArray data = m_imageCache.value(key);
            if (!data.isEmpty()) {
                emit stickerChosen(packId, stickerId, data);
            } else {
                m_imageFailed.remove(key); // an explicit click may retry a failed preview
                m_sendWhenLoaded.insert(key);
                setMessage(tr("Loading sticker…"));
                loadSticker(packId, stickerId);
            }
        });
    }

    m_renderStickerIndex = last;
    if (last >= count) {
        m_renderRow += (count + 3) / 4;
        auto *spacer = new QWidget(m_gridWidget);
        spacer->setFixedHeight(8);
        m_grid->addWidget(spacer, m_renderRow++, 0, 1, 4);
        ++m_renderPackIndex;
        m_renderStickerIndex = 0;
    }

    QTimer::singleShot(0, this, [this, generation] { appendStickerBatch(generation); });
}

void StickerPicker::updateCategorySelection()
{
    if (!m_scroll || m_packHeadings.isEmpty()) return;

    QString activeId;
    QString nearestId;
    int activeY = std::numeric_limits<int>::min();
    int nearestY = std::numeric_limits<int>::max();
    QWidget *viewport = m_scroll->viewport();
    for (auto it = m_packHeadings.cbegin(); it != m_packHeadings.cend(); ++it) {
        if (!it.value()) continue;
        // Work in viewport coordinates so selection follows what is actually visible,
        // independent of the scroll area's internal content offset.
        const int y = it.value()->mapTo(viewport, QPoint(0, 0)).y();
        if (y < nearestY) {
            nearestY = y;
            nearestId = it.key();
        }
        if (y <= 16 && y > activeY) {
            activeY = y;
            activeId = it.key();
        }
    }
    if (activeId.isEmpty()) activeId = nearestId;
    for (auto it = m_categoryButtons.begin(); it != m_categoryButtons.end(); ++it) {
        if (it.value()) it.value()->setChecked(it.key() == activeId);
    }
}

void StickerPicker::loadVisibleStickers()
{
    // Do not fetch or decode sticker images while the popup is closed.
    if (!isVisible() || !m_scroll || !m_gridWidget) return;
    constexpr int kMaxConcurrentStickerLoads = 2;
    const int top = m_scroll->verticalScrollBar()->value();
    const int height = m_scroll->viewport()->height();
    const int bottom = top + height;
    const int preloadMargin = height / 2;

    for (auto it = m_stickerButtons.cbegin(); it != m_stickerButtons.cend(); ++it) {
        QPushButton *button = it.value();
        if (!button) continue;
        const int buttonTop = button->mapTo(m_gridWidget, QPoint(0, 0)).y();
        const int buttonBottom = buttonTop + button->height();
        if (buttonBottom < top - preloadMargin || buttonTop > bottom + preloadMargin) continue;

        const QString key = it.key();
        if (m_imageCache.contains(key)) {
            // Cached animations are started only when their buttons are near
            // the viewport. Rebuilding/searching the list must not instantiate
            // hundreds of QMovie/APNG players for off-screen stickers.
            if (!button->property("stickerAnimationStarted").toBool()) {
                applyStickerImage(button, m_imageCache.value(key));
                button->setProperty("stickerAnimationStarted", true);
            }
            continue;
        }
        if (m_imagePending.value(key) || m_imageFailed.contains(key) ||
            m_imageLoadsInFlight >= kMaxConcurrentStickerLoads) continue;

        const QString packId = button->property("stickerPackId").toString();
        bool ok = false;
        const int stickerId = button->property("stickerId").toInt(&ok);
        if (ok) loadSticker(packId, stickerId, button);
    }
}

void StickerPicker::applyStickerImage(QPushButton *button, const QByteArray &bytes)
{
    if (!button || bytes.isEmpty()) return;
    // Show the first frame immediately, then play the animation on top of it.
    const QImage firstFrame = QImage::fromData(bytes);
    if (!firstFrame.isNull()) button->setIcon(QPixmap::fromImage(firstFrame));

    // Signal's animated stickers are APNG, which QMovie cannot play (Qt ships
    // no APNG image plugin) — drive those frames with ApngMovie instead.
    if (ApngMovie *apng = ApngMovie::fromData(bytes, button)) {
        connect(apng, &ApngMovie::updated, button, [button, apng] {
            button->setIcon(apng->currentPixmap());
        });
        apng->start();
        button->setIcon(apng->currentPixmap());
        return;
    }

    // Animated WebP or GIF stickers still go through QMovie, like the GIF tab.
    // A single-frame sticker just stays on its first frame.
    auto *movie = new QMovie(button);
    auto *buffer = new QBuffer(movie);
    buffer->setData(bytes);
    buffer->open(QIODevice::ReadOnly);
    movie->setDevice(buffer);
    movie->setCacheMode(QMovie::CacheNone);
    movie->setScaledSize(button->iconSize());
    if (movie->isValid()) {
        connect(movie, &QMovie::frameChanged, button, [button, movie] {
            button->setIcon(QIcon(movie->currentPixmap()));
        });
        movie->start();
    } else {
        delete movie; // no WebP/GIF plugin: the static first frame stays
    }
}

void StickerPicker::loadSticker(const QString &packId, int stickerId, QPointer<QPushButton> button)
{
    const QString key = cacheKey(packId, stickerId);
    if (m_imageCache.contains(key) || m_imagePending.value(key)) return;
    if (!m_cli || !m_cli->isRunning()) return;

    m_imagePending.insert(key, true);
    ++m_imageLoadsInFlight;
    const quint64 generation = m_cliGeneration;
    m_cli->call(QStringLiteral("getSticker"),
                QJsonObject{{QStringLiteral("packId"), packId}, {QStringLiteral("stickerId"), stickerId}},
                [this, generation, key, button, packId, stickerId](const QJsonValue &value, const QString &error) {
        if (generation != m_cliGeneration) return;
        m_imagePending.remove(key);
        m_imageLoadsInFlight = qMax(0, m_imageLoadsInFlight - 1);
        if (!error.isEmpty()) {
            m_imageFailed.insert(key);
            m_sendWhenLoaded.remove(key);
            loadVisibleStickers();
            return;
        }
        const QJsonObject object = value.toObject();
        QString encoded = object.value(QStringLiteral("data")).toString();
        if (encoded.isEmpty()) encoded = object.value(QStringLiteral("base64")).toString();
        if (encoded.isEmpty() && value.isString()) encoded = value.toString();
        const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
        if (bytes.isEmpty()) {
            m_imageFailed.insert(key);
            m_sendWhenLoaded.remove(key);
            loadVisibleStickers();
            return;
        }
        m_imageCache.insert(key, bytes);

        // The request may finish after the user has scrolled away. Keep the
        // bytes in cache, but only decode/start animation for a visible button.
        if (button && isVisible() && m_scroll && m_gridWidget) {
            const int top = m_scroll->verticalScrollBar()->value();
            const int height = m_scroll->viewport()->height();
            const int y = button->mapTo(m_gridWidget, QPoint(0, 0)).y();
            const int margin = height / 2;
            if (y + button->height() >= top - margin && y <= top + height + margin) {
                applyStickerImage(button, bytes);
                button->setProperty("stickerAnimationStarted", true);
            }
        }

        const QPointer<QToolButton> category = m_categoryButtons.value(packId);
        const auto packIt = std::find_if(m_packList.cbegin(), m_packList.cend(), [packId](const Pack &candidate) {
            return candidate.id == packId;
        });
        if (category && packIt != m_packList.cend() && !packIt->stickers.isEmpty() &&
            packIt->stickers.first().id == stickerId) {
            const QImage image = QImage::fromData(bytes);
            if (!image.isNull()) {
                category->setIcon(QPixmap::fromImage(image));
                category->setText(QString());
                category->setToolButtonStyle(Qt::ToolButtonIconOnly);
            }
        }
        if (m_sendWhenLoaded.remove(key)) {
            emit stickerChosen(packId, stickerId, bytes);
        }
        loadVisibleStickers();
    });
}

// ---------------------------------------------------------------- visibility

// No point in decoding animations nobody can see (same as the GIF tab).
void StickerPicker::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    setMoviesPaused(false);
    loadVisibleStickers();
}

void StickerPicker::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    setMoviesPaused(true);
}

void StickerPicker::setMoviesPaused(bool paused)
{
    const auto movies = findChildren<QMovie *>();
    for (QMovie *movie : movies) movie->setPaused(paused);
    const auto apngs = findChildren<ApngMovie *>();
    for (ApngMovie *apng : apngs) apng->setPaused(paused);
}

void StickerPicker::addPack()
{
    bool accepted = false;
    const QString uri = QInputDialog::getText(this, tr("Add sticker pack"),
                                               tr("Paste a Signal sticker-pack link:"),
                                               QLineEdit::Normal, QString(), &accepted).trimmed();
    if (accepted && !uri.isEmpty()) installPack(uri);
}

void StickerPicker::installPack(const QString &uri)
{
    if (!isValidPackUrl(uri)) {
        setMessage(tr("Enter a valid Signal sticker-pack link."), true);
        return;
    }
    if (!m_cli || !m_cli->isRunning()) {
        setMessage(tr("Connect to Signal before installing a sticker pack."), true);
        return;
    }
    m_addButton->setEnabled(false);
    setMessage(tr("Adding sticker pack…"));
    const quint64 generation = m_cliGeneration;
    m_cli->call(QStringLiteral("addStickerPack"), QJsonObject{{QStringLiteral("uri"), uri}},
                [this, generation](const QJsonValue &, const QString &error) {
        if (generation != m_cliGeneration) return;
        m_addButton->setEnabled(true);
        if (!error.isEmpty()) {
            setMessage(tr("Could not add sticker pack: %1").arg(error), true);
            return;
        }
        m_hasLoadedPacks = false;
        refresh();
    });
}

void StickerPicker::setMessage(const QString &text, bool error)
{
    m_message->setText(text);
    m_message->setVisible(!text.isEmpty());
    m_message->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
}
