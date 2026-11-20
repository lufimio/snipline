#pragma once
#include <QDBusMessage>
#include <QString>

namespace snip {
QDBusMessage screenshotNotification(const QString &title, const QString &body,
                                  const QString &imagePath, int timeout);
}
