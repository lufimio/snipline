#pragma once
#include <QObject>
#include <QVariantList>
#include <QProcess>
#include <QPointer>

namespace snip {
// Metadata only. Discovery never opens an audio recording stream.
class AudioSources : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  ~AudioSources() override;
  virtual void refresh();
  virtual QVariantList sources() const { return m_sources; }
  QString error() const { return m_error; }
  bool loading() const { return !m_process.isNull(); }
  void cancel();
  static QVariantList parse(const QByteArray &sources, const QByteArray &inputs, QString *error);
  static QStringList forWindow(const QVariantList &sources, const QVariantMap &window,
                               const QVariantList &windows);
  static QString streamIdentity(const QVariantMap &properties, quint32 client);
signals:
  void changed();
private:
  void query(bool inputs);
  QVariantList m_sources;
  QByteArray m_devices;
  QString m_error;
  QPointer<QProcess> m_process;
};
}
