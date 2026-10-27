#pragma once

#include <KOverlayIconPlugin>
#include <QCache>
#include <QProcess>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUrl>

class MarkOverlay : public KOverlayIconPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.local.DolphinMark.Overlay")

public:
    MarkOverlay();
    ~MarkOverlay() override;
    QStringList getOverlays(const QUrl &url) override;

private Q_SLOTS:
    void invalidate(const QStringList &paths);

private:
    struct Entry {
        QStringList icons;
    };
    void enqueue(const QUrl &url);
    void startBatch();
    void finishBatch(bool success);

    QCache<QUrl, Entry> m_cache{4096};
    QQueue<QUrl> m_queue;
    QSet<QUrl> m_pending;
    QList<QUrl> m_batch;
    QSet<QUrl> m_invalidated;
    QHash<QUrl, int> m_retries;
    QProcess m_process;
    QTimer m_timeout;
    QTimer m_next;
    QByteArray m_output;
    QString m_helper;
    bool m_failed = false;
    bool m_notifying = false;
};
