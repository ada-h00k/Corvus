#include "ImageViewer.h"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace {
class FittedImageLabel : public QLabel
{
public:
    using QLabel::QLabel;

    void setSource(const QPixmap &pixmap)
    {
        m_source = pixmap;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QLabel::paintEvent(event);
        if (m_source.isNull()) return;

        QSize fitted = m_source.size();
        fitted.scale(size(), Qt::KeepAspectRatio);
        const QRect target((width() - fitted.width()) / 2, (height() - fitted.height()) / 2,
                           fitted.width(), fitted.height());
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(target, m_source);
    }

private:
    QPixmap m_source;
};
} // namespace

ImageViewer::ImageViewer(const QByteArray &data, const QString &filename, QWidget *parent)
    : QDialog(parent), m_data(data), m_filename(filename)
{
    setWindowTitle(filename.isEmpty() ? tr("Image") : filename);
    QImage img = QImage::fromData(data);
    m_valid = !img.isNull();

    auto *lay = new QVBoxLayout(this);
    auto *scroll = new QScrollArea;
    auto *label = new FittedImageLabel;
    label->setAlignment(Qt::AlignCenter);
    label->setMinimumSize(1, 1);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    QSize maxSize(900, 650);
    if (const QScreen *s = QGuiApplication::primaryScreen()) maxSize = s->availableSize() * 0.8;
    maxSize = maxSize.boundedTo(QSize(1100, 800));
    if (m_valid) {
        label->setSource(QPixmap::fromImage(img));
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
    auto saveAs = [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save image"), m_filename);
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(m_data) != m_data.size()) {
            QMessageBox::warning(this, tr("Save image"), tr("The file could not be written."));
            return;
        }
        f.close();
        emit saved(path);
    };
    connect(save, &QPushButton::clicked, this, saveAs);

    // Right-click menu on the image, and Ctrl+C, to copy the full-size image.
    auto copyImage = [img] {
        if (!img.isNull()) QGuiApplication::clipboard()->setImage(img);
    };
    label->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(label, &QWidget::customContextMenuRequested, this,
            [this, label, copyImage, saveAs](const QPoint &pos) {
        QMenu menu(this);
        QAction *copy = menu.addAction(tr("Copy image"), copyImage);
        copy->setEnabled(m_valid);
        menu.addAction(tr("Save as …"), saveAs);
        menu.exec(label->mapToGlobal(pos));
    });
    new QShortcut(QKeySequence::Copy, this, copyImage);
    resize(qMin(img.width() + 60, maxSize.width()), qMin(img.height() + 110, maxSize.height()));
}
