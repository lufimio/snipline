#pragma once
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <gst/gst.h>
#include <pulse/pulseaudio.h>
#include <atomic>

namespace snip {
// Capture one sink-input, not the monitor's complete mix. No modules are
// loaded and no application streams/devices are moved or reconfigured.
class PulseTap : public QObject {
  Q_OBJECT
public:
  PulseTap(GstElement *pipeline, GstElement *source, QVariantMap selection, QObject *parent);
  ~PulseTap() override;
  void start();
  void setPaused(bool paused);
  bool end();
  void cancel();
  QVariantMap diagnostics() const;
signals:
  void failed(QString error);
private:
  void error(const QString &message);
  void read();
  void queryInput();
  void connectStream(const char *monitor);
  static void contextState(pa_context *, void *);
  static void inputInfo(pa_context *, const pa_sink_input_info *, int, void *);
  static void sinkInfo(pa_context *, const pa_sink_info *, int, void *);
  static void streamState(pa_stream *, void *);
  GstElement *m_pipeline, *m_source;
  QVariantMap m_selection;
  pa_threaded_mainloop *m_loop = nullptr;
  bool m_loopStarted = false;
  pa_context *m_context = nullptr;
  pa_stream *m_stream = nullptr;
  QTimer m_timeout, m_revalidate;
  bool m_infoPending = false;
  std::atomic_bool m_failed{false}, m_closing{false}, m_paused{false}, m_ready{false}, m_resetTimeline{true};
  bool m_eos = false;
  bool m_identityVerified = false;
  std::atomic_bool m_applicationCorked{false}, m_prepared{false};
  GstClockTime m_lastDelivery = GST_CLOCK_TIME_NONE;
  uint32_t m_sinkIndex = PA_INVALID_INDEX;
  GstClockTime m_anchor = 0;
  uint32_t m_sampleRate = 48000;
  uint32_t m_applicationRate = 0;
  quint64 m_samples = 0;
  std::atomic<quint64> m_pcmBytes{0};
  std::atomic<quint64> m_holeBytes{0};
};
}
