#include "MainWindow.h"

#include "AppearanceDialog.h"
#include "AppIcon.h"
#include "Attachments.h"
#include "ChatView.h"
#include "EmojiPicker.h"
#include "Envelope.h"
#include "ImageViewer.h"
#include "LinkDialog.h"
#include "MessageInput.h"
#include "NotificationDialog.h"
#include "Notifier.h"
#include "PassphraseDialog.h"
#include "Records.h"
#include "SoundPlayer.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <algorithm>

static const QString kFolderKey = QStringLiteral("::archived");
static const QString kBackKey = QStringLiteral("::back");
static const qint64 kMaxThumbSource = 30LL * 1024 * 1024;
static const qint64 kMaxAttachment = 100LL * 1024 * 1024; // Signal's upload limit

MainWindow::MainWindow()
{
    setWindowTitle(QStringLiteral("signal-cli Qt"));
    resize(1020, 680);
    setAcceptDrops(true);

    auto *split = new QSplitter(this);
    m_list = new QListWidget;
    m_list->setMinimumWidth(240);
    split->addWidget(m_list);

    auto *right = new QWidget;
    auto *v = new QVBoxLayout(right);
    m_title = new QLabel(tr("No conversation selected"));
    QFont f = m_title->font();
    f.setBold(true);
    f.setPointSize(f.pointSize() + 2);
    m_title->setFont(f);
    v->addWidget(m_title);

    m_view = new ChatView;
    m_view->setNameResolver([this](const QString &k) { return displayName(k); });
    m_appearance = Appearance::load(palette());
    m_view->setAppearance(m_appearance);
    v->addWidget(m_view, 1);

    // reply / attachment bars above the input
    auto makeBar = [&](QLabel *&label, std::function<void()> onClose) {
        auto *bar = new QWidget;
        auto *l = new QHBoxLayout(bar);
        l->setContentsMargins(4, 0, 4, 0);
        label = new QLabel;
        label->setTextFormat(Qt::PlainText);
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
    m_emojiBtn = new QToolButton;
    m_emojiBtn->setText(QStringLiteral("😊"));
    m_emojiBtn->setToolTip(tr("Emoji"));
    m_input = new MessageInput;
    m_input->setPlaceholderText(tr("Write a message …"));
    m_send = new QPushButton(tr("Send"));
    h->addWidget(m_attachBtn);
    h->addWidget(m_emojiBtn);
    h->addWidget(m_input, 1);
    h->addWidget(m_send);
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

    // Menus
    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("New conversation …"), QKeySequence::New, this, &MainWindow::newConversation);
    file->addAction(tr("Link device …"), this, &MainWindow::linkDevice);
    file->addAction(tr("Reconnect"), QKeySequence(Qt::CTRL | Qt::Key_R), this, &MainWindow::reconnect);
    file->addSeparator();
    file->addAction(tr("Change history passphrase …"), this, &MainWindow::changePassphrase);
    file->addSeparator();
    file->addAction(tr("Quit"), QKeySequence::Quit, this, &MainWindow::quitApplication);

    QMenu *settings = menuBar()->addMenu(tr("&Settings"));
    settings->addAction(tr("Appearance …"), this, &MainWindow::editAppearance);
    settings->addAction(tr("Notifications && sound …"), this, &MainWindow::editNotifications);
    settings->addAction(tr("Path to signal-cli …"), this, &MainWindow::chooseExecutable);

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Check for signal-cli update"), this, [this] { checkForUpdates(false); });
    help->addAction(tr("About"), this, &MainWindow::showAbout);

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
        const bool archived = m_convs[key].archived;
        menu.addAction(archived ? tr("Unarchive") : tr("Archive"), this, [this, key, archived] { setArchived(key, !archived); });
        const bool muted = m_convs[key].muted;
        menu.addAction(muted ? tr("Unmute notifications") : tr("Mute notifications"), this, [this, key, muted] { setMuted(key, !muted); });
        menu.addAction(tr("Delete conversation …"), this, [this, key] { deleteConversation(key); });
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });

    // Chat view actions
    connect(m_view, &ChatView::replyRequested, this, &MainWindow::startReply);
    connect(m_view, &ChatView::editRequested, this, &MainWindow::startEdit);
    connect(m_view, &ChatView::reactionChosen, this, &MainWindow::sendReaction);
    connect(m_view, &ChatView::moreReactionsRequested, this, [this](int idx, const QPoint &gp) {
        m_pickAction = [this, idx](const QString &e) { sendReaction(idx, e); };
        m_picker->popup(gp);
    });
    connect(m_view, &ChatView::attachmentActivated, this, &MainWindow::openAttachment);
    connect(m_view, &ChatView::linkClicked, this, [this](const QString &url) {
        const QUrl u(url);
        if (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https")) QDesktopServices::openUrl(u);
    });
    connect(m_view, &ChatView::attachmentSaveRequested, this, &MainWindow::saveAttachment);

    // Input actions
    connect(m_send, &QPushButton::clicked, this, &MainWindow::sendMessage);
    connect(m_input, &QLineEdit::returnPressed, this, &MainWindow::sendMessage);
    connect(m_input, &MessageInput::escapePressed, this, [this] { cancelEdit(); });
    connect(m_attachBtn, &QToolButton::clicked, this, [this] {
        addPendingFiles(QFileDialog::getOpenFileNames(this, tr("Attach files")));
    });
    connect(m_emojiBtn, &QToolButton::clicked, this, [this] {
        m_pickAction = [this](const QString &e) {
            m_input->insert(e);
            m_input->setFocus();
        };
        m_picker->popup(m_emojiBtn->mapToGlobal(QPoint(0, -m_picker->height() - 4)));
    });
    connect(m_picker, &EmojiPicker::emojiSelected, this, [this](const QString &e) {
        if (m_pickAction) m_pickAction(e);
    });
    // Ctrl+V / Shift+Insert / context menu: screenshots and copied files become attachments
    m_input->setPasteHook([this](const QMimeData *md) { return attachFromMimeData(md); });

    // Tray icon, notifications, sound
    m_notify = NotificationSettings::load();
    setWindowIcon(AppIcon::app());
    m_sound = new SoundPlayer(this);
    m_notifier = new Notifier(this);
    m_tray = new QSystemTrayIcon(AppIcon::tray(0), this);
    m_trayMenu = new QMenu(this);
    m_trayMenu->addAction(tr("Show / hide window"), this, &MainWindow::toggleWindow);
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(tr("Quit"), this, &MainWindow::quitApplication);
    m_tray->setContextMenu(m_trayMenu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) toggleWindow();
    });
    m_notifier->setTrayIcon(m_tray);
    connect(m_notifier, &Notifier::activated, this, &MainWindow::activateConversation);
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this] { m_quitting = true; }); // session end
    applyNotificationSettings();

    // signal-cli
    connect(&m_cli, &SignalCli::envelopeReceived, this, &MainWindow::onEnvelope);
    connect(&m_cli, &SignalCli::stopped, this, [this](const QString &reason) { setConnected(false, reason); });
    connect(&m_cli, &SignalCli::logLine, this, [this](const QString &l) { statusBar()->showMessage(l, 5000); });

    setConnected(false);
    QSettings s;
    restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
    QTimer::singleShot(0, this, &MainWindow::initialise);
}

bool MainWindow::shouldHideOnClose(bool trayVisible, bool closeToTray, bool quitting)
{
    return trayVisible && closeToTray && !quitting;
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
            m_tray->showMessage(tr("signal-cli Qt is still running"),
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
    if (isVisible()) {
        close(); // closeEvent accepts, the last window closing ends the app
    } else {
        m_cli.stop();
        QCoreApplication::quit();
    }
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
    m_input->setEnabled(connected);
    m_send->setEnabled(connected);
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
            if (title.isEmpty()) title = cv2.storedName.isEmpty() ? key : cv2.storedName;
            cv2.title = title;

            // Archive state: follow signal-cli only when *it* changed (e.g. archived on the phone);
            // otherwise keep the local state (e.g. auto-unarchived by a new message).
            if (sig != cv2.sigArchived) {
                cv2.sigArchived = sig;
                cv2.archived = sig;
                persist(Records::archive(key, cv2.archived, sig));
            }
        }
        refreshList();
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
        }
        refreshList();
    });
}

// ------------------------------------------------- Encrypted history

QString MainWindow::historyPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/history.scq");
}

bool MainWindow::unlockHistory()
{
    const QString path = historyPath();
    const bool exists = HistoryStore::exists(path);
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
            while (at > 0 && c.msgs[at - 1].time > m.time) --at;
            c.msgs.insert(at, m);
        } else if (t == QLatin1String("n")) {
            Conversation &c = conv(key, key, group);
            c.storedName = o.value(QStringLiteral("n")).toString();
            if (!c.storedName.isEmpty()) c.title = c.storedName;
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
        if (c.archived || c.sigArchived) out.append(Records::archive(c.key, c.archived, c.sigArchived));
        if (c.muted) out.append(Records::mute(c.key, true));
        for (const Message &m : c.msgs) {
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
    if (c.title != c.key && c.title != name) return; // a saved contact name wins
    c.storedName = name;
    c.title = name;
    persist(Records::name(key, name));
    refreshList();
}

void MainWindow::changePassphrase()
{
    QString error;
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
    if (key == m_current) m_current.clear();
    const int rc = m_store.rewrite(allRecords()); // compacts the file, old ciphertext is gone
    if (rc != SC_OK)
        statusBar()->showMessage(tr("Could not update history: %1").arg(HistoryStore::errorText(rc)), 10000);
    refreshList();
    showConversation();
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

    int at = c.msgs.size();
    while (at > 0 && c.msgs[at - 1].time > m.time) --at;
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
    if (!m.outgoing && !(key == m_current && isVisible() && !isMinimized()))
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
                         [](const Message &a, const Message &b) { return a.time < b.time; });
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
    if (emoji.isEmpty()) m.reactions.remove(reactor); else m.reactions[reactor] = emoji;
    persist(Records::reaction(key, m.ts(), effectiveAuthor(c, m), reactor, emoji)); // original ts = stable identity
    if (key == m_current) showConversation(true);
    return true;
}

bool MainWindow::applyEdit(const QString &key, qint64 targetTs, const QStringList &authors, const QString &text,
                           qint64 newTs)
{
    auto it = m_convs.find(key);
    if (it == m_convs.end()) return false;
    Conversation &c = it.value();
    const int i = findMessage(c, targetTs, authors);
    if (i < 0) return false;
    Message &m = c.msgs[i];
    if (newTs <= m.latestTs()) return false; // already applied / outdated revision
    m.text = text;
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

// ------------------------------------------------------------ List / view

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

    std::sort(items.begin(), items.end(), [](const Conversation *a, const Conversation *b) {
        const QDateTime ta = a->msgs.isEmpty() ? QDateTime() : a->msgs.last().time;
        const QDateTime tb = b->msgs.isEmpty() ? QDateTime() : b->msgs.last().time;
        if (ta != tb) return ta > tb;
        return a->title.localeAwareCompare(b->title) < 0;
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
        QString text = (c->muted ? QStringLiteral("🔕 ") : QString()) + (c->isGroup ? QStringLiteral("👥 ") + c->title : c->title);
        auto *it = new QListWidgetItem;
        if (c->unread > 0) {
            text += QStringLiteral(" (%1)").arg(c->unread);
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
        }
        it->setText(text);
        it->setData(Qt::UserRole, c->key);
        m_list->addItem(it);
        if (c->key == m_current) toSelect = it;
    }
    if (toSelect) m_list->setCurrentItem(toSelect);
    updateTray();
}

void MainWindow::onSelectionChanged(QListWidgetItem *item)
{
    if (!item) return;
    const QString key = item->data(Qt::UserRole).toString();
    if (key == kFolderKey || key == kBackKey) return; // handled on click
    openConversation(key);
}

void MainWindow::openConversation(const QString &key)
{
    if (!m_convs.contains(key)) return;
    if (key != m_current) cancelEdit();
    m_current = key;
    m_convs[m_current].unread = 0;
    m_replyQuote = Quote();
    updateReplyBar();
    showConversation();
    refreshList();
    m_notifier->close(key); // the notification for this chat is obsolete now
    m_input->setFocus();
}

void MainWindow::showConversation(bool keepScroll)
{
    if (!m_convs.contains(m_current)) {
        m_title->setText(tr("No conversation selected"));
        m_view->clearView();
        return;
    }
    const Conversation &c = m_convs[m_current];
    m_title->setText(c.title);
    m_view->setGroupMode(c.isGroup);
    m_view->showMessages(c.msgs, keepScroll);
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
    const QString number = QInputDialog::getText(this, tr("New conversation"),
                                                 tr("Phone number (international format, e.g. +4915112345678):"),
                                                 QLineEdit::Normal, QString(), &ok)
                               .trimmed();
    if (!ok || number.isEmpty()) return;
    Conversation &c = conv(number);
    if (c.archived) setArchived(number, false);
    m_showArchived = false;
    m_current = number;
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

    if (pe.kind == ParsedEnvelope::Reaction) {
        // The reaction's group id sits in the same dataMessage; groupId was not parsed for reactions.
        const QString gid = env.value(QStringLiteral("dataMessage")).toObject().value(QStringLiteral("groupInfo")).toObject()
                                .value(QStringLiteral("groupId")).toString();
        const QString rkey = !gid.isEmpty() ? QStringLiteral("g:") + gid : key;
        const QString reactor = pe.outgoing ? m_account : srcKey;
        applyReaction(rkey, pe.targetTs, {pe.targetNumber, pe.targetUuid}, reactor, pe.remove ? QString() : pe.emoji);
        return;
    }

    if (pe.kind == ParsedEnvelope::Edit) {
        // Like the official clients: an edit is only applied to a known message written by the
        // same author. Anything else (unknown target, someone else's message) is dropped.
        const QString author = pe.outgoing ? m_account : srcKey;
        applyEdit(key, pe.targetTs, {author}, pe.msg.text, pe.msg.ts());
        return;
    }

    Message m = pe.msg;
    m.outgoing = pe.outgoing;
    m.authorKey = pe.outgoing ? m_account : srcKey;
    if (m.quote.valid) m.quote.authorKey = resolveKey(m.quote.authorKey);

    if (!pe.outgoing) {
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
    if (!pe.outgoing) alertIncoming(key, m);
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
        sendEdit(m_input->text().trimmed());
        return;
    }
    const QString text = m_input->text().trimmed();
    if ((text.isEmpty() && m_pendingFiles.isEmpty()) || !m_convs.contains(m_current)) return;

    const Conversation c = m_convs[m_current];
    const QStringList files = m_pendingFiles;
    const Quote quote = m_replyQuote;

    QJsonObject p;
    if (!text.isEmpty()) p[QStringLiteral("message")] = text;

    QList<Attachment> atts;
    QJsonArray paths;
    for (const QString &f : files) {
        const QFileInfo fi(f);
        Attachment a;
        a.path = fi.absoluteFilePath();
        a.filename = fi.fileName();
        a.size = fi.size();
        a.contentType = AttachmentUtil::mimeTypeForFile(f);
        if (a.isImage()) {
            const QImage img = AttachmentUtil::loadImage(f);
            if (!img.isNull()) {
                a.width = img.width();
                a.height = img.height();
                a.thumb = AttachmentUtil::makeThumbnail(img);
            }
        }
        atts << a;
        paths.append(a.path);
    }
    if (!paths.isEmpty()) p[QStringLiteral("attachments")] = paths;

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
    qint64 temporaryTimestamp = preview.ts();
    Conversation &target = m_convs[m_current];
    while (findMessage(target, temporaryTimestamp, {}) >= 0) {
        ++temporaryTimestamp;
        preview.time = QDateTime::fromMSecsSinceEpoch(temporaryTimestamp);
    }
    preview.text = text;
    preview.outgoing = true;
    preview.authorKey = m_account;
    preview.quote = quote;
    preview.attachments = atts;

    const QString key = c.key;
    const QString pendingId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    PendingSend pending;
    pending.conversationKey = key;
    pending.temporaryTimestamp = temporaryTimestamp;
    pending.message = preview;
    m_pendingSends.insert(pendingId, pending);

    int at = target.msgs.size();
    while (at > 0 && target.msgs[at - 1].time > preview.time) --at;
    target.msgs.insert(at, preview);

    m_input->clear();
    m_pendingFiles.clear();
    m_replyQuote = Quote();
    updateAttachBar();
    updateReplyBar();
    if (!files.isEmpty()) statusBar()->showMessage(tr("Sending …"));
    if (key == m_current) showConversation(true);

    m_cli.call(QStringLiteral("send"), p,
               [this, pendingId, key, text, files, quote](const QJsonValue &res, const QString &err) {
        auto pendingIt = m_pendingSends.find(pendingId);
        if (pendingIt == m_pendingSends.end()) return;

        if (!err.isEmpty() && !pendingIt->echoReceived) {
            const PendingSend pending = pendingIt.value();
            removePendingPreview(pending);
            m_pendingSends.erase(pendingIt);
            statusBar()->showMessage(tr("Sending failed: %1").arg(err), 10000);
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
            return;
        }

        qint64 ts = res.toObject().value(QStringLiteral("timestamp")).toVariant().toLongLong();
        if (ts <= 0) ts = QDateTime::currentMSecsSinceEpoch();
        statusBar()->clearMessage();
        finalizePendingSend(pendingId, ts);
    });
}

void MainWindow::startEdit(int idx)
{
    if (!m_convs.contains(m_current) || idx < 0 || idx >= m_convs[m_current].msgs.size()) return;
    const Conversation &c = m_convs[m_current];
    const Message &m = c.msgs[idx];
    if (m.text.contains(QLatin1Char('\n'))) {
        statusBar()->showMessage(tr("Multi-line messages cannot be edited here yet."), 6000);
        return;
    }
    if (!m.canEdit(QDateTime::currentMSecsSinceEpoch())) {
        statusBar()->showMessage(tr("This message can no longer be edited (3 hours / 10 edits)."), 6000);
        return;
    }
    if (m_editKey.isEmpty()) m_editDraft = m_input->text(); // keep an already running edit's draft
    m_editKey = c.key;
    m_editTs = m.ts();
    m_input->setText(m.text);
    m_input->setCursorPosition(m.text.size());
    updateEditBar();
    m_input->setFocus();
}

void MainWindow::cancelEdit(bool restoreDraft)
{
    if (m_editKey.isEmpty()) return;
    m_editKey.clear();
    m_editTs = 0;
    if (restoreDraft) m_input->setText(m_editDraft);
    m_editDraft.clear();
    updateEditBar();
}

void MainWindow::updateEditBar()
{
    if (m_editKey.isEmpty()) {
        m_editBar->hide();
        return;
    }
    m_editLabel->setText(tr("✎ Editing message – Enter to save, Esc to cancel"));
    m_editBar->show();
}

void MainWindow::sendEdit(const QString &text)
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
    if (text == m.text) { // nothing changed
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
    if (m.quote.valid && !m.quote.authorKey.isEmpty()) { // an edit keeps the reply it belongs to
        p[QStringLiteral("quoteTimestamp")] = QJsonValue(static_cast<qint64>(m.quote.id));
        p[QStringLiteral("quoteAuthor")] = m.quote.authorKey;
        p[QStringLiteral("quoteMessage")] = m.quote.text;
    }
    addRecipient(p, c);

    const QString key = c.key;
    const qint64 origTs = m.ts();
    m_cli.call(QStringLiteral("send"), p, [this, key, origTs, text](const QJsonValue &res, const QString &err) {
        if (!err.isEmpty()) {
            // stay in edit mode with the typed text so nothing is lost
            statusBar()->showMessage(tr("Editing failed: %1").arg(err), 10000);
            return;
        }
        statusBar()->clearMessage();
        qint64 ts = res.toObject().value(QStringLiteral("timestamp")).toVariant().toLongLong();
        if (ts <= 0) ts = QDateTime::currentMSecsSinceEpoch();
        applyEdit(key, origTs, {m_account}, text, ts);
        if (m_editKey == key && m_editTs == origTs) cancelEdit();
    });
}

void MainWindow::sendReaction(int idx, const QString &emoji)
{
    if (!m_convs.contains(m_current) || idx < 0 || idx >= m_convs[m_current].msgs.size()) return;
    const Conversation c = m_convs[m_current];
    const Message m = c.msgs[idx];
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

static bool isRiskyFile(const QString &name)
{
    static const QStringList ext = {QStringLiteral("exe"),  QStringLiteral("bat"), QStringLiteral("cmd"),
                                    QStringLiteral("com"),  QStringLiteral("scr"), QStringLiteral("msi"),
                                    QStringLiteral("ps1"),  QStringLiteral("vbs"), QStringLiteral("js"),
                                    QStringLiteral("jar"),  QStringLiteral("sh"),  QStringLiteral("desktop"),
                                    QStringLiteral("appimage"), QStringLiteral("run"), QStringLiteral("lnk"),
                                    QStringLiteral("reg"),  QStringLiteral("dll"), QStringLiteral("app"),
                                    QStringLiteral("deb"),  QStringLiteral("rpm"), QStringLiteral("apk")};
    return ext.contains(QFileInfo(name).suffix().toLower());
}

void MainWindow::openAttachment(int msgIdx, int attIdx)
{
    if (!m_convs.contains(m_current)) return;
    const Conversation &c = m_convs[m_current];
    if (msgIdx < 0 || msgIdx >= c.msgs.size() || attIdx < 0 || attIdx >= c.msgs[msgIdx].attachments.size()) return;
    const Attachment att = c.msgs[msgIdx].attachments[attIdx];
    const QString key = m_current;

    if (!att.isImage() && isRiskyFile(att.filename)) {
        QMessageBox::information(this, tr("Attachment"),
                                 tr("For safety, “%1” is not opened directly. Use “Save …” from the message menu instead.")
                                     .arg(att.filename));
        return;
    }

    statusBar()->showMessage(tr("Loading attachment …"));
    fetchAttachmentData(key, att, [this, att](const QByteArray &data, const QString &err) {
        statusBar()->clearMessage();
        if (!err.isEmpty()) {
            if (att.isImage() && !att.thumb.isEmpty()) { // fall back to the stored preview
                ImageViewer(att.thumb, att.filename, this).exec();
            } else {
                QMessageBox::warning(this, tr("Attachment"), err);
            }
            return;
        }
        if (att.isImage()) {
            ImageViewer viewer(data, att.filename, this);
            if (viewer.isValid()) {
                viewer.exec();
                return;
            }
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

void MainWindow::saveAttachment(int msgIdx, int attIdx)
{
    if (!m_convs.contains(m_current)) return;
    const Conversation &c = m_convs[m_current];
    if (msgIdx < 0 || msgIdx >= c.msgs.size() || attIdx < 0 || attIdx >= c.msgs[msgIdx].attachments.size()) return;
    const Attachment att = c.msgs[msgIdx].attachments[attIdx];

    statusBar()->showMessage(tr("Loading attachment …"));
    fetchAttachmentData(m_current, att, [this, att](const QByteArray &data, const QString &err) {
        statusBar()->clearMessage();
        if (!err.isEmpty()) {
            QMessageBox::warning(this, tr("Attachment"), err);
            return;
        }
        const QString path = QFileDialog::getSaveFileName(this, tr("Save attachment"), QFileInfo(att.filename).fileName());
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
            QMessageBox::warning(this, tr("Attachment"), tr("The file could not be written."));
    });
}

// --------------------------------------------------- Updating signal-cli

void MainWindow::checkForUpdates(bool silent)
{
    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/AsamK/signal-cli/releases/latest")));
    req.setRawHeader("User-Agent", "signal-cli-qt");
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
    QMessageBox::about(this, tr("About signal-cli Qt"),
                       tr("<b>signal-cli Qt</b> %1<br><br>"
                          "Graphical front end for <a href=\"https://github.com/AsamK/signal-cli\">signal-cli</a>.<br>"
                          "signal-cli is used as a standalone program and is not part of this GUI.<br><br>"
                          "Path: %2<br>Version: %3<br><br>"
                          "Chat history (encrypted): %4")
                           .arg(QStringLiteral(APP_VERSION), m_cli.executable().toHtmlEscaped(), m_version.toHtmlEscaped(),
                                historyPath().toHtmlEscaped()));
}

void MainWindow::editAppearance()
{
    AppearanceDialog dlg(m_appearance, this);
    if (dlg.exec() != QDialog::Accepted) return;
    m_appearance = dlg.appearance();
    m_appearance.save(palette());
    m_view->setAppearance(m_appearance);
}

// ----------------------------------------- Tray, notifications, sound

void MainWindow::applyNotificationSettings()
{
    m_tray->setVisible(m_notify.trayIcon && QSystemTrayIcon::isSystemTrayAvailable());
    m_trayUnread = -1; // force an icon refresh
    updateTray();
}

void MainWindow::updateTray()
{
    if (!m_tray) return;
    int unread = 0;
    for (const Conversation &c : std::as_const(m_convs)) unread += c.unread;
    if (unread != m_trayUnread) {
        m_trayUnread = unread;
        m_tray->setIcon(AppIcon::tray(unread));
        setWindowTitle(unread > 0 ? QStringLiteral("(%1) signal-cli Qt").arg(unread) : QStringLiteral("signal-cli Qt"));
    }
    m_tray->setToolTip(unread > 0 ? tr("signal-cli Qt – %n unread message(s)", nullptr, unread) : tr("signal-cli Qt"));
}

void MainWindow::alertIncoming(const QString &key, const Message &m)
{
    const auto it = m_convs.constFind(key);
    if (it == m_convs.constEnd() || it->muted) return;
    // Nothing to announce while the user is looking at this very conversation.
    if (isVisible() && !isMinimized() && isActiveWindow() && key == m_current) return;

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
            title = tr("signal-cli Qt");
            body = tr("New message");
            break;
        case NotificationSettings::NameOnly:
            body = tr("New message");
            break;
        default:
            body = (it->isGroup && !m.sender.isEmpty()) ? m.sender + QStringLiteral(": ") + preview : preview;
            break;
        }
        m_notifier->notify(key, title, body);
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

void MainWindow::setMuted(const QString &key, bool muted)
{
    auto it = m_convs.find(key);
    if (it == m_convs.end() || it->muted == muted) return;
    it->muted = muted;
    persist(Records::mute(key, muted));
    refreshList();
}

void MainWindow::editNotifications()
{
    NotificationDialog dlg(m_notify, m_sound, m_notifier, QSystemTrayIcon::isSystemTrayAvailable(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    m_notify = dlg.settings();
    m_notify.save();
    applyNotificationSettings();
}
