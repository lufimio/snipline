#pragma once

#include "capture.h"
#include "core/selection.h"
#include "core/theme.h"
#include "surfacebarrier.h"
#include "windows.h"
#include "windowpicker.h"
#include <QQuickImageProvider>
#include <QQuickView>
#include <memory>
#include <vector>

namespace snip {

class FrameProvider final : public QQuickImageProvider {
public:
  FrameProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
  QImage requestImage(const QString &id, QSize *size, const QSize &) override;
  QVector<Frame> frames;
  QImage window;
};

class Controller final : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "io.github.snipline.Application")
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QString mode READ mode NOTIFY changed)
  Q_PROPERTY(bool live READ live NOTIFY changed)
  Q_PROPERTY(bool canConfirm READ canConfirm NOTIFY changed)
  Q_PROPERTY(QVariantList windows READ windows NOTIFY changed)
  Q_PROPERTY(QString windowUnavailable READ windowUnavailable NOTIFY changed)
  Q_PROPERTY(QString selectedWindow READ selectedWindow NOTIFY changed)
  Q_PROPERTY(int previewRevision READ previewRevision NOTIFY changed)
  Q_PROPERTY(bool desktopPicking READ desktopPicking NOTIFY changed)
  Q_PROPERTY(bool nativeWindowPicker READ nativeWindowPicker CONSTANT)
  Q_PROPERTY(QString pickMessage READ pickMessage NOTIFY changed)
public:
  Controller(QString directory, QByteArray format, QString themePath,
             QObject *parent = nullptr, CaptureBackend *capture = nullptr,
             WindowBackend *windows = nullptr, SurfaceBarrier *barrier = nullptr,
             WindowPicker *picker = nullptr);
  ~Controller() override;
  QString error() const { return m_error; }
  bool busy() const { return m_busy; }
  QString mode() const { return m_mode; }
  bool live() const { return m_live; }
  bool canConfirm() const;
  QVariantList windows() const { return m_windows->windows(); }
  QString windowUnavailable() const { return m_windows->unavailableReason(); }
  QString selectedWindow() const { return m_windowId; }
  int previewRevision() const { return m_previewRevision; }
  bool desktopPicking() const { return m_desktopPicking; }
  bool nativeWindowPicker() const { return m_picker->nativePickerRequired(); }
  QString pickMessage() const { return m_pickMessage; }
  Q_INVOKABLE void setMode(const QString &mode);
  Q_INVOKABLE void setLive(bool live);
  Q_INVOKABLE void selectWindow(const QString &id);
  Q_INVOKABLE void pickWindowAt(qreal x, qreal y);
  Q_INVOKABLE void startWindowPick();
  Q_INVOKABLE void selectMonitor(int index);
  Q_INVOKABLE void confirm();
  Q_INVOKABLE void cancel();
public slots:
  Q_SCRIPTABLE QString Capture(const QString &mode);
  Q_SCRIPTABLE QString CaptureWithMode(const QString &mode, bool live);
  Q_SCRIPTABLE void Cancel();
  Q_SCRIPTABLE void Quit();
signals:
  void changed();

private:
  void showFrames(QVector<Frame> frames);
  QVector<Output> expectedLayout() const;
  void afterHidden();
  void hideOverlays();
  void restartSelection(const QString &mode, bool live);
  void save(const QImage &image);
  void release();
  void fail(const QString &message);
  void commit();
  void finishWindowPick(const QStringList &ids, const QString &message = {});
  void notify(const QString &title, const QString &body);
  QString m_directory, m_error, m_mode = "region";
  QString m_windowId, m_restartMode;
  QString m_pickMessage;
  QByteArray m_format;
  bool m_busy = false;
  bool m_live = false, m_restartLive = false;
  bool m_desktopPicking = true;
  enum class Phase { Idle, Preparing, Selecting, WindowPreview, VerifyingFrozen,
                     Hiding, CapturingLive, VerifyingLiveWindow, CapturingLiveWindow, Restarting,
                     PickingWindow, NativePickHiding, PickingNative };
  Phase m_phase = Phase::Idle;
  int m_previewRevision = 0;
  CaptureBackend *m_capture;
  WindowBackend *m_windows;
  SurfaceBarrier *m_barrier;
  WindowPicker *m_picker;
  Selection m_selection;
  Theme m_theme;
  FrameProvider *m_provider = nullptr;
  std::unique_ptr<QQmlEngine> m_engine;
  std::vector<std::unique_ptr<QQuickView>> m_views;
};

} // namespace snip
