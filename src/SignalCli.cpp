#include "SignalCli.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <memory>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

SignalCli::SignalCli(QObject *parent) : QObject(parent) {}

SignalCli::~SignalCli()
{
    // Pending callbacks belong to objects (e.g. the main window) that may already be
    // partly destroyed at this point: drop them silently instead of failing them.
    m_pending.clear();
    stop();
}

QString SignalCli::autodetect()
{
    QString p = QStandardPaths::findExecutable(QStringLiteral("signal-cli"));
    if (!p.isEmpty())
        return p;
    const QStringList extra = {
#ifdef Q_OS_WIN
        // Installed by the Windows setup next to the application (signal-cli/bin/signal-cli.bat).
        QCoreApplication::applicationDirPath() + QStringLiteral("/signal-cli/bin"),
        qEnvironmentVariable("LOCALAPPDATA") + QStringLiteral("/Programs/Wren/signal-cli/bin"),
        qEnvironmentVariable("ProgramFiles") + QStringLiteral("/Wren/signal-cli/bin"),
        qEnvironmentVariable("ProgramFiles") + QStringLiteral("/signal-cli/bin"),
#endif
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/opt/signal-cli/bin"),
        QDir::homePath() + QStringLiteral("/.local/bin"),
        QDir::homePath() + QStringLiteral("/bin"),
    };
    return QStandardPaths::findExecutable(QStringLiteral("signal-cli"), extra);
}

void SignalCli::killTree(QProcess *p)
{
    if (!p || p->state() == QProcess::NotRunning) return;
#ifdef Q_OS_WIN
    const qint64 pid = p->processId();
    if (pid > 0)
        QProcess::execute(QStringLiteral("taskkill"),
                          {QStringLiteral("/PID"), QString::number(pid), QStringLiteral("/T"), QStringLiteral("/F")});
#endif
    p->kill();
}

QString SignalCli::Result::describe() const
{
    if (ok) return {};
    if (!startError.isEmpty()) return QObject::tr("signal-cli could not be started: %1").arg(startError);
    if (timedOut) return QObject::tr("signal-cli did not answer in time.");
    QString text = QObject::tr("signal-cli exited with code %1.").arg(exitCode);
    const QString e = err.trimmed();
    if (!e.isEmpty()) text += QStringLiteral("\n\n") + (e.size() > 700 ? e.left(700) + QStringLiteral("…") : e);
    return text;
}

QString SignalCli::maskNumbers(const QString &text)
{
    if (qEnvironmentVariableIsSet("WREN_DEBUG")) return text;
    static const QRegularExpression re(QStringLiteral(R"(\+?(\d{2})\d{5,}(\d{2}))"));
    QString t = text;
    t.replace(re, QStringLiteral("+\\1…\\2"));
    return t;
}

QProcessEnvironment SignalCli::childEnvironment(const QProcessEnvironment &base)
{
    QProcessEnvironment env = base;
#ifdef Q_OS_WIN
    // The Windows setup can install a private Java runtime next to signal-cli
    // (signal-cli.bat honours JAVA_HOME). It is only used if present.
    {
        const QString jre = QCoreApplication::applicationDirPath() + QStringLiteral("/signal-cli/jre");
        if (QFileInfo::exists(jre + QStringLiteral("/bin/java.exe")))
            env.insert(QStringLiteral("JAVA_HOME"), QDir::toNativeSeparators(jre));
    }
#endif
    const QString appdir = env.value(QStringLiteral("APPDIR"));
    if (appdir.isEmpty()) return env;

    // Running from an AppImage: drop everything that points into the bundle.
    const QStringList vars = {QStringLiteral("LD_LIBRARY_PATH"), QStringLiteral("PATH"),
                              QStringLiteral("QT_PLUGIN_PATH"), QStringLiteral("QML2_IMPORT_PATH"),
                              QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"), QStringLiteral("XDG_DATA_DIRS"),
                              QStringLiteral("GSETTINGS_SCHEMA_DIR")};
    for (const QString &name : vars) {
        if (!env.contains(name)) continue;
        QStringList keep;
        for (const QString &entry : env.value(name).split(QDir::listSeparator(), Qt::SkipEmptyParts))
            if (!entry.startsWith(appdir)) keep << entry;
        if (keep.isEmpty())
            env.remove(name);
        else
            env.insert(name, keep.join(QDir::listSeparator()));
    }
    return env;
}

void SignalCli::startProcess(QProcess *p, const QString &exe, const QStringList &args)
{
#ifdef Q_OS_MACOS
    const QString helper = QCoreApplication::applicationDirPath() +
                           QStringLiteral("/../Helpers/wren-signal-cli.app/Contents/MacOS/wren-signal-cli");
    // A missing signal-cli is started directly, so QProcess reports FailedToStart right away.
    const QString resolved = QFileInfo(exe).isAbsolute() ? exe : QStandardPaths::findExecutable(exe);
    if (QFileInfo::exists(helper) && QFileInfo(resolved).isExecutable()) {
        p->start(helper, QStringList{resolved} + args);
        return;
    }
#endif
    p->start(exe, args);
}

QStringList SignalCli::stageFiles(const QStringList &paths, const QString &dir)
{
#ifdef Q_OS_MACOS
    QStringList staged;
    for (const QString &path : paths) {
        // One folder per file: signal-cli sends the file name along.
        const QString target = dir + QLatin1Char('/') + QString::number(staged.size()) + QLatin1Char('/') +
                               QFileInfo(path).fileName();
        staged << (QDir().mkpath(QFileInfo(target).path()) && QFile::copy(path, target) ? target : path);
    }
    return staged;
#else
    Q_UNUSED(dir);
    return paths;
#endif
}

static void logResult(const QString &exe, const QStringList &args, const SignalCli::Result &r)
{
    auto show = [](QString t) {
        t = SignalCli::maskNumbers(t.trimmed());
        t.replace(QLatin1Char('\n'), QStringLiteral(" ⏎ "));
        return t.size() > 300 ? t.left(300) + QStringLiteral("…") : t;
    };
    QString status = r.ok ? QStringLiteral("ok")
                   : !r.startError.isEmpty() ? QStringLiteral("START FAILED: ") + r.startError
                   : r.timedOut ? QStringLiteral("TIMEOUT")
                   : QStringLiteral("exit code %1").arg(r.exitCode);
    qInfo().noquote() << "signal-cli:" << QFileInfo(exe).fileName() << args.join(QLatin1Char(' ')) << "->" << status
                      << QStringLiteral("(%1 ms)").arg(r.ms)
                      << (r.out.trimmed().isEmpty() ? QString() : QStringLiteral("| stdout: ") + show(r.out))
                      << (r.err.trimmed().isEmpty() ? QString() : QStringLiteral("| stderr: ") + show(r.err));
}

void SignalCli::runAsync(const QStringList &args, int timeoutMs, ResultCallback cb)
{
    auto *p = new QProcess(this);
    p->setProcessEnvironment(childEnvironment());
    auto *timer = new QTimer(p);
    timer->setSingleShot(true);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto done = std::make_shared<bool>(false);
    const QString exe = m_exe;

    auto finish = [=](bool timedOut, const QString &startError) {
        if (*done) return;
        *done = true;
        timer->stop();
        Result r;
        r.command = exe + QLatin1Char(' ') + args.join(QLatin1Char(' '));
        r.ms = clock->elapsed();
        r.timedOut = timedOut;
        r.startError = startError;
        r.out = QString::fromUtf8(p->readAllStandardOutput());
        r.err = QString::fromUtf8(p->readAllStandardError());
        if (!timedOut && startError.isEmpty()) {
            r.exitCode = p->exitCode();
            r.ok = p->exitStatus() == QProcess::NormalExit && r.exitCode == 0;
        }
        logResult(exe, args, r);
        p->deleteLater();
        if (cb) cb(r);
    };

    // Signals (not waitForFinished) so a fast process can never be mistaken for a hang.
    connect(p, &QProcess::finished, this, [finish](int, QProcess::ExitStatus) { finish(false, QString()); });
    connect(p, &QProcess::errorOccurred, this, [finish, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finish(false, p->errorString());
    });
    connect(timer, &QTimer::timeout, this, [finish, p] {
        killTree(p);
        p->waitForFinished(1000);
        finish(true, QString());
    });
    startProcess(p, m_exe, args);
    p->closeWriteChannel(); // signal-cli never needs stdin for these commands
    timer->start(timeoutMs);
}

void SignalCli::queryVersion(std::function<void(const Result &, const QString &)> cb)
{
    runAsync({QStringLiteral("--version")}, 60000, [cb](const Result &r) {
        cb(r, r.out.section(QLatin1Char('\n'), 0, 0).trimmed());
    });
}

QStringList SignalCli::parseAccounts(const QString &out)
{
    QStringList result;
    auto add = [&result](const QString &n) {
        if (!n.isEmpty() && !result.contains(n)) result << n;
    };
    const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
    if (doc.isArray()) {
        for (const QJsonValue &v : doc.array())
            add(v.isString() ? v.toString() : v.toObject().value(QStringLiteral("number")).toString());
        return result;
    }
    // Fallback: plain text ("Number: +49…") or JSON preceded by warnings
    static const QRegularExpression re(QStringLiteral(R"((\+\d{5,}))"));
    auto it = re.globalMatch(out);
    while (it.hasNext()) add(it.next().captured(1));
    return result;
}

void SignalCli::queryAccounts(std::function<void(const Result &, const QStringList &)> cb)
{
    runAsync({QStringLiteral("-o"), QStringLiteral("json"), QStringLiteral("listAccounts")}, 60000,
             [cb](const Result &r) { cb(r, parseAccounts(r.out)); });
}

void SignalCli::start(const QString &account)
{
    stop();
    QProcess *p = new QProcess(this);
    p->setProcessEnvironment(childEnvironment());
    m_proc = p;
    m_buf.clear();
    m_lastErrLine.clear();

    connect(p, &QProcess::readyReadStandardOutput, this, &SignalCli::onStdout);
    connect(p, &QProcess::readyReadStandardError, this, [this, p] {
        const QString s = QString::fromUtf8(p->readAllStandardError());
        for (const QString &l : s.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            m_lastErrLine = l.trimmed();
            emit logLine(l);
        }
    });
    connect(p, &QProcess::started, this, &SignalCli::started);
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && p == m_proc) {
            m_proc = nullptr;
            p->deleteLater();
            failPending(p->errorString());
            emit stopped(p->errorString());
        }
    });
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        if (p != m_proc) return;
        m_proc = nullptr;
        p->deleteLater();
        QString reason = QStringLiteral("signal-cli exited (exit code %1)").arg(code);
        if (!m_lastErrLine.isEmpty()) reason += QStringLiteral(": ") + m_lastErrLine;
        failPending(reason);
        emit stopped(reason);
    });

    startProcess(p, m_exe, {QStringLiteral("-a"), account, QStringLiteral("jsonRpc")});
}

void SignalCli::stop()
{
    if (!m_proc) return;
    QProcess *p = m_proc;
    m_proc = nullptr;
    p->disconnect(this);
    p->closeWriteChannel();
    p->terminate();
    if (!p->waitForFinished(3000)) {
        killTree(p);
        p->waitForFinished(1000);
    }
    p->deleteLater();
    failPending(QStringLiteral("Disconnected"));
}

void SignalCli::call(const QString &method, const QJsonObject &params, Callback cb)
{
    if (!m_proc) {
        if (cb) cb({}, QStringLiteral("signal-cli is not running"));
        return;
    }
    const int id = m_nextId++;
    QJsonObject req{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), method},
        {QStringLiteral("id"), id},
    };
    if (!params.isEmpty()) req[QStringLiteral("params")] = params;
    if (cb) m_pending.insert(id, std::move(cb));
    m_proc->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
}

void SignalCli::onStdout()
{
    if (!m_proc) return;
    m_buf += m_proc->readAllStandardOutput();
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        const QByteArray line = m_buf.left(nl).trimmed();
        m_buf.remove(0, nl + 1);
        if (!line.isEmpty()) handleLine(line);
    }
}

void SignalCli::handleLine(const QByteArray &line)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        emit logLine(QString::fromUtf8(line));
        return;
    }
    const QJsonObject o = doc.object();
    const QJsonValue idv = o.value(QStringLiteral("id"));

    // Response to a call
    if (idv.isDouble() && !o.contains(QStringLiteral("method"))) {
        const int id = idv.toInt();
        Callback cb = m_pending.take(id);
        if (!cb) return;
        if (o.contains(QStringLiteral("error"))) {
            cb({}, o.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(
                       QStringLiteral("Unknown error")));
        } else {
            cb(o.value(QStringLiteral("result")), {});
        }
        return;
    }

    // Notification (incoming message)
    if (o.value(QStringLiteral("method")).toString() == QLatin1String("receive")) {
        const QJsonObject params = o.value(QStringLiteral("params")).toObject();
        QJsonObject env = params.value(QStringLiteral("envelope")).toObject();
        if (env.isEmpty())
            env = params.value(QStringLiteral("result")).toObject().value(QStringLiteral("envelope")).toObject();
        if (!env.isEmpty()) emit envelopeReceived(env);
    }
}

void SignalCli::failPending(const QString &reason)
{
    auto pending = std::move(m_pending);
    m_pending.clear();
    for (auto it = pending.begin(); it != pending.end(); ++it)
        if (it.value()) it.value()({}, reason);
}
