#pragma once

#include <QFrame>
#include <QStringList>

class QGridLayout;
class QHBoxLayout;
class QLineEdit;
class QScrollArea;
class QTabWidget;
class QWidget;

// Small popup with categorised emojis, recent emojis and search.
class EmojiPicker : public QFrame
{
    Q_OBJECT
public:
    explicit EmojiPicker(QWidget *parent = nullptr);
    void popup(const QPoint &globalPos);

    static QStringList quickReactions();

signals:
    void emojiSelected(const QString &emoji);

private:
    void selectEmoji(const QString &emoji);
    void rebuildRecent();
    void updateSearchResults(const QString &query);

    QLineEdit *m_searchEdit = nullptr;
    QHBoxLayout *m_recentLayout = nullptr;
    QScrollArea *m_searchScroll = nullptr;
    QWidget *m_searchPage = nullptr;
    QGridLayout *m_searchGrid = nullptr;
    QTabWidget *m_tabs = nullptr;
};
