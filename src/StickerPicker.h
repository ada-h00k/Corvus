#pragma once

#include <QByteArray>
#include <QWidget>
#include <QHash>
#include <QList>
#include <QJsonValue>
#include <QStringList>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QString>

class SignalCli;
class QGridLayout;
class QHBoxLayout;
class QHideEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QToolButton;
class QWidget;

// A small Signal sticker-pack browser. Stickers are loaded through signal-cli,
// so selecting one sends a real Signal sticker rather than a regular image upload.
class StickerPicker : public QWidget
{
    Q_OBJECT
public:
    explicit StickerPicker(SignalCli *cli, QWidget *parent = nullptr);
    void refresh();
    void setSignalCli(SignalCli *cli);

    static bool isValidPackUrl(const QString &raw);

signals:
    void stickerChosen(const QString &packId, int stickerId, const QByteArray &imageData);

private:
    struct Sticker {
        int id = -1;
        QString emoji;
    };
    struct Pack {
        QString id;
        QString title;
        QString author;
        QList<Sticker> stickers;
    };

    void parsePacks(const QJsonValue &value);
    void showPacks();
    void appendStickerBatch(quint64 generation);
    void loadVisibleStickers();
    void addPack();
    void installPack(const QString &uri);
    void updateCategorySelection();
    void loadSticker(const QString &packId, int stickerId, QPointer<QPushButton> button = {});
    void applyStickerImage(QPushButton *button, const QByteArray &bytes); // icon + QMovie playback
    void setMessage(const QString &text, bool error = false);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void setMoviesPaused(bool paused);

    SignalCli *m_cli = nullptr;
    QGridLayout *m_grid = nullptr;
    QWidget *m_gridWidget = nullptr;
    QScrollArea *m_scroll = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QLabel *m_message = nullptr;
    QScrollArea *m_categoryScroll = nullptr;
    QWidget *m_categoryContents = nullptr;
    QHBoxLayout *m_categoryLayout = nullptr;
    QToolButton *m_addButton = nullptr;
    QHash<QString, QPointer<QLabel>> m_packHeadings;
    QHash<QString, QPointer<QToolButton>> m_categoryButtons;
    QList<Pack> m_packList;
    QList<Pack> m_visiblePacks;
    int m_renderPackIndex = 0;
    int m_renderStickerIndex = 0;
    int m_renderRow = 0;
    quint64 m_renderGeneration = 0;
    quint64 m_cliGeneration = 0;
    bool m_hasLoadedPacks = false;
    bool m_refreshInFlight = false;
    bool m_needsRender = false;
    int m_imageLoadsInFlight = 0;
    QHash<QString, QByteArray> m_imageCache;
    QHash<QString, bool> m_imagePending;
    QSet<QString> m_imageFailed;
    QHash<QString, QPointer<QPushButton>> m_stickerButtons;
    QSet<QString> m_sendWhenLoaded;
};
