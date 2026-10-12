#pragma once

#include "Appearance.h"
#include "HistoryStore.h"
#include "Model.h"
#include "NotificationSettings.h"
#include "SignalCli.h"

#include <QHash>
#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QSet>
#include <QTemporaryDir>
#include <functional>

class ChatView;
class EmojiPicker;
class MessageInput;
class Notifier;
class SoundPlayer;
class QAction;
class QMenu;
class QSystemTrayIcon;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QShowEvent;
class QLabel;
class QLineEdit;
class QMimeData;
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

protected:
    void closeEvent(QCloseEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

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
    void changePassphrase();
    void editAppearance();
    void editNotifications();
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
    void deleteConversation(const QString &key);

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

    // --- notifications, sound, tray
    void applyNotificationSettings();
    void updateTray();
    void alertIncoming(const QString &key, const Message &m);
    void openConversation(const QString &key);

    // --- list / view
    void refreshList();
    void showConversation(bool keepScroll = false);
    void updateReplyBar();
    void updateAttachBar();

    // --- actions on messages
    void startReply(int idx);
    void startEdit(int idx);
    void cancelEdit(bool restoreDraft = true);
    void sendEdit(const QString &text);
    // Applies an edit (own or received). author = who must have written the message.
    bool applyEdit(const QString &key, qint64 targetTs, const QStringList &authors, const QString &text, qint64 newTs);
    void updateEditBar();
    void sendReaction(int idx, const QString &emoji);
    void addRecipient(QJsonObject &params, const Conversation &c) const;
    void fetchThumbnails(const QString &key, qint64 ts, const QString &author, int attempt = 0);
    void fetchAttachmentData(const QString &key, const Attachment &att,
                             std::function<void(const QByteArray &, const QString &)> cb);
    void openAttachment(int msgIdx, int attIdx);
    void saveAttachment(int msgIdx, int attIdx);
    // Files or an image (e.g. a screenshot) from drag & drop / the clipboard -> pending attachments.
    bool attachFromMimeData(const QMimeData *md);

    SignalCli m_cli;
    HistoryStore m_store;
    QNetworkAccessManager m_nam;

    QHash<QString, Conversation> m_convs;
    QHash<QString, PendingSend> m_pendingSends; // visible previews until signal-cli confirms the send
    QHash<QString, QString> m_alias; // uuid -> number
    QHash<QString, QString> m_contactNames; // key -> contact name from signal-cli
    QString m_current;
    QString m_account;
    QString m_version; // e.g. "0.13.12"
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

    Quote m_replyQuote;
    // Message being edited (original timestamp + conversation); m_editKey empty = not editing.
    QString m_editKey;
    qint64 m_editTs = 0;
    QString m_editDraft; // what was typed before editing started
    QStringList m_pendingFiles;
    QSet<QString> m_thumbPending;
    std::function<void(const QString &)> m_pickAction;
    QTemporaryDir m_tmp;

    QListWidget *m_list;
    QLabel *m_title;
    ChatView *m_view;
    QWidget *m_editBar;
    QLabel *m_editLabel;
    QWidget *m_replyBar;
    QLabel *m_replyLabel;
    QWidget *m_attachBar;
    QLabel *m_attachLabel;
    QLabel *m_attachThumb;
    MessageInput *m_input;
    QToolButton *m_attachBtn;
    QToolButton *m_emojiBtn;
    QPushButton *m_send;
    EmojiPicker *m_picker;
    QLabel *m_statusLabel;
    QLabel *m_updateLabel;
};
