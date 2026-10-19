#pragma once

#include <QString>
#include <QStringList>

// One-time migration of data written under the former names of this program:
//   "wren"          (the name immediately before Corvus)
//   "signal-cli-qt" (the name before that)
namespace LegacyMigration
{
// Moves the encrypted history (history.wren / history.scq -> history.corvus) and the QSettings
// values from the old locations to the new ones. The newest former name wins. Nothing happens if
// there is no old data or if the new location already holds a history. Must run after the
// application/organisation name has been set and before anything touches the data directory.
// Returns false and sets `error` if the old data cannot be migrated safely, e.g. because a
// previous version is still running and holds the history lock.
bool migrateDataAndSettings(QString &error);

// Former keychain service names, newest first. HistoryKeychain / GiphyApiKeyStore move the first
// entry they find over to the new service name the first time it is read.
QStringList legacyHistoryServices(); // "wren-history", "signal-cli-qt-history"
QStringList legacyGiphyServices();   // "wren", "signal-cli-qt"
}
