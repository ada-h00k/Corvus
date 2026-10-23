#include "GiphyPicker.h"

#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkReply>
#include <QMovie>
#include <QPushButton>
#include <QScrollArea>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {
constexpr int kGifCount = 18;
constexpr int kColumns = 3;
constexpr int kCellWidth = 102;
constexpr int kCellHeight = 96;

QUrl analyticsUrl(const QJsonObject &gif, const QString &event)
{
    const QUrl url = gif.value(QStringLiteral("analytics")).toObject()
                         .value(event).toObject().value(QStringLiteral("url")).toString();
    return url.scheme() == QLatin1String("https") ? url : QUrl();
}
} // namespace

GiphyPicker::GiphyPicker(const QString &apiKey, QWidget *parent, const QUrl &apiBase)
    : QWidget(parent),
      m_apiBase(apiBase.isEmpty() ? QUrl(QStringLiteral("https://api.giphy.com")) : apiBase)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_status = new QLabel(tr("Trending GIFs from GIPHY"), this);
    m_status->setWordWrap(true);
    m_status->setForegroundRole(QPalette::PlaceholderText);
    m_status->setContentsMargins(2, 0, 2, 0);
    layout->addWidget(m_status);

    // Shown instead of the results while there is no API key.
    m_keyBox = new QWidget(this);
    auto *keyLayout = new QHBoxLayout(m_keyBox);
    keyLayout->setContentsMargins(0, 4, 0, 0);
    auto *configure = new QPushButton(tr("Configure API key …"), m_keyBox);
    auto *getKey = new QPushButton(tr("Get an API key"), m_keyBox);
    keyLayout->addWidget(configure);
    keyLayout->addWidget(getKey);
    keyLayout->addStretch(1);
    layout->addWidget(m_keyBox);
    connect(configure, &QPushButton::clicked, this, &GiphyPicker::configureRequested);
    connect(getKey, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://developers.giphy.com/dashboard/")));
    });

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_results = new QWidget(m_scroll);
    m_resultsLayout = new QVBoxLayout(m_results);
    m_resultsLayout->setContentsMargins(0, 0, 0, 0);
    m_grid = new QGridLayout;
    m_grid->setContentsMargins(2, 2, 2, 2);
    m_grid->setSpacing(6);
    m_resultsLayout->addLayout(m_grid);
    m_resultsLayout->addStretch(1);
    m_scroll->setWidget(m_results);
    layout->addWidget(m_scroll, 1);

    auto *attribution = new QLabel(QStringLiteral("<a href=\"https://giphy.com\">Powered by GIPHY</a>"), this);
    attribution->setOpenExternalLinks(true);
    attribution->setAlignment(Qt::AlignRight);
    layout->addWidget(attribution);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(350);
    connect(&m_debounce, &QTimer::timeout, this, [this] { search(m_query.trimmed()); });
    connect(&m_network, &QNetworkAccessManager::finished, this, [this](QNetworkReply *reply) {
        if (reply == m_searchReply) {
            m_searchReply = nullptr;
            const quint64 generation = reply->property("generation").toULongLong();
            if (generation != m_searchGeneration) {
                reply->deleteLater();
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                setStatus(tr("GIPHY search failed: %1").arg(reply->errorString()));
            } else {
                showResults(reply->readAll());
            }
            reply->deleteLater();
            return;
        }

        const int index = reply->property("gifIndex").toInt();
        const quint64 generation = reply->property("generation").toULongLong();
        m_previewReplies.removeAll(reply);
        if (reply->error() == QNetworkReply::NoError && generation == m_searchGeneration &&
            index >= 0 && index < m_gifs.size()) {
            const QByteArray previewData = reply->readAll();
            auto *button = m_results->findChild<QPushButton *>(QStringLiteral("giphyResult%1").arg(index));
            if (button) {
                auto *movie = new QMovie(button);
                auto *buffer = new QBuffer(movie);
                buffer->setData(previewData);
                buffer->open(QIODevice::ReadOnly);
                movie->setDevice(buffer);
                movie->setCacheMode(QMovie::CacheAll);
                movie->setScaledSize(button->iconSize());
                if (movie->isValid()) {
                    connect(movie, &QMovie::frameChanged, button, [button, movie] {
                        button->setIcon(QIcon(movie->currentPixmap()));
                    });
                    movie->start();
                } else {
                    const QPixmap preview = QPixmap::fromImage(QImage::fromData(previewData));
                    if (!preview.isNull()) button->setIcon(QIcon(preview));
                }
                button->setEnabled(true);
                const QUrl analytics = m_gifs[index].loadedAnalyticsUrl;
                if (!analytics.isEmpty()) track(analytics);
            }
        }
        reply->deleteLater();
    });
    setApiKey(apiKey);
}

void GiphyPicker::setApiKey(const QString &apiKey, const QString &error)
{
    const bool changed = apiKey != m_apiKey;
    m_apiKey = apiKey;
    const bool has = !m_apiKey.isEmpty();
    m_keyBox->setVisible(!has);
    m_scroll->setVisible(has);
    if (!has) {
        abortRequests();
        clearResults();
        setStatus(error.isEmpty() ? tr("GIPHY requires an API key to search and send GIFs.") : error);
        return;
    }
    if (changed || (m_gifs.isEmpty() && !m_searchReply)) search(m_query.trimmed());
}

void GiphyPicker::setApiBase(const QUrl &apiBase)
{
    if (!apiBase.isEmpty()) m_apiBase = apiBase;
}

void GiphyPicker::setQuery(const QString &query)
{
    if (query == m_query) return;
    m_query = query;
    if (hasApiKey()) m_debounce.start();
}

void GiphyPicker::searchNow()
{
    m_debounce.stop();
    search(m_query.trimmed());
}

void GiphyPicker::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    setMoviesPaused(false);
}

void GiphyPicker::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // No point in decoding animations nobody can see.
    setMoviesPaused(true);
}

void GiphyPicker::setMoviesPaused(bool paused)
{
    const auto movies = findChildren<QMovie *>();
    for (QMovie *movie : movies) movie->setPaused(paused);
}

void GiphyPicker::abortRequests()
{
    ++m_searchGeneration;
    if (m_searchReply) {
        m_searchReply->abort();
        m_searchReply = nullptr;
    }
    for (QNetworkReply *reply : std::as_const(m_previewReplies)) reply->abort();
    m_previewReplies.clear();
}

void GiphyPicker::search(const QString &query)
{
    if (m_apiKey.isEmpty()) return;
    abortRequests();
    clearResults();
    setStatus(query.isEmpty() ? tr("Loading trending GIFs …") : tr("Searching GIPHY …"));

    const QString path = query.isEmpty() ? QStringLiteral("/v1/gifs/trending") : QStringLiteral("/v1/gifs/search");
    QUrl url = m_apiBase;
    url.setPath(path);
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("api_key"), m_apiKey);
    if (!query.isEmpty()) params.addQueryItem(QStringLiteral("q"), query.left(50));
    params.addQueryItem(QStringLiteral("limit"), QString::number(kGifCount));
    params.addQueryItem(QStringLiteral("bundle"), QStringLiteral("messaging_non_clips"));
    params.addQueryItem(QStringLiteral("lang"), QStringLiteral("en"));
    url.setQuery(params);

    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "corvus");
    m_searchReply = m_network.get(request);
    m_searchReply->setProperty("generation", QVariant::fromValue(m_searchGeneration));
}

void GiphyPicker::showResults(const QByteArray &json)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonArray data = root.value(QStringLiteral("data")).toArray();
    for (const QJsonValue &value : data) {
        const QJsonObject gif = value.toObject();
        const QJsonObject images = gif.value(QStringLiteral("images")).toObject();
        QUrl preview = images.value(QStringLiteral("fixed_width_small")).toObject()
                           .value(QStringLiteral("url")).toString();
        if (preview.isEmpty())
            preview = images.value(QStringLiteral("fixed_width")).toObject().value(QStringLiteral("url")).toString();
        QUrl original = images.value(QStringLiteral("original")).toObject()
                            .value(QStringLiteral("url")).toString();
        if (!isAllowedUrl(preview) || !isAllowedUrl(original)) continue;

        GifResult result;
        result.previewUrl = preview;
        result.originalUrl = original;
        result.loadedAnalyticsUrl = analyticsUrl(gif, QStringLiteral("onload"));
        result.sentAnalyticsUrl = analyticsUrl(gif, QStringLiteral("onsent"));
        result.title = gif.value(QStringLiteral("title")).toString();
        m_gifs.append(result);
    }

    if (m_gifs.isEmpty()) {
        setStatus(tr("No GIFs found."));
        return;
    }
    setStatus(tr("Select a GIF to send."));
    for (int index = 0; index < m_gifs.size(); ++index) {
        auto *button = new QPushButton(m_results);
        button->setObjectName(QStringLiteral("giphyResult%1").arg(index));
        button->setAccessibleName(m_gifs[index].title.isEmpty() ? tr("GIF result %1").arg(index + 1) : m_gifs[index].title);
        button->setToolTip(m_gifs[index].title);
        button->setFixedSize(kCellWidth, kCellHeight);
        button->setIconSize(QSize(kCellWidth - 8, kCellHeight - 8));
        button->setFlat(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setEnabled(false);
        connect(button, &QPushButton::clicked, this, [this, index] {
            if (index < 0 || index >= m_gifs.size()) return;
            emit gifSelected(m_gifs[index].originalUrl, m_gifs[index].sentAnalyticsUrl);
        });
        m_grid->addWidget(button, index / kColumns, index % kColumns);
        loadPreview(index);
    }
}

void GiphyPicker::loadPreview(int index)
{
    if (index < 0 || index >= m_gifs.size()) return;
    QNetworkRequest request(m_gifs[index].previewUrl);
    request.setRawHeader("User-Agent", "corvus");
    QNetworkReply *reply = m_network.get(request);
    reply->setProperty("gifIndex", index);
    reply->setProperty("generation", QVariant::fromValue(m_searchGeneration));
    m_previewReplies.append(reply);
}

void GiphyPicker::clearResults()
{
    m_gifs.clear();
    while (QLayoutItem *item = m_grid->takeAt(0)) {
        if (QWidget *widget = item->widget()) widget->deleteLater();
        delete item;
    }
}

void GiphyPicker::setStatus(const QString &text)
{
    m_status->setText(text);
}

void GiphyPicker::track(const QUrl &url)
{
    if (url.isEmpty() || !isAllowedUrl(url)) return;
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "corvus");
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

bool GiphyPicker::isAllowedUrl(const QUrl &url) const
{
    if (url.scheme() == QLatin1String("https")) return true;
    const QString host = m_apiBase.host();
    return m_apiBase.scheme() == QLatin1String("http") &&
           (host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1")) &&
           url.scheme() == QLatin1String("http") && url.host() == host && url.port() == m_apiBase.port();
}
