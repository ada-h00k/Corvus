#pragma once

#include <QIcon>
#include <QPixmap>

// The application / tray icon is rendered from packaging/corvus.svg (embedded as :/icons/corvus.svg).
namespace AppIcon {
// The corvus on a white rounded square; `unread` > 0 adds a red badge (with the count on larger sizes).
QPixmap render(int size, int unread = 0);
QIcon app();
QIcon tray(int unread);
} // namespace AppIcon
