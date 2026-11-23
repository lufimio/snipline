#pragma once

#include <QObject>
#include <QRectF>

namespace snip {

class Selection final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QRectF rect READ rect NOTIFY changed)
  Q_PROPERTY(bool valid READ valid NOTIFY changed)
public:
  explicit Selection(QObject *parent = nullptr) : QObject(parent) {}

  QRectF rect() const { return m_rect; }

  bool valid() const { return m_rect.width() >= 1 && m_rect.height() >= 1; }

  void setBounds(QRectF bounds);
  void setRect(QRectF rect);
  void clear();
  Q_INVOKABLE void begin(qreal x, qreal y);
  Q_INVOKABLE void update(qreal x, qreal y);
  Q_INVOKABLE void end();
  Q_INVOKABLE int cursorShapeAt(qreal x, qreal y) const;
signals:
  void changed();

private:
  enum class Drag { None, Create, Move, Resize };
  QPointF bounded(QPointF point) const;
  Qt::Edges resizeEdgesAt(QPointF point) const;
  QRectF m_bounds, m_rect, m_initial;
  QPointF m_start;
  Drag m_drag = Drag::None;
  bool m_left = false, m_right = false, m_top = false, m_bottom = false;
};

} // namespace snip
