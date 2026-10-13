#pragma once

#include <QDialog>

#include "Appearance.h"

class ChatView;
class QCheckBox;
class QLabel;
class QPushButton;
class QSlider;

// Settings → Appearance …: bubble colours, corner radius (live preview) and the avatar switch.
class AppearanceDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AppearanceDialog(const Appearance &current, QWidget *parent = nullptr);

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
    QCheckBox *m_showAvatars;
    ChatView *m_preview;
};
