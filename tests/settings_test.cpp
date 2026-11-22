#include "core/config.h"
#include "core/recordingsession.h"
#include "notification.h"
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QKeySequence>
#include <QtTest>
using namespace snip;
class SettingsTest : public QObject {
  Q_OBJECT
private slots:
  void separateEditorCommands() {
    Config config;
    QString error;
    QVERIFY(config.load(R"({"screenshots":{"editor":"satty --filename -"},"recording":{"editor":"losslesscut {}"}})",
                        "/tmp/opencode", &error));
    QCOMPARE(config.screenshotEditor, "satty --filename -");
    QCOMPARE(config.videoEditor, "losslesscut {}");
    for (const auto &bad : {R"({"screenshots":{"editor":true}})",
                            R"({"screenshots":{"editor":"bad\ncommand"}})",
                            R"({"screenshots":{"editor":"{}"}})",
                            R"({"recording":{"editor":"{}"}})"}) {
      QVERIFY(!config.load(bad, "/tmp/opencode", &error));
      QCOMPARE(config.screenshotEditor, "satty --filename -");
      QCOMPARE(config.videoEditor, "losslesscut {}");
    }
  }
  void audioIsNotAJsonSetting() {
    Config config;
    QString error;
    for (const auto &bad : {
        R"({"recording":{"audio":{"system":true}}})",
        R"({"recording":{"systemAudio":true}})",
        R"({"recording":{"microphone":true}})",
        R"({"recording":{"systemAudioDevice":"desk.monitor"}})",
        R"({"recording":{"microphoneDevice":"desk.input"}})"}) {
      QVERIFY2(!config.load(bad, "/elsewhere", &error), bad);
      QVERIFY(!config.systemAudio);
      QVERIFY(!config.microphone);
      QCOMPARE(config.systemAudioDevice, "@DEFAULT_MONITOR@");
      QVERIFY(config.microphoneDevice.isEmpty());
    }
  }
  void toolAndTimingKeybindingsAreConfigurable() {
    Config config;
    QString error;
    QCOMPARE(config.keybindings["live"].toStringList(), QStringList{"Ctrl+L"});
    QCOMPARE(config.keybindings["cursor"].toStringList(), QStringList{"Ctrl+Shift+C"});
    QCOMPARE(config.keybindings["capture"].toStringList(), QStringList{"Ctrl+1"});
    QCOMPARE(config.keybindings["ocr"].toStringList(), QStringList{"Ctrl+4"});
    QCOMPARE(config.keybindings["region"].toStringList(), QStringList{"Shift+1"});
    QCOMPARE(config.keybindings["monitor"].toStringList(), QStringList{"Shift+2"});
    QCOMPARE(config.keybindings["window"].toStringList(), QStringList{"Shift+3"});
    const auto before = config.keybindings;
    QVERIFY(!config.load(R"({"keybindings":{"live":["Ctrl+1"]}})", "/tmp/opencode", &error));
    QCOMPARE(config.keybindings, before);
    QVERIFY(config.load(R"({"keybindings":{"live":["F5"],"capture":["F1"],"record":["F2"],"qr":["F3"],"ocr":["F4"]}})",
                        "/tmp/opencode", &error));
    QCOMPARE(config.keybindings["live"].toStringList(), QStringList{"F5"});
    QCOMPARE(config.keybindings["qr"].toStringList(), QStringList{"F3"});
    QVERIFY(config.load(R"({"keybindings":{"live":[]}})", "/tmp/opencode", &error));
    QVERIFY(config.keybindings["live"].toStringList().isEmpty());
  }
  void canonicalScreenshotsAndRecordingLayout() {
    Config config;
    QString error;
    QVERIFY(config.live);
    QCOMPARE(config.keybindings["confirm"].toStringList(), (QStringList{"Return", "Enter", "Space"}));
    QVERIFY2(config.load(R"({
      "screenshots":{"directory":"shots","target":"monitor","live":false,"format":"jpeg"},
      "recording":{"directory":"clips"},
      "clipboard":false,"notifications":false,"notificationTimeout":2500
    })", "/tmp/opencode", &error), qPrintable(error));
    QCOMPARE(config.directory, "/tmp/opencode/shots");
    QCOMPARE(config.recordingDirectory, "/tmp/opencode/clips");
    QCOMPARE(config.target, "monitor");
    QCOMPARE(config.format, QByteArray("jpeg"));
    QVERIFY(!config.live);
    QVERIFY(!config.clipboard);
    QVERIFY(!config.notifications);
    QCOMPARE(config.notificationTimeout, 2500);
    for (const auto &bad : {
        R"({"screenshots":true})", R"({"screenshots":{"unknown":true}})",
        R"({"screenshots":{"directory":""}})", R"({"screenshots":{"directory":false}})",
        R"({"screenshots":{"target":"invalid"}})", R"({"screenshots":{"live":1}})",
        R"({"screenshots":{"format":"gif"}})", R"({"screenshots":{"format":true}})",
        R"({"screenshots":{"target":false}})",
        R"({"screenshots":{"format":"png"},"format":"png"})",
        R"({"screenshots":{"target":"region"},"target":"window"})",
        R"({"screenshots":{"live":true},"live":false})",
        R"({"screenshots":{"directory":"new"},"directory":"old"})",
        R"({"screenshots":{"directory":"new"},"directory":{"screenshots":"old"}})",
        R"({"screenshots":{"directory":"new"},"recording":{"directory":false}})"}) {
      QVERIFY2(!config.load(bad, "/elsewhere", &error), bad);
      QCOMPARE(config.directory, "/tmp/opencode/shots");
      QCOMPARE(config.recordingDirectory, "/tmp/opencode/clips");
      QCOMPARE(config.target, "monitor");
      QCOMPARE(config.format, QByteArray("jpeg"));
      QVERIFY(!config.live);
    }
    QVERIFY(config.load(R"({"screenshots":{"directory":"~/shots"},"directory":{"recordings":"legacy-clips"}})", "/tmp/opencode", &error));
    QCOMPARE(config.directory, QDir::home().filePath("shots"));
    QCOMPARE(config.recordingDirectory, "/tmp/opencode/legacy-clips");
  }
  void shippedExampleMatchesDefaults() {
    const auto path = QFINDTESTDATA("../config.example.json");
    QVERIFY(!path.isEmpty());
    Config config, defaults;
    QString error;
    QVERIFY2(config.loadFile(path, false, &error), qPrintable(error));
    QCOMPARE(config.directory, QDir::home().filePath("Pictures/Snipline"));
    QCOMPARE(config.recordingDirectory, QDir::home().filePath("Videos/Snipline"));
    QCOMPARE(config.filenameTemplate, defaults.filenameTemplate);
    QCOMPARE(config.target, defaults.target);
    QCOMPARE(config.live, defaults.live);
    QCOMPARE(config.format, defaults.format);
    QCOMPARE(config.keybindings.keys(), defaults.keybindings.keys());
    for (const auto &action : config.keybindings.keys()) {
      const auto actual = config.keybindings[action].toStringList();
      const auto expected = defaults.keybindings[action].toStringList();
      QCOMPARE(actual.size(), expected.size());
      for (qsizetype i = 0; i < actual.size(); ++i)
        QCOMPARE(QKeySequence::fromString(actual[i], QKeySequence::PortableText),
                 QKeySequence::fromString(expected[i], QKeySequence::PortableText));
    }
    QCOMPARE(config.clipboard, defaults.clipboard);
    QCOMPARE(config.notifications, defaults.notifications);
    QCOMPARE(config.notificationTimeout, defaults.notificationTimeout);
    QCOMPARE(config.recordingFormat, defaults.recordingFormat);
    QCOMPARE(config.recordingCodec, defaults.recordingCodec);
    QCOMPARE(config.recordingEncoder, defaults.recordingEncoder);
    QCOMPARE(config.recordingFps, defaults.recordingFps);
    QCOMPARE(config.countdown, defaults.countdown);
    QCOMPARE(config.maxDuration, defaults.maxDuration);
    QCOMPARE(config.systemAudio, defaults.systemAudio);
    QCOMPARE(config.microphone, defaults.microphone);
    QCOMPARE(config.systemAudioDevice, defaults.systemAudioDevice);
    QCOMPARE(config.microphoneDevice, defaults.microphoneDevice);
    QCOMPARE(config.videoEditor, defaults.videoEditor);
    QCOMPARE(config.screenshotEditor, defaults.screenshotEditor);
    QCOMPARE(config.ocrLanguages, defaults.ocrLanguages);
  }
  void ocrLanguageConfiguration() {
    Config config;
    QString error;
    QVERIFY(config.load(R"({"ocr":{"languages":["eng","pol","script/Latin","eng"]}})", "/tmp/opencode", &error));
    QCOMPARE(config.ocrLanguages, (QStringList{"eng", "pol", "script/Latin"}));
    const auto before = config.ocrLanguages;
    for (const auto &json : {R"({"ocr":{"languages":[]}})", R"({"ocr":{"languages":["--bad"]}})",
                            R"({"ocr":{"languages":["../eng"]}})", R"({"ocr":{"languages":["eng+pol"]}})",
                            R"({"ocr":{"languages":["osd"]}})", R"({"ocr":{"unknown":true}})"}) {
      QVERIFY(!config.load(json, "/tmp/opencode", &error));
      QCOMPARE(config.ocrLanguages, before);
    }
  }
  void notificationsIncludeSavedScreenshot() {
    const QString path = "/tmp/opencode/screenshot with spaces #1.png";
    const auto message = screenshotNotification("Screenshot saved", "<saved>", path, 4000);
    const auto args = message.arguments();
    QCOMPARE(message.member(), QString("Notify"));
    QCOMPARE(args.size(), 8);
    QCOMPARE(QUrl(args[2].toString()).toLocalFile(), path);
    QVERIFY(args[2].toString().contains("%20"));
    QVERIFY(args[2].toString().contains("%23"));
    QCOMPARE(args[4].toString(), QString("&lt;saved&gt;"));
    QCOMPARE(QUrl(args[6].toMap().value("image-path").toString()).toLocalFile(), path);
    QCOMPARE(args[7].toInt(), 4000);
    QVERIFY(screenshotNotification("Error", "failed", {}, 5000).arguments()[6].toMap().isEmpty());
  }
  void directoryPathsSupportHomeExpansionAndLegacyConfigs() {
    Config config;
    QString error;
    QVERIFY(config.load(R"({"directory":{"screenshots":"~/shots","recordings":"~/clips"}})", "/elsewhere", &error));
    QCOMPARE(config.directory, QDir::home().filePath("shots"));
    QCOMPARE(config.recordingDirectory, QDir::home().filePath("clips"));
    QVERIFY(config.load(R"({"directory":"legacy-shots","recording":{"directory":"legacy-clips"}})", "/tmp/opencode", &error));
    QCOMPARE(config.directory, QString("/tmp/opencode/legacy-shots"));
    QCOMPARE(config.recordingDirectory, QString("/tmp/opencode/legacy-clips"));
    QVERIFY(config.load(R"({"directory":{"screenshots":"new-shots"}})", "/tmp/opencode", &error));
    QCOMPARE(config.directory, QString("/tmp/opencode/new-shots"));
    QCOMPARE(config.recordingDirectory, QString("/tmp/opencode/legacy-clips"));
  }
  void configIsTransactional() {
    Config config;
    QString error;
    QVERIFY(config.load(R"({"directory":{"screenshots":"shots","recordings":"clips"},"format":"webp","live":true,
      "clipboard":false,"keybindings":{"confirm":["Ctrl+Return"]},
      "recording":{"format":"webm","fps":60,"countdown":0,"maxDuration":10,"editor":"kdenlive --new"}})", "/tmp/opencode", &error));
    QCOMPARE(config.directory, QString("/tmp/opencode/shots"));
    QCOMPARE(config.recordingDirectory, QString("/tmp/opencode/clips"));
    QCOMPARE(config.format, QByteArray("webp"));
    QCOMPARE(config.recordingFps, 60);
    QCOMPARE(config.videoEditor, QString("kdenlive --new"));
    QVERIFY(!config.clipboard);
    QVERIFY(config.load(R"({"directory":{"recordings":"videos"}})", "/tmp/opencode", &error));
    QCOMPARE(config.recordingDirectory, QString("/tmp/opencode/videos"));
    QCOMPARE(config.directory, QString("/tmp/opencode/shots"));
    QVERIFY(config.load(R"({"recording":{"codec":"vp9","encoder":"auto"}})", "/tmp/opencode", &error));
    QCOMPARE(config.recordingCodec, QString("vp9"));
    QCOMPARE(config.recordingEncoder, QString("auto"));
    QVERIFY(config.load(R"({"filenameTemplate":"Screenshot_{date}_{time}"})", "/tmp/opencode", &error));
    QCOMPARE(config.filenameTemplate, QString("Screenshot_{date}_{time}"));
    Config custom;
    QVERIFY(custom.load(R"({"filenameTemplate":"Shot_{datetime:yyyyMMdd_HHmmss_zzz}"})", "/tmp/opencode", &error));
    QCOMPARE(custom.filenameTemplate, QString("Shot_{datetime:yyyyMMdd_HHmmss_zzz}"));
    for (const auto &bad : {R"({"format":"gif"})", R"({"live":1})",
         R"({"unknown":true})", R"({"recording":{"fps":30.5}})",
         R"({"keybindings":{"cancel":["Ctrl+Return"]}})",
         R"({"keybindings":{"confirm":["nonsense"]}})",
         R"({"filenameTemplate":"../outside"})",
         R"({"filenameTemplate":"{unknown}"})",
         R"({"filenameTemplate":"{datetime:}"})",
         R"({"filenameTemplate":"{datetime:yyyy/MM/dd}"})",
         R"({"recording":{"microphone":"yes"}})",
         R"({"recording":{"editor":true}})",
         R"({"recording":{"codec":"bogus"}})",
         R"({"recording":{"codec":"h264"}})",
         R"({"recording":{"encoder":"bogus"}})",
         R"({"recording":{"encoder":true}})",
         R"({"recording":{"directory":false}})",
         R"({"recording":{"directory":""}})",
         R"({"directory":false})",
         R"({"directory":{"screenshots":""}})",
         R"({"directory":{"recordings":17}})",
         R"({"directory":{"other":"path"}})",
         R"({"directory":{"screenshots":"changed","recordings":false}})",
         R"({"directory":{"recordings":"new"},"recording":{"directory":"conflicting"}})",
         R"({"recording":{"editor":"bad\ncommand"}})",
         R"({"recording":{"systemAudioDevice":""}})",
         R"({"filenameTemplate":false})"}) {
      QVERIFY2(!config.load(bad, "/elsewhere", &error), bad);
      QCOMPARE(config.format, QByteArray("webp"));
      QCOMPARE(config.directory, QString("/tmp/opencode/shots"));
      QCOMPARE(config.recordingDirectory, QString("/tmp/opencode/videos"));
      QCOMPARE(config.filenameTemplate, QString("Screenshot_{date}_{time}"));
    }
  }
  void recordingRequiresConfirmationAndIgnoresStaleCallbacks() {
    RecordingSession session;
    QSignalSpy prepare(&session, &RecordingSession::prepareRequested);
    QSignalSpy abort(&session, &RecordingSession::abortRequested);
    QVERIFY(session.begin("window", 0, 10));
    QCOMPARE(prepare.count(), 0);
    QVERIFY(session.confirm());
    const auto generation = session.generation();
    QCOMPARE(prepare.count(), 1);
    QCOMPARE(session.state(), RecordingSession::State::Preparing);
    session.discard();
    QCOMPARE(abort.count(), 1);
    session.backendStarted(generation);
    QCOMPARE(session.state(), RecordingSession::State::Idle);
    QVERIFY(session.begin("monitor", 0, 10));
    session.confirm();
    session.backendFailed(generation, "stale error");
    QCOMPARE(session.state(), RecordingSession::State::Preparing);
  }
  void pauseStopAndSave() {
    RecordingSession session;
    QSignalSpy finalize(&session, &RecordingSession::finalizeRequested);
    QSignalSpy pauses(&session, &RecordingSession::pauseRequested);
    QVERIFY(session.begin("region", 0, 10));
    session.confirm();
    const auto generation = session.generation();
    session.backendStarted(generation);
    session.pause();
    session.pause();
    QCOMPARE(pauses.count(), 1);
    QCOMPARE(session.state(), RecordingSession::State::Recording); // Await backend ACK.
    session.backendPaused(generation, true);
    QCOMPARE(session.state(), RecordingSession::State::Paused);
    const auto elapsed = session.elapsedMs();
    QTest::qWait(10);
    QCOMPARE(session.elapsedMs(), elapsed);
    session.resume();
    session.backendPaused(generation, false);
    session.stop();
    session.stop();
    QCOMPARE(finalize.count(), 1);
    QCOMPARE(session.state(), RecordingSession::State::Finalizing);
    session.backendSaved(generation, "/saved/test.mp4");
    QCOMPARE(session.state(), RecordingSession::State::Saved);
  }
  void countdownCanBeCancelledAndLimitStops() {
    RecordingSession session;
    QSignalSpy prepare(&session, &RecordingSession::prepareRequested);
    QVERIFY(session.begin("monitor", 2, 1));
    session.confirm();
    QCOMPARE(session.countdownRemaining(), 2);
    session.discard();
    QCOMPARE(prepare.count(), 0);
    QVERIFY(session.begin("monitor", 0, 1));
    session.confirm();
    session.backendStarted(session.generation());
    QTRY_COMPARE(session.state(), RecordingSession::State::Finalizing);
    session.backendFailed(session.generation(), "disk full");
    QCOMPARE(session.state(), RecordingSession::State::Failed);
    QCOMPARE(session.error(), QString("disk full"));
  }
};
QTEST_GUILESS_MAIN(SettingsTest)
#include "settings_test.moc"
