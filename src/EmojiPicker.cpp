#include "EmojiPicker.h"

#include <QGridLayout>
#include <QScreen>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QGuiApplication>

namespace {
struct Category {
    const char *name;
    const char *emojis; // space separated
};

const Category kCategories[] = {
    {"😀 Smileys",
     "😀 😃 😄 😁 😆 😅 😂 🤣 🙂 🙃 😉 😊 😇 🥰 😍 🤩 😘 😗 😚 😋 😛 😜 🤪 😝 🤗 🤔 🤨 😐 😑 😶 🙄 😏 😣 😥 😮 😯 😪 😫 🥱 😴 😌 😎 🤓 🥳 😕 😟 🙁 😲 😳 🥺 😢 😭 😱 😖 😞 😓 😩 😤 😡 😠 🤬 😈 💀 🤡 👻 🤖"},
    {"👍 Gestures",
     "👍 👎 👌 ✌️ 🤞 🤟 🤘 🤙 👈 👉 👆 👇 ☝️ ✋ 🤚 🖐️ 🖖 👋 👏 🙌 👐 🤲 🙏 💪 🤝 ✍️ 🤳 👀 👁️ 👂 👃 🧠 👄"},
    {"❤️ Symbols",
     "❤️ 🧡 💛 💚 💙 💜 🖤 🤍 🤎 💔 ❣️ 💕 💞 💓 💗 💖 💘 💝 ✨ ⭐ 🌟 💫 🔥 💥 💯 ✅ ❌ ❓ ❗ ⚠️ 🎉 🎊 🏆 🥇"},
    {"🐶 Nature & food",
     "🐶 🐱 🐭 🐹 🐰 🦊 🐻 🐼 🐨 🐯 🦁 🐮 🐷 🐸 🐵 🐔 🐧 🐦 🦆 🦉 🐢 🐍 🐙 🐬 🐳 🌸 🌹 🌻 🌲 🍀 🍎 🍌 🍇 🍓 🍕 🍔 🍟 🌭 🍿 🍩 🍪 🎂 ☕ 🍺 🍷"},
    {"⚽ Objects",
     "⚽ 🏀 🏈 🎾 🎮 🎲 🎯 🎵 🎶 🎤 🎧 📱 💻 ⌚ 📷 💡 📚 ✏️ 📌 📎 🔑 🔒 🎁 🚗 ✈️ 🚀 🏠 ⏰ 💰 📅"},
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

    for (const Category &cat : kCategories) {
        auto *page = new QWidget;
        auto *grid = new QGridLayout(page);
        grid->setSpacing(2);
        const QStringList list = QString::fromUtf8(cat.emojis).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        int i = 0;
        for (const QString &e : list) {
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
            grid->addWidget(b, i / 8, i % 8);
            ++i;
        }
        grid->setRowStretch(grid->rowCount(), 1);
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        const QString title = QString::fromUtf8(cat.name);
        tabs->addTab(scroll, title.section(QLatin1Char(' '), 0, 0)); // icon only
        tabs->setTabToolTip(tabs->count() - 1, title.section(QLatin1Char(' '), 1));
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
