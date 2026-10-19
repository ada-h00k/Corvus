#include "Attachments.h"

#include <QBuffer>
#include <QFileInfo>
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

QString genericFileName(const QString &contentType, const QString &originalName, const QDateTime &time)
{
    QString suffix = QFileInfo(originalName).suffix().toLower();
    if (suffix.isEmpty()) suffix = QMimeDatabase().mimeTypeForName(contentType).preferredSuffix();
    QString name = QStringLiteral("signal-") +
                   time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd-HH-mm-ss-zzz"));
    if (!suffix.isEmpty()) name += QLatin1Char('.') + suffix;
    return name;
}

QString displayFileName(const Attachment &att, const QDateTime &time)
{
    const QString original = QFileInfo(att.filename).fileName();
    if (!original.isEmpty()) return original; // the name the sender gave wins
    if (!time.isValid()) return original;
    return genericFileName(att.contentType, original, time);
}

} // namespace AttachmentUtil
