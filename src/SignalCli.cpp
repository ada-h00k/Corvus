#include "SignalCli.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

SignalCli::SignalCli(QObject *parent) : QObject(parent) {}

SignalCli::~SignalCli() { stop(); }

QString SignalCli::autodetect()
{
    QString p = QStandardPaths::findExecutable(QStringLiteral("signal-cli"));
    if (!p.isEmpty())
        return p;
    const QStringList extra = {
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/opt/signal-cli/bin"),
        QDir::homePath() + QStringLiteral("/.local/bin"),
        QDir::homePath() + QStringLiteral("/bin"),
    };
    return QStandardPaths::findExecutable(QStringLiteral("signal-cli"), extra);
}

QString SignalCli::runOnce(const QStringList &args, int timeoutMs, QString *error) const
{
    QProcess p;
    p.start(m_exe, args);
    if (!p.waitForStarted(5000)) {
        if (error) *error = p.errorString();
        return {};
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        if (error) *error = QStringLiteral("Timed out");
        return {};
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error) *error = QString::fromUtf8(p.readAllStandardError()).trimmed();
        return {};
    }
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

QString SignalCli::version() const
{
    return runOnce({QStringLiteral("--version")}).section(QLatin1Char('\n'), 0, 0).trimmed();
}

QStringList SignalCli::accounts() const
{
    QStringList result;
    const QString out = runOnce({QStringLiteral("-o"), QStringLiteral("json"), QStringLiteral("listAccounts")});
    const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
    if (doc.isArray()) {
        for (const QJsonValue &v : doc.array()) {
            const QString n = v.toObject().value(QStringLiteral("number")).toString();
            if (!n.isEmpty()) result << n;
        }
        return result;
    }
    // Fallback: plain-text output "Number: +49..."
    static const QRegularExpression re(QStringLiteral(R"((\+\d{5,}))"));
    auto it = re.globalMatch(out);
    while (it.hasNext()) result << it.next().captured(1);
    return result;
}

void SignalCli::start(const QString &account)
{
    stop();
    QProcess *p = new QProcess(this);
    m_proc = p;
    m_buf.clear();

    connect(p, &QProcess::readyReadStandardOutput, this, &SignalCli::onStdout);
    connect(p, &QProcess::readyReadStandardError, this, [this, p] {
        const QString s = QString::fromUtf8(p->readAllStandardError());
        for (const QString &l : s.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            emit logLine(l);
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
        const QString reason = QStringLiteral("signal-cli exited (exit code %1)").arg(code);
        failPending(reason);
        emit stopped(reason);
    });

    p->start(m_exe, {QStringLiteral("-a"), account, QStringLiteral("jsonRpc")});
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
        p->kill();
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
