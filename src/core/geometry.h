#pragma once

#include <QImage>
#include <QRectF>
#include <QString>
#include <QVector>

namespace snip {

// All rectangles use global logical coordinates and half-open edges.
// Frames are already transformed to display orientation by the capture backend.
struct Output {
  QString name;
  QRectF logical;
  qreal scale = 1;
  int transform = 0;
  bool operator==(const Output &) const = default;
};

struct Frame {
  Output output;
  QImage image;
};

struct Composition {
  QImage image;
  qreal scale = 1;
  QString error;
};

QRectF desktopBounds(const QVector<Output> &outputs);
Composition compose(const QVector<Frame> &frames, const QRectF &selection);

} // namespace snip
