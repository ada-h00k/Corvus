#pragma once

#include <QColor>
#include <QPalette>

// Colours and shape of the chat bubbles (Settings → Appearance …).
struct Appearance {
    QColor background; // chat area
    QColor outBg;      // your messages
    QColor outText;
    QColor inBg;       // received messages
    QColor inText;
    int radius = 16;   // bubble corner radius in px

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
