#pragma once

#include <QAbstractScrollArea>
#include <QImage>
#include <QPixmap>
#include <QTextCursor>
#include <functional>
#include <memory>

#include "Appearance.h"
#include "Model.h"

class QTextDocument;
class ApngMovie;
class QMovie;
class QMediaPlayer;
class QAudioOutput;
class QVideoSink;
class QToolButton;
class QSlider;
class QLabel;

// Message list that paints rounded bubbles itself: quotes, inline images,
// file rows, reactions, date separators, links and per-message text selection.
class ScrollToLatestButton;

class ChatView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit ChatView(QWidget *parent = nullptr);
    ~ChatView() override;

    // Maps an author key (number/UUID) to a display name.
    void setNameResolver(std::function<QString(const QString &)> fn) { m_nameOf = std::move(fn); }
    void setSelfKey(const QString &key) { m_self = key; }
    void setGroupMode(bool group);
    void setAppearance(const Appearance &a);
    Appearance appearance() const { return m_ap; }

    void showMessages(const QList<Message> &msgs, bool keepScroll = false); // full rebuild
    void appendMessage(const Message &m);                                   // fast path
    void setAudioPlayback(int messageIndex, int attachmentIndex, bool loading, bool playing,
                          qint64 positionMs, qint64 durationMs);
    void setVideoLoading(int messageIndex, int attachmentIndex, bool loading);
    bool isVideoActive(int messageIndex, int attachmentIndex) const;
    bool isVideoLoading(int messageIndex, int attachmentIndex) const;
    void playVideoAttachment(int messageIndex, int attachmentIndex, const QString &path);
    void toggleVideoPlayback();
    void stopVideoPlayback();
    void setAnimatedAttachment(int messageIndex, int attachmentIndex, const QByteArray &data);
    bool hasAnimatedAttachment(int messageIndex, int attachmentIndex) const;
    void clearView();
    void scrollToMessage(int idx);

    // Message under a viewport position, -1 if none.
    int messageAt(const QPoint &viewportPos) const;
    int messageCount() const { return m_msgs.size(); }
    // Incoming messages that arrived while the view was scrolled away from the bottom.
    int missedMessages() const { return m_missed; }

    // Geometry in viewport coordinates (also used by tests).
    QRect bubbleRect(int idx) const;
    QRect quoteRect(int idx) const;
    QRect attachmentRect(int idx, int att) const;
    QRect textRect(int idx) const;
    QString selectedText() const;
    // All messages as text (sender, quote, attachments, reactions) – for tests.
    QString plainText() const;

signals:
    void replyRequested(int idx);
    void editRequested(int idx); // own text message within the edit window
    void deleteRequested(int idx);
    void reactionChosen(int idx, const QString &emoji);
    void moreReactionsRequested(int idx, const QPoint &globalPos);
    void attachmentActivated(int msgIdx, int attIdx);     // click: view / open
    void attachmentSaveRequested(int msgIdx, int attIdx); // context menu
    void animatedAttachmentRequested(int msgIdx, int attIdx);
    void videoPlaybackError(const QString &error);
    void linkClicked(const QString &url);

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    struct Pill {
        QRect rect;
        QString text;
        bool own = false;
    };
    struct Item {
        bool built = false;
        bool renderResourcesBuilt = false;
        // content (built once)
        std::shared_ptr<QTextDocument> doc;
        QList<QPixmap> pix;
        QList<QMovie *> movies;
        QList<ApngMovie *> apngMovies; // Signal's animated stickers are APNG, which QMovie cannot play
        mutable QList<bool> animationRequested;
        QString sender, quoteAuthor, quoteText, timeText, dateLabel;
        QString quoteNote;
        QList<QSize> attachmentImageSizes;
        bool showSender = false;
        // layout (item-local coordinates, valid for m_layoutWidth)
        int y = 0, height = 0;
        QRect dateRect, bubble, senderRect, quoteBox, quoteAuthorRect, quoteTextRect, textRect, timeRect, pillsRect;
        QList<QRect> attRects;
        QList<Pill> pills;
    };
    struct Hit {
        enum Kind { None, Bubble, Text, Link, Quote, Attachment } kind = None;
        int msg = -1;
        int index = -1;   // attachment index
        int textPos = -1; // cursor position in the message text
        QString href;
    };

    void buildItem(int i);
    void ensureItemRenderResources(int i, bool includePixmaps = true);
    void releaseItemRenderResources(int i);
    void layoutItem(int i, int y);
    void relayoutAll();
    void updateScrollRange();
    int totalHeight() const { return m_items.isEmpty() ? 0 : m_items.last().y + m_items.last().height; }
    int itemAtY(int docY) const;
    Hit hitTest(const QPoint &vp) const;
    void paintItem(QPainter &p, int i);
    QString nameOf(const QString &key) const;
    void clearSelection();
    void updateCursorShape(const QPoint &vp);
    void stopAutoScroll();
    bool atBottom() const;
    void updateToLatestButton();
    void updateVideoGeometry();
    QRect attachmentRectForVideo() const;

    QList<Message> m_msgs;
    QList<Item> m_items;
    std::function<QString(const QString &)> m_nameOf;
    QString m_self;
    bool m_group = false;
    Appearance m_ap;
    int m_layoutWidth = -1;
    int m_renderFirst = -1;
    int m_renderLast = -1;
    int m_resizeScrollValue = 0;
    bool m_resizeAtBottom = true;
    int m_audioMessage = -1;
    int m_audioAttachment = -1;
    bool m_audioLoading = false;
    bool m_audioPlaying = false;
    qint64 m_audioPositionMs = 0;
    qint64 m_audioDurationMs = 0;
    QMediaPlayer *m_videoPlayer = nullptr;
    QAudioOutput *m_videoAudioOutput = nullptr;
    QVideoSink *m_videoSink = nullptr;
    QImage m_videoFrame;
    QToolButton *m_videoToggle = nullptr;
    QSlider *m_videoProgress = nullptr;
    QLabel *m_videoElapsed = nullptr;
    QLabel *m_videoRemaining = nullptr;
    qint64 m_videoMessageTs = 0;
    int m_videoAttachment = -1;
    qint64 m_videoLoadingTs = 0;
    int m_videoLoadingAttachment = -1;

    // selection inside one message
    int m_selItem = -1;
    QTextCursor m_selCursor;
    bool m_dragging = false;
    QPoint m_pressPos;
    Hit m_pressHit;
    QTimer *m_autoScrollTimer = nullptr;
    QTimer *m_resizeTimer = nullptr;
    ScrollToLatestButton *m_toLatest = nullptr; // round arrow button at the bottom right
    int m_missed = 0;
    QPoint m_autoScrollAnchor;
    QPoint m_autoScrollPos;
    bool m_autoScrolling = false;
    int m_flash = -1;
};
