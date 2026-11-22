#pragma once

#include "capture.h"
#include "core/selection.h"
#include "core/theme.h"
#include "core/config.h"
#include "surfacebarrier.h"
#include "windows.h"
#include "windowpicker.h"
#include "recorder.h"
#include "core/recordingsession.h"
#include "audiosources.h"
#include "recognition.h"
#include "notification.h"
#include "uploader.h"
#include <QDBusObjectPath>
#include <QQuickImageProvider>
#include <QQuickView>
#include <QTimer>
#include <QSet>
#include <QPointer>
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
  Q_PROPERTY(int frameRevision READ frameRevision NOTIFY changed)
  Q_PROPERTY(bool canConfirm READ canConfirm NOTIFY changed)
  Q_PROPERTY(QVariantList windows READ windows NOTIFY changed)
  Q_PROPERTY(QString windowUnavailable READ windowUnavailable NOTIFY changed)
  Q_PROPERTY(QString selectedWindow READ selectedWindow NOTIFY changed)
  Q_PROPERTY(QRectF windowRect READ windowRect NOTIFY changed)
  Q_PROPERTY(QRectF hoveredWindowRect READ hoveredWindowRect NOTIFY changed)
  Q_PROPERTY(bool canRetarget READ canRetarget NOTIFY changed)
  Q_PROPERTY(bool nativeWindowPicker READ nativeWindowPicker CONSTANT)
  Q_PROPERTY(QString pickMessage READ pickMessage NOTIFY changed)
  Q_PROPERTY(QVariantMap keybindings READ keybindings NOTIFY changed)
  Q_PROPERTY(bool recording READ recording NOTIFY changed)
  Q_PROPERTY(QString recordingFormat READ recordingFormat WRITE setRecordingFormat NOTIFY changed)
  Q_PROPERTY(QString recordingCodec READ recordingCodec WRITE setRecordingCodec NOTIFY changed)
  Q_PROPERTY(QString recordingEncoder READ recordingEncoder WRITE setRecordingEncoder NOTIFY changed)
  Q_PROPERTY(QStringList recordingCodecs READ recordingCodecs NOTIFY changed)
  Q_PROPERTY(bool systemAudio READ systemAudio WRITE setSystemAudio NOTIFY changed)
  Q_PROPERTY(bool microphone READ microphone WRITE setMicrophone NOTIFY changed)
  Q_PROPERTY(QString recordingState READ recordingState NOTIFY changed)
  Q_PROPERTY(qint64 recordingElapsedMs READ recordingElapsedMs NOTIFY changed)
  Q_PROPERTY(int countdownRemaining READ countdownRemaining NOTIFY changed)
  Q_PROPERTY(bool recordingControls READ recordingControls NOTIFY changed)
  Q_PROPERTY(QString recordingResultAction READ recordingResultAction NOTIFY changed)
  Q_PROPERTY(bool pausePending READ pausePending NOTIFY changed)
  Q_PROPERTY(QString videoEditor READ videoEditor WRITE setVideoEditor NOTIFY changed)
  Q_PROPERTY(QString editor READ editor WRITE setEditor NOTIFY changed)
  Q_PROPERTY(QString action READ action NOTIFY changed)
  Q_PROPERTY(QObject *recognition READ recognition CONSTANT)
  Q_PROPERTY(QObject *upload READ upload CONSTANT)
  Q_PROPERTY(QString lensDestination READ lensDestination WRITE setLensDestination NOTIFY changed)
  Q_PROPERTY(QString lastCapture READ lastCapture NOTIFY changed)
  Q_PROPERTY(QString imageFormat READ imageFormat WRITE setImageFormat NOTIFY changed)
  Q_PROPERTY(QString saveDirectory READ saveDirectory WRITE setSaveDirectory NOTIFY changed)
  Q_PROPERTY(QString filenameTemplate READ filenameTemplate WRITE setFilenameTemplate NOTIFY changed)
  Q_PROPERTY(bool copyToClipboard READ copyToClipboard WRITE setCopyToClipboard NOTIFY changed)
  Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled NOTIFY changed)
  Q_PROPERTY(int recordingFps READ recordingFps WRITE setRecordingFps NOTIFY changed)
  Q_PROPERTY(int countdownSeconds READ countdownSeconds WRITE setCountdownSeconds NOTIFY changed)
  Q_PROPERTY(QString systemAudioDevice READ systemAudioDevice WRITE setSystemAudioDevice NOTIFY changed)
  Q_PROPERTY(QString microphoneDevice READ microphoneDevice WRITE setMicrophoneDevice NOTIFY changed)
  Q_PROPERTY(QVariantList audioSources READ audioSources NOTIFY changed)
  Q_PROPERTY(int selectedAudioCount READ selectedAudioCount NOTIFY changed)
  Q_PROPERTY(QString audioMessage READ audioMessage NOTIFY changed)
  Q_PROPERTY(bool audioLoading READ audioLoading NOTIFY changed)
public:
  Controller(QString directory, QByteArray format, QString themePath,
             QObject *parent = nullptr, CaptureBackend *capture = nullptr,
             WindowBackend *windows = nullptr, SurfaceBarrier *barrier = nullptr,
               WindowPicker *picker = nullptr, VideoRecorder *recorder = nullptr,
                AudioSources *audio = nullptr, Uploader *upload = nullptr);
  ~Controller() override;
  QString error() const { return m_error; }
  bool busy() const { return m_busy; }
  QString mode() const { return m_mode; }
  bool live() const { return m_live; }
  int frameRevision() const { return m_frameRevision; }
  bool canConfirm() const;
  QVariantList windows() const { return m_windows->windows(); }
  QString windowUnavailable() const { return m_windows->unavailableReason(); }
  QString selectedWindow() const { return m_windowId; }
  QRectF windowRect() const { return m_windowGeometry.value(m_windowId).toRectF(); }
  QRectF hoveredWindowRect() const { return m_windowGeometry.value(m_hoveredWindow).toRectF(); }
  bool canRetarget() const;
  bool nativeWindowPicker() const { return m_picker->nativePickerRequired(); }
  QString pickMessage() const { return m_pickMessage; }
  QVariantMap keybindings() const { return m_config.keybindings; }
  void configure(const Config &config) {
    m_config = config;
    m_recordDirectory = config.recordingDirectory;
    m_recognition.setPreferredLanguages(config.ocrLanguages);
    m_upload->configure(config.uploadDestination, config.uploaders);
    emit changed();
  }
  bool recording() const { return m_recording; }
  QString recordingFormat() const { return m_recordOptions.format; }
  QString recordingCodec() const { return m_recordOptions.codec; }
  QString recordingEncoder() const { return m_recordOptions.encoder; }
  QStringList recordingCodecs() const { return VideoRecorder::codecsForFormat(recordingFormat()); }
  void setRecordingCodec(const QString &codec);
  void setRecordingEncoder(const QString &encoder);
  bool systemAudio() const { return m_recordOptions.systemAudio; }
  bool microphone() const { return m_recordOptions.microphone; }
  QString recordingState() const;
  qint64 recordingElapsedMs() const { return m_recordSession.elapsedMs(); }
  int countdownRemaining() const { return m_recordSession.countdownRemaining(); }
  bool recordingControls() const { return m_recordControls; }
  QString recordingResultAction() const { return m_postRecordingAction.isEmpty() ? QStringLiteral("save") : m_postRecordingAction; }
  bool pausePending() const { return m_recordSession.pausePending(); }
  QString videoEditor() const { return m_config.videoEditor; }
  QString editor() const { return m_recording ? m_config.videoEditor : m_config.screenshotEditor; }
  void setEditor(const QString &command);
  void setVideoEditor(const QString &command);
  Q_INVOKABLE void confirmWithAction(const QString &action);
  Q_INVOKABLE void stopWithAction(const QString &action);
  void setRecordingFormat(const QString &format);
  void setSystemAudio(bool enabled);
  void setMicrophone(bool enabled);
  QString action() const { return m_recording ? "record" : m_recognitionAction.isEmpty() ? "capture" : m_recognitionAction; }
  QString lastCapture() const { return m_lastCapture; }
  QObject *recognition() { return &m_recognition; }
  QObject *upload() { return m_upload; }
  QString lensDestination() const { return m_upload->resolveDestination(m_config.lensDestination); }
  void setLensDestination(const QString &destination);
  Q_INVOKABLE void cancelUpload() { m_upload->cancel(); }
  Q_INVOKABLE void closeRecognition();
  Q_INVOKABLE bool copyRecognition(int index = -1);
  Q_INVOKABLE bool canOpenCode(int index) const;
  Q_INVOKABLE bool openCode(int index);
  QString imageFormat() const { return QString::fromLatin1(m_format); }
  QString saveDirectory() const { return m_recording ? m_recordDirectory : m_directory; }
  QString filenameTemplate() const { return m_config.filenameTemplate; }
  bool copyToClipboard() const { return m_config.clipboard; }
  bool notificationsEnabled() const { return m_config.notifications; }
  int recordingFps() const { return m_recordOptions.fps; }
  int countdownSeconds() const { return m_config.countdown; }
  QString systemAudioDevice() const { return m_recordOptions.systemAudioDevice; }
  QString microphoneDevice() const { return m_recordOptions.microphoneDevice; }
  Q_INVOKABLE void setAction(const QString &action);
  void setImageFormat(const QString &format);
  void setSaveDirectory(const QString &directory);
  void setFilenameTemplate(const QString &value);
  void setCopyToClipboard(bool enabled);
  void setNotificationsEnabled(bool enabled);
  void setRecordingFps(int fps);
  void setCountdownSeconds(int seconds);
  void setSystemAudioDevice(const QString &device);
  void setMicrophoneDevice(const QString &device);
  QVariantList audioSources() const;
  int selectedAudioCount() const { return selectedAudioIds().size(); }
  QString audioMessage() const;
  bool audioLoading() const { return m_audioSources->loading(); }
  Q_INVOKABLE void refreshAudioSources();
  Q_INVOKABLE void toggleAudioSource(const QString &id);
  Q_INVOKABLE void setMode(const QString &mode);
  Q_INVOKABLE void setLive(bool live);
  Q_INVOKABLE void selectWindow(const QString &id);
  Q_INVOKABLE void pickWindowAt(qreal x, qreal y);
  Q_INVOKABLE void hoverWindowAt(qreal x, qreal y);
  Q_INVOKABLE void clearWindowHover();
  Q_INVOKABLE void startWindowPick();
  Q_INVOKABLE void selectMonitor(int index);
  Q_INVOKABLE void confirm();
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void hideRecordingControls();
public slots:
  Q_SCRIPTABLE QString Capture(const QString &mode);
  Q_SCRIPTABLE QString CaptureWithMode(const QString &mode, bool live);
  Q_SCRIPTABLE QString Recognize(const QString &kind, const QString &target, const QString &file);
  Q_SCRIPTABLE QString RecognizeWithMode(const QString &kind, const QString &target, const QString &file, int live);
  Q_SCRIPTABLE QString Upload(const QString &file, const QString &destination);
  Q_SCRIPTABLE QString Lens(const QString &target, const QString &file, int live);
  Q_SCRIPTABLE void Cancel();
  Q_SCRIPTABLE void Quit();
  Q_SCRIPTABLE QString Record(const QString &mode, const QString &format, bool systemAudio, bool microphone);
  Q_SCRIPTABLE QString RecordConfigured(const QString &mode, const QString &format, int systemAudio, int microphone);
  Q_SCRIPTABLE QString RecordEncoding(const QString &mode, const QString &format, int systemAudio, int microphone,
                                     const QString &codec, const QString &encoder);
  Q_SCRIPTABLE QString Pause();
  Q_SCRIPTABLE QString Resume();
  Q_SCRIPTABLE QString TogglePause();
  Q_SCRIPTABLE QString Stop();
  Q_SCRIPTABLE QString Status() const;
  void SessionLocked();
  void ScreenSaverActive(bool active);
  void SessionPropertiesChanged(QString interface, QVariantMap changed, QStringList invalidated);
signals:
  void changed();

private:
  void showFrames(QVector<Frame> frames);
  QVector<Output> expectedLayout() const;
  void afterHidden();
  void hideOverlays();
  QString showRecordingControls();
  bool validateResultAction(const QString &action, bool video);
  void openResult(const QString &path, const QString &action, const QString &editorCommand = {},
                  const QString &uploadDestination = {});
  QString beginUpload(const QString &path, const QString &destination, bool lens);
  void notifySaved(const QString &title, const QString &body, const QString &path,
                   const QString &editorCommand, bool image);
  void restartSelection(const QString &mode, bool live);
  void refreshFrozenFrames(QVector<Frame> frames);
  void restoreTransitionSurfaces();
  void save(const QImage &image);
  void recognitionFinished(bool success);
  QUrl codeUrl(int index) const;
  void release();
  void fail(const QString &message);
  void commit();
  void finishWindowPick(const QStringList &ids, const QString &message = {});
  void refreshWindowGeometry();
  void updateHoveredWindow();
  void notify(const QString &title, const QString &body, const QString &imagePath = {});
  void acquireRecordingFrame();
  void recordingFrame(const QImage &image);
  void stopRecordingCapture();
  void recordingSourceFailed(const QString &message);
  void watchSessionLock();
  bool recordingOptionsEditable() const;
  bool selectionOptionsEditable() const;
  QStringList selectedAudioIds() const;
  void beginAudioSelection();
  void autoSelectWindowAudio(bool retarget = false);
  void updateRecordingAudio();
  AudioSources *m_audioSources;
  Uploader *m_upload;
  QTimer m_audioTimer;
  bool m_queryAudio = false, m_audioUserEdited = false;
  QStringList m_manualAudioIds, m_autoAudioIds, m_suppressedAudioIds;
  QMap<QString, QVariantMap> m_knownAudioSources;
  QString m_autoAudioMessage;
  RecordingSession m_recordSession;
  VideoRecorder *m_recorder;
  RecordingOptions m_recordOptions;
  QTimer m_recordCaptureTimer;
  bool m_recording = false, m_recordAcquiring = false, m_encoderStarted = false;
  bool m_quitAfterRecording = false;
  bool m_sessionLocked = false;
  bool m_recordControls = false;
  QString m_captureDisposition = "capture", m_postRecordingAction;
  QString m_postUploadDestination, m_uploadEditor;
  bool m_uploadImage = false;
  quint64 m_uploadToken = 0;
  Recognition m_recognition;
  NotificationActions m_recognitionNotifications;
  NotificationActions m_savedNotifications;
  QString m_recognitionAction, m_lastCapture;
  bool m_captureLive = false;
  int m_frameRevision = 0;
  quint64 m_transitionEpoch = 0;
  QTimer m_transitionTimeout;
  QSet<QQuickWindow *> m_pendingRenders;
  QVector<QMetaObject::Connection> m_renderConnections;
  QVector<QPair<QPointer<QQuickWindow>, qreal>> m_transitionSurfaces;
  QString m_recordOutput, m_recordWarning, m_lastRecordingPath, m_lastRecordingError;
  QRectF m_recordRegion;
  Config m_config;
  QString m_directory, m_recordDirectory, m_error, m_mode = "region";
  QString m_windowId;
  QString m_pickMessage;
  QString m_hoveredWindow;
  QPointF m_hoverPoint;
  bool m_hoverActive = false;
  QByteArray m_format;
  bool m_busy = false;
  bool m_live = false;
  enum class Phase { Idle, Preparing, Selecting, FreezingWindow, VerifyingFrozen,
                      Hiding, CapturingLive, VerifyingLiveWindow, CapturingLiveWindow, RefreshingFrozen,
                      PickingWindow, NativePickHiding, PickingNative,
                      RecordingHiding, RecordVerifying, Recording };
  Phase m_phase = Phase::Idle;
  QVariantMap m_windowGeometry;
  QTimer m_geometryTimer;
  QTimer m_hoverTimer;
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
