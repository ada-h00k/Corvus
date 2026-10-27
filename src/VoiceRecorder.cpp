#include "VoiceRecorder.h"

#include <QAudioDevice>
#include <QAudioInput>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaRecorder>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

namespace VoiceRecording {

namespace {
Factory &testFactory()
{
    static Factory factory;
    return factory;
}
} // namespace

std::optional<Format> chooseFormat(const std::function<bool(QMediaFormat::FileFormat, QMediaFormat::AudioCodec)> &supported)
{
    const QList<Format> preference = {
        {QMediaFormat::MPEG4, QMediaFormat::AudioCodec::AAC, QStringLiteral("m4a")},
        {QMediaFormat::Ogg, QMediaFormat::AudioCodec::Opus, QStringLiteral("ogg")},
        {QMediaFormat::Ogg, QMediaFormat::AudioCodec::Vorbis, QStringLiteral("ogg")},
        {QMediaFormat::Wave, QMediaFormat::AudioCodec::Wave, QStringLiteral("wav")},
    };
    for (const Format &f : preference)
        if (supported(f.file, f.codec)) return f;
    return std::nullopt;
}

std::optional<Format> chooseSystemFormat()
{
    return chooseFormat([](QMediaFormat::FileFormat file, QMediaFormat::AudioCodec codec) {
        QMediaFormat format;
        format.setFileFormat(file);
        format.setAudioCodec(codec);
        return format.isSupported(QMediaFormat::Encode);
    });
}

QString baseName(const QDateTime &time)
{
    return QStringLiteral("voice-message-") + time.toString(QStringLiteral("yyyy-MM-dd-HH-mm-ss"));
}

QString durationText(qint64 ms)
{
    const qint64 total = qMax<qint64>(0, ms) / 1000;
    const qint64 h = total / 3600, m = (total / 60) % 60, s = total % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

bool supportsVoiceNote(const QString &version)
{
    const QVersionNumber v = QVersionNumber::fromString(version.section(QLatin1Char(' '), -1));
    return !v.isNull() && v >= QVersionNumber(0, 14, 2);
}

namespace {

class QtVoiceRecorder : public VoiceRecorder
{
public:
    explicit QtVoiceRecorder(QObject *parent) : VoiceRecorder(parent), m_input(this), m_recorder(this)
    {
        m_session.setAudioInput(&m_input);
        m_session.setRecorder(&m_recorder);
        connect(&m_recorder, &QMediaRecorder::durationChanged, this, [this](qint64 ms) {
            if (!m_active) return;
            emit durationChanged(ms);
            if (ms >= kMaxDurationMs && !m_stopping) stop(); // the result is kept
        });
        connect(&m_recorder, &QMediaRecorder::recorderStateChanged, this, [this](QMediaRecorder::RecorderState state) {
            if (state == QMediaRecorder::RecordingState && m_active) emit started();
            if (state == QMediaRecorder::StoppedState) {
                if (m_discard) {
                    m_discard = false;
                    QFile::remove(m_path);
                } else if (m_active) {
                    complete();
                }
            }
        });
        connect(&m_recorder, &QMediaRecorder::errorOccurred, this,
                [this](QMediaRecorder::Error, const QString &error) { fail(error); });
    }

    ~QtVoiceRecorder() override
    {
        if (m_active) {
            m_active = false;
            m_discard = true;
            m_recorder.stop();
            QFile::remove(m_path);
        }
    }

    QString unavailableReason() const override
    {
        if (QMediaDevices::defaultAudioInput().isNull()) return tr("No microphone was found.");
        if (!chooseSystemFormat()) return tr("This system cannot record audio with Qt Multimedia.");
        return {};
    }

    void start(const QString &directory, const QString &baseName) override
    {
        if (m_active) return;
        const auto format = chooseSystemFormat();
        if (!format) {
            emit failed(tr("This system cannot record audio with Qt Multimedia."));
            return;
        }
        QDir().mkpath(directory);
        m_path = QDir(directory).filePath(baseName + QLatin1Char('.') + format->extension);
        QMediaFormat mediaFormat;
        mediaFormat.setFileFormat(format->file);
        mediaFormat.setAudioCodec(format->codec);
        m_recorder.setMediaFormat(mediaFormat);
        m_recorder.setAudioChannelCount(1); // speech
        const bool wave = format->file == QMediaFormat::Wave;
        m_recorder.setAudioSampleRate(wave ? 16000 : 44100);
        if (!wave) m_recorder.setAudioBitRate(64000);
        m_recorder.setOutputLocation(QUrl::fromLocalFile(m_path));
        m_active = true;
        m_stopping = false;
        m_discard = false;
        m_recorder.record();
    }

    void stop() override
    {
        if (!m_active || m_stopping) return;
        m_stopping = true;
        m_recorder.stop();
    }

    void cancel() override
    {
        if (!m_active) return;
        m_active = false; // not recording any more, the file is removed once the recorder has stopped
        m_stopping = false;
        m_discard = true;
        m_recorder.stop();
    }

    bool isRecording() const override { return m_active; }

private:
    void complete()
    {
        const QString path = m_recorder.actualLocation().isLocalFile() ? m_recorder.actualLocation().toLocalFile() : m_path;
        const qint64 ms = m_recorder.duration();
        m_active = false;
        m_stopping = false;
        emit finished(path, ms);
    }

    void fail(const QString &error)
    {
        const bool was = m_active;
        m_active = false;
        m_stopping = false;
        m_discard = false;
        QFile::remove(m_path);
        if (was) emit failed(error);
    }

    QMediaCaptureSession m_session;
    QAudioInput m_input;
    QMediaRecorder m_recorder;
    QString m_path;
    bool m_active = false;
    bool m_stopping = false;
    bool m_discard = false;
};

} // namespace

VoiceRecorder *create(QObject *parent)
{
    if (testFactory()) return testFactory()(parent);
    return new QtVoiceRecorder(parent);
}

void setFactoryForTesting(Factory factory) { testFactory() = std::move(factory); }

} // namespace VoiceRecording
