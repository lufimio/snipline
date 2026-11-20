#include "windowpicker.h"
#include "core/windowtargets.h"
#include <QTimer>
#include <memory>

namespace snip {
void WindowPicker::pick(QPointF point, const QVariantList &catalog) {
  cancel();
  m_point = point;
  m_catalog = catalog;
  m_compositor = currentCompositor();
  m_native = false;
  if (m_compositor == Compositor::Unsupported) {
    emit failed("Click selection requires Hyprland or niri.");
    return;
  }
  next();
}

bool WindowPicker::nativePickerRequired() const {
  return currentCompositor() == Compositor::Niri;
}

void WindowPicker::pickNative(const QVariantList &catalog) {
  cancel();
  m_catalog = catalog;
  m_compositor = Compositor::Niri;
  m_native = true;
  next();
}

void WindowPicker::next() {
  const QStringList queries = m_native ? QStringList{"pick-window"} : m_compositor == Compositor::Hyprland
      ? QStringList{"monitors", "clients"} : QStringList{"outputs", "windows", "workspaces"};
  if (m_results.size() == queries.size()) {
    QString error;
    const auto hits = m_native ? niriPickedWindow(m_results[0], m_catalog, &error)
        : windowTargetsAt(m_compositor, m_point, m_catalog, m_results[0],
                          m_results[1], m_results.value(2), &error);
    m_catalog.clear();
    m_results.clear();
    if (!error.isEmpty()) emit failed(error);
    else emit picked(hits);
    return;
  }
  auto *process = new QProcess(this);
  m_process = process;
  auto bytes = std::make_shared<QByteArray>();
  auto error = std::make_shared<QString>();
  const auto query = queries[m_results.size()];
  connect(process, &QProcess::readyReadStandardOutput, this, [this, process, bytes, error] {
    const auto chunk = process->readAllStandardOutput();
    if (m_process != process) return;
    if (bytes->size() + chunk.size() > 4 * 1024 * 1024) {
      *error = "Window metadata exceeded the 4 MiB limit.";
      process->kill();
    } else bytes->append(chunk);
  });
  connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
  connect(process, &QProcess::finished, this, [this, process, bytes, error](int code, QProcess::ExitStatus status) {
    process->deleteLater();
    if (m_process != process) return;
    m_process = nullptr;
    if (status != QProcess::NormalExit || code != 0 || !error->isEmpty()) {
      emit failed(error->isEmpty() ? "Could not query compositor windows. Use the window list instead." : *error);
      return;
    }
    m_results.append(*bytes);
    next();
  });
  connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) return;
    process->deleteLater();
    if (m_process != process) return;
    m_process = nullptr;
    emit failed("Cannot start the compositor's window-query command.");
  });
  auto *timeout = new QTimer(process);
  timeout->setSingleShot(true);
  connect(timeout, &QTimer::timeout, process, [process, error] {
    *error = "Window picking timed out.";
    process->kill();
  });
  timeout->start(m_native ? 120000 : 3000);
  process->start(discoveryProgram(m_compositor),
                 m_compositor == Compositor::Hyprland ? QStringList{"-j", query}
                     : QStringList{"msg", "--json", query}, QIODevice::ReadOnly);
}

void WindowPicker::cancel() {
  if (auto *process = m_process.data()) { m_process = nullptr; process->kill(); }
  m_results.clear();
  m_catalog.clear();
}
}
