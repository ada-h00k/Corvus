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
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
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
    void loadVisibleStickers();
    void addPack();
    void installPack(const QString &uri);
    void updateCategorySelection();
    void loadSticker(const QString &packId, int stickerId, QPointer<QPushButton> button = {});
    void setMessage(const QString &text, bool error = false);

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
    QHash<QString, QByteArray> m_imageCache;
    QHash<QString, bool> m_imagePending;
    QHash<QString, QPointer<QPushButton>> m_stickerButtons;
    QSet<QString> m_sendWhenLoaded;
};
