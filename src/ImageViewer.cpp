#include "ImageViewer.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

ImageViewer::ImageViewer(const QByteArray &data, const QString &filename, QWidget *parent)
    : QDialog(parent), m_data(data), m_filename(filename)
{
    setWindowTitle(filename.isEmpty() ? tr("Image") : filename);
    QImage img = QImage::fromData(data);
    m_valid = !img.isNull();

    auto *lay = new QVBoxLayout(this);
    auto *scroll = new QScrollArea;
    auto *label = new QLabel;
    label->setAlignment(Qt::AlignCenter);
    if (m_valid) {
        QSize maxSize(900, 650);
        if (const QScreen *s = QGuiApplication::primaryScreen()) maxSize = s->availableSize() * 0.8;
        if (img.width() > maxSize.width() || img.height() > maxSize.height())
            img = img.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        label->setPixmap(QPixmap::fromImage(img));
    } else {
        label->setText(tr("This image cannot be displayed."));
    }
    scroll->setWidget(label);
    scroll->setWidgetResizable(true);
    lay->addWidget(scroll, 1);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *save = box->addButton(tr("Save as …"), QDialogButtonBox::ActionRole);
    lay->addWidget(box);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save image"), m_filename);
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(m_data) != m_data.size())
            QMessageBox::warning(this, tr("Save image"), tr("The file could not be written."));
    });
    resize(qMin(img.width() + 60, 1100), qMin(img.height() + 110, 800));
}
