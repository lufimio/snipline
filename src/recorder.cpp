#include "recorder.h"
#include "core/exporter.h"
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <gst/app/gstappsrc.h>
#include <cstdio>
#include <cstring>

namespace snip {
namespace {
bool elementExists(const char *name) {
  auto *factory = gst_element_factory_find(name);
  if (!factory) return false;
  gst_object_unref(factory);
  return true;
}
void initialize() { gst_init(nullptr, nullptr); }
QString codecFor(const RecordingOptions &options) {
  return options.codec == "auto" ? (options.format == "mp4" ? "h264" : "vp8") : options.codec;
}
QString parserFor(const QString &codec) {
  if (codec == "h264") return "h264parse";
  if (codec == "h265") return "h265parse";
  if (codec == "av1") return "av1parse";
  return {};
}
QString softwareFor(const QString &codec) {
  if (codec == "h264") return "x264enc";
  if (codec == "h265") return "x265enc";
  if (codec == "vp9") return "vp9enc";
  if (codec == "av1") return "av1enc";
  return "vp8enc";
}
QStringList candidatesFor(const RecordingOptions &options) {
  const auto codec = codecFor(options);
  QStringList result;
  const auto add = [&](const QString &backend, const QString &factory) {
    if ((options.encoder == "auto" || options.encoder == backend) &&
        elementExists(factory.toUtf8().constData())) result.append(factory);
  };
  if (codec == "h264" || codec == "h265" || codec == "av1" || codec == "vp9" || codec == "vp8")
    add("vaapi", "va" + codec + "enc");
  if (codec == "h264" || codec == "h265" || codec == "av1") {
    add("nvenc", "nv" + codec + "enc");
    add("qsv", "qsv" + codec + "enc");
  }
  if (codec == "vp9") add("qsv", "qsvvp9enc");
  if ((options.encoder == "auto" || options.encoder == "software") &&
      elementExists(softwareFor(codec).toUtf8().constData())) result.append(softwareFor(codec));
  return result;
}
bool softwareEncoder(const QString &factory) {
  return QStringList{"x264enc", "x265enc", "vp8enc", "vp9enc", "av1enc"}.contains(factory);
}
QString encoderChain(const QString &factory, const QString &codec, bool mp4) {
  QString chain = factory;
  if (factory == "x264enc") chain += " tune=zerolatency speed-preset=ultrafast quantizer=20";
  if (factory == "x265enc") chain += " tune=zerolatency speed-preset=ultrafast option-string=pools=2:frame-threads=2";
  if (factory == "vp8enc" || factory == "vp9enc")
    chain += " deadline=1 cpu-used=8 target-bitrate=4000000 lag-in-frames=0 threads=4";
  if (factory == "av1enc") chain += " usage-profile=realtime cpu-used=8 threads=4";
  const auto parser = parserFor(codec);
  if (!parser.isEmpty()) chain += " ! " + parser;
  if (codec == "h265" && mp4) chain += " ! video/x-h265,stream-format=hvc1";
  return chain;
}
QVariantList audioInputs(const RecordingOptions &options) {
  if (options.explicitAudioInputs) return options.audioInputs;
  QVariantList result;
  if (options.systemAudio) result.append(QVariantMap{{"id", "system"}, {"kind", "device"}, {"device", options.systemAudioDevice}});
  if (options.microphone) result.append(QVariantMap{{"id", "mic"}, {"kind", "device"}, {"device", options.microphoneDevice}});
  return result;
}
bool hasApplicationAudio(const RecordingOptions &options) {
  for (const auto &value : audioInputs(options))
    if (value.toMap().value("kind") == "application") return true;
  return false;
}
}
VideoRecorder::VideoRecorder(QObject *parent, AudioSourceFactory audioSourceFactory, QString encoderProbeProgram)
    : QObject(parent), m_audioSourceFactory(std::move(audioSourceFactory)),
      m_encoderProbeProgram(std::move(encoderProbeProgram)) {
  initialize();
  m_busTimer.setInterval(10);
  connect(&m_busTimer, &QTimer::timeout, this, &VideoRecorder::poll);
  m_frameTimer.setTimerType(Qt::PreciseTimer);
  connect(&m_frameTimer, &QTimer::timeout, this, &VideoRecorder::pushFrame);
  m_timeout.setSingleShot(true);
  connect(&m_timeout, &QTimer::timeout, this, [this] { fail("Encoder or GIF conversion timed out."); });
  m_probeTimeout.setSingleShot(true);
  connect(&m_probeTimeout, &QTimer::timeout, this, [this] {
    m_probeError = "Hardware encoder probe timed out: " + m_selectedEncoder;
    tryEncoder();
  });
}
VideoRecorder::~VideoRecorder() { cleanup(); }
QStringList VideoRecorder::codecsForFormat(const QString &format) {
  if (format == "mp4") return {"auto", "h264", "h265", "av1"};
  if (format == "webm") return {"auto", "vp8", "vp9", "av1"};
  if (format == "gif") return {"auto", "vp8"};
  return {};
}
QString VideoRecorder::check(const RecordingOptions &options) {
  initialize();
  if (options.format != "mp4" && options.format != "webm" && options.format != "gif")
    return "Recording format must be mp4, webm or gif.";
  if (options.fps < 1 || options.fps > 120) return "Recording fps must be 1–120.";
  if (!codecsForFormat(options.format).contains(options.codec))
    return "Codec is incompatible with the selected container. Use auto, or choose a compatible codec.";
  if (!QStringList{"software", "auto", "vaapi", "nvenc", "qsv"}.contains(options.encoder))
    return "Encoder must be software, auto, vaapi, nvenc or qsv.";
  if (options.format == "gif" && options.encoder != "software" && options.encoder != "auto")
    return "GIF uses a software VP8 intermediate; choose software or auto.";
  if (!options.explicitAudioInputs && options.systemAudio && options.systemAudioDevice.isEmpty())
    return "System audio requires a monitor source; an empty device could select a microphone.";
  QString error;
  if (!validateFilenameTemplate(options.filenameTemplate, &error)) return error;
  const auto inputs = audioInputs(options);
  if (inputs.size() > 16) return "Select at most 16 audio sources.";
  if (options.format == "gif" && !inputs.isEmpty())
    return "GIF cannot contain audio. Deselect audio sources, or choose MP4/WebM.";
  for (const auto &value : inputs) {
    const auto input = value.toMap();
    if (input.value("kind") != "device" && input.value("kind") != "application")
      return "An audio source is unavailable. Refresh and reselect it.";
    if (input.value("kind") == "application" &&
        (input.value("identity").toString().isEmpty() || !input.contains("index")))
      return "Application audio requires a verified stream identity.";
  }
  QStringList needed{"appsrc", "videoconvert", "queue", "capsfilter", "filesink"};
  needed += options.format == "mp4" ? QStringList{"mp4mux"} : QStringList{"webmmux"};
  const auto parser = parserFor(codecFor(options));
  if (!parser.isEmpty()) needed += parser;
  if (!inputs.isEmpty())
    needed += QStringList{"audiomixer", "audioconvert", "audioresample",
                          options.format == "mp4" ? "avenc_aac" : "opusenc"};
  if (hasApplicationAudio(options)) needed += "audiotestsrc";
  for (const auto &value : inputs)
    if (value.toMap().value("kind") == "device") { needed += "pulsesrc"; break; }
  for (const auto &name : needed)
    if (!elementExists(name.toUtf8().constData()))
      return "Missing GStreamer element: " + name + ". Install the required GStreamer plugins.";
  auto selection = options;
  if (options.format == "gif") selection.encoder = "software";
  if (candidatesFor(selection).isEmpty())
    return "No installed " + options.encoder + " encoder for " + codecFor(options) +
           ". Install the GStreamer encoder/driver or choose software.";
  if (options.format == "gif" && QStandardPaths::findExecutable("ffmpeg").isEmpty())
    return "GIF export requires ffmpeg.";
  return {};
}
void VideoRecorder::start(quint64 generation, RecordingOptions options, const QImage &firstFrame) {
  cleanup();
  m_generation = generation;
  m_options = std::move(options);
  const auto error = check(m_options);
  if (!error.isEmpty()) { fail(error); return; }
  if (firstFrame.isNull() || firstFrame.width() > 8192 || firstFrame.height() > 8192 ||
      qint64(firstFrame.width()) * firstFrame.height() > 16'777'216) {
    fail("Recording requires a nonempty canvas no larger than 8192 pixels per side and 16 megapixels.");
    return;
  }
  // Even dimensions keep H.264/I420 encoders happy. Resizes are letterboxed.
  m_canvas = QSize((firstFrame.width() + 1) & ~1, (firstFrame.height() + 1) & ~1);
  m_latest = firstFrame;
  m_selectedCodec = codecFor(m_options);
  auto selection = m_options;
  if (selection.format == "gif") selection.encoder = "software";
  m_encoderCandidates = candidatesFor(selection);
  tryEncoder();
}
void VideoRecorder::clearEncoderProbe() {
  m_probeTimeout.stop();
  if (!m_encoderProbe) return;
  m_encoderProbe->disconnect(this);
  if (m_encoderProbe->state() != QProcess::NotRunning) {
    m_encoderProbe->kill();
    m_encoderProbe->waitForFinished(1000);
  }
  m_encoderProbe->deleteLater();
  m_encoderProbe = nullptr;
}
void VideoRecorder::tryEncoder() {
  clearEncoderProbe();
  if (m_encoderCandidates.isEmpty()) {
    fail("No usable encoder for " + m_selectedCodec + ". " + m_probeError);
    return;
  }
  m_selectedEncoder = m_encoderCandidates.takeFirst();
  if (softwareEncoder(m_selectedEncoder)) {
    buildPipeline();
    return;
  }
  // Test real frame dimensions, format conversion and encoder initialization
  // asynchronously, with synthetic pixels only. No audio/native capture is
  // opened and no output file is created until a candidate passes.
  // Driver/plugin assertions must only kill this short-lived helper, not the
  // resident process. Probe failures/crashes are eligible for auto fallback.
  m_probeDiagnostic.clear();
  auto *probe = new QProcess(this);
  m_encoderProbe = probe;
  probe->setStandardOutputFile(QProcess::nullDevice());
  connect(probe, &QProcess::readyReadStandardError, this, [this, probe] {
    if (probe == m_encoderProbe) m_probeDiagnostic = (m_probeDiagnostic + probe->readAllStandardError()).right(4096);
  });
  connect(probe, &QProcess::errorOccurred, this, [this, probe](QProcess::ProcessError error) {
    if (probe == m_encoderProbe && error == QProcess::FailedToStart) {
      m_probeError = "Cannot start snipline-encoder-probe. Install it alongside snipline.";
      tryEncoder();
    }
  });
  connect(probe, &QProcess::finished, this, [this, probe](int code, QProcess::ExitStatus status) {
    if (probe != m_encoderProbe) return;
    if (status == QProcess::NormalExit && code == 0) {
      clearEncoderProbe();
      buildPipeline();
    } else {
      m_probeDiagnostic = (m_probeDiagnostic + probe->readAllStandardError()).right(4096);
      m_probeError = "Hardware preflight failed for " + m_selectedEncoder + ": " + QString::fromUtf8(m_probeDiagnostic);
      tryEncoder();
    }
  });
  m_probeTimeout.start(5000);
  probe->start(m_encoderProbeProgram.isEmpty()
                   ? QDir(QCoreApplication::applicationDirPath()).filePath("snipline-encoder-probe") : m_encoderProbeProgram,
               {m_selectedEncoder, m_selectedCodec, m_options.format,
                QString::number(m_canvas.width()), QString::number(m_canvas.height()), QString::number(m_options.fps)});
}
void VideoRecorder::buildPipeline() {
  if (!QDir().mkpath(m_options.directory)) { fail("Cannot create recording directory."); return; }
  QString templateError;
  const auto stem = formatFilenameTemplate(m_options.filenameTemplate, QDateTime::currentDateTime(), &templateError);
  if (stem.isEmpty()) { fail(templateError); return; }
  for (int attempt = 0; attempt < 10000; ++attempt) {
    const auto path = QDir(m_options.directory).filePath(stem +
        (attempt ? QString("_%1").arg(attempt) : QString{}) + "." + m_options.format);
    QFile reservation(path);
    if (reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
      reservation.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      m_final = path;
      break;
    }
    if (!QFile::exists(path)) { fail("Cannot reserve recording filename: " + reservation.errorString()); return; }
  }
  if (m_final.isEmpty()) { fail("Cannot reserve a unique recording filename."); return; }
  QTemporaryFile temporary(QDir(m_options.directory).filePath(".snipline-record-XXXXXX"));
  if (!temporary.open()) { fail("Cannot create temporary recording: " + temporary.errorString()); return; }
  temporary.setAutoRemove(false);
  m_temporary = temporary.fileName();
  temporary.close();
  QString pipeline =
      "appsrc name=frames is-live=true format=time block=false max-buffers=2 leaky-type=downstream "
      "! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream "
      "! videoconvert ! video/x-raw,format=";
  pipeline += softwareEncoder(m_selectedEncoder) ? "I420 ! " : "NV12 ! ";
  pipeline += encoderChain(m_selectedEncoder, m_selectedCodec, m_options.format == "mp4");
  pipeline += " ! queue ! mux. ";
  pipeline += m_options.format == "mp4" ? "mp4mux" : "webmmux";
  pipeline += " name=mux ! filesink name=output sync=false async=false ";
  const auto inputs = audioInputs(m_options);
  if (!inputs.isEmpty()) {
    // Application appsrc delivers captured (already elapsed) samples. A live
    // aggregator with no scheduling allowance considers them late and cuts
    // holes into otherwise valid PCM. This allowance affects mixing latency,
    // not sample timestamps or the recorded A/V timeline.
    pipeline += inputs.size() == 1 && !hasApplicationAudio(m_options)
        ? "queue name=mix ! " : "audiomixer name=mix latency=200000000 ignore-inactive-pads=true ! ";
    pipeline += "audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 ! ";
    pipeline += m_options.format == "mp4" ? "avenc_aac ! queue ! mux. " : "opusenc ! queue ! mux. ";
    for (int i = 0; i < inputs.size(); ++i) {
      const auto id = inputs[i].toMap().value("id").toString();
      const auto name = !m_options.explicitAudioInputs && (id == "system" || id == "mic")
          ? id : QString("audio%1").arg(i);
      m_audioNames.append(name);
      pipeline += QString("queue name=%1Input max-size-time=200000000 ! audioconvert ! audioresample "
                          "! audio/x-raw,format=F32LE,rate=48000,channels=2 ! mix. ").arg(name);
    }
    if (hasApplicationAudio(m_options)) {
      // Corked/idle applications may deliver no monitor buffers. A clocked
      // silence bed keeps the AAC/Opus track valid and the muxer moving without
      // capturing another device, while the mixer preserves live app timing.
      pipeline += "audiotestsrc name=audioSilence is-live=true wave=silence "
                  "! audio/x-raw,format=F32LE,rate=48000,channels=2 "
                  "! queue name=audioSilenceInput ! mix. ";
      m_audioNames.append("audioSilence");
    }
  }
  GError *gstError = nullptr;
  m_pipeline = gst_parse_launch(pipeline.toUtf8().constData(), &gstError);
  if (gstError || !m_pipeline) {
    const auto message = gstError ? QString::fromUtf8(gstError->message) : "Cannot create encoding pipeline.";
    if (gstError) g_error_free(gstError);
    fail(message);
    return;
  }
  auto *output = gst_bin_get_by_name(GST_BIN(m_pipeline), "output");
  g_object_set(output, "location", QFile::encodeName(m_temporary).constData(), nullptr);
  gst_object_unref(output);
  auto *mux = gst_bin_get_by_name(GST_BIN(m_pipeline), "mux");
  auto *pads = gst_element_iterate_sink_pads(mux);
  GValue value = G_VALUE_INIT;
  while (gst_iterator_next(pads, &value) == GST_ITERATOR_OK) {
    auto *pad = GST_PAD(g_value_get_object(&value));
    const auto name = QByteArray(GST_PAD_NAME(pad));
    auto *counter = name.startsWith("video_") ? &m_encodedVideo
        : name.startsWith("audio_") ? &m_encodedAudio : nullptr;
    if (counter) gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad *, GstPadProbeInfo *, gpointer data) {
          static_cast<std::atomic<quint64> *>(data)->fetch_add(1);
          return GST_PAD_PROBE_OK;
        }, counter, nullptr);
    g_value_reset(&value);
  }
  if (G_VALUE_TYPE(&value)) g_value_unset(&value);
  gst_iterator_free(pads);
  gst_object_unref(mux);
  for (int i = 0; i < inputs.size(); ++i) {
    const auto selection = inputs[i].toMap();
    const auto name = m_audioNames[i].toUtf8();
    const auto queueName = name + "Input";
    auto *input = gst_bin_get_by_name(GST_BIN(m_pipeline), queueName.constData());
    if (!input) continue;
    const bool isolated = selection.value("kind") == "application" && !m_audioSourceFactory;
    auto *source = m_audioSourceFactory ? m_audioSourceFactory(name.constData())
        : gst_element_factory_make(isolated ? "appsrc" : "pulsesrc", name.constData());
    if (!source) {
      gst_object_unref(input);
      fail("Cannot create audio source.");
      return;
    }
    // GstAudioBaseSrc already timestamps sample positions. Re-stamping at
    // delivery time makes scheduling jitter audible.
    g_object_set(source, "do-timestamp", FALSE, nullptr);
    if (isolated) {
      g_object_set(source, "is-live", TRUE, "format", GST_FORMAT_TIME, "block", FALSE,
                   "max-buffers", guint64(0), "max-bytes", guint64(768000), "leaky-type", 0, nullptr);
      auto *caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE",
          "rate", G_TYPE_INT, 48000, "channels", G_TYPE_INT, 2,
          "layout", G_TYPE_STRING, "interleaved", nullptr);
      gst_app_src_set_caps(GST_APP_SRC(source), caps);
      gst_caps_unref(caps);
    } else if (!m_audioSourceFactory && !selection.value("device").toString().isEmpty())
      g_object_set(source, "device", selection.value("device").toString().toUtf8().constData(), nullptr);
    gst_bin_add(GST_BIN(m_pipeline), source);
    if (!isolated) {
      auto ready = std::make_shared<std::atomic_bool>(false);
      m_audioReady.insert(QString::fromUtf8(name), ready);
      auto *pad = gst_element_get_static_pad(source, "src");
      gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
          [](GstPad *, GstPadProbeInfo *, gpointer data) {
            (*static_cast<std::shared_ptr<std::atomic_bool> *>(data))->store(true);
            return GST_PAD_PROBE_REMOVE;
          }, new std::shared_ptr<std::atomic_bool>(ready),
          [](gpointer data) { delete static_cast<std::shared_ptr<std::atomic_bool> *>(data); });
      gst_object_unref(pad);
    }
    const bool linked = gst_element_link(source, input);
    gst_object_unref(input);
    if (!linked) { fail("Cannot connect selected audio source."); return; }
    if (isolated) {
      auto *tap = new PulseTap(m_pipeline, source, selection, this);
      connect(tap, &PulseTap::failed, this, [this, tap](const QString &message) {
        if (!tap->diagnostics().value("ready").toBool()) fail(message);
        else audioSourceLost(message);
      });
      m_audioTaps.append(tap);
    }
  }
  m_source = gst_bin_get_by_name(GST_BIN(m_pipeline), "frames");
  auto *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA",
      "width", G_TYPE_INT, m_canvas.width(), "height", G_TYPE_INT, m_canvas.height(),
      "framerate", GST_TYPE_FRACTION, m_options.fps, 1, nullptr);
  gst_app_src_set_caps(GST_APP_SRC(m_source), caps);
  gst_caps_unref(caps);
  m_bus = gst_element_get_bus(m_pipeline);
  m_busTimer.start();
  m_timeout.start(30000);
  if (!m_audioTaps.isEmpty()) {
    m_waitingAudio = true;
    for (auto *tap : m_audioTaps) {
      tap->setPaused(true);
      tap->start();
    }
  } else startPipeline();
}
void VideoRecorder::startPipeline() {
  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    fail("Cannot start encoding pipeline.");
    return;
  }
  // Live appsrc must supply data before the pipeline can finish preroll.
  m_frameTimer.start(qMax(1, 1000 / m_options.fps));
  pushFrame();
}
void VideoRecorder::submitFrame(const QImage &image) {
  if (image.isNull() || !m_pipeline || m_stopping) return;
  m_latest = image;
}
QVariantList VideoRecorder::audioDiagnostics() const {
  QVariantList result;
  for (const auto &value : audioInputs(m_options)) {
    auto detail = value.toMap();
    for (auto *tap : m_audioTaps)
      if (tap->diagnostics().value("id") == detail.value("id")) detail["capture"] = tap->diagnostics();
    result.append(detail);
  }
  return result;
}
void VideoRecorder::pushFrame() {
  if (!m_pipeline || m_latest.isNull() || m_paused || m_wantPaused || m_stopping) return;
  QImage frame(m_canvas, QImage::Format_RGBA8888);
  frame.fill(Qt::black);
  {
    QPainter painter(&frame);
    const auto size = m_latest.size().scaled(m_canvas, Qt::KeepAspectRatio);
    painter.drawImage(QRect(QPoint((m_canvas.width() - size.width()) / 2,
                                  (m_canvas.height() - size.height()) / 2), size), m_latest);
  }
  auto *buffer = gst_buffer_new_allocate(nullptr, frame.sizeInBytes(), nullptr);
  if (!buffer) { fail("Cannot allocate encoder frame."); return; }
  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    gst_buffer_unref(buffer);
    fail("Cannot map encoder frame.");
    return;
  }
  std::memcpy(map.data, frame.constBits(), frame.sizeInBytes());
  gst_buffer_unmap(buffer, &map);
  auto *clock = gst_element_get_clock(m_pipeline);
  const auto base = gst_element_get_base_time(m_pipeline);
  const auto time = clock ? gst_clock_get_time(clock) : base;
  if (clock) gst_object_unref(clock);
  GST_BUFFER_PTS(buffer) = time >= base ? time - base : 0;
  GST_BUFFER_DURATION(buffer) = GST_SECOND / m_options.fps;
  const auto flow = gst_app_src_push_buffer(GST_APP_SRC(m_source), buffer);
  if (flow != GST_FLOW_OK && flow != GST_FLOW_FLUSHING) fail("Encoder rejected a video frame.");
}
void VideoRecorder::poll() {
  if (m_waitingAudio) {
    for (auto *tap : m_audioTaps)
      if (!tap->diagnostics().value("ready").toBool()) return;
    m_waitingAudio = false;
    startPipeline();
    return;
  }
  while (m_bus) {
    auto *message = gst_bus_pop(m_bus);
    if (!message) break;
    const auto type = GST_MESSAGE_TYPE(message);
    if (type == GST_MESSAGE_ERROR) {
      GError *error = nullptr;
      gchar *debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      const auto text = error ? QString::fromUtf8(error->message) : "Unknown GStreamer error.";
      if (error) g_error_free(error);
      g_free(debug);
      bool audioSource = false;
      bool recoverableAudio = false;
      for (const auto &name : m_audioNames) {
        auto *source = gst_bin_get_by_name(GST_BIN(m_pipeline), name.toUtf8().constData());
        if (!source) continue;
        auto *origin = GST_MESSAGE_SRC(message);
        if (origin && (origin == GST_OBJECT(source) || gst_object_has_as_ancestor(origin, GST_OBJECT(source)))) {
          audioSource = true;
          const auto ready = m_audioReady.value(name);
          recoverableAudio = m_started && ready && ready->load();
          if (recoverableAudio) {
            // Shut down only the failed source. Keep its downstream queued
            // samples/caps and terminate that branch so the muxer can finish.
            gst_element_set_locked_state(source, TRUE);
            gst_element_set_state(source, GST_STATE_NULL);
            auto *queue = gst_bin_get_by_name(GST_BIN(m_pipeline), (name + "Input").toUtf8().constData());
            auto *pad = queue ? gst_element_get_static_pad(queue, "sink") : nullptr;
            if (pad) { gst_pad_send_event(pad, gst_event_new_eos()); gst_object_unref(pad); }
            if (queue) gst_object_unref(queue);
          }
        }
        gst_object_unref(source);
        if (audioSource) break;
      }
      gst_message_unref(message);
      if (audioSource && recoverableAudio) audioSourceLost(text);
      else fail(text);
      return;
    }
    if (type == GST_MESSAGE_EOS) {
      gst_message_unref(message);
      finish();
      return;
    }
    if (type == GST_MESSAGE_STATE_CHANGED && GST_MESSAGE_SRC(message) == GST_OBJECT(m_pipeline)) {
      GstState old, current, pending;
      gst_message_parse_state_changed(message, &old, &current, &pending);
      Q_UNUSED(old)
      Q_UNUSED(pending)
      if (current == GST_STATE_PLAYING && m_stopping) endSources();
      else if (current == GST_STATE_PLAYING) {
        for (auto *tap : m_audioTaps) tap->setPaused(false);
        m_timeout.stop();
        if (!m_started) { m_started = true; emit started(m_generation); }
        else if (!m_wantPaused) { m_paused = false; emit paused(m_generation, false); }
      } else if (current == GST_STATE_PAUSED && m_started && m_wantPaused && !m_stopping) {
        m_paused = true;
        emit paused(m_generation, true);
      }
    }
    gst_message_unref(message);
  }
}
void VideoRecorder::setPaused(bool pausedValue) {
  if (!m_pipeline || !m_started || m_stopping) return;
  m_wantPaused = pausedValue;
  if (pausedValue) for (auto *tap : m_audioTaps) tap->setPaused(true);
  if (!pausedValue) m_paused = false; // Allow appsrc to preroll when resuming.
  if (gst_element_set_state(m_pipeline, pausedValue ? GST_STATE_PAUSED : GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    fail("Cannot change recording pause state.");
  // A synchronous transition might not produce a later poll before resume.
}
void VideoRecorder::stop() {
  if (!m_pipeline || m_stopping) return;
  m_stopping = true;
  m_frameTimer.stop();
  m_timeout.start(60000);
  const bool wasPaused = m_paused || m_wantPaused;
  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    fail("Cannot resume encoder for finalization.");
    return;
  }
  if (!wasPaused) endSources();
}
void VideoRecorder::endSources() {
  if (m_eosSent || !m_pipeline) return;
  m_eosSent = true;
  const auto flow = gst_app_src_end_of_stream(GST_APP_SRC(m_source));
  if (flow != GST_FLOW_OK) {
    // A live appsrc can still be unlocking during a PAUSED→PLAYING transition.
    // Retry on the Qt loop instead of losing EOS and hanging the muxer.
    m_eosSent = false;
    const auto generation = m_generation;
    QTimer::singleShot(10, this, [this, generation] {
      if (generation == m_generation && m_stopping && m_pipeline) endSources();
    });
    return;
  }
  // Send EOS once per actual source. Broadcasting through the bin can race
  // appsrc's queued EOS against the muxer's pending audio/video buffers.
  for (auto *tap : m_audioTaps) {
    if (!tap->end()) {
      m_eosSent = false;
      const auto generation = m_generation;
      QTimer::singleShot(10, this, [this, generation] {
        if (generation == m_generation && m_stopping && m_pipeline) endSources();
      });
      return;
    }
  }
  for (const auto &name : m_audioNames) {
    auto *source = gst_bin_get_by_name(GST_BIN(m_pipeline), name.toUtf8().constData());
    if (!source) continue;
    if (!GST_IS_APP_SRC(source)) gst_element_send_event(source, gst_event_new_eos());
    else if (m_audioSourceFactory) gst_app_src_end_of_stream(GST_APP_SRC(source));
    gst_object_unref(source);
  }
}
void VideoRecorder::finish() {
  if (!m_stopping) { fail("Encoder stopped unexpectedly."); return; }
  if (!m_encodedVideo.load() || (!audioInputs(m_options).isEmpty() && !m_encodedAudio.load())) {
    fail("Encoder produced an empty video or audio track. No invalid recording was saved.");
    return;
  }
  gst_element_set_state(m_pipeline, GST_STATE_NULL);
  gst_object_unref(m_bus); m_bus = nullptr;
  gst_object_unref(m_source); m_source = nullptr;
  gst_object_unref(m_pipeline); m_pipeline = nullptr;
  m_latest = {};
  m_busTimer.stop();
  if (m_options.format != "gif") { publish(); return; }
  QTemporaryFile gif(QDir(m_options.directory).filePath(".snipline-gif-XXXXXX"));
  if (!gif.open()) { fail("Cannot create temporary GIF."); return; }
  gif.setAutoRemove(false);
  m_gifTemporary = gif.fileName();
  gif.close();
  QTemporaryFile palette(QDir(m_options.directory).filePath(".snipline-palette-XXXXXX"));
  if (!palette.open()) { fail("Cannot create GIF palette file."); return; }
  palette.setAutoRemove(false);
  m_paletteTemporary = palette.fileName();
  palette.close();
  m_conversion = new QProcess(this);
  m_timeout.start(60000);
  connect(m_conversion, &QProcess::readyReadStandardError, this, [this] {
    m_conversionError += m_conversion->readAllStandardError();
    m_conversionError = m_conversionError.right(4096);
  });
  connect(m_conversion, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) fail("Cannot start ffmpeg for GIF export.");
  });
  connect(m_conversion, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
    if (code != 0 || status != QProcess::NormalExit) {
      fail("GIF conversion failed: " + QString::fromUtf8(m_conversionError));
      return;
    }
    if (!m_paletteReady) {
      m_paletteReady = true;
      m_conversionError.clear();
      // Decode a second time instead of buffering the entire video in a split
      // filter while palettegen waits for EOS.
      m_conversion->start("ffmpeg", {"-nostdin", "-hide_banner", "-loglevel", "error", "-y",
          "-i", m_temporary, "-i", m_paletteTemporary,
          "-lavfi", "paletteuse=dither=sierra2_4a", "-loop", "0", "-f", "gif", m_gifTemporary});
      return;
    }
    QFile::remove(m_temporary);
    m_temporary = m_gifTemporary;
    m_gifTemporary.clear();
    publish();
  });
  m_conversion->start("ffmpeg", {"-nostdin", "-hide_banner", "-loglevel", "error", "-y",
      "-i", m_temporary, "-vf", "palettegen=stats_mode=full", "-frames:v", "1",
      "-c:v", "png", "-f", "image2", "-update", "1", m_paletteTemporary});
}
void VideoRecorder::publish() {
  if (QFile(m_temporary).size() <= 0) { fail("Encoder produced an empty recording."); return; }
  // Both files are in the same directory. Replace only our NewOnly reservation.
  if (std::rename(QFile::encodeName(m_temporary).constData(), QFile::encodeName(m_final).constData()) != 0) {
    fail("Cannot publish recording atomically.");
    return;
  }
  const auto path = m_final;
  m_final.clear();
  m_temporary.clear();
  cleanup();
  emit saved(m_generation, path);
}
void VideoRecorder::fail(const QString &message) {
  const auto generation = m_generation;
  cleanup();
  emit failed(generation, message);
}
void VideoRecorder::audioSourceLost(const QString &message) {
  if (!m_started) { fail(message); return; }
  if (m_stopping) return;
  emit sourceLost(m_generation, message);
  // The controller transitions its state machine on sourceLost; standalone
  // users of the encoder must still finalize rather than continue silently.
  if (m_pipeline && !m_stopping) stop();
}
void VideoRecorder::cancel() { cleanup(); }
void VideoRecorder::cleanup() {
  m_busTimer.stop();
  m_frameTimer.stop();
  m_timeout.stop();
  clearEncoderProbe();
  m_encoderCandidates.clear();
  m_selectedEncoder.clear();
  m_selectedCodec.clear();
  m_probeError.clear();
  m_waitingAudio = false;
  for (auto *tap : m_audioTaps) {
    tap->disconnect(this);
    tap->cancel();
    delete tap;
  }
  m_audioTaps.clear();
  m_audioNames.clear();
  m_audioReady.clear();
  if (m_conversion) {
    m_conversion->disconnect(this);
    m_conversion->kill();
    m_conversion->waitForFinished(2000);
    m_conversion->deleteLater();
    m_conversion = nullptr;
  }
  if (m_pipeline) gst_element_set_state(m_pipeline, GST_STATE_NULL);
  if (m_source) gst_object_unref(m_source);
  if (m_bus) gst_object_unref(m_bus);
  if (m_pipeline) gst_object_unref(m_pipeline);
  m_encodedVideo = 0;
  m_encodedAudio = 0;
  m_pipeline = m_source = nullptr;
  m_bus = nullptr;
  for (const auto &path : {m_temporary, m_final, m_gifTemporary, m_paletteTemporary})
    if (!path.isEmpty()) QFile::remove(path);
  m_temporary.clear(); m_final.clear(); m_gifTemporary.clear();
  m_paletteTemporary.clear();
  m_conversionError.clear();
  m_latest = {};
  m_started = m_paused = m_wantPaused = m_stopping = false;
  m_eosSent = false;
  m_paletteReady = false;
}
}
