#pragma once

#include "Appearance.h"
#include "HistoryStore.h"
#include "Model.h"
#include "NotificationSettings.h"
#include "SignalCli.h"

#include <QHash>
#include <QMainWindow>
#include <QMediaPlayer>
#include <QNetworkAccessManager>
#include <QIcon>
#include <QPixmap>
#include <QPointer>
#include <QToolButton>
#include "MessageInput.h"
#include <QSet>
#include <QTimer>
#include <QTemporaryDir>
#include <functional>

class ChatView;
class EmojiPicker;
class GiphyPicker;
class MessageInput;
class ProfileDialog;
class Notifier;
class SoundPlayer;
class QAction;
class QMenu;
class QSystemTrayIcon;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QEvent;
class QShowEvent;
class QLabel;
class QLineEdit;
class QMimeData;
class QAudioOutput;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QToolButton;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow();

    // Queue files for the next message (attach button, drag & drop, paste).
    void addPendingFiles(const QStringList &files);
    // Local per-chat mute (signal-cli does not report mute state): no notification, no sound.
    void setMuted(const QString &key, bool muted);

    // Closing the window hides it to the tray only if the tray icon is really there and enabled.
    static bool shouldHideOnClose(bool trayVisible, bool closeToTray, bool quitting);
    static bool shouldStartMinimizedInTray(bool trayVisible, bool startMinimizedInTray);
    void showAtStartup();

protected:
    void closeEvent(QCloseEvent *e) override;
    void showEvent(QShowEvent *e) override;
    bool event(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void positionExpandButton();

private slots:
    void initialise();
    void onEnvelope(const QJsonObject &env);
    void onSelectionChanged(QListWidgetItem *item);
    void sendMessage();
    void newConversation();
    void linkDevice();
    void reconnect();
    void chooseExecutable();
    void checkForUpdates(bool silent);
    void showAbout();
    void changeHistoryProtection();
    void editAppearance();
    void editNotifications();
    void editProfile();
    void searchGifs();
    void configureGiphyApiKey();
    void quitApplication();
    void toggleWindow();
    void activateConversation(const QString &key);

private:
    // --- setup / signal-cli
    bool ensureExecutable();
    void showCliProblem(const QString &title, const SignalCli::Result &r);
    void connectAccount();
    void loadContactsAndGroups();
    void setConnected(bool connected, const QString &info = QString());

    // --- encrypted history (Rust store)
    QString historyPath() const;
    bool unlockHistory();
    void loadHistory(const QJsonArray &records);
    QJsonArray allRecords() const;
    void persist(const QJsonObject &record);
    void learnName(const QString &key, const QString &name);
    void setNickname(const QString &key, const QString &nickname);
    void deleteConversation(const QString &key);
    void deleteMessage(int idx);
    bool removeMessage(const QString &key, qint64 timestamp);

    // --- model
    struct PendingSend {
        QString conversationKey;
        qint64 temporaryTimestamp = 0;
        Message message;
        bool echoReceived = false;
    };
    void finalizePendingSend(const QString &id, qint64 timestamp, const Message *echo = nullptr);
    void removePendingPreview(const PendingSend &pending);
    Conversation &conv(const QString &key, const QString &title = QString(), bool group = false);
    QString resolveKey(const QString &id) const;
    QString displayName(const QString &key) const;
    QString effectiveAuthor(const Conversation &c, const Message &m) const;
    bool authorMatches(const Conversation &c, const Message &m, const QStringList &authors) const;
    int findMessage(const Conversation &c, qint64 ts, const QStringList &authors) const;
    bool addMessage(const QString &key, Message m, const QString &title = QString(), bool group = false);
    bool applyReaction(const QString &key, qint64 ts, const QStringList &authors, const QString &reactor,
                       const QString &emoji);
    void setArchived(const QString &key, bool archived);
    void editExpiration(const QString &key);
    void setExpiration(const QString &key, int seconds);
    void startIncomingExpirationTimers(const QString &key);
    void expireMessages();

    // --- notifications, sound, tray
    void applyNotificationSettings();
    void updateTray();
    void alertIncoming(const QString &key, const Message &m);
    void installInputWatcher();
    qint64 m_lastInputMs = 0; // last keyboard/mouse input inside this window
    bool userIsLooking() const; // window really in front of the user (not just "active" while covered)
    void openConversation(const QString &key);
    void focusComposer(); // put the keyboard focus into the message field (deferred)

    // --- list / view
    void refreshList();
    // Avatars in the chat list (signal-cli getAvatar, cached in memory only).
    QIcon avatarIcon(const Conversation &c) const;
    void fetchMissingAvatars();
    void fetchAvatar(const QString &key);
    void refreshAvatar(const QString &key);   // re-fetch (throttled), e.g. after a message from that contact
    void applyAvatarToList(const QString &key);
    bool avatarsWanted() const;
    QPixmap avatarPixmap(const Conversation &c) const;
    void showConversation(bool keepScroll = false);
    void openMessageSearch();
    void closeMessageSearch();
    void updateMessageSearch(bool selectFirst = false);
    void navigateMessageSearch(bool backwards);
    void updateReplyBar();
    void updateAttachBar();

    // --- actions on messages
    void startReply(int idx);
    void startEdit(int idx);
    void editLastMessage();
    void cancelEdit(bool restoreDraft = true);
    // Per-conversation drafts: whatever is typed / quoted / attached in the composer
    // belongs to the conversation it was written in.
    void saveDraft(const QString &key);
    void loadDraft(const QString &key);
    void switchConversation(const QString &key);
    void sendEdit(const QString &text);
    // Applies an edit (own or received). author = who must have written the message.
    bool applyEdit(const QString &key, qint64 targetTs, const QStringList &authors, const QString &text, qint64 newTs);
    void updateEditBar();
    void sendReaction(int idx, const QString &emoji);
    void addRecipient(QJsonObject &params, const Conversation &c) const;
    void fetchThumbnails(const QString &key, qint64 ts, const QString &author, int attempt = 0);
    void loadAnimatedAttachment(int messageIndex, int attachmentIndex);
    void fetchAttachmentData(const QString &key, const Attachment &att,
                             std::function<void(const QByteArray &, const QString &)> cb);
    void openAttachment(int msgIdx, int attIdx);
    void toggleAudioAttachment(int msgIdx, int attIdx);
    void updateAudioPlaybackView();
    void downloadAndSendGif(const QString &conversationKey, const QUrl &url, const QUrl &sentAnalyticsUrl);
    void registerGiphyAnalytics(const QUrl &url);
    void saveAttachment(int msgIdx, int attIdx);
    // Files or an image (e.g. a screenshot) from drag & drop / the clipboard -> pending attachments.
    bool attachFromMimeData(const QMimeData *md);
    void sendProfileUpdate(const QJsonObject &params, const QString &successMessage);
    void loadSignalUsername();
    void applySignalUsername(const QString &username);
    void reportProfileStatus(const QString &message, bool error);

    QPointer<ProfileDialog> m_profileDialog;

    SignalCli m_cli;
    HistoryStore m_store;
    QNetworkAccessManager m_nam;
    QMediaPlayer *m_audioPlayer = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QUrl m_pendingGifAnalyticsUrl;

    QHash<QString, Conversation> m_convs;
    QHash<QString, PendingSend> m_pendingSends; // visible previews until signal-cli confirms the send
    QHash<QString, QString> m_alias; // uuid -> number
    QHash<QString, QString> m_contactNames; // key -> contact name from signal-cli
    QString m_current;
    QString m_account;
    QString m_version; // e.g. "0.13.12"
    QString m_historyProtectionMode;
    bool m_showArchived = false;
    int m_connectGen = 0; // invalidates callbacks of an older connect attempt
    Appearance m_appearance;
    NotificationSettings m_notify;
    SoundPlayer *m_sound = nullptr;
    Notifier *m_notifier = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_trayMenu = nullptr;
    bool m_quitting = false;
    qint64 m_lastSoundMs = 0;
    int m_trayUnread = -1;
    QString m_audioKey;
    qint64 m_audioMessageTs = 0;
    int m_audioAttachmentIndex = -1;
    bool m_audioLoading = false;
    qint64 m_audioPositionMs = 0;
    qint64 m_audioDurationMs = 0;

    struct Draft {
        QString text;
        int cursor = 0;
        Quote reply;
        QStringList files;
        QUrl gifAnalyticsUrl;
    };
    QHash<QString, Draft> m_drafts; // conversation key -> unsent composer state

    Quote m_replyQuote;
    // Message being edited (original timestamp + conversation); m_editKey empty = not editing.
    QString m_editKey;
    qint64 m_editTs = 0;
    QString m_editDraft; // what was typed before editing started
    QStringList m_pendingFiles;
    QSet<QString> m_thumbPending;
    QSet<QString> m_gifPending;
    QHash<QString, QPixmap> m_avatars;   // conversation key -> circular avatar (64 px)
    QSet<QString> m_avatarTried;         // already requested since the last (re)connect
    int m_avatarInFlight = 0;
    QSet<QString> m_avatarBusy;                 // requests currently running (per conversation)
    QHash<QString, QByteArray> m_avatarDigest;  // conversation key -> hash of the picture last shown
    QHash<QString, qint64> m_avatarFetchedMs;   // conversation key -> time of the last request
    QTimer m_avatarRefreshTimer;                // re-checks all avatars from time to time
    std::function<void(const QString &)> m_pickAction;
    QTemporaryDir m_tmp;
    QTimer m_expirationTimer;

    QListWidget *m_list;
    QLabel *m_title;
    ChatView *m_view;
    QWidget *m_searchBar;
    QLineEdit *m_searchEdit;
    QLabel *m_searchCount;
    QList<int> m_searchResults;
    int m_searchPosition = -1;
    QWidget *m_editBar;
    QLabel *m_editLabel;
    QWidget *m_replyBar;
    QLabel *m_replyLabel;
    QWidget *m_attachBar;
    QLabel *m_attachLabel;
    QLabel *m_attachThumb;
    QPointer<MessageInput> m_input;
    QToolButton *m_attachBtn;
    QToolButton *m_emojiBtn;
    QToolButton *m_gifBtn;
    QPointer<QToolButton> m_expandInputBtn;
    QPushButton *m_sendBtn;
    EmojiPicker *m_picker;
    QLabel *m_statusLabel;
    QLabel *m_updateLabel;
};
