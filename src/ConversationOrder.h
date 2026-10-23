#pragma once

#include <QCollator>
#include <QDateTime>
#include <QString>

// Order of the contact list and stepping through it with Ctrl+Up / Ctrl+Down.
namespace ConversationOrder {

// Names compared the way people expect: ignoring case, and with the language's
// rules (accents, ä next to a) when the system has them.
inline int compareNames(const QString &a, const QString &b)
{
    static const QCollator collator = [] {
        QCollator c;
        c.setCaseSensitivity(Qt::CaseInsensitive);
        return c;
    }();
    return collator.compare(a, b);
}

// Strict weak ordering for the list. By default the conversation with the most
// recent message comes first; with `alphabetical` the order only depends on the
// names, so it does not change while messages arrive.
inline bool before(const QString &titleA, const QDateTime &lastA, const QString &keyA, const QString &titleB,
                   const QDateTime &lastB, const QString &keyB, bool alphabetical)
{
    if (!alphabetical && lastA != lastB) return lastA > lastB;
    const int byTitle = compareNames(titleA, titleB);
    if (byTitle != 0) return byTitle < 0;
    return keyA < keyB; // identical names: keep the order stable
}

// Row to switch to from `current` (-1: none) when moving `delta` (-1 up, +1 down)
// through `count` conversations. Does not wrap around; returns -1 if there is
// nowhere to go. Without a current row the first (down) or last (up) one is used.
inline int step(int current, int count, int delta)
{
    if (count <= 0 || delta == 0) return -1;
    if (current < 0 || current >= count) return delta > 0 ? 0 : count - 1;
    const int target = current + (delta > 0 ? 1 : -1);
    return target < 0 || target >= count ? -1 : target;
}

} // namespace ConversationOrder
