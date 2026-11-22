#pragma once
#include <QDBusMessage>
#include <QString>
#include <QObject>
#include <QMap>
#include <QUrl>

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
  void showEditor(const QString &title, const QString &body, int timeout,
                  const QString &filename, const QString &command, bool image);
  void invalidate();
signals:
  void openRequested(const QUrl &url);
  void editRequested(const QString &filename, const QString &command);
private slots:
  void actionInvoked(uint id, const QString &action);
  void notificationClosed(uint id, uint reason);
private:
  QMap<uint, QUrl> m_urls;
  QMap<uint, QPair<QString, QString>> m_editors;
  void send(const QDBusMessage &message, const QUrl &url,
            const QString &filename = {}, const QString &command = {});
  quint64 m_epoch = 0;
};
}
