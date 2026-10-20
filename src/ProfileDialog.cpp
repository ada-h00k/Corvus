#include "ProfileDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

ProfileDialog::ProfileDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Profile settings"));
    setObjectName(QStringLiteral("profileDialog"));
    auto *lay = new QVBoxLayout(this);

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

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    lay->addWidget(buttons);

    connect(m_username, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_applyUsername->setEnabled(!t.trimmed().isEmpty());
    });
    connect(m_displayName, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_applyDisplayName->setEnabled(!t.trimmed().isEmpty());
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
    connect(m_chooseAvatar, &QPushButton::clicked, this, &ProfileDialog::chooseAvatar);
    connect(m_removeAvatar, &QPushButton::clicked, this, [this] {
        setStatus(tr("Saving …"));
        emit avatarRemoveRequested();
    });

    setMinimumWidth(420);
}

void ProfileDialog::setCurrentUsername(const QString &text)
{
    m_currentUsername->setText(text);
}

void ProfileDialog::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
}

void ProfileDialog::chooseAvatar()
{
    QFileDialog picker(this);
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
