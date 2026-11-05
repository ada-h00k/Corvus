#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "history_store.h"

// Thin C++ wrapper around the Rust encrypted history store (history-store/).
//
// Writes are done by a background thread so the GUI never waits for fsync / encryption:
//  - append() only queues the record; the worker writes everything that is queued in one go
//    (one write + one fsync). A failure is reported through the error handler.
//  - rewriteAsync() queues a full rewrite (compaction after deletes); done() runs on the
//    context object's thread when it has finished.
//  - Everything else (rewrite, changePassphrase, close, ...) first waits until the queue is empty,
//    so the file is always written in the order the calls were made.
// The queue is drained on close()/destruction, so no accepted record is lost on a normal exit.
class HistoryStore
{
public:
    HistoryStore() = default;
    ~HistoryStore() { close(); }
    HistoryStore(const HistoryStore &) = delete;
    HistoryStore &operator=(const HistoryStore &) = delete;

    static bool exists(const QString &path);
    static QString errorText(int code);

    // All return SC_OK (0) or an SC_ERR_* code.
    int create(const QString &path, const QString &passphrase);
    int open(const QString &path, const QString &passphrase, QJsonArray *records);
    // Queues the record. Returns SC_OK when accepted (SC_ERR_ARG if no store is open);
    // I/O errors of the background write arrive through setAppendErrorHandler().
    int append(const QJsonObject &record);
    // Synchronous: waits for queued appends, then rewrites the file.
    int rewrite(const QJsonArray &records);
    // Queued rewrite; `done` is called with SC_OK or an error code, queued on `context`'s thread.
    void rewriteAsync(const QJsonArray &records, QObject *context, std::function<void(int)> done);
    int changePassphrase(const QString &oldPass, const QString &newPass);
    void close();
    bool isOpen() const { return m_store != nullptr; }

    // Blocks until everything queued so far has been written.
    void flush();
    // Called (queued on context's thread) when a background append failed.
    void setAppendErrorHandler(QObject *context, std::function<void(int)> handler);

private:
    struct Task {
        bool isRewrite = false;
        QByteArray json;       // compact JSON of the record (append)
        QJsonArray records;    // full snapshot (rewrite)
        QPointer<QObject> context;
        std::function<void(int)> done;
    };

    void startWorker();
    void stopWorker();
    void workerLoop();
    void writeAppends(const std::deque<Task> &tasks, size_t begin, size_t end);
    void writeRewrite(const Task &task);
    static void notify(const QPointer<QObject> &context, const std::function<void(int)> &fn, int rc);

    ScStore *m_store = nullptr;

    std::thread m_worker;
    std::mutex m_mutex;                 // guards m_queue, m_busy, m_stop
    std::condition_variable m_wake;     // worker waits for work here
    std::condition_variable m_idle;     // flush() waits for an empty, idle worker here
    std::deque<Task> m_queue;
    bool m_busy = false;
    bool m_stop = false;
    std::mutex m_io;                    // serialises every call into the Rust handle

    QPointer<QObject> m_errorContext;
    std::function<void(int)> m_errorHandler;
};
