#pragma once

#include <QJsonObject>

#include "Model.h"

// A signal-cli "receive" envelope reduced to what the GUI needs.
struct ParsedEnvelope {
    enum Kind { None, Message_, Reaction, Edit, Delete, Pin, Unpin, PollVote, PollTerminate, ExpirationUpdate, Receipt, Typing } kind = None;
    bool outgoing = false;   // sent by one of our own other devices (sync message)
    QString src;             // sender number / UUID (incoming)
    QString srcName;         // sender's profile name
    QString destination;     // recipient number / UUID (outgoing 1:1)
    QString groupId;         // set for group messages
    int expirationSeconds = 0;
    // corvus-cli marks an envelope whose sender's identity key changed without being
    // verified (L6): the chat shows the same warning bar a changed safety number does.
    bool untrustedIdentity = false;
    Message msg;             // Message_: time, text, attachments, quote (author keys unresolved)
    // Receipt notifications acknowledge timestamps of messages sent by this client.
    bool readReceipt = false;
    bool deliveryReceipt = false;
    qint64 receiptTimeMs = 0; // event timestamp, distinct from the acknowledged message timestamps
    QList<qint64> receiptTimestamps;
    // Typing notifications from a contact (or a member of a group).
    bool typingStarted = false;

    // Reaction
    QString emoji;
    bool remove = false;
    QString targetNumber, targetUuid;
    qint64 targetTs = 0;   // Reaction: reacted message; Edit: timestamp of the message being edited
    // Delete: targetTs = timestamp of the message deleted for everyone
    // Edit: new text in msg.text, timestamp of the edit in msg.time
    // Pin: targetTs = timestamp of the pinned message; Unpin: the unpinned message.
    // PollVote / PollTerminate: targetTs = timestamp of the poll message,
    // targetNumber/targetUuid = author of that poll message.
    // pinDurationSeconds as reported by the backend: -1 = forever, 0 = unspecified
    // (treated like forever here), otherwise seconds (24 h / 7 d / 30 d on the official clients).
    qint64 pinDurationSeconds = -1;
    // PollVote: the chosen option indexes (empty = vote taken back) and the per-voter
    // sequence number of the vote ("voteCount", counts up with every vote).
    QList<int> pollOptionIndexes;
    int pollVoteCount = 0;
};

ParsedEnvelope parseEnvelope(const QJsonObject &env);
