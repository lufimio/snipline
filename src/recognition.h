#pragma once
#include <QObject>
#include <QImage>
#include <QProcess>
#include <QTimer>
#include <QVariantList>

namespace snip {
class Recognition final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QString kind READ kind NOTIFY changed)
  Q_PROPERTY(QString state READ state NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString text READ text WRITE setText NOTIFY changed)
  Q_PROPERTY(QVariantList codes READ codes NOTIFY changed)
  Q_PROPERTY(bool hasInput READ hasInput NOTIFY changed)
  Q_PROPERTY(QString sourceFile READ sourceFile NOTIFY changed)
  Q_PROPERTY(QStringList languages READ languages NOTIFY changed)
  Q_PROPERTY(QStringList selectedLanguages READ selectedLanguages NOTIFY changed)
public:
  explicit Recognition(QObject *parent = nullptr, QString worker = {}, QString tesseract = {});
  ~Recognition() override;
  bool busy() const { return m_busy; }
  QString kind() const { return m_kind; }
  QString state() const { return m_state; }
  QString error() const { return m_error; }
  QString text() const { return m_text; }
  QVariantList codes() const { return m_codes; }
  bool hasInput() const { return !m_image.isNull() || !m_file.isEmpty(); }
  QString sourceFile() const { return m_file; }
  QStringList languages() const { return m_languages; }
  QStringList selectedLanguages() const { return m_selectedLanguages; }
  void setPreferredLanguages(const QStringList &languages) { if (!m_busy) { m_selectedLanguages = languages; emit changed(); } }
  Q_INVOKABLE void toggleLanguage(const QString &language);
  Q_INVOKABLE void refreshLanguages();
  void startImage(const QString &kind, const QImage &image);
  void startFile(const QString &kind, const QString &file);
  void prepare(const QString &kind);
  void clear();
  void setText(const QString &text);
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void rerun();
signals:
  void changed();
  void finished(bool success);
private:
  void begin();
  void launch(const QString &program, const QStringList &args, qint64 outputLimit, int timeout);
  void pump();
  void drain();
  void processFinished(int code, QProcess::ExitStatus status);
  void complete(const QString &error = {});
  void clearProcess();
  QImage m_image;
  QString m_file, m_kind, m_state = "Idle", m_error, m_text, m_worker;
  QString m_tesseract;
  QStringList m_languages, m_selectedLanguages{"eng"};
  enum class Stage { Qr, Languages, Image, Ocr };
  Stage m_stage = Stage::Qr;
  bool m_runAfterDiscovery = false;
  QVariantList m_codes;
  bool m_busy = false, m_writeClosed = false;
  QProcess *m_process = nullptr;
  QTimer m_timeout;
  QList<QByteArray> m_input;
  int m_part = 0;
  qsizetype m_offset = 0;
  QByteArray m_output, m_diagnostic;
  qint64 m_outputLimit = 0;
};
}
