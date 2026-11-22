#include "controller.h"
#include "notification.h"
#include "core/exporter.h"
#include "core/editorcommand.h"

#include <LayerShellQt/Window>
#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QMimeDatabase>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScreen>
#include <QQuickItem>
#include <QJsonArray>
#include <QDesktopServices>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>
#include <QDBusInterface>
#include <QDBusReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaEnum>
#include <QSet>
#include <atomic>
#include <unistd.h>

namespace snip {

QImage FrameProvider::requestImage(const QString &id, QSize *size,
                                   const QSize &) {
  bool ok = false;
  const int index = id.section('?', 0, 0).toInt(&ok);
  if (!ok || index < 0 || index >= frames.size())
    return {};
  const auto image = frames.at(index).image;
  if (size)
    *size = image.size();
  return image;
}

Controller::Controller(QString directory, QByteArray format, QString themePath,
                       QObject *parent, CaptureBackend *capture, WindowBackend *windows,
                         SurfaceBarrier *barrier, WindowPicker *picker, VideoRecorder *recorder, AudioSources *audio, Uploader *upload)
     : QObject(parent), m_audioSources(audio ? audio : new AudioSources(this)),
       m_upload(upload ? upload : new Uploader(this)),
      m_recorder(recorder ? recorder : new VideoRecorder(this)),
       m_directory(std::move(directory)), m_recordDirectory(m_directory),
      m_format(std::move(format)),
      m_capture(capture ? capture : new GrimCapture(this)),
      m_windows(windows ? windows : new WaylandWindows(this)),
      m_barrier(barrier ? barrier : new SurfaceBarrier(this)),
        m_picker(picker ? picker : new WindowPicker(this)) {
  connect(&m_recognition, &Recognition::finished, this, &Controller::recognitionFinished);
  connect(&m_recognitionNotifications, &NotificationActions::openRequested, this, [this](const QUrl &url) {
    if (!m_sessionLocked) QDesktopServices::openUrl(url);
  });
  connect(&m_savedNotifications, &NotificationActions::editRequested, this,
      [this](const QString &path, const QString &command) {
        if (m_sessionLocked) return;
        QString error;
        if (!launchEditor(command, path, &error)) notify("Could not start editor", error + "\nSaved to " + path);
      });
  connect(&m_savedNotifications, &NotificationActions::fileOpenRequested, this,
      [this](const QString &path, bool folder) {
        if (!m_sessionLocked) openResult(path, folder ? "folder" : "open");
      });
  connect(&m_savedNotifications, &NotificationActions::openRequested, this, [this](const QUrl &url) {
    if (!m_sessionLocked) QDesktopServices::openUrl(url);
  });
  connect(&m_savedNotifications, &NotificationActions::uploadRequested, this,
      [this](const QString &path, const QString &destination, bool lens) { beginUpload(path, destination, lens); });
  connect(&m_savedNotifications, &NotificationActions::cancelRequested, this, [this](quint64 token) {
    if (token == m_uploadToken) m_upload->cancel();
  });
  connect(m_upload, &Uploader::changed, this, &Controller::changed);
  connect(m_upload, &Uploader::failed, this, [this](const QString &error) {
    m_savedNotifications.closeProgress(m_uploadToken);
    if (!m_sessionLocked) notify("Upload failed", error);
  });
  connect(m_upload, &Uploader::cancelled, this, [this] {
    m_savedNotifications.closeProgress(m_uploadToken);
    if (!m_sessionLocked) notify("Upload cancelled", "The saved file was kept. Data already received by the host may remain there.");
  });
  connect(m_upload, &Uploader::completed, this,
      [this](const QUrl &url, const QString &, const QString &destination, bool lens) {
        m_savedNotifications.closeProgress(m_uploadToken);
        if (m_sessionLocked) return;
        QUrl open = url;
        QString body;
        if (lens) {
          open = QUrl("https://lens.google.com/uploadbyurl?ep=gisbubu&url=" +
                      QString::fromLatin1(QUrl::toPercentEncoding(url.toString(QUrl::FullyEncoded))));
          body = "Image uploaded via " + m_upload->destinationLabel(destination) + "; sent to Google Lens.";
          if (!QDesktopServices::openUrl(open)) body += " Could not launch the browser; use Open to try again.";
        } else {
          auto *clipboard = QGuiApplication::clipboard();
          if (clipboard) clipboard->setText(url.toString(QUrl::FullyEncoded));
          body = m_upload->destinationLabel(destination) + "\n" + url.toString(QUrl::FullyEncoded)
                 + (clipboard ? "\nLink copied to the clipboard." : "\nClipboard unavailable.");
        }
        if (m_config.notifications)
          m_savedNotifications.show(lens ? "Google Lens" : "Upload complete", body,
              m_config.notificationTimeout, open);
      });
  m_transitionTimeout.setSingleShot(true);
  connect(&m_transitionTimeout, &QTimer::timeout, this, [this] {
    fail("Timed out presenting a clean frame for frozen capture.");
  });
  m_queryAudio = audio || m_capture->requiresWayland();
  m_audioTimer.setInterval(2000);
  connect(&m_audioTimer, &QTimer::timeout, this, &Controller::refreshAudioSources);
  connect(m_audioSources, &AudioSources::changed, this, [this] {
    QMap<QString, QVariantMap> known;
    for (const auto &id : selectedAudioIds())
      if (m_knownAudioSources.contains(id)) known[id] = m_knownAudioSources.value(id);
    for (const auto &value : m_audioSources->sources()) {
      const auto source = value.toMap();
      known[source.value("id").toString()] = source;
    }
    m_knownAudioSources = known;
    if (m_recording && m_phase == Phase::Selecting && m_audioSources->error().isEmpty())
      autoSelectWindowAudio();
    emit changed();
  });
  m_recordCaptureTimer.setTimerType(Qt::PreciseTimer);
  connect(&m_recordCaptureTimer, &QTimer::timeout, this, &Controller::acquireRecordingFrame);
  connect(&m_recordSession, &RecordingSession::changed, this, [this] {
    emit changed();
    if (m_recordSession.state() == RecordingSession::State::Countdown)
      qInfo() << "Recording starts in" << m_recordSession.countdownRemaining();
  });
  connect(&m_recordSession, &RecordingSession::prepareRequested, this, [this] {
    m_phase = Phase::RecordingHiding;
    hideOverlays();
    m_barrier->start();
  });
  connect(&m_recordSession, &RecordingSession::pauseRequested, this, [this](quint64, bool paused) {
    if (paused) {
      m_recordCaptureTimer.stop();
      m_capture->cancel();
      m_windows->cancelCapture();
      m_recordAcquiring = false;
    }
    m_recorder->setPaused(paused);
  });
  connect(&m_recordSession, &RecordingSession::finalizeRequested, this, [this] {
    stopRecordingCapture();
    m_recorder->stop();
  });
  connect(&m_recordSession, &RecordingSession::abortRequested, this, [this] {
    stopRecordingCapture();
    m_recorder->cancel();
    if (m_recordSession.state() == RecordingSession::State::Failed) {
      const auto error = m_recordSession.error();
      m_lastRecordingError = error;
      const bool quit = m_quitAfterRecording;
      release();
      qWarning().noquote() << error;
      notify("Recording failed", error);
      if (quit) QGuiApplication::quit();
    }
  });
  connect(m_recorder, &VideoRecorder::started, this, [this](quint64 generation) {
    if (!m_recording || generation != m_recordSession.generation()) return;
    m_recordSession.backendStarted(generation);
    qInfo() << "Recording started. Use snipline record pause/stop/discard.";
  });
  connect(m_recorder, &VideoRecorder::paused, this, [this](quint64 generation, bool paused) {
    if (!m_recording || generation != m_recordSession.generation()) return;
    m_recordSession.backendPaused(generation, paused);
    if (!paused && m_recordSession.state() == RecordingSession::State::Recording) {
      m_recordCaptureTimer.start();
      acquireRecordingFrame();
    }
  });
  connect(m_recorder, &VideoRecorder::saved, this, [this](quint64 generation, const QString &path) {
    if (!m_recording || generation != m_recordSession.generation() ||
        m_recordSession.state() != RecordingSession::State::Finalizing) return;
    m_recordSession.backendSaved(generation, path);
    if (m_recordSession.state() != RecordingSession::State::Saved) return;
    m_lastRecordingPath = path;
    const auto body = "Saved to " + path + (m_recordWarning.isEmpty() ? QString{} : "\nStopped: " + m_recordWarning);
    const bool quit = m_quitAfterRecording;
    const auto postAction = m_postRecordingAction;
    const auto editorCommand = m_config.videoEditor;
    const auto uploadDestination = m_postUploadDestination;
    release();
    openResult(path, postAction, editorCommand, uploadDestination);
    qInfo().noquote() << body;
    if (postAction != "upload" && postAction != "lens" && postAction != "edit"
        && postAction != "open" && postAction != "folder")
      notifySaved("Recording saved", body, path, editorCommand, false);
    if (quit) QGuiApplication::quit();
  });
  connect(m_recorder, &VideoRecorder::failed, this, [this](quint64 generation, const QString &message) {
    if (!m_recording || generation != m_recordSession.generation()) return;
    m_recordSession.backendFailed(generation, message);
  });
  connect(m_recorder, &VideoRecorder::sourceLost, this, [this](quint64 generation, const QString &message) {
    if (m_recording && generation == m_recordSession.generation()) recordingSourceFailed(message);
  });
  if (!themePath.isEmpty()) {
    QFile file(themePath);
    QString error;
    if (!file.open(QIODevice::ReadOnly) ||
        !m_theme.load(file.read(64 * 1024 + 1), &error))
      qWarning().noquote() << "Theme not applied; using defaults:" << error;
  }
  connect(&m_selection, &Selection::changed, this, &Controller::changed);
  m_geometryTimer.setInterval(200);
  connect(&m_geometryTimer, &QTimer::timeout, this, &Controller::refreshWindowGeometry);
  m_hoverTimer.setSingleShot(true);
  m_hoverTimer.setInterval(16);
  connect(&m_hoverTimer, &QTimer::timeout, this, &Controller::updateHoveredWindow);
  connect(m_picker, &WindowPicker::geometryChanged, this, [this] {
    if (m_mode != "window" || m_views.empty()) return;
    m_windowGeometry = m_picker->geometry();
    updateHoveredWindow();
    if (m_recording) autoSelectWindowAudio();
    emit changed();
  });
  connect(m_picker, &WindowPicker::picked, this, [this](const QStringList &ids) {
    finishWindowPick(ids);
  });
  connect(m_picker, &WindowPicker::failed, this, [this](const QString &message) {
    finishWindowPick({}, message);
  });
  connect(m_capture, &CaptureBackend::ready, this, [this](QVector<Frame> frames) {
    if (m_phase == Phase::RefreshingFrozen) {
      refreshFrozenFrames(std::move(frames));
      return;
    }
    if (m_phase == Phase::Recording && m_recording) {
      const auto result = compose(frames, m_recordRegion);
      if (!result.error.isEmpty()) recordingSourceFailed(result.error);
      else recordingFrame(result.image);
      return;
    }
    if (m_phase == Phase::Preparing) showFrames(std::move(frames));
    else if (m_phase == Phase::CapturingLive) {
      const auto result = compose(frames, m_selection.rect());
      if (!result.error.isEmpty()) fail(result.error);
      else save(result.image);
    }
  });
  connect(m_capture, &CaptureBackend::failed, this, &Controller::fail);
  connect(m_capture, &CaptureBackend::layoutVerified, this, [this] {
    if (m_phase == Phase::RecordVerifying) {
      m_phase = Phase::Recording;
      m_recordCaptureTimer.start(qMax(1, 1000 / m_recordOptions.fps));
      acquireRecordingFrame();
    } else if (m_phase == Phase::VerifyingFrozen) commit();
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
    if (m_recording && m_phase == Phase::Recording) {
      recordingFrame(image);
      return;
    }
    if (m_phase == Phase::FreezingWindow) {
      m_provider->window = image;
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
  if (m_capture->requiresWayland() && QGuiApplication::platformName() == "wayland")
    watchSessionLock();
}

Controller::~Controller() {
  const bool blocked = m_upload->blockSignals(true);
  m_upload->cancel();
  m_upload->blockSignals(blocked);
  m_savedNotifications.closeProgress(m_uploadToken);
  m_recognition.clear();
  restoreTransitionSurfaces();
  m_audioTimer.stop();
  m_audioSources->cancel();
  m_recordCaptureTimer.stop();
  m_recorder->cancel();
  m_hoverTimer.stop();
  m_hoverActive = false;
  m_hoveredWindow.clear();
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
  if (m_recording) return showRecordingControls();
  if (mode != "region" && mode != "monitor" && mode != "window")
    return QStringLiteral("Capture targets are region, monitor and window.");
  if (m_busy || !m_views.empty())
    return QStringLiteral("A selection is already active.");
  closeRecognition();
  if (m_capture->requiresWayland() && QGuiApplication::platformName() != "wayland")
    return QStringLiteral("Capture requires the Qt Wayland platform. Set "
                          "QT_QPA_PLATFORM=wayland.");
  m_mode = mode;
  clearWindowHover();
  m_windowGeometry.clear();
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
  if (m_mode == "window") {
    m_windows->start();
    m_geometryTimer.start();
    refreshWindowGeometry();
  }
  if (m_recording) refreshAudioSources();
  emit changed();
}

bool Controller::selectionOptionsEditable() const {
  return !m_busy && m_error.isEmpty() && m_provider && m_phase == Phase::Selecting;
}
void Controller::setAction(const QString &action) {
  if (!selectionOptionsEditable() || action == this->action() ||
      (action != "capture" && action != "record" && action != "qr" && action != "ocr" && action != "lens")) return;
  m_recognitionAction = (action == "qr" || action == "ocr" || action == "lens") ? action : QString{};
  m_recognition.prepare(action);
  if (action == "ocr" && m_recognition.languages().isEmpty()) m_recognition.refreshLanguages();
  m_pickMessage.clear();
  if (action == "record") {
    if (m_sessionLocked) { m_pickMessage = "Cannot record while the session is locked."; emit changed(); return; }
    m_captureLive = m_live;
    m_recordOptions = {m_recordDirectory, m_config.filenameTemplate, m_config.recordingFormat,
                       m_config.recordingFps, m_config.systemAudio, m_config.microphone,
                        m_config.systemAudioDevice, m_config.microphoneDevice, false, {}};
    m_recordOptions.codec = m_config.recordingCodec;
    m_recordOptions.encoder = m_config.recordingEncoder;
    m_recording = true;
    m_live = true;
    for (auto &frame : m_provider->frames) frame.image = {};
    m_provider->window = {};
    m_lastRecordingPath.clear();
    m_lastRecordingError.clear();
    m_recordWarning.clear();
    m_recordSession.begin(m_mode, m_config.countdown, m_config.maxDuration);
    beginAudioSelection();
  } else if (m_recording) {
    m_recordSession.discard();
    m_audioTimer.stop();
    m_audioSources->cancel();
    m_recording = false;
    if (m_mode == "window") {
      m_live = m_captureLive;
      if (!m_live && !m_windowId.isEmpty()) selectWindow(m_windowId);
    } else if (!m_captureLive) {
      restartSelection(m_mode, false);
      return;
    } else m_live = true;
  }
  emit changed();
}
void Controller::setImageFormat(const QString &format) {
  if (!selectionOptionsEditable() || m_recording ||
      (format != "png" && format != "jpeg" && format != "webp")) return;
  m_format = format.toLatin1();
  emit changed();
}
void Controller::setSaveDirectory(const QString &directory) {
  if (!selectionOptionsEditable()) return;
  QString path = directory;
  if (path.startsWith("~/")) path = QDir::home().filePath(path.mid(2));
  if (path == "~") path = QDir::homePath();
  if (path.trimmed().isEmpty() || (QFileInfo::exists(path) && !QFileInfo(path).isDir())) {
    m_pickMessage = "Choose a nonempty directory path, not a file.";
    emit changed();
    return;
  }
  const auto resolved = QDir(path).absolutePath();
  if (m_recording) {
    m_recordDirectory = resolved;
    m_recordOptions.directory = resolved;
    m_config.recordingDirectory = resolved;
  } else {
    m_directory = resolved;
    m_config.directory = resolved;
  }
  m_pickMessage.clear();
  emit changed();
}
void Controller::setFilenameTemplate(const QString &value) {
  if (!selectionOptionsEditable()) return;
  QString error;
  if (!validateFilenameTemplate(value, &error)) {
    m_pickMessage = error;
  } else {
    m_config.filenameTemplate = value;
    m_recordOptions.filenameTemplate = value;
    m_pickMessage.clear();
  }
  emit changed();
}
void Controller::setCopyToClipboard(bool enabled) {
  if (!selectionOptionsEditable()) return;
  m_config.clipboard = enabled;
  emit changed();
}
void Controller::setNotificationsEnabled(bool enabled) {
  if (!selectionOptionsEditable()) return;
  m_config.notifications = enabled;
  emit changed();
}
void Controller::setRecordingFps(int fps) {
  if (!recordingOptionsEditable() || fps < 1 || fps > 120) return;
  m_recordOptions.fps = fps;
  m_config.recordingFps = fps;
  emit changed();
}
void Controller::setCountdownSeconds(int seconds) {
  if (!recordingOptionsEditable() || seconds < 0 || seconds > 30) return;
  m_config.countdown = seconds;
  m_recordSession.discard();
  m_recordSession.begin(m_mode, seconds, m_config.maxDuration);
  emit changed();
}
void Controller::setSystemAudioDevice(const QString &device) {
  if (!recordingOptionsEditable()) return;
  for (const auto ch : device) {
    if (!ch.isPrint()) {
      m_pickMessage = "Audio source names cannot contain control characters.";
      emit changed();
      return;
    }
  }
  if (device.trimmed().isEmpty()) {
    m_pickMessage = "System audio requires a monitor source, e.g. @DEFAULT_MONITOR@.";
  } else {
    m_recordOptions.systemAudioDevice = device;
    m_config.systemAudioDevice = device;
    m_pickMessage.clear();
  }
  emit changed();
}
void Controller::setMicrophoneDevice(const QString &device) {
  if (!recordingOptionsEditable()) return;
  for (const auto ch : device) {
    if (!ch.isPrint()) {
      m_pickMessage = "Audio source names cannot contain control characters.";
      emit changed();
      return;
    }
  }
  m_recordOptions.microphoneDevice = device;
  m_config.microphoneDevice = device;
  m_pickMessage.clear();
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
  if (m_recording) {
    m_recordSession.discard();
    m_recordSession.begin(mode, m_config.countdown, m_config.maxDuration);
    if (mode == "window" && !m_audioUserEdited)
      m_manualAudioIds.removeAll("default:system");
    m_autoAudioIds.clear();
    m_suppressedAudioIds.clear();
  }
  clearWindowHover();
  m_windowGeometry.clear();
  m_geometryTimer.stop();
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
  if (mode == "window") {
    m_windows->start();
    m_geometryTimer.start();
    refreshWindowGeometry();
  }
  if (m_recording) autoSelectWindowAudio(true);
  emit changed();
}

void Controller::setLive(bool live) {
  if (m_recording) return; // Recording always selects over the live desktop.
  if (m_busy || !m_error.isEmpty() || m_views.empty() || live == m_live) return;
  restartSelection(m_mode, live);
}

void Controller::restartSelection(const QString &mode, bool live) {
  if (!m_provider || m_views.empty()) return;
  if (mode != m_mode) {
    m_live = true;
    setMode(mode);
  }
  if (live) {
    m_live = true;
    for (auto &frame : m_provider->frames) frame.image = {};
    m_provider->window = {};
    emit changed();
    return;
  }
  if (m_mode == "window") {
    m_live = false;
    if (!m_windowId.isEmpty()) selectWindow(m_windowId);
    emit changed();
    return;
  }
  // Keep layer surfaces mapped and QML state alive. Unmapping/recreating them
  // triggers compositor close/open animations even for a simple mode switch.
  m_busy = true;
  m_phase = Phase::RefreshingFrozen;
  clearWindowHover();
  m_picker->cancel();
  m_geometryTimer.stop();
  const auto epoch = ++m_transitionEpoch;
  for (auto *window : QGuiApplication::topLevelWindows()) {
    auto *quick = qobject_cast<QQuickWindow *>(window);
    if (!quick || !quick->isVisible()) continue;
    m_transitionSurfaces.append({quick, quick->contentItem()->opacity()});
    m_pendingRenders.insert(quick);
    quick->contentItem()->setOpacity(0);
    auto synced = std::make_shared<std::atomic_bool>(false);
    m_renderConnections.append(connect(quick, &QQuickWindow::afterSynchronizing, this, [synced] {
      synced->store(true, std::memory_order_release);
    }, Qt::DirectConnection));
    m_renderConnections.append(connect(quick, &QQuickWindow::frameSwapped, this, [this, quick, epoch, synced] {
      // Gate at emission time, not delivery time: a queued swap for an older,
      // still-visible frame must never authorize acquiring desktop pixels.
      if (!synced->load(std::memory_order_acquire)) return;
      QMetaObject::invokeMethod(this, [this, quick, epoch] {
        if (epoch != m_transitionEpoch || m_phase != Phase::RefreshingFrozen) return;
        if (!m_pendingRenders.remove(quick)) return;
        if (!m_pendingRenders.isEmpty()) return;
        for (const auto &connection : m_renderConnections) disconnect(connection);
        m_renderConnections.clear();
        m_transitionTimeout.stop();
        m_barrier->start();
      }, Qt::QueuedConnection);
    }, Qt::DirectConnection));
    quick->requestUpdate();
  }
  m_transitionTimeout.start(3000);
  emit changed();
}

void Controller::restoreTransitionSurfaces() {
  ++m_transitionEpoch;
  m_transitionTimeout.stop();
  for (const auto &connection : m_renderConnections) disconnect(connection);
  m_renderConnections.clear();
  m_pendingRenders.clear();
  for (const auto &entry : m_transitionSurfaces) {
    if (!entry.first) continue;
    entry.first->contentItem()->setOpacity(entry.second);
    entry.first->requestUpdate();
  }
  m_transitionSurfaces.clear();
}

void Controller::refreshFrozenFrames(QVector<Frame> frames) {
  QVector<Output> outputs;
  for (const auto &frame : frames) {
    outputs.append(frame.output);
    if (frame.image.isNull()) { fail("Cannot freeze an empty desktop frame."); return; }
  }
  if (!m_provider || outputs != expectedLayout()) {
    fail("Output layout changed while freezing. Cancel and start a new selection.");
    return;
  }
  m_provider->frames = std::move(frames);
  m_live = false;
  ++m_frameRevision;
  m_phase = Phase::Selecting;
  m_busy = false;
  emit changed();
  restoreTransitionSurfaces();
}

void Controller::selectWindow(const QString &id) {
  if (!canRetarget()) return;
  bool found = false;
  for (const auto &value : windows())
    if (value.toMap().value("id").toString() == id) found = true;
  if (!found) return;
  m_windows->cancelCapture();
  m_pickMessage.clear();
  m_windowId = id;
  if (m_recording) autoSelectWindowAudio(true);
  m_provider->window = {};
  if (!m_live) {
    m_busy = true;
    m_phase = Phase::FreezingWindow;
    m_windows->capture(id);
  } else {
    m_phase = Phase::Selecting;
    m_busy = false;
  }
  emit changed();
}

void Controller::startWindowPick() {
  if (!canRetarget()) return;
  m_windows->cancelCapture();
  m_picker->cancel();
  m_phase = Phase::Selecting;
  m_busy = false;
  m_windowId.clear();
  if (m_recording) autoSelectWindowAudio(true);
  m_provider->window = {};
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
  if (!canRetarget()) return;
  if (nativeWindowPicker()) { startWindowPick(); return; }
  m_windows->cancelCapture();
  m_busy = true;
  m_phase = Phase::PickingWindow;
  m_windowId.clear();
  if (m_recording) autoSelectWindowAudio(true);
  m_provider->window = {};
  m_pickMessage = "Finding the topmost window…";
  emit changed();
  m_picker->pick({x, y}, windows());
}

bool Controller::canRetarget() const {
  return m_provider && m_mode == "window" && m_error.isEmpty()
      && (m_phase == Phase::Selecting || m_phase == Phase::FreezingWindow || m_phase == Phase::PickingWindow);
}

void Controller::hoverWindowAt(qreal x, qreal y) {
  if (!canRetarget()) { clearWindowHover(); return; }
  m_hoverPoint = {x, y};
  m_hoverActive = true;
  if (!m_hoverTimer.isActive()) m_hoverTimer.start();
}

void Controller::clearWindowHover() {
  m_hoverTimer.stop();
  m_hoverActive = false;
  if (m_hoveredWindow.isEmpty()) return;
  m_hoveredWindow.clear();
  emit changed();
}

void Controller::updateHoveredWindow() {
  if (!m_hoverActive || !canRetarget()) return;
  const auto id = m_picker->hoveredAt(m_hoverPoint);
  if (id == m_hoveredWindow) return;
  m_hoveredWindow = id;
  emit changed();
}

void Controller::refreshWindowGeometry() {
  // Offscreen tests must not query the user's compositor through a real picker.
  if (!m_capture->requiresWayland() || m_mode != "window" || !m_provider || !m_error.isEmpty()
      || (m_phase != Phase::Selecting && m_phase != Phase::FreezingWindow)) return;
  m_picker->refresh(windows());
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
  if (m_recognitionAction == "lens" && (!m_upload->available() || m_upload->busy() ||
      !m_upload->hasDestination(m_config.lensDestination))) return false;
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
  const auto rect = m_provider->frames.at(index).output.logical;
  if (m_selection.rect() != rect) m_selection.setRect(rect);
}

void Controller::setVideoEditor(const QString &command) {
  if (!selectionOptionsEditable()) return;
  QString error;
  if (!validateEditorCommand(command, &error)) { m_pickMessage = error; emit changed(); return; }
  m_config.videoEditor = command.trimmed();
  emit changed();
}
void Controller::setEditor(const QString &command) {
  if (m_recording) { setVideoEditor(command); return; }
  if (!selectionOptionsEditable()) return;
  QString error;
  if (!validateEditorCommand(command, &error)) { m_pickMessage = error; emit changed(); return; }
  m_config.screenshotEditor = command.trimmed();
  emit changed();
}
bool Controller::validateResultAction(const QString &action, bool video) {
  if (action != "capture" && action != "save" && action != "folder" &&
      action != "open" && action != "edit" && action != "upload" && (video || action != "copy")) return false;
  if (action == "upload" && (!m_upload->available() || m_upload->busy() || !m_upload->hasDestination(m_upload->destination()))) {
    m_pickMessage = "Select an available upload destination; only one upload can run at a time.";
    emit changed();
    return false;
  }
  const auto command = video ? m_config.videoEditor : m_config.screenshotEditor;
  if (action == "edit" && command.trimmed().isEmpty()) {
    m_pickMessage = video ? "Configure recording.editor in config.json first."
                         : "Configure screenshots.editor in config.json first.";
    emit changed();
    return false;
  }
  return true;
}
void Controller::confirmWithAction(const QString &action) {
  if (!canConfirm() || !validateResultAction(action, m_recording)) return;
  if (action == "upload") m_postUploadDestination = m_upload->destination();
  if (m_recording) m_postRecordingAction = action;
  else m_captureDisposition = action;
  confirm();
}
void Controller::stopWithAction(const QString &action) {
  if (!m_recording || !validateResultAction(action, true)) return;
  if (action == "upload") m_postUploadDestination = m_upload->destination();
  m_postRecordingAction = action;
  Stop();
}
void Controller::openResult(const QString &path, const QString &action, const QString &editorCommand,
                             const QString &uploadDestination) {
  if (m_sessionLocked) return;
  if (action == "folder" || action == "open") {
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(
            action == "folder" ? QFileInfo(path).absolutePath() : path)))
      notify("Could not open saved file", "Saved to " + path + "; no associated application could be launched.");
  } else if (action == "edit") {
    QString error;
    if (!launchEditor(editorCommand, path, &error))
      notify("Could not start editor", error + "\nSaved to " + path);
  } else if (action == "upload" || action == "lens") {
    beginUpload(path, uploadDestination, action == "lens");
  }
}
void Controller::notifySaved(const QString &title, const QString &body, const QString &path,
                             const QString &editorCommand, bool image) {
  if (!m_config.notifications) return;
  const bool transferable = !m_sessionLocked && m_upload->available() && !m_upload->busy();
  const auto upload = transferable ? m_upload->resolveDestination(m_upload->destination()) : QString{};
  const QString lens;
  QString details = body;
  if (!upload.isEmpty()) details += "\nUpload: " + m_upload->destinationLabel(upload);
  if (!lens.isEmpty()) details += "\nLens sends this image to Google via " + m_upload->destinationLabel(lens);
  m_savedNotifications.showSaved(title, details, m_config.notificationTimeout,
      path, m_sessionLocked ? QString{} : editorCommand, image, upload, lens);
}
void Controller::confirm() {
  if (!canConfirm()) return;
  if (m_recording) {
    updateRecordingAudio();
    const auto check = VideoRecorder::check(m_recordOptions);
    if (!check.isEmpty()) { m_pickMessage = check; emit changed(); return; }
    m_recordOutput.clear();
    m_recordRegion = m_selection.rect();
    if (m_mode != "window") {
      bool found = false;
      for (const auto &output : expectedLayout()) {
        if (output.logical.contains(m_recordRegion)) {
          m_recordOutput = output.name;
          found = true;
          break;
        }
      }
      if (!found) {
        m_pickMessage = "Recording regions must fit entirely within one output. Cross-output recording is not supported.";
        emit changed();
        return;
      }
    }
    clearWindowHover();
    m_geometryTimer.stop();
    m_audioTimer.stop();
    m_audioSources->cancel();
    m_picker->cancel();
    m_busy = true;
    m_pickMessage.clear();
    emit changed();
    m_recordSession.confirm();
    return;
  }
  clearWindowHover();
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
  if (m_phase == Phase::RecordingHiding) {
    m_phase = Phase::RecordVerifying;
    m_capture->verifyLayout(expectedLayout());
  } else if (m_phase == Phase::NativePickHiding) {
    m_phase = Phase::PickingNative;
    m_picker->pickNative(windows());
  } else if (m_phase == Phase::RefreshingFrozen) {
    m_capture->captureLive(expectedLayout());
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
  clearWindowHover();
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
  if (m_recognitionAction == "qr" || m_recognitionAction == "ocr") {
    const auto kind = m_recognitionAction;
    const QImage input = image;
    release();
    m_recognition.startImage(kind, input);
    return;
  }
  if (m_captureDisposition == "copy") {
    auto *clipboard = QGuiApplication::clipboard();
    if (!clipboard) { fail("Clipboard is unavailable."); return; }
    clipboard->setImage(image);
    if (!clipboard->mimeData()->hasImage()) { fail("Could not copy the screenshot."); return; }
    release();
    notify("Screenshot copied", "Copied to the clipboard without saving a file.");
    return;
  }
  const auto saved = saveImage(image, m_directory, m_format, m_config.filenameTemplate);
  if (!saved.error.isEmpty()) {
    fail(saved.error);
    return;
  }
  auto *clipboard = QGuiApplication::clipboard();
  const bool shouldCopy = m_config.clipboard && m_captureDisposition == "capture" && m_recognitionAction != "lens";
  if (clipboard && shouldCopy)
    clipboard->setImage(image);
  const bool copied =
      shouldCopy && clipboard && clipboard->mimeData() && clipboard->mimeData()->hasImage();
  const QString body =
      !shouldCopy ? QStringLiteral("Saved to %1").arg(saved.path) : copied
          ? QStringLiteral("Copied and saved to %1").arg(saved.path)
          : QStringLiteral("Saved to %1, but copying failed.").arg(saved.path);
  qInfo().noquote() << body;
  const auto postAction = m_recognitionAction == "lens" ? QString("lens") : m_captureDisposition;
  const auto editorCommand = m_config.screenshotEditor;
  const auto uploadDestination = postAction == "lens" ? m_upload->resolveDestination(m_config.lensDestination) : m_postUploadDestination;
  m_lastCapture = saved.path;
  release();
  openResult(saved.path, postAction, editorCommand, uploadDestination);
  if (postAction != "upload" && postAction != "lens" && postAction != "edit"
      && postAction != "open" && postAction != "folder")
    notifySaved((copied || !shouldCopy) ? QStringLiteral("Screenshot saved")
                : QStringLiteral("Clipboard error"),
          body, saved.path, editorCommand, true);
}

void Controller::setLensDestination(const QString &destination) {
  if (!selectionOptionsEditable() || !m_upload->hasDestination(destination)) return;
  m_config.lensDestination = m_upload->resolveDestination(destination);
  emit changed();
}
QString Controller::beginUpload(const QString &path, const QString &destination, bool lens) {
  const auto reject = [this](const QString &error) {
    if (!m_sessionLocked) notify("Upload unavailable", error);
    return error;
  };
  if (m_sessionLocked) return reject("Cannot upload while the session is locked.");
  if (m_upload->busy()) return reject("An upload is already running. Cancel it or wait for it to finish.");
  ++m_uploadToken;
  const auto mime = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchContent).name();
  m_uploadImage = mime.startsWith("image/");
  m_uploadEditor = m_uploadImage ? m_config.screenshotEditor : mime.startsWith("video/") ? m_config.videoEditor : QString{};
  if (!m_upload->start(path, destination, lens, m_config.uploadTimeout * 1000)) return m_upload->error();
  if (m_config.notifications)
    m_savedNotifications.showProgress(lens ? "Uploading for Google Lens" : "Uploading",
        QFileInfo(path).fileName() + "\n" + m_upload->status(), m_uploadToken);
  return {};
}
QString Controller::Upload(const QString &file, const QString &destination) {
  return beginUpload(QFileInfo(file).absoluteFilePath(), destination.isEmpty() ? m_upload->destination() : destination, false);
}
QString Controller::Lens(const QString &target, const QString &file, int live) {
  if (live < -1 || live > 1) return "Capture timing must be -1 (config default), 0 (frozen) or 1 (live).";
  if (m_sessionLocked) return "Cannot search with Lens while the session is locked.";
  if (!file.isEmpty()) {
    if (live != -1) return "Live/frozen timing cannot be used with an existing image.";
    return beginUpload(QFileInfo(file).absoluteFilePath(), m_config.lensDestination, true);
  }
  if (m_upload->busy()) return "An upload is already running.";
  if (!m_upload->hasDestination(m_config.lensDestination)) return "Choose an available Lens upload destination.";
  if (m_recording || m_busy || !m_views.empty()) return "A capture or recording is already active.";
  const auto error = CaptureWithMode(target.isEmpty() ? "region" : target, live < 0 ? m_config.live : bool(live));
  if (!error.isEmpty()) return error;
  m_recognitionAction = "lens";
  emit changed();
  return {};
}

QString Controller::Recognize(const QString &kind, const QString &target, const QString &file) {
  return RecognizeWithMode(kind, target, file, -1);
}
QString Controller::RecognizeWithMode(const QString &kind, const QString &target, const QString &file, int live) {
  if (kind != "qr" && kind != "ocr") return "Recognition actions are qr and ocr.";
  if (live < -1 || live > 1) return "Capture timing must be -1 (config default), 0 (frozen) or 1 (live).";
  if (!file.isEmpty() && live != -1) return "Live/frozen timing cannot be used with an existing image.";
  if (m_sessionLocked) return "Cannot recognize images while the session is locked.";
  if (m_recording || m_busy || !m_views.empty()) return "A capture or recording is already active.";
  if (!file.isEmpty()) {
    closeRecognition();
    m_recognition.startFile(kind, QFileInfo(file).absoluteFilePath());
    return {};
  }
  const auto error = CaptureWithMode(target.isEmpty() ? "region" : target, live < 0 ? m_config.live : bool(live));
  if (!error.isEmpty()) return error;
  m_recognitionAction = kind;
  m_recognition.prepare(kind);
  if (kind == "ocr" && m_recognition.languages().isEmpty()) m_recognition.refreshLanguages();
  emit changed();
  return {};
}
void Controller::recognitionFinished(bool success) {
  if (m_sessionLocked) return;
  const bool ocr = m_recognition.kind() == "ocr";
  const QString title = ocr ? "Text recognition" : "QR / barcodes";
  QString body;
  QUrl url;
  if (!success) body = m_recognition.error();
  else if (ocr) {
    body = m_recognition.text().trimmed().isEmpty() ? "No text found. Clipboard unchanged."
        : copyRecognition() ? "Recognized text copied to the clipboard." : "Could not copy recognized text.";
  } else {
    const auto codes = m_recognition.codes();
    if (codes.isEmpty()) body = "No codes found. Clipboard unchanged.";
    else {
      int index = 0;
      for (int i = 0; i < codes.size(); ++i) {
        const auto candidate = codeUrl(i);
        if (!candidate.isEmpty()) { index = i; url = candidate; break; }
      }
      const bool copied = copyRecognition(index);
      body = copied ? (url.isEmpty() ? "Code payload copied to the clipboard." : "Link copied to the clipboard.")
                    : "Could not copy the code payload.";
      if (codes.size() > 1) body += QString(" %1 codes found; copied the first web link or, if none, the first payload.").arg(codes.size());
    }
  }
  if (m_config.notifications) m_recognitionNotifications.show(title, body, m_config.notificationTimeout, url);
}
void Controller::closeRecognition() {
  m_recognition.clear();
}
bool Controller::copyRecognition(int index) {
  if (m_sessionLocked || m_recognition.busy()) return false;
  QString text;
  if (index < 0 && m_recognition.kind() == "ocr") text = m_recognition.text();
  else {
    const auto codes = m_recognition.codes();
    if (index < 0 || index >= codes.size()) return false;
    text = codes[index].toMap().value("text").toString();
  }
  auto *clipboard = QGuiApplication::clipboard();
  if (!clipboard || text.isEmpty()) return false;
  clipboard->setText(text);
  return clipboard->text() == text;
}
QUrl Controller::codeUrl(int index) const {
  const auto codes = m_recognition.codes();
  if (m_recognition.busy() || index < 0 || index >= codes.size()) return {};
  const auto code = codes[index].toMap();
  if (code.value("binary").toBool()) return {};
  const auto text = code.value("text").toString();
  for (const auto ch : text) if (!ch.isPrint() || ch.isSpace()) return {};
  const QUrl url(text, QUrl::StrictMode);
  if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
      (url.scheme() != "https" && url.scheme() != "http")) return {};
  return url;
}
bool Controller::canOpenCode(int index) const { return !m_sessionLocked && !codeUrl(index).isEmpty(); }
bool Controller::openCode(int index) {
  const auto url = codeUrl(index);
  return !m_sessionLocked && !url.isEmpty() && QDesktopServices::openUrl(url);
}

void Controller::cancel() {
  m_upload->cancel();
  closeRecognition();
  if (m_recording) m_recordSession.discard();
  release();
}

void Controller::Cancel() { cancel(); }
void Controller::hideRecordingControls() {
  if (!m_recordControls) return;
  m_recordControls = false;
  hideOverlays();
  emit changed();
}

void Controller::release() {
  m_postUploadDestination.clear();
  m_recognitionAction.clear();
  m_captureDisposition = "capture";
  m_postRecordingAction.clear();
  m_recordControls = false;
  restoreTransitionSurfaces();
  m_audioTimer.stop();
  m_audioSources->cancel();
  if (m_recording && m_recordSession.state() != RecordingSession::State::Idle &&
      m_recordSession.state() != RecordingSession::State::Saved &&
      m_recordSession.state() != RecordingSession::State::Failed)
    m_recordSession.discard();
  stopRecordingCapture();
  m_recorder->cancel();
  m_recording = false;
  m_encoderStarted = false;
  m_quitAfterRecording = false;
  clearWindowHover();
  m_picker->cancel();
  m_geometryTimer.stop();
  m_windowGeometry.clear();
  m_pickMessage.clear();
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
  restoreTransitionSurfaces();
  if (m_recording && (m_views.empty() || m_recordSession.state() != RecordingSession::State::Selecting)) {
    recordingSourceFailed(message);
    return;
  }
  clearWindowHover();
  const bool hidden = m_phase == Phase::Hiding || m_phase == Phase::CapturingLive
      || m_phase == Phase::VerifyingLiveWindow || m_phase == Phase::CapturingLiveWindow
      || (m_live && m_phase == Phase::VerifyingFrozen);
  const bool pickerHidden = m_phase == Phase::NativePickHiding || m_phase == Phase::PickingNative;
  m_geometryTimer.stop();
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

void Controller::notify(const QString &title, const QString &body, const QString &imagePath) {
  if (!m_config.notifications) return;
  QDBusConnection::sessionBus().asyncCall(
      screenshotNotification(title, body, imagePath,
          title == "Upload failed" ? (m_config.notificationTimeout > 0 ? m_config.notificationTimeout : 5000) : m_config.notificationTimeout));
}

QString Controller::Record(const QString &mode, const QString &format, bool systemAudioValue, bool microphoneValue) {
  if (m_recording) return showRecordingControls();
  if (m_sessionLocked) return "Cannot start recording while the session is locked.";
  if (m_busy || !m_views.empty() || m_recording) return "A capture or recording is already active.";
  RecordingOptions options;
  options.directory = m_recordDirectory;
  options.filenameTemplate = m_config.filenameTemplate;
  options.format = format.isEmpty() ? m_config.recordingFormat : format;
  options.fps = m_config.recordingFps;
  options.codec = m_config.recordingCodec;
  options.encoder = m_config.recordingEncoder;
  options.systemAudio = systemAudioValue;
  options.microphone = microphoneValue;
  options.systemAudioDevice = m_config.systemAudioDevice;
  options.microphoneDevice = m_config.microphoneDevice;
  const auto check = VideoRecorder::check(options);
  if (!check.isEmpty()) return check;
  const auto error = CaptureWithMode(mode, true);
  if (!error.isEmpty()) return error;
  if (m_phase == Phase::Idle) return m_error.isEmpty() ? "Cannot open recording selection." : m_error;
  m_recording = true;
  m_captureLive = m_config.live;
  m_recordOptions = options;
  m_lastRecordingPath.clear();
  m_lastRecordingError.clear();
  m_recordWarning.clear();
  m_recordSession.begin(mode, m_config.countdown, m_config.maxDuration);
  beginAudioSelection();
  emit changed();
  return {};
}
QString Controller::RecordEncoding(const QString &mode, const QString &format, int systemAudio, int microphone,
                                   const QString &codec, const QString &encoder) {
  if (m_recording) return showRecordingControls();
  const auto previousCodec = m_config.recordingCodec;
  const auto previousEncoder = m_config.recordingEncoder;
  if (!codec.isEmpty()) m_config.recordingCodec = codec;
  if (!encoder.isEmpty()) m_config.recordingEncoder = encoder;
  const auto result = RecordConfigured(mode, format, systemAudio, microphone);
  m_config.recordingCodec = previousCodec;
  m_config.recordingEncoder = previousEncoder;
  return result;
}
QString Controller::RecordConfigured(const QString &mode, const QString &format, int systemAudio, int microphone) {
  if (systemAudio < -1 || systemAudio > 1 || microphone < -1 || microphone > 1)
    return "Audio overrides must be -1 (config default), 0 or 1.";
  const auto error = Record(mode.isEmpty() ? m_config.target : mode, format,
      systemAudio < 0 ? m_config.systemAudio : bool(systemAudio),
      microphone < 0 ? m_config.microphone : bool(microphone));
  if (error.isEmpty() && m_mode == "window" && systemAudio == 1) {
    // An explicit CLI request may add the full mix; window auto-selection
    // itself must never add it as a substitute for application audio.
    m_manualAudioIds.append(m_recordOptions.systemAudioDevice == "@DEFAULT_MONITOR@"
                            ? "default:system" : "source:" + m_recordOptions.systemAudioDevice);
    emit changed();
  }
  return error;
}
bool Controller::recordingOptionsEditable() const {
  return m_recording && !m_busy && m_recordSession.state() == RecordingSession::State::Selecting;
}
QStringList Controller::selectedAudioIds() const {
  auto result = m_manualAudioIds;
  for (const auto &id : m_autoAudioIds)
    if (!m_suppressedAudioIds.contains(id) && !result.contains(id)) result.append(id);
  return result;
}
QVariantList Controller::audioSources() const {
  auto result = m_audioSources->sources();
  const auto selected = selectedAudioIds();
  QSet<QString> listed;
  for (auto &value : result) {
    auto source = value.toMap();
    const auto id = source.value("id").toString();
    source.insert("selected", selected.contains(id));
    listed.insert(id);
    value = source;
  }
  for (const auto &id : selected) {
    if (listed.contains(id)) continue;
    auto source = m_knownAudioSources.value(id);
    source.insert("id", id);
    source.insert("kind", "unavailable");
    source.insert("selected", true);
    source.insert("label", source.value("label", id).toString() + " (unavailable — uncheck to remove)");
    result.append(source);
  }
  return result;
}
QString Controller::audioMessage() const {
  if (!m_audioSources->error().isEmpty()) return m_audioSources->error();
  return m_autoAudioMessage;
}
void Controller::beginAudioSelection() {
  m_manualAudioIds.clear();
  m_autoAudioIds.clear();
  m_suppressedAudioIds.clear();
  m_autoAudioMessage.clear();
  m_audioUserEdited = false;
  if (m_mode != "window" && m_recordOptions.systemAudio)
    m_manualAudioIds.append(m_recordOptions.systemAudioDevice == "@DEFAULT_MONITOR@"
                            ? "default:system" : "source:" + m_recordOptions.systemAudioDevice);
  if (m_recordOptions.microphone)
    m_manualAudioIds.append(m_recordOptions.microphoneDevice.isEmpty()
                            ? "default:mic" : "source:" + m_recordOptions.microphoneDevice);
  for (const auto &value : m_audioSources->sources()) {
    const auto source = value.toMap();
    m_knownAudioSources[source.value("id").toString()] = source;
  }
  m_recordOptions.explicitAudioInputs = true;
  autoSelectWindowAudio(true);
  if (m_queryAudio) {
    m_audioTimer.start();
    refreshAudioSources();
  }
}
void Controller::refreshAudioSources() {
  if (!recordingOptionsEditable() || !m_queryAudio) return;
  m_audioSources->refresh();
}
void Controller::autoSelectWindowAudio(bool retarget) {
  if (!m_recording || m_mode != "window") {
    m_autoAudioIds.clear();
    m_autoAudioMessage.clear();
    return;
  }
  if (retarget) m_suppressedAudioIds.clear();
  QVariantMap window;
  auto catalog = windows();
  for (auto &value : catalog) {
    auto entry = value.toMap();
    const auto metadata = m_picker->metadata(entry.value("id").toString());
    for (auto it = metadata.begin(); it != metadata.end(); ++it) entry[it.key()] = it.value();
    if (entry.value("id").toString() == m_windowId) window = entry;
    value = entry;
  }
  m_autoAudioIds = AudioSources::forWindow(m_audioSources->sources(), window, catalog);
  m_autoAudioMessage = m_windowId.isEmpty() ? "Choose a window to auto-select its application audio."
      : m_autoAudioIds.isEmpty()
          ? "No unambiguous audio stream for this window. Select sources manually; desktop audio is never substituted."
          : "Matched application audio. Shared-process/browser windows require identifiable stream metadata.";
}
void Controller::toggleAudioSource(const QString &id) {
  if (!recordingOptionsEditable()) return;
  bool available = false;
  for (const auto &value : audioSources())
    if (value.toMap().value("id").toString() == id) available = true;
  if (!available) return;
  m_audioUserEdited = true;
  if (selectedAudioIds().contains(id)) {
    m_manualAudioIds.removeAll(id);
    if (m_autoAudioIds.contains(id)) m_suppressedAudioIds.append(id);
  } else {
    if (selectedAudioCount() >= 16) { m_pickMessage = "Select at most 16 audio sources."; emit changed(); return; }
    m_suppressedAudioIds.removeAll(id);
    m_manualAudioIds.append(id);
  }
  emit changed();
}
void Controller::updateRecordingAudio() {
  m_recordOptions.explicitAudioInputs = true;
  m_recordOptions.audioInputs.clear();
  for (const auto &value : audioSources())
    if (value.toMap().value("selected").toBool()) m_recordOptions.audioInputs.append(value);
}
void Controller::setRecordingFormat(const QString &format) {
  if (!recordingOptionsEditable() || (format != "mp4" && format != "webm" && format != "gif")) return;
  m_recordOptions.format = format;
  m_config.recordingFormat = format;
  if (!recordingCodecs().contains(m_recordOptions.codec)) m_recordOptions.codec = "auto";
  if (format == "gif" && m_recordOptions.encoder != "software" && m_recordOptions.encoder != "auto")
    m_recordOptions.encoder = "software";
  m_config.recordingCodec = m_recordOptions.codec;
  m_config.recordingEncoder = m_recordOptions.encoder;
  emit changed();
}
void Controller::setRecordingCodec(const QString &codec) {
  if (!recordingOptionsEditable() || !recordingCodecs().contains(codec)) return;
  m_config.recordingCodec = m_recordOptions.codec = codec;
  emit changed();
}
void Controller::setRecordingEncoder(const QString &encoder) {
  if (!recordingOptionsEditable() ||
      !QStringList{"software", "auto", "vaapi", "nvenc", "qsv"}.contains(encoder)) return;
  if (recordingFormat() == "gif" && encoder != "software" && encoder != "auto") return;
  m_config.recordingEncoder = m_recordOptions.encoder = encoder;
  emit changed();
}
void Controller::setSystemAudio(bool enabled) {
  if (!recordingOptionsEditable()) return;
  m_recordOptions.systemAudio = enabled;
  m_config.systemAudio = enabled;
  emit changed();
}
void Controller::setMicrophone(bool enabled) {
  if (!recordingOptionsEditable()) return;
  m_recordOptions.microphone = enabled;
  m_config.microphone = enabled;
  emit changed();
}
QString Controller::recordingState() const {
  return QString::fromLatin1(QMetaEnum::fromType<RecordingSession::State>()
                                .valueToKey(int(m_recordSession.state())));
}
QString Controller::showRecordingControls() {
  if (m_sessionLocked) return "Cannot show recording controls while the session is locked.";
  const auto state = m_recordSession.state();
  if (state == RecordingSession::State::Paused || state == RecordingSession::State::Recording) {
    m_recordControls = true;
  } else if (state != RecordingSession::State::Selecting &&
             state != RecordingSession::State::Countdown) {
    return "Recording is changing state; try again shortly.";
  }
  emit changed();
  for (const auto &view : m_views) {
    view->show();
    view->requestActivate();
  }
  return {};
}
QString Controller::Status() const {
  return QString::fromUtf8(QJsonDocument(QJsonObject{
      {"recording", m_recording}, {"state", recordingState()},
      {"target", m_mode}, {"format", m_recordOptions.format},
      {"codec", m_recordOptions.codec}, {"encoder", m_recordOptions.encoder},
      {"selectedCodec", m_recorder->selectedCodec()}, {"selectedEncoder", m_recorder->selectedEncoder()},
      {"elapsedMs", m_recordSession.elapsedMs()},
      {"countdown", m_recordSession.countdownRemaining()},
      {"audio", QJsonArray::fromVariantList(m_recorder->audioDiagnostics())},
      {"upload", QJsonObject{{"busy", m_upload->busy()}, {"status", m_upload->status()},
                             {"url", m_upload->url().toString()}, {"error", m_upload->error()}}},
      {"path", m_lastRecordingPath}, {"error", m_lastRecordingError},
      {"warning", m_recordWarning}}).toJson(QJsonDocument::Compact));
}
QString Controller::Pause() {
  if (!m_recording || m_recordSession.state() != RecordingSession::State::Recording)
    return "No running recording to pause.";
  m_recordSession.pause();
  return {};
}
QString Controller::Resume() {
  if (pausePending()) return "A pause/resume change is already pending.";
  if (!m_recording || m_recordSession.state() != RecordingSession::State::Paused)
    return "No paused recording to resume.";
  m_recordSession.resume();
  return {};
}
QString Controller::TogglePause() {
  if (!m_recording) return "No active recording to pause/resume.";
  if (m_recordSession.pausePending()) return "A pause/resume change is already pending.";
  return m_recordSession.state() == RecordingSession::State::Paused ? Resume() : Pause();
}
QString Controller::Stop() {
  if (!m_recording) return "No active recording.";
  m_recordSession.stop();
  if (m_recordSession.state() == RecordingSession::State::Idle) release();
  return {};
}
void Controller::stopRecordingCapture() {
  m_recordCaptureTimer.stop();
  m_recordAcquiring = false;
  if (m_recording) {
    m_capture->cancel();
    m_windows->cancelCapture();
  }
}
void Controller::acquireRecordingFrame() {
  const auto state = m_recordSession.state();
  if (!m_recording || m_recordAcquiring || m_phase != Phase::Recording ||
      (state != RecordingSession::State::Preparing && state != RecordingSession::State::Recording))
    return;
  m_recordAcquiring = true;
  if (m_mode == "window") m_windows->capture(m_windowId);
  else m_capture->captureRecording(expectedLayout(), m_recordOutput);
}
void Controller::recordingFrame(const QImage &image) {
  if (!m_recording || m_phase != Phase::Recording || !m_recordAcquiring) return;
  m_recordAcquiring = false;
  if (image.isNull()) { recordingSourceFailed("Recording source returned an empty frame."); return; }
  if (image.width() > 8192 || image.height() > 8192 ||
      qint64(image.width()) * image.height() > 16'777'216) {
    recordingSourceFailed("Recording source exceeded the 8192-pixel / 16-megapixel limit.");
    return;
  }
  if (!m_encoderStarted) {
    m_encoderStarted = true;
    m_recorder->start(m_recordSession.generation(), m_recordOptions, image);
  } else m_recorder->submitFrame(image);
}
void Controller::recordingSourceFailed(const QString &message) {
  const auto state = m_recordSession.state();
  if (state == RecordingSession::State::Finalizing) return;
  m_recordWarning = message;
  if (state == RecordingSession::State::Recording || state == RecordingSession::State::Paused) {
    qWarning().noquote() << "Stopping recording:" << message;
    m_recordSession.stop();
    return;
  }
  const bool quit = m_quitAfterRecording;
  m_recordSession.discard();
  m_lastRecordingError = message;
  release();
  qWarning().noquote() << message;
  notify("Recording failed", message);
  if (quit) QGuiApplication::quit();
}
void Controller::SessionLocked() {
  m_sessionLocked = true;
  m_recognitionNotifications.invalidate();
  m_savedNotifications.invalidate();
  m_upload->cancel();
  closeRecognition();
  if (!m_recognitionAction.isEmpty()) release();
  if (m_recording) {
    m_recordControls = false;
    m_postRecordingAction.clear(); // Never start a viewer/editor behind the lock screen.
    hideOverlays();
    m_recordWarning = "Session locked.";
    Stop();
    emit changed();
  }
}
void Controller::ScreenSaverActive(bool active) {
  if (active) SessionLocked();
  else m_sessionLocked = false;
}
void Controller::SessionPropertiesChanged(QString interface, QVariantMap changed, QStringList invalidated) {
  Q_UNUSED(invalidated)
  if (interface == "org.freedesktop.login1.Session" && changed.contains("LockedHint"))
    ScreenSaverActive(changed.value("LockedHint").toBool());
}
void Controller::watchSessionLock() {
  auto system = QDBusConnection::systemBus();
  QDBusInterface manager("org.freedesktop.login1", "/org/freedesktop/login1",
                         "org.freedesktop.login1.Manager", system);
  manager.setTimeout(1000);
  const QDBusReply<QDBusObjectPath> reply = manager.call("GetSessionByPID", uint(getpid()));
  if (reply.isValid()) {
    const auto path = reply.value().path();
    system.connect("org.freedesktop.login1", path, "org.freedesktop.login1.Session",
                   "Lock", this, SLOT(SessionLocked()));
    system.connect("org.freedesktop.login1", path, "org.freedesktop.DBus.Properties",
                   "PropertiesChanged", this, SLOT(SessionPropertiesChanged(QString,QVariantMap,QStringList)));
    QDBusInterface session("org.freedesktop.login1", path, "org.freedesktop.login1.Session", system);
    session.setTimeout(1000);
    m_sessionLocked = session.property("LockedHint").toBool();
  } else qWarning() << "logind lock monitoring unavailable; run snipline record stop before locking.";
  auto session = QDBusConnection::sessionBus();
  session.connect("org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                  "org.freedesktop.ScreenSaver", "ActiveChanged", this, SLOT(ScreenSaverActive(bool)));
}

void Controller::Quit() {
  const auto state = m_recordSession.state();
  if (m_recording && (state == RecordingSession::State::Recording ||
      state == RecordingSession::State::Paused || state == RecordingSession::State::Finalizing)) {
    m_quitAfterRecording = true;
    Stop();
    return;
  }
  if (m_recording) m_recordSession.discard();
  release();
  QGuiApplication::quit();
}

} // namespace snip
