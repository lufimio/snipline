#include "controller.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickItem>
#include <QScreen>
#include <QDir>
#include <wayland-client.h>
#include <QTemporaryDir>
#include <QtTest>

using namespace snip;

class FakePixels : public CaptureBackend {
public:
    using CaptureBackend::CaptureBackend;
    int frozenStarts = 0, liveStarts = 0, acquisitions = 0, verifications = 0;
    bool rejectLayout = false;
    Output output{QGuiApplication::primaryScreen()->name(), {0, 0, 640, 480}, 1};
    bool requiresWayland() const override { return false; }
    void start() override { ++frozenStarts; finish(Qt::red); }
    void startLive() override { ++liveStarts; emit ready({{output, {}}}); }
    void captureLive(const QVector<Output> &) override { ++acquisitions; }
    void verifyLayout(const QVector<Output> &) override {
        ++verifications;
        if (rejectLayout) emit failed("Output layout changed.");
        else emit layoutVerified();
    }
    void cancel() override {}
    void finish(QColor color) {
        QImage image(640, 480, QImage::Format_RGB32);
        image.fill(color);
        emit ready({{output, image}});
    }
};

class FakeWindows : public WindowBackend {
public:
    int captures = 0;
    QString target, reason;
    QVariantList list{
        QVariantMap{{"id", "one"}, {"label", "same title"}, {"title", "same title"}},
        QVariantMap{{"id", "two"}, {"label", "same title"}, {"title", "same title"}}};
    void start() override { emit changed(); }
    void stop() override {}
    void cancelCapture() override {}
    QVariantList windows() const override { return list; }
    QString unavailableReason() const override { return reason; }
    void capture(const QString &id) override { ++captures; target = id; }
    void finish(const QString &id, QColor color = Qt::green) {
        QImage image(8, 6, QImage::Format_RGB32);
        image.fill(color);
        emit imageReady(id, image);
    }
};

class FakeBarrier : public SurfaceBarrier {
public:
    int starts = 0;
    bool allHidden = false;
    void start() override {
        ++starts;
        allHidden = true;
        for (auto *window : QGuiApplication::topLevelWindows())
            if (window->isVisible()) allHidden = false;
    }
    void cancel() override {}
    void acknowledge() { emit synchronized(); }
};

class FakePicker : public WindowPicker {
public:
    int clicks = 0, nativePicks = 0;
    bool native = false;
    QPointF point;
    void pick(QPointF position, const QVariantList &) override { ++clicks; point = position; }
    void pickNative(const QVariantList &) override { ++nativePicks; }
    bool nativePickerRequired() const override { return native; }
    void cancel() override {}
};

class UiTest : public QObject {
    Q_OBJECT
    Selection *selectionModel() {
        for (auto *window : QGuiApplication::topLevelWindows())
            if (auto *view = qobject_cast<QQuickView *>(window); view && view->isVisible())
                return qobject_cast<Selection *>(view->rootContext()->contextProperty("selection").value<QObject *>());
        return nullptr;
    }
private slots:
    void clickingChoosesTargetWithoutConfirming()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker);
        QVERIFY(controller.CaptureWithMode("window", true).isEmpty());
        controller.pickWindowAt(-10, 20);
        QCOMPARE(picker.clicks, 1);
        QCOMPARE(picker.point, QPointF(-10, 20));
        emit picker.picked({"two"});
        QCOMPARE(controller.selectedWindow(), QString("two"));
        QVERIFY(controller.canConfirm());
        QCOMPARE(windows.captures, 0);
        QCOMPARE(pixels.acquisitions, 0);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
        controller.startWindowPick();
        controller.pickWindowAt(50, 60);
        controller.cancel();
        emit picker.picked({"one"}); // A cancelled click job cannot select or save.
        QVERIFY(controller.selectedWindow().isEmpty());
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void nativePickingWaitsForHideAndOnlySelects()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        picker.native = true;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker);
        QVERIFY(controller.Capture("window").isEmpty());
        controller.startWindowPick();
        QVERIFY(barrier.allHidden);
        QCOMPARE(picker.nativePicks, 0);
        barrier.acknowledge();
        QCOMPARE(picker.nativePicks, 1);
        emit picker.picked({"two"});
        QCOMPARE(controller.selectedWindow(), QString("two"));
        QCOMPARE(windows.captures, 1); // Frozen preview, not a save.
        windows.finish("two");
        QVERIFY(controller.canConfirm());
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void overlayLoadsWithSyntheticPixels()
    {
        QQuickStyle::setStyle("Basic");
        QTemporaryDir directory;
        Controller controller(directory.path(), "png", {});
        Selection selection;
        selection.setBounds({-640, 0, 640, 480});
        selection.setRect({-540, 100, 100, 100});
        Theme theme;
        QQmlEngine engine;
        auto *provider = new FrameProvider;
        QImage image(640, 480, QImage::Format_RGB32);
        image.fill(Qt::red);
        provider->frames.push_back({{"test", {-640, 0, 640, 480}, 1}, image});
        engine.addImageProvider("frames", provider);
        QQuickView view(&engine, nullptr);
        view.resize(640, 480);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        auto *context = view.rootContext();
        context->setContextProperty("controller", &controller);
        context->setContextProperty("selection", &selection);
        context->setContextProperty("theme", theme.tokens());
        context->setContextProperty("outputIndex", QVariant(0));
        context->setContextProperty("outputRect", QRectF(-640, 0, 640, 480));
        QList<QQmlError> warnings;
        connect(&engine, &QQmlEngine::warnings, &engine,
                [&warnings](const QList<QQmlError> &errors) { warnings.append(errors); });
        view.setSource(QUrl("qrc:/qml/Overlay.qml"));
        QCOMPARE(view.status(), QQuickView::Ready);
        view.show();
        QTest::qWait(100);
        QVERIFY2(warnings.isEmpty(), warnings.isEmpty() ? "" : qPrintable(warnings.first().toString()));
        QCOMPARE(view.rootObject()->property("rx").toDouble(), 100);
        // Read only our synthetic offscreen surface, never the user's desktop.
        const auto rendered = view.grabWindow();
        QVERIFY(!rendered.isNull());
        QCOMPARE(rendered.pixelColor(120, 120), QColor(Qt::red));
        QVERIFY(rendered.pixelColor(20, 20).red() < 200);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void liveRegionWaitsForUnmapAcknowledgement()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        QVERIFY(controller.CaptureWithMode("region", true).isEmpty());
        QCOMPARE(pixels.liveStarts, 1);
        QCOMPARE(pixels.frozenStarts, 0);
        auto *selection = selectionModel();
        QVERIFY(selection);
        selection->setRect({10, 10, 20, 30});
        QTest::qWait(50);
        for (auto *window : QGuiApplication::topLevelWindows()) {
            auto *view = qobject_cast<QQuickView *>(window);
            if (!view || !view->isVisible()) continue;
            const auto overlay = view->grabWindow();
            QVERIFY(!overlay.isNull());
            QCOMPARE(overlay.pixelColor(15, 15).alpha(), 0); // Live area is truly transparent.
            QVERIFY(overlay.pixelColor(300, 200).alpha() < 255);
        }
        QVERIFY(controller.canConfirm());
        controller.confirm();
        QCOMPARE(barrier.starts, 1);
        QVERIFY(barrier.allHidden);
        QCOMPARE(pixels.acquisitions, 0);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
        barrier.acknowledge();
        QCOMPARE(pixels.acquisitions, 1);
        pixels.finish(Qt::blue);
        const auto files = QDir(directory.path()).entryList(QDir::Files);
        QCOMPARE(files.size(), 1);
        const QImage saved(QDir(directory.path()).filePath(files.first()));
        QCOMPARE(saved.size(), QSize(20, 30));
        QCOMPARE(saved.pixelColor(0, 0), QColor(Qt::blue));
    }
    void liveBarrierFailureAndCancellationNeverCapture()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        QVERIFY(controller.CaptureWithMode("monitor", true).isEmpty());
        controller.confirm();
        emit barrier.failed("No compositor acknowledgement.");
        QVERIFY(!controller.error().isEmpty());
        QVERIFY(!controller.busy());
        barrier.acknowledge(); // A late acknowledgement must not resurrect the job.
        QCOMPARE(pixels.acquisitions, 0);
        controller.cancel();
        QVERIFY(controller.CaptureWithMode("monitor", true).isEmpty());
        controller.confirm();
        controller.cancel();
        barrier.acknowledge();
        QCOMPARE(pixels.acquisitions, 0);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void liveWindowKeepsOpaqueIdentityAndNeverCropsDesktop()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        QVERIFY(controller.CaptureWithMode("window", true).isEmpty());
        controller.selectWindow("two"); // Same title as "one", different stable handle.
        QVERIFY(controller.canConfirm());
        QCOMPARE(windows.captures, 0);
        controller.confirm();
        QCOMPARE(windows.captures, 0);
        barrier.acknowledge();
        QCOMPARE(windows.captures, 1);
        QCOMPARE(windows.target, QString("two"));
        QCOMPARE(pixels.acquisitions, 0); // Never acquire a desktop substitute.
        windows.finish("one", Qt::red); // Unrelated/stale result.
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
        windows.finish("two");
        QCOMPARE(pixels.verifications, 2); // Layout before AND after acquisition.
        const auto files = QDir(directory.path()).entryList(QDir::Files);
        QCOMPARE(files.size(), 1);
        const QImage saved(QDir(directory.path()).filePath(files.first()));
        QCOMPARE(saved.size(), QSize(8, 6));
        QCOMPARE(saved.pixelColor(0, 0), QColor(Qt::green));
    }
    void frozenWindowCommitsPreviewOnlyAfterConfirmation()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        QVERIFY(controller.Capture("window").isEmpty());
        QCOMPARE(pixels.frozenStarts, 0); // Isolated windows never need desktop pixels.
        // Exercise the list delegates too, not just its collapsed control.
        for (auto *window : QGuiApplication::topLevelWindows()) {
            auto *view = qobject_cast<QQuickView *>(window);
            if (!view || !view->isVisible()) continue;
            QList<QQmlError> warnings;
            const auto connection = connect(view->engine(), &QQmlEngine::warnings, view,
                [&warnings](const QList<QQmlError> &errors) { warnings.append(errors); });
            auto *picker = view->rootObject()->findChild<QObject *>("windowPicker");
            QVERIFY(picker);
            auto *popup = picker->property("popup").value<QObject *>();
            QVERIFY(popup);
            QVERIFY(QMetaObject::invokeMethod(popup, "open"));
            QTest::qWait(50);
            QVERIFY(QMetaObject::invokeMethod(popup, "close"));
            disconnect(connection);
            QVERIFY2(warnings.isEmpty(), warnings.isEmpty() ? "" : qPrintable(warnings.first().toString()));
        }
        controller.selectWindow("one");
        QVERIFY(!controller.canConfirm());
        QCOMPARE(windows.captures, 1);
        windows.finish("one");
        QVERIFY(controller.canConfirm());
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
        controller.confirm();
        QCOMPARE(windows.captures, 1);
        QCOMPARE(barrier.starts, 0);
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files).size(), 1);
    }
    void closedWindowAndLayoutChangeFailWithoutFallback()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        QVERIFY(controller.CaptureWithMode("window", true).isEmpty());
        controller.selectWindow("one");
        controller.confirm();
        emit windows.closed("one");
        barrier.acknowledge();
        QCOMPARE(windows.captures, 0);
        QCOMPARE(pixels.acquisitions, 0);
        controller.cancel();
        QVERIFY(controller.CaptureWithMode("window", true).isEmpty());
        controller.selectWindow("two");
        pixels.rejectLayout = true;
        controller.confirm();
        barrier.acknowledge();
        QCOMPARE(windows.captures, 0);
        QVERIFY(!controller.error().isEmpty());
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void windowBufferValidationAndOrientation()
    {
        QVERIFY(!validWindowBuffer(WL_SHM_FORMAT_XRGB8888, 0, 2, 8));
        QVERIFY(!validWindowBuffer(WL_SHM_FORMAT_XRGB8888, 2, 2, 4));
        QVERIFY(!validWindowBuffer(WL_SHM_FORMAT_XRGB8888, 2, 2, UINT32_MAX));
        QVERIFY(!validWindowBuffer(WL_SHM_FORMAT_RGB565, 2, 2, 8));
        QVERIFY(!validWindowBuffer(WL_SHM_FORMAT_XRGB8888, 32768, 32768, 131072));
        const quint32 data[]{0x00ff0000, 0x0000ff00, 0x000000ff, 0x00ffffff};
        const auto image = copyWindowBuffer(reinterpret_cast<const uchar *>(data),
                                            WL_SHM_FORMAT_XRGB8888, 2, 2, 8, true);
        QCOMPARE(image.pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(image.pixelColor(0, 1), QColor(Qt::red));
    }
    void switchingSelectionModeRestartsOnlyAfterBarrier()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        QVERIFY(controller.Capture("monitor").isEmpty());
        controller.setLive(true);
        QCOMPARE(pixels.liveStarts, 0);
        QVERIFY(barrier.allHidden);
        barrier.acknowledge();
        QCOMPARE(pixels.liveStarts, 1);
        QVERIFY(controller.live());
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
        controller.setLive(false);
        QCOMPARE(pixels.frozenStarts, 1);
        barrier.acknowledge();
        QCOMPARE(pixels.frozenStarts, 2);
        QVERIFY(!controller.live());
        QCOMPARE(pixels.acquisitions, 0);
    }
    void permissionRevocationPreventsFrozenWindowCommit()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        QVERIFY(controller.Capture("window").isEmpty());
        controller.selectWindow("one");
        windows.finish("one");
        QVERIFY(controller.canConfirm());
        windows.reason = "Window permission revoked.";
        emit windows.changed();
        QVERIFY(!controller.canConfirm());
        controller.confirm();
        QCOMPARE(pixels.verifications, 0);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
};

QTEST_MAIN(UiTest)
#include "ui_test.moc"
