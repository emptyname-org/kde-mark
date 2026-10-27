#include <KOverlayIconPlugin>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPluginLoader>
#include <QProcess>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <sys/xattr.h>
#include <signal.h>
#include <cerrno>

static QString pluginPath;

struct CountingHelper {
    QTemporaryDir directory;
    QPluginLoader loader{pluginPath};

    ~CountingHelper() { loader.unload(); }

    QString script() const { return directory.filePath(QStringLiteral("kde-mark/mark.py")); }

    bool setReply(const QByteArray &reply)
    {
        QFile file(script() + QStringLiteral(".reply"));
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(reply) == reply.size();
    }

    int reads() const
    {
        QFile file(script() + QStringLiteral(".calls"));
        return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
    }

    KOverlayIconPlugin *load(const QByteArray &reply)
    {
        if (!directory.isValid() || !QDir().mkpath(directory.filePath(QStringLiteral("kde-mark")))) {
            return nullptr;
        }
        QFile helper(script());
        if (!helper.open(QIODevice::WriteOnly)) {
            return nullptr;
        }
        helper.write("import json, pathlib, sys\n"
                     "paths = json.load(sys.stdin)\n"
                     "with open(__file__ + '.calls', 'a') as calls: calls.write('read\\n')\n"
                     "reply = json.loads(pathlib.Path(__file__ + '.reply').read_text())\n"
                     "print(json.dumps([dict(path=p, **reply) for p in paths]))\n");
        helper.close();
        if (!setReply(reply)) {
            return nullptr;
        }
        const QByteArray oldDataHome = qgetenv("XDG_DATA_HOME");
        const auto restore = qScopeGuard([&] { qputenv("XDG_DATA_HOME", oldDataHome); });
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        return qobject_cast<KOverlayIconPlugin *>(loader.instance());
    }
};

class OverlayTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void loadsAndRefreshesRealMetadata()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sample.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QCOMPARE(setxattr(QFile::encodeName(path).constData(), "user.dolphin-color-tags.red", "1", 1, 0), 0);
        QPluginLoader loader(pluginPath);
        const auto cleanup = qScopeGuard([&loader] { loader.unload(); });
        auto *plugin = qobject_cast<KOverlayIconPlugin *>(loader.instance());
        QVERIFY2(plugin, qPrintable(loader.errorString()));
        QSignalSpy spy(plugin, &KOverlayIconPlugin::overlaysChanged);
        const auto url = QUrl::fromLocalFile(path);
        QVERIFY(plugin->getOverlays(url).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
        QCOMPARE(plugin->getOverlays(url), QStringList{QStringLiteral(":/mark/mark-1.svg")});
        QCOMPARE(setxattr(QFile::encodeName(path).constData(), "user.dolphin-color-tags.blue", "1", 1, 0), 0);
        QVERIFY(QMetaObject::invokeMethod(plugin, "invalidate", Q_ARG(QStringList, QStringList{path})));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 2, 3000);
        const auto icons = plugin->getOverlays(url);
        QCOMPARE(icons, QStringList{QStringLiteral(":/mark/mark-17.svg")});
        QVERIFY(QFile::exists(icons.front()));

        // Exercise the actual service-menu helper and its D-Bus signature.
        QProcess helper;
        helper.start(QStringLiteral("python3"), {qEnvironmentVariable("XDG_DATA_HOME") + QStringLiteral("/kde-mark/mark.py"),
                                                  QStringLiteral("--clear"), QStringLiteral("--"), path});
        QTRY_VERIFY_WITH_TIMEOUT(helper.state() == QProcess::NotRunning, 5000);
        QVERIFY2(helper.exitCode() == 0, helper.readAllStandardError().constData());
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 3, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(plugin->getOverlays(url).isEmpty(), 3000);
        QVERIFY(loader.unload());
    }

    void ignoresRemoteUrls()
    {
        QPluginLoader loader(pluginPath);
        const auto cleanup = qScopeGuard([&loader] { loader.unload(); });
        auto *plugin = qobject_cast<KOverlayIconPlugin *>(loader.instance());
        QVERIFY(plugin);
        QSignalSpy spy(plugin, &KOverlayIconPlugin::overlaysChanged);
        QVERIFY(plugin->getOverlays(QUrl(QStringLiteral("smb://host/file"))).isEmpty());
        QVERIFY(plugin->getOverlays(QUrl(QStringLiteral("file://host/file"))).isEmpty());
        QTest::qWait(100);
        QCOMPARE(spy.count(), 0);
        QVERIFY(loader.unload());
    }

    void queuedRepaintsSettle()
    {
        QTemporaryDir directory;
        const auto path = directory.path();
        QCOMPARE(setxattr(QFile::encodeName(path).constData(), "user.dolphin-color-tags.red", "1", 1, 0), 0);
        QPluginLoader loader(pluginPath);
        const auto cleanup = qScopeGuard([&loader] { loader.unload(); });
        auto *plugin = qobject_cast<KOverlayIconPlugin *>(loader.instance());
        QVERIFY(plugin);
        QSignalSpy spy(plugin, &KOverlayIconPlugin::overlaysChanged);
        const auto url = QUrl::fromLocalFile(path);
        // Dolphin schedules its icon refresh after overlaysChanged returns.
        connect(plugin, &KOverlayIconPlugin::overlaysChanged, plugin,
                [plugin](const QUrl &changed) { plugin->getOverlays(changed); }, Qt::QueuedConnection);
        plugin->getOverlays(url);
        QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 3000);
        QTest::qWait(750);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(plugin->getOverlays(url), QStringList{QStringLiteral(":/mark/mark-1.svg")});
        QVERIFY(loader.unload());
    }

    void unmarkedRepaintsReuseCachedResult()
    {
        CountingHelper helper;
        auto *plugin = helper.load("{\"colors\": []}");
        QVERIFY(plugin);
        const auto url = QUrl::fromLocalFile(helper.directory.path());
        QSignalSpy spy(plugin, &KOverlayIconPlugin::overlaysChanged);
        QTimer repaint;
        connect(&repaint, &QTimer::timeout, plugin, [plugin, url] { plugin->getOverlays(url); });
        repaint.start(5);
        QTRY_COMPARE_WITH_TIMEOUT(helper.reads(), 1, 3000);
        QTest::qWait(500);
        QCOMPARE(helper.reads(), 1);
        QCOMPARE(spy.count(), 0);
        QVERIFY(plugin->getOverlays(url).isEmpty());
    }

    void expiredCacheRefreshesWithoutFeedback_data()
    {
        QTest::addColumn<bool>("changed");
        QTest::newRow("unchanged") << false;
        QTest::newRow("changed") << true;
    }

    void expiredCacheRefreshesWithoutFeedback()
    {
        QFETCH(bool, changed);
        CountingHelper helper;
        auto *plugin = helper.load("{\"colors\": [\"red\"]}");
        QVERIFY(plugin);
        const auto path = helper.directory.path();
        const auto url = QUrl::fromLocalFile(path);
        QSignalSpy spy(plugin, &KOverlayIconPlugin::overlaysChanged);
        connect(plugin, &KOverlayIconPlugin::overlaysChanged, plugin,
                [plugin](const QUrl &item) { plugin->getOverlays(item); }, Qt::QueuedConnection);
        plugin->getOverlays(url);
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
        if (changed) {
            QVERIFY(helper.setReply("{\"colors\": [\"blue\"]}"));
        }
        QTest::qWait(5100);
        // No polling while idle, even after the cached result expires.
        QCOMPARE(helper.reads(), 1);
        plugin->getOverlays(url);
        QTRY_COMPARE_WITH_TIMEOUT(helper.reads(), 2, 3000);
        QTest::qWait(200);
        QCOMPARE(spy.count(), changed ? 2 : 1);
        QCOMPARE(plugin->getOverlays(url), QStringList{changed ? QStringLiteral(":/mark/mark-16.svg")
                                                             : QStringLiteral(":/mark/mark-1.svg")});

        // Duplicate notifications must not cause a repaint when marks are unchanged.
        QVERIFY(QMetaObject::invokeMethod(plugin, "invalidate", Q_ARG(QStringList, QStringList{path})));
        QVERIFY(QMetaObject::invokeMethod(plugin, "invalidate", Q_ARG(QStringList, QStringList{path})));
        QTRY_COMPARE_WITH_TIMEOUT(helper.reads(), 3, 3000);
        QTest::qWait(200);
        QCOMPARE(spy.count(), changed ? 2 : 1);

        QVERIFY(helper.setReply("{\"colors\": []}"));
        QVERIFY(QMetaObject::invokeMethod(plugin, "invalidate", Q_ARG(QStringList, QStringList{path})));
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), changed ? 3 : 2, 3000);
        QVERIFY(plugin->getOverlays(url).isEmpty());
        QTest::qWait(200);
        QCOMPARE(helper.reads(), 4);
    }

    void failedReadsBackOffAndRecover_data()
    {
        QTest::addColumn<bool>("cached");
        QTest::newRow("unknown") << false;
        QTest::newRow("previous-marks") << true;
    }

    void failedReadsBackOffAndRecover()
    {
        QFETCH(bool, cached);
        CountingHelper helper;
        auto *plugin = helper.load(cached ? "{\"colors\": [\"red\"]}" : "{\"error\": \"temporary\"}");
        QVERIFY(plugin);
        const auto path = helper.directory.path();
        const auto url = QUrl::fromLocalFile(path);
        QSignalSpy spy(plugin, &KOverlayIconPlugin::overlaysChanged);
        if (cached) {
            plugin->getOverlays(url);
            QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
            QVERIFY(helper.setReply("{\"error\": \"temporary\"}"));
            QVERIFY(QMetaObject::invokeMethod(plugin, "invalidate", Q_ARG(QStringList, QStringList{path})));
        }
        QTimer repaint;
        connect(&repaint, &QTimer::timeout, plugin, [plugin, url] { plugin->getOverlays(url); });
        repaint.start(5);
        QTRY_COMPARE_WITH_TIMEOUT(helper.reads(), cached ? 4 : 3, 3000);
        QTest::qWait(750);
        QCOMPARE(helper.reads(), cached ? 4 : 3);
        QCOMPARE(spy.count(), cached ? 1 : 0);
        QCOMPARE(plugin->getOverlays(url), cached ? QStringList{QStringLiteral(":/mark/mark-1.svg")} : QStringList{});
        QVERIFY(helper.setReply("{\"colors\": [\"green\"]}"));
        if (cached) {
            // An explicit update bypasses the failure cooldown.
            QVERIFY(QMetaObject::invokeMethod(plugin, "invalidate", Q_ARG(QStringList, QStringList{path})));
        }
        // Otherwise a later repaint retries after the cooldown expires.
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), cached ? 2 : 1, 6500);
        QCOMPARE(plugin->getOverlays(url), QStringList{QStringLiteral(":/mark/mark-8.svg")});
        QTest::qWait(200);
        QCOMPARE(helper.reads(), cached ? 5 : 4);
    }

    void retriesMissingFilesInsteadOfCachingAbsence()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("appears-later"));
        const auto url = QUrl::fromLocalFile(path);
        QPluginLoader loader(pluginPath);
        const auto cleanup = qScopeGuard([&loader] { loader.unload(); });
        auto *plugin = qobject_cast<KOverlayIconPlugin *>(loader.instance());
        QVERIFY(plugin);
        QSignalSpy spy(plugin, &KOverlayIconPlugin::overlaysChanged);
        plugin->getOverlays(url);
        QTest::qWait(180);
        QCOMPARE(spy.count(), 0);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QCOMPARE(setxattr(QFile::encodeName(path).constData(), "user.dolphin-color-tags.green", "1", 1, 0), 0);
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
        QCOMPARE(plugin->getOverlays(url), QStringList{QStringLiteral(":/mark/mark-8.svg")});
        QVERIFY(loader.unload());
    }

    void deduplicatesPendingReads()
    {
        QTemporaryDir directory;
        QCOMPARE(setxattr(QFile::encodeName(directory.path()).constData(), "user.dolphin-color-tags.red", "1", 1, 0), 0);
        const auto url = QUrl::fromLocalFile(directory.path());
        QPluginLoader loader(pluginPath);
        const auto cleanup = qScopeGuard([&loader] { loader.unload(); });
        auto *plugin = qobject_cast<KOverlayIconPlugin *>(loader.instance());
        QVERIFY(plugin);
        QSignalSpy spy(plugin, &KOverlayIconPlugin::overlaysChanged);
        for (int i = 0; i < 100; ++i) {
            plugin->getOverlays(url);
        }
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
        QTest::qWait(100);
        QCOMPARE(spy.count(), 1);
        QVERIFY(loader.unload());
    }

    void rejectsStaleRepliesAfterInvalidation()
    {
        QTemporaryDir directory;
        const QByteArray oldDataHome = qgetenv("XDG_DATA_HOME");
        QDir().mkpath(directory.filePath(QStringLiteral("kde-mark")));
        QFile helper(directory.filePath(QStringLiteral("kde-mark/mark.py")));
        QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.write("import json, sys, time\npaths=json.load(sys.stdin)\ntime.sleep(0.25)\nprint(json.dumps([{'path': p, 'colors': ['red']} for p in paths]))\n");
        helper.close();
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        QPluginLoader loader(pluginPath);
        const auto cleanup = qScopeGuard([&loader] { loader.unload(); });
        auto *plugin = qobject_cast<KOverlayIconPlugin *>(loader.instance());
        qputenv("XDG_DATA_HOME", oldDataHome);
        QVERIFY(plugin);
        QSignalSpy spy(plugin, &KOverlayIconPlugin::overlaysChanged);
        const auto url = QUrl::fromLocalFile(directory.path());
        plugin->getOverlays(url);
        QTest::qWait(100);
        QVERIFY(QMetaObject::invokeMethod(plugin, "invalidate", Q_ARG(QStringList, QStringList{directory.path()})));
        QVERIFY(helper.open(QIODevice::WriteOnly | QIODevice::Truncate));
        helper.write("import json, sys\nprint(json.dumps([{'path': p, 'colors': ['blue']} for p in json.load(sys.stdin)]))\n");
        helper.close();
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
        QCOMPARE(plugin->getOverlays(url), QStringList{QStringLiteral(":/mark/mark-16.svg")});
        QVERIFY(loader.unload());
    }

    void retriesAfterWorkerTimeout()
    {
        QTemporaryDir directory;
        const QByteArray oldDataHome = qgetenv("XDG_DATA_HOME");
        QDir().mkpath(directory.filePath(QStringLiteral("kde-mark")));
        QFile helper(directory.filePath(QStringLiteral("kde-mark/mark.py")));
        QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.write("import json, sys, time, pathlib\npaths=json.load(sys.stdin)\nmarker=pathlib.Path(__file__+'.attempt')\n"
                     "if not marker.exists():\n marker.touch()\n time.sleep(30)\n"
                     "print(json.dumps([{'path': p, 'colors': ['green']} for p in paths]))\n");
        helper.close();
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        QPluginLoader loader(pluginPath);
        const auto cleanup = qScopeGuard([&loader] { loader.unload(); });
        auto *plugin = qobject_cast<KOverlayIconPlugin *>(loader.instance());
        qputenv("XDG_DATA_HOME", oldDataHome);
        QVERIFY(plugin);
        QSignalSpy spy(plugin, &KOverlayIconPlugin::overlaysChanged);
        const auto url = QUrl::fromLocalFile(directory.path());
        plugin->getOverlays(url);
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 8000);
        QCOMPARE(plugin->getOverlays(url), QStringList{QStringLiteral(":/mark/mark-8.svg")});
        QVERIFY(loader.unload());
    }

    void unloadTerminatesPendingWorker()
    {
        QTemporaryDir directory;
        const QByteArray oldDataHome = qgetenv("XDG_DATA_HOME");
        QDir().mkpath(directory.filePath(QStringLiteral("kde-mark")));
        const QString script = directory.filePath(QStringLiteral("kde-mark/mark.py"));
        QFile helper(script);
        QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.write("import os, time, pathlib\npathlib.Path(__file__+'.pid').write_text(str(os.getpid()))\ntime.sleep(30)\n");
        helper.close();
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        QPluginLoader loader(pluginPath);
        const auto cleanup = qScopeGuard([&loader] { loader.unload(); });
        auto *plugin = qobject_cast<KOverlayIconPlugin *>(loader.instance());
        qputenv("XDG_DATA_HOME", oldDataHome);
        QVERIFY(plugin);
        plugin->getOverlays(QUrl::fromLocalFile(directory.path()));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(script + QStringLiteral(".pid")), 3000);
        QFile pidFile(script + QStringLiteral(".pid"));
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        const int pid = pidFile.readAll().toInt();
        QVERIFY(pid > 0);
        QElapsedTimer clock;
        clock.start();
        QVERIFY(loader.unload());
        QVERIFY(clock.elapsed() < 1500);
        QCOMPARE(kill(pid, 0), -1);
        QCOMPARE(errno, ESRCH);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        return 2;
    }
    pluginPath = QString::fromLocal8Bit(argv[1]);
    OverlayTest test;
    return QTest::qExec(&test, 1, argv);
}

#include "test_overlay.moc"
