#pragma once

#include <QDialog>

#include "Appearance.h"

class ChatView;
class QLabel;
class QPushButton;
class QSlider;

// Settings → Appearance …: bubble colours and corner radius with a live preview.
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
    ChatView *m_preview;
};
