#include "SettingsDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "AppearancePage.h"
#include "NotificationPage.h"
#include "ProfilePage.h"

static QLabel *hint(const QString &text)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setEnabled(false); // greyed-out, like a secondary text
    return l;
}

// ===================================================================================== General
GeneralPage::GeneralPage(const Appearance &ap, const NotificationSettings &ns, bool trayAvailable, QWidget *parent)
    : QWidget(parent), m_trayAvailable(trayAvailable)
{
    setObjectName(QStringLiteral("generalPage"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    auto *windowBox = new QGroupBox(tr("Window && tray"));
    auto *wl = new QVBoxLayout(windowBox);
    m_tray = new QCheckBox(tr("Show a tray icon (with an unread counter)"));
    m_closeToTray = new QCheckBox(tr("Keep running in the tray when the window is closed"));
    m_startMinimizedInTray = new QCheckBox(tr("Start minimized in the tray"));
    wl->addWidget(m_tray);
    wl->addWidget(m_closeToTray);
    wl->addWidget(m_startMinimizedInTray);
    if (!trayAvailable) {
        wl->addWidget(hint(tr("No system tray was found on this desktop, so the tray icon cannot be shown. "
                              "(On GNOME a tray extension is required.)")));
        m_tray->setEnabled(false);
    }
    lay->addWidget(windowBox);

    auto *listBox = new QGroupBox(tr("Contact list"));
    auto *ll = new QVBoxLayout(listBox);
    m_showAvatars = new QCheckBox(tr("Show avatars in the contact list"));
    m_showAvatars->setObjectName(QStringLiteral("showAvatars"));
    m_sortAlphabetically = new QCheckBox(tr("Sort contacts alphabetically"));
    m_sortAlphabetically->setObjectName(QStringLiteral("sortAlphabetically"));
    m_sortAlphabetically->setToolTip(
        tr("Keep the contact list in alphabetical order instead of moving chats with new messages to the top"));
    ll->addWidget(m_showAvatars);
    ll->addWidget(m_sortAlphabetically);
    lay->addWidget(listBox);

    auto *inputBox = new QGroupBox(tr("Message field"));
    auto *il = new QVBoxLayout(inputBox);
    m_alwaysShowSend = new QCheckBox(tr("Always show the Send button"));
    m_alwaysShowSend->setObjectName(QStringLiteral("alwaysShowSend"));
    m_alwaysShowSend->setToolTip(
        tr("Show the Send button next to the message field, not only when the field is expanded"));
    m_generateLinkPreviews = new QCheckBox(tr("Generate link previews"));
    m_generateLinkPreviews->setObjectName(QStringLiteral("generateLinkPreviews"));
    m_generateLinkPreviews->setToolTip(tr("Fetch metadata only for HTTPS links and attach a preview when sending a message"));
    il->addWidget(m_alwaysShowSend);
    il->addWidget(m_generateLinkPreviews);
    lay->addWidget(inputBox);
    lay->addStretch(1);

    for (QCheckBox *c : {m_tray, m_closeToTray, m_startMinimizedInTray, m_showAvatars, m_sortAlphabetically, m_alwaysShowSend, m_generateLinkPreviews})
        connect(c, &QCheckBox::toggled, this, [this] {
            refreshEnabled();
            emit changed();
        });
    load(ap, ns);
}

void GeneralPage::load(const Appearance &ap, const NotificationSettings &ns)
{
    const QSignalBlocker b1(m_tray), b2(m_closeToTray), b3(m_startMinimizedInTray), b4(m_showAvatars),
        b5(m_sortAlphabetically), b6(m_alwaysShowSend), b7(m_generateLinkPreviews);
    m_tray->setChecked(ns.trayIcon);
    m_closeToTray->setChecked(ns.closeToTray);
    m_startMinimizedInTray->setChecked(ns.startMinimizedInTray);
    m_showAvatars->setChecked(ap.showAvatars);
    m_sortAlphabetically->setChecked(ap.sortAlphabetically);
    m_alwaysShowSend->setChecked(ap.alwaysShowSend);
    m_generateLinkPreviews->setChecked(ap.generateLinkPreviews);
    refreshEnabled();
}

void GeneralPage::refreshEnabled()
{
    const bool on = m_trayAvailable && m_tray->isChecked();
    m_closeToTray->setEnabled(on);
    m_startMinimizedInTray->setEnabled(on);
}

void GeneralPage::applyTo(Appearance &ap) const
{
    ap.showAvatars = m_showAvatars->isChecked();
    ap.sortAlphabetically = m_sortAlphabetically->isChecked();
    ap.alwaysShowSend = m_alwaysShowSend->isChecked();
    ap.generateLinkPreviews = m_generateLinkPreviews->isChecked();
}

void GeneralPage::applyTo(NotificationSettings &ns) const
{
    ns.trayIcon = m_tray->isChecked();
    ns.closeToTray = m_closeToTray->isChecked();
    ns.startMinimizedInTray = m_startMinimizedInTray->isChecked();
}

// ===================================================================================== Privacy
PrivacyPage::PrivacyPage(const NotificationSettings &settings, QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("privacyPage"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    auto *messagingBox = new QGroupBox(tr("Messaging privacy"));
    auto *ml = new QVBoxLayout(messagingBox);
    m_readReceipts = new QCheckBox(tr("Read receipts"));
    m_readReceipts->setObjectName(QStringLiteral("readReceipts"));
    m_readReceipts->setToolTip(tr("Let contacts know when you have read their messages, and show when they have read yours. Both sides must enable this."));
    m_typingIndicators = new QCheckBox(tr("Typing indicators"));
    m_typingIndicators->setObjectName(QStringLiteral("typingIndicators"));
    m_typingIndicators->setToolTip(tr("Let contacts know when you are typing, and show when they are typing. Both sides must enable this."));
    m_showDeliveryStatus = new QCheckBox(tr("Show delivery status"));
    m_showDeliveryStatus->setObjectName(QStringLiteral("showDeliveryStatus"));
    m_showDeliveryStatus->setToolTip(tr("Show sent, delivered, and read check marks on your outgoing messages. This only affects your local display; receipt handling is unchanged."));
    m_readReceipts->setChecked(settings.readReceipts);
    m_typingIndicators->setChecked(settings.typingIndicators);
    m_showDeliveryStatus->setChecked(settings.showDeliveryStatus);
    ml->addWidget(m_readReceipts);
    ml->addWidget(m_typingIndicators);
    ml->addWidget(m_showDeliveryStatus);
    ml->addWidget(hint(tr("Read receipts and typing indicators are sent and shown only while their respective options are enabled. Show delivery status only controls the check marks in your chat view.")));
    lay->addWidget(messagingBox);

    auto *box = new QGroupBox(tr("Chat history"));
    auto *bl = new QVBoxLayout(box);
    bl->addWidget(new QLabel(tr("Corvus keeps your chat history in an encrypted file:")));
    m_path = new QLabel;
    m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_path->setWordWrap(true);
    bl->addWidget(m_path);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("The key is protected by:")));
    m_mode = new QLabel;
    m_mode->setObjectName(QStringLiteral("historyModeLabel"));
    row->addWidget(m_mode, 1);
    m_change = new QPushButton(tr("Change …"));
    m_change->setObjectName(QStringLiteral("historyChange"));
    row->addWidget(m_change);
    bl->addLayout(row);
    bl->addWidget(hint(tr("A forgotten passphrase cannot be recovered.")));
    lay->addWidget(box);

    lay->addWidget(hint(tr("Corvus only encrypts its own chat history. The data of signal-cli itself (account, "
                           "sessions, contacts, attachments) is not encrypted by Corvus – use your system's disk "
                           "encryption to protect it.")));
    lay->addStretch(1);
    connect(m_change, &QPushButton::clicked, this, &PrivacyPage::changeProtectionRequested);
    connect(m_readReceipts, &QCheckBox::toggled, this, &PrivacyPage::changed);
    connect(m_typingIndicators, &QCheckBox::toggled, this, &PrivacyPage::changed);
    connect(m_showDeliveryStatus, &QCheckBox::toggled, this, &PrivacyPage::changed);
}

void PrivacyPage::applyTo(NotificationSettings &settings) const
{
    settings.readReceipts = m_readReceipts->isChecked();
    settings.typingIndicators = m_typingIndicators->isChecked();
    settings.showDeliveryStatus = m_showDeliveryStatus->isChecked();
}

void PrivacyPage::setHistory(const QString &path, const QString &mode, bool ready)
{
    m_path->setText(path);
    m_mode->setText(mode == QLatin1String("keychain") ? tr("System keychain") : tr("Passphrase"));
    m_change->setEnabled(ready);
}

// ===================================================================================== Advanced
AdvancedPage::AdvancedPage(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("advancedPage"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    auto *cliBox = new QGroupBox(tr("signal-cli"));
    auto *cl = new QVBoxLayout(cliBox);
    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(new QLabel(tr("Path:")));
    m_path = new QLabel;
    m_path->setObjectName(QStringLiteral("signalCliPath"));
    m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_path->setWordWrap(true);
    pathRow->addWidget(m_path, 1);
    cl->addLayout(pathRow);
    auto *verRow = new QHBoxLayout;
    verRow->addWidget(new QLabel(tr("Version:")));
    m_version = new QLabel;
    verRow->addWidget(m_version, 1);
    cl->addLayout(verRow);
    auto *buttons = new QHBoxLayout;
    auto *choose = new QPushButton(tr("Choose …"));
    auto *update = new QPushButton(tr("Check for update"));
    auto *reconnect = new QPushButton(tr("Reconnect"));
    buttons->addWidget(choose);
    buttons->addWidget(update);
    buttons->addWidget(reconnect);
    buttons->addStretch(1);
    cl->addLayout(buttons);
    lay->addWidget(cliBox);

    auto *giphyBox = new QGroupBox(tr("GIPHY"));
    auto *gl = new QHBoxLayout(giphyBox);
    m_giphy = new QLabel;
    m_giphy->setObjectName(QStringLiteral("giphyState"));
    gl->addWidget(m_giphy, 1);
    auto *key = new QPushButton(tr("API key …"));
    gl->addWidget(key);
    lay->addWidget(giphyBox);
    lay->addStretch(1);

    connect(choose, &QPushButton::clicked, this, &AdvancedPage::chooseExecutableRequested);
    connect(update, &QPushButton::clicked, this, &AdvancedPage::checkForUpdateRequested);
    connect(reconnect, &QPushButton::clicked, this, &AdvancedPage::reconnectRequested);
    connect(key, &QPushButton::clicked, this, &AdvancedPage::giphyKeyRequested);
}

void AdvancedPage::setSignalCli(const QString &path, const QString &version)
{
    m_path->setText(path.isEmpty() ? tr("not found") : path);
    m_version->setText(version.isEmpty() ? QStringLiteral("–") : version);
}

void AdvancedPage::setGiphyKeyState(bool known, bool set)
{
    m_giphy->setText(!known ? tr("API key: unavailable") : set ? tr("API key: set") : tr("API key: not set (needed for GIFs)"));
}

// ===================================================================================== Dialog
// Pages are put into a scroll area so the dialog still works on small screens.
static QWidget *scrolled(QWidget *page)
{
    auto *area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(page);
    return area;
}

SettingsDialog::SettingsDialog(const Appearance &ap, const NotificationSettings &ns, SoundPlayer *sound,
                               Notifier *notifier, bool trayAvailable, QWidget *parent)
    : QDialog(parent), m_initialAppearance(ap), m_initialNotify(ns)
{
    setWindowTitle(tr("Settings"));
    setObjectName(QStringLiteral("settingsDialog"));

    m_general = new GeneralPage(ap, ns, trayAvailable);
    m_appearance = new AppearancePage(ap);
    m_notify = new NotificationPage(ns, sound, notifier);
    m_profile = new ProfilePage;
    m_privacy = new PrivacyPage(ns);
    m_advanced = new AdvancedPage;

    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("settingsCategories"));
    m_list->setFixedWidth(170);
    m_list->setIconSize(QSize(20, 20));
    m_stack = new QStackedWidget;

    struct Entry { QString title; QString icon; QWidget *page; };
    const QList<Entry> entries = {
        {tr("General"), QStringLiteral("preferences-system"), m_general},
        {tr("Appearance"), QStringLiteral("preferences-desktop-theme"), m_appearance},
        {tr("Notifications"), QStringLiteral("preferences-desktop-notification"), m_notify},
        {tr("Profile"), QStringLiteral("user-identity"), m_profile},
        {tr("Privacy"), QStringLiteral("security-high"), m_privacy},
        {tr("Advanced"), QStringLiteral("applications-development"), m_advanced},
    };
    for (const Entry &e : entries) {
        auto *item = new QListWidgetItem(QIcon::fromTheme(e.icon), e.title);
        item->setSizeHint(QSize(0, 32));
        m_list->addItem(item);
        m_stack->addWidget(scrolled(e.page));
    }
    connect(m_list, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    connect(m_list, &QListWidget::currentRowChanged, this, &SettingsDialog::pageChanged);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    m_applyButton = box->button(QDialogButtonBox::Apply);
    connect(box, &QDialogButtonBox::accepted, this, [this] {
        apply();
        accept();
    });
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_applyButton, &QPushButton::clicked, this, &SettingsDialog::apply);

    auto *body = new QHBoxLayout;
    body->addWidget(m_list);
    body->addWidget(m_stack, 1);
    auto *lay = new QVBoxLayout(this);
    lay->addLayout(body, 1);
    lay->addWidget(box);

    connect(m_general, &GeneralPage::changed, this, &SettingsDialog::updateButtons);
    connect(m_appearance, &AppearancePage::changed, this, &SettingsDialog::updateButtons);
    connect(m_notify, &NotificationPage::changed, this, &SettingsDialog::updateButtons);
    connect(m_privacy, &PrivacyPage::changed, this, &SettingsDialog::updateButtons);

    resize(760, 560);
    m_list->setCurrentRow(0);
    updateButtons();
}

Appearance SettingsDialog::appearance() const
{
    Appearance a = m_appearance->appearance();
    m_general->applyTo(a);
    return a;
}

NotificationSettings SettingsDialog::notifications() const
{
    NotificationSettings n = m_notify->settings();
    m_general->applyTo(n);
    m_privacy->applyTo(n);
    return n;
}

void SettingsDialog::showPage(Page page)
{
    m_list->setCurrentRow(int(page));
}

void SettingsDialog::updateButtons()
{
    m_applyButton->setEnabled(appearance() != m_initialAppearance || notifications() != m_initialNotify);
}

void SettingsDialog::apply()
{
    m_initialAppearance = appearance();
    m_initialNotify = notifications();
    updateButtons();
    emit applied(m_initialAppearance, m_initialNotify);
}
