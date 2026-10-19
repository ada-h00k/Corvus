#pragma once

#include <QIcon>
#include <QPixmap>

// The application icon is a raven on a rounded tile: white on black (packaging/corvus.svg, dark
// theme; also the desktop / installer icon) or black on white (packaging/corvus-light.svg, light
// theme). The tray shows the raven alone
// on a transparent background (packaging/tray-light.svg / tray-dark.svg). All are embedded via
// resources/icons.qrc.
namespace AppIcon {
// Settings → Appearance: app and tray icon follow the theme, or always use one variant.
enum class Mode { FollowTheme, Light, Dark };
void setMode(Mode mode);
// Qt's colour scheme, and when that is unknown, the palette.
bool themeIsDark();
// The app icon variant: the chosen one, or with FollowTheme, themeIsDark().
bool prefersDark();
// The app icon (tile); `unread` > 0 adds a red badge (with the count on larger sizes).
QPixmap render(int size, int unread, bool dark);
QPixmap render(int size, int unread = 0);
// The tray icon (transparent raven), with the same badge.
QPixmap renderTray(int size, int unread, bool dark);
QIcon app();
// Follows the App icon setting like app(): the theme by default, or the chosen raven colour.
QIcon tray(int unread);
} // namespace AppIcon
