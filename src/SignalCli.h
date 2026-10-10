#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QProcess>
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

    // One-shot calls (blocking, short)
    QString runOnce(const QStringList &args, int timeoutMs = 20000, QString *error = nullptr) const;
    QString version() const;      // e.g. "signal-cli 0.13.12"
    QStringList accounts() const; // registered numbers

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
    int m_nextId = 1;
    QHash<int, Callback> m_pending;
};
