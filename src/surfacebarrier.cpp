#include "surfacebarrier.h"
#include <QGuiApplication>
#include <QtGui/qguiapplication_platform.h>
#include <wayland-client.h>
#include <cerrno>

namespace snip {

SurfaceBarrier::SurfaceBarrier(QObject *parent) : QObject(parent) {
  m_timeout.setSingleShot(true);
  connect(&m_timeout, &QTimer::timeout, this, [this] {
    cancel();
    emit failed("The compositor did not acknowledge hiding the capture UI.");
  });
}

SurfaceBarrier::~SurfaceBarrier() { cancel(); }

void SurfaceBarrier::start() {
  cancel();
  auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
  auto *native = app ? app->nativeInterface<QNativeInterface::QWaylandApplication>() : nullptr;
  auto *display = native ? native->display() : nullptr;
  if (!display) {
    emit failed("Live capture requires Qt's native Wayland connection.");
    return;
  }
  m_callback = wl_display_sync(display);
  static const wl_callback_listener listener = {
      [](void *data, wl_callback *, uint32_t) {
        auto *self = static_cast<SurfaceBarrier *>(data);
        self->cancel();
        emit self->synchronized();
      }};
  wl_callback_add_listener(m_callback, &listener, this);
  if (wl_display_flush(display) < 0 && errno != EAGAIN) {
    cancel();
    emit failed("Could not synchronize the capture UI with the compositor.");
    return;
  }
  m_timeout.start(3000);
}

void SurfaceBarrier::cancel() {
  m_timeout.stop();
  if (m_callback) wl_callback_destroy(m_callback);
  m_callback = nullptr;
}

}
