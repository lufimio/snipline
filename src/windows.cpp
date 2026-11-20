#include "windows.h"
#include "ext-toplevel.h"
#include "hypr-export.h"
#include "hypr-mapping.h"
#include "wlr-toplevel.h"

#include <QPointer>
#include <QProcess>
#include <QSocketNotifier>
#include <QTimer>
#include <algorithm>
#include <cerrno>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>
#include <vector>

namespace snip {

bool validWindowBuffer(quint32 format, quint32 width, quint32 height, quint32 stride) {
  return (format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888)
         && width > 0 && height > 0 && width <= 32768 && height <= 32768
         && quint64(width) * height <= 100'000'000
         && stride >= quint64(width) * 4 && stride % 4 == 0
         && quint64(stride) * height <= 400'000'000;
}

QImage copyWindowBuffer(const uchar *data, quint32 format, quint32 width,
                        quint32 height, quint32 stride, bool inverted) {
  if (!data || !validWindowBuffer(format, width, height, stride)) return {};
  const QImage view(data, int(width), int(height), int(stride),
                    format == WL_SHM_FORMAT_ARGB8888 ? QImage::Format_ARGB32_Premultiplied
                                                    : QImage::Format_RGB32);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
  return inverted ? view.flipped(Qt::Vertical) : view.copy();
#else
  return inverted ? view.mirrored(false, true) : view.copy();
#endif
}

struct WaylandWindows::Private {
  WaylandWindows *q;
  struct Window {
    Private *owner;
    QString id, identifier, title, appId, pendingTitle, pendingApp;
    bool done = false;
    zwlr_foreign_toplevel_handle_v1 *wlr = nullptr;
    ext_foreign_toplevel_handle_v1 *ext = nullptr;
    hyprland_toplevel_window_mapping_handle_v1 *mapping = nullptr;
    quint64 address = 0;
    ~Window() {
      if (mapping) hyprland_toplevel_window_mapping_handle_v1_destroy(mapping);
      if (wlr) zwlr_foreign_toplevel_handle_v1_destroy(wlr);
      if (ext) ext_foreign_toplevel_handle_v1_destroy(ext);
    }
  };
  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  wl_callback *sync = nullptr;
  wl_shm *shm = nullptr;
  zwlr_foreign_toplevel_manager_v1 *wlr = nullptr;
  ext_foreign_toplevel_list_v1 *ext = nullptr;
  hyprland_toplevel_export_manager_v1 *hypr = nullptr;
  hyprland_toplevel_mapping_manager_v1 *mappingManager = nullptr;
  hyprland_toplevel_export_frame_v1 *frame = nullptr;
  wl_buffer *buffer = nullptr;
  uchar *mapping = nullptr;
  quint32 format = 0, width = 0, height = 0, stride = 0, flags = 0;
  qsizetype mappingSize = 0;
  QSocketNotifier *notifier = nullptr;
  QTimer timer;
  QPointer<QProcess> process;
  std::vector<std::unique_ptr<Window>> entries;
  uint32_t wlrName = 0, extName = 0, hyprName = 0, shmName = 0, sourceName = 0, copyName = 0;
  bool standardSource = false, imageCopy = false, initializing = false, revoked = false;
  QString reason = "Window discovery has not started.", active;
  quint64 nextId = 0, epoch = 0;

  explicit Private(WaylandWindows *parent) : q(parent) {
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, q, [this] {
      if (initializing) {
        q->stop();
        reason = "Window discovery timed out.";
        emit q->changed();
      } else {
        error("Isolated window capture timed out; no desktop fallback was used.");
      }
    });
  }

  void flush() {
    if (display && wl_display_flush(display) < 0 && errno != EAGAIN)
      lost();
  }
  void lost() {
    if (notifier) notifier->setEnabled(false);
    reason = "The window-capture Wayland connection or permission was lost.";
    initializing = false;
    timer.stop();
    if (!active.isEmpty()) error(reason);
    emit q->changed();
  }
  void clearFrame() {
    timer.stop();
    if (frame) hyprland_toplevel_export_frame_v1_destroy(frame);
    frame = nullptr;
    if (buffer) wl_buffer_destroy(buffer);
    buffer = nullptr;
    if (mapping) munmap(mapping, size_t(mappingSize));
    mapping = nullptr;
    mappingSize = 0;
    width = height = stride = flags = 0;
    if (auto *p = process.data()) { process = nullptr; p->kill(); }
  }
  void error(QString message) {
    clearFrame();
    active.clear();
    const auto token = ++epoch;
    QMetaObject::invokeMethod(q, [this, token, message] {
      if (token == epoch) emit q->failed(message);
    }, Qt::QueuedConnection);
  }
  void deliver(QImage image) {
    const auto id = active;
    clearFrame();
    active.clear();
    const auto token = epoch;
    QMetaObject::invokeMethod(q, [this, token, id, image] {
      if (token == epoch) emit q->imageReady(id, image);
    }, Qt::QueuedConnection);
  }
  void closed(Window *window) {
    const auto id = window->id;
    if (active == id) q->cancelCapture();
    std::erase_if(entries, [window](const auto &entry) { return entry.get() == window; });
    emit q->closed(id);
    emit q->changed();
  }
  static void done(Window *window) {
    window->title = window->pendingTitle;
    window->appId = window->pendingApp;
    window->done = true;
    emit window->owner->q->changed();
  }
  void mapWindow(Window *window) {
    if (!mappingManager) return;
    window->mapping = window->wlr
        ? hyprland_toplevel_mapping_manager_v1_get_window_for_toplevel_wlr(mappingManager, window->wlr)
        : hyprland_toplevel_mapping_manager_v1_get_window_for_toplevel(mappingManager, window->ext);
    static const hyprland_toplevel_window_mapping_handle_v1_listener listener = {
        [](void *data, auto *, uint32_t high, uint32_t low) {
          auto *window = static_cast<Window *>(data);
          window->address = (quint64(high) << 32) | low;
          emit window->owner->q->changed();
        },
        [](void *data, auto *) {
          auto *window = static_cast<Window *>(data);
          window->address = 0;
          emit window->owner->q->changed();
        }};
    hyprland_toplevel_window_mapping_handle_v1_add_listener(window->mapping, &listener, window);
  }
  void addWlr(zwlr_foreign_toplevel_handle_v1 *handle) {
    auto window = std::make_unique<Window>();
    window->owner = this;
    window->id = QStringLiteral("wlr:%1").arg(++nextId);
    window->wlr = handle;
    static const zwlr_foreign_toplevel_handle_v1_listener listener = {
        [](void *d, auto *, const char *s) { static_cast<Window *>(d)->pendingTitle = QString::fromUtf8(s); },
        [](void *d, auto *, const char *s) { static_cast<Window *>(d)->pendingApp = QString::fromUtf8(s); },
        [](void *, auto *, wl_output *) {},
        [](void *, auto *, wl_output *) {},
        [](void *, auto *, wl_array *) {},
        [](void *d, auto *) { done(static_cast<Window *>(d)); },
        [](void *d, auto *) { auto *w = static_cast<Window *>(d); w->owner->closed(w); },
        [](void *, auto *, zwlr_foreign_toplevel_handle_v1 *) {}};
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &listener, window.get());
    mapWindow(window.get());
    entries.push_back(std::move(window));
  }
  void addExt(ext_foreign_toplevel_handle_v1 *handle) {
    auto window = std::make_unique<Window>();
    window->owner = this;
    window->id = QStringLiteral("ext:%1").arg(++nextId);
    window->ext = handle;
    static const ext_foreign_toplevel_handle_v1_listener listener = {
        [](void *d, auto *) { auto *w = static_cast<Window *>(d); w->owner->closed(w); },
        [](void *d, auto *) { done(static_cast<Window *>(d)); },
        [](void *d, auto *, const char *s) { static_cast<Window *>(d)->pendingTitle = QString::fromUtf8(s); },
        [](void *d, auto *, const char *s) { static_cast<Window *>(d)->pendingApp = QString::fromUtf8(s); },
        [](void *d, auto *, const char *s) { static_cast<Window *>(d)->identifier = QString::fromUtf8(s); }};
    ext_foreign_toplevel_handle_v1_add_listener(handle, &listener, window.get());
    mapWindow(window.get());
    entries.push_back(std::move(window));
  }
  void managerFinished() {
    revoked = true;
    reason = "The compositor revoked window-list access.";
    if (!active.isEmpty()) error(reason);
    emit q->changed();
  }
  void bindList() {
    if (hypr && wlrName && shm) {
      wlr = static_cast<zwlr_foreign_toplevel_manager_v1 *>(
          wl_registry_bind(registry, wlrName, &zwlr_foreign_toplevel_manager_v1_interface, 3));
      static const zwlr_foreign_toplevel_manager_v1_listener listener = {
          [](void *d, auto *, auto *handle) { static_cast<Private *>(d)->addWlr(handle); },
          [](void *d, auto *) { static_cast<Private *>(d)->managerFinished(); }};
      zwlr_foreign_toplevel_manager_v1_add_listener(wlr, &listener, this);
    } else if (extName && standardSource && imageCopy) {
      ext = static_cast<ext_foreign_toplevel_list_v1 *>(
          wl_registry_bind(registry, extName, &ext_foreign_toplevel_list_v1_interface, 1));
      static const ext_foreign_toplevel_list_v1_listener listener = {
          [](void *d, auto *, auto *handle) { static_cast<Private *>(d)->addExt(handle); },
          [](void *d, auto *) { static_cast<Private *>(d)->managerFinished(); }};
      ext_foreign_toplevel_list_v1_add_listener(ext, &listener, this);
    } else {
      initializing = false;
      timer.stop();
      reason = "Isolated windows require Hyprland toplevel-export v2 + wlr toplevels, "
               "or ext toplevel-list + toplevel capture-source + image-copy-capture.";
      emit q->changed();
      return;
    }
    sync = wl_display_sync(display);
    static const wl_callback_listener listener = {
        [](void *d, wl_callback *callback, uint32_t) {
          auto *self = static_cast<Private *>(d);
          wl_callback_destroy(callback);
          self->sync = nullptr;
          self->timer.stop();
          self->initializing = false;
          if (!self->revoked) self->reason.clear();
          emit self->q->changed();
        }};
    wl_callback_add_listener(sync, &listener, this);
    flush();
  }
  void allocate() {
    if (!validWindowBuffer(format, width, height, stride)) {
      error("The compositor offered an unsupported or oversized window buffer.");
      return;
    }
    mappingSize = qsizetype(stride) * height;
    const int fd = int(syscall(SYS_memfd_create, "snipline-window", MFD_CLOEXEC));
    if (fd < 0 || ftruncate(fd, mappingSize) < 0) {
      if (fd >= 0) close(fd);
      error("Cannot allocate private window-capture memory.");
      return;
    }
    void *memory = mmap(nullptr, size_t(mappingSize), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) { close(fd); error("Cannot map window-capture memory."); return; }
    mapping = static_cast<uchar *>(memory);
    auto *pool = wl_shm_create_pool(shm, fd, int(mappingSize));
    buffer = wl_shm_pool_create_buffer(pool, 0, int(width), int(height), int(stride), format);
    static const wl_buffer_listener bufferListener = {[](void *, wl_buffer *) {}};
    wl_buffer_add_listener(buffer, &bufferListener, this);
    wl_shm_pool_destroy(pool);
    close(fd);
    hyprland_toplevel_export_frame_v1_copy(frame, buffer, 1);
    flush();
  }
};

WaylandWindows::WaylandWindows(QObject *parent) : WindowBackend(parent), d(std::make_unique<Private>(this)) {}
WaylandWindows::~WaylandWindows() { stop(); }

void WaylandWindows::start() {
  stop();
  d->reason = "Discovering isolated-window capture support…";
  d->initializing = true;
  d->display = wl_display_connect(nullptr);
  if (!d->display) { d->lost(); return; }
  d->registry = wl_display_get_registry(d->display);
  static const wl_registry_listener registryListener = {
      [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
        auto *s = static_cast<Private *>(data);
        const QByteArray type(interface);
        if (type == "wl_shm") {
          s->shmName = name;
          s->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
          static const wl_shm_listener listener = {[](void *, wl_shm *, uint32_t) {}};
          wl_shm_add_listener(s->shm, &listener, s);
        } else if (type == "hyprland_toplevel_export_manager_v1" && version >= 2) {
          s->hyprName = name;
          s->hypr = static_cast<hyprland_toplevel_export_manager_v1 *>(
              wl_registry_bind(registry, name, &hyprland_toplevel_export_manager_v1_interface, 2));
        } else if (type == "zwlr_foreign_toplevel_manager_v1" && version >= 3) s->wlrName = name;
        else if (type == "hyprland_toplevel_mapping_manager_v1")
          s->mappingManager = static_cast<hyprland_toplevel_mapping_manager_v1 *>(
              wl_registry_bind(registry, name, &hyprland_toplevel_mapping_manager_v1_interface, 1));
        else if (type == "ext_foreign_toplevel_list_v1") s->extName = name;
        else if (type == "ext_foreign_toplevel_image_capture_source_manager_v1") { s->standardSource = true; s->sourceName = name; }
        else if (type == "ext_image_copy_capture_manager_v1") { s->imageCopy = true; s->copyName = name; }
      },
      [](void *data, wl_registry *, uint32_t name) {
        auto *s = static_cast<Private *>(data);
        if ((s->wlr && (name == s->hyprName || name == s->wlrName || name == s->shmName))
            || (s->ext && (name == s->extName || name == s->sourceName || name == s->copyName)))
          s->managerFinished();
      }};
  wl_registry_add_listener(d->registry, &registryListener, d.get());
  d->sync = wl_display_sync(d->display);
  static const wl_callback_listener listener = {
      [](void *data, wl_callback *callback, uint32_t) {
        auto *s = static_cast<Private *>(data);
        wl_callback_destroy(callback);
        s->sync = nullptr;
        s->bindList();
      }};
  wl_callback_add_listener(d->sync, &listener, d.get());
  d->notifier = new QSocketNotifier(wl_display_get_fd(d->display), QSocketNotifier::Read, this);
  connect(d->notifier, &QSocketNotifier::activated, this, [this] {
    if (wl_display_dispatch(d->display) < 0) d->lost();
    else d->flush();
  });
  d->timer.start(3000);
  d->flush();
}

QVariantList WaylandWindows::windows() const {
  QVariantList list;
  if (!d->reason.isEmpty()) return list;
  for (const auto &entry : d->entries) {
    if (!entry->done || (entry->ext && entry->identifier.isEmpty())) continue;
    list.append(QVariantMap{{"id", entry->id}, {"title", entry->title},
                            {"appId", entry->appId},
                            {"address", entry->address ? QString::number(entry->address, 16) : QString{}},
                            {"identifier", entry->identifier},
                            {"label", entry->appId + " — " + (entry->title.isEmpty() ? "(untitled)" : entry->title)}});
  }
  return list;
}

QString WaylandWindows::unavailableReason() const { return d->reason; }

void WaylandWindows::capture(const QString &id) {
  cancelCapture();
  if (!d->reason.isEmpty()) { d->error(d->reason); return; }
  const auto found = std::find_if(d->entries.begin(), d->entries.end(),
                                 [&id](const auto &entry) { return entry->done && entry->id == id; });
  if (found == d->entries.end()) { d->error("The selected window is no longer available."); return; }
  auto *window = found->get();
  d->active = id;
  d->timer.start(15000);
  if (window->wlr && d->hypr) {
    d->frame = hyprland_toplevel_export_manager_v1_capture_toplevel_with_wlr_toplevel_handle(
        d->hypr, 0, window->wlr);
    static const hyprland_toplevel_export_frame_v1_listener listener = {
        [](void *data, auto *, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
          auto *s = static_cast<Private *>(data);
          s->format = format; s->width = width; s->height = height; s->stride = stride;
        },
        [](void *, auto *, uint32_t, uint32_t, uint32_t, uint32_t) {},
        [](void *data, auto *, uint32_t flags) { static_cast<Private *>(data)->flags = flags; },
        [](void *data, auto *, uint32_t, uint32_t, uint32_t) {
          auto *s = static_cast<Private *>(data);
          auto image = copyWindowBuffer(s->mapping, s->format, s->width, s->height, s->stride,
                                       s->flags & HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_FLAGS_Y_INVERT);
          if (image.isNull()) s->error("Invalid isolated-window image.");
          else s->deliver(image);
        },
        [](void *data, auto *) { static_cast<Private *>(data)->error("Isolated window capture failed. The window may have closed; no screen crop was substituted."); },
        [](void *, auto *, uint32_t, uint32_t, uint32_t) {},
        [](void *data, auto *) { static_cast<Private *>(data)->allocate(); }};
    hyprland_toplevel_export_frame_v1_add_listener(d->frame, &listener, d.get());
    d->flush();
    return;
  }
  // Standard ext source path. The compositor's opaque identifier is shared
  // across connections and is never replaced by a title/address match.
  auto *process = new QProcess(this);
  d->process = process;
  const auto token = d->epoch;
  auto bytes = std::make_shared<QByteArray>();
  connect(process, &QProcess::readyReadStandardOutput, this, [this, process, bytes, token] {
    const auto chunk = process->readAllStandardOutput();
    if (token != d->epoch) return;
    if (bytes->size() + chunk.size() > 400 * 1024 * 1024)
      d->error("Isolated window image exceeded the capture limit.");
    else bytes->append(chunk);
  });
  connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
  connect(process, &QProcess::finished, this, [this, process, bytes, token](int code, QProcess::ExitStatus status) {
    process->deleteLater();
    if (token != d->epoch) return;
    d->process = nullptr;
    if (status != QProcess::NormalExit || code != 0) {
      d->error("Isolated window capture failed. Check grim -T and compositor support; no desktop fallback was used.");
      return;
    }
    const auto image = QImage::fromData(*bytes, "PPM");
    if (image.isNull() || image.width() > 32768 || image.height() > 32768
        || quint64(image.width()) * image.height() > 100'000'000)
      d->error("Invalid or oversized isolated-window image.");
    else d->deliver(image);
  });
  connect(process, &QProcess::errorOccurred, this, [this, process, token](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) return;
    process->deleteLater();
    if (token == d->epoch) d->error("Cannot start grim for isolated-window capture.");
  });
  process->start("grim", {"-T", window->identifier, "-t", "ppm", "-"}, QIODevice::ReadOnly);
}

void WaylandWindows::cancelCapture() {
  ++d->epoch;
  d->clearFrame();
  d->active.clear();
}

void WaylandWindows::stop() {
  cancelCapture();
  delete d->notifier;
  d->notifier = nullptr;
  if (d->sync) wl_callback_destroy(d->sync);
  d->sync = nullptr;
  d->entries.clear();
  if (d->wlr) zwlr_foreign_toplevel_manager_v1_destroy(d->wlr);
  if (d->ext) ext_foreign_toplevel_list_v1_destroy(d->ext);
  if (d->hypr) hyprland_toplevel_export_manager_v1_destroy(d->hypr);
  if (d->mappingManager) hyprland_toplevel_mapping_manager_v1_destroy(d->mappingManager);
  if (d->shm) wl_shm_destroy(d->shm);
  if (d->registry) wl_registry_destroy(d->registry);
  if (d->display) wl_display_disconnect(d->display);
  d->wlr = nullptr; d->ext = nullptr; d->hypr = nullptr; d->shm = nullptr;
  d->mappingManager = nullptr;
  d->registry = nullptr; d->display = nullptr;
  d->wlrName = d->extName = d->hyprName = d->shmName = d->sourceName = d->copyName = 0;
  d->standardSource = d->imageCopy = d->initializing = d->revoked = false;
  d->reason = "Window discovery has not started.";
}

}
