#pragma once

#include <QIcon>
#include <QPixmap>

// The application / tray icon is rendered from packaging/corvus.svg (dark theme, embedded as
// :/icons/corvus.svg) or packaging/corvus-light.svg (light theme, :/icons/corvus-light.svg).
namespace AppIcon {
// Qt's colour scheme; when that is unknown, the palette decides.
bool prefersDark();
// The corvus on a navy (dark) or creme (light) rounded square; `unread` > 0 adds a red badge
// (with the count on larger sizes).
QPixmap render(int size, int unread, bool dark);
QPixmap render(int size, int unread = 0);
QIcon app();
QIcon tray(int unread);
} // namespace AppIcon
