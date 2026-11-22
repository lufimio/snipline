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
            {"help", "capture", "window"}, {"qr", "--help"}, {"ocr", "--help"},
            {"help", "ocr", "region"}, {"capture", "region", "--help"},
            {"capture", "monitor", "--help"}, {"qr", "window", "--help"},
            {"qr", "monitor", "--help"}, {"ocr", "window", "--help"},
            {"record", "region", "--help"}, {"record", "monitor", "--help"},
            {"record", "window", "--help"}, {"record", "discard", "--help"},
            {"record", "status", "--help"}, {"daemon", "--help"},
            {"cancel", "--help"}, {"quit", "--help"}, {"doctor", "--help"},
            {"upload", "--help"}, {"lens", "--help"}, {"lens", "window", "--help"},
            {"recent", "--help"}, {"forget", "--help"}, {"--help-all"}};
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
            QVERIFY(!output.contains("snipline [command]"));
            if (arguments == QStringList{"--help"}) {
                QVERIFY(output.contains("Commands:"));
                QVERIFY(output.contains("Toggle recording"));
            }
            if (arguments == QStringList{"help", "record"}) QVERIFY(output.contains("Subcommands:"));
             if (arguments.contains("pause") || arguments.contains("resume"))
                 QVERIFY(output.contains("same toggle"));
             if (arguments == QStringList{"qr", "--help"} || arguments == QStringList{"ocr", "--help"}) {
                 QVERIFY(output.contains("[target]"));
                 QVERIFY(output.contains("--file"));
                 QVERIFY(output.contains("--live"));
                 QVERIFY(output.contains("--frozen"));
                 QVERIFY(!output.contains("default frozen"));
                 QVERIFY(!output.contains("--video-format"));
             }
             if (arguments == QStringList{"help", "ocr", "region"}) {
                 QVERIFY(output.contains("Positional arguments:"));
                 QVERIFY(!output.contains("  --file <path>"));
                 QVERIFY(output.contains("--live"));
             }
             QVERIFY(!output.contains("config.target"));
             QVERIFY(!output.contains("config.live"));
             QVERIFY(!output.contains("ocr.languages"));
             QVERIFY(!output.contains("config default"));
             QVERIFY(!output.contains("30 FPS"));
             QVERIFY(!output.contains("countdown 3"));
             QVERIFY(!output.contains("Startup settings:"));
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
        config.write(R"({"screenshots":{"format":"webp","live":true},"clipboard":false})");
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
        QCOMPARE(run({"recent"}), 0);
        QCOMPARE(run({"forget"}), 0);
        QCOMPARE(run({"daemon", "--tray"}), 1); // Cannot change an existing instance's mode.
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
        QCOMPARE(run({"qr", "region"}), 1);
        QCOMPARE(run({"qr", "region", "--live"}, &error), 1);
        QVERIFY(error.contains("Wayland")); // Forwarded timing request, not rejected by parser.
        QCOMPARE(run({"ocr", "monitor", "--frozen"}, &error), 1);
        QVERIFY(error.contains("Wayland"));
        QCOMPARE(run({"qr", "--live", "--frozen"}), 1);
        QCOMPARE(run({"ocr", "--file", "image.png", "--live"}), 1);
        QCOMPARE(run({"qr", "--file", "image.png", "--frozen"}), 1);
        QCOMPARE(run({"qr", "--file", configDirectory.filePath("missing.png")}), 0);
        QCOMPARE(run({"cancel"}), 0);
        QCOMPARE(run({"ocr", "--file", configDirectory.filePath("missing.png")}), 0);
        QCOMPARE(run({"cancel"}), 0);
        QCOMPARE(run({"capture", "--file", "image.png"}), 1);
        QCOMPARE(run({"qr", "region", "--file", "image.png"}), 1);
        QCOMPARE(run({"upload"}), 1);
        QCOMPARE(run({"upload", "--file", configDirectory.filePath("missing.png")}), 1);
        QCOMPARE(run({"upload", "--file", "missing.png", "--service", "missing-host"}, &error), 1);
        QVERIFY(error.contains("fallback") || error.contains("grabit"));
        QCOMPARE(run({"lens", "--file", configDirectory.filePath("missing.png")}), 1);
        QCOMPARE(run({"lens", "--file", "image.png", "--live"}), 1);
        QCOMPARE(run({"lens", "region"}), 1); // Offscreen rejects capture; never uploads.
        QCOMPARE(run({"daemon", "--format", "jpeg"}), 1);
        QCOMPARE(run({"quit"}), 0);
        QVERIFY(resident.waitForFinished(5000));
        QCOMPARE(resident.exitCode(), 0);
        QVERIFY(!bus.interface()->isServiceRegistered("io.github.snipline.Application").value());
        resident.start(binary, {"daemon", "--tray"});
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
