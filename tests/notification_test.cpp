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
  QList<uint> closed;
public slots:

  uint Notify(const QString &, uint, const QString &, const QString &, const QString &body,
              const QStringList &buttons, const QVariantMap &, int) {
    actions.append(buttons);
    bodies.append(body);
    return next++;
  }

  void CloseNotification(uint id) {
    closed.append(id);
    emit NotificationClosed(id, 3);
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
    QVERIFY(
        bus.registerObject("/org/freedesktop/Notifications", &server,
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
    notifications.showSaved("Screenshot", "Saved", 5000, "/first.png", "satty --filename -", true);
    notifications.showSaved("Recording", "Saved", 5000, "/second.mp4", "losslesscut {}", false);
    QTRY_COMPARE(server.actions.size(), 10);
    QTest::qWait(30);
    QCOMPARE(server.actions[8],
             (QStringList{"open", "Open", "folder", "Open Folder", "edit", "Edit"}));
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
    QTRY_COMPARE(server.actions.size(), 11);
    QTest::qWait(30);
    QVERIFY(bus.unregisterService("org.freedesktop.Notifications"));
    QTest::qWait(30);
    QVERIFY(bus.registerService("org.freedesktop.Notifications"));
    QTest::qWait(30);
    emit server.ActionInvoked(11, "open"); // Never reuse actions from a previous server owner.
    QTest::qWait(30);
    QCOMPARE(opened.size(), 2);
    QSignalSpy files(&notifications, &snip::NotificationActions::fileOpenRequested);
    notifications.showSaved("Screenshot", "Saved", 5000, "/no-editor.png", {}, true);
    QTRY_COMPARE(server.actions.size(), 12);
    QTest::qWait(30);
    QCOMPARE(server.actions[11], (QStringList{"open", "Open", "folder", "Open Folder"}));
    emit server.ActionInvoked(12, "edit"); // Omitted actions cannot be forged.
    QTest::qWait(30);
    QCOMPARE(edits.size(), 1);
    emit server.ActionInvoked(12, "open");
    QTRY_COMPARE(files.size(), 1);
    QCOMPARE(files[0][0].toString(), "/no-editor.png");
    QVERIFY(!files[0][1].toBool());
    notifications.showSaved("Recording", "Saved", 5000, "/video.mp4", {}, false);
    QTRY_COMPARE(server.actions.size(), 13);
    QTest::qWait(30);
    emit server.ActionInvoked(13, "folder");
    QTRY_COMPARE(files.size(), 2);
    QCOMPARE(files[1][0].toString(), "/video.mp4");
    QVERIFY(files[1][1].toBool());
    notifications.showSaved("Screenshot", "Saved", 5000, "/locked.png", "editor {}", true);
    notifications.invalidate();
    QTRY_COMPARE(server.actions.size(), 14);
    QTest::qWait(30);
    emit server.ActionInvoked(14, "edit");
    QTest::qWait(30);
    QCOMPARE(edits.size(), 1);
    QSignalSpy transfers(&notifications, &snip::NotificationActions::uploadRequested);
    notifications.showSaved("Screenshot", "Saved", 5000, "/transfer.png", {}, true, "catbox");
    QTRY_COMPARE(server.actions.size(), 15);
    QTest::qWait(30);
    QVERIFY(server.actions[14].contains("upload"));
    QVERIFY(!server.actions[14].contains("lens"));
    QCOMPARE(transfers.size(), 0); // No confirmation or implicit upload on notification creation.
    emit server.ActionInvoked(15, "upload");
    QTRY_COMPARE(transfers.size(), 1);
    QCOMPARE(transfers[0][0].toString(), "/transfer.png");
    QCOMPARE(transfers[0][1].toString(), "catbox");
    QVERIFY(!transfers[0][2].toBool());
    QSignalSpy cancellations(&notifications, &snip::NotificationActions::cancelRequested);
    notifications.showProgress("Uploading", "Fixture", 77);
    notifications.closeProgress(77); // May finish before Notify replies.
    QTRY_COMPARE(server.actions.size(), 16);
    QTRY_VERIFY(server.closed.contains(16));
    emit server.ActionInvoked(16, "cancel");
    QTest::qWait(30);
    QCOMPARE(cancellations.size(), 0);
    notifications.showProgress("Uploading", "Fixture", 78);
    QTRY_COMPARE(server.actions.size(), 17);
    QTest::qWait(30);
    emit server.ActionInvoked(17, "cancel");
    QTRY_COMPARE(cancellations.size(), 1);
    QCOMPARE(cancellations[0][0].toULongLong(), quint64(78));
    notifications.show("Upload complete", "Link copied", 5000, first);
    notifications.show("Google Lens", "Image uploaded", 5000, second);
    QTRY_COMPARE(server.actions.size(), 19);
    QCOMPARE(server.actions[17], (QStringList{"open", "Open"}));
    QCOMPARE(server.actions[18], (QStringList{"open", "Open"}));
  }
};
QTEST_GUILESS_MAIN(NotificationTest)
#include "notification_test.moc"
