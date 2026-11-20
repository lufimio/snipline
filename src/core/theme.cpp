#include "theme.h"

#include <QColor>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

namespace snip {

Theme::Theme(QObject *parent)
    : QObject(parent), m_tokens{{"background", "#20232b"},
                                {"foreground", "#f3f4f6"},
                                {"accent", "#83b4ff"},
                                {"dimColor", "#000000"},
                                {"dimOpacity", 0.45},
                                {"windowDimColor", "#000000"},
                                {"windowDimOpacity", 0.25},
                                {"monitorDimColor", "#000000"},
                                {"monitorDimOpacity", 0.25},
                                {"fontFamily", "sans-serif"},
                                {"fontSize", 14},
                                {"spacing", 10},
                                {"radius", 12},
                                {"outlineWidth", 2},
                                {"handleSize", 8}} {}

bool Theme::load(const QByteArray &json, QString *error) {
  auto fail = [error](const QString &message) {
    if (error)
      *error = message;
    return false;
  };
  if (json.size() > 64 * 1024)
    return fail(QStringLiteral("Theme exceeds 64 KiB."));
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(json, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject())
    return fail(QStringLiteral("Theme must be a JSON object."));
  auto next = m_tokens;
  const auto object = document.object();
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (!next.contains(it.key()))
      return fail(QStringLiteral("Unknown theme token: %1").arg(it.key()));
    const auto &key = it.key();
    const auto value = it.value();
    if (key == "background" || key == "foreground" || key == "accent" ||
        key == "dimColor" || key == "windowDimColor" || key == "monitorDimColor") {
      if (!value.isString() || !QColor::isValidColorName(value.toString()))
        return fail(QStringLiteral("Invalid color: %1").arg(key));
    } else if (key == "fontFamily") {
      if (!value.isString() || value.toString().isEmpty() ||
          value.toString().size() > 128)
        return fail(QStringLiteral("Invalid fontFamily."));
    } else {
      const double number = value.toDouble(-1);
      double min = 0, max = 64;
      if (key == "dimOpacity" || key == "windowDimOpacity" || key == "monitorDimOpacity")
        max = 1;
      if (key == "fontSize") {
        min = 8;
        max = 32;
      }
      if (key == "outlineWidth") {
        min = 1;
        max = 8;
      }
      if (key == "handleSize") {
        min = 4;
        max = 24;
      }
      if (!value.isDouble() || !std::isfinite(number) || number < min ||
          number > max)
        return fail(QStringLiteral("Out-of-range number: %1").arg(key));
    }
    next[key] = value.toVariant();
  }
  m_tokens = next;
  emit changed();
  return true;
}

} // namespace snip
