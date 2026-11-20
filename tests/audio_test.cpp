#include "audiosources.h"
#include <QFile>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QtTest>
using namespace snip;

class AudioTest : public QObject {
  Q_OBJECT
private slots:
  void friendlyNamesAndStableIdentity() {
    QString error;
    const auto sources = AudioSources::parse(
      R"([{"name":"usb.input","description":"USB podcast microphone","monitor_of_sink":4294967295},
          {"name":"headphones.monitor","description":"Headphones","monitor_of_sink":0}])",
      R"([{"index":41,"client":"7","properties":{"application.name":"Firefox","application.id":"firefox",
           "application.process.id":"123","application.process.binary":"firefox","media.name":"Video tab"}}])", &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(sources.size(), 5);
    QCOMPARE(sources[2].toMap()["label"].toString(), QString("Input — USB podcast microphone"));
    QCOMPARE(sources[3].toMap()["label"].toString(), QString("All apps — Headphones"));
    QCOMPARE(sources[4].toMap()["label"].toString(), QString("Firefox — Video tab"));
    const auto application = sources[4].toMap();
    QVERIFY(application["id"].toString().startsWith("app:41:"));
    QVariantMap properties{{"application.process.id", "123"}, {"application.process.binary", "firefox"},
                           {"application.id", "firefox"}, {"application.name", "Firefox"}};
    QCOMPARE(application["identity"].toString(), AudioSources::streamIdentity(properties, 7));
    properties["media.name"] = "Another tab";
    QCOMPARE(application["identity"].toString(), AudioSources::streamIdentity(properties, 7));
    properties["application.process.id"] = "999";
    QVERIFY(application["identity"].toString() != AudioSources::streamIdentity(properties, 7));
  }
  void windowMatchingDoesNotGuessSharedBrowserAudio() {
    QVariantList sources{
      QVariantMap{{"id", "a"}, {"kind", "application"}, {"pid", "123"}, {"appId", "firefox"}, {"mediaName", "Video"}},
      QVariantMap{{"id", "b"}, {"kind", "application"}, {"pid", "456"}, {"appId", "other"}, {"mediaName", "Music"}}};
    QVariantMap window{{"id", "one"}, {"pid", 123}, {"appId", "firefox"}, {"title", "Browser"}};
    QCOMPARE(AudioSources::forWindow(sources, window, {window}), QStringList{"a"});
    const QVariantMap other{{"id", "two"}, {"pid", 123}, {"appId", "firefox"}, {"title", "Another browser"}};
    QVERIFY(AudioSources::forWindow(sources, window, {window, other}).isEmpty());
    window["title"] = "Video";
    QCOMPARE(AudioSources::forWindow(sources, window, {window, other}), QStringList{"a"});
    window["pid"] = 999; // An exact application name cannot override a conflicting PID.
    QVERIFY(AudioSources::forWindow(sources, window, {window}).isEmpty());
    QVERIFY(AudioSources::parse("{}", "[]", nullptr).isEmpty());
    QVERIFY(AudioSources::parse("[]", "not JSON", nullptr).isEmpty());
  }
  void asynchronousPactlDiscoveryIsMetadataOnly() {
    QTemporaryDir directory;
    QFile executable(directory.filePath("pactl"));
    QVERIFY(executable.open(QIODevice::WriteOnly));
    executable.write("#!/bin/sh\n"
      "[ \"$1\" = '--format=json' ] && [ \"$2\" = list ] || exit 9\n"
      "case \"$3\" in\n"
      "sources) printf '%s' '[{\"name\":\"mic\",\"description\":\"Desk microphone\"}]' ;;\n"
      "sink-inputs) printf '%s' '[{\"index\":9,\"client\":1,\"properties\":{\"application.name\":\"Music\",\"application.process.id\":\"88\"}}]' ;;\n"
      "*) exit 9 ;;\nesac\n");
    executable.close();
    QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const auto oldPath = qgetenv("PATH");
    qputenv("PATH", directory.path().toUtf8());
    const auto restore = qScopeGuard([&] { qputenv("PATH", oldPath); });
    AudioSources sources;
    sources.refresh();
    QTRY_COMPARE(sources.sources().size(), 4);
    QVERIFY(sources.error().isEmpty());
    QVERIFY(!sources.loading());
    QVERIFY(executable.open(QIODevice::WriteOnly | QIODevice::Truncate));
    executable.write("#!/bin/sh\nexit 1\n");
    executable.close();
    sources.refresh();
    QTRY_VERIFY(!sources.error().isEmpty());
    QVERIFY(sources.sources().isEmpty());
  }
};
QTEST_GUILESS_MAIN(AudioTest)
#include "audio_test.moc"
