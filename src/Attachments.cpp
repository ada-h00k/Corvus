#include "Attachments.h"

#include <QBuffer>
#include <QImageReader>
#include <QLocale>
#include <QMimeDatabase>

namespace AttachmentUtil {

QByteArray makeThumbnail(const QImage &src, int maxDim)
{
    if (src.isNull()) return {};
    QImage img = src;
    if (img.width() > maxDim || img.height() > maxDim)
        img = img.scaled(maxDim, maxDim, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    if (img.hasAlphaChannel())
        img.save(&buf, "PNG");
    else
        img.convertToFormat(QImage::Format_RGB32).save(&buf, "JPEG", 82);
    return out;
}

QString mimeTypeForFile(const QString &path)
{
    return QMimeDatabase().mimeTypeForFile(path).name();
}

QString humanSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1);
}

QImage imageFromData(const QByteArray &data)
{
    QBuffer buf;
    buf.setData(data);
    buf.open(QIODevice::ReadOnly);
    QImageReader r(&buf);
    r.setAutoTransform(true);
    return r.read();
}

QImage loadImage(const QString &path)
{
    QImageReader r(path);
    r.setAutoTransform(true);
    return r.read();
}

} // namespace AttachmentUtil
