#include "core/config.h"
#include "core/recordingsession.h"
#include "notification.h"
#include <QUrl>
#include <QtTest>
using namespace snip;
class SettingsTest : public QObject {
  Q_OBJECT
private slots:
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
  void configIsTransactional() {
    Config config;
    QString error;
    QVERIFY(config.load(R"({"directory":"shots","format":"webp","live":true,
      "clipboard":false,"keybindings":{"confirm":["Ctrl+Return"]},
      "recording":{"format":"webm","fps":60,"countdown":0,"maxDuration":10}})", "/tmp/opencode", &error));
    QCOMPARE(config.directory, QString("/tmp/opencode/shots"));
    QCOMPARE(config.format, QByteArray("webp"));
    QCOMPARE(config.recordingFps, 60);
    QVERIFY(!config.clipboard);
    QVERIFY(config.load(R"({"recording":{"systemAudio":true,"microphone":false,
      "systemAudioDevice":"sink.monitor","microphoneDevice":"mic.source"}})", "/tmp/opencode", &error));
    QVERIFY(config.systemAudio);
    QVERIFY(!config.microphone);
    QCOMPARE(config.systemAudioDevice, QString("sink.monitor"));
    QCOMPARE(config.microphoneDevice, QString("mic.source"));
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
         R"({"recording":{"systemAudioDevice":""}})",
         R"({"filenameTemplate":false})"}) {
      QVERIFY2(!config.load(bad, "/elsewhere", &error), bad);
      QCOMPARE(config.format, QByteArray("webp"));
      QCOMPARE(config.directory, QString("/tmp/opencode/shots"));
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
