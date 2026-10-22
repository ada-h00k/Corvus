#pragma once

#include "Model.h"

#include <QDialog>
#include <QCache>
#include <QHash>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <functional>

class QLabel;
class QMediaPlayer;
class QAudioOutput;
class QTabWidget;

namespace MediaGallery {

struct MediaItem {
    qint64 ts = 0;     // timestamp of the message (its identity)
    int attIndex = 0;  // index in Message::attachments
    QDateTime time;
    bool outgoing = false;
    Attachment att;
};

struct LinkItem {
    qint64 ts = 0;
    QDateTime time;
    bool outgoing = false;
    QString url;
};

// What the gallery shows for one conversation, newest first.
struct Collection {
    QVector<MediaItem> media; // images, GIFs and videos
    QVector<MediaItem> audio;
    QVector<MediaItem> files; // everything else
    QVector<LinkItem> links;
};

// Sorts all attachments and links of a conversation into the four gallery
// categories. Deleted messages contribute nothing.
Collection collect(const QList<Message> &messages);

// http(s) links in `text`, in order, without duplicates.
QStringList extractLinks(const QString &text);

// "October 2026": the heading under which items of that month are grouped.
QString monthTitle(const QDate &date);

} // namespace MediaGallery

// Everything the gallery needs from the application. Keeps the dialog independent
// of MainWindow (and testable).
struct MediaGalleryHost {
    // Current messages of the conversation.
    std::function<QList<Message>()> messages;
    // Loads the full attachment (local file or via signal-cli).
    std::function<void(const Attachment &, std::function<void(const QByteArray &, const QString &)>)> fetch;
    // Directory for files that have to exist on disk (playback, "open").
    std::function<QString()> tempDir;
    // Jump to the message with this timestamp in the chat.
    std::function<void(qint64 ts)> showInChat;
    // A file was saved to `path`.
    std::function<void(const QString &path)> saved;
};

// "Media gallery" of a conversation, modelled after the "All media" view of
// Signal Desktop: tabs for Media (images and videos), Audio, Links and Files,
// grouped by month, newest first.
class MediaGalleryDialog : public QDialog
{
    Q_OBJECT
public:
    MediaGalleryDialog(const QString &conversationTitle, MediaGalleryHost host, QWidget *parent = nullptr);
    ~MediaGalleryDialog() override;

    // Re-reads the messages (also done whenever the window is activated).
    void reload();

    // For tests: number of entries per tab.
    int count(int tab) const;
    // Index of the tabs.
    enum Tab { TabMedia = 0, TabAudio, TabLinks, TabFiles };

protected:
    bool event(QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

private:
    class View;
    void rebuildMedia();
    void rebuildAudio();
    void rebuildLinks();
    void rebuildFiles();

    QPixmap thumbnailFor(int id);
    void pumpThumbnails();

    void activateMedia(int id);
    void activateAudio(int id);
    void activateFile(int id);
    void activateLink(int id);
    void showContextMenu(Tab tab, int id, const QPoint &globalPos);
    void saveItem(const MediaGallery::MediaItem &item);
    void showStatus(const QString &text, int timeoutMs = 0);
    void openVideo(const MediaGallery::MediaItem &item);
    void openImage(const MediaGallery::MediaItem &item);
    QString writeTempFile(const QByteArray &data, const QString &prefix, const Attachment &att);

    MediaGalleryHost m_host;
    QString m_title;
    MediaGallery::Collection m_col;
    QTabWidget *m_tabs = nullptr;
    View *m_media = nullptr;
    View *m_audio = nullptr;
    View *m_links = nullptr;
    View *m_files = nullptr;
    QLabel *m_status = nullptr;

    // thumbnails of the media grid (square, cropped)
    QCache<QString, QPixmap> m_thumbs;
    QStringList m_thumbQueue;
    QSet<QString> m_thumbQueued;
    QSet<QString> m_thumbFailed;
    int m_thumbInFlight = 0;
    bool m_pumpScheduled = false;

    // audio playback
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOut = nullptr;
    int m_audioId = -1;        // index in m_col.audio
    qint64 m_audioTs = 0;      // survives reload()
    int m_audioAttIndex = -1;
    bool m_audioLoading = false;
    qint64 m_audioPos = 0;
    qint64 m_audioDur = 0;
    int m_tempCounter = 0;
};
