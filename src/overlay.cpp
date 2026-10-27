#include "overlay.h"

#include <QDBusConnection>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

static constexpr qint64 refreshInterval = 5000;

MarkOverlay::MarkOverlay()
{
    m_clock.start();
    m_helper = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                     QStringLiteral("kde-mark/mark.py"));
    m_timeout.setSingleShot(true);
    m_next.setSingleShot(true);
    connect(&m_next, &QTimer::timeout, this, &MarkOverlay::startBatch);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_failed = true;
        m_process.kill();
    });
    connect(&m_process, &QProcess::started, this, [this] {
        QJsonArray paths;
        for (const auto &url : m_batch) {
            paths.append(url.toLocalFile());
        }
        m_process.write(QJsonDocument(paths).toJson(QJsonDocument::Compact));
        m_process.closeWriteChannel();
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_output += m_process.readAllStandardOutput();
        if (m_output.size() > 1024 * 1024) {
            m_failed = true;
            m_process.kill();
        }
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        finishBatch(!m_failed && code == 0 && status == QProcess::NormalExit);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finishBatch(false);
        }
    });
    QDBusConnection::sessionBus().connect(QString(), QStringLiteral("/Mark"),
                                          QStringLiteral("org.local.DolphinMark"), QStringLiteral("Changed"),
                                          this, SLOT(invalidate(QStringList)));
}

MarkOverlay::~MarkOverlay()
{
    disconnect(&m_process, nullptr, this, nullptr);
    m_process.kill();
    m_process.waitForFinished(1000);
}

QStringList MarkOverlay::getOverlays(const QUrl &url)
{
    if (!url.isLocalFile() || !url.host().isEmpty()) {
        return {};
    }
    if (const auto *entry = m_cache.object(url)) {
        const auto icons = entry->icons;
        // Repaints can be queued after overlaysChanged has returned.
        if (m_clock.elapsed() - entry->checkedAt >= refreshInterval) {
            enqueue(url);
        }
        return icons;
    }
    enqueue(url);
    return {};
}

void MarkOverlay::enqueue(const QUrl &url)
{
    if (const auto *retryAfter = m_retryAfter.object(url)) {
        if (m_clock.elapsed() < *retryAfter) {
            return;
        }
        m_retryAfter.remove(url);
    }
    if (m_pending.contains(url) || m_pending.size() >= 8192) {
        return;
    }
    m_pending.insert(url);
    m_queue.enqueue(url);
    if (m_batch.isEmpty() && !m_next.isActive()) {
        m_next.start(0);
    }
}

void MarkOverlay::startBatch()
{
    if (!m_batch.isEmpty() || m_queue.isEmpty()) {
        return;
    }
    while (!m_queue.isEmpty() && m_batch.size() < 128) {
        m_batch.append(m_queue.dequeue());
    }
    m_output.clear();
    m_failed = false;
    if (m_helper.isEmpty()) {
        m_helper = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                         QStringLiteral("kde-mark/mark.py"));
    }
    m_process.setStandardErrorFile(QProcess::nullDevice());
    m_process.start(QStringLiteral("python3"), {m_helper, QStringLiteral("--worker"), QStringLiteral("read")});
    m_timeout.start(5000);
}

void MarkOverlay::finishBatch(bool success)
{
    if (m_batch.isEmpty()) {
        return;
    }
    m_timeout.stop();
    m_output += m_process.readAllStandardOutput();
    const auto document = QJsonDocument::fromJson(m_output);
    success = success && m_output.size() <= 1024 * 1024 && document.isArray();
    QHash<QUrl, QStringList> results;
    const QStringList colors{QStringLiteral("red"), QStringLiteral("orange"), QStringLiteral("yellow"),
                             QStringLiteral("green"), QStringLiteral("blue"), QStringLiteral("purple"),
                             QStringLiteral("grey")};
    if (success) {
        for (const auto &value : document.array()) {
            const auto record = value.toObject();
            if (!record.value(QStringLiteral("path")).isString()
                || !record.value(QStringLiteral("colors")).isArray() || record.contains(QStringLiteral("error"))) {
                continue;
            }
            const QUrl url = QUrl::fromLocalFile(record.value(QStringLiteral("path")).toString());
            if (!m_batch.contains(url)) {
                continue;
            }
            unsigned mask = 0;
            bool valid = true;
            for (const auto &color : record.value(QStringLiteral("colors")).toArray()) {
                if (!color.isString() || !colors.contains(color.toString())) {
                    valid = false;
                    break;
                }
                mask |= 1U << colors.indexOf(color.toString());
            }
            if (valid) {
                results.insert(url, mask ? QStringList{QStringLiteral(":/mark/mark-%1.svg").arg(mask)} : QStringList{});
            }
        }
    }
    const auto batch = m_batch;
    m_batch.clear();
    bool retrying = false;
    for (const auto &url : batch) {
        m_pending.remove(url);
        if (m_invalidated.remove(url)) {
            m_retries.remove(url);
            enqueue(url);
        } else if (results.contains(url)) {
            const auto icons = results.value(url);
            const auto *previous = m_cache.object(url);
            const bool changed = previous ? previous->icons != icons : !icons.isEmpty();
            m_retries.remove(url);
            m_retryAfter.remove(url);
            m_cache.insert(url, new Entry{icons, m_clock.elapsed()});
            if (changed) {
                Q_EMIT overlaysChanged(url, icons);
            }
        } else {
            // Preserve the last known marks; failures are not empty results.
            const int retries = m_retries.value(url) + 1;
            if (retries < 3) {
                m_retries.insert(url, retries);
                enqueue(url);
                retrying = true;
            } else {
                m_retries.remove(url);
                m_retryAfter.insert(url, new qint64(m_clock.elapsed() + refreshInterval));
            }
        }
    }
    if (!m_queue.isEmpty()) {
        m_next.start(retrying ? 500 : 0);
    }
}

void MarkOverlay::invalidate(const QStringList &paths)
{
    for (const auto &path : paths.mid(0, 8192)) {
        if (!path.startsWith(QLatin1Char('/'))) {
            continue;
        }
        const QUrl url = QUrl::fromLocalFile(path);
        // Ignore notifications for files this view has never requested.
        if (!m_cache.contains(url) && !m_pending.contains(url) && !m_retryAfter.contains(url)) {
            continue;
        }
        m_retryAfter.remove(url);
        if (auto *entry = m_cache.object(url)) {
            entry->checkedAt = -refreshInterval;
        }
        if (m_batch.contains(url)) {
            m_invalidated.insert(url);
        } else {
            enqueue(url);
        }
    }
}
