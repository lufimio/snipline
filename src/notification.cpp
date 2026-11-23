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
      imagePath.isEmpty() ? QString{} : QUrl::fromLocalFile(imagePath).toString(QUrl::FullyEncoded);
  if (!uri.isEmpty()) {
    hints.insert("image-path", uri);
  }
  auto message = QDBusMessage::createMethodCall("org.freedesktop.Notifications",
                                                "/org/freedesktop/Notifications",
                                                "org.freedesktop.Notifications", "Notify");
  message.setArguments({QStringLiteral("Snipline"), uint(0),
                        uri.isEmpty() ? QStringLiteral("camera-photo") : uri, title,
                        body.toHtmlEscaped(), actions, hints, timeout});
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
  auto *watcher = new QDBusServiceWatcher("org.freedesktop.Notifications", bus,
                                          QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
          [this](const QString &, const QString &oldOwner, const QString &) {
            if (!oldOwner.isEmpty()) {
              invalidate();
            }
          });
}

void NotificationActions::invalidate() {
  ++m_epoch;
  m_urls.clear();
  m_saved.clear();
  const auto tokens = m_activeProgress;
  for (const auto token : tokens) {
    closeProgress(token);
  }
}

void NotificationActions::show(const QString &title, const QString &body, int timeout,
                               const QUrl &url) {
  const bool openable = url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
                        (url.scheme() == "https" || url.scheme() == "http");
  const auto message = screenshotNotification(
      title, body, {}, timeout, openable ? QStringList{"open", "Open"} : QStringList{});
  send(message, openable ? url : QUrl{});
}

void NotificationActions::showSaved(const QString &title, const QString &body, int timeout,
                                    const QString &filename, const QString &command, bool image,
                                    const QString &upload) {
  QStringList actions{"open", "Open", "folder", "Open Folder"};
  // The desktop notification protocol has no disabled-action representation.
  if (!command.trimmed().isEmpty()) {
    actions += QStringList{"edit", "Edit"};
  }
  if (!upload.isEmpty()) {
    actions += QStringList{"upload", "Upload"};
  }
  const auto message =
      screenshotNotification(title, body, image ? filename : QString{}, timeout, actions);
  send(message, {}, {filename, command, upload});
}

void NotificationActions::send(const QDBusMessage &message, const QUrl &url,
                               const SavedAction &saved) {
  const auto epoch = m_epoch;
  auto *pending =
      new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
  connect(pending, &QDBusPendingCallWatcher::finished, this,
          [this, epoch, url, saved](QDBusPendingCallWatcher *call) {
            const QDBusPendingReply<uint> reply = *call;
            if (epoch == m_epoch && !reply.isError()) {
              m_urls.remove(reply.value());
              m_saved.remove(reply.value());
              if (m_urls.size() + m_saved.size() >= 128) {
                if (!m_urls.isEmpty()) {
                  m_urls.erase(m_urls.begin());
                } else {
                  m_saved.erase(m_saved.begin());
                }
              }
              if (!url.isEmpty()) {
                m_urls.insert(reply.value(), url);
              } else if (!saved.filename.isEmpty()) {
                m_saved.insert(reply.value(), saved);
              }
            }
            call->deleteLater();
          });
}

void NotificationActions::actionInvoked(uint id, const QString &action) {
  if (action == "cancel" && m_progress.contains(id)) {
    const auto token = m_progress.value(id);
    closeProgress(token);
    emit cancelRequested(token);
    return;
  }
  if (m_saved.contains(id)) {
    const auto saved = m_saved.value(id);
    if (action == "edit" && !saved.command.trimmed().isEmpty()) {
      m_saved.remove(id);
      emit editRequested(saved.filename, saved.command);
    } else if (action == "open" || action == "folder") {
      m_saved.remove(id);
      emit fileOpenRequested(saved.filename, action == "folder");
    } else if (action == "upload" && !saved.upload.isEmpty()) {
      m_saved.remove(id);
      emit uploadRequested(saved.filename, saved.upload, false);
    }
    return;
  }
  if (action != "open" || !m_urls.contains(id)) {
    return;
  }
  emit openRequested(m_urls.take(id));
}

void NotificationActions::notificationClosed(uint id, uint reason) {
  Q_UNUSED(reason)
  m_urls.remove(id);
  m_saved.remove(id);
  if (m_progress.contains(id)) {
    m_activeProgress.remove(m_progress.take(id));
  }
}

void NotificationActions::showProgress(const QString &title, const QString &body, quint64 token) {
  m_activeProgress.insert(token);
  const auto epoch = m_epoch;
  const auto message = screenshotNotification(title, body, {}, 0, {"cancel", "Cancel"});
  auto *pending =
      new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
  connect(pending, &QDBusPendingCallWatcher::finished, this,
          [this, epoch, token](QDBusPendingCallWatcher *call) {
            const QDBusPendingReply<uint> reply = *call;
            if (!reply.isError()) {
              m_progress.insert(reply.value(), token);
              if (epoch != m_epoch || !m_activeProgress.contains(token)) {
                closeProgress(token);
              }
            }
            call->deleteLater();
          });
}

void NotificationActions::closeProgress(quint64 token) {
  m_activeProgress.remove(token);
  for (auto it = m_progress.begin(); it != m_progress.end();) {
    if (it.value() != token) {
      ++it;
      continue;
    }
    auto message = QDBusMessage::createMethodCall(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "CloseNotification");
    message.setArguments({it.key()});
    QDBusConnection::sessionBus().asyncCall(message);
    it = m_progress.erase(it);
  }
}
} // namespace snip
