#pragma once

#include <QWidget>

#include "Appearance.h"

class ChatView;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;

// Settings → Appearance: bubble colours, corner radius (live preview) and the app icon.
// appearance() returns the values of these controls; the other fields (avatars, sorting, Send
// button – see the General page) are passed through unchanged from setAppearance().
class AppearancePage : public QWidget
{
    Q_OBJECT
public:
    explicit AppearancePage(const Appearance &current, QWidget *parent = nullptr);

    Appearance appearance() const { return m_ap; }
    void setAppearance(const Appearance &a);

private:
    struct Row {
        QColor Appearance::*member;
        QPushButton *button;
        QString title;
    };
    void addColorRow(class QFormLayout *form, const QString &title, QColor Appearance::*member);
    void refresh();

    Appearance m_ap;
    QList<Row> m_rows;
    QSlider *m_radius;
    QLabel *m_radiusLabel;
    QComboBox *m_appIcon;
    ChatView *m_preview;

signals:
    void changed();
};
