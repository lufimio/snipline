#include "windowtargets.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cmath>

namespace snip {
namespace {
bool pair(const QJsonValue &value, QPointF *point) {
  const auto array = value.toArray();
  if (array.size() != 2 || !array[0].isDouble() || !array[1].isDouble()) return false;
  *point = {array[0].toDouble(), array[1].toDouble()};
  return std::isfinite(point->x()) && std::isfinite(point->y());
}
QString decimalId(const QJsonValue &value) {
  // Qt 6 retains JSON integers without a double roundtrip.
  if (!value.isDouble() || value.toInteger(-1) < 0) return {};
  return QString::number(value.toInteger());
}
QString address(const QString &value) {
  bool ok = false;
  const auto number = value.toULongLong(&ok, 16);
  return ok && number ? QString::number(number, 16) : QString{};
}
}

QStringList windowTargetsAt(Compositor compositor, QPointF point,
                           const QVariantList &catalog, const QByteArray &outputsJson,
                           const QByteArray &windowsJson, const QByteArray &workspacesJson,
                           QString *error) {
  if (error) error->clear();
  Q_UNUSED(workspacesJson);
  auto fail = [error](const QString &message) -> QStringList {
    if (error) *error = message;
    return {};
  };
  if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return fail("Invalid click position.");
  QJsonParseError parse;
  const auto windows = QJsonDocument::fromJson(windowsJson, &parse);
  if (parse.error != QJsonParseError::NoError || !windows.isArray())
    return fail("Invalid compositor window metadata. Use the window list instead.");
  const auto outputs = QJsonDocument::fromJson(outputsJson, &parse);
  if (parse.error != QJsonParseError::NoError)
    return fail("Invalid compositor output metadata.");
  if (compositor == Compositor::Hyprland) {
    if (!outputs.isArray()) return fail("Invalid Hyprland monitor metadata.");
    QSet<qint64> visible;
    bool onOutput = false, mappingAvailable = false;
    for (const auto &value : outputs.array()) {
      const auto monitor = value.toObject();
      if (monitor.value("disabled").toBool()) continue;
      visible.insert(monitor.value("activeWorkspace").toObject().value("id").toInteger(-1));
      const auto special = monitor.value("specialWorkspace").toObject().value("id").toInteger(0);
      if (special) visible.insert(special);
    }
    QString layoutError;
    const auto layout = parseOutputs(compositor, outputsJson, &layoutError);
    if (!layoutError.isEmpty()) return fail(layoutError);
    for (const auto &output : layout) onOutput |= output.logical.contains(point);
    if (!onOutput) return {};
    for (const auto &entry : catalog) mappingAvailable |= !entry.toMap().value("address").toString().isEmpty();
    if (!mappingAvailable)
      return fail("Click selection requires Hyprland's toplevel-mapping protocol. Use the window list instead.");
    QSet<qint64> fullscreenWorkspaces, specialWorkspaces;
    for (const auto &value : outputs.array()) {
      const auto special = value.toObject().value("specialWorkspace").toObject().value("id").toInteger(0);
      if (special) specialWorkspaces.insert(special);
    }
    for (const auto &value : windows.array()) {
      const auto window = value.toObject();
      if (window.value("mapped").toBool() && !window.value("hidden").toBool()
          && window.value("fullscreen").toInt() > 0)
        fullscreenWorkspaces.insert(window.value("workspace").toObject().value("id").toInteger(-2));
    }
    QString topAddress;
    int topLayer = -1;
    for (const auto &value : windows.array()) {
      const auto window = value.toObject();
      const auto workspace = window.value("workspace").toObject().value("id").toInteger(-2);
      const bool floating = window.value("floating").toBool();
      const bool pinned = floating && window.value("pinned").toBool();
      if (!window.value("mapped").toBool() || window.value("hidden").toBool()
          || (window.contains("visible") && !window.value("visible").toBool())
          || (window.contains("acceptsInput") && !window.value("acceptsInput").toBool())
          || (!pinned && !visible.contains(workspace))) continue;
      QPointF position, size;
      if (!pair(window.value("at"), &position) || !pair(window.value("size"), &size)
          || size.x() <= 0 || size.y() <= 0
          || !QRectF(position, QSizeF(size.x(), size.y())).contains(point)) continue;
      const auto key = address(window.value("address").toString());
      if (key.isEmpty()) continue;
      const bool aboveFullscreen = window.value("allowedOverFullscreen").toBool()
          || window.value("overFullscreen").toBool();
      int layer = window.value("fullscreen").toInt() > 0 ? 20
          : floating ? (fullscreenWorkspaces.contains(workspace) && !aboveFullscreen ? 10 : 30) : 0;
      if (specialWorkspaces.contains(workspace)) layer += 50;
      if (pinned) layer = 100;
      if (layer >= topLayer) { topLayer = layer; topAddress = key; }
    }
    if (topAddress.isEmpty()) return {};
    for (const auto &entry : catalog) {
      const auto item = entry.toMap();
      if (address(item.value("address").toString()) == topAddress)
        return {item.value("id").toString()};
    }
    return fail("The topmost window is no longer capturable. Pick again or use the window list.");
  }
  return fail("Use niri's compositor-native window picker.");
}

QStringList niriPickedWindow(const QByteArray &json, const QVariantList &catalog, QString *error) {
  if (error) error->clear();
  if (json.trimmed() == "null") return {}; // Escape or empty-space click.
  QJsonParseError parse;
  const auto doc = QJsonDocument::fromJson(json, &parse);
  if (parse.error == QJsonParseError::NoError && doc.isObject()) {
    const auto id = decimalId(doc.object().value("id"));
    if (!id.isEmpty()) {
      for (const auto &entry : catalog) {
        const auto item = entry.toMap();
        if (item.value("identifier").toString() == id)
          return {item.value("id").toString()};
      }
    }
  }
  if (error) *error = "The picked window could not be matched to a retained capture handle. Pick again or use the list.";
  return {};
}
}
