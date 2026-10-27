#include "MainWindow.h"

#include "AppearancePage.h"
#include "AppIcon.h"
#include "Attachments.h"
#include "ChatView.h"
#include "ConversationOrder.h"
#include "EmojiPicker.h"
#include "GiphyPicker.h"
#include "GiphyApiKeyStore.h"
#include "Envelope.h"
#include "HistoryKeychain.h"
#include "ImageViewer.h"
#include "LinkDialog.h"
#include "MediaGallery.h"
#include "MessageInput.h"
#include "Notifier.h"
#include "PassphraseDialog.h"
#include "SafetyNumberDialog.h"
#include "ProfilePage.h"
#include "SettingsDialog.h"
#include "Records.h"
#include "SoundPlayer.h"

#include <QAction>
#include <QApplication>
#include <QAudioOutput>
#include <QClipboard>
#include <QCursor>
#include <QCryptographicHash>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFrame>
#include <QProcess>
#ifdef HAVE_QT_DBUS
#include <QDBusConnection>
#include <QDBusInterface>
#endif
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShowEvent>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleHints>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <algorithm>

static const QString kFolderKey = QStringLiteral("::archived");
static const QString kBackKey = QStringLiteral("::back");
static const qint64 kMaxThumbSource = 30LL * 1024 * 1024;
static const qint64 kMaxAttachment = 100LL * 1024 * 1024; // Signal's upload limit

static QJsonArray styleRangesToJson(const QList<TextStyleRange> &styles)
{
    QJsonArray arr;
    for (const TextStyleRange &r : styles)
        if (r.start >= 0 && r.length > 0 && !r.style.isEmpty())
            arr.append(QStringLiteral("%1:%2:%3").arg(r.start).arg(r.length).arg(r.style));
    return arr;
}

static QList<TextStyleRange> styleRangesFromJson(const QJsonArray &arr)
{
    QList<TextStyleRange> styles;
    for (const QJsonValue &v : arr) {
        const QStringList p = v.toString().split(QLatin1Char(':'));
        if (p.size() != 3) continue;
        bool ok1 = false, ok2 = false;
        TextStyleRange r;
        r.start = p.at(0).toInt(&ok1);
        r.length = p.at(1).toInt(&ok2);
        r.style = p.at(2);
        if (ok1 && ok2 && r.start >= 0 && r.length > 0 && !r.style.isEmpty()) styles.append(r);
    }
    return styles;
}

static QJsonArray normalizedStyles(const QJsonArray &styles, int lead, int end)
{
    QJsonArray out;
    for (const QJsonValue &v : styles) {
        const QStringList p = v.toString().split(QLatin1Char(':'));
        if (p.size() != 3) continue;
        bool ok1 = false, ok2 = false;
        const int start = p.at(0).toInt(&ok1);
        const int length = p.at(1).toInt(&ok2);
        if (!ok1 || !ok2 || length <= 0) continue;
        const int a = qMax(start, lead);
        const int b = qMin(start + length, end);
        if (a < b)
            out.append(QStringLiteral("%1:%2:%3").arg(a - lead).arg(b - a).arg(p.at(2)));
    }
    return out;
}


static AppIcon::Mode iconMode(const Appearance &a)
{
    switch (a.appIcon) {
    case Appearance::IconLight: return AppIcon::Mode::Light;
    case Appearance::IconDark: return AppIcon::Mode::Dark;
    default: return AppIcon::Mode::FollowTheme;
    }
}

MainWindow::MainWindow()
{
    setWindowTitle(QStringLiteral("Corvus"));
    resize(1020, 680);
    setAcceptDrops(true);

    auto *split = new QSplitter(this);
    m_list = new QListWidget;
    m_list->setMinimumWidth(240);
    m_list->setIconSize(QSize(32, 32));
    split->addWidget(m_list);
#ifdef Q_OS_MACOS
    // Sidebar like a macOS source list: no frame, no focus ring, hairline divider.
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setAttribute(Qt::WA_MacShowFocusRect, false);
    split->setHandleWidth(1);
#endif

    auto *right = new QWidget;
    auto *v = new QVBoxLayout(right);
    m_title = new QLabel(tr("No conversation selected"));
    QFont f = m_title->font();
    f.setBold(true);
    f.setPointSize(f.pointSize() + 2);
    m_title->setFont(f);
    auto *titleRow = new QHBoxLayout;
    titleRow->addWidget(m_title, 1);
    auto *findMessages = new QToolButton(right);
    findMessages->setObjectName(QStringLiteral("openMessageSearch"));
    findMessages->setText(tr("Search"));
    findMessages->setToolTip(tr("Search this conversation (Ctrl+F)"));
    findMessages->setAccessibleName(tr("Search this conversation"));
    titleRow->addWidget(findMessages);
    v->addLayout(titleRow);

    // "Safety number changed" warning above the chat (shown by updateSafetyBar()).
    m_safetyBar = new QWidget;
    m_safetyBar->setObjectName(QStringLiteral("safetyChangedBar"));
    m_safetyBar->setStyleSheet(QStringLiteral(
        "#safetyChangedBar { background: #fff3cd; border: 1px solid #e0c36a; border-radius: 4px; }"
        "#safetyChangedBar QLabel { color: #664d03; }"));
    {
        auto *l = new QHBoxLayout(m_safetyBar);
        l->setContentsMargins(8, 6, 8, 6);
        m_safetyLabel = new QLabel;
        m_safetyLabel->setTextFormat(Qt::PlainText);
        m_safetyLabel->setWordWrap(true);
        m_safetyLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        l->addWidget(m_safetyLabel, 1);
        auto *verifyBtn = new QPushButton(tr("Verify …"));
        verifyBtn->setObjectName(QStringLiteral("safetyVerifyButton"));
        connect(verifyBtn, &QPushButton::clicked, this, [this] { showSafetyNumber(m_current); });
        l->addWidget(verifyBtn);
        auto *dismissBtn = new QToolButton;
        dismissBtn->setObjectName(QStringLiteral("safetyDismissButton"));
        dismissBtn->setText(QStringLiteral("✕"));
        dismissBtn->setToolTip(tr("Dismiss"));
        connect(dismissBtn, &QToolButton::clicked, this, [this] { dismissSafetyChange(m_current); });
        l->addWidget(dismissBtn);
    }
    m_safetyBar->hide();
    v->addWidget(m_safetyBar);

    m_view = new ChatView;
    m_view->installEventFilter(this);
    m_view->setNameResolver([this](const QString &k) { return displayName(k); });
    m_appearance = Appearance::load(palette());
    m_view->setAppearance(m_appearance);
    v->addWidget(m_view, 1);
    // main() set the application icon before the icon choice was known
    AppIcon::setMode(iconMode(m_appearance));
    QApplication::setWindowIcon(AppIcon::app());

    // reply / attachment bars above the input
    auto makeBar = [&](QLabel *&label, std::function<void()> onClose) {
        auto *bar = new QWidget;
        auto *l = new QHBoxLayout(bar);
        l->setContentsMargins(4, 0, 4, 0);
        label = new QLabel;
        label->setTextFormat(Qt::PlainText);
        // Ignored: the label's text width must not count towards the window's minimum width,
        // otherwise showing a long reply / edit / file bar makes a small window grow by itself.
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setMinimumWidth(0);
        l->addWidget(label, 1);
        auto *x = new QToolButton;
        x->setText(QStringLiteral("✕"));
        x->setAutoRaise(true);
        connect(x, &QToolButton::clicked, this, [onClose] { onClose(); });
        l->addWidget(x);
        bar->hide();
        v->addWidget(bar);
        return bar;
    };
    m_editBar = makeBar(m_editLabel, [this] { cancelEdit(); });
    m_replyBar = makeBar(m_replyLabel, [this] {
        m_replyQuote = Quote();
        updateReplyBar();
    });
    m_attachBar = makeBar(m_attachLabel, [this] {
        m_pendingFiles.clear();
        m_pendingGifAnalyticsUrl.clear();
        updateAttachBar();
    });
    m_attachThumb = new QLabel;
    m_attachThumb->setFixedSize(44, 44);
    m_attachThumb->setAlignment(Qt::AlignCenter);
    m_attachThumb->hide();
    static_cast<QHBoxLayout *>(m_attachBar->layout())->insertWidget(0, m_attachThumb);

    auto *h = new QHBoxLayout;
    m_attachBtn = new QToolButton;
    m_attachBtn->setText(QStringLiteral("📎"));
    m_attachBtn->setToolTip(tr("Attach files"));
    m_attachBtn->setFixedSize(40, 40);
    m_emojiBtn = new QToolButton;
    m_emojiBtn->setText(QStringLiteral("😊"));
    m_emojiBtn->setToolTip(tr("Emoji and GIFs"));
    m_emojiBtn->setFixedSize(40, 40);
#ifdef Q_OS_MACOS
    // Flat, borderless buttons next to the input field, as in Messages.
    for (QToolButton *b : {m_attachBtn, m_emojiBtn})
        b->setAutoRaise(true);
#endif
    m_input = new MessageInput;
    m_input->setPlaceholderText(tr("Write a message …"));
    // Floating "popout" handle: it hovers centered above the message field,
    // overlapping the chat view, and is only shown while the mouse is over the field or over
    // the spot where the handle sits (see positionExpandButton()).
    // It is a child of the right-hand panel (not part of any layout) and is
    // positioned in positionExpandButton().
    m_expandInputBtn = new QToolButton(right);
    m_expandInputBtn->setObjectName(QStringLiteral("expandInputButton"));
    m_expandInputBtn->setText(QStringLiteral("⌃"));
    m_expandInputBtn->setToolTip(tr("Expand message field"));
    m_expandInputBtn->setAccessibleName(tr("Expand message field"));
    m_expandInputBtn->setFocusPolicy(Qt::NoFocus); // clicking must not steal focus from the field
    m_expandInputBtn->setCursor(Qt::PointingHandCursor);
    m_expandInputBtn->setFixedSize(40, 20);
    m_expandInputBtn->setStyleSheet(QStringLiteral(
        "QToolButton { background: palette(window); color: palette(window-text);"
        " border: 1px solid palette(mid); border-radius: 10px; padding: 0px; margin: 0px; font-size: 14px; }"
        "QToolButton:hover { background: palette(button); }"
        "QToolButton:disabled { color: palette(mid); }"));
    m_expandInputBtn->hide();

    m_sendBtn = new QPushButton(tr("Send"));
    m_sendBtn->setFixedHeight(40);
    m_sendBtn->setVisible(m_appearance.alwaysShowSend); // otherwise shown while the field is expanded

    h->addWidget(m_emojiBtn);
    h->addWidget(m_input, 1);
    h->addWidget(m_sendBtn);
    h->addWidget(m_attachBtn);

    m_searchBar = new QWidget(right);
    auto *searchLayout = new QHBoxLayout(m_searchBar);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    m_searchEdit = new QLineEdit(m_searchBar);
    m_searchEdit->setObjectName(QStringLiteral("messageSearchInput"));
    m_searchEdit->setPlaceholderText(tr("Search this conversation"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->installEventFilter(this);
    searchLayout->addWidget(m_searchEdit, 1);
    m_searchCount = new QLabel(m_searchBar);
    m_searchCount->setObjectName(QStringLiteral("messageSearchCount"));
    m_searchCount->setMinimumWidth(48);
    m_searchCount->setAlignment(Qt::AlignCenter);
    searchLayout->addWidget(m_searchCount);
    auto *previousMatch = new QToolButton(m_searchBar);
    previousMatch->setText(QStringLiteral("↑"));
    previousMatch->setToolTip(tr("Previous match"));
    previousMatch->setAccessibleName(tr("Previous match"));
    searchLayout->addWidget(previousMatch);
    auto *nextMatch = new QToolButton(m_searchBar);
    nextMatch->setText(QStringLiteral("↓"));
    nextMatch->setToolTip(tr("Next match"));
    nextMatch->setAccessibleName(tr("Next match"));
    searchLayout->addWidget(nextMatch);
    auto *closeSearch = new QToolButton(m_searchBar);
    closeSearch->setText(QStringLiteral("×"));
    closeSearch->setToolTip(tr("Close search"));
    closeSearch->setAccessibleName(tr("Close search"));
    searchLayout->addWidget(closeSearch);
    m_searchBar->hide();
    v->insertWidget(1, m_searchBar);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this] { updateMessageSearch(true); });
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this] { navigateMessageSearch(false); });
    connect(previousMatch, &QToolButton::clicked, this, [this] { navigateMessageSearch(true); });
    connect(nextMatch, &QToolButton::clicked, this, [this] { navigateMessageSearch(false); });
    connect(closeSearch, &QToolButton::clicked, this, &MainWindow::closeMessageSearch);
    connect(findMessages, &QToolButton::clicked, this, &MainWindow::openMessageSearch);
    m_input->installEventFilter(this);
    right->installEventFilter(this);
    v->addLayout(h);
    split->addWidget(right);
    split->setStretchFactor(1, 1);
    setCentralWidget(split);

    m_picker = new EmojiPicker(this);

    // Status bar
    m_statusLabel = new QLabel(tr("Not connected"));
    m_updateLabel = new QLabel;
    m_updateLabel->setOpenExternalLinks(true);
    statusBar()->addWidget(m_statusLabel, 1);
    statusBar()->addPermanentWidget(m_updateLabel);
#ifdef Q_OS_MACOS
    statusBar()->setSizeGripEnabled(false);
#endif

    // Menus
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("New conversation …"), QKeySequence::New, this, &MainWindow::newConversation);
    file->addAction(tr("Link device …"), this, &MainWindow::linkDevice);
    file->addAction(tr("Reconnect"), QKeySequence(Qt::CTRL | Qt::Key_R), this, &MainWindow::reconnect);
    file->addSeparator();
    file->addAction(tr("Quit"), QKeySequence::Quit, this, &MainWindow::quitApplication);

    QMenu *settings = menuBar()->addMenu(tr("&Settings"));
    settings->addAction(tr("Configure Corvus …"), QKeySequence::Preferences, this, [this] { openSettings(); });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Check for signal-cli update"), this, [this] { checkForUpdates(false); });
    help->addAction(tr("About"), this, &MainWindow::showAbout);

    for (int index = 0; index < 9; ++index) {
        auto *action = new QAction(this);
        action->setShortcut(QKeySequence(Qt::CTRL | (Qt::Key_1 + index)));
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, index] {
            int conversationIndex = 0;
            for (int row = 0; row < m_list->count(); ++row) {
                QListWidgetItem *item = m_list->item(row);
                const QString key = item->data(Qt::UserRole).toString();
                if (key == kFolderKey || key == kBackKey) continue;
                if (conversationIndex++ == index) {
                    m_list->setCurrentItem(item);
                    return;
                }
            }
        });
    }

    // Ctrl+Up / Ctrl+Down: previous / next conversation in the list
    for (const int delta : {-1, 1}) {
        auto *action = new QAction(this);
        action->setShortcut(QKeySequence(Qt::CTRL | (delta < 0 ? Qt::Key_Up : Qt::Key_Down)));
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
        connect(action, &QAction::triggered, this, [this, delta] { stepConversation(delta); });
    }

    // List: selection, folder navigation, context menu
    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *cur, QListWidgetItem *) { onSelectionChanged(cur); });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        const QString key = it->data(Qt::UserRole).toString();
        if (key == kFolderKey || key == kBackKey) {
            m_showArchived = (key == kFolderKey);
            refreshList();
        }
    });
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *it = m_list->itemAt(pos);
        if (!it) return;
        const QString key = it->data(Qt::UserRole).toString();
        if (!m_convs.contains(key)) return;
        QMenu menu(this);
        Conversation &conversation = m_convs[key];
        menu.addAction(tr("Media gallery …"), this, [this, key] { openMediaGallery(key); });
        menu.addSeparator();
        if (!conversation.isGroup) {
            menu.addAction(conversation.localNickname.isEmpty() ? tr("Set nickname…") : tr("Change nickname…"),
                           this, [this, key] {
                const QString current = m_convs.value(key).localNickname;
                bool ok = false;
                const QString nickname = QInputDialog::getText(this, tr("Contact nickname"), tr("Nickname:"),
                                                               QLineEdit::Normal, current, &ok);
                if (ok) setNickname(key, nickname.trimmed());
            });
            if (!conversation.localNickname.isEmpty())
                menu.addAction(tr("Remove nickname"), this, [this, key] { setNickname(key, QString()); });
            if (key != m_account && resolveKey(key) != m_account)
                menu.addAction(tr("View safety number …"), this, [this, key] { showSafetyNumber(key); });
            menu.addSeparator();
        }
        const bool archived = m_convs[key].archived;
        menu.addAction(archived ? tr("Unarchive") : tr("Archive"), this, [this, key, archived] { setArchived(key, !archived); });
        const bool muted = m_convs[key].muted;
        menu.addAction(muted ? tr("Unmute notifications") : tr("Mute notifications"), this, [this, key, muted] { setMuted(key, !muted); });
        menu.addAction(tr("Disappearing messages …"), this, [this, key] { editExpiration(key); });
        menu.addAction(tr("Delete conversation …"), this, [this, key] { deleteConversation(key); });
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });

    // Chat view actions
    connect(m_view, &ChatView::replyRequested, this, &MainWindow::startReply);
    connect(m_view, &ChatView::editRequested, this, &MainWindow::startEdit);
    connect(m_view, &ChatView::deleteRequested, this, &MainWindow::deleteMessage);
    connect(m_view, &ChatView::reactionChosen, this, &MainWindow::sendReaction);
    connect(m_view, &ChatView::moreReactionsRequested, this, [this](int idx, const QPoint &gp) {
        m_pickAction = [this, idx](const QString &e) { sendReaction(idx, e); };
        m_picker->setGifsAvailable(false); // a GIF cannot be a reaction
        m_picker->popup(gp);
    });
    connect(m_view, &ChatView::attachmentActivated, this, &MainWindow::openAttachment);
    connect(m_view, &ChatView::animatedAttachmentRequested, this, &MainWindow::loadAnimatedAttachment);
    connect(m_view, &ChatView::videoPlaybackError, this, [this](const QString &error) {
        statusBar()->showMessage(tr("Video playback failed: %1").arg(error), 10000);
    });
    connect(m_view, &ChatView::linkClicked, this, [this](const QString &url) {
        const QUrl u(url);
        if (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https")) QDesktopServices::openUrl(u);
    });
    connect(m_view, &ChatView::attachmentSaveRequested, this, &MainWindow::saveAttachment);

    // Input actions
    connect(m_input, &MessageInput::returnPressed, this, &MainWindow::sendMessage);
    connect(m_input, &MessageInput::escapePressed, this, [this] { cancelEdit(); });
    connect(m_sendBtn, &QPushButton::clicked, this, &MainWindow::sendMessage);
    connect(m_expandInputBtn, &QToolButton::clicked, this, [this] {
        m_input->setExpanded(!m_input->isExpanded());
    });
    connect(m_input, &MessageInput::expandedChanged, this, [this](bool expanded) {
        m_expandInputBtn->setText(expanded ? QStringLiteral("⌄") : QStringLiteral("⌃"));
        m_expandInputBtn->setToolTip(expanded ? tr("Collapse message field") : tr("Expand message field"));
        m_expandInputBtn->setAccessibleName(expanded ? tr("Collapse message field") : tr("Expand message field"));
        updateSendButton();
        if (expanded) m_input->setFocus();
        positionExpandButton();
        updateEditBar(); // the save hint depends on the mode
    });
    connect(m_input, &MessageInput::upPressed, this, &MainWindow::editLastMessage);
    connect(m_attachBtn, &QToolButton::clicked, this, [this] {
        addPendingFiles(QFileDialog::getOpenFileNames(this, tr("Attach files")));
    });
    connect(m_emojiBtn, &QToolButton::clicked, this, &MainWindow::showComposerPicker);
    connect(m_picker, &EmojiPicker::emojiSelected, this, [this](const QString &e) {
        if (m_pickAction) m_pickAction(e);
    });
    // The GIF tab of the emoji popup: the key is only read from the key store when
    // the tab is actually opened.
    connect(m_picker, &EmojiPicker::giphyKeyRequested, this, [this] {
        QString apiKey;
        QString error;
        if (!GiphyApiKeyStore::load(apiKey, error)) apiKey.clear();
        m_picker->setGiphyApiKey(apiKey, apiKey.isEmpty() ? error : QString());
    });
    connect(m_picker, &EmojiPicker::giphyConfigureRequested, this, [this] {
        configureGiphyApiKey();
        showComposerPicker(); // back to the GIF tab, now with the new key
    });
    connect(m_picker, &EmojiPicker::gifSelected, this, [this](const QUrl &url, const QUrl &analytics) {
        if (m_current.isEmpty() || !m_convs.contains(m_current)) {
            statusBar()->showMessage(tr("Open a conversation before sending a GIF."), 5000);
            return;
        }
        downloadAndSendGif(m_current, url, analytics);
    });
    // Ctrl+V / Shift+Insert / context menu: screenshots and copied files become attachments
    m_input->setPasteHook([this](const QMimeData *md) { return attachFromMimeData(md); });

    // Tray icon, notifications, sound
    m_notify = NotificationSettings::load();
    setWindowIcon(AppIcon::app());
    m_sound = new SoundPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_audioPlayer = new QMediaPlayer(this);
    m_audioPlayer->setAudioOutput(m_audioOutput);
    m_audioPlayer->setObjectName(QStringLiteral("chatAudioPlayer"));
    connect(m_audioPlayer, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        m_audioPositionMs = position;
        updateAudioPlaybackView();
    });
    connect(m_audioPlayer, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        m_audioDurationMs = duration;
        updateAudioPlaybackView();
    });
    connect(m_audioPlayer, &QMediaPlayer::playbackStateChanged, this, [this] {
        updateAudioPlaybackView();
    });
    connect(m_audioPlayer, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &error) {
        m_audioLoading = false;
        statusBar()->showMessage(tr("Audio playback failed: %1").arg(error), 10000);
        updateAudioPlaybackView();
    });
    m_notifier = new Notifier(this);
    m_tray = new QSystemTrayIcon(AppIcon::tray(0), this);
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &MainWindow::applyTheme);
    m_trayMenu = new QMenu(this);
    m_trayMenu->addAction(tr("Show / hide window"), this, &MainWindow::toggleWindow);
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(tr("Quit"), this, &MainWindow::quitApplication);
    m_tray->setContextMenu(m_trayMenu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) toggleWindow();
    });
    m_notifier->setTrayIcon(m_tray);
    // Contacts change their picture at any time and signal-cli does not announce it: look again regularly.
    m_avatarRefreshTimer.setInterval(15 * 60 * 1000);
    connect(&m_avatarRefreshTimer, &QTimer::timeout, this, [this] {
        m_avatarTried.clear(); // keeps the shown pictures; they are only replaced when a result arrives
        fetchMissingAvatars();
    });
    m_avatarRefreshTimer.start();
    connect(m_notifier, &Notifier::activated, this, &MainWindow::activateConversation);
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this] { m_quitting = true; }); // session end
    applyNotificationSettings();
    installInputWatcher();

    // signal-cli
    connect(&m_cli, &SignalCli::envelopeReceived, this, &MainWindow::onEnvelope);
    connect(&m_cli, &SignalCli::stopped, this, [this](const QString &reason) { setConnected(false, reason); });
    connect(&m_cli, &SignalCli::logLine, this, [this](const QString &l) { statusBar()->showMessage(l, 5000); });

    m_expirationTimer.setInterval(1000);
    connect(&m_expirationTimer, &QTimer::timeout, this, &MainWindow::expireMessages);
    m_expirationTimer.start();

    // The handle is invisible (and so cannot receive hover events itself, nor steal clicks from the
    // chat view), so the mouse position is polled to show it when the pointer gets near. The timer
    // only runs while the pointer is inside the right-hand panel (see eventFilter()), so nothing
    // wakes up while the window is idle, minimized or in the tray.
    m_expandHoverTimer.setInterval(80);
    connect(&m_expandHoverTimer, &QTimer::timeout, this, &MainWindow::positionExpandButton);

    setConnected(false);
    QSettings s;
    restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
    QTimer::singleShot(0, this, &MainWindow::initialise);

    // The window must never resize itself: showing the edit / reply / file bars or expanding the
    // input field raises the layout's minimum size, and Qt would then grow a small window to fit.
    // Fix the floor at the minimum of the plain layout (bars hidden, compact input) and stop the
    // layout from raising it later.
    QTimer::singleShot(0, this, [this] {
        setMinimumSize(minimumSizeHint());
        if (layout()) layout()->setSizeConstraint(QLayout::SetNoConstraint);
    });
}

namespace {
// Remembers when the user last used keyboard or mouse in the main window. Qt reports a window as
// "active" even while another window without keyboard focus (overlay, always-on-top window, …)
// covers it, so activity is the only hint that somebody is really looking at the chat.
class InputWatcher : public QObject
{
public:
    InputWatcher(QWidget *window, qint64 *stamp, QObject *parent) : QObject(parent), m_window(window), m_stamp(stamp) {}

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::KeyPress:
        case QEvent::Wheel:
        case QEvent::TouchBegin: {
            const auto *w = qobject_cast<QWidget *>(watched);
            if (w && w->window() == m_window) *m_stamp = QDateTime::currentMSecsSinceEpoch();
            break;
        }
        default:
            break;
        }
        return false;
    }

private:
    QWidget *m_window;
    qint64 *m_stamp;
};
} // namespace

void MainWindow::installInputWatcher()
{
    qApp->installEventFilter(new InputWatcher(this, &m_lastInputMs, this));
}

bool MainWindow::userIsLooking() const
{
    if (!isVisible() || isMinimized() || !isActiveWindow()) return false;
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) return false;
    // Pointer really above our window (a covering window of another program would be found there instead)?
    const QWidget *under = QApplication::widgetAt(QCursor::pos());
    if (under && under->window() == this) return true;
    // Otherwise the user must have been working in this window a moment ago.
    return QDateTime::currentMSecsSinceEpoch() - m_lastInputMs < 30 * 1000;
}

bool MainWindow::event(QEvent *event)
{
    const bool result = QMainWindow::event(event);
    // The chat colours follow the palette (and so does the icon with an unknown colour scheme).
    if (event->type() == QEvent::ApplicationPaletteChange) applyTheme();
    if (event->type() == QEvent::WindowActivate) {
        m_lastInputMs = QDateTime::currentMSecsSinceEpoch();
        // Messages that arrived in the open chat while the window was covered are read now.
        const auto cur = m_convs.find(m_current);
        if (cur != m_convs.end() && cur->unread > 0) {
            cur->unread = 0;
            m_notifier->close(m_current);
            refreshList();
        }
        // When returning to the app (e.g. via Alt+Tab), put the caret back into
        // the composer so typing can start immediately. Do this after Qt has
        // finished activation so another child widget cannot steal the focus.
        QTimer::singleShot(0, this, [this] {
            if (!isVisible() || !isActiveWindow() || !m_searchBar || m_searchBar->isVisible()) return;
            if (QApplication::activeModalWidget() || QApplication::activePopupWidget()) return;
            if (!m_input || !m_input->isEnabled() || m_current.isEmpty()) return;
            // Qt already restores the last focused widget on activation. Calling
            // setFocus() on a widget that has focus again only confuses the input
            // method (caret blinks, keys are lost), so leave it alone then.
            if (QApplication::focusWidget() == m_input) return;
            m_input->setFocus(Qt::ActiveWindowFocusReason);
        });
    }
    return result;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->matches(QKeySequence::Find)) {
            openMessageSearch();
            return true;
        }
        if (watched == m_searchEdit && keyEvent->key() == Qt::Key_Escape) {
            closeMessageSearch();
            return true;
        }
    }
    if (m_savedToast && watched == m_savedToast->parentWidget() && event->type() == QEvent::Resize)
        positionSavedToast();
    // m_input / m_expandInputBtn are QPointers (null until built, null again once
    // destroyed), because this filter is already active on m_view while the
    // constructor is still running.
    if (m_expandInputBtn && m_input
        && (watched == m_input.data() || watched == m_expandInputBtn->parentWidget())) {
        switch (event->type()) {
        case QEvent::Enter:
            // Pointer entered the panel. Moving on to a child (field, handle) does not leave it.
            if (watched == m_expandInputBtn->parentWidget()) {
                m_expandHoverTimer.start();
                positionExpandButton();
            }
            break;
        case QEvent::Leave:
            if (watched == m_expandInputBtn->parentWidget()) {
                m_expandHoverTimer.stop();
                m_expandInputBtn->hide();
            }
            break;
        case QEvent::FocusIn:
        case QEvent::FocusOut:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::LayoutRequest:
        case QEvent::EnabledChange:
            // Defer so the layout has settled before we read the field's geometry.
            QTimer::singleShot(0, this, &MainWindow::positionExpandButton);
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// Opens the file manager with the saved file selected (or at least its folder).
static void revealInFolder(const QString &path)
{
    const QFileInfo fi(path);
#if defined(Q_OS_WIN)
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,") + QDir::toNativeSeparators(fi.absoluteFilePath())});
#elif defined(Q_OS_MACOS)
    QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), fi.absoluteFilePath()});
#else
#ifdef HAVE_QT_DBUS
    QDBusInterface fm(QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
                      QStringLiteral("org.freedesktop.FileManager1"), QDBusConnection::sessionBus());
    if (fm.isValid()) {
        fm.asyncCall(QStringLiteral("ShowItems"), QStringList{QUrl::fromLocalFile(fi.absoluteFilePath()).toString()},
                     QString());
        return;
    }
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
#endif
}

void MainWindow::showSavedToast(const QString &path)
{
    if (!m_expandInputBtn) return;
    QWidget *panel = m_expandInputBtn->parentWidget(); // the right-hand chat panel
    m_savedToastPath = path;
    if (!m_savedToast) {
        auto *toast = new QFrame(panel);
        toast->setObjectName(QStringLiteral("savedToast"));
        toast->setStyleSheet(QStringLiteral(
            "#savedToast { background: palette(window); border: 1px solid palette(mid); border-radius: 8px; }"
            "#savedToast QLabel { color: palette(window-text); }"
            "#savedToast QPushButton { border: none; background: transparent; color: palette(link);"
            " padding: 2px 4px; }"
            "#savedToast QPushButton:hover { text-decoration: underline; }"));
        auto *lay = new QHBoxLayout(toast);
        lay->setContentsMargins(14, 8, 10, 8);
        lay->setSpacing(10);
        lay->addWidget(new QLabel(tr("Attachment saved"), toast));
        auto *show = new QPushButton(tr("Show in folder"), toast);
        show->setCursor(Qt::PointingHandCursor);
        show->setFocusPolicy(Qt::NoFocus); // must not take focus from the message field
        lay->addWidget(show);
        connect(show, &QPushButton::clicked, this, [this] {
            revealInFolder(m_savedToastPath);
            if (m_savedToast) m_savedToast->hide();
        });
        m_savedToast = toast;
        m_savedToastTimer.setSingleShot(true);
        m_savedToastTimer.setInterval(5000);
        connect(&m_savedToastTimer, &QTimer::timeout, this, [this] {
            if (m_savedToast) m_savedToast->hide();
        });
    }
    m_savedToast->adjustSize();
    positionSavedToast();
    m_savedToast->show();
    m_savedToast->raise();
    m_savedToastTimer.start(); // a new save restarts the countdown
}

void MainWindow::positionSavedToast()
{
    if (!m_savedToast || !m_input) return;
    QWidget *panel = m_savedToast->parentWidget();
    m_savedToast->adjustSize();
    const int inputTop = m_input->mapTo(panel, QPoint(0, 0)).y();
    const int x = (panel->width() - m_savedToast->width()) / 2;
    const int y = qMax(0, inputTop - m_savedToast->height() - 12);
    m_savedToast->move(qMax(0, x), y);
}

// The Send button is shown while the message field is expanded, or always if the
// appearance option says so.
void MainWindow::updateSendButton()
{
    if (!m_sendBtn || !m_input) return;
    m_sendBtn->setVisible(m_appearance.alwaysShowSend || m_input->isExpanded());
    positionExpandButton(); // the field's width changes with the button
}

void MainWindow::positionExpandButton()
{
    if (!m_expandInputBtn || !m_input) return;
    QWidget *panel = m_expandInputBtn->parentWidget();
    if (!m_input->isEnabled() || !m_input->isVisible() || !isVisible() || isMinimized()) {
        m_expandInputBtn->hide();
        m_expandHoverTimer.stop();
        return;
    }
    const QPoint topLeft = m_input->mapTo(panel, QPoint(0, 0));
    const int x = topLeft.x() + (m_input->width() - m_expandInputBtn->width()) / 2;
    // Hover half over the chat view, half over the field's top edge.
    const int y = qMax(0, topLeft.y() - m_expandInputBtn->height() / 2);
    const QRect spot(QPoint(x, y), m_expandInputBtn->size());
    const QRect field(topLeft, m_input->size());

    // Shown only while the mouse is over the message field or over the spot of the handle.
    // A window or menu covering the panel at the pointer does not count.
    const QPoint globalPos = QCursor::pos();
    const QPoint pos = panel->mapFromGlobal(globalPos);
    QWidget *under = QApplication::widgetAt(globalPos);
    const bool hover = (spot.contains(pos) || field.contains(pos))
                       && under && (under == panel || panel->isAncestorOf(under));

    if (!hover) {
        m_expandInputBtn->hide();
        // Pointer outside the panel (e.g. Leave got lost): stop polling until the next Enter.
        if (!panel->rect().contains(pos)) m_expandHoverTimer.stop();
        return;
    }
    m_expandInputBtn->move(x, y);
    if (m_expandInputBtn->isHidden()) m_expandInputBtn->show();
    m_expandInputBtn->raise();
}

bool MainWindow::shouldHideOnClose(bool trayVisible, bool closeToTray, bool quitting)
{
    return trayVisible && closeToTray && !quitting;
}

bool MainWindow::shouldStartMinimizedInTray(bool trayVisible, bool startMinimizedInTray)
{
    return trayVisible && startMinimizedInTray;
}

void MainWindow::showAtStartup()
{
    if (shouldStartMinimizedInTray(m_tray && m_tray->isVisible(), m_notify.startMinimizedInTray)) {
        QApplication::setQuitOnLastWindowClosed(false);
        return;
    }
    show();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    QSettings().setValue(QStringLiteral("geometry"), saveGeometry());
    if (shouldHideOnClose(m_tray && m_tray->isVisible(), m_notify.closeToTray, m_quitting)) {
        // Keep running in the tray. A hidden main window must not quit the app when a
        // dialog closes, so "quit on last window closed" is off while hidden (see showEvent).
        QApplication::setQuitOnLastWindowClosed(false);
        e->ignore();
        hide();
        QSettings s;
        if (!s.value(QStringLiteral("trayHintShown")).toBool()) {
            s.setValue(QStringLiteral("trayHintShown"), true);
            m_tray->showMessage(tr("Corvus is still running"),
                                tr("Click the tray icon to open it again. Use the tray menu or File → Quit to quit."),
                                QSystemTrayIcon::NoIcon, 7000);
        }
        return;
    }
    m_cli.stop();
    e->accept();
}

void MainWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    QApplication::setQuitOnLastWindowClosed(true);
    // Mark the selected chat read again when the window is shown from the tray.
    if (!m_current.isEmpty()) {
        startIncomingExpirationTimers(m_current);
        auto it = m_convs.find(m_current);
        if (it != m_convs.end() && it->unread > 0) {
            it->unread = 0;
            refreshList();
        }
    }
}

void MainWindow::quitApplication()
{
    m_quitting = true;
    if (isVisible()) close(); // closeEvent accepts instead of hiding to the tray
    m_cli.stop();
    QCoreApplication::quit();
}

void MainWindow::toggleWindow()
{
    if (isVisible() && !isMinimized() && isActiveWindow()) {
        close(); // goes through closeEvent: hides to the tray
    } else {
        showNormal();
        raise();
        activateWindow();
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasImage()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    if (attachFromMimeData(e->mimeData())) e->acceptProposedAction();
}

QString MainWindow::displayName(const QString &key) const
{
    const QString r = resolveKey(key);
    if (!m_account.isEmpty() && (key == m_account || r == m_account)) return tr("You");
    auto it = m_convs.constFind(r);
    return it != m_convs.constEnd() ? it->title : key;
}

// ---------------------------------------------------------------- Start

void MainWindow::initialise()
{
    if (!unlockHistory()) {
        close();
        return;
    }
    if (!ensureExecutable()) return;
    connectAccount(); // asynchronous: version, accounts, then the update hint
}

bool MainWindow::ensureExecutable()
{
    QSettings s;
    QString exe = s.value(QStringLiteral("executable")).toString();
    if (exe.isEmpty() || !QFileInfo::exists(exe)) exe = SignalCli::autodetect();

    while (exe.isEmpty()) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("signal-cli not found"));
        box.setText(tr("signal-cli is not installed or not in PATH.\n\n"
                       "This GUI requires a separately installed signal-cli."));
        QPushButton *dl = box.addButton(tr("Open download page"), QMessageBox::HelpRole);
        QPushButton *pick = box.addButton(tr("Choose path …"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Close);
        box.exec();
        if (box.clickedButton() == dl) {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/AsamK/signal-cli/releases")));
        } else if (box.clickedButton() == pick) {
            exe = QFileDialog::getOpenFileName(this, tr("Select signal-cli"));
        } else {
            return false;
        }
    }
    s.setValue(QStringLiteral("executable"), exe);
    m_cli.setExecutable(exe);
    return true;
}

void MainWindow::chooseExecutable()
{
    const QString exe = QFileDialog::getOpenFileName(this, tr("Select signal-cli"), m_cli.executable());
    if (exe.isEmpty()) return;
    QSettings().setValue(QStringLiteral("executable"), exe);
    m_cli.setExecutable(exe);
    connectAccount();
}

void MainWindow::connectAccount()
{
    // Everything here is asynchronous so the window never freezes while the JVM
    // behind signal-cli starts. The version is read on every (re)connect, so an
    // update of signal-cli is noticed without restarting the GUI.
    const int gen = ++m_connectGen;
    m_cli.stop();
    setConnected(false);
    m_statusLabel->setText(tr("Connecting …"));

    m_cli.queryVersion([this, gen](const SignalCli::Result &r, const QString &version) {
        if (gen != m_connectGen) return;
        if (!r.ok) {
            showCliProblem(tr("signal-cli could not be run."), r);
            return;
        }
        m_version = version.section(QLatin1Char(' '), -1);
        syncSettingsDialog();

        m_cli.queryAccounts([this, gen](const SignalCli::Result &r, const QStringList &accounts) {
            if (gen != m_connectGen) return;
            // A failing command is NOT "no account": report what actually went wrong.
            if (!r.ok && accounts.isEmpty()) {
                showCliProblem(tr("The accounts could not be listed."), r);
                return;
            }
            if (accounts.isEmpty()) {
                setConnected(false, tr("no account linked"));
                if (QMessageBox::question(this, tr("No account"),
                                          tr("signal-cli reports that no account is set up yet.\n"
                                             "Link this device with your phone now?"))
                        == QMessageBox::Yes &&
                    gen == m_connectGen)
                    linkDevice();
                return;
            }

            QSettings s;
            m_account = s.value(QStringLiteral("account")).toString();
            if (!accounts.contains(m_account)) {
                if (accounts.size() == 1) {
                    m_account = accounts.first();
                } else {
                    bool ok = false;
                    m_account = QInputDialog::getItem(this, tr("Choose account"), tr("Account:"), accounts, 0, false, &ok);
                    if (!ok) {
                        setConnected(false, tr("no account chosen"));
                        return;
                    }
                }
                s.setValue(QStringLiteral("account"), m_account);
            }
            if (gen != m_connectGen) return; // reconnect requested while a dialog was open

            m_view->setSelfKey(m_account);
            m_cli.start(m_account);
            setConnected(true);
            loadContactsAndGroups();
            checkForUpdates(true); // silent: only a hint in the status bar
        });
    });
}

void MainWindow::showCliProblem(const QString &title, const SignalCli::Result &r)
{
    setConnected(false, title);
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("signal-cli problem"));
    box.setText(title + QStringLiteral("\n\n") + r.describe());
    box.setInformativeText(tr("Command: %1\n\nThis does not mean that no account is linked. Check that the command "
                              "works in a terminal (the non-native signal-cli build needs Java).")
                               .arg(SignalCli::maskNumbers(r.command)));
    QPushButton *retry = box.addButton(tr("Retry"), QMessageBox::AcceptRole);
    QPushButton *pick = box.addButton(tr("Choose signal-cli …"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (box.clickedButton() == retry)
        QTimer::singleShot(0, this, &MainWindow::connectAccount);
    else if (box.clickedButton() == pick)
        chooseExecutable();
}

void MainWindow::reconnect()
{
    connectAccount();
}

void MainWindow::linkDevice()
{
    if (m_cli.executable().isEmpty() && !ensureExecutable()) return;
    m_cli.stop();
    setConnected(false);
    LinkDialog dlg(m_cli.executable(), this);
    if (dlg.exec() == QDialog::Accepted) {
        QSettings().remove(QStringLiteral("account"));
        connectAccount();
    }
}

void MainWindow::setConnected(bool connected, const QString &info)
{
    QTimer::singleShot(0, this, [this] { syncSettingsDialog(); });
    m_input->setEnabled(connected);
    m_expandInputBtn->setEnabled(connected);
    m_sendBtn->setEnabled(connected);
    m_attachBtn->setEnabled(connected);
    m_emojiBtn->setEnabled(connected);
    if (connected) {
        m_statusLabel->setText(tr("Connected · signal-cli %1").arg(m_version));
    } else {
        m_statusLabel->setText(info.isEmpty() ? tr("Not connected") : tr("Disconnected: %1").arg(info));
    }
}

// ----------------------------------------------------- Contacts / groups

void MainWindow::loadContactsAndGroups()
{
    m_avatarTried.clear(); // ask signal-cli again, avatars may have changed
    m_avatarBusy.clear();
    m_avatarInFlight = 0;
    m_cli.call(QStringLiteral("listContacts"), {}, [this](const QJsonValue &res, const QString &err) {
        if (!err.isEmpty()) {
            statusBar()->showMessage(tr("Could not load contacts: %1").arg(err), 8000);
            return;
        }
        for (const QJsonValue &cv : res.toArray()) {
            const QJsonObject c = cv.toObject();
            const QString number = c.value(QStringLiteral("number")).toString();
            const QString uuid = c.value(QStringLiteral("uuid")).toString();
            const QString key = !number.isEmpty() ? number : uuid;
            if (key.isEmpty()) continue;
            if (!number.isEmpty() && !uuid.isEmpty()) m_alias.insert(uuid, number);

            QString title = c.value(QStringLiteral("name")).toString();
            if (title.isEmpty()) title = c.value(QStringLiteral("profileName")).toString();
            if (!title.isEmpty()) m_contactNames.insert(key, title);

            // signal-cli lists every known recipient (address book, group members, …), not just
            // people we have chatted with. Only show a chat once there is one: local history,
            // a new message, or an archived chat on the phone.
            const bool sig = c.value(QStringLiteral("isArchived")).toBool();
            if (c.value(QStringLiteral("isHidden")).toBool()) continue;
            if (!m_convs.contains(key) && !sig) continue;

            Conversation &cv2 = conv(key, key);
            const int expiration = c.value(QStringLiteral("messageExpirationTime")).toInt();
            if (cv2.expirationSeconds != expiration) {
                cv2.expirationSeconds = expiration;
                persist(Records::expiration(key, expiration));
            }
            if (title.isEmpty()) title = cv2.storedName.isEmpty() ? key : cv2.storedName;
            cv2.title = cv2.localNickname.isEmpty() ? title : cv2.localNickname;

            // Archive state: follow signal-cli only when *it* changed (e.g. archived on the phone);
            // otherwise keep the local state (e.g. auto-unarchived by a new message).
            if (sig != cv2.sigArchived) {
                cv2.sigArchived = sig;
                cv2.archived = sig;
                persist(Records::archive(key, cv2.archived, sig));
            }
        }
        refreshList();
        checkIdentities(); // any safety number that changed while Corvus was not running
    });

    m_cli.call(QStringLiteral("listGroups"), {}, [this](const QJsonValue &res, const QString &err) {
        if (!err.isEmpty()) return;
        for (const QJsonValue &gv : res.toArray()) {
            const QJsonObject g = gv.toObject();
            if (g.contains(QStringLiteral("isMember")) && !g.value(QStringLiteral("isMember")).toBool()) continue;
            const QString id = g.value(QStringLiteral("id")).toString();
            if (id.isEmpty()) continue;
            QString title = g.value(QStringLiteral("name")).toString();
            if (title.isEmpty()) title = tr("Group");
            Conversation &c = conv(QStringLiteral("g:") + id, title, true);
            c.title = title;
            const int expiration = g.value(QStringLiteral("messageExpirationTime")).toInt();
            if (c.expirationSeconds != expiration) {
                c.expirationSeconds = expiration;
                persist(Records::expiration(c.key, expiration));
            }
        }
        refreshList();
    });
}

// ------------------------------------------------- Encrypted history

QString MainWindow::historyPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/history.corvus");
}

bool MainWindow::unlockHistory()
{
    const QString path = historyPath();
    const bool exists = HistoryStore::exists(path);
    QSettings settings;
    m_historyProtectionMode = settings.value(QStringLiteral("historyProtectionMode")).toString();
    if (exists && m_historyProtectionMode.isEmpty())
        m_historyProtectionMode = QStringLiteral("passphrase");
    if (!exists && m_historyProtectionMode.isEmpty()) {
        m_historyProtectionMode = QStringLiteral("keychain");
        QMessageBox choice(this);
        choice.setWindowTitle(tr("Protect chat history"));
        QPushButton *keychain = nullptr;
        if (HistoryKeychain::isAvailable()) {
            choice.setText(tr("Choose how to protect your new chat history."));
            keychain = choice.addButton(tr("System keychain (recommended)"), QMessageBox::AcceptRole);
        } else {
            choice.setText(tr("No system keychain is available. Choose a passphrase to protect your chat history."));
        }
        QPushButton *passphrase = choice.addButton(tr("Use a passphrase"), QMessageBox::ActionRole);
        choice.addButton(QMessageBox::Cancel);
        choice.setDefaultButton(keychain ? keychain : passphrase);
        choice.exec();
        if (choice.clickedButton() == passphrase)
            m_historyProtectionMode = QStringLiteral("passphrase");
        else if (!keychain || choice.clickedButton() != keychain)
            return false;
    }

    if (exists && settings.value(QStringLiteral("historyProtectionMode")).toString().isEmpty()) {
        QString secret;
        QString error;
        if (HistoryKeychain::isAvailable() && HistoryKeychain::load(secret, error)) {
            QJsonArray records;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const int rc = m_store.open(path, secret, &records);
            QApplication::restoreOverrideCursor();
            if (rc == SC_OK) {
                m_historyProtectionMode = QStringLiteral("keychain");
                settings.setValue(QStringLiteral("historyProtectionMode"), m_historyProtectionMode);
                settings.sync();
                loadHistory(records);
                return true;
            }
        }
        m_historyProtectionMode = QStringLiteral("passphrase");
    }

    if (m_historyProtectionMode == QLatin1String("keychain")) {
        QString secret;
        QString error;
        if (!HistoryKeychain::isAvailable()) {
            QMessageBox::warning(this, tr("Chat history"),
                                 tr("A system keychain is required to protect this history."));
            return false;
        }

        if (exists) {
            if (!HistoryKeychain::load(secret, error)) {
                QMessageBox::critical(this, tr("Chat history"),
                                      tr("Could not read the history key from the system keychain:\n%1").arg(error));
                return false;
            }
            QApplication::setOverrideCursor(Qt::WaitCursor);
            QJsonArray records;
            const int rc = m_store.open(path, secret, &records);
            QApplication::restoreOverrideCursor();
            if (rc != SC_OK) {
                if (rc != SC_ERR_WRONG_PASSPHRASE) {
                    QMessageBox::critical(this, tr("Chat history"), HistoryStore::errorText(rc));
                    return false;
                }
                error = tr("The keychain key did not unlock this history. Try its previous passphrase if protection was just changed.");
                m_historyProtectionMode = QStringLiteral("passphrase");
            }
            if (rc == SC_OK) {
                loadHistory(records);
                return true;
            }
        } else {
            secret = HistoryKeychain::generateSecret();
            if (!HistoryKeychain::save(secret, error)) {
                QMessageBox::critical(this, tr("Chat history"),
                                      tr("Could not save the history key in the system keychain:\n%1").arg(error));
                return false;
            }
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const int rc = m_store.create(path, secret);
            QApplication::restoreOverrideCursor();
            if (rc != SC_OK) {
                HistoryKeychain::clear(error);
                QMessageBox::critical(this, tr("Chat history"), HistoryStore::errorText(rc));
                return false;
            }
            settings.setValue(QStringLiteral("historyProtectionMode"), m_historyProtectionMode);
            settings.sync();
            if (settings.status() != QSettings::NoError) {
                QMessageBox::warning(this, tr("Chat history"),
                                     tr("Could not save the history protection setting."));
            }
            return true;
        }
    }

    QString error;
    for (;;) {
        PassphraseDialog dlg(exists ? PassphraseDialog::Unlock : PassphraseDialog::Create, this);
        if (!error.isEmpty()) dlg.setError(error);
        if (dlg.exec() != QDialog::Accepted) return false;

        QApplication::setOverrideCursor(Qt::WaitCursor); // key derivation takes a moment
        QJsonArray records;
        const int rc = exists ? m_store.open(path, dlg.passphrase(), &records) : m_store.create(path, dlg.passphrase());
        QApplication::restoreOverrideCursor();

        if (rc == SC_OK) {
            m_historyProtectionMode = QStringLiteral("passphrase");
            settings.setValue(QStringLiteral("historyProtectionMode"), m_historyProtectionMode);
            settings.sync();
            if (settings.status() != QSettings::NoError) {
                QMessageBox::warning(this, tr("Chat history"),
                                     tr("Could not save the history protection setting."));
            }
            loadHistory(records);
            return true;
        }
        if (rc == SC_ERR_WRONG_PASSPHRASE) {
            error = HistoryStore::errorText(rc);
            continue;
        }
        QMessageBox::critical(this, tr("Chat history"),
                              tr("%1\n\nFile: %2\n\nMove or delete the file to start with a new history.")
                                  .arg(HistoryStore::errorText(rc), path));
        return false;
    }
}

void MainWindow::loadHistory(const QJsonArray &records)
{
    for (const QJsonValue &v : records) {
        const QJsonObject o = v.toObject();
        const QString t = o.value(QStringLiteral("t")).toString();
        const QString key = o.value(QStringLiteral("c")).toString();
        if (key.isEmpty()) continue;
        const bool group = key.startsWith(QLatin1String("g:"));

        if (t == QLatin1String("m")) {
            Conversation &c = conv(key, group ? tr("Group") : key, group);
            const Message m = Records::messageFromRecord(o);
            int at = c.msgs.size();
            while (at > 0 && c.msgs[at - 1].sortTs() > m.sortTs()) --at;
            c.msgs.insert(at, m);
        } else if (t == QLatin1String("n")) {
            Conversation &c = conv(key, key, group);
            c.storedName = o.value(QStringLiteral("n")).toString();
            if (!c.storedName.isEmpty() && c.localNickname.isEmpty()) c.title = c.storedName;
        } else if (t == QLatin1String("p")) {
            Conversation &c = conv(key, key, group);
            c.localNickname = o.value(QStringLiteral("n")).toString();
            if (!c.localNickname.isEmpty()) c.title = c.localNickname;
            else c.title = m_contactNames.value(key, c.storedName.isEmpty() ? key : c.storedName);
        } else if (t == QLatin1String("d")) {
            m_convs.remove(key);
        } else if (t == QLatin1String("u") || t == QLatin1String("r")) {
            auto it = m_convs.find(key);
            if (it == m_convs.end()) continue;
            const QString au = o.value(QStringLiteral("au")).toString();
            const int i = findMessage(it.value(), o.value(QStringLiteral("ts")).toVariant().toLongLong(),
                                      au.isEmpty() ? QStringList() : QStringList{au});
            if (i < 0) continue;
            Message &m = it.value().msgs[i];
            if (t == QLatin1String("u")) {
                Records::applyExtras(m, o.value(QStringLiteral("d")).toObject());
            } else {
                const QString by = o.value(QStringLiteral("by")).toString();
                const QString e = o.value(QStringLiteral("e")).toString();
                if (e.isEmpty()) m.reactions.remove(by); else m.reactions[by] = e;
            }
        } else if (t == QLatin1String("k")) {
            conv(key, key, group).muted = o.value(QStringLiteral("v")).toBool();
        } else if (t == QLatin1String("e")) {
            conv(key, key, group).expirationSeconds = o.value(QStringLiteral("v")).toInt();
        } else if (t == QLatin1String("a")) {
            Conversation &c = conv(key, key, group);
            c.archived = o.value(QStringLiteral("v")).toBool();
            c.sigArchived = o.value(QStringLiteral("s")).toBool();
        }
    }
    refreshList();
}

QJsonArray MainWindow::allRecords() const
{
    QJsonArray out;
    QStringList keys = m_convs.keys();
    keys.sort();
    for (const QString &k : keys) {
        const Conversation &c = m_convs[k];
        if (!c.storedName.isEmpty()) out.append(Records::name(c.key, c.storedName));
        if (!c.localNickname.isEmpty()) out.append(Records::nickname(c.key, c.localNickname));
        if (c.archived || c.sigArchived) out.append(Records::archive(c.key, c.archived, c.sigArchived));
        if (c.muted) out.append(Records::mute(c.key, true));
        out.append(Records::expiration(c.key, c.expirationSeconds));
        for (const Message &m : c.msgs) {
            bool pending = false;
            for (auto it = m_pendingSends.cbegin(); it != m_pendingSends.cend(); ++it) {
                if (it->conversationKey == c.key && it->temporaryTimestamp == m.ts()) {
                    pending = true;
                    break;
                }
            }
            if (pending) continue;
            out.append(Records::message(c.key, m));
            for (auto it = m.reactions.cbegin(); it != m.reactions.cend(); ++it)
                out.append(Records::reaction(c.key, m.ts(), effectiveAuthor(c, m), it.key(), it.value()));
        }
    }
    return out;
}

void MainWindow::persist(const QJsonObject &record)
{
    const int rc = m_store.append(record);
    if (rc != SC_OK)
        statusBar()->showMessage(tr("Could not save message: %1").arg(HistoryStore::errorText(rc)), 10000);
}

void MainWindow::learnName(const QString &key, const QString &name)
{
    Conversation &c = conv(key);
    if (!c.storedName.isEmpty() || c.isGroup) return;
    if (c.title != c.key && c.title != name && c.localNickname.isEmpty()) return; // a saved contact name wins
    c.storedName = name;
    if (c.localNickname.isEmpty()) c.title = name;
    persist(Records::name(key, name));
    refreshList();
}

void MainWindow::setNickname(const QString &key, const QString &nickname)
{
    auto it = m_convs.find(key);
    if (it == m_convs.end() || it->isGroup) return;
    it->localNickname = nickname;
    it->title = nickname.isEmpty()
                    ? m_contactNames.value(key, it->storedName.isEmpty() ? key : it->storedName)
                    : nickname;
    persist(Records::nickname(key, nickname));
    refreshList();
    if (m_current == key) showConversation(true);
}

void MainWindow::changeHistoryProtection()
{
    QMessageBox choice(this);
    choice.setWindowTitle(tr("Chat history protection"));
    choice.setText(tr("Choose how to protect the encrypted chat history."));
    QPushButton *keychain = choice.addButton(tr("System keychain"), QMessageBox::AcceptRole);
    QPushButton *passphrase = choice.addButton(tr("Passphrase"), QMessageBox::ActionRole);
    choice.addButton(QMessageBox::Cancel);
    choice.setDefaultButton(m_historyProtectionMode == QLatin1String("keychain") ? keychain : passphrase);
    choice.exec();

    const bool useKeychain = choice.clickedButton() == keychain;
    if (!useKeychain && choice.clickedButton() != passphrase) return;
    if (useKeychain == (m_historyProtectionMode == QLatin1String("keychain"))) return;

    QString error;
    if (useKeychain) {
        if (!HistoryKeychain::isAvailable()) {
            QMessageBox::warning(this, tr("Chat history"),
                                 tr("No system keychain is available."));
            return;
        }
        for (;;) {
            PassphraseDialog oldDialog(PassphraseDialog::Unlock, this);
            if (!error.isEmpty()) oldDialog.setError(error);
            if (oldDialog.exec() != QDialog::Accepted) return;

            const QString newSecret = HistoryKeychain::generateSecret();
            if (!HistoryKeychain::save(newSecret, error)) {
                QMessageBox::critical(this, tr("Chat history"), error);
                return;
            }
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const int rc = m_store.changePassphrase(oldDialog.passphrase(), newSecret);
            QApplication::restoreOverrideCursor();
            if (rc == SC_ERR_WRONG_PASSPHRASE) {
                HistoryKeychain::clear(error);
                error = HistoryStore::errorText(rc);
                continue;
            }
            if (rc != SC_OK) {
                HistoryKeychain::clear(error);
                QMessageBox::critical(this, tr("Chat history"), HistoryStore::errorText(rc));
                return;
            }

            QSettings settings;
            settings.setValue(QStringLiteral("historyProtectionMode"), QStringLiteral("keychain"));
            settings.sync();
            if (settings.status() != QSettings::NoError) {
                const int rollbackRc = m_store.changePassphrase(newSecret, oldDialog.passphrase());
                if (rollbackRc != SC_OK) {
                    m_historyProtectionMode = QStringLiteral("keychain");
                    settings.setValue(QStringLiteral("historyProtectionMode"), m_historyProtectionMode);
                    settings.sync();
                    QMessageBox::critical(this, tr("Chat history"),
                                          tr("The history key was changed, but the protection setting could not be saved. "
                                             "The keychain entry was retained so the history remains accessible."));
                    return;
                }
                HistoryKeychain::clear(error);
                QMessageBox::critical(this, tr("Chat history"),
                                      tr("Could not save the history protection setting."));
                return;
            }
            m_historyProtectionMode = QStringLiteral("keychain");
            QMessageBox::information(this, tr("Chat history"),
                                     tr("The history is now protected by the system keychain."));
            return;
        }
    }

    QString oldSecret;
    if (m_historyProtectionMode == QLatin1String("keychain")) {
        if (!HistoryKeychain::load(oldSecret, error)) {
            QMessageBox::critical(this, tr("Chat history"), error);
            return;
        }
        PassphraseDialog newDialog(PassphraseDialog::Create, this);
        if (newDialog.exec() != QDialog::Accepted) return;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const int rc = m_store.changePassphrase(oldSecret, newDialog.passphrase());
        QApplication::restoreOverrideCursor();
        if (rc != SC_OK) {
            QMessageBox::critical(this, tr("Chat history"), HistoryStore::errorText(rc));
            return;
        }
        QSettings settings;
        settings.setValue(QStringLiteral("historyProtectionMode"), QStringLiteral("passphrase"));
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            m_store.changePassphrase(newDialog.passphrase(), oldSecret);
            QMessageBox::critical(this, tr("Chat history"),
                                  tr("Could not save the history protection setting."));
            return;
        }
        m_historyProtectionMode = QStringLiteral("passphrase");
        if (!HistoryKeychain::clear(error))
            QMessageBox::warning(this, tr("Chat history"), error);
        QMessageBox::information(this, tr("Chat history"),
                                 tr("The history is now protected by your passphrase."));
        return;
    }

    for (;;) {
        PassphraseDialog dlg(PassphraseDialog::Change, this);
        if (!error.isEmpty()) dlg.setError(error);
        if (dlg.exec() != QDialog::Accepted) return;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const int rc = m_store.changePassphrase(dlg.oldPassphrase(), dlg.passphrase());
        QApplication::restoreOverrideCursor();
        if (rc == SC_OK) {
            QMessageBox::information(this, tr("Chat history"), tr("The passphrase has been changed."));
            return;
        }
        error = HistoryStore::errorText(rc);
        if (rc != SC_ERR_WRONG_PASSPHRASE) {
            QMessageBox::critical(this, tr("Chat history"), error);
            return;
        }
    }
}

void MainWindow::deleteConversation(const QString &key)
{
    if (!m_convs.contains(key)) return;
    if (QMessageBox::question(this, tr("Delete conversation"),
                              tr("Delete “%1” and its stored messages from this computer?").arg(m_convs[key].title))
        != QMessageBox::Yes)
        return;
    if (key == m_editKey) cancelEdit();
    m_convs.remove(key);
    m_drafts.remove(key);
    if (key == m_current) {
        m_current.clear();
        m_input->clear();
        m_replyQuote = Quote();
        m_pendingFiles.clear();
        m_pendingGifAnalyticsUrl.clear();
        updateReplyBar();
        updateAttachBar();
    }
    const int rc = m_store.rewrite(allRecords()); // compacts the file, old ciphertext is gone
    if (rc != SC_OK)
        statusBar()->showMessage(tr("Could not update history: %1").arg(HistoryStore::errorText(rc)), 10000);
    refreshList();
    showConversation();
}

void MainWindow::deleteMessage(int idx)
{
    if (!m_convs.contains(m_current) || idx < 0 || idx >= m_convs[m_current].msgs.size()) return;
    const Conversation &conversation = m_convs[m_current];
    const Message message = conversation.msgs[idx];
    for (auto it = m_pendingSends.cbegin(); it != m_pendingSends.cend(); ++it) {
        if (it->conversationKey == conversation.key && it->temporaryTimestamp == message.ts()) {
            statusBar()->showMessage(tr("Wait until the message has finished sending before deleting it."), 6000);
            return;
        }
    }

    const qint64 age = QDateTime::currentMSecsSinceEpoch() - message.ts();
    const bool canDeleteForEveryone = message.outgoing && !message.deleted && age >= 0 && age <= 24LL * 60 * 60 * 1000;
    QMessageBox box(QMessageBox::Question, tr("Delete message"),
                    canDeleteForEveryone ? tr("Delete this message from this device or for everyone?")
                                         : tr("Delete this message from this device?"),
                    QMessageBox::NoButton, this);
    QPushButton *local = box.addButton(tr("Delete for me"), QMessageBox::DestructiveRole);
    QPushButton *remote = canDeleteForEveryone
                              ? box.addButton(tr("Delete for everyone"), QMessageBox::DestructiveRole)
                              : nullptr;
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == local) {
        removeMessage(conversation.key, message.ts());
        return;
    }
    if (box.clickedButton() != remote || !remote) return;

    QJsonObject params{{QStringLiteral("targetTimestamp"), QJsonValue(static_cast<qint64>(message.ts()))}};
    if (conversation.isGroup)
        params[QStringLiteral("groupIds")] = QJsonArray{conversation.key.mid(2)};
    else
        params[QStringLiteral("recipients")] = QJsonArray{resolveKey(conversation.key)};

    const QString key = conversation.key;
    const qint64 timestamp = message.ts();
    m_cli.call(QStringLiteral("remoteDelete"), params, [this, key, timestamp](const QJsonValue &, const QString &err) {
        if (!err.isEmpty()) {
            statusBar()->showMessage(tr("Could not delete message for everyone: %1").arg(err), 10000);
            return;
        }
        // Like the official clients: a placeholder stays where the message was.
        if (!applyDelete(key, timestamp, {})) return;
        statusBar()->showMessage(tr("Message deleted for everyone."), 5000);
    });
}

bool MainWindow::removeMessage(const QString &key, qint64 timestamp)
{
    auto conversation = m_convs.find(key);
    if (conversation == m_convs.end()) return false;
    const int index = findMessage(conversation.value(), timestamp, {});
    if (index < 0) return false;
    const Message removed = conversation->msgs.takeAt(index);
    if (m_editKey == key && m_editTs == removed.ts()) cancelEdit();

    const int rc = m_store.rewrite(allRecords());
    if (rc != SC_OK) {
        int at = conversation->msgs.size();
        while (at > 0 && conversation->msgs[at - 1].sortTs() > removed.sortTs()) --at;
        conversation->msgs.insert(at, removed);
        statusBar()->showMessage(tr("Could not remove message from history: %1").arg(HistoryStore::errorText(rc)), 10000);
        return false;
    }
    if (key == m_current) showConversation(true);
    refreshList();
    return true;
}

// --------------------------------------------------------------- Model

QString MainWindow::resolveKey(const QString &id) const
{
    return m_alias.value(id, id);
}

Conversation &MainWindow::conv(const QString &key, const QString &title, bool group)
{
    auto it = m_convs.find(key);
    if (it == m_convs.end()) {
        Conversation c;
        c.key = key;
        c.title = title.isEmpty() ? key : title;
        if (c.title == key) c.title = m_contactNames.value(key, key); // saved contact name, if known
        c.isGroup = group;
        it = m_convs.insert(key, c);
    }
    return it.value();
}

QString MainWindow::effectiveAuthor(const Conversation &c, const Message &m) const
{
    if (!m.authorKey.isEmpty()) return m.authorKey;
    if (m.outgoing) return m_account;
    return c.isGroup ? QString() : c.key;
}

bool MainWindow::authorMatches(const Conversation &c, const Message &m, const QStringList &authors) const
{
    const QString mk = effectiveAuthor(c, m);
    for (const QString &a : authors)
        if (!a.isEmpty() && (mk == a || mk == resolveKey(a))) return true;
    return false;
}

// Authors empty => match by timestamp only.
int MainWindow::findMessage(const Conversation &c, qint64 ts, const QStringList &authors) const
{
    QStringList nonEmpty;
    for (const QString &a : authors)
        if (!a.isEmpty()) nonEmpty << a;
    for (int i = c.msgs.size() - 1; i >= 0; --i) {
        if (c.msgs[i].ts() != ts && !(c.msgs[i].editTs > 0 && c.msgs[i].editTs == ts)) continue;
        if (nonEmpty.isEmpty() || authorMatches(c, c.msgs[i], nonEmpty)) return i;
    }
    return -1;
}

bool MainWindow::addMessage(const QString &key, Message m, const QString &title, bool group)
{
    Conversation &c = conv(key, title, group);
    if (findMessage(c, m.ts(), QStringList{effectiveAuthor(c, m)}) >= 0) return false; // duplicate
    const qint64 previousOrder = c.msgs.isEmpty() ? 0 : c.msgs.last().sortTs();
    m.orderTs = std::max({m.orderTs, QDateTime::currentMSecsSinceEpoch(), previousOrder + 1});
    if (!m.outgoing && m.expiresInSeconds > 0 && key == m_current && isVisible() && !isMinimized())
        m.expirationStartMs = QDateTime::currentMSecsSinceEpoch();

    int at = c.msgs.size();
    while (at > 0 && c.msgs[at - 1].sortTs() > m.sortTs()) --at;
    c.msgs.insert(at, m);
    persist(Records::message(key, m));

    // Like the official client: activity brings an archived chat back.
    if (c.archived) {
        c.archived = false;
        persist(Records::archive(key, false, c.sigArchived));
    }

    if (key == m_current) {
        if (at == c.msgs.size() - 1)
            m_view->appendMessage(m);
        else
            showConversation(true);
    }
    // A message in the selected chat is only read while the window is visible.
    if (!m.outgoing && !(key == m_current && userIsLooking()))
        c.unread++;
    refreshList();
    if (!m.outgoing && !isActiveWindow()) QApplication::alert(this);
    return true;
}

void MainWindow::finalizePendingSend(const QString &id, qint64 timestamp, const Message *echo)
{
    auto pendingIt = m_pendingSends.find(id);
    if (pendingIt == m_pendingSends.end()) return;

    const PendingSend pending = pendingIt.value();
    if (timestamp <= 0) timestamp = QDateTime::currentMSecsSinceEpoch();

    Message confirmed = pending.message;
    confirmed.time = QDateTime::fromMSecsSinceEpoch(timestamp);
    if (echo && echo->expiresInSeconds > 0) confirmed.expiresInSeconds = echo->expiresInSeconds;
    if (confirmed.expiresInSeconds > 0) confirmed.expirationStartMs = timestamp;
    if (echo) {
        // Keep the local file paths and thumbnails, but copy the remote attachment IDs
        // when signal-cli's own-message envelope reaches us before the RPC reply.
        for (int i = 0; i < confirmed.attachments.size() && i < echo->attachments.size(); ++i) {
            if (!echo->attachments[i].id.isEmpty())
                confirmed.attachments[i].id = echo->attachments[i].id;
        }
    }

    auto conversationIt = m_convs.find(pending.conversationKey);
    if (conversationIt == m_convs.end()) {
        if (echo) pendingIt->echoReceived = true;
        else m_pendingSends.erase(pendingIt);
        addMessage(pending.conversationKey, confirmed);
        return;
    }

    Conversation &conversation = conversationIt.value();
    int previewIndex = -1;
    for (int i = 0; i < conversation.msgs.size(); ++i) {
        if (conversation.msgs[i].ts() == pending.temporaryTimestamp &&
            conversation.msgs[i].outgoing &&
            conversation.msgs[i].authorKey == m_account) {
            previewIndex = i;
            break;
        }
    }

    if (previewIndex < 0) {
        if (echo) pendingIt->echoReceived = true;
        else m_pendingSends.erase(pendingIt);
        addMessage(pending.conversationKey, confirmed);
        return;
    }

    // If an envelope already inserted the confirmed timestamp by another path, remove
    // only our temporary preview rather than creating a duplicate chat bubble.
    int duplicateIndex = -1;
    for (int i = 0; i < conversation.msgs.size(); ++i) {
        if (i == previewIndex) continue;
        const Message &existing = conversation.msgs[i];
        if (existing.ts() == timestamp && existing.outgoing && existing.authorKey == m_account) {
            duplicateIndex = i;
            break;
        }
    }

    if (duplicateIndex >= 0) {
        conversation.msgs.removeAt(previewIndex);
    } else {
        conversation.msgs[previewIndex] = confirmed;
        std::stable_sort(conversation.msgs.begin(), conversation.msgs.end(),
                         [](const Message &a, const Message &b) { return a.sortTs() < b.sortTs(); });
        persist(Records::message(pending.conversationKey, confirmed));
    }

    // Sending a message brings an archived conversation back, just like receiving one.
    if (conversation.archived) {
        conversation.archived = false;
        persist(Records::archive(pending.conversationKey, false, conversation.sigArchived));
    }

    if (echo)
        pendingIt->echoReceived = true; // keep the entry until the corresponding RPC callback
    else
        m_pendingSends.erase(pendingIt);

    if (pending.conversationKey == m_current) showConversation(true);
    refreshList();
}

void MainWindow::removePendingPreview(const PendingSend &pending)
{
    auto conversationIt = m_convs.find(pending.conversationKey);
    if (conversationIt == m_convs.end()) return;

    Conversation &conversation = conversationIt.value();
    for (int i = 0; i < conversation.msgs.size(); ++i) {
        const Message &message = conversation.msgs[i];
        if (message.ts() == pending.temporaryTimestamp && message.outgoing &&
            message.authorKey == m_account) {
            conversation.msgs.removeAt(i);
            break;
        }
    }

    if (pending.conversationKey == m_current) showConversation(true);
    refreshList();
}

bool MainWindow::applyReaction(const QString &key, qint64 ts, const QStringList &authors, const QString &reactor,
                               const QString &emoji)
{
    auto it = m_convs.find(key);
    if (it == m_convs.end()) return false;
    Conversation &c = it.value();
    const int i = findMessage(c, ts, authors);
    if (i < 0) return false;
    Message &m = c.msgs[i];
    if (m.deleted) return false; // nothing to react to any more
    if (emoji.isEmpty()) m.reactions.remove(reactor); else m.reactions[reactor] = emoji;
    persist(Records::reaction(key, m.ts(), effectiveAuthor(c, m), reactor, emoji)); // original ts = stable identity
    if (key == m_current) showConversation(true);
    return true;
}

// Delete for everyone (ours or the author's): the message turns into a placeholder.
// Authors empty => match by timestamp only.
bool MainWindow::applyDelete(const QString &key, qint64 targetTs, const QStringList &authors)
{
    auto it = m_convs.find(key);
    if (it == m_convs.end()) return false;
    Conversation &c = it.value();
    const int i = findMessage(c, targetTs, authors);
    if (i < 0) return false;
    Message &m = c.msgs[i];
    if (m.deleted) return false; // redelivery
    if (m_editKey == key && m_editTs == m.ts()) cancelEdit();
    m.deleted = true;
    m.text.clear();
    m.attachments.clear();
    m.quote = Quote();
    m.reactions.clear();
    m.editTs = 0;
    m.editCount = 0;

    // Rewrite instead of append: the old ciphertext of the message must not stay in the file.
    const int rc = m_store.rewrite(allRecords());
    if (rc != SC_OK) {
        statusBar()->showMessage(tr("Could not remove message from history: %1").arg(HistoryStore::errorText(rc)), 10000);
        persist(Records::deletion(key, m)); // at least keep the placeholder after a restart
    }
    if (key == m_current) showConversation(true);
    refreshList();
    return true;
}

bool MainWindow::applyEdit(const QString &key, qint64 targetTs, const QStringList &authors, const QString &text,
                           qint64 newTs, const QList<TextStyleRange> &textStyles)
{
    auto it = m_convs.find(key);
    if (it == m_convs.end()) return false;
    Conversation &c = it.value();
    const int i = findMessage(c, targetTs, authors);
    if (i < 0) return false;
    Message &m = c.msgs[i];
    if (newTs <= m.latestTs()) return false; // already applied / outdated revision
    m.text = text;
    m.textStyles = textStyles;
    m.editTs = newTs;
    m.editCount++;
    persist(Records::edit(key, m));
    if (key == m_current) showConversation(true);
    refreshList();
    return true;
}

void MainWindow::setArchived(const QString &key, bool archived)
{
    auto it = m_convs.find(key);
    if (it == m_convs.end() || it->archived == archived) return;
    it->archived = archived;
    persist(Records::archive(key, archived, it->sigArchived));
    refreshList();
}

void MainWindow::editExpiration(const QString &key)
{
    const auto it = m_convs.constFind(key);
    if (it == m_convs.constEnd()) return;

    const QStringList labels = {tr("Off"), tr("30 seconds"), tr("1 minute"), tr("5 minutes"),
                                tr("1 hour"), tr("6 hours"), tr("12 hours"), tr("1 day"),
                                tr("1 week"), tr("4 weeks")};
    const QList<int> values = {0, 30, 60, 5 * 60, 60 * 60, 6 * 60 * 60, 12 * 60 * 60,
                               24 * 60 * 60, 7 * 24 * 60 * 60, 28 * 24 * 60 * 60};
    const int current = values.indexOf(it->expirationSeconds);
    bool ok = false;
    const QString selected = QInputDialog::getItem(this, tr("Disappearing messages"),
                                                   tr("Timer for %1:").arg(it->title), labels,
                                                   current < 0 ? 0 : current, false, &ok);
    if (ok) setExpiration(key, values.at(labels.indexOf(selected)));
}

void MainWindow::showSafetyNumber(const QString &key)
{
    const auto it = m_convs.constFind(key);
    if (it == m_convs.constEnd() || it->isGroup) return;
    if (!m_cli.isRunning()) {
        statusBar()->showMessage(tr("Not connected to signal-cli."), 5000);
        return;
    }
    auto *dialog = new SafetyNumberDialog(m_cli, resolveKey(key), displayName(key), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &SafetyNumberDialog::verified, this,
            [this, key](const QString &recipient, const QString &digits) { onSafetyVerified(key, recipient, digits); });
    dialog->show();
}

// ------------------------------------------- Safety number changes

static QString identityRecipient(const SafetyNumber::Identity &id)
{
    return id.number.isEmpty() ? id.uuid : id.number;
}

void MainWindow::loadKnownSafetyNumbers()
{
    if (m_knownSafetyAccount == m_account) return;
    m_knownSafetyAccount = m_account;
    m_knownSafety.clear();
    QSettings s;
    s.beginGroup(QStringLiteral("knownSafetyNumbers/") + m_account);
    for (const QString &k : s.childKeys())
        m_knownSafety.insert(k, s.value(k).toString());
}

void MainWindow::setKnownSafetyNumber(const QString &recipient, const QString &digits)
{
    if (recipient.isEmpty() || digits.isEmpty() || m_account.isEmpty()) return;
    loadKnownSafetyNumbers();
    m_knownSafety.insert(recipient, digits);
    QSettings().setValue(QStringLiteral("knownSafetyNumbers/") + m_account + QLatin1Char('/') + recipient, digits);
}

QString MainWindow::conversationForIdentity(const SafetyNumber::Identity &id) const
{
    for (const QString &k : {id.number, id.uuid})
        if (!k.isEmpty() && m_convs.contains(k) && !m_convs.value(k).isGroup) return k;
    return {};
}

void MainWindow::checkIdentities(const QString &convKey, bool force)
{
    if (!m_cli.isRunning() || m_account.isEmpty()) return;
    loadKnownSafetyNumbers();
    QJsonObject params;
    if (!convKey.isEmpty()) {
        const auto it = m_convs.constFind(convKey);
        if (it == m_convs.constEnd() || it->isGroup) return;
        const QString recipient = resolveKey(convKey);
        if (recipient == m_account) return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!force && now - m_identityCheckedMs.value(convKey) < 20000) return;
        m_identityCheckedMs.insert(convKey, now);
        params[QStringLiteral("number")] = recipient;
    }
    m_cli.call(QStringLiteral("listIdentities"), params, [this, force](const QJsonValue &res, const QString &err) {
        if (!err.isEmpty()) return; // e.g. a contact signal-cli does not know: nothing to compare
        handleIdentities(SafetyNumber::parse(res), force);
    });
}

void MainWindow::handleIdentities(const QList<SafetyNumber::Identity> &all, bool force)
{
    QHash<QString, QList<SafetyNumber::Identity>> byRecipient;
    for (const SafetyNumber::Identity &id : all) {
        const QString recipient = identityRecipient(id);
        if (!recipient.isEmpty() && recipient != m_account && id.uuid != m_account)
            byRecipient[recipient] << id;
    }
    for (auto it = byRecipient.cbegin(); it != byRecipient.cend(); ++it) {
        const int index = SafetyNumber::pick(it.value(), it.key());
        if (index < 0) continue;
        const SafetyNumber::Identity &id = it.value().at(index);
        const QString stored = m_knownSafety.value(it.key());
        const bool changed = SafetyNumber::evaluate(stored, id) == SafetyNumber::Change::Changed;
        if (changed || (force && id.isUntrusted())) {
            const QString convKey = conversationForIdentity(id);
            if (!convKey.isEmpty()) flagSafetyChange(convKey, {it.key(), id.digits(), id.isUntrusted()});
        } else if (stored.isEmpty()) {
            setKnownSafetyNumber(it.key(), id.digits()); // first sight: this is the baseline
        }
    }
}

void MainWindow::checkSendResults(const QJsonValue &sendResult)
{
    // Messages to several recipients (groups) report failures per recipient, not as an error.
    for (const QJsonValue &rv : sendResult.toObject().value(QStringLiteral("results")).toArray()) {
        const QJsonObject r = rv.toObject();
        if (r.value(QStringLiteral("type")).toString() != QLatin1String("IDENTITY_FAILURE")) continue;
        const QJsonObject address = r.value(QStringLiteral("recipientAddress")).toObject();
        QString who = address.value(QStringLiteral("number")).toString();
        if (who.isEmpty()) who = address.value(QStringLiteral("uuid")).toString();
        who = resolveKey(who);
        statusBar()->showMessage(tr("Not delivered to %1: the safety number has changed.").arg(displayName(who)), 10000);
        checkIdentities(who, true);
    }
}

void MainWindow::flagSafetyChange(const QString &convKey, const SafetyChange &change)
{
    const auto old = m_safetyChanged.constFind(convKey);
    if (old != m_safetyChanged.constEnd() && old->digits == change.digits && old->blocked == change.blocked) return;
    const bool isNew = old == m_safetyChanged.constEnd() || old->digits != change.digits;
    m_safetyChanged.insert(convKey, change);
    if (convKey == m_current)
        updateSafetyBar();
    else if (isNew)
        statusBar()->showMessage(tr("The safety number with %1 has changed.").arg(displayName(convKey)), 10000);
    refreshList();
}

void MainWindow::dismissSafetyChange(const QString &convKey)
{
    const auto it = m_safetyChanged.constFind(convKey);
    if (it == m_safetyChanged.constEnd()) return;
    setKnownSafetyNumber(it->recipient, it->digits); // accepted: only a further change warns again
    m_safetyChanged.remove(convKey);
    updateSafetyBar();
    refreshList();
}

void MainWindow::onSafetyVerified(const QString &convKey, const QString &recipient, const QString &digits)
{
    setKnownSafetyNumber(recipient, digits);
    if (m_safetyChanged.remove(convKey)) {
        updateSafetyBar();
        refreshList();
    }
}

void MainWindow::updateSafetyBar()
{
    if (!m_safetyBar) return;
    const auto it = m_safetyChanged.constFind(m_current);
    if (it == m_safetyChanged.constEnd() || !m_convs.contains(m_current) || m_convs.value(m_current).isGroup) {
        m_safetyBar->hide();
        return;
    }
    QString text = tr("Your safety number with %1 has changed. They may have reinstalled Signal or "
                      "switched phones, but it can also mean that someone is intercepting your chat. "
                      "Compare the number with them before you rely on this chat.")
                       .arg(displayName(m_current));
    if (it->blocked) text += QLatin1Char(' ') + tr("Messages cannot be sent until you verify it.");
    m_safetyLabel->setText(text);
    m_safetyBar->show();
}

void MainWindow::setExpiration(const QString &key, int seconds)
{
    const auto it = m_convs.constFind(key);
    if (it == m_convs.constEnd() || seconds < 0) return;
    if (it->isGroup) {
        m_cli.call(QStringLiteral("updateGroup"),
                   {{QStringLiteral("groupId"), key.mid(2)}, {QStringLiteral("expiration"), seconds}},
                   [this, key, seconds](const QJsonValue &, const QString &err) {
            if (!err.isEmpty()) {
                statusBar()->showMessage(tr("Could not change disappearing-message timer: %1").arg(err), 10000);
                return;
            }
            auto conversation = m_convs.find(key);
            if (conversation == m_convs.end()) return;
            conversation->expirationSeconds = seconds;
            persist(Records::expiration(key, seconds));
            statusBar()->showMessage(tr("Disappearing-message timer updated."), 5000);
        });
    } else {
        m_cli.call(QStringLiteral("updateContact"),
                   {{QStringLiteral("recipient"), resolveKey(key)}, {QStringLiteral("expiration"), seconds}},
                   [this, key, seconds](const QJsonValue &, const QString &err) {
            if (!err.isEmpty()) {
                statusBar()->showMessage(tr("Could not change disappearing-message timer: %1").arg(err), 10000);
                return;
            }
            auto conversation = m_convs.find(key);
            if (conversation == m_convs.end()) return;
            conversation->expirationSeconds = seconds;
            persist(Records::expiration(key, seconds));
            statusBar()->showMessage(tr("Disappearing-message timer updated."), 5000);
        });
    }
}

// ------------------------------------------------------------ List / view

void MainWindow::startIncomingExpirationTimers(const QString &key)
{
    auto conversation = m_convs.find(key);
    if (conversation == m_convs.end()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (Message &message : conversation->msgs) {
        if (message.outgoing || message.expiresInSeconds <= 0 || message.expirationStartMs > 0) continue;
        message.expirationStartMs = now;
        persist(Records::expirationUpdate(key, message));
    }
}

void MainWindow::expireMessages()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for (Conversation &conversation : m_convs) {
        for (int i = conversation.msgs.size() - 1; i >= 0; --i) {
            const Message &message = conversation.msgs[i];
            if (message.expiresInSeconds <= 0 || message.expirationStartMs <= 0) continue;
            if (now - message.expirationStartMs < static_cast<qint64>(message.expiresInSeconds) * 1000) continue;
            conversation.msgs.removeAt(i);
            changed = true;
        }
    }
    if (!changed) return;

    const int rc = m_store.rewrite(allRecords());
    if (rc != SC_OK)
        statusBar()->showMessage(tr("Could not remove expired messages: %1").arg(HistoryStore::errorText(rc)), 10000);
    if (m_convs.contains(m_current)) showConversation(true);
    refreshList();
}

void MainWindow::refreshList()
{
    int archivedCount = 0, archivedUnread = 0;
    for (const Conversation &c : std::as_const(m_convs)) {
        if (c.archived) {
            ++archivedCount;
            archivedUnread += c.unread;
        }
    }
    if (m_showArchived && archivedCount == 0) m_showArchived = false; // folder became empty

    QList<const Conversation *> items;
    for (const Conversation &c : std::as_const(m_convs))
        if (c.archived == m_showArchived) items.append(&c);

    const bool alphabetical = m_appearance.sortAlphabetically;
    std::sort(items.begin(), items.end(), [alphabetical](const Conversation *a, const Conversation *b) {
        const QDateTime ta = a->msgs.isEmpty() ? QDateTime() : QDateTime::fromMSecsSinceEpoch(a->msgs.last().sortTs());
        const QDateTime tb = b->msgs.isEmpty() ? QDateTime() : QDateTime::fromMSecsSinceEpoch(b->msgs.last().sortTs());
        return ConversationOrder::before(a->title, ta, a->key, b->title, tb, b->key, alphabetical);
    });

    QSignalBlocker blocker(m_list);
    m_list->clear();

    auto addSpecial = [this](const QString &text, const QString &key, bool bold) {
        auto *it = new QListWidgetItem(text);
        it->setData(Qt::UserRole, key);
        QFont f = it->font();
        f.setBold(bold);
        f.setItalic(true);
        it->setFont(f);
        m_list->addItem(it);
    };
    if (m_showArchived)
        addSpecial(tr("← Back to chats"), kBackKey, false);
    else if (archivedCount > 0)
        addSpecial(tr("🗄 Archived chats (%1)").arg(archivedCount) + (archivedUnread > 0 ? QStringLiteral(" ●") : QString()),
                   kFolderKey, archivedUnread > 0);

    QListWidgetItem *toSelect = nullptr;
    for (const Conversation *c : items) {
        QString text = (m_safetyChanged.contains(c->key) ? QStringLiteral("⚠ ") : QString()) +
                       (c->muted ? QStringLiteral("🔕 ") : QString()) + (c->isGroup ? QStringLiteral("👥 ") + c->title : c->title);
        auto *it = new QListWidgetItem;
        if (c->unread > 0) {
            text += QStringLiteral(" (%1)").arg(c->unread);
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
        }
        it->setText(text);
        it->setData(Qt::UserRole, c->key);
        if (m_appearance.showAvatars) it->setIcon(avatarIcon(*c));
        m_list->addItem(it);
        if (c->key == m_current) toSelect = it;
    }
    if (toSelect) m_list->setCurrentItem(toSelect);
    updateTray();
    fetchMissingAvatars();
}

// ------------------------------------------------------------- Avatars

// Round avatar, cut from the centre of the picture.
static QPixmap roundAvatar(const QImage &img, int size = 64)
{
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    const QImage scaled = img.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addEllipse(QRectF(0, 0, size, size));
    p.setClipPath(clip);
    p.drawImage((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);
    return out;
}

// Coloured circle with the first letter, like the official client shows without a picture.
static QPixmap placeholderAvatar(const QString &key, const QString &title, int size = 64)
{
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    QString letter = QStringLiteral("?");
    for (const QChar ch : title) {
        if (ch.isLetterOrNumber()) {
            letter = QString(ch).toUpper();
            break;
        }
    }
    const int hue = int(qHash(key) % 360);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor::fromHsl(hue, 120, 110));
    p.drawEllipse(QRectF(0, 0, size, size));
    QFont f = p.font();
    f.setPixelSize(size * 11 / 20);
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, letter);
    return out;
}

QPixmap MainWindow::avatarPixmap(const Conversation &c) const
{
    const auto it = m_avatars.constFind(c.key);
    return it != m_avatars.constEnd() ? *it : placeholderAvatar(c.key, c.title);
}

QIcon MainWindow::avatarIcon(const Conversation &c) const
{
    return QIcon(avatarPixmap(c));
}

// Avatars are needed for the contact list and for notifications (unless those hide the sender).
bool MainWindow::avatarsWanted() const
{
    return m_appearance.showAvatars || (m_notify.notifications && m_notify.content != NotificationSettings::Hidden);
}

void MainWindow::applyAvatarToList(const QString &key)
{
    if (!m_appearance.showAvatars) return;
    const auto cit = m_convs.constFind(key);
    if (cit == m_convs.constEnd()) return;
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        if (item->data(Qt::UserRole).toString() == key) {
            item->setIcon(avatarIcon(*cit));
            break;
        }
    }
}

void MainWindow::fetchMissingAvatars()
{
    // Only while connected (otherwise the request would just fail and never be retried).
    if (!avatarsWanted() || !m_cli.isRunning()) return;
    constexpr int kMaxParallel = 4;
    const QStringList keys = m_convs.keys();
    for (const QString &key : keys) {
        if (m_avatarInFlight >= kMaxParallel) return; // continues when a request finishes
        if (m_avatarTried.contains(key) || m_avatarBusy.contains(key)) continue;
        fetchAvatar(key);
    }
}

void MainWindow::refreshAvatar(const QString &key)
{
    if (!avatarsWanted() || !m_cli.isRunning()) return;
    if (m_avatarBusy.contains(key)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_avatarFetchedMs.value(key, 0) < 60 * 1000) return; // a burst of messages is one request
    fetchAvatar(key);
}

void MainWindow::fetchAvatar(const QString &key)
{
    const auto cit = m_convs.constFind(key);
    if (cit == m_convs.constEnd()) return;
    m_avatarTried.insert(key);
    m_avatarBusy.insert(key);
    m_avatarFetchedMs.insert(key, QDateTime::currentMSecsSinceEpoch());
    ++m_avatarInFlight;

    const int gen = m_connectGen;
    // Apply the result (or give up) and go on with the next conversation.
    auto done = [this, key, gen](const QByteArray &data) {
        if (gen != m_connectGen) return; // reconnect in between: everything restarts anyway
        m_avatarInFlight = qMax(0, m_avatarInFlight - 1);
        m_avatarBusy.remove(key);
        const QImage img = data.isEmpty() ? QImage() : AttachmentUtil::imageFromData(data);
        if (!img.isNull()) {
            const QByteArray digest = QCryptographicHash::hash(data, QCryptographicHash::Md5);
            // Same picture as before: nothing to redraw.
            if (m_avatarDigest.value(key) != digest || !m_avatars.contains(key)) {
                m_avatarDigest.insert(key, digest);
                m_avatars.insert(key, roundAvatar(img));
                applyAvatarToList(key);
            }
        } else if (m_avatars.contains(key)) {
            // The contact removed the picture: back to the letter placeholder.
            m_avatars.remove(key);
            m_avatarDigest.remove(key);
            applyAvatarToList(key);
        }
        fetchMissingAvatars();
    };
    auto dataOf = [](const QJsonValue &res) {
        return QByteArray::fromBase64(res.toObject().value(QStringLiteral("data")).toString().toLatin1());
    };

    if (cit->isGroup) {
        m_cli.call(QStringLiteral("getAvatar"), {{QStringLiteral("groupId"), key.mid(2)}},
                   [done, dataOf](const QJsonValue &res, const QString &err) { done(err.isEmpty() ? dataOf(res) : QByteArray()); });
        return;
    }
    // Contact avatar first (set in the address book), then the person's own profile picture.
    m_cli.call(QStringLiteral("getAvatar"), {{QStringLiteral("contact"), key}},
               [this, key, done, dataOf](const QJsonValue &res, const QString &err) {
                   const QByteArray data = err.isEmpty() ? dataOf(res) : QByteArray();
                   if (!data.isEmpty()) {
                       done(data);
                       return;
                   }
                   m_cli.call(QStringLiteral("getAvatar"), {{QStringLiteral("profile"), key}},
                              [done, dataOf](const QJsonValue &res2, const QString &err2) {
                                  done(err2.isEmpty() ? dataOf(res2) : QByteArray());
                              });
               });
}

// Ctrl+Up / Ctrl+Down. The "Archived chats" and "Back" entries are skipped, and
// the list does not wrap around.
void MainWindow::stepConversation(int delta)
{
    QList<QListWidgetItem *> rows;
    int current = -1;
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        const QString key = item->data(Qt::UserRole).toString();
        if (key == kFolderKey || key == kBackKey) continue;
        if (item == m_list->currentItem()) current = int(rows.size());
        rows.append(item);
    }
    const int target = ConversationOrder::step(current, int(rows.size()), delta);
    if (target >= 0) m_list->setCurrentItem(rows[target]);
}

void MainWindow::onSelectionChanged(QListWidgetItem *item)
{
    if (!item) return;
    const QString key = item->data(Qt::UserRole).toString();
    if (key == kFolderKey || key == kBackKey) return; // handled on click
    openConversation(key);
}

void MainWindow::saveDraft(const QString &key)
{
    if (key.isEmpty()) return;
    Draft d;
    d.text = m_input->text();
    d.richText = m_input->richText();
    d.cursor = m_input->textCursor().position();
    d.reply = m_replyQuote;
    d.files = m_pendingFiles;
    d.gifAnalyticsUrl = m_pendingGifAnalyticsUrl;
    if (d.text.isEmpty() && !d.reply.valid && d.files.isEmpty())
        m_drafts.remove(key);
    else
        m_drafts.insert(key, d);
}

void MainWindow::loadDraft(const QString &key)
{
    const Draft d = m_drafts.take(key); // the composer owns it while the chat is open
    if (!d.richText.isEmpty()) m_input->setRichText(d.richText);
    else m_input->setText(d.text);
    m_input->setCursorPosition(d.cursor);
    m_replyQuote = d.reply;
    m_pendingFiles = d.files;
    m_pendingGifAnalyticsUrl = d.gifAnalyticsUrl;
    updateReplyBar();
    updateAttachBar();
}

void MainWindow::switchConversation(const QString &key)
{
    if (key == m_current) return;
    cancelEdit(); // an edit in progress is dropped and the text typed before it comes back
    saveDraft(m_current);
    m_current = key;
    loadDraft(key);
}

void MainWindow::openConversation(const QString &key)
{
    if (!m_convs.contains(key)) return;
    switchConversation(key); // stashes the old chat's draft, restores this chat's draft
    m_convs[m_current].unread = 0;
    startIncomingExpirationTimers(key);
    showConversation();
    refreshList();
    m_notifier->close(key); // the notification for this chat is obsolete now
    checkIdentities(key);
    focusComposer();
}

void MainWindow::focusComposer()
{
    // openConversation() normally runs from inside the chat list's mouse-press
    // handler (currentItemChanged), right after the list took the focus and while
    // refreshList() is rebuilding it. Focus set at that point can leave the text
    // field half focused: the caret blinks but key presses go nowhere until the
    // next click. So set it once the click has been handled, and if the field
    // already counts as focused, force a clean FocusOut/FocusIn so the caret and
    // the input method are in sync again.
    QTimer::singleShot(0, this, [this] {
        if (!m_input || !m_input->isEnabled() || !m_input->isVisible()) return;
        if (isActiveWindow() && QApplication::focusWidget() == m_input) m_input->clearFocus();
        m_input->setFocus(Qt::OtherFocusReason);
    });
}

void MainWindow::showConversation(bool keepScroll)
{
    if (!m_convs.contains(m_current)) {
        m_title->setText(tr("No conversation selected"));
        m_view->clearView();
        updateSafetyBar();
        updateMessageSearch();
        return;
    }
    const Conversation &c = m_convs[m_current];
    m_title->setText(c.title);
    updateSafetyBar();
    m_view->setGroupMode(c.isGroup);
    m_view->showMessages(c.msgs, keepScroll);
    updateMessageSearch();
    updateAudioPlaybackView();
}

void MainWindow::openMessageSearch()
{
    m_searchBar->show();
    m_searchEdit->setFocus();
    m_searchEdit->selectAll();
    updateMessageSearch(true);
}

void MainWindow::closeMessageSearch()
{
    m_searchBar->hide();
    m_searchEdit->clear();
    m_input->setFocus();
}

void MainWindow::updateMessageSearch(bool selectFirst)
{
    const int oldPosition = m_searchPosition;
    m_searchResults.clear();
    const QString needle = m_searchEdit->text().trimmed();
    const auto conversation = m_convs.constFind(m_current);
    if (!needle.isEmpty() && conversation != m_convs.cend()) {
        const QList<Message> &messages = conversation->msgs;
        for (int i = 0; i < messages.size(); ++i) {
            const Message &message = messages.at(i);
            QString searchable = message.sender + QLatin1Char('\n') + message.text +
                                 QLatin1Char('\n') + message.quote.text;
            for (const Attachment &attachment : message.attachments)
                searchable += QLatin1Char('\n') + attachment.filename;
            if (searchable.contains(needle, Qt::CaseInsensitive)) m_searchResults.append(i);
        }
    }

    if (m_searchResults.isEmpty()) {
        m_searchPosition = -1;
        m_searchCount->setText(needle.isEmpty() ? QString() : QStringLiteral("0 / 0"));
        return;
    }
    m_searchPosition = selectFirst || oldPosition < 0
                           ? 0
                           : qMin(oldPosition, m_searchResults.size() - 1);
    m_searchCount->setText(tr("%1 / %2").arg(m_searchPosition + 1).arg(m_searchResults.size()));
    if (selectFirst) m_view->scrollToMessage(m_searchResults.at(m_searchPosition));
}

void MainWindow::navigateMessageSearch(bool backwards)
{
    if (m_searchResults.isEmpty()) return;
    const int count = m_searchResults.size();
    m_searchPosition = (m_searchPosition + (backwards ? count - 1 : 1)) % count;
    m_searchCount->setText(tr("%1 / %2").arg(m_searchPosition + 1).arg(count));
    m_view->scrollToMessage(m_searchResults.at(m_searchPosition));
}

void MainWindow::updateReplyBar()
{
    if (!m_replyQuote.valid) {
        m_replyBar->hide();
        return;
    }
    QString text = m_replyQuote.text.isEmpty() ? tr("[attachment]") : m_replyQuote.text;
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (text.size() > 120) text = text.left(120) + QStringLiteral("…");
    m_replyLabel->setText(tr("↩ Replying to %1: %2").arg(displayName(m_replyQuote.authorKey), text));
    m_replyBar->show();
}

void MainWindow::updateAttachBar()
{
    if (m_pendingFiles.isEmpty()) {
        m_attachBar->hide();
        return;
    }
    QStringList names;
    for (const QString &f : std::as_const(m_pendingFiles)) names << QFileInfo(f).fileName();
    m_attachLabel->setText(QStringLiteral("📎 ") + names.join(QStringLiteral(", ")));

    // small preview of the first image, so it is obvious that the paste worked
    m_attachThumb->hide();
    for (const QString &f : std::as_const(m_pendingFiles)) {
        if (!AttachmentUtil::mimeTypeForFile(f).startsWith(QLatin1String("image/"))) continue;
        const QImage img = AttachmentUtil::loadImage(f);
        if (img.isNull()) continue;
        m_attachThumb->setPixmap(QPixmap::fromImage(img.scaled(44, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        m_attachThumb->show();
        break;
    }
    m_attachBar->show();
}

void MainWindow::addPendingFiles(const QStringList &files)
{
    for (const QString &f : files) {
        const QFileInfo fi(f);
        if (!fi.isFile() || !fi.isReadable()) continue;
        if (fi.size() > kMaxAttachment) {
            QMessageBox::warning(this, tr("Attach files"),
                                 tr("“%1” is larger than 100 MB, the limit for Signal attachments.").arg(fi.fileName()));
            continue;
        }
        if (!m_pendingFiles.contains(fi.absoluteFilePath())) m_pendingFiles << fi.absoluteFilePath();
    }
    updateAttachBar();
    m_input->setFocus();
}

bool MainWindow::attachFromMimeData(const QMimeData *md)
{
    if (!md) return false;

    // 1. files (copied in a file manager, dropped, or a screenshot tool that saved a file)
    QStringList files;
    for (const QUrl &u : md->urls())
        if (u.isLocalFile()) files << u.toLocalFile();
    if (!files.isEmpty()) {
        addPendingFiles(files);
        return true;
    }

    // 2. raw image data (screenshot copied to the clipboard). If there is
    //    real text as well (e.g. copied from a document), the text wins.
    if (md->hasImage() && md->text().trimmed().isEmpty()) {
        const QImage img = qvariant_cast<QImage>(md->imageData());
        if (img.isNull() || !m_tmp.isValid()) return false;
        static int counter = 0;
        const QString path = m_tmp.path() + QStringLiteral("/pasted-%1-%2.png")
                                 .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")))
                                 .arg(++counter);
        if (!img.save(path, "PNG")) {
            statusBar()->showMessage(tr("The image from the clipboard could not be used."), 8000);
            return false;
        }
        addPendingFiles({path});
        return true;
    }
    return false;
}

void MainWindow::newConversation()
{
    bool ok = false;
    const QString recipient = QInputDialog::getText(this, tr("New conversation"),
                                                 tr("Phone number or Signal username (e.g. username.1234):"),
                                                 QLineEdit::Normal, QString(), &ok)
                               .trimmed();
    if (!ok || recipient.isEmpty()) return;
    Conversation &c = conv(recipient);
    if (c.archived) setArchived(recipient, false);
    m_showArchived = false;
    switchConversation(recipient);
    refreshList();
    showConversation();
}

// ------------------------------------------------------ Receive / send

void MainWindow::onEnvelope(const QJsonObject &env)
{
    ParsedEnvelope pe = parseEnvelope(env);
    if (pe.kind == ParsedEnvelope::None) return;

    const bool group = !pe.groupId.isEmpty();
    const QString srcKey = resolveKey(pe.src);
    const QString key = group ? QStringLiteral("g:") + pe.groupId : (pe.outgoing ? resolveKey(pe.destination) : srcKey);
    if (key.isEmpty()) return;

    if (pe.kind == ParsedEnvelope::ExpirationUpdate) {
        auto conversation = m_convs.find(key);
        if (conversation != m_convs.end() && conversation->expirationSeconds != pe.expirationSeconds) {
            conversation->expirationSeconds = pe.expirationSeconds;
            persist(Records::expiration(key, pe.expirationSeconds));
        }
        return;
    }

    if (pe.kind == ParsedEnvelope::Reaction) {
        // The reaction's group id sits in the same dataMessage; groupId was not parsed for reactions.
        const QString gid = env.value(QStringLiteral("dataMessage")).toObject().value(QStringLiteral("groupInfo")).toObject()
                                .value(QStringLiteral("groupId")).toString();
        const QString rkey = !gid.isEmpty() ? QStringLiteral("g:") + gid : key;
        const QString reactor = pe.outgoing ? m_account : srcKey;
        applyReaction(rkey, pe.targetTs, {pe.targetNumber, pe.targetUuid}, reactor, pe.remove ? QString() : pe.emoji);
        return;
    }

    if (pe.kind == ParsedEnvelope::Delete) {
        // Only the author can delete a message for everyone (the phone or the other side).
        const QString author = pe.outgoing ? m_account : srcKey;
        applyDelete(key, pe.targetTs, {author});
        return;
    }

    if (pe.kind == ParsedEnvelope::Edit) {
        // Like the official clients: an edit is only applied to a known message written by the
        // same author. Anything else (unknown target, someone else's message) is dropped.
        const QString author = pe.outgoing ? m_account : srcKey;
        applyEdit(key, pe.targetTs, {author}, pe.msg.text, pe.msg.ts(), pe.msg.textStyles);
        return;
    }

    Message m = pe.msg;
    m.outgoing = pe.outgoing;
    m.authorKey = pe.outgoing ? m_account : srcKey;
    if (m.outgoing && m.expiresInSeconds > 0) m.expirationStartMs = m.ts();
    if (m.quote.valid) m.quote.authorKey = resolveKey(m.quote.authorKey);

    if (!pe.outgoing) {
        checkIdentities(srcKey); // a new key becomes known when its first message arrives
        QString display = m_convs.contains(srcKey) ? m_convs[srcKey].title : QString();
        if (display.isEmpty()) display = pe.srcName.isEmpty() ? pe.src : pe.srcName;
        m.sender = display;
    }

    if (pe.outgoing) {
        // An envelope for an earlier send can arrive while another identical message is
        // pending. Don't let that older echo consume the newer message's preview.
        bool alreadyConfirmed = false;
        const auto existingConversation = m_convs.constFind(key);
        if (existingConversation != m_convs.constEnd()) {
            for (const Message &existing : existingConversation->msgs) {
                if (existing.ts() != m.ts() || !existing.outgoing || existing.authorKey != m_account)
                    continue;
                bool isPendingPreview = false;
                for (auto pendingIt = m_pendingSends.cbegin(); pendingIt != m_pendingSends.cend(); ++pendingIt) {
                    if (pendingIt->conversationKey == key &&
                        pendingIt->temporaryTimestamp == existing.ts()) {
                        isPendingPreview = true;
                        break;
                    }
                }
                if (!isPendingPreview) {
                    alreadyConfirmed = true;
                    break;
                }
            }
        }

        // signal-cli can publish our own message envelope before the send RPC returns.
        // Reconcile it with the visible preview so the chat never shows two bubbles.
        QString pendingId;
        qint64 oldestPendingTimestamp = 0;
        if (!alreadyConfirmed) {
            for (auto it = m_pendingSends.cbegin(); it != m_pendingSends.cend(); ++it) {
                const PendingSend &pending = it.value();
                if (pending.echoReceived || pending.conversationKey != key || pending.message.text != m.text)
                    continue;
                if (!pending.message.attachments.isEmpty() && !m.attachments.isEmpty() &&
                    pending.message.attachments.size() != m.attachments.size())
                    continue;
                if (pendingId.isEmpty() || pending.temporaryTimestamp < oldestPendingTimestamp) {
                    pendingId = it.key();
                    oldestPendingTimestamp = pending.temporaryTimestamp;
                }
            }
        }
        if (!pendingId.isEmpty()) {
            auto pendingIt = m_pendingSends.find(pendingId);
            if (pendingIt != m_pendingSends.end()) {
                const qint64 temporaryTimestamp = pendingIt->temporaryTimestamp;
                bool previewExists = false;
                const auto conversationIt = m_convs.constFind(key);
                if (conversationIt != m_convs.constEnd()) {
                    for (const Message &message : conversationIt->msgs) {
                        if (message.ts() == temporaryTimestamp && message.outgoing &&
                            message.authorKey == m_account) {
                            previewExists = true;
                            break;
                        }
                    }
                }
                if (previewExists) {
                    finalizePendingSend(pendingId, m.ts(), &m);
                    fetchThumbnails(key, m.ts(), m.authorKey);
                    return;
                }
            }
        }
    }

    const QString title = group ? tr("Group") : (pe.outgoing ? key : m.sender);
    if (!addMessage(key, m, title, group)) return;
    if (!pe.outgoing && !group && !pe.srcName.isEmpty()) learnName(key, pe.srcName);
    if (!pe.outgoing) {
        alertIncoming(key, m);
        if (!group) refreshAvatar(key); // the contact may have changed the picture
    }
    fetchThumbnails(key, m.ts(), m.authorKey);
}

void MainWindow::addRecipient(QJsonObject &params, const Conversation &c) const
{
    if (c.isGroup)
        params[QStringLiteral("groupId")] = c.key.mid(2);
    else
        params[QStringLiteral("recipient")] = QJsonArray{c.key};
}

void MainWindow::startReply(int idx)
{
    if (!m_convs.contains(m_current) || idx < 0 || idx >= m_convs[m_current].msgs.size()) return;
    const Conversation &c = m_convs[m_current];
    const Message &m = c.msgs[idx];
    if (m.deleted) return;
    m_replyQuote = Quote();
    m_replyQuote.valid = true;
    m_replyQuote.id = m.latestTs(); // peers know an edited message by its latest timestamp
    m_replyQuote.authorKey = effectiveAuthor(c, m);
    m_replyQuote.text = m.text;
    if (m.text.isEmpty() && !m.attachments.isEmpty()) m_replyQuote.attachmentNote = m.attachments.first().contentType;
    updateReplyBar();
    m_input->setFocus();
}

void MainWindow::sendMessage()
{
    if (!m_editKey.isEmpty()) {
        const QString raw = m_input->text();
        const QString text = raw.trimmed();
        const int first = raw.indexOf(QRegularExpression(QStringLiteral("\\S")));
        const int lead = first < 0 ? raw.size() : first;
        const int last = raw.lastIndexOf(QRegularExpression(QStringLiteral("\\S")));
        const int end = last < 0 ? lead : last + 1;
        sendEdit(text, normalizedStyles(m_input->textStyles(), lead, end));
        return;
    }
    const QString rawText = m_input->text();
    const QString text = rawText.trimmed();
    const int first = rawText.indexOf(QRegularExpression(QStringLiteral("\\S")));
    const int lead = first < 0 ? rawText.size() : first;
    const int last = rawText.lastIndexOf(QRegularExpression(QStringLiteral("\\S")));
    const int end = last < 0 ? lead : last + 1;
    const QJsonArray textStyles = normalizedStyles(m_input->textStyles(), lead, end);
    if ((text.isEmpty() && m_pendingFiles.isEmpty()) || !m_convs.contains(m_current)) return;

    const Conversation c = m_convs[m_current];
    const QStringList files = m_pendingFiles;
    const Quote quote = m_replyQuote;
    const QUrl giphySentAnalyticsUrl = m_pendingGifAnalyticsUrl;

    QJsonObject p;
    if (!text.isEmpty()) {
        p[QStringLiteral("message")] = text;
        if (!textStyles.isEmpty()) p[QStringLiteral("textStyle")] = textStyles;
    }

    QList<Attachment> atts;
    QStringList paths;
    const QString stageDir = m_tmp.path() + QStringLiteral("/send-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QDateTime sendTime = QDateTime::currentDateTime();
    int fileIndex = 0;
    for (const QString &f : files) {
        const QFileInfo fi(f);
        Attachment a;
        a.path = fi.absoluteFilePath();
        a.filename = fi.fileName();
        a.size = fi.size();
        a.contentType = AttachmentUtil::mimeTypeForFile(f);
        QString sendPath = a.path;
        if (a.isImage()) {
            // Like the official Signal clients: images go out under a generic, time-based
            // name instead of the local file name. One folder per file keeps the name intact.
            const QString generic = AttachmentUtil::genericFileName(a.contentType, a.filename,
                                                                    sendTime.addMSecs(fileIndex));
            const QString renamed = stageDir + QStringLiteral("/named-") + QString::number(fileIndex) +
                                    QLatin1Char('/') + generic;
            if (QDir().mkpath(QFileInfo(renamed).path()) && QFile::copy(a.path, renamed)) {
                sendPath = renamed;
                a.filename = generic;
            }
        }
        ++fileIndex;
        if (a.isImage()) {
            const QImage img = AttachmentUtil::loadImage(f);
            if (!img.isNull()) {
                a.width = img.width();
                a.height = img.height();
                a.thumb = AttachmentUtil::makeThumbnail(img);
            }
        }
        atts << a;
        paths.append(sendPath);
    }
    if (!paths.isEmpty())
        p[QStringLiteral("attachments")] = QJsonArray::fromStringList(SignalCli::stageFiles(paths, stageDir));

    if (quote.valid && !quote.authorKey.isEmpty()) {
        p[QStringLiteral("quoteTimestamp")] = QJsonValue(static_cast<qint64>(quote.id));
        p[QStringLiteral("quoteAuthor")] = quote.authorKey;
        p[QStringLiteral("quoteMessage")] = quote.text;
    }
    addRecipient(p, c);

    // Insert a temporary local message before waiting for signal-cli. It is kept out of
    // encrypted history until the send succeeds, so a failed send leaves no stale record.
    Message preview;
    preview.time = QDateTime::currentDateTime();
    preview.textStyles = styleRangesFromJson(textStyles);
    qint64 temporaryTimestamp = preview.ts();
    Conversation &target = m_convs[m_current];
    while (findMessage(target, temporaryTimestamp, {}) >= 0) {
        ++temporaryTimestamp;
        preview.time = QDateTime::fromMSecsSinceEpoch(temporaryTimestamp);
    }
    preview.text = text;
    preview.outgoing = true;
    preview.authorKey = m_account;
    preview.expiresInSeconds = c.expirationSeconds;
    preview.quote = quote;
    preview.attachments = atts;
    preview.orderTs = std::max(preview.ts(), (target.msgs.isEmpty() ? qint64(0) : target.msgs.last().sortTs()) + 1);

    const QString key = c.key;
    const QString pendingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingSend pending;
    pending.conversationKey = key;
    pending.temporaryTimestamp = temporaryTimestamp;
    pending.message = preview;
    m_pendingSends.insert(pendingId, pending);

    int at = target.msgs.size();
    while (at > 0 && target.msgs[at - 1].sortTs() > preview.sortTs()) --at;
    target.msgs.insert(at, preview);

    m_input->clear();
    m_pendingFiles.clear();
    m_replyQuote = Quote();
    updateAttachBar();
    updateReplyBar();
    if (!files.isEmpty()) statusBar()->showMessage(tr("Sending …"));
    if (key == m_current) showConversation(true);

    m_cli.call(QStringLiteral("send"), p,
               [this, pendingId, key, text, textStyles, files, quote, giphySentAnalyticsUrl, stageDir](const QJsonValue &res, const QString &err) {
        QDir(stageDir).removeRecursively();
        auto pendingIt = m_pendingSends.find(pendingId);
        if (pendingIt == m_pendingSends.end()) return;

        if (!err.isEmpty() && !pendingIt->echoReceived) {
            const PendingSend pending = pendingIt.value();
            removePendingPreview(pending);
            m_pendingSends.erase(pendingIt);
            statusBar()->showMessage(tr("Sending failed: %1").arg(err), 10000);
            if (SafetyNumber::isUntrustedIdentityError(err)) checkIdentities(key, true);
            if (key != m_current) {
                // The user has moved on to another chat: put the unsent message back into
                // that chat's draft, never into the composer of the chat that is open now.
                Draft &d = m_drafts[key];
                if (d.text.isEmpty() && d.files.isEmpty() && !d.reply.valid) {
                    d.text = text;
                    d.cursor = text.size();
                    d.files = files;
                    d.reply = quote;
                }
                if (d.text.isEmpty() && d.files.isEmpty() && !d.reply.valid) m_drafts.remove(key);
                return;
            }
            const bool draftWasEmpty = m_input->text().isEmpty();
            if (draftWasEmpty) m_input->setText(text); // don't overwrite a newer draft
            if (draftWasEmpty && m_pendingFiles.isEmpty() && !files.isEmpty()) {
                m_pendingFiles = files;
                updateAttachBar();
            }
            if (draftWasEmpty && !m_replyQuote.valid && quote.valid) {
                m_replyQuote = quote;
                updateReplyBar();
            }
            return;
        }

        // An outgoing envelope is stronger evidence than a failed/late RPC response.
        // The preview is already confirmed and persisted in that case.
        if (pendingIt->echoReceived) {
            m_pendingSends.erase(pendingIt);
            statusBar()->clearMessage();
            registerGiphyAnalytics(giphySentAnalyticsUrl);
            if (m_pendingGifAnalyticsUrl == giphySentAnalyticsUrl) m_pendingGifAnalyticsUrl.clear();
            return;
        }

        qint64 ts = res.toObject().value(QStringLiteral("timestamp")).toVariant().toLongLong();
        if (ts <= 0) ts = QDateTime::currentMSecsSinceEpoch();
        statusBar()->clearMessage();
        finalizePendingSend(pendingId, ts);
        registerGiphyAnalytics(giphySentAnalyticsUrl);
        if (m_pendingGifAnalyticsUrl == giphySentAnalyticsUrl) m_pendingGifAnalyticsUrl.clear();
        checkSendResults(res);
    });
}

void MainWindow::startEdit(int idx)
{
    if (!m_convs.contains(m_current) || idx < 0 || idx >= m_convs[m_current].msgs.size()) return;
    const Conversation &c = m_convs[m_current];
    const Message &m = c.msgs[idx];
    if (!m.canEdit(QDateTime::currentMSecsSinceEpoch())) {
        statusBar()->showMessage(tr("This message can no longer be edited (3 hours / 10 edits)."), 6000);
        return;
    }
    if (m_editKey.isEmpty()) {
        m_editDraft = m_input->text();
        m_editDraftRichText = m_input->richText();
    } // keep an already running edit's draft
    m_editKey = c.key;
    m_editTs = m.ts();
    // A multi-line message needs the expanded field, otherwise only one line would be visible.
    // Remember that we opened it ourselves so it is collapsed again when the edit ends.
    if (m.text.contains(QLatin1Char('\n')) && !m_input->isExpanded()) {
        m_input->setExpanded(true);
        m_editExpandedInput = true;
    }
    m_input->setText(m.text);
    m_input->setCursorPosition(m.text.size());
    updateEditBar();
    m_input->setFocus();
}

void MainWindow::editLastMessage()
{
    if (!m_input->text().isEmpty() || !m_editKey.isEmpty() || !m_pendingFiles.isEmpty() || !m_convs.contains(m_current))
        return;

    const Conversation &conversation = m_convs[m_current];
    for (int i = conversation.msgs.size() - 1; i >= 0; --i) {
        const Message &message = conversation.msgs[i];
        if (!message.outgoing || message.authorKey != m_account) continue;
        bool pending = false;
        for (auto it = m_pendingSends.cbegin(); it != m_pendingSends.cend(); ++it) {
            if (it->conversationKey == conversation.key && it->temporaryTimestamp == message.ts()) {
                pending = true;
                break;
            }
        }
        if (pending) continue;
        startEdit(i);
        return;
    }
}

void MainWindow::cancelEdit(bool restoreDraft)
{
    if (m_editKey.isEmpty()) return;
    m_editKey.clear();
    m_editTs = 0;
    if (restoreDraft) {
        if (!m_editDraftRichText.isEmpty()) m_input->setRichText(m_editDraftRichText);
        else m_input->setText(m_editDraft);
    }
    m_editDraft.clear();
    m_editDraftRichText.clear();
    if (m_editExpandedInput) {
        m_editExpandedInput = false;
        m_input->setExpanded(false);
    }
    updateEditBar();
}

void MainWindow::updateEditBar()
{
    if (m_editKey.isEmpty()) {
        m_editBar->hide();
        return;
    }
    // In the expanded field Enter inserts a line break, so saving goes through the Send button.
    m_editLabel->setText(m_input->isExpanded() ? tr("✎ Editing message – click Send to save, Esc to cancel")
                                               : tr("✎ Editing message – Enter to save, Esc to cancel"));
    m_editBar->show();
}

void MainWindow::sendEdit(const QString &text, const QJsonArray &textStyles)
{
    auto it = m_convs.find(m_editKey);
    if (it == m_convs.end()) {
        cancelEdit();
        return;
    }
    const Conversation c = it.value();
    const int i = findMessage(c, m_editTs, {m_account});
    if (i < 0) {
        cancelEdit();
        return;
    }
    const Message m = c.msgs[i];
    if (text.isEmpty()) {
        statusBar()->showMessage(tr("A message cannot be empty."), 6000);
        return;
    }
    if (text == m.text && textStyles == styleRangesToJson(m.textStyles)) { // nothing changed
        cancelEdit();
        return;
    }
    if (!m.canEdit(QDateTime::currentMSecsSinceEpoch())) {
        statusBar()->showMessage(tr("This message can no longer be edited (3 hours / 10 edits)."), 6000);
        cancelEdit();
        return;
    }

    // signal-cli: `send --edit-timestamp <ts>`; the target is the latest revision the peers know.
    QJsonObject p{{QStringLiteral("message"), text},
                  {QStringLiteral("editTimestamp"), QJsonValue(static_cast<qint64>(m.latestTs()))}};
    if (!textStyles.isEmpty()) p[QStringLiteral("textStyle")] = textStyles;
    if (m.quote.valid && !m.quote.authorKey.isEmpty()) { // an edit keeps the reply it belongs to
        p[QStringLiteral("quoteTimestamp")] = QJsonValue(static_cast<qint64>(m.quote.id));
        p[QStringLiteral("quoteAuthor")] = m.quote.authorKey;
        p[QStringLiteral("quoteMessage")] = m.quote.text;
    }
    addRecipient(p, c);

    const QString key = c.key;
    const qint64 origTs = m.ts();
    m_cli.call(QStringLiteral("send"), p, [this, key, origTs, text, textStyles](const QJsonValue &res, const QString &err) {
        if (!err.isEmpty()) {
            // stay in edit mode with the typed text so nothing is lost
            statusBar()->showMessage(tr("Editing failed: %1").arg(err), 10000);
            return;
        }
        statusBar()->clearMessage();
        qint64 ts = res.toObject().value(QStringLiteral("timestamp")).toVariant().toLongLong();
        if (ts <= 0) ts = QDateTime::currentMSecsSinceEpoch();
        applyEdit(key, origTs, {m_account}, text, ts, styleRangesFromJson(textStyles));
        if (m_editKey == key && m_editTs == origTs) cancelEdit();
    });
}

void MainWindow::sendReaction(int idx, const QString &emoji)
{
    if (!m_convs.contains(m_current) || idx < 0 || idx >= m_convs[m_current].msgs.size()) return;
    const Conversation c = m_convs[m_current];
    const Message m = c.msgs[idx];
    if (m.deleted) return;
    const QString author = effectiveAuthor(c, m);
    const bool remove = m.reactions.value(m_account) == emoji; // same emoji again = take it back

    QJsonObject p{{QStringLiteral("emoji"), emoji},
                  {QStringLiteral("targetAuthor"), author},
                  {QStringLiteral("targetTimestamp"), QJsonValue(static_cast<qint64>(m.latestTs()))},
                  {QStringLiteral("remove"), remove}};
    addRecipient(p, c);

    const QString key = c.key;
    const qint64 ts = m.ts();
    m_cli.call(QStringLiteral("sendReaction"), p, [this, key, ts, author, emoji, remove](const QJsonValue &, const QString &err) {
        if (!err.isEmpty()) {
            statusBar()->showMessage(tr("Reaction failed: %1").arg(err), 8000);
            return;
        }
        applyReaction(key, ts, {author}, m_account, remove ? QString() : emoji);
    });
}

// ----------------------------------------------------------- Attachments

void MainWindow::fetchThumbnails(const QString &key, qint64 ts, const QString &author, int attempt)
{
    auto it = m_convs.find(key);
    if (it == m_convs.end()) return;
    const Conversation &c = it.value();
    const int mi = findMessage(c, ts, QStringList{author});
    if (mi < 0) return;
    const Message &m = c.msgs[mi];

    for (int a = 0; a < m.attachments.size(); ++a) {
        const Attachment &att = m.attachments[a];
        if (!att.isImage() || !att.thumb.isEmpty() || att.id.isEmpty() || att.size > kMaxThumbSource) continue;
        const QString token = QStringLiteral("%1|%2|%3|%4").arg(key).arg(ts).arg(author).arg(a);
        if (m_thumbPending.contains(token)) continue;
        m_thumbPending.insert(token);

        QJsonObject p{{QStringLiteral("id"), att.id}};
        addRecipient(p, c);
        m_cli.call(QStringLiteral("getAttachment"), p,
                   [this, key, ts, author, a, attempt, token](const QJsonValue &res, const QString &err) {
                       m_thumbPending.remove(token);
                       if (!err.isEmpty()) {
                           // signal-cli may still be downloading it: retry a few times
                           if (attempt < 3)
                               QTimer::singleShot(2000 * (attempt + 1), this,
                                                  [this, key, ts, author, attempt] { fetchThumbnails(key, ts, author, attempt + 1); });
                           return;
                       }
                       const QByteArray data =
                           QByteArray::fromBase64(res.toObject().value(QStringLiteral("data")).toString().toLatin1());
                       const QImage img = AttachmentUtil::imageFromData(data);
                       auto cit = m_convs.find(key);
                       if (img.isNull() || cit == m_convs.end()) return;
                       const int i = findMessage(cit.value(), ts, QStringList{author});
                       if (i < 0 || a >= cit.value().msgs[i].attachments.size()) return;
                       Message &mm = cit.value().msgs[i];
                       Attachment &at = mm.attachments[a];
                       at.width = img.width();
                       at.height = img.height();
                       at.thumb = AttachmentUtil::makeThumbnail(img);
                       persist(Records::attachmentsUpdate(key, mm));
                       if (key == m_current) showConversation(true);
                   });
    }
}

void MainWindow::loadAnimatedAttachment(int messageIndex, int attachmentIndex)
{
    if (!m_convs.contains(m_current) || messageIndex < 0 || messageIndex >= m_convs[m_current].msgs.size()) return;
    const Conversation &conversation = m_convs[m_current];
    const Message &message = conversation.msgs[messageIndex];
    if (attachmentIndex < 0 || attachmentIndex >= message.attachments.size()) return;
    const Attachment attachment = message.attachments[attachmentIndex];
    if (!attachment.isGif() || attachment.size > kMaxThumbSource) return;

    const QString key = conversation.key;
    const QString author = effectiveAuthor(conversation, message);
    const qint64 timestamp = message.ts();
    const QString token = QStringLiteral("%1|%2|%3|%4").arg(key).arg(timestamp).arg(author).arg(attachmentIndex);
    if (m_gifPending.contains(token) || m_view->hasAnimatedAttachment(messageIndex, attachmentIndex)) return;
    m_gifPending.insert(token);

    fetchAttachmentData(key, attachment, [this, key, author, timestamp, attachmentIndex, token]
                        (const QByteArray &data, const QString &error) {
        m_gifPending.remove(token);
        if (!error.isEmpty() || data.isEmpty()) return;
        QTimer::singleShot(0, this, [this, key, author, timestamp, attachmentIndex, data] {
            if (m_current != key || !m_convs.contains(key)) return;
            const int index = findMessage(m_convs[key], timestamp, {author});
            if (index >= 0) m_view->setAnimatedAttachment(index, attachmentIndex, data);
        });
    });
}

void MainWindow::fetchAttachmentData(const QString &key, const Attachment &att,
                                     std::function<void(const QByteArray &, const QString &)> cb)
{
    if (!att.path.isEmpty() && QFileInfo::exists(att.path)) {
        QFile f(att.path);
        if (f.open(QIODevice::ReadOnly)) {
            cb(f.readAll(), QString());
            return;
        }
    }
    if (att.id.isEmpty() || !m_convs.contains(key)) {
        cb({}, tr("The attachment is no longer available."));
        return;
    }
    QJsonObject p{{QStringLiteral("id"), att.id}};
    addRecipient(p, m_convs[key]);
    m_cli.call(QStringLiteral("getAttachment"), p, [cb](const QJsonValue &res, const QString &err) {
        if (!err.isEmpty()) {
            cb({}, err);
            return;
        }
        cb(QByteArray::fromBase64(res.toObject().value(QStringLiteral("data")).toString().toLatin1()), QString());
    });
}

static bool isRiskyFile(const QString &name) { return AttachmentUtil::isRiskyFileName(name); }

void MainWindow::openAttachment(int msgIdx, int attIdx)
{
    if (!m_convs.contains(m_current)) return;
    const Conversation &c = m_convs[m_current];
    if (msgIdx < 0 || msgIdx >= c.msgs.size() || attIdx < 0 || attIdx >= c.msgs[msgIdx].attachments.size()) return;
    const Attachment att = c.msgs[msgIdx].attachments[attIdx];
    const QString viewerName = AttachmentUtil::displayFileName(att, c.msgs[msgIdx].time);
    const QString key = m_current;

    if (att.isAudio()) {
        toggleAudioAttachment(msgIdx, attIdx);
        return;
    }

    if (att.isVideo()) {
        if (m_view->isVideoActive(msgIdx, attIdx)) {
            m_view->toggleVideoPlayback();
            return;
        }
        if (m_view->isVideoLoading(msgIdx, attIdx)) return;
        const Message message = c.msgs[msgIdx];
        const QString author = effectiveAuthor(c, message);
        const qint64 timestamp = message.ts();
        m_view->setVideoLoading(msgIdx, attIdx, true);
        fetchAttachmentData(key, att, [this, key, author, timestamp, attIdx, att]
                            (const QByteArray &data, const QString &error) {
            if (m_current != key || !m_convs.contains(key)) return;
            const int index = findMessage(m_convs[key], timestamp, {author});
            if (index < 0 || attIdx >= m_convs[key].msgs[index].attachments.size()) return;
            m_view->setVideoLoading(index, attIdx, false);
            if (!error.isEmpty() || data.isEmpty()) {
                statusBar()->showMessage(error.isEmpty() ? tr("The video file is empty.") : error, 10000);
                return;
            }
            if (!m_tmp.isValid()) {
                statusBar()->showMessage(tr("No temporary directory available for video playback."), 10000);
                return;
            }
            QString suffix = QFileInfo(att.filename).suffix();
            if (suffix.isEmpty()) suffix = QMimeDatabase().mimeTypeForName(att.contentType).preferredSuffix();
            const QString path = m_tmp.path() + QStringLiteral("/video-") +
                                 QUuid::createUuid().toString(QUuid::WithoutBraces) +
                                 (suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix);
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
                statusBar()->showMessage(tr("The video file could not be prepared for playback."), 10000);
                return;
            }
            file.close();
            statusBar()->clearMessage();
            m_view->playVideoAttachment(index, attIdx, path);
        });
        return;
    }

    if (!att.isImage() && isRiskyFile(att.filename)) {
        QMessageBox::information(this, tr("Attachment"),
                                 tr("For safety, “%1” is not opened directly. Use “Save …” from the message menu instead.")
                                     .arg(att.filename));
        return;
    }

    statusBar()->showMessage(tr("Loading attachment …"));
    fetchAttachmentData(key, att, [this, att, viewerName](const QByteArray &data, const QString &err) {
        statusBar()->clearMessage();
        if (!err.isEmpty()) {
            if (att.isImage() && !att.thumb.isEmpty()) { // fall back to the stored preview
                auto *viewer = new ImageViewer(att.thumb, viewerName, this);
                connect(viewer, &ImageViewer::saved, this, &MainWindow::showSavedToast);
                viewer->setAttribute(Qt::WA_DeleteOnClose);
                viewer->show();
                viewer->raise();
                viewer->activateWindow();
            } else {
                QMessageBox::warning(this, tr("Attachment"), err);
            }
            return;
        }
        if (att.isImage()) {
            auto *viewer = new ImageViewer(data, viewerName, this);
            connect(viewer, &ImageViewer::saved, this, &MainWindow::showSavedToast);
            if (viewer->isValid()) {
                viewer->setAttribute(Qt::WA_DeleteOnClose);
                viewer->show();
                viewer->raise();
                viewer->activateWindow();
                return;
            }
            delete viewer;
        }
        if (!m_tmp.isValid()) {
            QMessageBox::warning(this, tr("Attachment"), tr("No temporary directory available."));
            return;
        }
        static int counter = 0;
        QString name = QFileInfo(att.filename).fileName();
        if (name.isEmpty()) name = QStringLiteral("attachment") + QLatin1Char('.') + QMimeDatabase().mimeTypeForName(att.contentType).preferredSuffix();
        const QString dir = m_tmp.path() + QLatin1Char('/') + QString::number(++counter);
        QDir().mkpath(dir);
        const QString path = dir + QLatin1Char('/') + name;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
            QMessageBox::warning(this, tr("Attachment"), tr("The file could not be written."));
            return;
        }
        f.close();
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
}

void MainWindow::toggleAudioAttachment(int msgIdx, int attIdx)
{
    if (!m_convs.contains(m_current)) return;
    const Conversation &conversation = m_convs[m_current];
    if (msgIdx < 0 || msgIdx >= conversation.msgs.size() || attIdx < 0 ||
        attIdx >= conversation.msgs[msgIdx].attachments.size()) return;
    const Message &message = conversation.msgs[msgIdx];
    const Attachment attachment = message.attachments[attIdx];
    if (!attachment.isAudio()) return;

    const bool sameClip = m_audioKey == conversation.key && m_audioMessageTs == message.ts() &&
                          m_audioAttachmentIndex == attIdx;
    if (sameClip && m_audioLoading) return;
    if (sameClip && m_audioPlayer->source().isValid()) {
        if (m_audioPlayer->playbackState() == QMediaPlayer::PlayingState) {
            m_audioPlayer->pause();
        } else {
            if (m_audioDurationMs > 0 && m_audioPositionMs >= m_audioDurationMs)
                m_audioPlayer->setPosition(0);
            m_audioPlayer->play();
        }
        return;
    }

    m_audioPlayer->stop();
    m_audioPlayer->setSource(QUrl());
    m_audioKey = conversation.key;
    m_audioMessageTs = message.ts();
    m_audioAttachmentIndex = attIdx;
    m_audioPositionMs = 0;
    m_audioDurationMs = 0;
    m_audioLoading = true;
    updateAudioPlaybackView();
    statusBar()->showMessage(tr("Loading audio …"));

    fetchAttachmentData(conversation.key, attachment,
                        [this, key = conversation.key, timestamp = message.ts(), attIdx, attachment]
                        (const QByteArray &data, const QString &error) {
        if (m_audioKey != key || m_audioMessageTs != timestamp || m_audioAttachmentIndex != attIdx) return;
        if (!error.isEmpty() || data.isEmpty()) {
            m_audioLoading = false;
            statusBar()->showMessage(error.isEmpty() ? tr("The audio file is empty.") : error, 10000);
            updateAudioPlaybackView();
            return;
        }

        QString suffix = QFileInfo(attachment.filename).suffix();
        if (suffix.isEmpty()) suffix = QMimeDatabase().mimeTypeForName(attachment.contentType).preferredSuffix();
        const QString path = m_tmp.path() + QStringLiteral("/audio-") +
                             QUuid::createUuid().toString(QUuid::WithoutBraces) +
                             (suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
            m_audioLoading = false;
            statusBar()->showMessage(tr("The audio file could not be prepared for playback."), 10000);
            updateAudioPlaybackView();
            return;
        }
        file.close();
        m_audioLoading = false;
        m_audioPositionMs = 0;
        m_audioDurationMs = 0;
        m_audioPlayer->setSource(QUrl::fromLocalFile(path));
        m_audioPlayer->play();
        statusBar()->clearMessage();
        updateAudioPlaybackView();
    });
}

void MainWindow::updateAudioPlaybackView()
{
    if (!m_view || !m_convs.contains(m_current) || m_audioKey != m_current || m_audioMessageTs <= 0 ||
        m_audioAttachmentIndex < 0) {
        if (m_view) m_view->setAudioPlayback(-1, -1, false, false, 0, 0);
        return;
    }
    const int index = findMessage(m_convs[m_current], m_audioMessageTs, {});
    if (index < 0 || m_audioAttachmentIndex >= m_convs[m_current].msgs[index].attachments.size()) {
        m_view->setAudioPlayback(-1, -1, false, false, 0, 0);
        return;
    }
    m_view->setAudioPlayback(index, m_audioAttachmentIndex, m_audioLoading,
                             m_audioPlayer->playbackState() == QMediaPlayer::PlayingState,
                             m_audioPositionMs, m_audioDurationMs);
}

void MainWindow::saveAttachment(int msgIdx, int attIdx)
{
    if (!m_convs.contains(m_current)) return;
    const Conversation &c = m_convs[m_current];
    if (msgIdx < 0 || msgIdx >= c.msgs.size() || attIdx < 0 || attIdx >= c.msgs[msgIdx].attachments.size()) return;
    const Attachment att = c.msgs[msgIdx].attachments[attIdx];
    const QString suggestedName = AttachmentUtil::displayFileName(att, c.msgs[msgIdx].time);

    statusBar()->showMessage(tr("Loading attachment …"));
    fetchAttachmentData(m_current, att, [this, att, suggestedName](const QByteArray &data, const QString &err) {
        statusBar()->clearMessage();
        if (!err.isEmpty()) {
            QMessageBox::warning(this, tr("Attachment"), err);
            return;
        }
        const QString path = QFileDialog::getSaveFileName(this, tr("Save attachment"), suggestedName);
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
            QMessageBox::warning(this, tr("Attachment"), tr("The file could not be written."));
            return;
        }
        f.close();
        showSavedToast(path);
    });
}

// --------------------------------------------------- Updating signal-cli

void MainWindow::checkForUpdates(bool silent)
{
    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/AsamK/signal-cli/releases/latest")));
    req.setRawHeader("User-Agent", "corvus");
    req.setRawHeader("Accept", "application/vnd.github+json");
    QNetworkReply *rep = m_nam.get(req);

    connect(rep, &QNetworkReply::finished, this, [this, rep, silent] {
        rep->deleteLater();
        if (rep->error() != QNetworkReply::NoError) {
            if (!silent)
                QMessageBox::warning(this, tr("Update check"),
                                     tr("Could not reach GitHub:\n%1").arg(rep->errorString()));
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(rep->readAll()).object();
        QString latest = o.value(QStringLiteral("tag_name")).toString();
        if (latest.startsWith(QLatin1Char('v'))) latest.remove(0, 1);
        const QString url = o.value(QStringLiteral("html_url")).toString();

        const bool newer = QVersionNumber::fromString(latest) > QVersionNumber::fromString(m_version);
        if (newer) {
            m_updateLabel->setText(
                tr("<a href=\"%1\">signal-cli %2 available</a>").arg(url.toHtmlEscaped(), latest.toHtmlEscaped()));
        } else {
            m_updateLabel->clear();
        }

        if (silent) return;
        if (newer) {
            QMessageBox box(this);
            box.setWindowTitle(tr("Update available"));
            box.setText(tr("Installed: signal-cli %1\nLatest version: signal-cli %2\n\n"
                           "Update signal-cli the way you usually do (package manager or download) – this GUI "
                           "does not need to be changed for that. Afterwards choose “File → Reconnect”.")
                            .arg(m_version, latest));
            QPushButton *open = box.addButton(tr("Open release page"), QMessageBox::AcceptRole);
            box.addButton(QMessageBox::Close);
            box.exec();
            if (box.clickedButton() == open) QDesktopServices::openUrl(QUrl(url));
        } else {
            QMessageBox::information(this, tr("Update check"), tr("signal-cli %1 is up to date.").arg(m_version));
        }
    });
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About Corvus"),
                       tr("<b>Corvus</b> %1<br><br>"
                          "Graphical front end for <a href=\"https://github.com/AsamK/signal-cli\">signal-cli</a>.<br>"
                          "signal-cli is used as a standalone program and is not part of this GUI.<br><br>"
                          "Path: %2<br>Version: %3<br><br>"
                          "Chat history (encrypted): %4")
                           .arg(QStringLiteral(APP_VERSION), m_cli.executable().toHtmlEscaped(), m_version.toHtmlEscaped(),
                                historyPath().toHtmlEscaped()));
}

void MainWindow::applyAppearance(const Appearance &appearance)
{
    if (appearance == m_appearance) return;
    m_appearance = appearance;
    m_appearance.save(palette());
    m_view->setAppearance(m_appearance);
    updateSendButton();
    AppIcon::setMode(iconMode(m_appearance));
    refreshIcons();
    refreshList(); // shows or hides the avatars (and fetches them if needed), re-sorts the list
}

// ----------------------------------------- Tray, notifications, sound

void MainWindow::applyNotificationSettings()
{
    m_tray->setVisible(m_notify.trayIcon && QSystemTrayIcon::isSystemTrayAvailable());
    m_trayUnread = -1; // force an icon refresh
    updateTray();
}

void MainWindow::refreshIcons()
{
    QApplication::setWindowIcon(AppIcon::app());
    setWindowIcon(AppIcon::app());
    if (m_tray) m_tray->setIcon(AppIcon::tray(qMax(m_trayUnread, 0)));
}

void MainWindow::applyTheme()
{
    refreshIcons(); // with "Follow theme" the icon variant changes along

    // Default chat colours derive from the palette; colours the user picked stay as they are.
    // A theme switch arrives both as colorSchemeChanged and as a palette change, so only
    // re-layout the chat when something actually differs.
    if (!m_view) return;
    const Appearance ap = Appearance::load(palette());
    if (ap != m_appearance) {
        m_appearance = ap;
        m_view->setAppearance(m_appearance);
        updateSendButton();
    }
}

void MainWindow::updateTray()
{
    if (!m_tray) return;
    int unread = 0;
    for (const Conversation &c : std::as_const(m_convs)) unread += c.unread;
    if (unread != m_trayUnread) {
        m_trayUnread = unread;
        m_tray->setIcon(AppIcon::tray(unread));
        setWindowTitle(unread > 0 ? QStringLiteral("(%1) Corvus").arg(unread) : QStringLiteral("Corvus"));
    }
    m_tray->setToolTip(unread > 0 ? tr("Corvus – %n unread message(s)", nullptr, unread) : tr("Corvus"));
}

void MainWindow::alertIncoming(const QString &key, const Message &m)
{
    const auto it = m_convs.constFind(key);
    if (it == m_convs.constEnd() || it->muted) return;
    // Nothing to announce while the user is looking at this very conversation.
    if (key == m_current && userIsLooking()) return;

    // Notification and sound are independent switches.
    if (m_notify.notifications) {
        QString preview = m.text.simplified();
        if (preview.size() > 240) preview = preview.left(240) + QStringLiteral("…");
        if (preview.isEmpty() && !m.attachments.isEmpty()) {
            const Attachment &a = m.attachments.first();
            preview = a.isImage() ? tr("[Image]") : tr("[File] %1").arg(a.filename);
        }
        QString title = it->title, body;
        switch (m_notify.content) {
        case NotificationSettings::Hidden:
            title = tr("Corvus");
            body = tr("New message");
            break;
        case NotificationSettings::NameOnly:
            body = tr("New message");
            break;
        default:
            body = (it->isGroup && !m.sender.isEmpty()) ? m.sender + QStringLiteral(": ") + preview : preview;
            break;
        }
        // The sender's picture (or the letter placeholder) instead of a generic icon; not when the
        // notification is meant to hide who wrote.
        const QPixmap icon = m_notify.content == NotificationSettings::Hidden ? QPixmap() : avatarPixmap(*it);
        m_notifier->notify(key, title, body, icon);
    }
    if (m_notify.sound) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastSoundMs >= 700) { // a burst of messages should not become a burst of noise
            m_lastSoundMs = now;
            m_sound->play(m_notify);
        }
    }
}

void MainWindow::activateConversation(const QString &key)
{
    if (!m_convs.contains(key)) return;
    showNormal();
    raise();
    activateWindow();
    m_showArchived = m_convs[key].archived;
    openConversation(key);
}

// "All media" of a conversation (images/videos, audio, links, files), like Signal Desktop.
void MainWindow::openMediaGallery(const QString &key)
{
    if (!m_convs.contains(key)) return;
    if (m_gallery) {
        if (m_galleryKey == key) {
            m_gallery->reload();
            m_gallery->show();
            m_gallery->raise();
            m_gallery->activateWindow();
            return;
        }
        m_gallery->close();
    }

    MediaGalleryHost host;
    host.messages = [this, key] { return m_convs.value(key).msgs; };
    host.fetch = [this, key](const Attachment &att, std::function<void(const QByteArray &, const QString &)> cb) {
        fetchAttachmentData(key, att, std::move(cb));
    };
    host.tempDir = [this] { return m_tmp.isValid() ? m_tmp.path() : QString(); };
    host.saved = [this](const QString &path) { showSavedToast(path); };
    host.showInChat = [this, key](qint64 ts) {
        activateConversation(key);
        // Let the chat finish loading before scrolling to the message.
        QTimer::singleShot(0, this, [this, key, ts] {
            if (m_current != key || !m_convs.contains(key)) return;
            const int index = findMessage(m_convs[key], ts, {});
            if (index >= 0) m_view->scrollToMessage(index);
        });
    };

    m_galleryKey = key;
    m_gallery = new MediaGalleryDialog(m_convs[key].title, host, this);
    m_gallery->show();
    m_gallery->raise();
    m_gallery->activateWindow();
}

void MainWindow::setMuted(const QString &key, bool muted)
{
    auto it = m_convs.find(key);
    if (it == m_convs.end() || it->muted == muted) return;
    it->muted = muted;
    persist(Records::mute(key, muted));
    refreshList();
}

void MainWindow::applyNotifications(const NotificationSettings &notify)
{
    if (notify == m_notify) return;
    m_notify = notify;
    m_notify.save();
    applyNotificationSettings();
}

// One window for everything (Settings → Configure Corvus …). General, Appearance and Notifications
// are applied through OK / Apply; the buttons of the other pages act at once and are wired here.
void MainWindow::openSettings(int page)
{
    if (m_settings) { // already open: just bring it to the front
        m_settings->showPage(SettingsDialog::Page(page));
        m_settings->raise();
        m_settings->activateWindow();
        return;
    }
    SettingsDialog dlg(m_appearance, m_notify, m_sound, m_notifier, QSystemTrayIcon::isSystemTrayAvailable(), this);
    m_settings = &dlg;
    m_profilePage = dlg.profilePage();

    connect(&dlg, &SettingsDialog::applied, this, [this](const Appearance &ap, const NotificationSettings &ns) {
        applyAppearance(ap);
        applyNotifications(ns);
    });

    // Profile (talks to Signal)
    connect(m_profilePage, &ProfilePage::usernameChosen, this, &MainWindow::applySignalUsername);
    connect(m_profilePage, &ProfilePage::displayNameChosen, this, [this](const QString &name) {
        sendProfileUpdate({{QStringLiteral("givenName"), name},
                           {QStringLiteral("familyName"), QString()},
                           {QStringLiteral("removeAvatar"), false}},
                          tr("Signal display name updated."), [this, name] {
                              if (m_profilePage) m_profilePage->setCurrentDisplayName(name);
                          });
    });
    connect(m_profilePage, &ProfilePage::avatarChosen, this, [this](const QString &path) {
        // The copy stays in the temporary directory until Corvus quits.
        const QString staged = SignalCli::stageFiles({path}, m_tmp.path() + QStringLiteral("/avatar-") +
                                                                  QUuid::createUuid().toString(QUuid::WithoutBraces)).first();
        sendProfileUpdate({{QStringLiteral("avatar"), staged}, {QStringLiteral("removeAvatar"), false}},
                          tr("Signal avatar updated."), [this, staged] {
                              if (!m_profilePage) return;
                              const QImage img(staged);
                              m_profilePage->setAvatar(img.isNull() ? QPixmap() : roundAvatar(img, 80));
                          });
    });
    connect(m_profilePage, &ProfilePage::avatarRemoveRequested, this, [this] {
        sendProfileUpdate({{QStringLiteral("removeAvatar"), true}}, tr("Signal avatar removed."), [this] {
            if (m_profilePage) m_profilePage->setAvatar(QPixmap());
        });
    });

    // Privacy, Advanced
    connect(dlg.privacyPage(), &PrivacyPage::changeProtectionRequested, this, [this] {
        changeHistoryProtection();
        syncSettingsDialog();
    });
    AdvancedPage *adv = dlg.advancedPage();
    connect(adv, &AdvancedPage::chooseExecutableRequested, this, [this] {
        chooseExecutable();
        syncSettingsDialog();
    });
    connect(adv, &AdvancedPage::checkForUpdateRequested, this, [this] { checkForUpdates(false); });
    connect(adv, &AdvancedPage::reconnectRequested, this, [this] {
        reconnect();
        syncSettingsDialog();
    });
    connect(adv, &AdvancedPage::giphyKeyRequested, this, [this] {
        configureGiphyApiKey();
        syncSettingsDialog();
    });

    syncSettingsDialog();
    dlg.showPage(SettingsDialog::Page(page));
    if (page == SettingsDialog::Profile && m_profilePage && !m_account.isEmpty() && m_cli.isRunning()) loadSignalUsername();
    connect(&dlg, &SettingsDialog::pageChanged, this, [this](int p) {
        if (p == SettingsDialog::Profile && !m_account.isEmpty() && m_cli.isRunning()) loadSignalUsername();
    });
    dlg.exec();
    m_settings = nullptr;
    m_profilePage = nullptr;
}

// Brings the pages that show state of the application (connection, paths, key) up to date.
void MainWindow::syncSettingsDialog()
{
    if (!m_settings) return;
    m_settings->profilePage()->setConnected(!m_account.isEmpty() && m_cli.isRunning());
    m_settings->privacyPage()->setHistory(historyPath(), m_historyProtectionMode, m_store.isOpen());
    m_settings->advancedPage()->setSignalCli(m_cli.executable(), m_version);
    QString key, error;
    const bool known = GiphyApiKeyStore::load(key, error);
    m_settings->advancedPage()->setGiphyKeyState(known, !key.isEmpty());
}

void MainWindow::reportProfileStatus(const QString &message, bool error)
{
    statusBar()->showMessage(message, 10000);
    if (m_profilePage) m_profilePage->setStatus(message, error);
}

void MainWindow::loadSignalUsername()
{
    m_cli.call(QStringLiteral("listContacts"),
               {{QStringLiteral("recipients"), QJsonArray{m_account}}},
               [this](const QJsonValue &res, const QString &err) {
        if (!m_profilePage) return;
        if (!err.isEmpty()) {
            m_profilePage->setCurrentUsername(tr("unavailable"));
            reportProfileStatus(tr("Could not read your Signal username: %1").arg(err), true);
            return;
        }

        bool accountFound = false;
        QString currentUsername;
        QString currentName;
        for (const QJsonValue &value : res.toArray()) {
            const QJsonObject account = value.toObject();
            if (account.value(QStringLiteral("number")).toString() == m_account ||
                account.value(QStringLiteral("uuid")).toString() == m_account) {
                accountFound = true;
                currentUsername = account.value(QStringLiteral("username")).toString();
                // The display name is the profile's given + family name (signal-cli also has a
                // plain "name" for contacts; it is the fallback).
                const QJsonObject profile = account.value(QStringLiteral("profile")).toObject();
                QString given = profile.value(QStringLiteral("givenName")).toString();
                QString family = profile.value(QStringLiteral("familyName")).toString();
                if (given.isEmpty() && family.isEmpty()) {
                    given = account.value(QStringLiteral("givenName")).toString();
                    family = account.value(QStringLiteral("familyName")).toString();
                }
                currentName = (given + QLatin1Char(' ') + family).trimmed();
                if (currentName.isEmpty()) currentName = account.value(QStringLiteral("name")).toString();
                break;
            }
        }
        m_profilePage->setCurrentUsername(!accountFound ? tr("unavailable")
                                            : currentUsername.isEmpty() ? tr("not set") : currentUsername);
        if (accountFound) m_profilePage->setCurrentDisplayName(currentName);
        loadOwnAvatar();
    });
}

// The avatar of this account: first the profile picture, then (as the other avatars do) the address book.
void MainWindow::loadOwnAvatar()
{
    if (!m_profilePage || m_account.isEmpty() || !m_cli.isRunning()) return;
    auto show = [this](const QByteArray &data) {
        if (!m_profilePage) return;
        const QImage img = data.isEmpty() ? QImage() : AttachmentUtil::imageFromData(data);
        m_profilePage->setAvatar(img.isNull() ? QPixmap() : roundAvatar(img, 80));
    };
    auto dataOf = [](const QJsonValue &res) {
        return QByteArray::fromBase64(res.toObject().value(QStringLiteral("data")).toString().toLatin1());
    };
    m_cli.call(QStringLiteral("getAvatar"), {{QStringLiteral("profile"), m_account}},
               [this, show, dataOf](const QJsonValue &res, const QString &err) {
                   const QByteArray data = err.isEmpty() ? dataOf(res) : QByteArray();
                   if (!data.isEmpty()) {
                       show(data);
                       return;
                   }
                   m_cli.call(QStringLiteral("getAvatar"), {{QStringLiteral("contact"), m_account}},
                              [show, dataOf](const QJsonValue &res2, const QString &err2) {
                                  show(err2.isEmpty() ? dataOf(res2) : QByteArray());
                              });
               });
}

void MainWindow::applySignalUsername(const QString &username)
{
    if (m_account.isEmpty() || !m_cli.isRunning()) {
        reportProfileStatus(tr("Connect to signal-cli before editing your profile."), true);
        return;
    }
    m_cli.call(QStringLiteral("updateAccount"),
               {{QStringLiteral("username"), username}, {QStringLiteral("deleteUsername"), false}},
               [this](const QJsonValue &result, const QString &updateError) {
        if (!updateError.isEmpty()) {
            reportProfileStatus(tr("Could not change your Signal username: %1").arg(updateError), true);
            return;
        }
        const QString updatedUsername = result.toObject().value(QStringLiteral("username")).toString();
        if (m_profilePage && !updatedUsername.isEmpty()) m_profilePage->setCurrentUsername(updatedUsername);
        reportProfileStatus(updatedUsername.isEmpty() ? tr("Signal username updated.")
                                                      : tr("Signal username: %1").arg(updatedUsername),
                            false);
    });
}

void MainWindow::sendProfileUpdate(const QJsonObject &params, const QString &successMessage,
                                   const std::function<void()> &onSuccess)
{
    if (m_account.isEmpty() || !m_cli.isRunning()) {
        reportProfileStatus(tr("Connect to signal-cli before editing your profile."), true);
        return;
    }
    m_cli.call(QStringLiteral("updateProfile"), params, [this, successMessage, onSuccess](const QJsonValue &, const QString &err) {
        if (err.isEmpty() && onSuccess) onSuccess();
        reportProfileStatus(err.isEmpty() ? successMessage
                                          : tr("Could not update your Signal profile: %1").arg(err),
                            !err.isEmpty());
    });
}

void MainWindow::configureGiphyApiKey()
{
    QString currentKey;
    QString error;
    if (!GiphyApiKeyStore::load(currentKey, error)) {
        QMessageBox::warning(this, tr("GIPHY API key"), error);
        return;
    }
    bool accepted = false;
    const QString apiKey = QInputDialog::getText(
        this, tr("GIPHY API key"),
        tr("Enter your GIPHY API key. Create one at developers.giphy.com/dashboard."),
        QLineEdit::Password, currentKey, &accepted).trimmed();
    if (!accepted) return;
    const bool stored = apiKey.isEmpty()
        ? GiphyApiKeyStore::clear(error)
        : GiphyApiKeyStore::save(apiKey, error);
    if (!stored) QMessageBox::warning(this, tr("GIPHY API key"), error);
}

// Emoji / GIF popup above the message field.
void MainWindow::showComposerPicker()
{
    m_pickAction = [this](const QString &e) {
        m_input->insertPlainText(e);
        m_input->setFocus();
    };
    m_picker->setGifsAvailable(true);
    m_picker->popup(m_emojiBtn->mapToGlobal(QPoint(0, -m_picker->height() - 4)));
}

void MainWindow::downloadAndSendGif(const QString &conversationKey, const QUrl &url, const QUrl &sentAnalyticsUrl)
{
    if (!m_convs.contains(conversationKey) || url.scheme() != QLatin1String("https") || !m_tmp.isValid()) {
        statusBar()->showMessage(tr("The selected GIF could not be downloaded."), 8000);
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "corvus");
    QNetworkReply *reply = m_nam.get(request);
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (length > kMaxAttachment) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, sentAnalyticsUrl, conversationKey] {
        const QByteArray data = reply->readAll();
        const QString error = reply->errorString();
        const bool ok = reply->error() == QNetworkReply::NoError && !data.isEmpty() && data.size() <= kMaxAttachment;
        reply->deleteLater();
        if (m_current != conversationKey) {
            statusBar()->showMessage(tr("GIF sending cancelled because the active conversation changed."), 8000);
            return;
        }
        if (!ok) {
            statusBar()->showMessage(tr("Could not download the selected GIF: %1").arg(error), 10000);
            return;
        }

        QString suffix = QFileInfo(reply->url().path()).suffix().toLower();
        if (suffix != QLatin1String("gif")) suffix = QStringLiteral("gif");
        const QString path = m_tmp.path() + QStringLiteral("/giphy-") +
                             QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char('.') + suffix;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
            statusBar()->showMessage(tr("Could not prepare the selected GIF for sending."), 10000);
            return;
        }
        file.close();
        m_pendingGifAnalyticsUrl = sentAnalyticsUrl;
        addPendingFiles({path});
        sendMessage();
    });
    statusBar()->showMessage(tr("Downloading GIF …"));
}

void MainWindow::registerGiphyAnalytics(const QUrl &url)
{
    if (url.scheme() != QLatin1String("https")) return;
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "corvus");
    QNetworkReply *reply = m_nam.get(request);
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}
