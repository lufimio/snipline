#include "recorder.h"
#include "core/exporter.h"
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QStandardPaths>
#include <QTemporaryFile>
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
QVariantList audioInputs(const RecordingOptions &options) {
  if (options.explicitAudioInputs) return options.audioInputs;
  QVariantList result;
  if (options.systemAudio) result.append(QVariantMap{{"id", "system"}, {"kind", "device"}, {"device", options.systemAudioDevice}});
  if (options.microphone) result.append(QVariantMap{{"id", "mic"}, {"kind", "device"}, {"device", options.microphoneDevice}});
  return result;
}
}
VideoRecorder::VideoRecorder(QObject *parent, AudioSourceFactory audioSourceFactory)
    : QObject(parent), m_audioSourceFactory(std::move(audioSourceFactory)) {
  initialize();
  m_busTimer.setInterval(10);
  connect(&m_busTimer, &QTimer::timeout, this, &VideoRecorder::poll);
  m_frameTimer.setTimerType(Qt::PreciseTimer);
  connect(&m_frameTimer, &QTimer::timeout, this, &VideoRecorder::pushFrame);
  m_timeout.setSingleShot(true);
  connect(&m_timeout, &QTimer::timeout, this, [this] { fail("Encoder or GIF conversion timed out."); });
}
VideoRecorder::~VideoRecorder() { cleanup(); }
QString VideoRecorder::check(const RecordingOptions &options) {
  initialize();
  if (options.format != "mp4" && options.format != "webm" && options.format != "gif")
    return "Recording format must be mp4, webm or gif.";
  if (options.fps < 1 || options.fps > 120) return "Recording fps must be 1–120.";
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
  needed += options.format == "mp4" ? QStringList{"x264enc", "h264parse", "mp4mux"}
                                    : QStringList{"vp8enc", "webmmux"};
  if (!inputs.isEmpty())
    needed += QStringList{"pulsesrc", "audiomixer", "audioconvert", "audioresample",
                         options.format == "mp4" ? "avenc_aac" : "opusenc"};
  for (const auto &name : needed)
    if (!elementExists(name.toUtf8().constData()))
      return "Missing GStreamer element: " + name + ". Install the required GStreamer plugins.";
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
      "! videoconvert ! video/x-raw,format=I420 ! ";
  if (m_options.format == "mp4")
    pipeline += "x264enc tune=zerolatency speed-preset=ultrafast quantizer=20 ! h264parse ! queue ! mux. "
                "mp4mux name=mux ! filesink name=output sync=false async=false ";
  else
    pipeline += "vp8enc deadline=1 cpu-used=8 target-bitrate=4000000 ! queue ! mux. "
                "webmmux name=mux ! filesink name=output sync=false async=false ";
  const auto inputs = audioInputs(m_options);
  if (!inputs.isEmpty()) {
    pipeline += "audiomixer name=mix ! audioconvert ! audioresample ! "
                "audio/x-raw,rate=48000,channels=2 ! ";
    pipeline += m_options.format == "mp4" ? "avenc_aac ! queue ! mux. " : "opusenc ! queue ! mux. ";
    for (int i = 0; i < inputs.size(); ++i) {
      const auto id = inputs[i].toMap().value("id").toString();
      const auto name = !m_options.explicitAudioInputs && (id == "system" || id == "mic")
          ? id : QString("audio%1").arg(i);
      m_audioNames.append(name);
      pipeline += QString("queue name=%1Input max-size-time=200000000 ! audioconvert ! audioresample ! mix. ").arg(name);
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
    g_object_set(source, "do-timestamp", !isolated, nullptr);
    if (isolated) {
      g_object_set(source, "is-live", TRUE, "format", GST_FORMAT_TIME, "block", FALSE,
                   "max-buffers", guint64(8), "leaky-type", 2, nullptr);
      auto *caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE",
          "rate", G_TYPE_INT, 48000, "channels", G_TYPE_INT, 2,
          "layout", G_TYPE_STRING, "interleaved", nullptr);
      gst_app_src_set_caps(GST_APP_SRC(source), caps);
      gst_caps_unref(caps);
    } else if (!m_audioSourceFactory && !selection.value("device").toString().isEmpty())
      g_object_set(source, "device", selection.value("device").toString().toUtf8().constData(), nullptr);
    gst_bin_add(GST_BIN(m_pipeline), source);
    const bool linked = gst_element_link(source, input);
    gst_object_unref(input);
    if (!linked) { fail("Cannot connect selected audio source."); return; }
    if (isolated) {
      auto *tap = new PulseTap(m_pipeline, source, selection, this);
      connect(tap, &PulseTap::failed, this, &VideoRecorder::fail);
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
  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    fail("Cannot start encoding pipeline.");
    return;
  }
  // Live appsrc must supply data before the pipeline can finish preroll.
  m_frameTimer.start(qMax(1, 1000 / m_options.fps));
  pushFrame();
  for (auto *tap : m_audioTaps) tap->start();
}
void VideoRecorder::submitFrame(const QImage &image) {
  if (image.isNull() || !m_pipeline || m_stopping) return;
  m_latest = image;
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
      gst_message_unref(message);
      fail(text);
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
    gst_object_unref(source);
  }
}
void VideoRecorder::finish() {
  if (!m_stopping) { fail("Encoder stopped unexpectedly."); return; }
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
void VideoRecorder::cancel() { cleanup(); }
void VideoRecorder::cleanup() {
  m_busTimer.stop();
  m_frameTimer.stop();
  m_timeout.stop();
  for (auto *tap : m_audioTaps) {
    tap->disconnect(this);
    tap->cancel();
    delete tap;
  }
  m_audioTaps.clear();
  m_audioNames.clear();
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
