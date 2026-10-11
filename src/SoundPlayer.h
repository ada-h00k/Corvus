#pragma once

#include <QObject>
#include <QProcess>
#include <QStringList>

#include "NotificationSettings.h"

// Plays the "new message" sound.
//
// Deliberately uses the desktop's own command line players (pw-play, paplay,
// canberra-gtk-play, aplay, sox/play; afplay on macOS; PowerShell on Windows)
// instead of Qt Multimedia: no extra dependency, no codec libraries to bundle,
// and it plays whatever the system player understands (wav, ogg, flac, …).
class SoundPlayer : public QObject
{
    Q_OBJECT
public:
    explicit SoundPlayer(QObject *parent = nullptr);
    ~SoundPlayer() override;

    static QStringList builtinIds();              // "chime", "pop", "ding"
    static QString builtinTitle(const QString &id);
    static QByteArray synthesize(const QString &id); // complete WAV file (16-bit mono, 44.1 kHz)

    // File to play for the given settings (built-in sounds are written to the cache directory).
    QString resolveFile(const NotificationSettings &s) const;

    // Starts playback without blocking. Returns false if no player could be started.
    bool play(const QString &file);
    bool play(const NotificationSettings &s) { return play(resolveFile(s)); }

    // Name of the player that would be used ("" if none was found).
    QString playerName() const;

signals:
    void failed(const QString &message);

private:
    struct Command {
        QString program;
        QStringList args;
    };
    bool findCommand(const QString &file, Command *out) const;

    QProcess *m_proc = nullptr;
};
