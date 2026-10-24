#include "NotificationPage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "Notifier.h"
#include "SoundPlayer.h"

NotificationPage::NotificationPage(const NotificationSettings &current, SoundPlayer *sound, Notifier *notifier,
                                   QWidget *parent)
    : QWidget(parent), m_sound(sound), m_notifier(notifier)
{
    setObjectName(QStringLiteral("notificationPage"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

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
    m_notifyNote = new QLabel;
    m_notifyNote->setWordWrap(true);
    nl->addWidget(m_notifyNote);
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

    lay->addStretch(1);

    for (QCheckBox *c : {m_notify, m_soundOn}) connect(c, &QCheckBox::toggled, this, [this] { refreshEnabled(); });
    connect(m_content, &QComboBox::currentIndexChanged, this, &NotificationPage::changed);
    connect(m_soundBox, &QComboBox::currentIndexChanged, this, [this] { refreshEnabled(); });
    connect(m_pickFile, &QPushButton::clicked, this, &NotificationPage::chooseFile);
    connect(m_testNotify, &QPushButton::clicked, this, [this] {
        if (!m_notifier) return;
        const int c = m_content->currentData().toInt();
        const bool hide = c == NotificationSettings::Hidden;
        const Notifier::Delivery d = m_notifier->notify(
            QStringLiteral("::test"), hide ? tr("Corvus") : tr("Alex"),
            hide || c == NotificationSettings::NameOnly ? tr("New message") : tr("This is a test notification 👋"));
        m_notifyNote->setText(Notifier::describe(d)); // says how it was handed over, or why it could not be
    });
    connect(m_testSound, &QPushButton::clicked, this, [this] {
        if (m_sound) m_sound->play(settings());
    });
    if (m_sound)
        connect(m_sound, &SoundPlayer::failed, this, [this](const QString &msg) { m_soundNote->setText(msg); });

    setSettings(current);
}

NotificationSettings NotificationPage::settings() const
{
    NotificationSettings s = m_base;
    s.notifications = m_notify->isChecked();
    s.content = m_content->currentData().toInt();
    s.sound = m_soundOn->isChecked();
    s.soundId = m_soundBox->currentData().toString();
    s.soundFile = m_customFile;
    return s;
}

void NotificationPage::setSettings(const NotificationSettings &s)
{
    m_base = s;
    m_notify->setChecked(s.notifications);
    m_content->setCurrentIndex(qMax(0, m_content->findData(s.content)));
    m_soundOn->setChecked(s.sound);
    m_customFile = s.soundFile;
    m_soundBox->setCurrentIndex(qMax(0, m_soundBox->findData(s.soundId)));
    refreshEnabled();
}

void NotificationPage::refreshEnabled()
{
    emit changed();
    const bool custom = m_soundBox->currentData().toString() == QLatin1String("custom");
    m_content->setEnabled(m_notify->isChecked());
    m_testNotify->setEnabled(m_notify->isChecked());
    if (!m_notify->isChecked()) m_notifyNote->clear();
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

void NotificationPage::chooseFile()
{
    const QString f = QFileDialog::getOpenFileName(this, tr("Choose a sound"), m_customFile,
                                                   tr("Sounds (*.wav *.ogg *.oga *.flac *.mp3);;All files (*)"));
    if (f.isEmpty()) return;
    m_customFile = f;
    refreshEnabled();
}
