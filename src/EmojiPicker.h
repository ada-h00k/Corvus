#pragma once

#include <QFrame>

// Small popup with categorised emojis. Emits emojiSelected() and hides itself.
class EmojiPicker : public QFrame
{
    Q_OBJECT
public:
    explicit EmojiPicker(QWidget *parent = nullptr);
    void popup(const QPoint &globalPos);

    static QStringList quickReactions();

signals:
    void emojiSelected(const QString &emoji);
};
