#pragma once

#include <QByteArray>
#include <QDialog>

// Shows a full-size image with a "Save as …" button and a right-click menu (Copy image, Save as …).
class ImageViewer : public QDialog
{
    Q_OBJECT
public:
    ImageViewer(const QByteArray &data, const QString &filename, QWidget *parent = nullptr, bool allowSave = true);
    bool isValid() const { return m_valid; }

signals:
    void saved(const QString &path); // the image was written to `path`

private:
    QByteArray m_data;
    QString m_filename;
    bool m_valid = false;
};
