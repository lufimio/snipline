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
  m_poll.setInterval(5);
  connect(&m_poll, &QTimer::timeout, this, [this] {
    for (int i = 0; i < 32 && m_loop && !m_failed && !m_closing; ++i) {
      const auto result = pa_mainloop_iterate(m_loop, 0, nullptr);
      if (result < 0) { error("Application audio connection failed."); break; }
      if (result == 0) break;
    }
  });
  m_timeout.setSingleShot(true);
  connect(&m_timeout, &QTimer::timeout, this, [this] { error("Application audio connection timed out."); });
}
PulseTap::~PulseTap() {
  cancel();
  gst_object_unref(m_source);
  gst_object_unref(m_pipeline);
}
void PulseTap::start() {
  m_loop = pa_mainloop_new();
  if (!m_loop) { error("Cannot allocate application audio connection."); return; }
  m_context = pa_context_new(pa_mainloop_get_api(m_loop), "Snipline isolated application audio");
  if (!m_context) { error("Cannot create application audio context."); return; }
  pa_context_set_state_callback(m_context, contextState, this);
  if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
    error("Cannot connect to PulseAudio/PipeWire-Pulse for application audio.");
    return;
  }
  m_timeout.start(10000);
  m_poll.start();
}
void PulseTap::contextState(pa_context *context, void *data) {
  auto *self = static_cast<PulseTap *>(data);
  if (self->m_closing || self->m_failed) return;
  switch (pa_context_get_state(context)) {
  case PA_CONTEXT_READY:
    pa_context_set_subscribe_callback(context, [](pa_context *, pa_subscription_event_type_t type,
                                                 uint32_t index, void *data) {
      auto *self = static_cast<PulseTap *>(data);
      if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK_INPUT &&
          (type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE &&
          index == self->m_selection.value("index").toUInt())
        self->error("The selected application's audio stream closed. No desktop audio was substituted.");
    }, self);
    unref(pa_context_subscribe(context, PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr));
    unref(pa_context_get_sink_input_info(context, self->m_selection.value("index").toUInt(), inputInfo, self));
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
    if (!self->m_identityVerified) self->error("Selected application audio is unavailable.");
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
  self->m_timeout.start(10000);
  auto *operation = pa_context_get_sink_info_by_index(context, info->sink, sinkInfo, self);
  if (!operation) self->error("Cannot resolve the selected application's monitor.");
  else pa_operation_unref(operation);
}
void PulseTap::sinkInfo(pa_context *, const pa_sink_info *info, int end, void *data) {
  auto *self = static_cast<PulseTap *>(data);
  if (self->m_closing || self->m_failed) return;
  if (info && !self->m_stream) self->connectStream(info->monitor_source_name);
  else if (end && !self->m_stream) self->error("Selected application's output monitor is unavailable.");
}
void PulseTap::connectStream(const char *monitor) {
  if (!monitor || !*monitor) { error("No output monitor for the selected application."); return; }
  const pa_sample_spec spec{PA_SAMPLE_S16LE, 48000, 2};
  m_stream = pa_stream_new(m_context, "Snipline isolated stream", &spec, nullptr);
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
  attributes.maxlength = 19200;
  attributes.fragsize = 3840;
  if (pa_stream_connect_record(m_stream, monitor, &attributes,
      pa_stream_flags_t(PA_STREAM_ADJUST_LATENCY | PA_STREAM_START_CORKED | PA_STREAM_DONT_MOVE)) < 0)
    error("Cannot capture this application's audio in isolation.");
}
void PulseTap::streamState(pa_stream *stream, void *data) {
  auto *self = static_cast<PulseTap *>(data);
  if (self->m_closing || self->m_failed) return;
  const auto state = pa_stream_get_state(stream);
  if (state == PA_STREAM_READY) {
    self->m_timeout.stop();
    self->setPaused(self->m_paused);
  } else if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED)
    self->error("Isolated application audio failed. No fallback source was used.");
}
void PulseTap::setPaused(bool paused) {
  if (m_paused != paused) m_nextPts = GST_CLOCK_TIME_NONE;
  m_paused = paused;
  if (!m_stream || m_closing || pa_stream_get_state(m_stream) != PA_STREAM_READY) return;
  unref(pa_stream_cork(m_stream, paused, nullptr, nullptr));
  if (paused) unref(pa_stream_flush(m_stream, nullptr, nullptr));
}
void PulseTap::read() {
  if (!m_stream || m_failed || m_closing) return;
  const void *bytes = nullptr;
  size_t size = 0;
  if (pa_stream_peek(m_stream, &bytes, &size) < 0) { error("Cannot read selected application's audio."); return; }
  if (!size) return;
  if (size > 192000 || size % 4) {
    pa_stream_drop(m_stream);
    error("Application audio exceeded its bounded PCM buffer.");
    return;
  }
  if (m_paused) { pa_stream_drop(m_stream); return; }
  auto *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
  GstMapInfo map;
  if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    if (buffer) gst_buffer_unref(buffer);
    pa_stream_drop(m_stream);
    error("Cannot allocate application audio buffer.");
    return;
  }
  if (bytes) std::memcpy(map.data, bytes, size);
  else std::memset(map.data, 0, size); // A PulseAudio hole represents silence.
  gst_buffer_unmap(buffer, &map);
  pa_stream_drop(m_stream);
  auto *clock = gst_element_get_clock(m_pipeline);
  const auto base = gst_element_get_base_time(m_pipeline);
  const auto now = clock ? gst_clock_get_time(clock) : base;
  if (clock) gst_object_unref(clock);
  const auto duration = gst_util_uint64_scale(size, GST_SECOND, 48000 * 4);
  const auto running = now >= base ? now - base : 0;
  if (m_nextPts == GST_CLOCK_TIME_NONE || running > m_nextPts + 200 * GST_MSECOND)
    m_nextPts = running > duration ? running - duration : 0;
  GST_BUFFER_PTS(buffer) = m_nextPts;
  GST_BUFFER_DURATION(buffer) = duration;
  m_nextPts += duration;
  const auto flow = gst_app_src_push_buffer(GST_APP_SRC(m_source), buffer);
  if (flow != GST_FLOW_OK && flow != GST_FLOW_FLUSHING)
    error("Encoder rejected isolated application audio.");
}
bool PulseTap::end() {
  cancel(); // Close the server connection immediately, including while muxing EOS.
  if (!m_eos) m_eos = gst_app_src_end_of_stream(GST_APP_SRC(m_source)) == GST_FLOW_OK;
  return m_eos;
}
void PulseTap::error(const QString &message) {
  if (m_failed || m_closing) return;
  m_failed = true;
  m_poll.stop();
  m_timeout.stop();
  // Never destroy a mainloop while pa_mainloop_iterate is on its stack.
  QTimer::singleShot(0, this, [this, message] { emit failed(message); });
}
void PulseTap::cancel() {
  m_closing = true;
  m_poll.stop();
  m_timeout.stop();
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
  if (m_loop) { pa_mainloop_free(m_loop); m_loop = nullptr; }
}
}
