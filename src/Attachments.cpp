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

// Bidi override/isolate controls (U+202A..U+202E, U+2066..U+2069, U+200E, U+200F,
// U+061C) make the surrounding text render in reversed or isolated order:
// "invoice\u202Egpj.exe" (U+202E before "gpj") displays as "invoiceexe.jpg".
// They are meaningless in a file name, so they are dropped before the name is
// shown or written anywhere.
static QString stripBidiControls(const QString &name)
{
    QString out;
    out.reserve(name.size());
    bool stripped = false;
    for (const QChar &c : name) {
        const char16_t u = c.unicode();
        const bool control = (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069) ||
                             u == 0x200E || u == 0x200F || u == 0x061C;
        if (control) stripped = true;
        else out.append(c);
    }
    return stripped ? out : name;
}

QString displayFileName(const Attachment &att, const QDateTime &time)
{
    const QString original = stripBidiControls(QFileInfo(att.filename).fileName());
    if (!original.isEmpty()) return original; // the name the sender gave wins
    if (!time.isValid()) return original;
    return genericFileName(att.contentType, original, time);
}

QString normalizedFileName(const QString &name)
{
    // Only the last path component is ever shown or written (callers use
    // QFileInfo::fileName()), so directory separators cannot escape here.
    QString out = QFileInfo(name).fileName();
    // An NTFS alternate data stream ("name.exe:stream") must not hide the real
    // suffix: everything from the first ":" onwards is not part of the file name.
    const int stream = out.indexOf(QLatin1Char(':'));
    if (stream >= 0) out.truncate(stream);
    // Windows drops trailing dots and spaces when saving, which would turn
    // "evil.exe." into "evil.exe" after the check saw an empty suffix.
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) out.chop(1);
    return stripBidiControls(out);
}

QString normalizedSuffix(const QString &name)
{
    return QFileInfo(normalizedFileName(name)).suffix().toLower();
}

bool isSafeToOpen(const Attachment &att)
{
    // Images, audio and video are decoded and played inside Corvus and never fall
    // through to the system "open" handler, so the sender's claim is acceptable here.
    if (att.isImage() || att.isAudio() || att.isVideo()) return true;
    // Anything handed to the OS default handler must be a harmless document type.
    static const QStringList openable = {QStringLiteral("pdf"), QStringLiteral("txt")};
    return openable.contains(normalizedSuffix(att.filename));
}

} // namespace AttachmentUtil
