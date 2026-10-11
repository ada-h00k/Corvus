#pragma once

#include <QIcon>
#include <QPixmap>

// The application / tray icon is drawn in code (no image resources, no SVG plugin needed).
namespace AppIcon {
// Rounded blue square with a white chat bubble; `unread` > 0 adds a red badge (with the count on larger sizes).
QPixmap render(int size, int unread = 0);
QIcon app();
QIcon tray(int unread);
} // namespace AppIcon
