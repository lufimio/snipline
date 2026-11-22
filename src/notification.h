#pragma once
#include <QDBusMessage>
#include <QString>
#include <QObject>
#include <QMap>
#include <QUrl>
#include <QSet>

namespace snip {
QDBusMessage screenshotNotification(const QString &title, const QString &body,
                                  const QString &imagePath, int timeout,
                                  const QStringList &actions = {});

// Retain URLs per notification, independently of the current recognition result.
class NotificationActions final : public QObject {
  Q_OBJECT
public:
  explicit NotificationActions(QObject *parent = nullptr);
  void show(const QString &title, const QString &body, int timeout, const QUrl &url = {});
  void showSaved(const QString &title, const QString &body, int timeout,
                  const QString &filename, const QString &command, bool image,
                  const QString &upload = {}, const QString &lens = {}, const QUrl &remote = {});
  void showProgress(const QString &title, const QString &body, quint64 token);
  void closeProgress(quint64 token);
  void invalidate();
signals:
  void openRequested(const QUrl &url);
  void editRequested(const QString &filename, const QString &command);
  void fileOpenRequested(const QString &filename, bool folder);
  void uploadRequested(const QString &filename, const QString &destination, bool lens);
  void cancelRequested(quint64 token);
private slots:
  void actionInvoked(uint id, const QString &action);
  void notificationClosed(uint id, uint reason);
private:
  QMap<uint, QUrl> m_urls;
  struct SavedAction { QString filename, command, upload, lens; QUrl remote; };
  QMap<uint, SavedAction> m_saved;
  QMap<uint, quint64> m_progress;
  QSet<quint64> m_activeProgress;
  void send(const QDBusMessage &message, const QUrl &url,
            const SavedAction &saved = {});
  quint64 m_epoch = 0;
};
}
