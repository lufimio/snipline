#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QProcess>
#include <QTemporaryDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QtTest>

class InstanceTest : public QObject {
    Q_OBJECT
private slots:
    void helpUsesSubcommandsWithoutAGraphicalSession()
    {
        const auto binary = qEnvironmentVariable("SNIPLINE_BINARY");
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "nonexistent-snipline-test-platform");
        const QList<QStringList> commands{
            {"--help"}, {"help", "record"}, {"record", "--help"},
            {"record", "pause", "--help"}, {"record", "resume", "--help"},
            {"record", "stop", "--help"}, {"capture", "--help"},
            {"help", "capture", "window"}, {"--help-all"}};
        for (const auto &arguments : commands) {
            QProcess process;
            process.setProcessEnvironment(environment);
            process.start(binary, arguments + QStringList{"--config", "/nonexistent/snipline-config.json"});
            QVERIFY(process.waitForFinished(5000));
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(process.exitCode(), 0);
            const auto output = process.readAllStandardOutput();
            QVERIFY(output.contains("Usage: snipline"));
            QVERIFY(!output.contains("Arguments:"));
            QVERIFY(!output.contains("[command]"));
            if (arguments == QStringList{"--help"}) {
                QVERIFY(output.contains("Commands:"));
                QVERIFY(output.contains("Toggle recording"));
            }
            if (arguments == QStringList{"help", "record"}) QVERIFY(output.contains("Subcommands:"));
            if (arguments.contains("pause") || arguments.contains("resume"))
                QVERIFY(output.contains("same toggle"));
        }
        for (const auto &arguments : QList<QStringList>{
                 {"help", "unknown"}, {"pause"}, {"record", "unknown"},
                 {"record", "stop", "extra"}}) {
            QProcess process;
            process.setProcessEnvironment(environment);
            process.start(binary, arguments);
            QVERIFY(process.waitForFinished(5000));
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(process.exitCode(), 1);
        }
    }
    void forwardsCommandsWithoutCapturing()
    {
        const auto binary = qEnvironmentVariable("SNIPLINE_BINARY");
        QVERIFY(!binary.isEmpty());
        QTemporaryDir configDirectory;
        QVERIFY(configDirectory.isValid());
        qputenv("XDG_CONFIG_HOME", configDirectory.path().toUtf8());
        QFile config(configDirectory.filePath("custom.json"));
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(R"({"format":"webp","clipboard":false})");
        config.close();
        QProcess resident;
        resident.start(binary, {"daemon", "--config", config.fileName()});
        QVERIFY(resident.waitForStarted());
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());
        QTRY_VERIFY(bus.interface()->isServiceRegistered("io.github.snipline.Application").value());
        auto run = [&binary](QStringList arguments, QString *error = nullptr) {
            QProcess client;
            client.start(binary, arguments);
            if (!client.waitForFinished(5000)) return -1;
            if (error) *error = QString::fromUtf8(client.readAllStandardError());
            return client.exitCode();
        };
        QCOMPARE(run({"daemon"}), 0);
        QCOMPARE(run({"daemon", "--config", config.fileName()}), 1);
        QCOMPARE(run({"daemon", "--config", configDirectory.filePath("missing.json")}), 1);
        QCOMPARE(run({"cancel"}), 0);
        QCOMPARE(run({"record", "status"}), 0);
        QString error;
        QCOMPARE(run({"record", "pause"}, &error), 1);
        QVERIFY(error.contains("No active recording to pause/resume."));
        QCOMPARE(run({"record", "resume"}, &error), 1);
        QVERIFY(error.contains("No active recording to pause/resume."));
        QCOMPARE(run({"record", "stop"}), 1);
        QCOMPARE(run({"record", "discard"}), 0);
        QCOMPARE(run({"pause"}), 1); // Controls are no longer top-level commands.
        QCOMPARE(run({"record", "stop", "extra"}), 1);
        QCOMPARE(run({"record", "status", "--microphone"}), 1);
        QCOMPARE(run({"record", "window"}), 1); // Offscreen must reject real recording.
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
        resident.start(binary, {"daemon"});
        QVERIFY(resident.waitForStarted());
        QTRY_VERIFY(bus.interface()->isServiceRegistered("io.github.snipline.Application").value());
        resident.terminate(); // SIGTERM is handled on Qt's loop, not abrupt termination.
        QVERIFY(resident.waitForFinished(5000));
        QCOMPARE(resident.exitStatus(), QProcess::NormalExit);
        QCOMPARE(resident.exitCode(), 0);
    }
};

QTEST_GUILESS_MAIN(InstanceTest)
#include "instance_test.moc"
