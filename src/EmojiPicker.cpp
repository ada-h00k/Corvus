#include "EmojiPicker.h"

#include <QAction>
#include <QFile>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QScreen>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QGuiApplication>

namespace {
struct Category {
    const char *key;
    const char *icon;
    const char *name;
};

const Category kCategories[] = {
    {"Smileys & People", "😀", "Smileys & People"},
    {"Animals & Nature", "🐻", "Animals & Nature"},
    {"Food & Drink", "🍕", "Food & Drink"},
    {"Travel & Places", "🚗", "Travel & Places"},
    {"Activities", "⚽", "Activities"},
    {"Objects", "💡", "Objects"},
    {"Symbols", "🔣", "Symbols"},
    {"Flags", "🏁", "Flags"},
};
} // namespace

QStringList EmojiPicker::quickReactions()
{
    return {QStringLiteral("👍"), QStringLiteral("❤️"), QStringLiteral("😂"),
            QStringLiteral("😮"), QStringLiteral("😢"), QStringLiteral("🙏")};
}

EmojiPicker::EmojiPicker(QWidget *parent) : QFrame(parent, Qt::Popup)
{
    setFrameShape(QFrame::StyledPanel);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    auto *tabs = new QTabWidget;
    lay->addWidget(tabs);

    QFile catalogFile(QStringLiteral(":/emoji/catalog.json"));
    if (!catalogFile.open(QIODevice::ReadOnly))
        qFatal("Could not open embedded emoji catalog");
    const QJsonObject catalog = QJsonDocument::fromJson(catalogFile.readAll()).object();

    for (const Category &cat : kCategories) {
        auto *page = new QWidget;
        auto *grid = new QGridLayout(page);
        grid->setSpacing(2);
        const QJsonArray list = catalog.value(QString::fromUtf8(cat.key)).toArray();
        int i = 0;
        for (const QJsonValue &item : list) {
            const QJsonObject entry = item.toObject();
            const QString e = entry.value(QStringLiteral("emoji")).toString();
            auto *b = new QToolButton;
            b->setText(e);
            b->setAutoRaise(true);
            b->setFixedSize(36, 36);
            QFont f = b->font();
            f.setPointSize(16);
            b->setFont(f);
            connect(b, &QToolButton::clicked, this, [this, e] {
                hide();
                emit emojiSelected(e);
            });
            const QJsonArray tones = entry.value(QStringLiteral("tones")).toArray();
            if (!tones.isEmpty()) {
                auto *menu = new QMenu(b);
                for (const QJsonValue &tone : tones) {
                    const QString variant = tone.toString();
                    QAction *action = menu->addAction(variant);
                    action->setFont(f);
                    connect(action, &QAction::triggered, this, [this, variant] {
                        hide();
                        emit emojiSelected(variant);
                    });
                }
                b->setMenu(menu);
                b->setPopupMode(QToolButton::MenuButtonPopup);
            }
            grid->addWidget(b, i / 8, i % 8);
            ++i;
        }
        grid->setRowStretch(grid->rowCount(), 1);
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        tabs->addTab(scroll, QString::fromUtf8(cat.icon));
        tabs->setTabToolTip(tabs->count() - 1, QString::fromUtf8(cat.name));
    }
    setFixedSize(8 * 38 + 36, 260);
}

void EmojiPicker::popup(const QPoint &globalPos)
{
    QPoint p = globalPos;
    if (const QScreen *s = QGuiApplication::screenAt(globalPos)) {
        const QRect r = s->availableGeometry();
        p.setX(qBound(r.left(), p.x(), r.right() - width()));
        p.setY(qBound(r.top(), p.y(), r.bottom() - height()));
    }
    move(p);
    show();
}
