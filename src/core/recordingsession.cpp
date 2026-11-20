#include "recordingsession.h"

namespace snip {
RecordingSession::RecordingSession(QObject *parent) : QObject(parent) {
  m_watchdog.setSingleShot(true);
  connect(&m_watchdog, &QTimer::timeout, this, [this] {
    backendFailed(m_generation, "Recording backend timed out.");
  });
  m_countdownTimer.setInterval(1000);
  connect(&m_countdownTimer, &QTimer::timeout, this, [this] {
    if (--m_remaining <= 0) {
      m_countdownTimer.stop();
      prepare();
    } else emit changed();
  });
  m_limitTimer.setSingleShot(true);
  connect(&m_limitTimer, &QTimer::timeout, this, &RecordingSession::stop);
  m_tickTimer.setInterval(250);
  connect(&m_tickTimer, &QTimer::timeout, this, &RecordingSession::changed);
}
void RecordingSession::transition(State state) { m_state = state; emit changed(); }
bool RecordingSession::begin(QString target, int countdown, int maxDuration) {
  if (m_state != State::Idle && m_state != State::Saved && m_state != State::Failed)
    return false;
  if ((target != "region" && target != "monitor" && target != "window") ||
      countdown < 0 || countdown > 30 || maxDuration < 1 || maxDuration > 86400)
    return false;
  ++m_generation;
  m_target = target;
  m_countdown = countdown;
  m_maxDuration = maxDuration;
  m_accumulated = 0;
  m_elapsed.invalidate();
  m_error.clear();
  m_path.clear();
  m_pausePending = false;
  m_remaining = 0;
  transition(State::Selecting);
  return true;
}
bool RecordingSession::confirm() {
  if (m_state != State::Selecting) return false;
  m_remaining = m_countdown;
  if (m_remaining) {
    m_countdownTimer.start();
    transition(State::Countdown);
  } else prepare();
  return true;
}
void RecordingSession::prepare() {
  // Preparation begins only after confirmation and countdown; the integration
  // must hide overlays and establish the display barrier before starting pixels.
  m_backendActive = true;
  m_watchdog.start(30000);
  transition(State::Preparing);
  emit prepareRequested(m_generation, m_target);
}
void RecordingSession::backendStarted(quint64 generation) {
  if (generation != m_generation || m_state != State::Preparing) return;
  m_watchdog.stop();
  m_elapsed.start();
  m_limitTimer.start(m_maxDuration * 1000); // Includes pauses: bounded wall-clock session.
  m_tickTimer.start();
  transition(State::Recording);
}
qint64 RecordingSession::elapsedMs() const {
  return m_accumulated + (m_elapsed.isValid() ? m_elapsed.elapsed() : 0);
}
void RecordingSession::freezeElapsed() {
  if (m_elapsed.isValid()) m_accumulated += m_elapsed.elapsed();
  m_elapsed.invalidate();
}
void RecordingSession::pause() {
  if (m_state != State::Recording || m_pausePending) return;
  m_pausePending = true;
  m_watchdog.start(5000);
  emit pauseRequested(m_generation, true);
}
void RecordingSession::resume() {
  if (m_state != State::Paused || m_pausePending) return;
  m_pausePending = true;
  m_watchdog.start(5000);
  emit pauseRequested(m_generation, false);
}
void RecordingSession::backendPaused(quint64 generation, bool paused) {
  if (generation != m_generation || !m_pausePending ||
      (paused ? m_state != State::Recording : m_state != State::Paused)) return;
  m_pausePending = false;
  m_watchdog.stop();
  if (paused) freezeElapsed();
  else m_elapsed.start();
  transition(paused ? State::Paused : State::Recording);
}
void RecordingSession::stop() {
  if (m_state == State::Selecting || m_state == State::Countdown || m_state == State::Preparing) {
    discard();
    return;
  }
  if (m_state != State::Recording && m_state != State::Paused) return;
  freezeElapsed();
  m_tickTimer.stop();
  m_limitTimer.stop();
  m_pausePending = false;
  m_watchdog.start(60000);
  transition(State::Finalizing);
  emit finalizeRequested(m_generation); // Backend must drain EOS and publish atomically.
}
void RecordingSession::discard() {
  const auto old = m_generation++;
  const bool abort = m_backendActive;
  m_backendActive = false;
  m_countdownTimer.stop();
  m_limitTimer.stop();
  m_tickTimer.stop();
  m_watchdog.stop();
  freezeElapsed();
  m_remaining = 0;
  m_pausePending = false;
  m_path.clear();
  m_error.clear();
  transition(State::Idle);
  if (abort) emit abortRequested(old);
}
void RecordingSession::backendSaved(quint64 generation, QString path) {
  if (generation != m_generation || m_state != State::Finalizing) return;
  if (path.isEmpty()) { backendFailed(generation, "Encoder returned no saved output."); return; }
  m_backendActive = false;
  m_watchdog.stop();
  m_path = path;
  transition(State::Saved);
}
void RecordingSession::backendFailed(quint64 generation, QString error) {
  if (generation != m_generation || !m_backendActive) return;
  const auto old = m_generation++;
  m_backendActive = false;
  m_countdownTimer.stop();
  m_limitTimer.stop();
  m_tickTimer.stop();
  m_watchdog.stop();
  freezeElapsed();
  m_pausePending = false;
  m_error = error.isEmpty() ? "Recording backend failed." : error;
  transition(State::Failed);
  emit abortRequested(old);
}
}
