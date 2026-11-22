#include "pulsetap.h"
#include "audiosources.h"
#include <gst/app/gstappsrc.h>
#include <cstring>

namespace snip {
namespace {
void unref(pa_operation *operation) { if (operation) pa_operation_unref(operation); }
}
PulseTap::PulseTap(GstElement *pipeline, GstElement *source, QVariantMap selection, QObject *parent)
    : QObject(parent), m_pipeline(GST_ELEMENT(gst_object_ref(pipeline))),
       m_source(GST_ELEMENT(gst_object_ref(source))), m_selection(std::move(selection)) {
  m_timeout.setSingleShot(true);
  connect(&m_timeout, &QTimer::timeout, this, [this] {
    if (!m_ready && !m_applicationCorked) error("Application audio connection timed out.");
  });
  m_revalidate.setInterval(500);
  connect(&m_revalidate, &QTimer::timeout, this, [this] {
    if (!m_loop || !m_loopStarted || m_closing) return;
    pa_threaded_mainloop_lock(m_loop);
    queryInput();
    pa_threaded_mainloop_unlock(m_loop);
  });
}
PulseTap::~PulseTap() {
  cancel();
  gst_object_unref(m_source);
  gst_object_unref(m_pipeline);
}
void PulseTap::start() {
  m_loop = pa_threaded_mainloop_new();
  if (!m_loop) { error("Cannot allocate application audio connection."); return; }
  m_context = pa_context_new(pa_threaded_mainloop_get_api(m_loop), "Snipline isolated application audio");
  if (!m_context) { error("Cannot create application audio context."); return; }
  pa_context_set_state_callback(m_context, contextState, this);
  if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
    error("Cannot connect to PulseAudio/PipeWire-Pulse for application audio.");
    return;
  }
  m_timeout.start(10000);
  if (pa_threaded_mainloop_start(m_loop) < 0) {
    error("Cannot start the dedicated application-audio thread.");
    return;
  }
  m_loopStarted = true;
  m_revalidate.start();
}
void PulseTap::queryInput() {
  if (!m_context || m_closing || m_failed || m_infoPending ||
      pa_context_get_state(m_context) != PA_CONTEXT_READY) return;
  m_infoPending = true;
  auto *operation = pa_context_get_sink_input_info(m_context, m_selection.value("index").toUInt(), inputInfo, this);
  if (!operation) {
    m_infoPending = false;
    error("Cannot verify the selected application's audio stream.");
  }
  unref(operation);
}
void PulseTap::contextState(pa_context *context, void *data) {
  auto *self = static_cast<PulseTap *>(data);
  if (self->m_closing || self->m_failed) return;
  switch (pa_context_get_state(context)) {
  case PA_CONTEXT_READY:
    pa_context_set_subscribe_callback(context, [](pa_context *, pa_subscription_event_type_t type,
                                                 uint32_t index, void *data) {
      auto *self = static_cast<PulseTap *>(data);
      if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) != PA_SUBSCRIPTION_EVENT_SINK_INPUT ||
          index != self->m_selection.value("index").toUInt()) return;
      const auto event = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
      if (event == PA_SUBSCRIPTION_EVENT_REMOVE)
        self->error("The selected application's audio stream closed. No desktop audio was substituted.");
      else if (event == PA_SUBSCRIPTION_EVENT_CHANGE) {
        self->queryInput();
      }
    }, self);
    unref(pa_context_subscribe(context, PA_SUBSCRIPTION_MASK_SINK_INPUT,
        [](pa_context *, int success, void *data) {
          if (!success) static_cast<PulseTap *>(data)->error("Cannot watch application audio source changes.");
        }, self));
    self->queryInput();
    break;
  case PA_CONTEXT_FAILED: case PA_CONTEXT_TERMINATED:
    self->error("Application audio server disconnected. No fallback source was used.");
    break;
  default: break;
  }
}
void PulseTap::inputInfo(pa_context *context, const pa_sink_input_info *info, int end, void *data) {
  auto *self = static_cast<PulseTap *>(data);
  if (self->m_closing || self->m_failed) return;
  if (end) {
    self->m_infoPending = false;
    if (end < 0 || !self->m_identityVerified) self->error("Selected application audio is unavailable.");
    return;
  }
  if (!info) return;
  QVariantMap properties;
  for (const auto *key : {"application.process.id", "application.process.binary", "application.id", "application.name"}) {
    const auto *value = pa_proplist_gets(info->proplist, key);
    properties.insert(key, value ? QString::fromUtf8(value) : QString{});
  }
  if (AudioSources::streamIdentity(properties, info->client) != self->m_selection.value("identity").toString()) {
    self->error("Application audio identity changed. Reselect the source; no other audio was substituted.");
    return;
  }
  // Mark the identity lookup as complete before its end-of-list callback.
  self->m_identityVerified = true;
  if (info->sample_spec.rate < 8000 || info->sample_spec.rate > 192000) {
    self->error("Application audio has an unsupported sample rate.");
    return;
  }
  if (self->m_applicationRate != info->sample_spec.rate) {
    self->m_applicationRate = info->sample_spec.rate;
    self->m_resetTimeline = true;
  }
  if (self->m_applicationCorked != bool(info->corked)) {
    self->m_applicationCorked = info->corked;
    self->m_resetTimeline = true;
    if (!info->corked && !self->m_ready)
      QMetaObject::invokeMethod(self, [self] {
        if (!self->m_closing && !self->m_ready) self->m_timeout.start(10000);
      }, Qt::QueuedConnection);
  }
  if (self->m_sinkIndex != PA_INVALID_INDEX) {
    if (self->m_sinkIndex != info->sink)
      self->error("The selected application's audio moved to another output. Recording was stopped without substituting audio.");
    return;
  }
  self->m_sinkIndex = info->sink;
  auto *operation = pa_context_get_sink_info_by_index(context, info->sink, sinkInfo, self);
  if (!operation) self->error("Cannot resolve the selected application's monitor.");
  else pa_operation_unref(operation);
}
void PulseTap::sinkInfo(pa_context *, const pa_sink_info *info, int end, void *data) {
  auto *self = static_cast<PulseTap *>(data);
  if (self->m_closing || self->m_failed) return;
  if (info && !self->m_stream) {
    if (info->sample_spec.rate < 8000 || info->sample_spec.rate > 192000) {
      self->error("Application output monitor has an unsupported sample rate.");
      return;
    }
    self->m_sampleRate = info->sample_spec.rate;
    self->connectStream(info->monitor_source_name);
  }
  else if (end && !self->m_stream) self->error("Selected application's output monitor is unavailable.");
}
void PulseTap::connectStream(const char *monitor) {
  if (!monitor || !*monitor) { error("No output monitor for the selected application."); return; }
  // Follow the output graph's native rate; GStreamer handles any conversion
  // to the fixed encoder rate after acquisition.
  const pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, m_sampleRate, 2};
  auto *caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE",
      "rate", G_TYPE_INT, int(m_sampleRate), "channels", G_TYPE_INT, 2,
      "layout", G_TYPE_STRING, "interleaved", nullptr);
  gst_app_src_set_caps(GST_APP_SRC(m_source), caps);
  gst_caps_unref(caps);
  g_object_set(m_source, "max-bytes", guint64(m_sampleRate) * 16, nullptr);
  auto *properties = pa_proplist_new();
  // On PipeWire follow the application's existing playback driver instead of
  // making the monitor another active driver of that playback graph.
  pa_proplist_sets(properties, "node.passive", "true");
  m_stream = pa_stream_new_with_proplist(m_context, "Snipline isolated stream", &spec, nullptr, properties);
  pa_proplist_free(properties);
  if (!m_stream) { error("Cannot allocate isolated application audio."); return; }
  pa_stream_set_state_callback(m_stream, streamState, this);
  pa_stream_set_read_callback(m_stream, [](pa_stream *, size_t, void *data) {
    static_cast<PulseTap *>(data)->read();
  }, this);
  if (pa_stream_set_monitor_stream(m_stream, m_selection.value("index").toUInt()) < 0) {
    error("Audio server cannot isolate this application. No full-output fallback was used.");
    return;
  }
  pa_buffer_attr attributes;
  std::memset(&attributes, 0xff, sizeof(attributes));
  attributes.maxlength = m_sampleRate * 8;
  attributes.fragsize = (m_sampleRate / 50) * 8;
  if (pa_stream_connect_record(m_stream, monitor, &attributes,
      pa_stream_flags_t(PA_STREAM_ADJUST_LATENCY | PA_STREAM_START_CORKED | PA_STREAM_DONT_MOVE)) < 0)
    error("Cannot capture this application's audio in isolation.");
  else if (m_applicationCorked) m_prepared = true;
}
void PulseTap::streamState(pa_stream *stream, void *data) {
  auto *self = static_cast<PulseTap *>(data);
  if (self->m_closing || self->m_failed) return;
  const auto state = pa_stream_get_state(stream);
  if (state == PA_STREAM_READY) {
    const auto *spec = pa_stream_get_sample_spec(stream);
    if (!spec || spec->format != PA_SAMPLE_FLOAT32LE || spec->rate != self->m_sampleRate || spec->channels != 2) {
      self->error("Application audio negotiated an unexpected PCM format.");
      return;
    }
    self->m_ready = true;
    self->m_prepared = true;
    QMetaObject::invokeMethod(self, [self] { self->m_timeout.stop(); }, Qt::QueuedConnection);
    unref(pa_stream_cork(stream, self->m_paused.load(), nullptr, nullptr));
  } else if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED)
    self->error("Isolated application audio failed. No fallback source was used.");
}
void PulseTap::setPaused(bool paused) {
  if (m_paused.exchange(paused) != paused) m_resetTimeline = true;
  if (!m_loop || !m_loopStarted || m_closing) return;
  pa_threaded_mainloop_lock(m_loop);
  if (m_stream && pa_stream_get_state(m_stream) == PA_STREAM_READY) {
    unref(pa_stream_cork(m_stream, paused, nullptr, nullptr));
    if (paused) unref(pa_stream_flush(m_stream, nullptr, nullptr));
  }
  pa_threaded_mainloop_unlock(m_loop);
}
void PulseTap::read() {
  while (m_stream && !m_failed && !m_closing) {
    const auto available = pa_stream_readable_size(m_stream);
    if (available == size_t(-1)) { error("Cannot query application audio buffer."); return; }
    if (!available) return;
    const void *bytes = nullptr;
    size_t size = 0;
    if (pa_stream_peek(m_stream, &bytes, &size) < 0) { error("Cannot read selected application's audio."); return; }
    if (!size) return;
    if (size > m_sampleRate * 8 || size % 8) {
      pa_stream_drop(m_stream);
      error("Application audio exceeded its bounded PCM buffer.");
      return;
    }
    if (m_paused) { pa_stream_drop(m_stream); continue; }
    const auto delivered = gst_util_get_timestamp();
    if (m_lastDelivery != GST_CLOCK_TIME_NONE && delivered > m_lastDelivery + 200 * GST_MSECOND)
      m_resetTimeline = true;
    m_lastDelivery = delivered;
    if (gst_app_src_get_current_level_bytes(GST_APP_SRC(m_source)) + size > m_sampleRate * 16) {
      pa_stream_drop(m_stream);
      error("Audio encoder backlog exceeded two seconds; refusing to drop/corrupt PCM.");
      return;
    }
    auto *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    GstMapInfo map;
    if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
      if (buffer) gst_buffer_unref(buffer);
      pa_stream_drop(m_stream);
      error("Cannot allocate application audio buffer.");
      return;
    }
    if (bytes) std::memcpy(map.data, bytes, size);
    else {
      std::memset(map.data, 0, size); // A PulseAudio hole represents silence.
      m_holeBytes.fetch_add(size);
    }
    gst_buffer_unmap(buffer, &map);
    pa_stream_drop(m_stream);
    if (m_resetTimeline.exchange(false)) {
      GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
      auto *clock = gst_element_get_clock(m_pipeline);
      const auto base = gst_element_get_base_time(m_pipeline);
      const auto now = clock ? gst_clock_get_time(clock) : base;
      if (clock) gst_object_unref(clock);
      const auto running = now >= base ? now - base : 0;
      pa_usec_t latency = 0;
      int negative = 0;
      const auto offset = pa_stream_get_latency(m_stream, &latency, &negative) == 0 && !negative
          ? qMin<guint64>(latency * 1000, GST_SECOND)
          : gst_util_uint64_scale(size, GST_SECOND, m_sampleRate * 8);
      m_anchor = running > offset ? running - offset : 0;
      m_samples = 0;
    }
    // Timestamp sample positions, never delivery time. UI/capture scheduling
    // jitter must not insert gaps, overlaps or resampling discontinuities.
    GST_BUFFER_PTS(buffer) = m_anchor + gst_util_uint64_scale(m_samples, GST_SECOND, m_sampleRate);
    const auto frames = size / 8;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(m_samples + frames, GST_SECOND, m_sampleRate)
                                 - gst_util_uint64_scale(m_samples, GST_SECOND, m_sampleRate);
    m_samples += frames;
    const auto flow = gst_app_src_push_buffer(GST_APP_SRC(m_source), buffer);
    if (flow == GST_FLOW_OK) m_pcmBytes.fetch_add(size);
    if (flow != GST_FLOW_OK && flow != GST_FLOW_FLUSHING)
      error("Encoder rejected isolated application audio.");
  }
}
bool PulseTap::end() {
  cancel(); // Close the server connection immediately, including while muxing EOS.
  if (!m_eos) m_eos = gst_app_src_end_of_stream(GST_APP_SRC(m_source)) == GST_FLOW_OK;
  return m_eos;
}
void PulseTap::error(const QString &message) {
  if (m_closing || m_failed.exchange(true)) return;
  QMetaObject::invokeMethod(this, [this, message] {
    if (m_closing) return;
    m_timeout.stop();
    emit failed(message);
  }, Qt::QueuedConnection);
}
void PulseTap::cancel() {
  m_closing = true;
  m_timeout.stop();
  m_revalidate.stop();
  if (m_loopStarted) pa_threaded_mainloop_lock(m_loop);
  if (m_stream) {
    pa_stream_set_state_callback(m_stream, nullptr, nullptr);
    pa_stream_set_read_callback(m_stream, nullptr, nullptr);
    pa_stream_disconnect(m_stream);
    pa_stream_unref(m_stream);
    m_stream = nullptr;
  }
  if (m_context) {
    pa_context_set_state_callback(m_context, nullptr, nullptr);
    pa_context_set_subscribe_callback(m_context, nullptr, nullptr);
    pa_context_disconnect(m_context);
    pa_context_unref(m_context);
    m_context = nullptr;
  }
  if (m_loopStarted) {
    pa_threaded_mainloop_unlock(m_loop);
    pa_threaded_mainloop_stop(m_loop);
    m_loopStarted = false;
  }
  if (m_loop) { pa_threaded_mainloop_free(m_loop); m_loop = nullptr; }
}
QVariantMap PulseTap::diagnostics() const {
  const bool prepared = m_prepared.load();
  return {{"id", m_selection.value("id")}, {"ready", prepared}, {"streamReady", m_ready.load()},
          {"corked", m_applicationCorked.load()},
          {"sampleRate", prepared ? int(m_sampleRate) : 0},
          {"pcmBytes", QVariant::fromValue(m_pcmBytes.load())},
          {"holeBytes", QVariant::fromValue(m_holeBytes.load())},
          {"failed", m_failed.load()}, {"paused", m_paused.load()}};
}
}
