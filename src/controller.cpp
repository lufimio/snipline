#include "controller.h"
#include "core/exporter.h"

#include <LayerShellQt/Window>
#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDebug>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScreen>
#include <QUrl>

namespace snip {

QImage FrameProvider::requestImage(const QString &id, QSize *size,
                                   const QSize &) {
  bool ok = false;
  if (id.startsWith("window/")) {
    if (size) *size = window.size();
    return window;
  }
  const int index = id.toInt(&ok);
  if (!ok || index < 0 || index >= frames.size())
    return {};
  const auto image = frames.at(index).image;
  if (size)
    *size = image.size();
  return image;
}

Controller::Controller(QString directory, QByteArray format, QString themePath,
                       QObject *parent, CaptureBackend *capture, WindowBackend *windows,
                       SurfaceBarrier *barrier, WindowPicker *picker)
    : QObject(parent), m_directory(std::move(directory)),
      m_format(std::move(format)),
      m_capture(capture ? capture : new GrimCapture(this)),
      m_windows(windows ? windows : new WaylandWindows(this)),
      m_barrier(barrier ? barrier : new SurfaceBarrier(this)),
      m_picker(picker ? picker : new WindowPicker(this)) {
  if (!themePath.isEmpty()) {
    QFile file(themePath);
    QString error;
    if (!file.open(QIODevice::ReadOnly) ||
        !m_theme.load(file.read(64 * 1024 + 1), &error))
      qWarning().noquote() << "Theme not applied; using defaults:" << error;
  }
  connect(&m_selection, &Selection::changed, this, &Controller::changed);
  connect(m_picker, &WindowPicker::picked, this, [this](const QStringList &ids) {
    finishWindowPick(ids);
  });
  connect(m_picker, &WindowPicker::failed, this, [this](const QString &message) {
    finishWindowPick({}, message);
  });
  connect(m_capture, &CaptureBackend::ready, this, [this](QVector<Frame> frames) {
    if (m_phase == Phase::Preparing) showFrames(std::move(frames));
    else if (m_phase == Phase::CapturingLive) {
      const auto result = compose(frames, m_selection.rect());
      if (!result.error.isEmpty()) fail(result.error);
      else save(result.image);
    }
  });
  connect(m_capture, &CaptureBackend::failed, this, &Controller::fail);
  connect(m_capture, &CaptureBackend::layoutVerified, this, [this] {
    if (m_phase == Phase::VerifyingFrozen) commit();
    else if (m_phase == Phase::VerifyingLiveWindow) {
      m_phase = Phase::CapturingLiveWindow;
      m_windows->capture(m_windowId);
    }
  });
  connect(m_barrier, &SurfaceBarrier::synchronized, this, &Controller::afterHidden);
  connect(m_barrier, &SurfaceBarrier::failed, this, &Controller::fail);
  connect(m_windows, &WindowBackend::changed, this, [this] {
    if (m_mode == "window" && !m_windowId.isEmpty() && !windowUnavailable().isEmpty())
      fail(windowUnavailable());
    else
      emit changed();
  });
  connect(m_windows, &WindowBackend::failed, this, &Controller::fail);
  connect(m_windows, &WindowBackend::closed, this, [this](const QString &id) {
    if (m_mode == "window" && id == m_windowId) {
      if (m_provider) m_provider->window = {};
      fail("The selected window closed. No other window or desktop source was substituted.");
    }
  });
  connect(m_windows, &WindowBackend::imageReady, this, [this](const QString &id, const QImage &image) {
    if (id != m_windowId || !m_provider) return;
    if (m_phase == Phase::WindowPreview) {
      m_provider->window = image;
      ++m_previewRevision;
      m_phase = Phase::Selecting;
      m_busy = false;
      emit changed();
    } else if (m_phase == Phase::CapturingLiveWindow) {
      // Revalidate after acquisition too, before committing anything.
      m_provider->window = image;
      m_phase = Phase::VerifyingFrozen;
      m_capture->verifyLayout(expectedLayout());
    }
  });
  auto layoutChanged = [this] {
    if (m_busy || !m_views.empty())
      fail(QStringLiteral(
          "Output layout changed. Cancel and start a new selection."));
  };
  auto watch = [this, layoutChanged](QScreen *screen) {
    connect(screen, &QScreen::geometryChanged, this, layoutChanged);
    connect(screen, &QScreen::physicalDotsPerInchChanged, this, layoutChanged);
    connect(screen, &QScreen::orientationChanged, this, layoutChanged);
  };
  for (auto *screen : QGuiApplication::screens())
    watch(screen);
  connect(qGuiApp, &QGuiApplication::screenAdded, this,
          [watch, layoutChanged](QScreen *screen) {
            watch(screen);
            layoutChanged();
          });
  connect(qGuiApp, &QGuiApplication::screenRemoved, this, layoutChanged);
}

Controller::~Controller() {
  m_picker->cancel();
  m_capture->cancel();
  m_windows->stop();
  m_barrier->cancel();
  m_views.clear();
  m_engine.reset();
}

QString Controller::Capture(const QString &mode) {
  return CaptureWithMode(mode, false);
}

QString Controller::CaptureWithMode(const QString &mode, bool live) {
  if (mode != "region" && mode != "monitor" && mode != "window")
    return QStringLiteral("Capture targets are region, monitor and window.");
  if (m_busy || !m_views.empty())
    return QStringLiteral("A selection is already active.");
  if (m_capture->requiresWayland() && QGuiApplication::platformName() != "wayland")
    return QStringLiteral("Capture requires the Qt Wayland platform. Set "
                          "QT_QPA_PLATFORM=wayland.");
  m_mode = mode;
  m_desktopPicking = true;
  m_pickMessage.clear();
  m_live = live;
  m_phase = Phase::Preparing;
  m_error.clear();
  m_selection.clear();
  m_busy = true;
  emit changed();
  // Window picking needs output geometry, not a desktop snapshot. Its frozen
  // pixels come exclusively from the isolated window selected in the list.
  if (m_live || m_mode == "window") m_capture->startLive();
  else m_capture->start();
  return {};
}

void Controller::showFrames(QVector<Frame> frames) {
  m_engine = std::make_unique<QQmlEngine>();
  m_provider = new FrameProvider;
  m_provider->frames = std::move(frames);
  m_engine->addImageProvider("frames", m_provider);
  QVector<Output> outputs;
  for (const auto &frame : m_provider->frames)
    outputs.push_back(frame.output);
  m_selection.setBounds(desktopBounds(outputs));
  for (int index = 0; index < outputs.size(); ++index) {
    const auto &output = outputs.at(index);
    QScreen *screen = nullptr;
    for (auto *candidate : QGuiApplication::screens())
      if (candidate->name() == output.name)
        screen = candidate;
    if (!screen) {
      release();
      fail(QStringLiteral("Cannot match compositor output to a Qt screen."));
      return;
    }
    auto view = std::make_unique<QQuickView>(m_engine.get(), nullptr);
    view->setScreen(screen);
    view->setResizeMode(QQuickView::SizeRootObjectToView);
    view->resize(output.logical.size().toSize());
    view->setColor(Qt::transparent);
    if (QGuiApplication::platformName() == "wayland") {
      auto *layer = LayerShellQt::Window::get(view.get());
    layer->setScreen(screen);
    layer->setScope("snipline-selection");
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    layer->setAnchors(
        LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) |
        LayerShellQt::Window::AnchorBottom | LayerShellQt::Window::AnchorLeft |
        LayerShellQt::Window::AnchorRight);
    layer->setExclusiveZone(-1);
    layer->setKeyboardInteractivity(
        LayerShellQt::Window::KeyboardInteractivityExclusive);
    }
    auto *context = view->rootContext();
    context->setContextProperty("controller", this);
    context->setContextProperty("selection", &m_selection);
    context->setContextProperty("theme", m_theme.tokens());
    context->setContextProperty("outputIndex", index);
    context->setContextProperty("outputRect", output.logical);
    view->setSource(QUrl("qrc:/qml/Overlay.qml"));
    if (view->status() == QQuickView::Error) {
      release();
      fail(QStringLiteral("Could not load the selection UI."));
      return;
    }
    m_views.push_back(std::move(view));
  }
  m_busy = false;
  m_phase = Phase::Selecting;
  if (m_mode == "monitor")
    selectMonitor(0);
  for (const auto &view : m_views)
    view->show();
  if (m_mode == "window") m_windows->start();
  emit changed();
}

void Controller::setMode(const QString &mode) {
  if (m_busy || !m_error.isEmpty() ||
      (mode != "region" && mode != "monitor" && mode != "window"))
    return;
  if (mode == m_mode) return;
  if (!m_live && mode != "window" && m_provider) {
    for (const auto &frame : m_provider->frames) {
      if (frame.image.isNull()) {
        restartSelection(mode, false);
        return;
      }
    }
  }
  m_mode = mode;
  m_desktopPicking = true;
  m_pickMessage.clear();
  m_picker->cancel();
  m_selection.clear();
  m_windowId.clear();
  if (m_provider) m_provider->window = {};
  if (mode == "window" && m_provider)
    for (auto &frame : m_provider->frames) frame.image = {};
  m_windows->stop();
  if (mode == "monitor")
    selectMonitor(0);
  if (mode == "window") m_windows->start();
  emit changed();
}

void Controller::setLive(bool live) {
  if (m_busy || !m_error.isEmpty() || m_views.empty() || live == m_live) return;
  restartSelection(m_mode, live);
}

void Controller::restartSelection(const QString &mode, bool live) {
  m_restartMode = mode;
  m_restartLive = live;
  m_busy = true;
  m_phase = Phase::Restarting;
  emit changed();
  hideOverlays();
  m_barrier->start();
}

void Controller::selectWindow(const QString &id) {
  if (m_busy || !m_provider || m_mode != "window" || !m_error.isEmpty()) return;
  bool found = false;
  for (const auto &value : windows())
    if (value.toMap().value("id").toString() == id) found = true;
  if (!found) return;
  m_desktopPicking = false;
  m_pickMessage.clear();
  m_windowId = id;
  m_provider->window = {};
  if (!m_live) {
    m_busy = true;
    m_phase = Phase::WindowPreview;
    m_windows->capture(id);
  }
  emit changed();
}

void Controller::startWindowPick() {
  if (m_busy || !m_provider || m_mode != "window" || !m_error.isEmpty()) return;
  m_windowId.clear();
  m_provider->window = {};
  m_desktopPicking = true;
  m_pickMessage.clear();
  if (nativeWindowPicker()) {
    m_busy = true;
    m_phase = Phase::NativePickHiding;
    hideOverlays();
    m_barrier->start();
  }
  emit changed();
}

void Controller::pickWindowAt(qreal x, qreal y) {
  if (m_busy || !m_provider || m_mode != "window" || !m_error.isEmpty() || !m_desktopPicking) return;
  if (nativeWindowPicker()) { startWindowPick(); return; }
  m_busy = true;
  m_phase = Phase::PickingWindow;
  m_windowId.clear();
  m_provider->window = {};
  m_pickMessage = "Finding the topmost window…";
  emit changed();
  m_picker->pick({x, y}, windows());
}

void Controller::finishWindowPick(const QStringList &ids, const QString &message) {
  if (m_phase != Phase::PickingWindow && m_phase != Phase::PickingNative) return;
  const bool hidden = m_phase == Phase::PickingNative;
  m_phase = Phase::Selecting;
  m_busy = false;
  if (hidden)
    for (const auto &view : m_views) view->show();
  m_pickMessage = message;
  if (message.isEmpty() && !ids.isEmpty())
    selectWindow(ids.first()); // Target choice never calls confirm().
  if (m_windowId.isEmpty() && m_pickMessage.isEmpty())
    m_pickMessage = "No capturable window was picked. Click again or use the list.";
  emit changed();
}

bool Controller::canConfirm() const {
  if (m_busy || !m_error.isEmpty() || m_phase != Phase::Selecting || !m_provider) return false;
  if (m_mode == "window")
    return !m_windowId.isEmpty() && windowUnavailable().isEmpty()
        && (m_live || !m_provider->window.isNull());
  return m_selection.valid();
}

QVector<Output> Controller::expectedLayout() const {
  QVector<Output> expected;
  if (m_provider)
    for (const auto &frame : m_provider->frames) expected.push_back(frame.output);
  return expected;
}

void Controller::selectMonitor(int index) {
  if (m_busy || !m_provider || index < 0 || index >= m_provider->frames.size())
    return;
  m_selection.setRect(m_provider->frames.at(index).output.logical);
}

void Controller::confirm() {
  if (!canConfirm()) return;
  m_busy = true;
  emit changed();
  if (m_live) {
    m_phase = Phase::Hiding;
    hideOverlays();
    m_barrier->start();
  } else {
    m_phase = Phase::VerifyingFrozen;
    m_capture->verifyLayout(expectedLayout());
  }
}

void Controller::afterHidden() {
  if (m_phase == Phase::NativePickHiding) {
    m_phase = Phase::PickingNative;
    m_picker->pickNative(windows());
  } else if (m_phase == Phase::Restarting) {
    const auto mode = m_restartMode;
    const bool live = m_restartLive;
    release();
    CaptureWithMode(mode, live);
  } else if (m_phase == Phase::Hiding) {
    if (m_mode == "window") {
      m_phase = Phase::VerifyingLiveWindow;
      m_capture->verifyLayout(expectedLayout());
    } else {
      m_phase = Phase::CapturingLive;
      m_capture->captureLive(expectedLayout());
    }
  }
}

void Controller::hideOverlays() {
  // Qt Quick Controls may create transient popup windows. Hiding only the
  // per-output views would not establish that every capture surface is gone.
  for (auto *window : QGuiApplication::topLevelWindows()) window->hide();
}

void Controller::commit() {
  if (!m_provider || !m_busy)
    return;
  if (m_mode == "window") {
    save(m_provider->window);
    return;
  }
  auto result = compose(m_provider->frames, m_selection.rect());
  if (!result.error.isEmpty()) {
    fail(result.error);
    return;
  }
  save(result.image);
}

void Controller::save(const QImage &image) {
  const auto saved = saveImage(image, m_directory, m_format);
  if (!saved.error.isEmpty()) {
    fail(saved.error);
    return;
  }
  auto *clipboard = QGuiApplication::clipboard();
  if (clipboard)
    clipboard->setImage(image);
  const bool copied =
      clipboard && clipboard->mimeData() && clipboard->mimeData()->hasImage();
  const QString body =
      copied
          ? QStringLiteral("Copied and saved to %1").arg(saved.path)
          : QStringLiteral("Saved to %1, but copying failed.").arg(saved.path);
  qInfo().noquote() << body;
  release();
  notify(copied ? QStringLiteral("Screenshot saved")
                : QStringLiteral("Clipboard error"),
         body);
}

void Controller::cancel() { release(); }

void Controller::Cancel() { cancel(); }

void Controller::release() {
  m_picker->cancel();
  m_pickMessage.clear();
  m_desktopPicking = true;
  m_capture->cancel();
  m_windows->stop();
  m_barrier->cancel();
  m_phase = Phase::Idle;
  m_windowId.clear();
  hideOverlays();
  // QML may be on the stack; defer destruction until its event returns.
  for (auto &view : m_views)
    view.release()->deleteLater();
  m_views.clear();
  if (m_engine)
    m_engine.release()->deleteLater();
  m_provider = nullptr;
  m_selection.clear();
  m_busy = false;
  m_error.clear();
  emit changed();
}

void Controller::fail(const QString &message) {
  const bool hidden = m_phase == Phase::Hiding || m_phase == Phase::CapturingLive
      || m_phase == Phase::VerifyingLiveWindow || m_phase == Phase::CapturingLiveWindow
      || m_phase == Phase::Restarting || (m_live && m_phase == Phase::VerifyingFrozen);
  const bool pickerHidden = m_phase == Phase::NativePickHiding || m_phase == Phase::PickingNative;
  m_picker->cancel();
  m_capture->cancel();
  m_windows->cancelCapture();
  m_barrier->cancel();
  m_phase = m_views.empty() ? Phase::Idle : Phase::Selecting;
  m_error = message;
  m_busy = false;
  qWarning().noquote() << message;
  emit changed();
  if (hidden || pickerHidden)
    for (const auto &view : m_views) view->show();
  if (m_views.empty())
    notify(QStringLiteral("Capture failed"), message);
}

void Controller::notify(const QString &title, const QString &body) {
  auto message = QDBusMessage::createMethodCall(
      "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", "Notify");
  message.setArguments(
      {QStringLiteral("Snipline"), uint(0), QStringLiteral("camera-photo"),
       title, body.toHtmlEscaped(), QStringList{}, QVariantMap{}, 5000});
  QDBusConnection::sessionBus().asyncCall(message);
}

void Controller::Quit() {
  release();
  QGuiApplication::quit();
}

} // namespace snip
