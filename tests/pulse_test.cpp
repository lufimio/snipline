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
  GstElement *producer(const QString &app, double frequency) {
    GError *error = nullptr;
    const auto description = QString("audiotestsrc is-live=true freq=%1 volume=0.1 "
        "! audioconvert ! audioresample ! pulsesink name=player sync=true async=false").arg(frequency);
    auto *pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
    if (error) { g_error_free(error); if (pipeline) gst_object_unref(pipeline); return nullptr; }
    auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), "player");
    const auto name = app.toUtf8();
    auto *properties = gst_structure_new("props", "application.name", G_TYPE_STRING, name.constData(),
        "application.id", G_TYPE_STRING, name.constData(), "media.name", G_TYPE_STRING, name.constData(), nullptr);
    g_object_set(sink, "device", "snipline_test", "stream-properties", properties, nullptr);
    g_object_set(sink, "server", qgetenv("PULSE_SERVER").constData(), nullptr);
    gst_structure_free(properties);
    gst_object_unref(sink);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      gst_object_unref(pipeline);
      return nullptr;
    }
    return pipeline;
  }
private slots:
  void initTestCase() {
    if (QStandardPaths::findExecutable("pipewire").isEmpty() ||
        QStandardPaths::findExecutable("pipewire-pulse").isEmpty() ||
        QStandardPaths::findExecutable("wireplumber").isEmpty() ||
        QStandardPaths::findExecutable("pactl").isEmpty() ||
        QStandardPaths::findExecutable("ffmpeg").isEmpty())
      QSKIP("Private PipeWire/PulseAudio integration test requires pipewire, pipewire-pulse, pactl and ffmpeg.");
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
context.properties = { core.daemon = true core.name = pipewire-0 }
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
    createSink.start("pactl", {"load-module", "module-null-sink", "sink_name=snipline_test"});
    QVERIFY(createSink.waitForFinished(5000));
    QVERIFY2(createSink.exitCode() == 0, createSink.readAllStandardError().constData());
    gst_init(nullptr, nullptr);
  }
  void applicationMonitorExcludesTheOtherApplication() {
    RecordingOptions options;
    options.directory = m_runtime.filePath("output");
    options.filenameTemplate = "IsolatedApp";
    options.format = "webm";
    options.explicitAudioInputs = true;
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("WebM encoding plugins unavailable.");
    auto *a = producer("Snipline-test-A", 440);
    auto *b = producer("Snipline-test-B", 880);
    QVERIFY(a && b);
    const auto stopPlayers = qScopeGuard([&] {
      gst_element_set_state(a, GST_STATE_NULL); gst_object_unref(a);
      gst_element_set_state(b, GST_STATE_NULL); gst_object_unref(b);
    });
    AudioSources catalog;
    QVariantMap chosen;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
      catalog.refresh();
      bool second = false;
      for (const auto &value : catalog.sources()) {
        if (value.toMap().value("appId") == "Snipline-test-A") chosen = value.toMap();
        if (value.toMap().value("appId") == "Snipline-test-B") second = true;
      }
      return !chosen.isEmpty() && second;
    })(), 5000);
    options.audioInputs = {chosen};
    VideoRecorder recorder;
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy paused(&recorder, &VideoRecorder::paused);
    QSignalSpy saved(&recorder, &VideoRecorder::saved);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::green);
    recorder.start(1, options, frame);
    QTRY_VERIFY_WITH_TIMEOUT(started.count() || failed.count(), 5000);
    QTest::qWait(700);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][1].toString()));
    recorder.setPaused(true);
    QTRY_COMPARE(paused.count(), 1);
    QTest::qWait(100);
    recorder.setPaused(false);
    QTRY_COMPARE(paused.count(), 2);
    QTest::qWait(400);
    recorder.stop();
    QTRY_VERIFY_WITH_TIMEOUT(saved.count() || failed.count(), 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][1].toString()));
    QCOMPARE(saved.count(), 1);
    QProcess decode;
    decode.start("ffmpeg", {"-nostdin", "-v", "error", "-i", saved[0][1].toString(),
                           "-map", "0:a:0", "-ar", "48000", "-ac", "1", "-f", "f32le", "-"});
    QVERIFY(decode.waitForFinished(5000));
    QCOMPARE(decode.exitCode(), 0);
    const auto pcm = decode.readAllStandardOutput();
    const auto samples = pcm.size() / sizeof(float);
    QVERIFY(samples > 10000);
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
    QVERIFY2(amplitude(880) < 0.005, qPrintable(QString("Unselected tone leaked: %1").arg(amplitude(880))));
  }
  void cleanupTestCase() {
    m_policy.terminate();
    if (!m_policy.waitForFinished(2000)) { m_policy.kill(); m_policy.waitForFinished(); }
    m_pulse.terminate();
    if (!m_pulse.waitForFinished(2000)) { m_pulse.kill(); m_pulse.waitForFinished(); }
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
