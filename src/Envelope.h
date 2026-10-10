#pragma once

#include <QJsonObject>

#include "Model.h"

// A signal-cli "receive" envelope reduced to what the GUI needs.
struct ParsedEnvelope {
    enum Kind { None, Message_, Reaction } kind = None;
    bool outgoing = false;   // sent by one of our own other devices (sync message)
    QString src;             // sender number / UUID (incoming)
    QString srcName;         // sender's profile name
    QString destination;     // recipient number / UUID (outgoing 1:1)
    QString groupId;         // set for group messages
    Message msg;             // Message_: time, text, attachments, quote (author keys unresolved)

    // Reaction
    QString emoji;
    bool remove = false;
    QString targetNumber, targetUuid;
    qint64 targetTs = 0;
};

ParsedEnvelope parseEnvelope(const QJsonObject &env);
