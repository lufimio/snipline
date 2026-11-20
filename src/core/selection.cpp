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
  constexpr qreal radius = 10;
  const bool near =
      m_rect.adjusted(-radius, -radius, radius, radius).contains(m_start);
  m_left = near && std::abs(x - m_rect.left()) <= radius;
  m_right = near && !m_left && std::abs(x - m_rect.right()) <= radius;
  m_top = near && std::abs(y - m_rect.top()) <= radius;
  m_bottom = near && !m_top && std::abs(y - m_rect.bottom()) <= radius;
  if (valid() && (m_left || m_right || m_top || m_bottom))
    m_drag = Drag::Resize;
  else if (valid() && m_rect.contains(m_start))
    m_drag = Drag::Move;
  else {
    m_drag = Drag::Create;
    setRect(QRectF(m_start, m_start));
  }
}

void Selection::update(qreal x, qreal y) {
  const auto point = bounded({x, y});
  if (m_drag == Drag::Create)
    setRect(QRectF(m_start, point));
  else if (m_drag == Drag::Move) {
    const auto delta = point - m_start;
    auto rect = m_initial.translated(delta);
    rect.moveLeft(
        qBound(m_bounds.left(), rect.left(), m_bounds.right() - rect.width()));
    rect.moveTop(
        qBound(m_bounds.top(), rect.top(), m_bounds.bottom() - rect.height()));
    setRect(rect);
  } else if (m_drag == Drag::Resize) {
    auto rect = m_initial;
    if (m_left)
      rect.setLeft(point.x());
    if (m_right)
      rect.setRight(point.x());
    if (m_top)
      rect.setTop(point.y());
    if (m_bottom)
      rect.setBottom(point.y());
    setRect(rect);
  }
}

void Selection::end() { m_drag = Drag::None; }

} // namespace snip
