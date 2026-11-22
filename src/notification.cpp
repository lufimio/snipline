#include "notification.h"
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QUrl>
#include <QVariantMap>

namespace snip {
QDBusMessage screenshotNotification(const QString &title, const QString &body,
                                    const QString &imagePath, int timeout,
                                    const QStringList &actions) {
  QVariantMap hints;
  // Saved files persist after overlays close and can be loaded asynchronously.
  const auto uri =
      imagePath.isEmpty()
          ? QString{}
          : QUrl::fromLocalFile(imagePath).toString(QUrl::FullyEncoded);
  if (!uri.isEmpty())
    hints.insert("image-path", uri);
  auto message = QDBusMessage::createMethodCall(
      "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", "Notify");
  message.setArguments({QStringLiteral("Snipline"), uint(0),
                        uri.isEmpty() ? QStringLiteral("camera-photo") : uri,
                        title, body.toHtmlEscaped(), actions, hints, timeout});
  return message;
}

NotificationActions::NotificationActions(QObject *parent) : QObject(parent) {
  auto bus = QDBusConnection::sessionBus();
  bus.connect("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
              "org.freedesktop.Notifications", "ActionInvoked", this,
              SLOT(actionInvoked(uint, QString)));
  bus.connect("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
              "org.freedesktop.Notifications", "NotificationClosed", this,
              SLOT(notificationClosed(uint, uint)));
  auto *watcher =
      new QDBusServiceWatcher("org.freedesktop.Notifications", bus,
                              QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
          [this](const QString &, const QString &oldOwner, const QString &) {
            if (!oldOwner.isEmpty())
              invalidate();
          });
}
void NotificationActions::invalidate() {
  ++m_epoch;
  m_urls.clear();
  m_editors.clear();
}
void NotificationActions::show(const QString &title, const QString &body,
                               int timeout, const QUrl &url) {
  const bool openable = url.isValid() && !url.host().isEmpty() &&
                        url.userInfo().isEmpty() &&
                        (url.scheme() == "https" || url.scheme() == "http");
  const auto message = screenshotNotification(
      title, body, {}, timeout,
      openable ? QStringList{"open", "Open"} : QStringList{});
  send(message, openable ? url : QUrl{});
}
void NotificationActions::showEditor(const QString &title, const QString &body, int timeout,
                                     const QString &filename, const QString &command, bool image) {
  const auto message = screenshotNotification(title, body, image ? filename : QString{}, timeout,
      command.trimmed().isEmpty() ? QStringList{} : QStringList{"edit", "Edit"});
  send(message, {}, filename, command);
}
void NotificationActions::send(const QDBusMessage &message, const QUrl &url,
                               const QString &filename, const QString &command) {
  const auto epoch = m_epoch;
  auto *pending = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(message), this);
  connect(pending, &QDBusPendingCallWatcher::finished, this,
           [this, epoch, url, filename, command](QDBusPendingCallWatcher *call) {
            const QDBusPendingReply<uint> reply = *call;
            if (epoch == m_epoch && !reply.isError()) {
              m_urls.remove(reply.value());
              m_editors.remove(reply.value());
              if (m_urls.size() + m_editors.size() >= 128) {
                if (!m_urls.isEmpty()) m_urls.erase(m_urls.begin());
                else m_editors.erase(m_editors.begin());
              }
              if (!url.isEmpty()) {
                m_urls.insert(reply.value(), url);
              } else if (!command.trimmed().isEmpty()) {
                m_editors.insert(reply.value(), {filename, command});
              }
            }
            call->deleteLater();
          });
}
void NotificationActions::actionInvoked(uint id, const QString &action) {
  if (action == "edit" && m_editors.contains(id)) {
    const auto editor = m_editors.take(id);
    emit editRequested(editor.first, editor.second);
    return;
  }
  if (action != "open" || !m_urls.contains(id))
    return;
  emit openRequested(m_urls.take(id));
}
void NotificationActions::notificationClosed(uint id, uint reason) {
  Q_UNUSED(reason)
  m_urls.remove(id);
  m_editors.remove(id);
}
} // namespace snip
