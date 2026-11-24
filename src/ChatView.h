#pragma once

#include <QAbstractScrollArea>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QStringList>
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
    void setNameResolver(std::function<QString(const QString &)> fn);
    void setSelfKey(const QString &key) { m_self = key; }
    void setGroupMode(bool group);
    void setReadReceiptsEnabled(bool enabled);
    void setDeliveryStatusVisible(bool visible);
    void setAppearance(const Appearance &a);
    Appearance appearance() const { return m_ap; }

    void showMessages(const QList<Message> &msgs, bool keepScroll = false); // full rebuild
    // Switch chats using a small per-conversation layout cache. The cache is invalidated
    // when a background conversation receives an update.
    void showMessages(const QList<Message> &msgs, bool keepScroll, const QString &conversationKey,
                      bool group, const QList<PinInfo> &pins);
    void invalidateConversation(const QString &key);
    // Update only the status indicators; receipt events must not rebuild a whole chat.
    void updateMessageReceipt(const QString &conversationKey, int messageIndex, int receiptStatus);
    void appendMessage(const Message &m);                                   // fast path
    void replaceLastMessage(const Message &m);
    // Pinned messages shown in the banner above the list (oldest first, at most three).
    void setPins(const QList<PinInfo> &pins);
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
    // Short description of a message for the pinned-message banner / its menu.
    QString pinSnippet(int idx) const;

signals:
    void replyRequested(int idx);
    void forwardRequested(int idx);
    void messageDetailsRequested(int idx);
    void editRequested(int idx); // own text message within the edit window
    void deleteRequested(int idx);
    void reactionChosen(int idx, const QString &emoji);
    void moreReactionsRequested(int idx, const QPoint &globalPos);
    // Pin this message for everyone, with the duration in seconds (-1 = forever).
    void pinRequested(int idx, qint64 durationSeconds);
    void unpinRequested(int idx);
    // Right-click or "…" on the pinned-message banner: MainWindow builds the options menu.
    void pinBannerMenuRequested(const QPoint &globalPos);
    // Polls (like the official clients): click an option to vote, the context menu shows
    // the details and lets the author end the poll early.
    void pollOptionClicked(int idx, int optionIndex);
    void pollVotesRequested(int idx);
    void pollTerminateRequested(int idx);
    // Contact cards: clicking the card opens (or creates) the chat with that contact.
    void contactCardActivated(int idx);
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
        // Reused only while the window is being resized; discarded shortly after.
        std::shared_ptr<QTextDocument> measureDoc;
        QList<QPixmap> pix;
        QPixmap linkPreviewPixmap;
        QString linkPreviewTitle, linkPreviewDescription, linkPreviewHost;
        QList<QMovie *> movies;
        QList<ApngMovie *> apngMovies; // Signal's animated stickers are APNG, which QMovie cannot play
        mutable QList<bool> animationRequested;
        QString sender, quoteAuthor, quoteText, timeText, statusText, dateLabel;
        bool statusRead = false;
        QString quoteNote;
        QList<QSize> attachmentImageSizes;
        bool showSender = false;
        // layout (item-local coordinates, valid for m_layoutWidth)
        int y = 0, height = 0;
        QRect dateRect, bubble, senderRect, quoteBox, quoteAuthorRect, quoteTextRect, textRect, timeRect, statusRect, pillsRect;
        QRect linkPreviewRect, linkPreviewImageRect, linkPreviewTitleRect, linkPreviewDescriptionRect, linkPreviewHostRect;
        QList<QRect> attRects;
        QList<Pill> pills;
        // Poll block (question, status line, one hit row per option, vote total).
        QRect pollRect, pollStatusRect, pollQuestionRect, pollFooterRect;
        QList<QRect> pollOptionRects;
        QStringList pollOptionTexts; // the (line-fitted) option texts painted in the rows
        // Contact card block (avatar left, name + phone number right, Message button below).
        QRect contactRect, contactButtonRect;
        QPixmap contactAvatarPixmap;
    };
    struct Hit {
        enum Kind { None, Bubble, Text, Link, Quote, Attachment, PollOption, ContactCard } kind = None;
        int msg = -1;
        int index = -1;   // attachment index
        int textPos = -1; // cursor position in the message text
        QString href;
    };
    struct CachedConversation {
        QList<Item> items;
        QList<PinInfo> pins;
        int messageCount = 0;
        qint64 lastSortTs = 0;
        bool group = false;
        int bannerCycle = 0;
        int missed = 0;
        int scrollValue = 0;
        int layoutWidth = -1;
    };

    void buildItem(int i);
    void ensureItemRenderResources(int i, bool includePixmaps = true);
    void releaseItemRenderResources(int i);
    void disposeItemResources(QList<Item> &items);
    void cacheCurrentConversation();
    void discardCachedConversation(const QString &key);
    void invalidateAllCachedConversations();
    void layoutItem(int i, int y, bool cacheMeasureDoc = false);
    void previewResize(int width);
    void relayoutAll(bool cacheMeasureDocs = false);
    void updateScrollRange();
    int totalHeight() const { return m_items.isEmpty() ? 0 : m_items.last().y + m_items.last().height; }
    int itemAtY(int docY) const;
    Hit hitTest(const QPoint &vp) const;
    void paintItem(QPainter &p, int i);
    QString nameOf(const QString &key) const;
    void clearSelection();
    void selectWordAt(int msg, int textPos); // double click: select the word under the cursor
    void updateCursorShape(const QPoint &vp);
    void stopAutoScroll();
    bool atBottom() const;
    void updateToLatestButton();
    void updateVideoGeometry();
    QRect attachmentRectForVideo() const;

    // Pinned-message banner (fixed strip at the top of the viewport).
    static constexpr int kBannerHeight = 44;
    QRect pinBannerRect() const;                // whole banner in viewport coordinates
    QRect pinBannerMenuRect() const;            // the "…" button at the right end
    int pinMessageIndex(const PinInfo &pin) const;
    void drawPinBanner(QPainter &p);

    QList<Message> m_msgs;
    QList<Item> m_items;
    QString m_conversationKey;
    QHash<QString, CachedConversation> m_conversationCache;
    QStringList m_conversationCacheOrder;
    QList<PinInfo> m_pins;   // oldest first; empty = no banner, no content offset
    int m_bannerCycle = 0;   // which pin the banner currently shows
    int m_bannerOffset = 0;  // extra top offset of the message list (kBannerHeight when pinned)
    // Measured text size per (message text/styles, wrap width, font). Re-laying out a conversation
    // (resize, switching back to a chat) then skips the expensive QTextDocument::setHtml().
    QHash<size_t, QSize> m_textSizeCache;
    size_t textMeasureKey(const Message &m, int innerMax) const; // 0 = do not cache
    mutable QFont m_measureFont;           // font/DPI the hash below was computed for
    mutable size_t m_measureFontHash = 0;
    mutable int m_measureDpi = 0;
    std::function<QString(const QString &)> m_nameOf;
    QString m_self;
    bool m_group = false;
    bool m_showReadReceipts = true;
    bool m_showDeliveryStatus = true;
    Appearance m_ap;
    int m_layoutWidth = -1;
    int m_resizePreviewWidth = -1;
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
    bool m_wordSelected = false; // word selected by double click; keep it until the next press
    bool m_dblClick = false;     // armed by mouseDoubleClickEvent, consumed by the next release
    QTimer *m_autoScrollTimer = nullptr;
    QTimer *m_resizeTimer = nullptr;
    QTimer *m_resizeCleanupTimer = nullptr;
    ScrollToLatestButton *m_toLatest = nullptr; // round arrow button at the bottom right
    int m_missed = 0;
    QPoint m_autoScrollAnchor;
    QPoint m_autoScrollPos;
    bool m_autoScrolling = false;
    int m_flash = -1;
};
