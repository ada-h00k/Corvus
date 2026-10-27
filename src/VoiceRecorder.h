#pragma once

#include <QDateTime>
#include <QMediaFormat>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

// Recording of voice messages. VoiceRecorder is the small interface the window talks to; the
// implementation on top of Qt Multimedia is created by VoiceRecording::create().
class VoiceRecorder : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    // Empty if a recording can be started, otherwise why not (no microphone, no encoder, …).
    virtual QString unavailableReason() const = 0;
    // Starts recording into `directory`/`baseName`.<extension of the chosen format>.
    virtual void start(const QString &directory, const QString &baseName) = 0;
    // Ends the recording; finished() follows once the file is complete.
    virtual void stop() = 0;
    // Ends the recording and deletes the file; finished() does not follow.
    virtual void cancel() = 0;
    virtual bool isRecording() const = 0;

signals:
    void started();
    void durationChanged(qint64 milliseconds);
    void finished(const QString &path, qint64 durationMs);
    void failed(const QString &error);
};

namespace VoiceRecording {

// Longest recording; it stops by itself then (the result is kept).
constexpr qint64 kMaxDurationMs = 15 * 60 * 1000;
// Shorter recordings are taken for an accidental click and dropped.
constexpr qint64 kMinDurationMs = 500;

struct Format {
    QMediaFormat::FileFormat file;
    QMediaFormat::AudioCodec codec;
    QString extension;
};

// The first of AAC in MP4 (what the Signal apps record and play everywhere), Opus or Vorbis in
// Ogg, and WAV that `supported` accepts. nullopt if none.
std::optional<Format> chooseFormat(const std::function<bool(QMediaFormat::FileFormat, QMediaFormat::AudioCodec)> &supported);
// Same, asking Qt Multimedia which formats this system can encode.
std::optional<Format> chooseSystemFormat();

// "voice-message-2026-10-27-14-15-23" (without extension)
QString baseName(const QDateTime &time);
// "0:07", "12:03", "1:02:03"
QString durationText(qint64 milliseconds);
// signal-cli can mark audio attachments as voice notes ("voiceNote") since 0.14.2.
bool supportsVoiceNote(const QString &signalCliVersion);

// The recorder on top of Qt Multimedia, or the one a test installed.
VoiceRecorder *create(QObject *parent);
using Factory = std::function<VoiceRecorder *(QObject *parent)>;
void setFactoryForTesting(Factory factory);

} // namespace VoiceRecording
