#pragma once
#include <QObject>
#include <QJsonObject>
#include <QThread>
#include <atomic>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>

namespace snip {
class Uploader final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool available READ available NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QString destination READ destination WRITE setDestination NOTIFY changed)
  Q_PROPERTY(QVariantList destinations READ destinations NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QUrl url READ url NOTIFY changed)
public:
  explicit Uploader(QObject *parent = nullptr, QString program = {}, QString storage = {});
  ~Uploader() override;
  bool available() const;
  bool busy() const { return m_worker != nullptr; }
  QString destination() const { return m_selected; }
  QVariantList destinations() const;
  QString status() const { return m_status; }
  QString error() const { return m_error; }
  QUrl url() const { return m_url; }
  QString resolveDestination(const QString &id) const;
  Q_INVOKABLE QString destinationLabel(const QString &id) const;
  bool hasDestination(const QString &id) const { return !resolveDestination(id).isEmpty(); }
  void configure(const QString &destination, const QStringList &definitions);
  void setDestination(const QString &id);
  Q_INVOKABLE bool importDefinition(const QUrl &file);
  bool start(const QString &filename, const QString &destination, bool lens = false, int timeoutMs = 180000);
  Q_INVOKABLE void cancel();
signals:
  void changed();
  void completed(const QUrl &url, const QString &filename, const QString &destination, bool lens);
  void failed(const QString &message);
  void cancelled();
private:
  struct Destination {
    QString id, alias, label;
    QJsonObject definition;
    QUrl endpoint;
  };
  void addBuiltins();
  bool readDefinition(const QString &path, Destination *destination, QString *error) const;
  void cleanup();
  void fail(const QString &message);
  QThread *m_worker = nullptr;
  std::atomic_bool m_cancelled{false};
  quint64 m_generation = 0;
  QString m_program, m_storage, m_selected, m_status = "Idle", m_error;
  QString m_filename, m_destination;
  bool m_lens = false;
  QUrl m_url;
  QList<Destination> m_destinations;
  QTimer m_timeout;
  std::unique_ptr<QTemporaryDir> m_temporary;
  QByteArray m_output;
};
}
