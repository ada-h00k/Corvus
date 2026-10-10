#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace AttachmentUtil {
// Scaled preview as JPEG (or PNG when the image has transparency).
QByteArray makeThumbnail(const QImage &img, int maxDim = 480);
QString mimeTypeForFile(const QString &path);
QString humanSize(qint64 bytes);
// Decode image bytes honouring EXIF rotation.
QImage imageFromData(const QByteArray &data);
// Image from a file, honouring EXIF rotation.
QImage loadImage(const QString &path);
} // namespace AttachmentUtil
