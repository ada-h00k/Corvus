#include "EmojiPicker.h"

#include <QAction>
#include <QFile>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSettings>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
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

constexpr int kRecentLimit = 16;
constexpr int kColumns = 8;
constexpr int kButtonSize = 36;
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
    lay->setSpacing(4);

    // Recently used emojis, like the official Signal client.
    auto *recentScroll = new QScrollArea;
    recentScroll->setFrameShape(QFrame::NoFrame);
    recentScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recentScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recentScroll->setWidgetResizable(true);
    recentScroll->setFixedHeight(40);

    auto *recentPage = new QWidget;
    m_recentLayout = new QHBoxLayout(recentPage);
    m_recentLayout->setContentsMargins(0, 0, 0, 0);
    m_recentLayout->setSpacing(2);
    m_recentLayout->addStretch();
    recentScroll->setWidget(recentPage);
    lay->addWidget(recentScroll);

    // Search is kept above the category tabs. Typing immediately switches
    // the content area to the matching emojis.
    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText(QStringLiteral("Search emojis…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setToolTip(QStringLiteral("Search by emoji name"));
    lay->addWidget(m_searchEdit);

    auto *content = new QStackedWidget;
    lay->addWidget(content, 1);

    auto *searchPageContainer = new QWidget;
    auto *searchContainerLayout = new QVBoxLayout(searchPageContainer);
    searchContainerLayout->setContentsMargins(0, 0, 0, 0);

    m_searchScroll = new QScrollArea;
    m_searchScroll->setWidgetResizable(true);
    m_searchScroll->setFrameShape(QFrame::NoFrame);
    m_searchPage = new QWidget;
    m_searchGrid = new QGridLayout(m_searchPage);
    m_searchGrid->setSpacing(2);
    m_searchGrid->setContentsMargins(0, 0, 0, 0);
    m_searchScroll->setWidget(m_searchPage);
    searchContainerLayout->addWidget(m_searchScroll);
    content->addWidget(searchPageContainer);

    m_tabs = new QTabWidget;
    content->addWidget(m_tabs);

    // Keep the content stack in sync with the search field.
    connect(m_searchEdit, &QLineEdit::textChanged, this,
            [this, content](const QString &text) {
                const bool searching = !text.trimmed().isEmpty();
                content->setCurrentIndex(searching ? 0 : 1);
                updateSearchResults(text);
            });

    QFile catalogFile(QStringLiteral(":/emoji/catalog.json"));
    if (!catalogFile.open(QIODevice::ReadOnly))
        qFatal("Could not open embedded emoji catalog");
    const QJsonObject catalog = QJsonDocument::fromJson(catalogFile.readAll()).object();

    // One button factory is used by categories, search results and recent
    // emojis so skin-tone menus behave consistently everywhere.
    auto makeButton = [this](const QString &e, const QJsonObject &entry) {
        auto *b = new QToolButton;
        b->setText(e);
        b->setAutoRaise(true);
        b->setFixedSize(kButtonSize, kButtonSize);
        QFont f = b->font();
        f.setPointSize(16);
        b->setFont(f);

        connect(b, &QToolButton::clicked, this, [this, e] {
            selectEmoji(e);
        });

        const QJsonArray tones = entry.value(QStringLiteral("tones")).toArray();
        if (!tones.isEmpty()) {
            auto *menu = new QMenu(b);
            for (const QJsonValue &tone : tones) {
                const QString variant = tone.toString();
                QAction *action = menu->addAction(variant);
                action->setFont(f);
                connect(action, &QAction::triggered, this, [this, variant] {
                    selectEmoji(variant);
                });
            }
            b->setMenu(menu);
            b->setPopupMode(QToolButton::MenuButtonPopup);
        }
        return b;
    };

    for (const Category &cat : kCategories) {
        auto *page = new QWidget;
        auto *grid = new QGridLayout(page);
        grid->setSpacing(2);
        const QJsonArray list = catalog.value(QString::fromUtf8(cat.key)).toArray();
        int i = 0;
        for (const QJsonValue &item : list) {
            const QJsonObject entry = item.toObject();
            const QString e = entry.value(QStringLiteral("emoji")).toString();
            grid->addWidget(makeButton(e, entry), i / kColumns, i % kColumns);
            ++i;
        }
        grid->setRowStretch(grid->rowCount(), 1);

        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page);
        m_tabs->addTab(scroll, QString::fromUtf8(cat.icon));
        m_tabs->setTabToolTip(m_tabs->count() - 1, QString::fromUtf8(cat.name));
    }

    // Store the catalog on the picker so the search routine can use it
    // without reparsing the resource file on every keystroke.
    QFile namesFile(QStringLiteral(":/emoji/names.json"));
    if (!namesFile.open(QIODevice::ReadOnly))
        qFatal("Could not open embedded emoji name index");
    const QJsonObject names =
        QJsonDocument::fromJson(namesFile.readAll()).object();

    m_searchEdit->setProperty("emojiCatalog",
                              QJsonDocument(catalog).toJson(QJsonDocument::Compact));
    m_searchEdit->setProperty("emojiNames",
                              QJsonDocument(names).toJson(QJsonDocument::Compact));

    rebuildRecent();
    setFixedSize(kColumns * 38 + 36, 300);

    // Start in the normal category view. QStackedWidget defaults to the
    // first page (the search page), and clear() does not emit textChanged
    // when the field is already empty.
    content->setCurrentIndex(1);
    m_searchEdit->clear();
}

void EmojiPicker::selectEmoji(const QString &emoji)
{
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("emojiPicker/recent")).toStringList();
    recent.removeAll(emoji);
    recent.prepend(emoji);
    while (recent.size() > kRecentLimit)
        recent.removeLast();
    settings.setValue(QStringLiteral("emojiPicker/recent"), recent);

    rebuildRecent();
    hide();
    emit emojiSelected(emoji);
}

void EmojiPicker::rebuildRecent()
{
    if (!m_recentLayout)
        return;

    while (QLayoutItem *item = m_recentLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    QSettings settings;
    const QStringList recent = settings.value(QStringLiteral("emojiPicker/recent")).toStringList();

    for (const QString &emoji : recent) {
        auto *b = new QToolButton;
        b->setText(emoji);
        b->setAutoRaise(true);
        b->setFixedSize(kButtonSize, kButtonSize);
        QFont f = b->font();
        f.setPointSize(16);
        b->setFont(f);
        b->setToolTip(QStringLiteral("Recently used"));
        connect(b, &QToolButton::clicked, this, [this, emoji] {
            selectEmoji(emoji);
        });
        m_recentLayout->addWidget(b);
    }
    m_recentLayout->addStretch();
}

void EmojiPicker::updateSearchResults(const QString &query)
{
    if (!m_searchGrid)
        return;

    while (QLayoutItem *item = m_searchGrid->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    const QString needle = query.trimmed().toCaseFolded();
    if (needle.isEmpty())
        return;

    const QByteArray catalogData =
        m_searchEdit->property("emojiCatalog").toByteArray();
    const QJsonObject catalog =
        QJsonDocument::fromJson(catalogData).object();
    const QJsonObject names =
        QJsonDocument::fromJson(
            m_searchEdit->property("emojiNames").toByteArray()).object();

    int row = 0;
    int column = 0;
    for (const Category &cat : kCategories) {
        const QJsonArray list = catalog.value(QString::fromUtf8(cat.key)).toArray();
        for (const QJsonValue &item : list) {
            const QJsonObject entry = item.toObject();
            const QString emoji = entry.value(QStringLiteral("emoji")).toString();
            const QString name = names.value(emoji).toString();

            if (!name.toCaseFolded().contains(needle) && !emoji.contains(query.trimmed()))
                continue;

            auto *b = new QToolButton;
            b->setText(emoji);
            b->setAutoRaise(true);
            b->setFixedSize(kButtonSize, kButtonSize);
            QFont f = b->font();
            f.setPointSize(16);
            b->setFont(f);
            b->setToolTip(name);

            connect(b, &QToolButton::clicked, this, [this, emoji] {
                selectEmoji(emoji);
            });

            const QJsonArray tones = entry.value(QStringLiteral("tones")).toArray();
            if (!tones.isEmpty()) {
                auto *menu = new QMenu(b);
                for (const QJsonValue &tone : tones) {
                    const QString variant = tone.toString();
                    QAction *action = menu->addAction(variant);
                    action->setFont(f);
                    connect(action, &QAction::triggered, this, [this, variant] {
                        selectEmoji(variant);
                    });
                }
                b->setMenu(menu);
                b->setPopupMode(QToolButton::MenuButtonPopup);
            }

            m_searchGrid->addWidget(b, row, column);
            if (++column == kColumns) {
                column = 0;
                ++row;
            }
        }
    }

    if (column != 0)
        m_searchGrid->setColumnStretch(column, 1);
    m_searchGrid->setRowStretch(row + 1, 1);
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
