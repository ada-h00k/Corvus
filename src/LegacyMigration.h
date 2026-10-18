#pragma once

#include <QString>

// One-time migration of data written under the former name "signal-cli-qt"
// (before the project was renamed to Wren).
namespace LegacyMigration
{
// Moves the encrypted history (history.scq -> history.wren) and the QSettings values from the
// old locations to the new ones. Nothing happens if there is no old data or if the new location
// already holds a history. Must run after the application/organisation name has been set and
// before anything touches the data directory.
// Returns false and sets `error` if the old data cannot be migrated safely, e.g. because a
// previous version is still running and holds the history lock.
bool migrateDataAndSettings(QString &error);

// Former keychain service names, used by HistoryKeychain / GiphyApiKeyStore to move entries
// over to the new service names the first time they are read.
extern const char *const kLegacyHistoryService; // "signal-cli-qt-history"
extern const char *const kLegacyGiphyService;   // "signal-cli-qt"
}
