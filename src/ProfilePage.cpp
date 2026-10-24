#include "ProfilePage.h"

#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

ProfilePage::ProfilePage(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("profilePage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_offline = new QLabel(tr("Connect to signal-cli to edit your Signal profile."));
    m_offline->setWordWrap(true);
    m_offline->setVisible(false);
    outer->addWidget(m_offline);
    m_content = new QWidget;
    outer->addWidget(m_content);
    outer->addStretch(1);
    auto *lay = new QVBoxLayout(m_content);
    lay->setContentsMargins(0, 0, 0, 0);

    // --- Signal username
    auto *userBox = new QGroupBox(tr("Signal username"));
    auto *ul = new QVBoxLayout(userBox);
    auto *currentRow = new QHBoxLayout;
    currentRow->addWidget(new QLabel(tr("Current username:")));
    m_currentUsername = new QLabel(tr("loading …"));
    m_currentUsername->setObjectName(QStringLiteral("currentUsernameLabel"));
    m_currentUsername->setTextInteractionFlags(Qt::TextSelectableByMouse);
    currentRow->addWidget(m_currentUsername, 1);
    ul->addLayout(currentRow);
    ul->addWidget(new QLabel(tr("Enter a nickname or a full username with discriminator:")));
    auto *userRow = new QHBoxLayout;
    m_username = new QLineEdit;
    m_username->setObjectName(QStringLiteral("usernameEdit"));
    m_applyUsername = new QPushButton(tr("Change"));
    m_applyUsername->setObjectName(QStringLiteral("usernameApply"));
    m_applyUsername->setEnabled(false);
    userRow->addWidget(m_username, 1);
    userRow->addWidget(m_applyUsername);
    ul->addLayout(userRow);
    lay->addWidget(userBox);

    // --- display name
    auto *nameBox = new QGroupBox(tr("Display name"));
    auto *nl = new QVBoxLayout(nameBox);
    nl->addWidget(new QLabel(tr("Name shown to your Signal contacts:")));
    auto *nameRow = new QHBoxLayout;
    m_displayName = new QLineEdit;
    m_displayName->setObjectName(QStringLiteral("displayNameEdit"));
    m_applyDisplayName = new QPushButton(tr("Change"));
    m_applyDisplayName->setObjectName(QStringLiteral("displayNameApply"));
    m_applyDisplayName->setEnabled(false);
    nameRow->addWidget(m_displayName, 1);
    nameRow->addWidget(m_applyDisplayName);
    nl->addLayout(nameRow);
    lay->addWidget(nameBox);

    // --- avatar
    auto *avatarBox = new QGroupBox(tr("Avatar"));
    auto *al = new QHBoxLayout(avatarBox);
    m_avatarView = new QLabel;
    m_avatarView->setObjectName(QStringLiteral("avatarView"));
    m_avatarView->setFixedSize(80, 80);
    m_avatarView->setAlignment(Qt::AlignCenter);
    m_avatarView->setText(tr("No avatar"));
    m_avatarView->setEnabled(false);
    al->addWidget(m_avatarView);
    m_chooseAvatar = new QPushButton(tr("Choose avatar …"));
    m_chooseAvatar->setObjectName(QStringLiteral("avatarChoose"));
    m_removeAvatar = new QPushButton(tr("Remove avatar"));
    m_removeAvatar->setObjectName(QStringLiteral("avatarRemove"));
    al->addWidget(m_chooseAvatar);
    al->addWidget(m_removeAvatar);
    al->addStretch(1);
    lay->addWidget(avatarBox);

    // --- result of the last action
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("profileStatusLabel"));
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    connect(m_username, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_applyUsername->setEnabled(!t.trimmed().isEmpty());
    });
    connect(m_displayName, &QLineEdit::textChanged, this, [this](const QString &t) {
        // nothing to change while the field still shows the current name
        m_applyDisplayName->setEnabled(!t.trimmed().isEmpty() && t.trimmed() != m_currentName);
    });

    auto sendUsername = [this] {
        const QString v = m_username->text().trimmed();
        if (v.isEmpty()) return;
        setStatus(tr("Saving …"));
        emit usernameChosen(v);
    };
    auto sendDisplayName = [this] {
        const QString v = m_displayName->text().trimmed();
        if (v.isEmpty()) return;
        setStatus(tr("Saving …"));
        emit displayNameChosen(v);
    };
    connect(m_applyUsername, &QPushButton::clicked, this, sendUsername);
    connect(m_username, &QLineEdit::returnPressed, this, sendUsername);
    connect(m_applyDisplayName, &QPushButton::clicked, this, sendDisplayName);
    connect(m_displayName, &QLineEdit::returnPressed, this, sendDisplayName);
    connect(m_chooseAvatar, &QPushButton::clicked, this, &ProfilePage::chooseAvatar);
    connect(m_removeAvatar, &QPushButton::clicked, this, [this] {
        setStatus(tr("Saving …"));
        emit avatarRemoveRequested();
    });

}

void ProfilePage::setConnected(bool connected)
{
    m_offline->setVisible(!connected);
    m_content->setEnabled(connected);
}

void ProfilePage::setCurrentDisplayName(const QString &name)
{
    // Do not overwrite what the user is typing: only follow while the field shows the old value.
    const bool untouched = m_displayName->text().trimmed() == m_currentName;
    m_currentName = name.trimmed();
    if (untouched) m_displayName->setText(m_currentName);
    m_applyDisplayName->setEnabled(!m_displayName->text().trimmed().isEmpty() &&
                                   m_displayName->text().trimmed() != m_currentName);
}

void ProfilePage::setAvatar(const QPixmap &avatar)
{
    if (avatar.isNull()) {
        m_avatarView->setPixmap(QPixmap());
        m_avatarView->setText(tr("No avatar"));
        m_avatarView->setEnabled(false);
        return;
    }
    m_avatarView->setEnabled(true);
    m_avatarView->setPixmap(avatar.scaled(m_avatarView->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void ProfilePage::setCurrentUsername(const QString &text)
{
    m_currentUsername->setText(text);
}

void ProfilePage::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
}

void ProfilePage::chooseAvatar()
{
    QFileDialog picker(window());
    picker.setWindowTitle(tr("Choose avatar"));
    picker.setFileMode(QFileDialog::ExistingFile);
    picker.setNameFilter(tr("Image files (*.png *.jpg *.jpeg *.webp *.gif);;All files (*)"));
    // In a Flatpak only the portal's file chooser can see the user's files.
    if (qEnvironmentVariableIsEmpty("FLATPAK_ID"))
        picker.setOption(QFileDialog::DontUseNativeDialog);
    if (picker.exec() != QDialog::Accepted || picker.selectedFiles().isEmpty()) return;
    setStatus(tr("Saving …"));
    emit avatarChosen(picker.selectedFiles().first());
}
