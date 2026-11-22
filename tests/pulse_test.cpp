#include "audiosources.h"
#include "recorder.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>
#include <cmath>
#include <cstring>
#include <numbers>

using namespace snip;
class PulseTest : public QObject {
  Q_OBJECT
  QTemporaryDir m_runtime;
  QProcess m_core, m_pulse, m_policy;
  QMap<QByteArray, QByteArray> m_previous;
  void env(const QByteArray &name, const QByteArray &value) {
    m_previous[name] = qgetenv(name.constData());
    qputenv(name.constData(), value);
  }
  QProcess *producer(const QString &app, double frequency, int rate, const QString &device = QStringLiteral("snipline_test")) {
    // Feed known, continuous PCM through an independent native Pulse client.
    // A live GStreamer test-source/sink pair can itself reslave/drop samples
    // during graph changes, confounding input distortion with recorder errors.
    QFile file(m_runtime.filePath(app + ".f32"));
    if (!file.open(QIODevice::WriteOnly)) return nullptr;
    QByteArray pcm(rate * 10 * 8, Qt::Uninitialized);
    for (int i = 0; i < rate * 10; ++i) {
      const float value = 0.1 * std::sin(2 * std::numbers::pi * frequency * i / rate);
      std::memcpy(pcm.data() + i * 8, &value, 4);
      std::memcpy(pcm.data() + i * 8 + 4, &value, 4);
    }
    if (file.write(pcm) != pcm.size()) return nullptr;
    file.close();
    auto *process = new QProcess(this);
    process->start("paplay", {"--raw", "--format=float32le", "--rate=" + QString::number(rate),
        "--channels=2", "--server=" + QString::fromUtf8(qgetenv("PULSE_SERVER")), "--device=" + device,
        "--property=application.name=" + app, "--property=application.id=" + app, file.fileName()});
    if (!process->waitForStarted(5000)) { delete process; return nullptr; }
    return process;
  }
  void stopProducer(QProcess *process) {
    if (!process) return;
    if (process->state() != QProcess::NotRunning) {
      process->terminate();
      if (!process->waitForFinished(2000)) { process->kill(); process->waitForFinished(); }
    }
    delete process;
  }
private slots:
  void initTestCase() {
    if (QStandardPaths::findExecutable("pipewire").isEmpty() ||
        QStandardPaths::findExecutable("pipewire-pulse").isEmpty() ||
        QStandardPaths::findExecutable("wireplumber").isEmpty() ||
        QStandardPaths::findExecutable("pactl").isEmpty() ||
        QStandardPaths::findExecutable("paplay").isEmpty() ||
        QStandardPaths::findExecutable("ffmpeg").isEmpty())
      QSKIP("Private PipeWire/PulseAudio integration test requires pipewire, pipewire-pulse, pactl, paplay and ffmpeg.");
    QVERIFY(m_runtime.isValid());
    // A separate core/server with no hardware nodes or hardware monitors.
    // Every command and stream uses this private socket, never the user's server.
    env("XDG_RUNTIME_DIR", m_runtime.path().toUtf8());
    env("XDG_CONFIG_HOME", m_runtime.filePath("config").toUtf8());
    env("XDG_CONFIG_DIRS", "/nonexistent");
    env("XDG_STATE_HOME", m_runtime.filePath("state").toUtf8());
    env("XDG_CACHE_HOME", m_runtime.filePath("cache").toUtf8());
    env("XDG_DATA_HOME", m_runtime.filePath("data").toUtf8());
    env("WIREPLUMBER_CONFIG_DIR", "/usr/share/wireplumber");
    env("PIPEWIRE_RUNTIME_DIR", m_runtime.path().toUtf8());
    env("PIPEWIRE_CORE", "pipewire-0");
    env("PIPEWIRE_REMOTE", "pipewire-0");
    env("PULSE_RUNTIME_PATH", m_runtime.filePath("pulse").toUtf8());
    env("PULSE_SERVER", "unix:" + m_runtime.filePath("pulse/native").toUtf8());
    QVERIFY(QDir().mkpath(m_runtime.filePath("pulse")));
    env("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent");
    env("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent");
    QFile coreConfig(m_runtime.filePath("test-core.conf"));
    QVERIFY(coreConfig.open(QIODevice::WriteOnly));
    coreConfig.write(R"(
context.properties = { core.daemon = true core.name = pipewire-0 default.clock.rate = 48000 }
context.spa-libs = {
  support.* = support/libspa-support
  audio.convert.* = audioconvert/libspa-audioconvert
}
context.modules = [
  { name = libpipewire-module-protocol-native }
  { name = libpipewire-module-access args = { access.socket = { pipewire-0 = "unrestricted" } } }
  { name = libpipewire-module-metadata }
  { name = libpipewire-module-spa-node-factory }
  { name = libpipewire-module-client-node }
  { name = libpipewire-module-adapter }
  { name = libpipewire-module-link-factory }
]
context.objects = [
  { factory = spa-node-factory args = {
      factory.name = support.node.driver node.name = Dummy-Driver
      node.group = pipewire.dummy priority.driver = 20000
  } }
]
)");
    coreConfig.close();
    m_core.start("pipewire", {"-c", coreConfig.fileName()});
    QVERIFY(m_core.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(m_runtime.filePath("pipewire-0")), 5000);
    QFile pulseConfig(m_runtime.filePath("test-pulse.conf"));
    QVERIFY(pulseConfig.open(QIODevice::WriteOnly));
    pulseConfig.write(QString(
        "context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }\n"
        "context.modules = [ { name = libpipewire-module-protocol-native } "
        "{ name = libpipewire-module-client-node } { name = libpipewire-module-adapter } "
        "{ name = libpipewire-module-metadata } "
        "{ name = libpipewire-module-protocol-pulse args = { server.address = [ \"unix:%1\" ] } } ]\n")
        .arg(m_runtime.filePath("pulse/native")).toUtf8());
    pulseConfig.close();
    m_pulse.start("pipewire-pulse", {"-c", pulseConfig.fileName()});
    QVERIFY(m_pulse.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(m_runtime.filePath("pulse/native")) ||
                            m_pulse.state() == QProcess::NotRunning, 5000);
    QVERIFY2(QFile::exists(m_runtime.filePath("pulse/native")), m_pulse.readAllStandardError().constData());
    // Policy-only profile configures/links virtual nodes. It does not load
    // ALSA, Bluetooth or video device monitors.
    m_policy.start("wireplumber", {"--profile", "policy"});
    QVERIFY(m_policy.waitForStarted());
    QProcess createSink;
    createSink.start("pactl", {"load-module", "module-null-sink", "sink_name=snipline_test", "rate=48000", "channels=2"});
    QVERIFY(createSink.waitForFinished(5000));
    QVERIFY2(createSink.exitCode() == 0, createSink.readAllStandardError().constData());
    createSink.start("pactl", {"load-module", "module-null-sink", "sink_name=snipline_native44100", "rate=44100", "channels=2"});
    QVERIFY(createSink.waitForFinished(5000));
    QVERIFY2(createSink.exitCode() == 0, createSink.readAllStandardError().constData());
    gst_init(nullptr, nullptr);
  }
  void pausedApplicationProducesValidSilenceAndCanResume_data() {
    QTest::addColumn<bool>("resume");
    QTest::addColumn<bool>("closeQuietSource");
    QTest::newRow("quiet") << false << false;
    QTest::newRow("resume") << true << false;
    QTest::newRow("closed-quiet-source") << false << true;
  }
  void pausedApplicationProducesValidSilenceAndCanResume() {
    QFETCH(bool, resume);
    QFETCH(bool, closeQuietSource);
    GError *error = nullptr;
    auto *player = gst_parse_launch(
        "audiotestsrc is-live=true freq=440 volume=0.1 ! audio/x-raw,rate=48000,channels=2 "
        "! pulsesink name=player sync=true async=false", &error);
    QVERIFY(player && !error);
    const auto cleanup = qScopeGuard([&] { gst_element_set_state(player, GST_STATE_NULL); gst_object_unref(player); });
    auto *sink = gst_bin_get_by_name(GST_BIN(player), "player");
    const auto appId = QString("Snipline-paused-%1").arg(QTest::currentDataTag());
    auto *properties = gst_structure_new("props", "application.name", G_TYPE_STRING, appId.toUtf8().constData(),
        "application.id", G_TYPE_STRING, appId.toUtf8().constData(), nullptr);
    g_object_set(sink, "device", "snipline_test", "stream-properties", properties, nullptr);
    gst_structure_free(properties);
    gst_object_unref(sink);
    QVERIFY(gst_element_set_state(player, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    QTest::qWait(150);
    QVERIFY(gst_element_set_state(player, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE);
    AudioSources catalog;
    QVariantMap chosen;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
      catalog.refresh();
      for (const auto &entry : catalog.sources())
        if (entry.toMap().value("appId") == appId && entry.toMap().value("corked").toBool()) chosen = entry.toMap();
      return !chosen.isEmpty();
    })(), 5000);
    RecordingOptions options;
    options.directory = m_runtime.filePath(appId + "-output");
    options.filenameTemplate = "PausedApplication";
    options.explicitAudioInputs = true;
    options.audioInputs = {chosen};
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("Required codecs unavailable.");
    VideoRecorder recorder;
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QSignalSpy saved(&recorder, &VideoRecorder::saved);
    QSignalSpy lost(&recorder, &VideoRecorder::sourceLost);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::blue);
    recorder.start(1, options, frame);
    QTRY_VERIFY_WITH_TIMEOUT(started.count() || failed.count(), 5000);
    QVERIFY(failed.isEmpty());
    QTest::qWait(300);
    if (resume) {
      gst_element_set_state(player, GST_STATE_PLAYING);
      QTest::qWait(700);
    }
    if (closeQuietSource) gst_element_set_state(player, GST_STATE_NULL);
    else recorder.stop();
    QTRY_VERIFY_WITH_TIMEOUT(saved.count() || failed.count(), 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first()[1].toString()));
    QCOMPARE(saved.count(), 1);
    QCOMPARE(lost.count(), closeQuietSource ? 1 : 0);
    QProcess decode;
    decode.start("ffmpeg", {"-nostdin", "-v", "error", "-i", saved.first()[1].toString(),
        "-map", "0:a:0", "-ar", "48000", "-ac", "1", "-f", "f32le", "-"});
    QVERIFY(decode.waitForFinished(5000));
    QCOMPARE(decode.exitCode(), 0);
    const auto pcm = decode.readAllStandardOutput();
    const auto samples = pcm.size() / sizeof(float);
    QVERIFY(samples > 10000);
    double maximum = 0, tailEnergy = 0;
    for (qsizetype i = 0; i < qsizetype(samples); ++i) {
      float value;
      std::memcpy(&value, pcm.constData() + i * sizeof(float), sizeof(float));
      maximum = qMax(maximum, double(std::abs(value)));
      if (i > qsizetype(samples) - 4800) tailEnergy += value * value;
    }
    if (resume) QVERIFY(tailEnergy / 4800 > 0.00005);
    else QVERIFY(maximum < 0.001);
  }
  void applicationMonitorExcludesTheOtherApplication_data() {
    QTest::addColumn<bool>("application");
    QTest::addColumn<QString>("format");
    QTest::addColumn<int>("rate");
    QTest::addColumn<QString>("sourceEvent");
    QTest::addColumn<int>("sinkRate");
    QTest::newRow("isolated-opus") << true << QString("webm") << 48000 << QString{} << 48000;
    QTest::newRow("isolated-aac") << true << QString("mp4") << 48000 << QString{} << 48000;
    QTest::newRow("output-opus") << false << QString("webm") << 48000 << QString{} << 48000;
    QTest::newRow("output-aac") << false << QString("mp4") << 48000 << QString{} << 48000;
    QTest::newRow("isolated-resampled") << true << QString("webm") << 44100 << QString{} << 48000;
    QTest::newRow("isolated-aac-resampled") << true << QString("mp4") << 44100 << QString{} << 48000;
    QTest::newRow("native-44100") << true << QString("webm") << 44100 << QString{} << 44100;
    QTest::newRow("native-44100-app-48000") << true << QString("mp4") << 48000 << QString{} << 44100;
    QTest::newRow("output-resampled") << false << QString("webm") << 44100 << QString{} << 48000;
    QTest::newRow("identity-mismatch") << true << QString("webm") << 48000 << QString("identity") << 48000;
    QTest::newRow("closed-application") << true << QString("webm") << 48000 << QString("close") << 48000;
    QTest::newRow("moved-application") << true << QString("webm") << 48000 << QString("move") << 48000;
    QTest::newRow("server-disconnect") << true << QString("webm") << 48000 << QString("server") << 48000;
  }
  void applicationMonitorExcludesTheOtherApplication() {
    QFETCH(bool, application);
    QFETCH(QString, format);
    QFETCH(int, rate);
    QFETCH(QString, sourceEvent);
    QFETCH(int, sinkRate);
    RecordingOptions options;
    options.directory = m_runtime.filePath("output/" + QString::fromLatin1(QTest::currentDataTag()));
    options.filenameTemplate = "IsolatedApp";
    options.format = format;
    options.explicitAudioInputs = true;
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("WebM encoding plugins unavailable.");
    const auto appA = QString("Snipline-test-A-%1").arg(QTest::currentDataTag());
    const auto appB = QString("Snipline-test-B-%1").arg(QTest::currentDataTag());
    const auto device = sinkRate == 48000 ? QString("snipline_test") : QString("snipline_native44100");
    auto *a = producer(appA, 440, rate, device);
    auto *b = producer(appB, 880, rate, device);
    const auto stopPlayers = qScopeGuard([&] {
      stopProducer(a);
      stopProducer(b);
      QFile::remove(m_runtime.filePath(appA + ".f32"));
      QFile::remove(m_runtime.filePath(appB + ".f32"));
    });
    QVERIFY(a && b);
    AudioSources catalog;
    QVariantMap chosen;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
      catalog.refresh();
      bool second = false;
      for (const auto &value : catalog.sources()) {
        if (value.toMap().value("appId") == appA) chosen = value.toMap();
        if (value.toMap().value("appId") == appB) second = true;
      }
      return !chosen.isEmpty() && second;
    })(), 5000);
    if (sourceEvent == "identity") chosen["identity"] = "not-the-selected-client";
    options.audioInputs = {application ? chosen : QVariantMap{
        {"id", "test-output"}, {"kind", "device"}, {"device", "snipline_test.monitor"}}};
    VideoRecorder recorder;
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy paused(&recorder, &VideoRecorder::paused);
    QSignalSpy saved(&recorder, &VideoRecorder::saved);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QSignalSpy lost(&recorder, &VideoRecorder::sourceLost);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::green);
    recorder.start(1, options, frame);
    if (sourceEvent == "identity") {
      QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 5000);
      QVERIFY(failed.first()[1].toString().contains("identity"));
      QVERIFY(saved.isEmpty());
      QVERIFY(QDir(options.directory).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
      return; // A reused index must never record a different application.
    }
    QTRY_VERIFY_WITH_TIMEOUT(started.count() || failed.count(), 5000);
    QTest::qWait(700);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][1].toString()));
    if (application)
      QCOMPARE(recorder.audioDiagnostics().first().toMap().value("capture").toMap().value("sampleRate").toInt(), sinkRate);
    const auto beforeStall = recorder.audioDiagnostics().first().toMap()
                                 .value("capture").toMap().value("pcmBytes").toULongLong();
    // Deliberately block Qt's thread, just as expensive capture/image copying
    // can. The dedicated PulseAudio thread must continue delivering samples.
    QTest::qSleep(650);
    const auto afterStall = recorder.audioDiagnostics().first().toMap()
                                .value("capture").toMap().value("pcmBytes").toULongLong();
    if (application)
      QVERIFY2(afterStall > beforeStall + 48000, "Audio stopped while Qt's event loop was blocked.");
    QTest::qWait(100);
    recorder.setPaused(true);
    QTRY_COMPARE(paused.count(), 1);
    QTest::qWait(100);
    recorder.setPaused(false);
    QTRY_COMPARE(paused.count(), 2);
    QTest::qWait(400);
    if (sourceEvent == "close") {
      a->terminate();
      QVERIFY(a->waitForFinished(5000));
    }
    else if (sourceEvent == "move") {
      QProcess command;
      command.start("pactl", {"load-module", "module-null-sink", "sink_name=snipline_other", "rate=48000", "channels=2"});
      QVERIFY(command.waitForFinished(5000));
      QCOMPARE(command.exitCode(), 0);
      command.start("pactl", {"move-sink-input", chosen.value("index").toString(), "snipline_other"});
      QVERIFY(command.waitForFinished(5000));
      QCOMPARE(command.exitCode(), 0);
      QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 3000);
    } else if (sourceEvent == "server") {
      m_pulse.terminate(); // Only the fixture's private server.
      QVERIFY(m_pulse.waitForFinished(5000));
    } else recorder.stop();
    QTRY_VERIFY_WITH_TIMEOUT(saved.count() || failed.count(), 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][1].toString()));
    QCOMPARE(saved.count(), 1);
    QCOMPARE(lost.count(), sourceEvent.isEmpty() ? 0 : 1);
    QProcess decode;
    decode.start("ffmpeg", {"-nostdin", "-v", "error", "-i", saved[0][1].toString(),
                           "-map", "0:a:0", "-ar", "48000", "-ac", "1", "-f", "f32le", "-"});
    QVERIFY(decode.waitForFinished(5000));
    QCOMPARE(decode.exitCode(), 0);
    const auto pcm = decode.readAllStandardOutput();
    const auto samples = pcm.size() / sizeof(float);
    QVERIFY(samples > 10000);
    // Inspect the body, not the strongest single window. Repeated silence or
    // shredded PCM used to pass because one intact tone fragment was enough.
    int checkedWindows = 0, cleanWindows = 0;
    for (qsizetype offset = 9600; offset + 9600 < qsizetype(samples); offset += 4800) {
      double real = 0, imaginary = 0, otherReal = 0, otherImaginary = 0, energy = 0;
      for (qsizetype i = 0; i < 4800; ++i) {
        float value;
        std::memcpy(&value, pcm.constData() + (offset + i) * sizeof(float), sizeof(float));
        const auto angle = 2 * std::numbers::pi * 440 * double(i) / 48000;
        real += value * std::cos(angle);
        imaginary += value * std::sin(angle);
        otherReal += value * std::cos(angle * 2);
        otherImaginary += value * std::sin(angle * 2);
        energy += value * value;
      }
      const auto toneEnergy = 2 * (real * real + imaginary * imaginary) / (4800.0 * 4800);
      const auto otherEnergy = 2 * (otherReal * otherReal + otherImaginary * otherImaginary) / (4800.0 * 4800);
      // A full output contains both tones. Check their combined purity rather
      // than assuming the server/policy gives both applications identical gain.
      const auto expectedEnergy = toneEnergy + (application ? 0 : otherEnergy);
      if (energy / 4800 > 0.00005 && expectedEnergy / (energy / 4800) > 0.90) ++cleanWindows;
      ++checkedWindows;
    }
    QVERIFY(checkedWindows >= 5);
    QVERIFY2(cleanWindows >= checkedWindows - 2,
             qPrintable(QString("Only %1/%2 continuous clean tone windows").arg(cleanWindows).arg(checkedWindows)));
    auto amplitude = [&](double frequency) {
      double maximum = 0;
      // Analyze short windows so removing a pause does not cancel the tone
      // through an otherwise legitimate phase discontinuity.
      for (qsizetype offset = 0; offset + 4800 <= qsizetype(samples); offset += 4800) {
        double real = 0, imaginary = 0;
        for (qsizetype i = 0; i < 4800; ++i) {
          float value;
          std::memcpy(&value, pcm.constData() + (offset + i) * sizeof(float), sizeof(float));
          const double angle = 2 * std::numbers::pi * frequency * double(i) / 48000;
          real += value * std::cos(angle); imaginary += value * std::sin(angle);
        }
        maximum = qMax(maximum, 2 * std::hypot(real, imaginary) / 4800);
      }
      return maximum;
    };
    QVERIFY2(amplitude(440) > 0.01, qPrintable(QString("Selected tone: %1").arg(amplitude(440))));
    if (application)
      QVERIFY2(amplitude(880) < 0.005, qPrintable(QString("Unselected tone leaked: %1").arg(amplitude(880))));
    else
      QVERIFY2(amplitude(880) > 0.01, "Full output should contain both applications.");
  }
  void cleanupTestCase() {
    m_policy.terminate();
    if (!m_policy.waitForFinished(2000)) { m_policy.kill(); m_policy.waitForFinished(); }
    if (m_pulse.state() != QProcess::NotRunning) {
      m_pulse.terminate();
      if (!m_pulse.waitForFinished(2000)) { m_pulse.kill(); m_pulse.waitForFinished(); }
    }
    m_core.terminate();
    if (!m_core.waitForFinished(2000)) { m_core.kill(); m_core.waitForFinished(); }
    for (auto it = m_previous.begin(); it != m_previous.end(); ++it) {
      if (it.value().isNull()) qunsetenv(it.key().constData());
      else qputenv(it.key().constData(), it.value());
    }
  }
};
QTEST_GUILESS_MAIN(PulseTest)
#include "pulse_test.moc"
