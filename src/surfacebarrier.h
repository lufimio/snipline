#pragma once

#include <QObject>
#include <QTimer>
struct wl_callback;

namespace snip {

// Uses Qt's OWN Wayland connection: a second connection or a sleep cannot
// establish that the compositor processed the selector's unmap requests.
class SurfaceBarrier : public QObject {
  Q_OBJECT
public:
  explicit SurfaceBarrier(QObject *parent = nullptr);
  ~SurfaceBarrier() override;
  virtual void start();
  virtual void cancel();
signals:
  void synchronized();
  void failed(QString message);
private:
  wl_callback *m_callback = nullptr;
  QTimer m_timeout;
};

}
