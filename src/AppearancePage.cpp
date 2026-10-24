#include "AppearancePage.h"

#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include "AppIcon.h"
#include "ChatView.h"

static QList<Message> sampleMessages()
{
    const QDateTime now = QDateTime::currentDateTime();
    Message a;
    a.time = now.addSecs(-120);
    a.sender = QObject::tr("Alex");
    a.authorKey = QStringLiteral("preview-alex");
    a.text = QObject::tr("Hi! Do you like the new colours? 😀");

    Message b;
    b.time = now.addSecs(-60);
    b.outgoing = true;
    b.authorKey = QStringLiteral("preview-me");
    b.text = QObject::tr("Looks great!");
    b.reactions.insert(QStringLiteral("preview-alex"), QStringLiteral("❤️"));

    Message c;
    c.time = now;
    c.sender = a.sender;
    c.authorKey = a.authorKey;
    c.text = QObject::tr("Perfect, I will keep them.");
    c.quote.valid = true;
    c.quote.id = b.ts();
    c.quote.authorKey = b.authorKey;
    c.quote.text = b.text;
    return {a, b, c};
}

AppearancePage::AppearancePage(const Appearance &current, QWidget *parent) : QWidget(parent), m_ap(current)
{
    setObjectName(QStringLiteral("appearancePage"));
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    m_preview = new ChatView;
    m_preview->setMinimumSize(440, 320);
    m_preview->setSelfKey(QStringLiteral("preview-me"));
    m_preview->setNameResolver([](const QString &k) { return k == QStringLiteral("preview-alex") ? QObject::tr("Alex") : k; });
    lay->addWidget(m_preview, 1);

    auto *form = new QFormLayout;
    addColorRow(form, tr("Your messages – background"), &Appearance::outBg);
    addColorRow(form, tr("Your messages – text"), &Appearance::outText);
    addColorRow(form, tr("Received messages – background"), &Appearance::inBg);
    addColorRow(form, tr("Received messages – text"), &Appearance::inText);
    addColorRow(form, tr("Chat background"), &Appearance::background);

    auto *radiusRow = new QHBoxLayout;
    m_radius = new QSlider(Qt::Horizontal);
    m_radius->setRange(0, 28);
    m_radiusLabel = new QLabel;
    m_radiusLabel->setMinimumWidth(36);
    radiusRow->addWidget(m_radius, 1);
    radiusRow->addWidget(m_radiusLabel);
    form->addRow(tr("Corner radius"), radiusRow);
    m_appIcon = new QComboBox;
    m_appIcon->setObjectName(QStringLiteral("appIcon"));
    // Item index == Appearance::IconChoice; the labels name the raven's colour
    // (IconLight is the dark raven on a white tile and the other way round)
    m_appIcon->addItem(tr("Follow theme"));
    m_appIcon->addItem(QIcon(AppIcon::render(32, 0, false)), tr("Dark"));
    m_appIcon->addItem(QIcon(AppIcon::render(32, 0, true)), tr("Light"));
    form->addRow(tr("App icon"), m_appIcon);
    lay->addLayout(form);
    connect(m_appIcon, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_ap.appIcon = index;
        refresh();
    });
    connect(m_radius, &QSlider::valueChanged, this, [this](int v) {
        m_ap.radius = v;
        refresh();
    });

    auto *resetRow = new QHBoxLayout;
    auto *reset = new QPushButton(tr("Reset to defaults"));
    connect(reset, &QPushButton::clicked, this, [this] {
        // Only what this page edits: the General page's options are not touched.
        const Appearance d = Appearance::defaults(palette());
        Appearance a = m_ap;
        a.background = d.background; a.outBg = d.outBg; a.outText = d.outText; a.inBg = d.inBg;
        a.inText = d.inText; a.radius = d.radius; a.appIcon = d.appIcon;
        setAppearance(a);
    });
    resetRow->addStretch(1);
    resetRow->addWidget(reset);
    lay->addLayout(resetRow);

    m_preview->showMessages(sampleMessages());
    refresh();
}

void AppearancePage::addColorRow(QFormLayout *form, const QString &title, QColor Appearance::*member)
{
    auto *b = new QPushButton;
    b->setMinimumWidth(130);
    form->addRow(title, b);
    m_rows.append({member, b, title});
    connect(b, &QPushButton::clicked, this, [this, member, title] {
        const QColor c = QColorDialog::getColor(m_ap.*member, this, title);
        if (!c.isValid()) return;
        m_ap.*member = c;
        refresh();
    });
}

void AppearancePage::setAppearance(const Appearance &a)
{
    m_ap = a;
    refresh();
}

void AppearancePage::refresh()
{
    for (const Row &r : std::as_const(m_rows)) {
        const QColor c = m_ap.*(r.member);
        r.button->setText(c.name().toUpper());
        r.button->setStyleSheet(QStringLiteral("QPushButton { background-color: %1; color: %2; border: 1px solid gray; "
                                               "border-radius: 4px; padding: 4px; }")
                                    .arg(c.name(), c.lightness() < 128 ? QStringLiteral("white") : QStringLiteral("black")));
    }
    QSignalBlocker blockIcon(m_appIcon);
    m_appIcon->setCurrentIndex(m_ap.appIcon);
    QSignalBlocker block(m_radius);
    m_radius->setValue(m_ap.radius);
    m_radiusLabel->setText(tr("%1 px").arg(m_ap.radius));
    m_preview->setAppearance(m_ap);
    emit changed();
}
