#pragma once

#include <QImage>
#include <QObject>
#include <QVariantList>
#include <memory>

namespace snip {

// No window geometry is required. Handles remain alive for the whole session,
// avoiding ambiguous title matching and recycled compositor addresses.
class WindowBackend : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  virtual void start() = 0;
  virtual void capture(const QString &id) = 0;
  virtual void cancelCapture() = 0;
  virtual void stop() = 0;
  virtual QVariantList windows() const = 0;
  virtual QString unavailableReason() const = 0;
signals:
  void changed();
  void imageReady(QString id, QImage image);
  void closed(QString id);
  void failed(QString message);
};

class WaylandWindows final : public WindowBackend {
  Q_OBJECT
public:
  explicit WaylandWindows(QObject *parent = nullptr);
  ~WaylandWindows() override;
  void start() override;
  void capture(const QString &id) override;
  void cancelCapture() override;
  void stop() override;
  QVariantList windows() const override;
  QString unavailableReason() const override;
private:
  struct Private;
  std::unique_ptr<Private> d;
};

// Shared-memory validation/conversion is independently testable without pixels
// from a compositor. Only the two baseline Wayland SHM formats are accepted.
QImage copyWindowBuffer(const uchar *data, quint32 format, quint32 width,
                        quint32 height, quint32 stride, bool inverted);
bool validWindowBuffer(quint32 format, quint32 width, quint32 height, quint32 stride);

}
