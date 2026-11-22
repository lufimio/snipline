#include "recorder.h"
#include "core/exporter.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QScopeGuard>
#include <sys/resource.h>
#include <csignal>
#include <cmath>
#include <cstring>
#include <numbers>
#include <QtTest>
using namespace snip;
class RecordingTest : public QObject {
  Q_OBJECT
private slots:
  void realEncoding_data() {
    QTest::addColumn<QString>("format");
    QTest::addColumn<QString>("codec");
    QTest::addColumn<QString>("encoder");
    QTest::addColumn<QString>("expectedCodec");
    QTest::newRow("mp4") << QString("mp4") << QString("auto") << QString("software") << QString("h264");
    QTest::newRow("webm") << QString("webm") << QString("auto") << QString("software") << QString("vp8");
    QTest::newRow("gif") << QString("gif") << QString("auto") << QString("software") << QString("gif");
    QTest::newRow("hevc") << QString("mp4") << QString("h265") << QString("software") << QString("hevc");
    QTest::newRow("vp9") << QString("webm") << QString("vp9") << QString("software") << QString("vp9");
    QTest::newRow("av1-webm") << QString("webm") << QString("av1") << QString("software") << QString("av1");
    QTest::newRow("av1-mp4") << QString("mp4") << QString("av1") << QString("software") << QString("av1");
    QTest::newRow("automatic-h264") << QString("mp4") << QString("h264") << QString("auto") << QString("h264");
    QTest::newRow("automatic-tiny-hevc-fallback") << QString("mp4") << QString("h265") << QString("auto") << QString("hevc");
  }
  void realEncoding() {
    QFETCH(QString, format);
    QFETCH(QString, codec);
    QFETCH(QString, encoder);
    QFETCH(QString, expectedCodec);
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.filenameTemplate = "Synthetic";
    options.format = format;
    options.codec = codec;
    options.encoder = encoder;
    options.fps = 10;
    QFile existing(directory.filePath("Synthetic." + format));
    QVERIFY(existing.open(QIODevice::WriteOnly));
    existing.write("existing");
    existing.close();
    const auto check = VideoRecorder::check(options);
    if (!check.isEmpty()) QSKIP(qPrintable(check));
    VideoRecorder recorder;
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy saved(&recorder, &VideoRecorder::saved);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::red);
    recorder.start(7, options, frame);
    QTRY_VERIFY_WITH_TIMEOUT(started.count() || failed.count(), 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first()[1].toString()));
    QVERIFY(!recorder.selectedEncoder().isEmpty());
    if (codec == "h265" && encoder == "auto")
      QCOMPARE(recorder.selectedEncoder(), QString("x265enc"));
    QTest::qWait(250);
    QImage resized(32, 64, QImage::Format_RGB32);
    resized.fill(Qt::blue);
    recorder.submitFrame(resized);
    QTest::qWait(300);
    recorder.stop();
    QTRY_VERIFY_WITH_TIMEOUT(saved.count() || failed.count(), 15000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first()[1].toString()));
    QCOMPARE(saved.count(), 1);
    const auto path = saved.first()[1].toString();
    QCOMPARE(path, directory.filePath("Synthetic_1." + format));
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("existing"));
    QVERIFY(QFile(path).size() > 0);
    QVERIFY(!(QFile::permissions(path) & QFileDevice::ReadOther));
    QProcess probe;
    probe.start("ffprobe", {"-v", "error", "-show_entries", "stream=width,height,codec_type,codec_name",
                            "-of", "json", path});
    QVERIFY(probe.waitForFinished(5000));
    QCOMPARE(probe.exitCode(), 0);
    const auto streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object()["streams"].toArray();
    QCOMPARE(streams.size(), 1);
    QCOMPARE(streams[0].toObject()["width"].toInt(), 64);
    QCOMPARE(streams[0].toObject()["height"].toInt(), 48);
    QCOMPARE(streams[0].toObject()["codec_name"].toString(), expectedCodec);
    QProcess decode;
    decode.start("ffmpeg", {"-nostdin", "-v", "error", "-i", path,
                            "-f", "rawvideo", "-pix_fmt", "rgb24", "-"});
    QVERIFY(decode.waitForFinished(5000));
    QCOMPARE(decode.exitCode(), 0);
    const auto decoded = decode.readAllStandardOutput();
    constexpr int frameBytes = 64 * 48 * 3;
    QVERIFY(decoded.size() >= frameBytes * 2);
    const auto tail = decoded.right(frameBytes);
    const QImage last(reinterpret_cast<const uchar *>(tail.constData()), 64, 48, 64 * 3, QImage::Format_RGB888);
    QVERIFY2(last.pixelColor(32, 24).blue() > 150,
             qPrintable(QString("last=%1 bytes=%2 path=%3").arg(last.pixelColor(32, 24).name()).arg(decoded.size()).arg(path)));
    QVERIFY(last.pixelColor(2, 2).blue() < 40); // Resize is letterboxed, not stretched.
    QVERIFY(last.pixelColor(2, 2).red() < 40);
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(), 2);
  }
  void invalidCodecContainerCombinationsFailBeforeCreatingFiles() {
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.codec = "vp9";
    QVERIFY(VideoRecorder::check(options).contains("incompatible"));
    options.codec = "h264";
    options.encoder = "unknown";
    QVERIFY(VideoRecorder::check(options).contains("Encoder must"));
    VideoRecorder recorder;
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::red);
    recorder.start(1, options, frame);
    QCOMPARE(failed.count(), 1);
    QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
  }
  void encoderProbeRejectsUnsafeDimensionsAndNonEncoderArguments() {
    const auto helper = QDir(QCoreApplication::applicationDirPath()).filePath("snipline-encoder-probe");
    for (const auto &arguments : {
        QStringList{"vah265enc", "h265", "mp4", "64", "48", "30"},
        QStringList{"vah264enc", "h264", "mp4", "2", "2", "30"},
        QStringList{"vah264enc", "h264", "mp4", "9000", "64", "30"},
        QStringList{"vah264enc ! filesink", "h264", "mp4", "320", "240", "30"}}) {
      QProcess probe;
      probe.start(helper, arguments);
      QVERIFY(probe.waitForFinished(5000));
      QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
      QCOMPARE(probe.exitCode(), 1);
    }
  }
  void hardwareProbeCrashFallsBackOnlyInAutomaticMode() {
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.filenameTemplate = "ProbeCrash";
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("Software H.264 unavailable.");
    QString hardware;
    for (const auto &backend : {"vaapi", "nvenc", "qsv"}) {
      options.encoder = backend;
      if (VideoRecorder::check(options).isEmpty()) { hardware = backend; break; }
    }
    if (hardware.isEmpty()) QSKIP("No registered hardware encoder to exercise preflight.");
    QFile script(directory.filePath("probe"));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("#!/bin/sh\nkill -KILL $$\n");
    script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    VideoRecorder recorder(nullptr, {}, script.fileName());
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::blue);
    options.encoder = hardware;
    recorder.start(1, options, frame);
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(started.isEmpty());
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(), 1); // Only the fixture.
    options.encoder = "auto";
    recorder.start(2, options, frame);
    QTRY_VERIFY_WITH_TIMEOUT(started.count() || failed.count() > 1, 10000);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(started.count(), 1);
    QCOMPARE(recorder.selectedEncoder(), QString("x264enc"));
    recorder.cancel();
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(), 1);
  }
  void cancelDuringHardwareProbeDoesNotRestartOrLeaveOutput() {
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.filenameTemplate = "CancelledProbe";
    options.encoder = "auto";
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("No H.264 encoder.");
    VideoRecorder recorder;
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::red);
    recorder.start(1, options, frame);
    recorder.cancel();
    QTest::qWait(100);
    QVERIFY(failed.isEmpty());
    QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
  }
  void automaticFallsBackForUnsupportedHardwareDimensions() {
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.filenameTemplate = "Fallback";
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("Software H.264 unavailable.");
    options.encoder = "auto";
    VideoRecorder recorder;
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QImage frame(2, 2, QImage::Format_RGB32);
    frame.fill(Qt::green);
    recorder.start(1, options, frame);
    QTRY_VERIFY_WITH_TIMEOUT(started.count() || failed.count(), 15000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first()[1].toString()));
    QCOMPARE(recorder.selectedEncoder(), QString("x264enc"));
    recorder.cancel();
    QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
  }
  void explicitHardwareEncoders_data() {
    QTest::addColumn<QString>("backend");
    QTest::addColumn<QString>("codec");
    QTest::addColumn<bool>("withAudio");
    QTest::newRow("VA-API-H264") << QString("vaapi") << QString("h264") << false;
    QTest::newRow("VA-API-H265") << QString("vaapi") << QString("h265") << false;
    QTest::newRow("VA-API-H264-AAC") << QString("vaapi") << QString("h264") << true;
    QTest::newRow("VA-API-H265-AAC") << QString("vaapi") << QString("h265") << true;
    QTest::newRow("NVENC-H264") << QString("nvenc") << QString("h264") << false;
    QTest::newRow("NVENC-H265") << QString("nvenc") << QString("h265") << false;
    QTest::newRow("NVENC-AV1") << QString("nvenc") << QString("av1") << false;
    QTest::newRow("QSV-H264") << QString("qsv") << QString("h264") << false;
    QTest::newRow("QSV-H265") << QString("qsv") << QString("h265") << false;
  }
  void explicitHardwareEncoders() {
    QFETCH(QString, backend);
    QFETCH(QString, codec);
    QFETCH(bool, withAudio);
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.filenameTemplate = "Hardware";
    options.encoder = backend;
    options.codec = codec;
    options.systemAudio = withAudio;
    const auto check = VideoRecorder::check(options);
    if (!check.isEmpty()) QSKIP(qPrintable(check));
    VideoRecorder::AudioSourceFactory factory;
    if (withAudio) factory = [](const char *name) {
      auto *source = gst_element_factory_make("audiotestsrc", name);
      g_object_set(source, "is-live", TRUE, "freq", 440.0, "volume", 0.1, nullptr);
      return source;
    };
    VideoRecorder recorder(nullptr, factory);
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QSignalSpy paused(&recorder, &VideoRecorder::paused);
    QSignalSpy saved(&recorder, &VideoRecorder::saved);
    QImage frame(320, 240, QImage::Format_RGB32);
    frame.fill(Qt::green);
    recorder.start(1, options, frame);
    QTRY_VERIFY_WITH_TIMEOUT(started.count() || failed.count(), 10000);
    if (!failed.isEmpty()) {
      qInfo() << "Hardware preflight rejected:" << failed.first()[1].toString();
      QVERIFY(started.isEmpty());
      QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
      return; // Registered plugin != usable device; failure must be explicit.
    }
    QVERIFY(recorder.selectedEncoder() != "x264enc"); // Explicit hardware never falls back.
    QVERIFY(recorder.selectedEncoder() != "x265enc");
    QCOMPARE(recorder.selectedCodec(), codec);
    qInfo() << "Exercising actual hardware pipeline:" << recorder.selectedEncoder();
    recorder.setPaused(true);
    QTRY_COMPARE(paused.count(), 1);
    recorder.setPaused(false);
    QTRY_COMPARE(paused.count(), 2);
    QTest::qWait(150);
    recorder.stop();
    QTRY_VERIFY_WITH_TIMEOUT(saved.count() || failed.count(), 15000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first()[1].toString()));
    QCOMPARE(saved.count(), 1);
    QProcess decode;
    decode.start("ffmpeg", {"-nostdin", "-v", "error", "-i", saved.first()[1].toString(), "-f", "null", "-"});
    QVERIFY(decode.waitForFinished(5000));
    QCOMPARE(decode.exitCode(), 0);
  }
  void pauseResumeDiscardAndErrors() {
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.filenameTemplate = "Discard";
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("MP4 codecs unavailable.");
    VideoRecorder recorder;
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy paused(&recorder, &VideoRecorder::paused);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::red);
    recorder.start(1, options, frame);
    QTRY_COMPARE(started.count(), 1);
    recorder.setPaused(true);
    QTRY_COMPARE(paused.count(), 1);
    QCOMPARE(paused[0][1].toBool(), true);
    recorder.setPaused(false);
    QTRY_COMPARE(paused.count(), 2);
    QCOMPARE(paused[1][1].toBool(), false);
    recorder.cancel();
    QVERIFY(failed.isEmpty());
    QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    recorder.start(2, options, {});
    QCOMPARE(failed.count(), 1);
    QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    options.format = "gif";
    options.microphone = true;
    QVERIFY(!VideoRecorder::check(options).isEmpty());
  }
  void mixedAudioAndPause_data() {
    QTest::addColumn<QString>("format");
    QTest::newRow("mp4") << QString("mp4");
    QTest::newRow("webm") << QString("webm");
  }
  void mixedAudioAndPause() {
    QFETCH(QString, format);
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.filenameTemplate = "Audio";
    options.format = format;
    options.systemAudio = options.microphone = true;
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("Audio/video codecs unavailable.");
    int sources = 0;
    VideoRecorder recorder(nullptr, [&sources](const char *name) {
      ++sources;
      auto *source = gst_element_factory_make("audiotestsrc", name);
      g_object_set(source, "is-live", TRUE, "volume", 0.05, "freq", sources == 1 ? 440.0 : 880.0, nullptr);
      return source;
    });
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy paused(&recorder, &VideoRecorder::paused);
    QSignalSpy saved(&recorder, &VideoRecorder::saved);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::green);
    recorder.start(9, options, frame);
    QTRY_VERIFY_WITH_TIMEOUT(started.count() || failed.count(), 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][1].toString()));
    QCOMPARE(sources, 2);
    QTest::qWait(300);
    recorder.setPaused(true);
    QTRY_COMPARE(paused.count(), 1);
    QTest::qWait(500);
    recorder.setPaused(false);
    QTRY_COMPARE(paused.count(), 2);
    QTest::qWait(300);
    recorder.setPaused(true);
    QTRY_COMPARE(paused.count(), 3);
    recorder.stop(); // EOS must drain even when stopped from PAUSED.
    QTRY_VERIFY_WITH_TIMEOUT(saved.count() || failed.count(), 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][1].toString()));
    QProcess probe;
    probe.start("ffprobe", {"-v", "error", "-show_entries", "stream=codec_type,duration:format=duration",
                            "-of", "json", saved[0][1].toString()});
    QVERIFY(probe.waitForFinished(5000));
    QCOMPARE(probe.exitCode(), 0);
    const auto result = QJsonDocument::fromJson(probe.readAllStandardOutput()).object();
    const auto streams = result["streams"].toArray();
    QCOMPARE(streams.size(), 2);
    const double duration = result["format"].toObject()["duration"].toString().toDouble();
    QVERIFY2(duration > 0.4 && duration < 1.2, qPrintable(QString::number(duration)));
    QProcess decode;
    decode.start("ffmpeg", {"-nostdin", "-v", "error", "-i", saved[0][1].toString(),
                            "-map", "0:a:0", "-ar", "48000", "-ac", "1", "-f", "f32le", "-"});
    QVERIFY(decode.waitForFinished(5000));
    QCOMPARE(decode.exitCode(), 0);
    const auto pcm = decode.readAllStandardOutput();
    const auto samples = pcm.size() / sizeof(float);
    QVERIFY(samples > 10000);
    // Both selected synthetic sources must survive mixing and actual decoding.
    for (const double frequency : {440.0, 880.0}) {
      double real = 0, imaginary = 0;
      for (qsizetype i = 0; i < qsizetype(samples); ++i) {
        float value;
        std::memcpy(&value, pcm.constData() + i * sizeof(float), sizeof(float));
        const auto angle = 2 * std::numbers::pi * frequency * double(i) / 48000;
        real += value * std::cos(angle);
        imaginary += value * std::sin(angle);
      }
      const auto amplitude = 2 * std::hypot(real, imaginary) / double(samples);
      QVERIFY2(amplitude > 0.005, qPrintable(QString("Missing tone %1 Hz: %2").arg(frequency).arg(amplitude)));
    }
  }
  void audioFailureBeforeFirstSamplesDoesNotPublishAnEmptySuccess() {
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.filenameTemplate = "AudioStartupFailure";
    options.systemAudio = true;
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("Required codecs unavailable.");
    GstElement *audio = nullptr;
    VideoRecorder recorder(nullptr, [&audio](const char *name) {
      audio = gst_element_factory_make("appsrc", name);
      auto *caps = gst_caps_from_string("audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved");
      g_object_set(audio, "is-live", TRUE, "format", GST_FORMAT_TIME, "caps", caps, nullptr);
      gst_caps_unref(caps);
      return audio; // Deliberately never delivers a PCM buffer.
    });
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QSignalSpy saved(&recorder, &VideoRecorder::saved);
    QSignalSpy lost(&recorder, &VideoRecorder::sourceLost);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::blue);
    recorder.start(1, options, frame);
    QTRY_COMPARE(started.count(), 1);
    auto *error = g_error_new_literal(g_quark_from_static_string("snipline-test"), 1, "Audio device could not start");
    gst_element_post_message(audio, gst_message_new_error(GST_OBJECT(audio), error, nullptr));
    g_error_free(error);
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(saved.isEmpty());
    QVERIFY(lost.isEmpty());
    QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
  }
  void emptyAudioTrackIsNotPublishedAsSuccessfulMp4() {
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.filenameTemplate = "EmptyAudio";
    options.systemAudio = true;
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("Required codecs unavailable.");
    VideoRecorder recorder(nullptr, [](const char *name) {
      auto *source = gst_element_factory_make("appsrc", name);
      auto *caps = gst_caps_from_string("audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved");
      g_object_set(source, "is-live", TRUE, "format", GST_FORMAT_TIME, "caps", caps, nullptr);
      gst_caps_unref(caps);
      return source; // No buffers: AAC must not become an invalid empty track.
    });
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QSignalSpy saved(&recorder, &VideoRecorder::saved);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::blue);
    recorder.start(1, options, frame);
    QTRY_COMPARE(started.count(), 1);
    QTest::qWait(150);
    recorder.stop();
    QTRY_VERIFY_WITH_TIMEOUT(failed.count() || saved.count(), 10000);
    QCOMPARE(failed.count(), 1);
    QVERIFY(saved.isEmpty());
    QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
  }
  void audioFailureDoesNotSubstituteAnotherSource_data() {
    QTest::addColumn<bool>("pauseFirst");
    QTest::newRow("running") << false;
    QTest::newRow("paused") << true;
  }
  void audioFailureDoesNotSubstituteAnotherSource()
  {
    QFETCH(bool, pauseFirst);
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.filenameTemplate = "AudioFailure";
    options.systemAudio = true;
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("Required codecs unavailable.");
    GstElement *audio = nullptr;
    VideoRecorder recorder(nullptr, [&audio](const char *name) {
      audio = gst_element_factory_make("audiotestsrc", name);
      g_object_set(audio, "is-live", TRUE, nullptr);
      return audio;
    });
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QSignalSpy saved(&recorder, &VideoRecorder::saved);
    QSignalSpy lost(&recorder, &VideoRecorder::sourceLost);
    QSignalSpy paused(&recorder, &VideoRecorder::paused);
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::blue);
    recorder.start(1, options, frame);
    QTRY_COMPARE(started.count(), 1);
    QTest::qWait(200);
    if (pauseFirst) {
      recorder.setPaused(true);
      QTRY_COMPARE(paused.count(), 1);
    }
    auto *error = g_error_new_literal(g_quark_from_static_string("snipline-test"), 1,
                                      "Synthetic audio source removed");
    gst_element_post_message(audio, gst_message_new_error(GST_OBJECT(audio), error, nullptr));
    g_error_free(error);
    QTRY_VERIFY_WITH_TIMEOUT(saved.count() || failed.count(), 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][1].toString()));
    QCOMPARE(lost.count(), 1);
    QVERIFY(lost[0][1].toString().contains("source removed"));
    QCOMPARE(saved.count(), 1);
    QProcess decode;
    decode.start("ffmpeg", {"-nostdin", "-v", "error", "-i", saved.first()[1].toString(), "-f", "null", "-"});
    QVERIFY(decode.waitForFinished(5000));
    QCOMPARE(decode.exitCode(), 0);
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(), 1);
  }
  void writeFailureCleansUpWithoutPublishing()
  {
    QTemporaryDir directory;
    RecordingOptions options;
    options.directory = directory.path();
    options.filenameTemplate = "WriteFailure";
    if (!VideoRecorder::check(options).isEmpty()) QSKIP("MP4 codecs unavailable.");
    VideoRecorder recorder;
    QSignalSpy started(&recorder, &VideoRecorder::started);
    QSignalSpy failed(&recorder, &VideoRecorder::failed);
    QSignalSpy saved(&recorder, &VideoRecorder::saved);
    // Confined file-size limit simulates a full destination without consuming
    // disk space or changing filesystem permissions. Restore on every exit.
    struct rlimit previous{};
    QVERIFY(getrlimit(RLIMIT_FSIZE, &previous) == 0);
    struct sigaction oldAction{}, ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    QVERIFY(sigaction(SIGXFSZ, &ignore, &oldAction) == 0);
    const auto restoreSignal = qScopeGuard([&] { sigaction(SIGXFSZ, &oldAction, nullptr); });
    auto limited = previous;
    limited.rlim_cur = qMin<rlim_t>(previous.rlim_cur, 512);
    QVERIFY(setrlimit(RLIMIT_FSIZE, &limited) == 0);
    const auto restoreLimit = qScopeGuard([&] { setrlimit(RLIMIT_FSIZE, &previous); });
    QImage frame(64, 48, QImage::Format_RGB32);
    frame.fill(Qt::red);
    recorder.start(2, options, frame);
    QTRY_VERIFY(started.count() || failed.count());
    recorder.stop();
    QTRY_VERIFY_WITH_TIMEOUT(failed.count() || saved.count(), 10000);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(saved.count(), 0);
    QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
  }
};
QTEST_GUILESS_MAIN(RecordingTest)
#include "recording_test.moc"
