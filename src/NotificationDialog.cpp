#include "NotificationDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "Notifier.h"
#include "SoundPlayer.h"

NotificationDialog::NotificationDialog(const NotificationSettings &current, SoundPlayer *sound, Notifier *notifier,
                                       bool trayAvailable, QWidget *parent)
    : QDialog(parent), m_sound(sound), m_notifier(notifier)
{
    setWindowTitle(tr("Notifications & sound"));
    auto *lay = new QVBoxLayout(this);

    // --- tray icon
    auto *trayBox = new QGroupBox(tr("Tray icon"));
    auto *tl = new QVBoxLayout(trayBox);
    m_tray = new QCheckBox(tr("Show a tray icon (with an unread counter)"));
    m_closeToTray = new QCheckBox(tr("Keep running in the tray when the window is closed"));
    m_startMinimizedInTray = new QCheckBox(tr("Start minimized in the tray"));
    tl->addWidget(m_tray);
    tl->addWidget(m_closeToTray);
    tl->addWidget(m_startMinimizedInTray);
    if (!trayAvailable) {
        auto *note = new QLabel(tr("No system tray was found on this desktop, so the tray icon cannot be shown. "
                                   "(On GNOME a tray extension is required.)"));
        note->setWordWrap(true);
        tl->addWidget(note);
        m_tray->setEnabled(false);
    }
    lay->addWidget(trayBox);

    // --- notifications
    auto *notifyBox = new QGroupBox(tr("Desktop notifications"));
    auto *nl = new QVBoxLayout(notifyBox);
    m_notify = new QCheckBox(tr("Show a notification for new messages"));
    nl->addWidget(m_notify);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Show:")));
    m_content = new QComboBox;
    m_content->addItem(tr("Name and message"), NotificationSettings::NameAndText);
    m_content->addItem(tr("Name only"), NotificationSettings::NameOnly);
    m_content->addItem(tr("Nothing (just “New message”)"), NotificationSettings::Hidden);
    row->addWidget(m_content, 1);
    m_testNotify = new QPushButton(tr("Test"));
    row->addWidget(m_testNotify);
    nl->addLayout(row);
    lay->addWidget(notifyBox);

    // --- sound
    auto *soundBox = new QGroupBox(tr("Sound"));
    auto *sl = new QVBoxLayout(soundBox);
    m_soundOn = new QCheckBox(tr("Play a sound for new messages"));
    sl->addWidget(m_soundOn);
    auto *srow = new QHBoxLayout;
    srow->addWidget(new QLabel(tr("Sound:")));
    m_soundBox = new QComboBox;
    for (const QString &id : SoundPlayer::builtinIds()) m_soundBox->addItem(SoundPlayer::builtinTitle(id), id);
    m_soundBox->addItem(tr("Custom file …"), QStringLiteral("custom"));
    srow->addWidget(m_soundBox, 1);
    m_pickFile = new QPushButton(tr("Choose file …"));
    srow->addWidget(m_pickFile);
    m_testSound = new QPushButton(tr("Test"));
    srow->addWidget(m_testSound);
    sl->addLayout(srow);
    m_fileLabel = new QLabel;
    m_fileLabel->setWordWrap(true);
    sl->addWidget(m_fileLabel);
    m_soundNote = new QLabel;
    m_soundNote->setWordWrap(true);
    sl->addWidget(m_soundNote);
    lay->addWidget(soundBox);

    auto *note = new QLabel(tr("Notifications and sound are independent of each other. Neither is triggered while "
                               "you are looking at the conversation, nor for chats you muted (right-click a chat)."));
    note->setWordWrap(true);
    lay->addWidget(note);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);

    for (QCheckBox *c : {m_tray, m_notify, m_soundOn}) connect(c, &QCheckBox::toggled, this, [this] { refreshEnabled(); });
    connect(m_soundBox, &QComboBox::currentIndexChanged, this, [this] { refreshEnabled(); });
    connect(m_pickFile, &QPushButton::clicked, this, &NotificationDialog::chooseFile);
    connect(m_testNotify, &QPushButton::clicked, this, [this] {
        if (!m_notifier) return;
        const int c = m_content->currentData().toInt();
        const bool hide = c == NotificationSettings::Hidden;
        m_notifier->notify(QStringLiteral("::test"), hide ? tr("Wren") : tr("Alex"),
                           hide || c == NotificationSettings::NameOnly ? tr("New message") : tr("This is a test notification 👋"));
    });
    connect(m_testSound, &QPushButton::clicked, this, [this] {
        if (m_sound) m_sound->play(settings());
    });
    if (m_sound)
        connect(m_sound, &SoundPlayer::failed, this, [this](const QString &msg) { m_soundNote->setText(msg); });

    setSettings(current);
}

NotificationSettings NotificationDialog::settings() const
{
    NotificationSettings s;
    s.trayIcon = m_tray->isChecked();
    s.closeToTray = m_closeToTray->isChecked();
    s.startMinimizedInTray = m_startMinimizedInTray->isChecked();
    s.notifications = m_notify->isChecked();
    s.content = m_content->currentData().toInt();
    s.sound = m_soundOn->isChecked();
    s.soundId = m_soundBox->currentData().toString();
    s.soundFile = m_customFile;
    return s;
}

void NotificationDialog::setSettings(const NotificationSettings &s)
{
    m_tray->setChecked(s.trayIcon);
    m_closeToTray->setChecked(s.closeToTray);
    m_startMinimizedInTray->setChecked(s.startMinimizedInTray);
    m_notify->setChecked(s.notifications);
    m_content->setCurrentIndex(qMax(0, m_content->findData(s.content)));
    m_soundOn->setChecked(s.sound);
    m_customFile = s.soundFile;
    m_soundBox->setCurrentIndex(qMax(0, m_soundBox->findData(s.soundId)));
    refreshEnabled();
}

void NotificationDialog::refreshEnabled()
{
    const bool custom = m_soundBox->currentData().toString() == QLatin1String("custom");
    m_closeToTray->setEnabled(m_tray->isEnabled() && m_tray->isChecked());
    m_startMinimizedInTray->setEnabled(m_tray->isEnabled() && m_tray->isChecked());
    m_content->setEnabled(m_notify->isChecked());
    m_testNotify->setEnabled(m_notify->isChecked());
    m_soundBox->setEnabled(m_soundOn->isChecked());
    m_pickFile->setEnabled(m_soundOn->isChecked() && custom);
    m_testSound->setEnabled(m_soundOn->isChecked());
    m_fileLabel->setVisible(custom);
    m_fileLabel->setText(m_customFile.isEmpty() ? tr("No file chosen – the Chime is used instead.")
                                                : QFileInfo(m_customFile).fileName());
    if (m_sound && m_sound->playerName().isEmpty())
        m_soundNote->setText(tr("No audio player (pw-play, paplay, aplay, …) was found; only the system beep is available."));
    else
        m_soundNote->clear();
}

void NotificationDialog::chooseFile()
{
    const QString f = QFileDialog::getOpenFileName(this, tr("Choose a sound"), m_customFile,
                                                   tr("Sounds (*.wav *.ogg *.oga *.flac *.mp3);;All files (*)"));
    if (f.isEmpty()) return;
    m_customFile = f;
    refreshEnabled();
}
