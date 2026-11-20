// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

// Holt ein app-spezifisches Secret vom XDG Secret Portal
// (org.freedesktop.portal.Secret). Gedacht für die Flatpak-Sandbox, in der
// der Zugriff auf das Host-Keyring (org.freedesktop.secrets) nicht erlaubt ist.
namespace PortalSecret {

// true, wenn die Anwendung in einer Flatpak-Sandbox läuft.
bool runningInFlatpak();

// Fragt das Portal nach dem app-spezifischen Secret. Blockiert mit lokaler
// Event-Loop, bis das Portal antwortet oder timeoutMs abgelaufen ist.
std::optional<QByteArray> retrieve(QString *error = nullptr, int timeoutMs = 60000);

// 32-Byte-Schlüssel (HMAC-SHA256 über `purpose`), abgeleitet vom Portal-Secret.
std::optional<QByteArray> deriveKey(const QByteArray &purpose, QString *error = nullptr);

} // namespace PortalSecret
