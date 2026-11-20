#include "notification.h"
#include <QUrl>
#include <QVariantMap>

namespace snip {
QDBusMessage screenshotNotification(const QString &title, const QString &body,
                                  const QString &imagePath, int timeout) {
  QVariantMap hints;
  // Saved files persist after overlays close and can be loaded asynchronously.
  const auto uri = imagePath.isEmpty() ? QString{} : QUrl::fromLocalFile(imagePath).toString(QUrl::FullyEncoded);
  if (!uri.isEmpty()) hints.insert("image-path", uri);
  auto message = QDBusMessage::createMethodCall(
      "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", "Notify");
  message.setArguments({QStringLiteral("Snipline"), uint(0),
      uri.isEmpty() ? QStringLiteral("camera-photo") : uri,
      title, body.toHtmlEscaped(), QStringList{}, hints, timeout});
  return message;
}
}
