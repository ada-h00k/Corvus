#pragma once

#include <QFrame>
#include <QHash>
#include <QStringList>
#include <QVector>

class QLineEdit;

namespace emojipicker {
class Grid;
class CategoryBar;
struct Entry;
struct Section;
} // namespace emojipicker

// Popup with a search field, recently used emojis and all emojis grouped by
// category. The emoji grid is painted by a single widget, so opening the
// picker is cheap even though it holds ~1900 emojis.
//
// Left click picks an emoji. Emojis with skin tones show a small marker in the
// corner; right click or press-and-hold opens the tone variants.
class EmojiPicker : public QFrame
{
    Q_OBJECT
public:
    explicit EmojiPicker(QWidget *parent = nullptr);
    ~EmojiPicker() override;
    void popup(const QPoint &globalPos);

    static QStringList quickReactions();

    // Number of emojis in the catalog (without skin tone variants).
    int emojiCount() const;
    // Skin tone variants of `emoji`; empty if it has none.
    QStringList toneVariants(const QString &emoji) const;
    // Emojis currently listed in the grid (recent ones included, in order).
    QStringList visibleEmojis() const;
    // Same as clicking the emoji: stores it as recent, hides and emits.
    void activateEmoji(const QString &emoji);

signals:
    void emojiSelected(const QString &emoji);

private:
    void rebuildSections(bool resetScroll = false);
    void showTonePopup(const emojipicker::Entry &entry, const QRect &globalCellRect);
    void updateSearchResults(const QString &query);

    QLineEdit *m_searchEdit = nullptr;
    emojipicker::Grid *m_grid = nullptr;
    emojipicker::CategoryBar *m_bar = nullptr;
    QVector<emojipicker::Section> *m_browse = nullptr; // categories (+ recent)
    QVector<emojipicker::Entry> *m_all = nullptr;      // flat catalog for search
    QHash<QString, int> m_index;                       // emoji -> m_all index
};
