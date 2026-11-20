#include "windowpicker.h"
#include "core/windowtargets.h"
#include <QTimer>
#include <memory>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace snip {
void WindowPicker::pick(QPointF point, const QVariantList &catalog) {
  cancel();
  m_point = point;
  m_catalog = catalog;
  m_compositor = currentCompositor();
  m_native = false;
  m_refresh = false;
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
  m_refresh = false;
  next();
}

void WindowPicker::refresh(const QVariantList &catalog) {
  if (m_process) return; // Never interrupt an explicit target pick.
  cancel();
  m_catalog = catalog;
  m_compositor = currentCompositor();
  if (m_compositor == Compositor::Unsupported) return;
  m_native = false;
  m_refresh = true;
  next();
}

void WindowPicker::next() {
  const QStringList queries = m_native ? QStringList{"pick-window"} : m_compositor == Compositor::Hyprland
      ? QStringList{"monitors", "clients"} : QStringList{"outputs", "windows", "workspaces"};
  if (m_results.size() == queries.size()) {
    QString error;
    if (!m_native) {
      m_geometry = windowDisplayGeometry(m_compositor, m_catalog, m_results[0],
                                         m_results[1], m_results.value(2), &error);
      m_snapshotCatalog = m_catalog;
      m_snapshot = m_results;
      m_snapshotCompositor = error.isEmpty() ? m_compositor : Compositor::Unsupported;
      emit geometryChanged();
    }
    if (m_refresh) {
      m_results.clear();
      m_catalog.clear();
      return;
    }
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
      if (m_refresh) { m_geometry.clear(); m_snapshot.clear(); emit geometryChanged(); }
      else emit failed(error->isEmpty() ? "Could not query compositor windows. Use the window list instead." : *error);
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
    if (m_refresh) { m_geometry.clear(); m_snapshot.clear(); emit geometryChanged(); }
    else emit failed("Cannot start the compositor's window-query command.");
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

QString WindowPicker::hoveredAt(QPointF point) const {
  if (m_snapshot.size() < 2) return {};
  if (m_snapshotCompositor == Compositor::Hyprland) {
    QString error;
    return windowTargetsAt(m_snapshotCompositor, point, m_snapshotCatalog,
                           m_snapshot[0], m_snapshot[1], {}, &error).value(0);
  }
  // niri resolves actual overlap order in its native picker. Do not guess the
  // topmost hover when IPC exposes several tiles under the same point.
  if (m_snapshotCompositor == Compositor::Niri) {
    QString hit;
    for (auto it = m_geometry.begin(); it != m_geometry.end(); ++it) {
      if (!it.value().toRectF().contains(point)) continue;
      if (!hit.isEmpty()) return {};
      hit = it.key();
    }
    return hit;
  }
  return {};
}

QVariantMap WindowPicker::metadata(const QString &id) const {
  if (m_snapshot.size() < 2) return {};
  QVariantMap entry;
  for (const auto &value : m_snapshotCatalog)
    if (value.toMap().value("id").toString() == id) entry = value.toMap();
  if (entry.isEmpty()) return {};
  for (const auto &value : QJsonDocument::fromJson(m_snapshot[1]).array()) {
    const auto window = value.toObject();
    bool matches = false;
    if (m_snapshotCompositor == Compositor::Hyprland) {
      auto address = entry.value("address").toString();
      if (address.startsWith("0x")) address = address.mid(2);
      auto candidate = window["address"].toString();
      if (candidate.startsWith("0x")) candidate = candidate.mid(2);
      bool a = false, b = false;
      const auto left = address.toULongLong(&a, 16), right = candidate.toULongLong(&b, 16);
      matches = a && b && left == right;
    } else if (m_snapshotCompositor == Compositor::Niri) {
      matches = entry.value("identifier").toString() == window["id"].toVariant().toString();
    }
    if (matches) {
      entry.insert("pid", window["pid"].toVariant());
      return entry;
    }
  }
  return entry;
}
}
