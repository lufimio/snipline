#include "selection.h"

#include <cmath>

namespace snip {

void Selection::setBounds(QRectF bounds) {
  m_bounds = bounds;
  setRect(m_rect);
}

void Selection::setRect(QRectF rect) {
  m_rect = rect.normalized().intersected(m_bounds);
  emit changed();
}

void Selection::clear() {
  m_drag = Drag::None;
  m_rect = {};
  emit changed();
}

QPointF Selection::bounded(QPointF point) const {
  return {qBound(m_bounds.left(), point.x(), m_bounds.right()),
          qBound(m_bounds.top(), point.y(), m_bounds.bottom())};
}

void Selection::begin(qreal x, qreal y) {
  m_start = bounded({x, y});
  m_initial = m_rect;
  const auto edges = resizeEdgesAt(m_start);
  m_left = edges.testFlag(Qt::LeftEdge);
  m_right = edges.testFlag(Qt::RightEdge);
  m_top = edges.testFlag(Qt::TopEdge);
  m_bottom = edges.testFlag(Qt::BottomEdge);
  if (valid() && (m_left || m_right || m_top || m_bottom)) {
    m_drag = Drag::Resize;
  } else if (valid() && m_rect.contains(m_start)) {
    m_drag = Drag::Move;
  } else {
    m_drag = Drag::Create;
    setRect(QRectF(m_start, m_start));
  }
}

void Selection::update(qreal x, qreal y) {
  const auto point = bounded({x, y});
  if (m_drag == Drag::Create) {
    setRect(QRectF(m_start, point));
  } else if (m_drag == Drag::Move) {
    const auto delta = point - m_start;
    auto rect = m_initial.translated(delta);
    rect.moveLeft(qBound(m_bounds.left(), rect.left(), m_bounds.right() - rect.width()));
    rect.moveTop(qBound(m_bounds.top(), rect.top(), m_bounds.bottom() - rect.height()));
    setRect(rect);
  } else if (m_drag == Drag::Resize) {
    auto rect = m_initial;
    if (m_left) {
      rect.setLeft(point.x());
    }
    if (m_right) {
      rect.setRight(point.x());
    }
    if (m_top) {
      rect.setTop(point.y());
    }
    if (m_bottom) {
      rect.setBottom(point.y());
    }
    setRect(rect);
  }
}

void Selection::end() {
  m_drag = Drag::None;
}

Qt::Edges Selection::resizeEdgesAt(QPointF point) const {
  constexpr qreal hitRadius = 10;
  if (!valid() || !m_rect.adjusted(-hitRadius, -hitRadius, hitRadius, hitRadius).contains(point)) {
    return {};
  }

  Qt::Edges edges;
  if (std::abs(point.x() - m_rect.left()) <= hitRadius) {
    edges |= Qt::LeftEdge;
  } else if (std::abs(point.x() - m_rect.right()) <= hitRadius) {
    edges |= Qt::RightEdge;
  }
  if (std::abs(point.y() - m_rect.top()) <= hitRadius) {
    edges |= Qt::TopEdge;
  } else if (std::abs(point.y() - m_rect.bottom()) <= hitRadius) {
    edges |= Qt::BottomEdge;
  }
  return edges;
}

int Selection::cursorShapeAt(qreal x, qreal y) const {
  const QPointF point(x, y);
  const auto edges = resizeEdgesAt(point);
  const bool horizontal = edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge);
  const bool vertical = edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge);

  if (horizontal && vertical) {
    const bool topLeftOrBottomRight = edges.testFlag(Qt::LeftEdge) == edges.testFlag(Qt::TopEdge);
    return topLeftOrBottomRight ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
  }
  if (horizontal) {
    return Qt::SizeHorCursor;
  }
  if (vertical) {
    return Qt::SizeVerCursor;
  }
  if (valid() && m_rect.contains(point)) {
    return Qt::SizeAllCursor;
  }
  return Qt::CrossCursor;
}

} // namespace snip
