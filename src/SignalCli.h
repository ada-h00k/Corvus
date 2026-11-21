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

// Thin layer around the externally installed corvus-cli.
// Only the CLI program is invoked (one-shot commands and
// "corvus-cli -a <account> jsonRpc" as a long-running process). There is no
// dependency on corvus-cli internals, so corvus-cli can be updated
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

    // The secret that unlocks the encrypted data (the keychain entry, or the user's passphrase).
    // It is passed to every corvus-cli process in its environment, so the backend's sessions,
    // contacts and attachments open with the same unlock the chat history uses. The previous
    // secret is passed along after a change until corvus-cli has re-wrapped its keyring.
    void setSecret(const QString &secret) { m_secret = secret; }
    void setPreviousSecret(const QString &secret) { m_prevSecret = secret; }

    // Result of a one-shot corvus-cli call.
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
    void queryVersion(std::function<void(const Result &, const QString &version)> cb); // "corvus-cli 1.6.0 (abc1234) compatible with signal-cli 0.14.9"
    void queryAccounts(std::function<void(const Result &, const QStringList &accounts)> cb);

    static QStringList parseAccounts(const QString &output);
    // Normalizes JSON-RPC receive notifications. Some integrations expose
    // typing as a separate receiveTyping notification instead of a regular receive
    // envelope; normalize both to the envelope format consumed by MainWindow.
    static QJsonObject envelopeFromNotification(const QJsonObject &notification);
    // Environment for child processes: inside an AppImage the library/plugin
    // paths that point into the bundle are removed so the backend is not confused.
    static QProcessEnvironment childEnvironment(const QProcessEnvironment &base = QProcessEnvironment::systemEnvironment());
    // The same, plus the unlock secret in the variables corvus-cli reads. Used for every
    // backend process, so the encrypted sessions, contacts and attachments open with the
    // secret the chat history was unlocked with.
    static QProcessEnvironment environmentWithSecret(const QString &secret, const QString &previous = QString(),
                                                     const QProcessEnvironment &base = QProcessEnvironment::systemEnvironment());
    QProcessEnvironment backendEnvironment() const;
    // Starts corvus-cli in `p`; in the macOS app bundle through the helper app (src/SignalCliHelper.cpp).
    static void startProcess(QProcess *p, const QString &exe, const QStringList &args);
    // On macOS corvus-cli runs as another app and may not read protected folders (Desktop, Documents, …):
    // copies the files into `dir` and returns the copies. Elsewhere the paths are returned unchanged.
    static QStringList stageFiles(const QStringList &paths, const QString &dir);
    // Forcefully ends a child process (on Windows including any processes it started).
    static void killTree(QProcess *p);
    // Phone numbers are masked in diagnostics.
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
    // corvus-cli finished downloading an attachment in its background worker (F4): the id
    // is the name the file is stored under (getAttachment / deleteAttachment).
    void attachmentDownloaded(const QString &id);
    void logLine(const QString &line);

private:
    void onStdout();
    void handleLine(const QByteArray &line);
    void failPending(const QString &reason);

    QString m_exe;
    QString m_secret;
    QString m_prevSecret;
    QProcess *m_proc = nullptr;
    QByteArray m_buf;
    QString m_lastErrLine;
    int m_nextId = 1;
    QHash<int, Callback> m_pending;
};
