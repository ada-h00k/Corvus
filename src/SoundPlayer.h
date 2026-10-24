#pragma once

#include <QObject>
#include <QProcess>
#include <QStringList>

#include "NotificationSettings.h"

// Plays the "new message" sound.
//
// Uses the desktop's own command line players (pw-play, paplay, canberra-gtk-play,
// aplay, sox/play; afplay on macOS) so that it plays whatever the system player
// understands (wav, ogg, flac, …) without bundling codecs.
//
// Windows has no such player: PowerShell's Media.SoundPlayer only understands WAV.
// So the built-in "Pop" is shipped as WAV there, and other formats (a custom .ogg,
// .flac, .mp3) are played in-process with Qt Multimedia.
class QAudioOutput;
class QMediaPlayer;

class SoundPlayer : public QObject
{
    Q_OBJECT
public:
    explicit SoundPlayer(QObject *parent = nullptr);
    ~SoundPlayer() override;

    static QStringList builtinIds();              // "chime", "pop", "ding"
    static QString builtinTitle(const QString &id);
    static QByteArray synthesize(const QString &id); // complete WAV file (16-bit mono, 44.1 kHz)

    // Resource holding the built-in "Pop" sound: the OGG everywhere, a WAV copy where
    // only WAV can be played (Windows).
    static QString popResource(bool wavOnly);
    // True if the command line player of Windows (PowerShell) can play `file`.
    static bool powerShellCanPlay(const QString &file);

    // File to play for the given settings (built-in sounds are written to the cache directory).
    QString resolveFile(const NotificationSettings &s) const;

    // Starts playback without blocking. Returns false if no player could be started.
    bool play(const QString &file);
    // Plays `file` inside the application with Qt Multimedia (used on Windows for
    // everything except WAV). Failures are reported through failed().
    bool playInProcess(const QString &file);
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

    void stopProcess();

    QProcess *m_proc = nullptr;
    QMediaPlayer *m_media = nullptr;
    QAudioOutput *m_audioOut = nullptr;
};
