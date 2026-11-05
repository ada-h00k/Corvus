#include "HistoryStore.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QMetaObject>

#include <cstring>

namespace {
// Passphrase bytes are wiped after use (best effort).
struct Secret {
    QByteArray bytes;
    explicit Secret(const QString &s) : bytes(s.toUtf8()) {}
    ~Secret() { bytes.fill('\0'); }
    const char *c() const { return bytes.constData(); }
};
} // namespace

bool HistoryStore::exists(const QString &path)
{
    return sc_store_exists(path.toUtf8().constData()) == 1;
}

QString HistoryStore::errorText(int code)
{
    switch (code) {
    case SC_OK: return QString();
    case SC_ERR_WRONG_PASSPHRASE: return QCoreApplication::translate("HistoryStore", "Wrong passphrase.");
    case SC_ERR_CORRUPT: return QCoreApplication::translate("HistoryStore", "The history file is damaged or has been modified.");
    case SC_ERR_EXISTS: return QCoreApplication::translate("HistoryStore", "A history file already exists.");
    case SC_ERR_IO: return QCoreApplication::translate("HistoryStore", "The history file could not be read or written.");
    default: return QCoreApplication::translate("HistoryStore", "Encryption error (code %1).").arg(code);
    }
}

int HistoryStore::create(const QString &path, const QString &passphrase)
{
    close();
    Secret pw(passphrase);
    int err = SC_OK;
    m_store = sc_store_create(path.toUtf8().constData(), pw.c(), &err);
    if (!m_store) return err;
    startWorker();
    return SC_OK;
}

int HistoryStore::open(const QString &path, const QString &passphrase, QJsonArray *records)
{
    close();
    Secret pw(passphrase);
    int err = SC_OK;
    char *json = nullptr;
    m_store = sc_store_open(path.toUtf8().constData(), pw.c(), &json, &err);
    if (!m_store) return err;
    // Parse straight from the C string: no extra copy of what can be a very large history.
    if (records && json) *records = QJsonDocument::fromJson(QByteArray::fromRawData(json, qsizetype(std::strlen(json)))).array();
    sc_string_free(json);
    startWorker();
    return SC_OK;
}

int HistoryStore::append(const QJsonObject &record)
{
    if (!m_store) return SC_ERR_ARG;
    Task t;
    t.json = QJsonDocument(record).toJson(QJsonDocument::Compact);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(t));
    }
    m_wake.notify_one();
    return SC_OK;
}

int HistoryStore::rewrite(const QJsonArray &records)
{
    if (!m_store) return SC_ERR_ARG;
    flush();
    const QByteArray j = QJsonDocument(records).toJson(QJsonDocument::Compact);
    std::lock_guard<std::mutex> io(m_io);
    return sc_store_rewrite(m_store, j.constData());
}

void HistoryStore::rewriteAsync(const QJsonArray &records, QObject *context, std::function<void(int)> done)
{
    if (!m_store) {
        notify(QPointer<QObject>(context), done, SC_ERR_ARG);
        return;
    }
    Task t;
    t.isRewrite = true;
    t.records = records; // implicitly shared: serialised and encrypted on the worker thread
    t.context = context;
    t.done = std::move(done);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(t));
    }
    m_wake.notify_one();
}

int HistoryStore::changePassphrase(const QString &oldPass, const QString &newPass)
{
    if (!m_store) return SC_ERR_ARG;
    flush();
    Secret o(oldPass), n(newPass);
    std::lock_guard<std::mutex> io(m_io);
    return sc_store_change_passphrase(m_store, o.c(), n.c());
}

void HistoryStore::close()
{
    if (m_worker.joinable()) stopWorker(); // drains the queue first
    sc_store_close(m_store);
    m_store = nullptr;
}

void HistoryStore::flush()
{
    if (!m_worker.joinable()) return;
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle.wait(lock, [this] { return m_queue.empty() && !m_busy; });
}

void HistoryStore::setAppendErrorHandler(QObject *context, std::function<void(int)> handler)
{
    // Only touched from the GUI thread while no worker is writing; the worker reads it per failure.
    std::lock_guard<std::mutex> lock(m_mutex);
    m_errorContext = context;
    m_errorHandler = std::move(handler);
}

void HistoryStore::startWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = false;
        m_busy = false;
        m_queue.clear();
    }
    m_worker = std::thread([this] { workerLoop(); });
}

void HistoryStore::stopWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    m_worker.join();
}

void HistoryStore::notify(const QPointer<QObject> &context, const std::function<void(int)> &fn, int rc)
{
    if (!fn || !context) return;
    QMetaObject::invokeMethod(context.data(), [fn, rc] { fn(rc); }, Qt::QueuedConnection);
}

void HistoryStore::workerLoop()
{
    for (;;) {
        std::deque<Task> batch;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) break; // m_stop and nothing left to write
            batch.swap(m_queue);
            m_busy = true;
        }

        // Consecutive appends go out as one batch; a rewrite is a barrier between batches.
        size_t i = 0;
        while (i < batch.size()) {
            if (batch[i].isRewrite) {
                writeRewrite(batch[i]);
                ++i;
                continue;
            }
            size_t j = i;
            while (j < batch.size() && !batch[j].isRewrite) ++j;
            writeAppends(batch, i, j);
            i = j;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_busy = false;
        }
        m_idle.notify_all();
    }
    m_idle.notify_all();
}

void HistoryStore::writeAppends(const std::deque<Task> &tasks, size_t begin, size_t end)
{
    QByteArray array;
    array.append('[');
    for (size_t k = begin; k < end; ++k) {
        if (k > begin) array.append(',');
        array.append(tasks[k].json);
    }
    array.append(']');

    int rc;
    {
        std::lock_guard<std::mutex> io(m_io);
        rc = sc_store_append_batch(m_store, array.constData());
        if (rc == SC_ERR_ARG && end - begin > 1) {
            // A malformed record must not take its neighbours down: fall back to one by one.
            rc = SC_OK;
            for (size_t k = begin; k < end; ++k) {
                const int one = sc_store_append(m_store, tasks[k].json.constData());
                if (one != SC_OK) rc = one;
            }
        }
    }
    if (rc == SC_OK) return;
    QPointer<QObject> ctx;
    std::function<void(int)> handler;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ctx = m_errorContext;
        handler = m_errorHandler;
    }
    notify(ctx, handler, rc);
}

void HistoryStore::writeRewrite(const Task &task)
{
    const QByteArray j = QJsonDocument(task.records).toJson(QJsonDocument::Compact);
    int rc;
    {
        std::lock_guard<std::mutex> io(m_io);
        rc = sc_store_rewrite(m_store, j.constData());
    }
    notify(task.context, task.done, rc);
}
