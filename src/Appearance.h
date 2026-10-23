#pragma once

#include <QColor>
#include <QPalette>

// Colours and shape of the chat bubbles, the avatar switch and the app icon (Settings → Appearance …).
struct Appearance {
    QColor background; // chat area
    QColor outBg;      // your messages
    QColor outText;
    QColor inBg;       // received messages
    QColor inText;
    int radius = 16;   // bubble corner radius in px
    bool showAvatars = true; // contact avatars next to the names in the chat list
    bool sortAlphabetically = false; // contact list ordered by name instead of by latest message
    bool alwaysShowSend = false; // Send button next to the message field even when it is not expanded
    enum IconChoice { IconFollowTheme = 0, IconLight = 1, IconDark = 2 };
    int appIcon = IconFollowTheme; // window / tray icon variant

    // Defaults adapt to a light or dark system theme.
    static Appearance defaults(const QPalette &pal);
    // Defaults overridden by whatever the user saved.
    static Appearance load(const QPalette &pal);
    // Only values that differ from the defaults are stored, so untouched
    // colours keep following the system theme.
    void save(const QPalette &pal) const;
    static void reset();

    bool operator==(const Appearance &o) const;
    bool operator!=(const Appearance &o) const { return !(*this == o); }
};
