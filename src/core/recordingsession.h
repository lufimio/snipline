#pragma once
#include <QObject>
#include <QTimer>
#include <QElapsedTimer>

namespace snip {
// Backend-independent Milestone 2 lifecycle. Backend callbacks carry a generation
// so late portal/encoder replies cannot revive a discarded session.
class RecordingSession : public QObject {
  Q_OBJECT
public:
  enum class State { Idle, Selecting, Countdown, Preparing, Recording, Paused,
                     Finalizing, Saved, Failed };
  Q_ENUM(State)
  explicit RecordingSession(QObject *parent = nullptr);
  State state() const { return m_state; }
  quint64 generation() const { return m_generation; }
  QString target() const { return m_target; }
  QString error() const { return m_error; }
  QString savedPath() const { return m_path; }
  int countdownRemaining() const { return m_remaining; }
  bool pausePending() const { return m_pausePending; }
  qint64 elapsedMs() const;
  bool begin(QString target, int countdownSeconds, int maxDurationSeconds);
  bool confirm();
  void discard();
  void stop();
  void pause();
  void resume();
  void backendStarted(quint64 generation);
  void backendPaused(quint64 generation, bool paused);
  void backendSaved(quint64 generation, QString path);
  void backendFailed(quint64 generation, QString error);
signals:
  void changed();
  void prepareRequested(quint64 generation, QString target);
  void pauseRequested(quint64 generation, bool paused);
  void finalizeRequested(quint64 generation);
  // Backend must close portal/streams/encoder and remove temporary output.
  void abortRequested(quint64 generation);
private:
  void transition(State state);
  void prepare();
  void freezeElapsed();
  State m_state = State::Idle;
  quint64 m_generation = 0;
  QString m_target, m_error, m_path;
  int m_countdown = 0, m_remaining = 0, m_maxDuration = 3600;
  qint64 m_accumulated = 0;
  bool m_pausePending = false, m_backendActive = false;
  QElapsedTimer m_elapsed;
  QTimer m_countdownTimer, m_limitTimer, m_tickTimer, m_watchdog;
};
}
