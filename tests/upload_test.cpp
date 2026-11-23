#include "uploader.h"
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace snip;

class UploadTest : public QObject {
  Q_OBJECT
private slots:

  void publicHosts_data() {
    QTest::addColumn<QString>("destination");
    QTest::newRow("catbox") << QString("catbox");
    QTest::newRow("litterbox") << QString("litterbox-1h");
  }

  void publicHosts() {
    if (!qEnvironmentVariableIsSet("SNIPLINE_TEST_PUBLIC_UPLOADS")) {
      QSKIP("Opt-in synthetic public upload");
    }
    QFETCH(QString, destination);
    QTemporaryDir dir;
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(QColor(39, 64, 96));
    const auto path = dir.filePath("synthetic.png");
    QVERIFY(image.save(path));
    Uploader upload;
    QVERIFY(upload.start(path, destination, true, 60000));
    QTRY_VERIFY_WITH_TIMEOUT(!upload.busy(), 65000);
    QVERIFY2(upload.error().isEmpty(), qPrintable(upload.error()));
    qInfo() << upload.url();
  }

  void nativeTransport() {
    const auto python = QStandardPaths::findExecutable("python");
    if (python.isEmpty()) {
      QSKIP("Python fixture unavailable");
    }
    QTemporaryDir dir;
    const auto portFile = dir.filePath("port");
    const auto bodyFile = dir.filePath("body");
    QProcess server;
    server.start(python, {"-u", QFINDTESTDATA("fixtures/upload_server.py"), portFile, bodyFile});
    QVERIFY(server.waitForStarted());
    const auto stop = qScopeGuard([&] {
      server.kill();
      server.waitForFinished();
    });
    QTRY_VERIFY(QFileInfo(portFile).size() > 0);
    QFile port(portFile);
    QVERIFY(port.open(QIODevice::ReadOnly));
    const auto endpoint = "http://127.0.0.1:" + QString::fromUtf8(port.readAll()) + "/upload";
    const QJsonObject definition{{"Name", "Fixture"},       {"RequestURL", endpoint},
                                 {"RequestMethod", "POST"}, {"Body", "MultipartFormData"},
                                 {"FileFormName", "file"},  {"URL", "{response}"}};
    QFile profile(dir.filePath("host.sxcu"));
    QVERIFY(profile.open(QIODevice::WriteOnly));
    profile.write(QJsonDocument(definition).toJson());
    profile.close();
    QFile source(dir.filePath("file with spaces.mp4"));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("synthetic upload fixture");
    source.close();
    Uploader upload(nullptr, dir.filePath("storage"));
    upload.configure("sxcu:Fixture", {profile.fileName()});
    QVERIFY(!upload.busy());
    QVERIFY(!QFile::exists(bodyFile)); // Configuration is offline.
    QSignalSpy completed(&upload, &Uploader::completed);
    QVERIFY(upload.start(source.fileName(), upload.destination()));
    QTRY_VERIFY_WITH_TIMEOUT(!upload.busy(), 10000);
    QVERIFY2(upload.error().isEmpty(), qPrintable(upload.error()));
    QCOMPARE(completed.size(), 1);
    QCOMPARE(upload.url(), QUrl("https://example.com/fixture-upload"));
    QFile body(bodyFile);
    QVERIFY(body.open(QIODevice::ReadOnly));
    QVERIFY(body.readAll().contains("synthetic upload fixture"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), QByteArray("synthetic upload fixture"));
    source.close();
    auto changeProfile = [&](const QString &path, const QString &response) {
      auto custom = definition;
      custom["RequestURL"] = endpoint.left(endpoint.lastIndexOf('/')) + path;
      custom["URL"] = response;
      QVERIFY(profile.open(QIODevice::WriteOnly | QIODevice::Truncate));
      profile.write(QJsonDocument(custom).toJson());
      profile.close();
      upload.configure("sxcu:Fixture", {profile.fileName()});
    };
    changeProfile("/json", "{json:files[0].url}");
    QVERIFY(upload.start(source.fileName(), upload.destination()));
    QTRY_VERIFY(!upload.busy());
    QCOMPARE(upload.url(), QUrl("https://example.com/json-upload"));
    for (const auto &responseTemplate : {"{json:files[99].url}", "{json:files[0].missing}",
                                         "{json:files[invalid].url}", "{response}"}) {
      changeProfile("/json", responseTemplate);
      QVERIFY(upload.start(source.fileName(), upload.destination()));
      QTRY_VERIFY(!upload.busy());
      QVERIFY(upload.url().isEmpty());
      QVERIFY(upload.error().contains("valid HTTP(S) URL"));
    }
    changeProfile("/error", "{response}");
    QVERIFY(upload.start(source.fileName(), upload.destination()));
    QTRY_VERIFY(!upload.busy());
    QVERIFY(upload.error().contains("HTTP 503"));
    QVERIFY(upload.url().isEmpty());
    QSignalSpy cancelled(&upload, &Uploader::cancelled);
    QVERIFY(upload.start(source.fileName(), upload.destination()));
    upload.cancel();
    QCOMPARE(cancelled.size(), 1);
    QVERIFY(!upload.busy());
    QTest::qWait(50); // A queued completion cannot undo cancellation.
    QCOMPARE(upload.status(), QString("Cancelled"));
    QVERIFY(!upload.start(source.fileName(), "missing"));
    QVERIFY(upload.error().contains("fallback"));
    server.kill();
    server.waitForFinished();
    QVERIFY(upload.start(source.fileName(), upload.destination(), false, 1000));
    QTRY_VERIFY_WITH_TIMEOUT(!upload.busy(), 5000);
    QVERIFY(upload.error().contains("libcurl"));
    QVERIFY(upload.error().contains("HTTP"));
  }

  void profiles() {
    Uploader upload;
    for (const auto &id :
         {"catbox", "litterbox-1h", "litterbox-12h", "litterbox-24h", "litterbox-72h", "pomf"}) {
      QVERIFY(upload.hasDestination(id));
    }
    QVERIFY(!upload.hasDestination("uguu"));
  }
};
QTEST_GUILESS_MAIN(UploadTest)
#include "upload_test.moc"
