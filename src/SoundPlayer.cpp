#include "SoundPlayer.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtMath>

#include "SignalCli.h"

namespace {
constexpr int kRate = 44100;

QByteArray wavFile(const QVector<float> &samples)
{
    QByteArray pcm;
    pcm.resize(samples.size() * 2);
    auto *out = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < samples.size(); ++i) out[i] = static_cast<qint16>(qBound(-1.0f, samples[i], 1.0f) * 32767.0f);

    auto le32 = [](quint32 v) { return QByteArray(reinterpret_cast<const char *>(&v), 4); };
    auto le16 = [](quint16 v) { return QByteArray(reinterpret_cast<const char *>(&v), 2); };
    QByteArray w("RIFF");
    w += le32(36 + pcm.size());
    w += "WAVEfmt ";
    w += le32(16) + le16(1) + le16(1) + le32(kRate) + le32(kRate * 2) + le16(2) + le16(16);
    w += "data";
    w += le32(pcm.size());
    w += pcm;
    return w;
}

// A decaying note with two overtones; short attack and release avoid clicks.
void addNote(QVector<float> &buf, double freq, double start, double dur, double amp, double decay)
{
    const int from = int(start * kRate);
    const int n = int(dur * kRate);
    if (from + n > buf.size()) buf.resize(from + n);
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / kRate;
        const double attack = qMin(1.0, t / 0.004);
        const double release = qMin(1.0, (dur - t) / 0.02);
        const double env = qExp(-t * decay) * attack * release;
        const double ph = 2 * M_PI * freq * t;
        buf[from + i] += float(amp * env * (qSin(ph) + 0.28 * qSin(2 * ph) + 0.08 * qSin(3 * ph)));
    }
}
} // namespace

QStringList SoundPlayer::builtinIds()
{
    return {QStringLiteral("chime"), QStringLiteral("pop"), QStringLiteral("ding")};
}

QString SoundPlayer::builtinTitle(const QString &id)
{
    if (id == QLatin1String("chime")) return tr("Chime");
    if (id == QLatin1String("pop")) return tr("Pop");
    if (id == QLatin1String("ding")) return tr("Ding");
    return id;
}

QByteArray SoundPlayer::synthesize(const QString &id)
{
    QVector<float> buf;
    if (id == QLatin1String("pop")) {
        const int n = int(0.14 * kRate);
        buf.resize(n);
        double phase = 0;
        for (int i = 0; i < n; ++i) {
            const double t = double(i) / kRate;
            phase += 2 * M_PI * (950.0 - 520.0 * qMin(1.0, t / 0.1)) / kRate; // quick downward sweep
            const double env = qExp(-t * 28) * qMin(1.0, t / 0.002) * qMin(1.0, (0.14 - t) / 0.01);
            buf[i] = float(0.6 * env * qSin(phase));
        }
    } else if (id == QLatin1String("ding")) {
        addNote(buf, 1046.5, 0.0, 0.9, 0.5, 4.5); // C6
    } else { // chime: two rising notes
        addNote(buf, 1318.5, 0.0, 0.45, 0.45, 7.0); // E6
        addNote(buf, 1760.0, 0.16, 0.65, 0.45, 5.5); // A6
    }
    float peak = 0.0f;
    for (float v : std::as_const(buf)) peak = qMax(peak, qAbs(v));
    if (peak > 0.0f)
        for (float &v : buf) v = v / peak * 0.8f; // normalise, leave headroom
    return wavFile(buf);
}

SoundPlayer::SoundPlayer(QObject *parent) : QObject(parent) {}

SoundPlayer::~SoundPlayer()
{
    if (m_proc) { // application is closing: cut a still-playing sound short
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(300);
    }
}

QString SoundPlayer::resolveFile(const NotificationSettings &s) const
{
    if (s.soundId == QLatin1String("custom") && !s.soundFile.isEmpty() && QFileInfo::exists(s.soundFile))
        return s.soundFile;

    const QString id = builtinIds().contains(s.soundId) ? s.soundId : QStringLiteral("chime");
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/sounds");
    QDir().mkpath(dir);
    const QString path = dir + QLatin1Char('/') + id + QStringLiteral(".wav");
    const QByteArray wav = synthesize(id);
    if (QFileInfo(path).size() != wav.size()) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(wav);
    }
    return path;
}

bool SoundPlayer::findCommand(const QString &file, Command *out) const
{
    auto have = [](const QString &name) { return !QStandardPaths::findExecutable(name).isEmpty(); };
#if defined(Q_OS_MACOS)
    if (have(QStringLiteral("afplay"))) {
        *out = {QStringLiteral("afplay"), {file}};
        return true;
    }
#elif defined(Q_OS_WIN)
    QString quoted = file;
    quoted.replace(QLatin1Char('\''), QStringLiteral("''"));
    *out = {QStringLiteral("powershell"),
            {QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
             QStringLiteral("(New-Object Media.SoundPlayer '%1').PlaySync()").arg(quoted)}};
    return true;
#else
    if (have(QStringLiteral("pw-play"))) { *out = {QStringLiteral("pw-play"), {file}}; return true; }
    if (have(QStringLiteral("paplay"))) { *out = {QStringLiteral("paplay"), {file}}; return true; }
    if (have(QStringLiteral("canberra-gtk-play"))) { *out = {QStringLiteral("canberra-gtk-play"), {QStringLiteral("-f"), file}}; return true; }
    if (have(QStringLiteral("aplay"))) { *out = {QStringLiteral("aplay"), {QStringLiteral("-q"), file}}; return true; }
    if (have(QStringLiteral("play"))) { *out = {QStringLiteral("play"), {QStringLiteral("-q"), file}}; return true; }
#endif
    Q_UNUSED(have)
    return false;
}

QString SoundPlayer::playerName() const
{
    Command c;
    return findCommand(QString(), &c) ? c.program : QString();
}

bool SoundPlayer::play(const QString &file)
{
    Command cmd;
    if (file.isEmpty() || !findCommand(file, &cmd)) {
        QApplication::beep(); // better than silence
        emit failed(tr("No audio player was found (tried pw-play, paplay, canberra-gtk-play, aplay, play)."));
        return false;
    }
    if (m_proc) { // a new sound replaces one that is still playing
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    auto *p = new QProcess(this);
    m_proc = p;
    p->setProcessEnvironment(SignalCli::childEnvironment());
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) {
        if (code != 0 || st != QProcess::NormalExit)
            emit failed(tr("The audio player failed: %1").arg(QString::fromUtf8(p->readAllStandardError()).trimmed()));
        if (m_proc == p) m_proc = nullptr;
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            emit failed(tr("The audio player could not be started: %1").arg(p->errorString()));
            if (m_proc == p) m_proc = nullptr;
            p->deleteLater();
        }
    });
    p->start(cmd.program, cmd.args);
    p->closeWriteChannel();
    return true;
}
