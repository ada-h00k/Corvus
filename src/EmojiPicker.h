#pragma once

#include <QFrame>
#include <QHash>
#include <QStringList>
#include <QUrl>
#include <QVector>

class GiphyPicker;
class QLineEdit;
class QStackedWidget;

namespace emojipicker {
class Grid;
class CategoryBar;
class ModeBar;
struct Entry;
struct Section;
} // namespace emojipicker

// Popup with a search field, recently used emojis and all emojis grouped by
// category. In the message composer a second tab, "GIF", shows GIPHY results
// below the same search field (see setGifsAvailable()). The emoji grid is painted by a single widget, so opening the
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

    // Shows the "Emoji | GIF" tabs. Off for reactions, where a GIF makes no sense.
    void setGifsAvailable(bool available);
    // The GIF tab asks for the key (giphyKeyRequested) whenever it is opened; the
    // answer goes here. `error` is shown instead of the usual hint if it is set.
    void setGiphyApiKey(const QString &apiKey, const QString &error = QString());
    void setGiphyApiBase(const QUrl &apiBase);
    bool isGifTabActive() const;
    // Switches to the GIF tab (no effect unless setGifsAvailable(true)).
    void showGifTab();

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
    // A GIF was chosen; the popup closes.
    void gifSelected(const QUrl &originalUrl, const QUrl &sentAnalyticsUrl);
    // The GIF tab was opened: answer with setGiphyApiKey().
    void giphyKeyRequested();
    // "Configure API key …" was clicked in the GIF tab; the popup closes.
    void giphyConfigureRequested();

private:
    void rebuildSections(bool resetScroll = false);
    void showTonePopup(const emojipicker::Entry &entry, const QRect &globalCellRect);
    void updateSearchResults(const QString &query);

    enum class Mode { Emoji, Gif };
    void setMode(Mode mode);

    QLineEdit *m_searchEdit = nullptr;
    emojipicker::ModeBar *m_modeBar = nullptr;
    QStackedWidget *m_pages = nullptr;
    GiphyPicker *m_gif = nullptr;
    Mode m_mode = Mode::Emoji;
    bool m_gifsAvailable = false;
    emojipicker::Grid *m_grid = nullptr;
    emojipicker::CategoryBar *m_bar = nullptr;
    QVector<emojipicker::Section> *m_browse = nullptr; // categories (+ recent)
    QVector<emojipicker::Entry> *m_all = nullptr;      // flat catalog for search
    QHash<QString, int> m_index;                       // emoji -> m_all index
};
