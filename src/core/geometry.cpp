#include "geometry.h"

#include <QPainter>
#include <cmath>

namespace snip {

QRectF desktopBounds(const QVector<Output> &outputs) {
  QRectF result;
  for (const auto &output : outputs)
    result = result.united(output.logical);
  return result;
}

Composition compose(const QVector<Frame> &frames, const QRectF &selection) {
  Composition result;
  if (!selection.isValid() || !std::isfinite(selection.x()) ||
      !std::isfinite(selection.y()) || !std::isfinite(selection.width()) ||
      !std::isfinite(selection.height())) {
    result.error = QStringLiteral("Select a non-empty region.");
    return result;
  }
  bool intersects = false;
  result.scale = 0;
  for (const auto &frame : frames) {
    if (!frame.output.logical.intersects(selection))
      continue;
    if (frame.image.isNull() || !frame.output.logical.isValid() ||
        !std::isfinite(frame.output.scale) || frame.output.scale <= 0) {
      result.error = QStringLiteral("An output frame is invalid.");
      return result;
    }
    intersects = true;
    result.scale = qMax(result.scale, frame.output.scale);
  }
  if (!intersects) {
    result.error =
        QStringLiteral("The selection does not intersect an output.");
    return result;
  }
  const double width = std::ceil(selection.width() * result.scale);
  const double height = std::ceil(selection.height() * result.scale);
  // Bound allocations, including malformed layouts and extreme selections.
  if (width > 32768 || height > 32768 || width * height > 100'000'000) {
    result.error =
        QStringLiteral("Selection exceeds the 100 megapixel export limit.");
    return result;
  }
  result.image = QImage(QSize(int(width), int(height)),
                        QImage::Format_ARGB32_Premultiplied);
  if (result.image.isNull()) {
    result.error = QStringLiteral("Not enough memory for this capture.");
    return result;
  }
  result.image.fill(Qt::transparent); // Preserve gaps between outputs.
  QPainter painter(&result.image);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  for (const auto &frame : frames) {
    const auto area = frame.output.logical.intersected(selection);
    if (area.isEmpty())
      continue;
    const auto &logical = frame.output.logical;
    // Use actual frame dimensions to absorb fractional-scale rounding.
    const qreal sx = frame.image.width() / logical.width();
    const qreal sy = frame.image.height() / logical.height();
    const QRectF source((area.x() - logical.x()) * sx,
                        (area.y() - logical.y()) * sy, area.width() * sx,
                        area.height() * sy);
    const QRectF target((area.x() - selection.x()) * result.scale,
                        (area.y() - selection.y()) * result.scale,
                        area.width() * result.scale,
                        area.height() * result.scale);
    painter.drawImage(target, frame.image, source);
  }
  return result;
}

} // namespace snip
