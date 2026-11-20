#pragma once
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <gst/gst.h>
#include <pulse/pulseaudio.h>

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
signals:
  void failed(QString error);
private:
  void error(const QString &message);
  void read();
  void connectStream(const char *monitor);
  static void contextState(pa_context *, void *);
  static void inputInfo(pa_context *, const pa_sink_input_info *, int, void *);
  static void sinkInfo(pa_context *, const pa_sink_info *, int, void *);
  static void streamState(pa_stream *, void *);
  GstElement *m_pipeline, *m_source;
  QVariantMap m_selection;
  pa_mainloop *m_loop = nullptr;
  pa_context *m_context = nullptr;
  pa_stream *m_stream = nullptr;
  QTimer m_poll, m_timeout;
  bool m_failed = false, m_closing = false, m_paused = false, m_eos = false;
  bool m_identityVerified = false;
  GstClockTime m_nextPts = GST_CLOCK_TIME_NONE;
};
}
