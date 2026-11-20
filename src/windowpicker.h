#pragma once
#include <QObject>
#include <QPointF>
#include <QVariantList>
#include <QPointer>
#include <QProcess>
#include "core/outputs.h"

namespace snip {
class WindowPicker : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  virtual void pick(QPointF point, const QVariantList &catalog);
  virtual void pickNative(const QVariantList &catalog);
  virtual bool nativePickerRequired() const;
  virtual void cancel();
signals:
  void picked(QStringList ids);
  void failed(QString message);
private:
  void next();
  QPointF m_point;
  QVariantList m_catalog;
  Compositor m_compositor = Compositor::Unsupported;
  QList<QByteArray> m_results;
  QPointer<QProcess> m_process;
  bool m_native = false;
};
}
