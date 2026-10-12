#pragma once

#include <QDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

class QLabel;
class QGridLayout;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;

class GiphyPicker : public QDialog
{
    Q_OBJECT
public:
    explicit GiphyPicker(const QString &apiKey, QWidget *parent = nullptr, const QUrl &apiBase = {});

signals:
    void gifSelected(const QUrl &originalUrl, const QUrl &sentAnalyticsUrl);

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
    void setStatus(const QString &text);
    void track(const QUrl &url);
    bool isAllowedUrl(const QUrl &url) const;

    QString m_apiKey;
    QUrl m_apiBase;
    QNetworkAccessManager m_network;
    QNetworkReply *m_searchReply = nullptr;
    QList<QNetworkReply *> m_previewReplies;
    QTimer m_debounce;
    QLineEdit *m_search = nullptr;
    QLabel *m_status = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_results = nullptr;
    QGridLayout *m_grid = nullptr;
    QVBoxLayout *m_resultsLayout = nullptr;
    QList<GifResult> m_gifs;
    quint64 m_searchGeneration = 0;
};
