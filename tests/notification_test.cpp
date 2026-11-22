#include "notification.h"
#include <QDBusConnection>
#include <QSignalSpy>
#include <QtTest>

class FakeNotifications : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
public:
  uint next = 1;
  QList<QStringList> actions;
  QStringList bodies;
public slots:
  uint Notify(const QString &, uint, const QString &, const QString &, const QString &body,
              const QStringList &buttons, const QVariantMap &, int) {
    actions.append(buttons);
    bodies.append(body);
    return next++;
  }
signals:
  void ActionInvoked(uint id, const QString &action);
  void NotificationClosed(uint id, uint reason);
};

class NotificationTest : public QObject {
  Q_OBJECT
private slots:
  void actionsKeepOriginalUrlsAndRequireExplicitClick() {
    auto bus = QDBusConnection::sessionBus();
    QVERIFY(bus.isConnected());
    FakeNotifications server;
    QVERIFY(bus.registerObject("/org/freedesktop/Notifications", &server,
        QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
    QVERIFY(bus.registerService("org.freedesktop.Notifications"));
    const auto cleanup = qScopeGuard([&] {
      bus.unregisterService("org.freedesktop.Notifications");
      bus.unregisterObject("/org/freedesktop/Notifications");
    });
    snip::NotificationActions notifications;
    QSignalSpy opened(&notifications, &snip::NotificationActions::openRequested);
    const QUrl first("https://example.com/first"), second("https://example.com/second");
    notifications.show("QR", "Copied <link>", 5000, first);
    notifications.show("QR", "Copied another link", 5000, second);
    QTRY_COMPARE(server.actions.size(), 2);
    QTest::qWait(30); // Process Notify replies before simulating user actions.
    QCOMPARE(server.actions[0], (QStringList{"open", "Open"}));
    QCOMPARE(server.bodies[0], "Copied &lt;link&gt;");
    QCOMPARE(opened.size(), 0);
    emit server.ActionInvoked(999, "open");
    emit server.ActionInvoked(1, "unknown");
    QTest::qWait(30);
    QCOMPARE(opened.size(), 0);
    emit server.ActionInvoked(1, "open");
    QTRY_COMPARE(opened.size(), 1);
    QCOMPARE(opened[0][0].toUrl(), first);
    emit server.ActionInvoked(1, "open"); // Consumed; do not launch twice.
    emit server.NotificationClosed(2, 1);
    emit server.ActionInvoked(2, "open");
    QTest::qWait(30);
    QCOMPARE(opened.size(), 1);
    notifications.show("OCR", "Text copied", 5000);
    notifications.show("QR", "Non-web payload", 5000, QUrl("file:///etc/passwd"));
    notifications.show("QR", "Credentials", 5000, QUrl("https://user:password@example.com/"));
    notifications.show("QR", "Link", 5000, second);
    QTRY_COMPARE(server.actions.size(), 6);
    QTest::qWait(30);
    QVERIFY(server.actions[2].isEmpty());
    QVERIFY(server.actions[3].isEmpty());
    QVERIFY(server.actions[4].isEmpty());
    notifications.invalidate(); // Session lock invalidates old actions, including after unlock.
    emit server.ActionInvoked(6, "open");
    QTest::qWait(30);
    QCOMPARE(opened.size(), 1);
    notifications.show("QR", "New link", 5000, first);
    QTRY_COMPARE(server.actions.size(), 7);
    QTest::qWait(30);
    emit server.ActionInvoked(7, "open");
    QTRY_COMPARE(opened.size(), 2);
    QCOMPARE(opened[1][0].toUrl(), first);
    notifications.show("QR", "Pending link", 5000, second);
    notifications.invalidate(); // Lock before the asynchronous Notify reply arrives.
    QTRY_COMPARE(server.actions.size(), 8);
    QTest::qWait(30);
    emit server.ActionInvoked(8, "open");
    QTest::qWait(30);
    QCOMPARE(opened.size(), 2);
    QSignalSpy edits(&notifications, &snip::NotificationActions::editRequested);
    notifications.showEditor("Screenshot", "Saved", 5000, "/first.png", "satty --filename -", true);
    notifications.showEditor("Recording", "Saved", 5000, "/second.mp4", "losslesscut {}", false);
    QTRY_COMPARE(server.actions.size(), 10);
    QTest::qWait(30);
    QCOMPARE(server.actions[8], (QStringList{"edit", "Edit"}));
    QCOMPARE(edits.size(), 0);
    emit server.ActionInvoked(9, "edit");
    QTRY_COMPARE(edits.size(), 1);
    QCOMPARE(edits[0][0].toString(), "/first.png");
    QCOMPARE(edits[0][1].toString(), "satty --filename -");
    emit server.NotificationClosed(10, 1);
    emit server.ActionInvoked(10, "edit");
    QTest::qWait(30);
    QCOMPARE(edits.size(), 1);
    notifications.show("QR", "Old server link", 5000, first);
    QTRY_COMPARE(server.actions.size(), 9);
    QTest::qWait(30);
    QVERIFY(bus.unregisterService("org.freedesktop.Notifications"));
    QTest::qWait(30);
    QVERIFY(bus.registerService("org.freedesktop.Notifications"));
    QTest::qWait(30);
    emit server.ActionInvoked(9, "open"); // Never reuse actions from a previous server owner.
    QTest::qWait(30);
    QCOMPARE(opened.size(), 2);
  }
};
QTEST_GUILESS_MAIN(NotificationTest)
#include "notification_test.moc"
