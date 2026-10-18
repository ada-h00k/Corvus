#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <functional>

// Thin layer around the externally installed signal-cli.
// Only the CLI program is invoked (one-shot commands and
// "signal-cli -a <account> jsonRpc" as a long-running process). There is no
// dependency on signal-cli internals, so signal-cli can be updated
// independently of this GUI.
class SignalCli : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const QJsonValue &result, const QString &error)>;

    explicit SignalCli(QObject *parent = nullptr);
    ~SignalCli() override;

    static QString autodetect();

    QString executable() const { return m_exe; }
    void setExecutable(const QString &path) { m_exe = path; }

    // Result of a one-shot signal-cli call.
    struct Result {
        bool ok = false;        // started, exited normally with code 0
        bool timedOut = false;
        int exitCode = -1;
        QString out, err;
        QString startError;     // set when the process could not be started at all
        QString command;        // for diagnostics
        qint64 ms = 0;

        // Human-readable description of what went wrong ("" if ok).
        QString describe() const;
    };
    using ResultCallback = std::function<void(const Result &)>;

    // One-shot calls. They never block the GUI: the callback runs in the event loop.
    void runAsync(const QStringList &args, int timeoutMs, ResultCallback cb);
    void queryVersion(std::function<void(const Result &, const QString &version)> cb); // "signal-cli 0.13.12"
    void queryAccounts(std::function<void(const Result &, const QStringList &accounts)> cb);

    static QStringList parseAccounts(const QString &output);
    // Environment for child processes: inside an AppImage the library/plugin
    // paths that point into the bundle are removed so Java etc. are not confused.
    static QProcessEnvironment childEnvironment(const QProcessEnvironment &base = QProcessEnvironment::systemEnvironment());
    // Forcefully ends a child process. On Windows signal-cli is started through signal-cli.bat
    // (cmd.exe -> java.exe); a plain kill() would leave the JVM running, so the whole tree is ended.
    static void killTree(QProcess *p);
    // Phone numbers are masked in diagnostics unless WREN_DEBUG is set.
    static QString maskNumbers(const QString &text);

    // Long-running process (JSON-RPC over stdin/stdout)
    void start(const QString &account);
    void stop();
    bool isRunning() const { return m_proc != nullptr; }
    void call(const QString &method, const QJsonObject &params, Callback cb);

signals:
    void started();
    void stopped(const QString &reason);
    void envelopeReceived(const QJsonObject &envelope);
    void logLine(const QString &line);

private:
    void onStdout();
    void handleLine(const QByteArray &line);
    void failPending(const QString &reason);

    QString m_exe;
    QProcess *m_proc = nullptr;
    QByteArray m_buf;
    QString m_lastErrLine;
    int m_nextId = 1;
    QHash<int, Callback> m_pending;
};
