#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QString>

#include "Model.h"

namespace AttachmentUtil {
// Scaled preview as JPEG (or PNG when the image has transparency).
QByteArray makeThumbnail(const QImage &img, int maxDim = 480);
QString mimeTypeForFile(const QString &path);
QString humanSize(qint64 bytes);
// Decode image bytes honouring EXIF rotation.
QImage imageFromData(const QByteArray &data);
// Image from a file, honouring EXIF rotation.
QImage loadImage(const QString &path);
// "signal-2026-10-19-14-15-23-939.png": the name the official Signal clients give an
// image (derived from the message time). `time` is the message's sent time.
QString genericFileName(const QString &contentType, const QString &originalName, const QDateTime &time);
// File name to show and to pre-fill in save dialogs: the name the sender gave it; only
// attachments that arrived without a name get the generic Signal name.
QString displayFileName(const Attachment &att, const QDateTime &time);
// The name as it would actually land on disk: remote names can carry NTFS stream
// separators (":") or trailing dots/spaces that Windows strips when saving, which
// hides a dangerous suffix from the safety checks.
QString normalizedFileName(const QString &name);
// Lower-case suffix of the normalized name ("" when there is none).
QString normalizedSuffix(const QString &name);
// Allowlist instead of a blocklist: only attachments that Corvus renders or plays
// itself (images in the image viewer, audio/video in the media players) or plain
// documents without executable semantics (PDF, text) may reach the system "open"
// handler. Everything else — whatever the sender claims the content type to be —
// is save-only.
bool isSafeToOpen(const Attachment &att);
} // namespace AttachmentUtil
