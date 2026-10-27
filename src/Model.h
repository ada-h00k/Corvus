#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

struct Attachment {
    QString id;          // signal-cli attachment id (for getAttachment)
    QString contentType;
    QString filename;
    QString path;        // local source file (outgoing attachments only)
    qint64 size = 0;
    int width = 0;
    int height = 0;
    QByteArray thumb;    // JPEG/PNG preview (<= 480 px), kept in the encrypted history

    bool isImage() const { return contentType.startsWith(QLatin1String("image/")); }
    bool isAudio() const
    {
        if (contentType.startsWith(QLatin1String("audio/"))) return true;
        const QString suffix = filename.section(QLatin1Char('.'), -1).toLower();
        return QStringList{QStringLiteral("aac"), QStringLiteral("flac"), QStringLiteral("m4a"),
                           QStringLiteral("mp3"), QStringLiteral("oga"), QStringLiteral("ogg"),
                           QStringLiteral("opus"), QStringLiteral("wav"), QStringLiteral("weba"),
                           QStringLiteral("webm")}.contains(suffix);
    }
    bool isVideo() const
    {
        if (contentType.startsWith(QLatin1String("video/"), Qt::CaseInsensitive)) return true;
        const QString suffix = filename.section(QLatin1Char('.'), -1).toLower();
        return QStringList{QStringLiteral("3gp"), QStringLiteral("avi"), QStringLiteral("m4v"),
                           QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("mp4"),
                           QStringLiteral("mpeg"), QStringLiteral("mpg"), QStringLiteral("ogv"),
                           QStringLiteral("webm")}.contains(suffix);
    }
    bool isGif() const
    {
        return contentType.compare(QLatin1String("image/gif"), Qt::CaseInsensitive) == 0 ||
               filename.endsWith(QLatin1String(".gif"), Qt::CaseInsensitive);
    }
};

struct Quote {
    bool valid = false;
    qint64 id = 0;        // timestamp of the quoted message
    QString authorKey;    // number or UUID of the quoted author
    QString text;
    QString attachmentNote; // e.g. "image/png"
};

struct TextStyleRange {
    int start = 0;       // UTF-16 offset, as used by signal-cli
    int length = 0;      // UTF-16 length
    QString style;       // BOLD, ITALIC, STRIKETHROUGH, MONOSPACE or SPOILER
};

struct Message {
    QDateTime time;
    // Local timeline order is independent from Signal's sender timestamp (`time`). A sender's
    // device clock can be wrong; new messages should still appear where they were received.
    // Zero means a legacy history record without an explicit order value.
    qint64 orderTs = 0;
    QString sender;       // display name at the time of receiving
    QString text;
    QList<TextStyleRange> textStyles;
    bool outgoing = false;
    QString authorKey;    // number/UUID of the author (own number for outgoing)
    Quote quote;
    QList<Attachment> attachments;
    QHash<QString, QString> reactions; // reactor key -> emoji

    // Editing: `time` stays the timestamp of the original message (its identity in the
    // history and displayed time). An edit arrives as a new message timestamp, which is
    // remembered in editTs; peers refer to the message by that latest timestamp.
    qint64 editTs = 0;   // timestamp of the latest revision (0 = never edited)
    int editCount = 0;
    int expiresInSeconds = 0;
    qint64 expirationStartMs = 0;

    // "Delete for everyone" (by us or by the author): like the official clients the message stays
    // in the chat as a placeholder. Text, attachments, quote and reactions are gone for good.
    bool deleted = false;

    qint64 ts() const { return time.toMSecsSinceEpoch(); }
    qint64 sortTs() const { return orderTs > 0 ? orderTs : ts(); }
    qint64 latestTs() const { return editTs > 0 ? editTs : ts(); }
    bool isEdited() const { return editCount > 0; }
    // Same limits as the official clients: own text messages, 3 hours after sending, 10 edits.
    static constexpr qint64 kEditWindowMs = 3LL * 60 * 60 * 1000;
    static constexpr int kMaxEdits = 10;
    bool canEdit(qint64 nowMs) const
    {
        return outgoing && !deleted && !text.isEmpty() && editCount < kMaxEdits && nowMs - ts() <= kEditWindowMs;
    }
};

struct Conversation {
    QString key; // number/UUID or "g:<groupId>"
    QString title;
    bool isGroup = false;
    QList<Message> msgs;
    int unread = 0;
    QString storedName;       // display name learnt from an incoming message (persisted)
    QString localNickname;    // local-only contact nickname (persisted)
    bool archived = false;    // shown in the "Archived chats" folder
    bool sigArchived = false; // archive state last reported by signal-cli
    bool muted = false;       // local "mute notifications" (signal-cli does not report mute state)
    int expirationSeconds = 0; // Signal disappearing-message timer; 0 = disabled
};
