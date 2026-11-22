#pragma once
#include <QObject>
#include <QImage>
#include <QTimer>
#include <QProcess>
#include <QHash>
#include <gst/gst.h>
#include <functional>
#include <memory>
#include <atomic>
#include "pulsetap.h"

namespace snip {
struct RecordingOptions {
  QString directory, filenameTemplate, format = "mp4";
  int fps = 30;
  bool systemAudio = false, microphone = false;
  QString systemAudioDevice = "@DEFAULT_MONITOR@", microphoneDevice;
  bool explicitAudioInputs = false;
  QVariantList audioInputs;
  QString codec = "auto", encoder = "auto";
};

// Native capture remains outside the encoder. Frames are fitted to a fixed
// canvas; only the newest frame is retained, and appsrc/queues are bounded.
class VideoRecorder : public QObject {
  Q_OBJECT
public:
  using AudioSourceFactory = std::function<GstElement *(const char *name)>;
  explicit VideoRecorder(QObject *parent = nullptr, AudioSourceFactory audioSourceFactory = {},
                         QString encoderProbeProgram = {});
  ~VideoRecorder() override;
  virtual void start(quint64 generation, RecordingOptions options, const QImage &firstFrame);
  virtual void submitFrame(const QImage &image);
  virtual void setPaused(bool paused);
  virtual void stop();
  virtual void cancel();
  static QString check(const RecordingOptions &options);
  static QStringList codecsForFormat(const QString &format);
  QString selectedEncoder() const { return m_selectedEncoder; }
  QString selectedCodec() const { return m_selectedCodec; }
  QVariantList audioDiagnostics() const;
signals:
  void started(quint64 generation);
  void paused(quint64 generation, bool paused);
  void saved(quint64 generation, QString path);
  void failed(quint64 generation, QString error);
  void sourceLost(quint64 generation, QString warning);
private:
  void tryEncoder();
  void clearEncoderProbe();
  void buildPipeline();
  void startPipeline();
  void poll();
  void pushFrame();
  void finish();
  void endSources();
  void publish();
  void fail(const QString &message);
  void audioSourceLost(const QString &message);
  void cleanup();
  GstElement *m_pipeline = nullptr, *m_source = nullptr;
  GstBus *m_bus = nullptr;
  RecordingOptions m_options;
  quint64 m_generation = 0;
  QImage m_latest;
  QSize m_canvas;
  QTimer m_busTimer, m_frameTimer, m_timeout;
  QProcess *m_conversion = nullptr;
  QString m_temporary, m_final, m_gifTemporary, m_paletteTemporary;
  bool m_started = false, m_paused = false, m_wantPaused = false, m_stopping = false;
  bool m_eosSent = false;
  bool m_paletteReady = false;
  QByteArray m_conversionError;
  AudioSourceFactory m_audioSourceFactory;
  QList<PulseTap *> m_audioTaps;
  QStringList m_audioNames;
  QHash<QString, std::shared_ptr<std::atomic_bool>> m_audioReady;
  QStringList m_encoderCandidates;
  QString m_selectedEncoder, m_selectedCodec, m_probeError;
  QProcess *m_encoderProbe = nullptr;
  QTimer m_probeTimeout;
  QByteArray m_probeDiagnostic;
  QString m_encoderProbeProgram;
  bool m_waitingAudio = false;
  std::atomic<quint64> m_encodedVideo{0}, m_encodedAudio{0};
};
}
