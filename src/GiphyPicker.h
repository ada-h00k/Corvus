#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QWidget>

class QLabel;
class QGridLayout;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

// GIF results from GIPHY (trending or a search) as a panel that is embedded in
// the emoji popup. It has no search field of its own: the owner feeds it the
// query via setQuery().
class GiphyPicker : public QWidget
{
    Q_OBJECT
public:
    explicit GiphyPicker(const QString &apiKey = {}, QWidget *parent = nullptr, const QUrl &apiBase = {});

    // Without a key the panel explains how to get one and offers to configure it.
    // `error` replaces the explanation, e.g. when the key store could not be read.
    void setApiKey(const QString &apiKey, const QString &error = {});
    bool hasApiKey() const { return !m_apiKey.isEmpty(); }
    void setApiBase(const QUrl &apiBase);

    // Search for `query` shortly after the last change (empty: trending GIFs).
    void setQuery(const QString &query);
    // Search right away, e.g. when Enter was pressed.
    void searchNow();

signals:
    void gifSelected(const QUrl &originalUrl, const QUrl &sentAnalyticsUrl);
    void configureRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct GifResult {
        QUrl previewUrl;
        QUrl originalUrl;
        QUrl loadedAnalyticsUrl;
        QUrl sentAnalyticsUrl;
        QString title;
    };

    void search(const QString &query);
    void showResults(const QByteArray &json);
    void loadPreview(int index);
    void clearResults();
    void abortRequests();
    void setStatus(const QString &text);
    void setMoviesPaused(bool paused);
    void track(const QUrl &url);
    bool isAllowedUrl(const QUrl &url) const;

    QString m_apiKey;
    QUrl m_apiBase;
    QString m_query;
    QNetworkAccessManager m_network;
    QNetworkReply *m_searchReply = nullptr;
    QList<QNetworkReply *> m_previewReplies;
    QTimer m_debounce;
    QLabel *m_status = nullptr;
    QWidget *m_keyBox = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_results = nullptr;
    QGridLayout *m_grid = nullptr;
    QVBoxLayout *m_resultsLayout = nullptr;
    QList<GifResult> m_gifs;
    quint64 m_searchGeneration = 0;
};
