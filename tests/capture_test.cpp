#include "capture.h"
#include "windowpicker.h"
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace snip;

class CaptureTest : public QObject {
    Q_OBJECT
    QByteArray m_path, m_desktop;
    QTemporaryDir m_dir;
    void script(const QString &name, const QByteArray &body)
    {
        QFile file(m_dir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("#!/bin/sh\n");
        file.write(body);
        file.close();
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner));
    }
    void validDiscovery()
    {
        script("hyprctl", "printf '%s' '[{\"name\":\"test\",\"x\":0,\"y\":0,\"width\":2,\"height\":2,\"scale\":1,\"transform\":0}]'\n");
    }
private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_path = qgetenv("PATH");
        m_desktop = qgetenv("XDG_CURRENT_DESKTOP");
        qputenv("PATH", m_dir.path().toUtf8()); // Only controlled test executables.
        qputenv("XDG_CURRENT_DESKTOP", "Hyprland");
    }
    void cleanupTestCase()
    {
        qputenv("PATH", m_path);
        qputenv("XDG_CURRENT_DESKTOP", m_desktop);
    }
    void frozenAcquisitionAndVerification()
    {
        validDiscovery();
        script("grim", "printf 'P3\\n2 2\\n255\\n255 0 0 255 0 0 255 0 0 255 0 0\\n'\n");
        GrimCapture capture;
        QSignalSpy ready(&capture, &CaptureBackend::ready);
        QSignalSpy failed(&capture, &CaptureBackend::failed);
        capture.start();
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
        const auto frames = qvariant_cast<QVector<Frame>>(ready.first().first());
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.first().image.pixelColor(0, 0), QColor(Qt::red));
        QSignalSpy verified(&capture, &CaptureBackend::layoutVerified);
        capture.verifyLayout({frames.first().output});
        QTRY_COMPARE(verified.count(), 1);
        validDiscovery();
        capture.verifyLayout({{"other", {0, 0, 2, 2}, 1}});
        QTRY_COMPARE(failed.count(), 1);
    }
    void processFailure()
    {
        validDiscovery();
        script("grim", "exit 1\n");
        GrimCapture capture;
        QSignalSpy ready(&capture, &CaptureBackend::ready);
        QSignalSpy failed(&capture, &CaptureBackend::failed);
        capture.start();
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(ready.count(), 0);
    }
    void invalidPixels()
    {
        validDiscovery();
        script("grim", "printf 'not pixels'\n");
        GrimCapture capture;
        QSignalSpy failed(&capture, &CaptureBackend::failed);
        capture.start();
        QTRY_COMPARE(failed.count(), 1);
    }
    void cancellationDiscardsPendingWork()
    {
        validDiscovery();
        GrimCapture capture;
        QSignalSpy ready(&capture, &CaptureBackend::ready);
        QSignalSpy failed(&capture, &CaptureBackend::failed);
        capture.start();
        capture.cancel();
        QTest::qWait(100);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 0);
    }
    void livePreparationDoesNotAcquirePixels()
    {
        validDiscovery();
        script("grim", "exit 99\n"); // Must never run during live preparation.
        GrimCapture capture;
        QSignalSpy ready(&capture, &CaptureBackend::ready);
        QSignalSpy failed(&capture, &CaptureBackend::failed);
        capture.startLive();
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
        const auto frames = qvariant_cast<QVector<Frame>>(ready.first().first());
        QCOMPARE(frames.size(), 1);
        QVERIFY(frames.first().image.isNull());
        // The final acquisition must reject a stale layout before pixels.
        capture.captureLive({{"wrong", {0, 0, 2, 2}, 1}});
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.first().first().toString().contains("layout"));
        QCOMPARE(ready.count(), 1);
    }
    void liveAcquiresFreshPixels()
    {
        validDiscovery();
        script("grim", "printf 'P3\\n2 2\\n255\\n0 0 255 0 0 255 0 0 255 0 0 255\\n'\n");
        GrimCapture capture;
        QSignalSpy ready(&capture, &CaptureBackend::ready);
        capture.startLive();
        QTRY_COMPARE(ready.count(), 1);
        const auto placeholders = qvariant_cast<QVector<Frame>>(ready.first().first());
        capture.captureLive({placeholders.first().output});
        QTRY_COMPARE(ready.count(), 2);
        const auto frames = qvariant_cast<QVector<Frame>>(ready.last().first());
        QCOMPARE(frames.first().image.pixelColor(0, 0), QColor(Qt::blue));
    }
    void clickPickerQueriesMetadataOnly()
    {
        script("hyprctl", "case \"$2\" in\n"
               "monitors) printf '%s' '[{\"name\":\"test\",\"x\":0,\"y\":0,\"width\":2,\"height\":2,\"scale\":1,\"transform\":0,\"activeWorkspace\":{\"id\":1}}]' ;;\n"
               "clients) printf '%s' '[{\"address\":\"0x10\",\"at\":[0,0],\"size\":[2,2],\"workspace\":{\"id\":1},\"mapped\":true}]' ;;\n"
               "*) exit 9 ;;\nesac\n");
        script("grim", "exit 99\n");
        WindowPicker picker;
        QSignalSpy picked(&picker, &WindowPicker::picked);
        QSignalSpy failed(&picker, &WindowPicker::failed);
        picker.pick({1, 1}, {QVariantMap{{"id", "exact"}, {"address", "10"}}});
        QTRY_COMPARE(picked.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(picked.first().first().toStringList(), QStringList{"exact"});
        picker.pick({1, 1}, {QVariantMap{{"id", "exact"}, {"address", "10"}}});
        picker.cancel();
        QTest::qWait(50);
        QCOMPARE(picked.count(), 1);
    }
    void niriNativePickerUsesOpaqueNumericIdentity()
    {
        script("niri", "[ \"$1\" = msg ] && [ \"$2\" = --json ] && [ \"$3\" = pick-window ] || exit 9\nprintf '%s' '{\"id\":42}'\n");
        WindowPicker picker;
        QSignalSpy picked(&picker, &WindowPicker::picked);
        picker.pickNative({QVariantMap{{"id", "exact"}, {"identifier", "42"}}});
        QTRY_COMPARE(picked.count(), 1);
        QCOMPARE(picked.first().first().toStringList(), QStringList{"exact"});
    }
};

QTEST_GUILESS_MAIN(CaptureTest)
#include "capture_test.moc"
