#include "outputs.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <cmath>

namespace snip {

Compositor currentCompositor() {
  const auto desktop =
      qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower().split(':');
  if (desktop.contains("hyprland"))
    return Compositor::Hyprland;
  if (desktop.contains("niri"))
    return Compositor::Niri;
  return Compositor::Unsupported;
}

QString compositorName(Compositor compositor) {
  switch (compositor) {
  case Compositor::Hyprland:
    return QStringLiteral("Hyprland");
  case Compositor::Niri:
    return QStringLiteral("niri");
  default:
    return QStringLiteral("unsupported");
  }
}

QString discoveryProgram(Compositor compositor) {
  return compositor == Compositor::Hyprland ? QStringLiteral("hyprctl")
                                            : QStringLiteral("niri");
}

QStringList discoveryArguments(Compositor compositor) {
  return compositor == Compositor::Hyprland
             ? QStringList{"-j", "monitors"}
             : QStringList{"msg", "--json", "outputs"};
}

QVector<Output> parseOutputs(Compositor compositor, const QByteArray &json,
                             QString *error) {
  if (error)
    error->clear();
  auto fail = [error](const QString &message) -> QVector<Output> {
    if (error)
      *error = message;
    return {};
  };
  QJsonParseError parse;
  const auto doc = QJsonDocument::fromJson(json, &parse);
  if (parse.error != QJsonParseError::NoError)
    return fail(QStringLiteral("Invalid compositor output JSON."));
  QVector<Output> outputs;
  if (compositor == Compositor::Hyprland && doc.isArray()) {
    for (const auto &value : doc.array()) {
      const auto obj = value.toObject();
      if (obj.value("disabled").toBool())
        continue;
      for (const auto *key :
           {"x", "y", "width", "height", "scale", "transform"})
        if (!obj.value(key).isDouble())
          return fail(QStringLiteral("Missing numeric Hyprland output field."));
      const qreal scale = obj.value("scale").toDouble(0);
      const int transform = obj.value("transform").toInt(-1);
      qreal width = obj.value("width").toDouble();
      qreal height = obj.value("height").toDouble();
      if (transform % 2 == 1)
        qSwap(width, height);
      outputs.push_back(
          {obj.value("name").toString(),
           QRectF(obj.value("x").toDouble(), obj.value("y").toDouble(),
                  std::floor(width / scale), std::floor(height / scale)),
           scale, transform});
    }
  } else if (compositor == Compositor::Niri && doc.isObject()) {
    const auto object = doc.object();
    for (auto it = object.begin(); it != object.end(); ++it) {
      const auto obj = it.value().toObject();
      if (!obj.value("logical").isObject())
        continue; // Disabled output.
      const auto logical = obj.value("logical").toObject();
      for (const auto *key : {"x", "y", "width", "height", "scale"})
        if (!logical.value(key).isDouble())
          return fail(QStringLiteral("Missing numeric niri output field."));
      const QString transform = logical.value("transform")
                                    .toString()
                                    .toLower()
                                    .remove('_')
                                    .remove('-');
      const QMap<QString, int> transforms{{"normal", 0},     {"90", 1},
                                          {"180", 2},        {"270", 3},
                                          {"flipped", 4},    {"flipped90", 5},
                                          {"flipped180", 6}, {"flipped270", 7}};
      if (!transforms.contains(transform))
        return fail(QStringLiteral("Unknown niri output transform."));
      outputs.push_back(
          {it.key(),
           QRectF(logical.value("x").toDouble(), logical.value("y").toDouble(),
                  logical.value("width").toDouble(),
                  logical.value("height").toDouble()),
           logical.value("scale").toDouble(0), transforms.value(transform)});
    }
  } else {
    return fail(QStringLiteral("Unsupported compositor response."));
  }
  QSet<QString> names;
  for (const auto &output : outputs) {
    const auto &rect = output.logical;
    if (output.name.isEmpty() || names.contains(output.name) ||
        !rect.isValid() || !std::isfinite(rect.x()) ||
        !std::isfinite(rect.y()) || !std::isfinite(rect.width()) ||
        !std::isfinite(rect.height()) || !std::isfinite(output.scale) ||
        output.scale < 0.25 || output.scale > 8 || output.transform < 0 ||
        output.transform > 7)
      return fail(QStringLiteral("Invalid or duplicate compositor output."));
    names.insert(output.name);
  }
  if (outputs.isEmpty())
    return fail(QStringLiteral("No enabled outputs found."));
  return outputs;
}

} // namespace snip
