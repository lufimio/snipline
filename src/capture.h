#pragma once

#include "core/outputs.h"
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <functional>

namespace snip {

// Backends acquire pixels; they never manage the selection UI.
class CaptureBackend : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  virtual void start() = 0;
  virtual void startLive() = 0;
  virtual void captureLive(const QVector<Output> &expected) = 0;
  virtual bool requiresWayland() const { return true; }
  virtual void verifyLayout(const QVector<Output> &expected) = 0;
  virtual void cancel() = 0;
signals:
  void ready(QVector<snip::Frame> frames);
  void layoutVerified();
  void failed(QString message);
};

// Initial native adapter: delegate screencopy to grim using argument lists.
class GrimCapture final : public CaptureBackend {
  Q_OBJECT
public:
  explicit GrimCapture(QObject *parent = nullptr);
  void start() override;
  void startLive() override;
  void captureLive(const QVector<Output> &expected) override;
  void verifyLayout(const QVector<Output> &expected) override;
  void cancel() override;

private:
  using Completion = std::function<void(QByteArray, QString)>;
  void run(const QString &program, const QStringList &arguments,
           Completion complete, qsizetype limit = 1024 * 1024);
  void discover(std::function<void(QVector<Output>)> complete);
  void nextFrame();
  Compositor m_compositor;
  QPointer<QProcess> m_process;
  QVector<Output> m_outputs;
  QVector<Frame> m_frames;
};

} // namespace snip
