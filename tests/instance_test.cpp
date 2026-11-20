#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QProcess>
#include <QtTest>

class InstanceTest : public QObject {
    Q_OBJECT
private slots:
    void forwardsCommandsWithoutCapturing()
    {
        const auto binary = qEnvironmentVariable("SNIPLINE_BINARY");
        QVERIFY(!binary.isEmpty());
        QProcess resident;
        resident.start(binary, {"daemon"});
        QVERIFY(resident.waitForStarted());
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());
        QTRY_VERIFY(bus.interface()->isServiceRegistered("io.github.snipline.Application").value());
        auto run = [&binary](QStringList arguments) {
            QProcess client;
            client.start(binary, arguments);
            if (!client.waitForFinished(5000)) return -1;
            return client.exitCode();
        };
        QCOMPARE(run({"daemon"}), 0);
        QCOMPARE(run({"cancel"}), 0);
        QCOMPARE(run({"capture", "window"}), 1);
        // Offscreen platform must reject real capture even when forwarded.
        QCOMPARE(run({"capture", "region"}), 1);
        QCOMPARE(run({"capture", "region", "--live"}), 1);
        QCOMPARE(run({"capture", "window", "--live"}), 1);
        QCOMPARE(run({"daemon", "--format", "jpeg"}), 1);
        QCOMPARE(run({"quit"}), 0);
        QVERIFY(resident.waitForFinished(5000));
        QCOMPARE(resident.exitCode(), 0);
        QVERIFY(!bus.interface()->isServiceRegistered("io.github.snipline.Application").value());
    }
};

QTEST_GUILESS_MAIN(InstanceTest)
#include "instance_test.moc"
