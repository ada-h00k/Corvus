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
};

struct Quote {
    bool valid = false;
    qint64 id = 0;        // timestamp of the quoted message
    QString authorKey;    // number or UUID of the quoted author
    QString text;
    QString attachmentNote; // e.g. "image/png"
};

struct Message {
    QDateTime time;
    QString sender;       // display name at the time of receiving
    QString text;
    bool outgoing = false;
    QString authorKey;    // number/UUID of the author (own number for outgoing)
    Quote quote;
    QList<Attachment> attachments;
    QHash<QString, QString> reactions; // reactor key -> emoji

    qint64 ts() const { return time.toMSecsSinceEpoch(); }
};

struct Conversation {
    QString key; // number/UUID or "g:<groupId>"
    QString title;
    bool isGroup = false;
    QList<Message> msgs;
    int unread = 0;
    QString storedName;       // display name learnt from an incoming message (persisted)
    bool archived = false;    // shown in the "Archived chats" folder
    bool sigArchived = false; // archive state last reported by signal-cli
};
