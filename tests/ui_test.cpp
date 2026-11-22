#include "controller.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickItem>
#include <QScreen>
#include <QDir>
#include <QSvgRenderer>
#include <QDesktopServices>
#include <QUrlQuery>
#include <QClipboard>
#include <wayland-client.h>
#include <QTemporaryDir>
#include <QtTest>
#include <ZXing/MultiFormatWriter.h>
#include <ZXing/BitMatrix.h>

using namespace snip;
class ResultUrls : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl &url) { urls.append(url); }
};

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
    bool allTransparent = false;
    void start() override {
        ++starts;
        allHidden = true;
        allTransparent = true;
        for (auto *window : QGuiApplication::topLevelWindows()) {
            if (!window->isVisible()) continue;
            allHidden = false;
            auto *quick = qobject_cast<QQuickWindow *>(window);
            if (!quick || quick->contentItem()->opacity() != 0) allTransparent = false;
        }
    }
    void cancel() override {}
    void acknowledge() { emit synchronized(); }
};

class FakePicker : public WindowPicker {
public:
    int clicks = 0, nativePicks = 0;
    bool native = false;
    QPointF point;
    QVariantMap rectangles;
    QMap<QString, QVariantMap> identities;
    QVariantMap metadata(const QString &id) const override { return identities.value(id); }
    void pick(QPointF position, const QVariantList &) override { ++clicks; point = position; }
    void pickNative(const QVariantList &) override { ++nativePicks; }
    bool nativePickerRequired() const override { return native; }
    void refresh(const QVariantList &) override {}
    QVariantMap geometry() const override { return rectangles; }
    QString hoveredAt(QPointF position) const override {
        for (auto it = rectangles.begin(); it != rectangles.end(); ++it)
            if (it.value().toRectF().contains(position)) return it.key();
        return {};
    }
    void cancel() override {}
};

class FakeAudio : public AudioSources {
public:
    int queries = 0;
    QVariantList values{
        QVariantMap{{"id", "default:mic"}, {"kind", "device"}, {"device", ""}, {"label", "Desk microphone"}, {"icon", "mic"}},
        QVariantMap{{"id", "default:system"}, {"kind", "device"}, {"device", "@DEFAULT_MONITOR@"}, {"label", "All apps — speakers"}, {"icon", "monitor"}}};
    void refresh() override { ++queries; emit changed(); }
    QVariantList sources() const override { return values; }
};

class FakeRecorder : public VideoRecorder {
public:
    int starts = 0, frames = 0, stops = 0, cancels = 0;
    quint64 token = 0;
    RecordingOptions options;
    QImage firstFrame;
    void start(quint64 generation, RecordingOptions settings, const QImage &image) override {
        ++starts; token = generation; options = settings; firstFrame = image;
        emit started(token);
    }
    void submitFrame(const QImage &) override { ++frames; }
    void setPaused(bool value) override { emit paused(token, value); }
    void stop() override { ++stops; }
    void cancel() override { ++cancels; }
    void finish(const QString &path) { emit saved(token, path); }
};

class UiTest : public QObject {
    Q_OBJECT
    QQuickItem *visualItem(QQuickItem *parent, const QString &name) {
        if (parent->objectName() == name) return parent;
        for (auto *child : parent->childItems())
            if (auto *found = visualItem(child, name)) return found;
        return nullptr;
    }
    QQuickView *selectorView() {
        for (auto *window : QGuiApplication::topLevelWindows())
            if (auto *view = qobject_cast<QQuickView *>(window); view && view->isVisible())
                return view;
        return nullptr;
    }
    Selection *selectionModel() {
        for (auto *window : QGuiApplication::topLevelWindows())
            if (auto *view = qobject_cast<QQuickView *>(window); view && view->isVisible())
                return qobject_cast<Selection *>(view->rootContext()->contextProperty("selection").value<QObject *>());
        return nullptr;
    }
private slots:


    void configuredEditorsAreAvailableOnlyAfterSaving()
    {
        QTemporaryDir directory;
        const auto executable = directory.filePath("editor");
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf '%s\\n' \"$1\" > \"$0.arg\"\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, nullptr, &recorder);
        Config config;
        config.notifications = false;
        controller.configure(config);
        QVERIFY(controller.Capture("region").isEmpty());
        auto *view = selectorView();
        QVERIFY(view);
        selectionModel()->setRect({10, 10, 100, 100});
        auto *arrow = view->rootObject()->findChild<QObject *>("advancedCapture");
        QVERIFY(QMetaObject::invokeMethod(arrow, "clicked"));
        QQuickItem *edit = nullptr;
        QTRY_VERIFY((edit = visualItem(view->contentItem(), "resultAction_edit")));
        QVERIFY(!edit->isEnabled());
        controller.setEditor(executable + " {}");
        QTRY_VERIFY(edit->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(edit, "clicked"));
        const auto argument = [&] {
            QFile file(executable + ".arg");
            return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray{};
        };
        QTRY_COMPARE(argument(), controller.lastCapture().toUtf8());
        QVERIFY(QFile::exists(controller.lastCapture()));
        QVERIFY(QFile::remove(executable + ".arg"));
        QVERIFY(controller.Record("monitor", "mp4", false, false).isEmpty());
        controller.setVideoEditor(executable + " {}");
        controller.setCountdownSeconds(0);
        controller.confirmWithAction("edit");
        barrier.acknowledge();
        pixels.finish(Qt::green);
        QCOMPARE(controller.recordingState(), "Recording");
        QVERIFY(!QFile::exists(executable + ".arg"));
        controller.Stop();
        QVERIFY(!QFile::exists(executable + ".arg"));
        const auto video = directory.filePath("recording with spaces.mp4");
        QFile file(video);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("simulated finalized recording");
        file.close();
        recorder.finish(video);
        QTRY_COMPARE(argument(), video.toUtf8());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("simulated finalized recording"));
    }
    void arrowActionShortcutsMatchMenuActions()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        Config config;
        config.notifications = false;
        controller.configure(config);
        ResultUrls urls;
        QDesktopServices::setUrlHandler("file", &urls, "open");
        const auto clearHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        for (const auto &action : {"save", "copy", "open", "folder"}) {
            QVERIFY(controller.Capture("region").isEmpty());
            auto *view = selectorView();
            QVERIFY(view);
            view->requestActivate();
            QTest::qWait(30);
            const auto before = QDir(directory.path()).entryList(QDir::Files).size();
            QGuiApplication::clipboard()->clear();
            QTest::keyClick(view, Qt::Key_S, Qt::ControlModifier); // No selection: disabled.
            QCOMPARE(QDir(directory.path()).entryList(QDir::Files).size(), before);
            selectionModel()->setRect({10, 10, 100, 100});
            const QString value(action);
            QTest::keyClick(view, value == "save" ? Qt::Key_S : value == "copy" ? Qt::Key_C : Qt::Key_O,
                value == "folder" ? Qt::ControlModifier | Qt::ShiftModifier : Qt::ControlModifier);
            QTRY_COMPARE(QDir(directory.path()).entryList(QDir::Files).size(), before + (value == "copy" ? 0 : 1));
            if (value == "copy") QVERIFY(!QGuiApplication::clipboard()->image().isNull());
            else QVERIFY(QGuiApplication::clipboard()->image().isNull());
        }
        QCOMPARE(urls.urls.size(), 2);
        QVERIFY(urls.urls.first().toLocalFile().endsWith(".png"));
        QCOMPARE(urls.urls.last().toLocalFile(), directory.path());
    }
    void recordingArrowShortcutsWaitForFinalization()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, nullptr, &recorder);
        ResultUrls urls;
        QDesktopServices::setUrlHandler("file", &urls, "open");
        const auto clearHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        QVERIFY(controller.Record("monitor", "mp4", false, false).isEmpty());
        controller.setCountdownSeconds(0);
        auto *view = selectorView();
        QVERIFY(view);
        view->requestActivate();
        QTest::qWait(30);
        QTest::keyClick(view, Qt::Key_E, Qt::ControlModifier);
        QCOMPARE(recorder.starts, 0);
        QVERIFY(controller.pickMessage().isEmpty()); // No configured editor: shortcut disabled.
        QTest::keyClick(view, Qt::Key_S, Qt::ControlModifier);
        barrier.acknowledge();
        pixels.finish(Qt::green);
        QCOMPARE(recorder.starts, 1);
        QCOMPARE(controller.recordingResultAction(), "save");
        QVERIFY(controller.Capture("monitor").isEmpty()); // Bring controls back.
        view = selectorView();
        view->requestActivate();
        QTest::qWait(30);
        QTest::keyClick(view, Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(controller.recordingState(), "Finalizing");
        QCOMPARE(recorder.stops, 1);
        QVERIFY(urls.urls.isEmpty());
        recorder.finish(directory.filePath("recording.mp4"));
        QTRY_COMPARE(urls.urls.size(), 1);
        QCOMPARE(urls.urls.first().toLocalFile(), directory.path());
    }
    void toolTargetAndTimingShortcutsDoNotConfirm()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        FakeRecorder recorder;
        FakeAudio audio;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker, &recorder, &audio);
        Config config;
        config.live = false;
        config.notifications = false;
        controller.configure(config);
        QVERIFY(controller.CaptureWithMode("region", false).isEmpty());
        auto *view = selectorView();
        QVERIFY(view);
        view->requestActivate();
        QTest::qWait(50);
        QVERIFY(!view->rootObject()->findChild<QObject *>("copyToClipboard"));
        QVERIFY(!view->rootObject()->property("captureCursorVisible").toBool());
        QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(view->rootObject()->property("captureCursorVisible").toBool());
        QTest::keyClick(view, Qt::Key_3, Qt::ControlModifier);
        QTRY_COMPARE(controller.action(), "qr");
        QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(view->rootObject()->property("captureCursorVisible").toBool());
        QTest::keyClick(view, Qt::Key_4, Qt::ControlModifier);
        QTRY_COMPARE(controller.action(), "ocr");
        QTest::keyClick(view, Qt::Key_1, Qt::ControlModifier);
        QTRY_COMPARE(controller.action(), "capture");
        QTest::keyClick(view, Qt::Key_L, Qt::ControlModifier);
        QTRY_VERIFY(controller.live());
        QTest::keyClick(view, Qt::Key_2, Qt::ControlModifier);
        QTRY_COMPARE(controller.action(), "record");
        QVERIFY(!view->rootObject()->property("recordCursorVisible").toBool());
        QTest::keyClick(view, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(view->rootObject()->property("recordCursorVisible").toBool());
        QTest::keyClick(view, Qt::Key_L, Qt::ControlModifier);
        QVERIFY(controller.live()); // Recording selection must remain live.
        QTest::keyClick(view, Qt::Key_1, Qt::ControlModifier);
        QTRY_COMPARE(controller.action(), "capture");
        QTest::keyClick(view, Qt::Key_2, Qt::ShiftModifier);
        QTRY_COMPARE(controller.mode(), "monitor");
        QTest::keyClick(view, Qt::Key_3, Qt::ShiftModifier);
        QTRY_COMPARE(controller.mode(), "window");
        QTest::keyClick(view, Qt::Key_1, Qt::ShiftModifier);
        QTRY_COMPARE(controller.mode(), "region");
        QTest::keyClick(view, Qt::Key_L, Qt::ControlModifier);
        QTRY_COMPARE(barrier.starts, 1);
        barrier.acknowledge();
        pixels.finish(Qt::green);
        QVERIFY(!controller.live());
        QCOMPARE(selectorView(), view);
        QCOMPARE(recorder.starts, 0);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
        selectionModel()->setRect({10, 10, 100, 100});
        QVERIFY(controller.canConfirm());
        QTest::keyClick(view, Qt::Key_Space);
        QTRY_COMPARE(QDir(directory.path()).entryList(QDir::Files).size(), 1);
    }
    void recognitionTimingOverridesRespectResidentConfig()
    {
        QTemporaryDir directory;
        for (const auto &kind : {"qr", "ocr"}) {
            for (bool defaultLive : {false, true}) {
                for (int timing : {-1, 0, 1}) {
                    FakePixels pixels;
                    FakeWindows windows;
                    FakeBarrier barrier;
                    Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
                    Config config;
                    config.live = defaultLive;
                    config.notifications = false;
                    controller.configure(config);
                    QVERIFY(controller.RecognizeWithMode(kind, "region", {}, timing).isEmpty());
                    const bool live = timing < 0 ? defaultLive : bool(timing);
                    QCOMPARE(controller.live(), live);
                    QCOMPARE(pixels.liveStarts, live ? 1 : 0);
                    QCOMPARE(pixels.frozenStarts, live ? 0 : 1);
                    controller.cancel();
                }
            }
        }
    }
    void recognitionFromSelectionAndImages()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker);
        Config config;
        config.live = false; // This test exercises the frozen selection path.
        config.notifications = false;
        controller.configure(config);
        QVERIFY(controller.Recognize("qr", "monitor", {}).isEmpty());
        QCOMPARE(controller.action(), "qr");
        auto *selectionView = selectorView();
        QVERIFY(selectionView);
        selectionView->rootObject()->setProperty("optionsExpanded", true);
        QVERIFY(!selectionView->rootObject()->findChild<QQuickItem *>("showCursor")->isVisible());
        controller.selectMonitor(0);
        QVERIFY(controller.canConfirm());
        QGuiApplication::clipboard()->setText("Keep existing clipboard");
        controller.confirm();
        for (auto *window : QGuiApplication::topLevelWindows()) QVERIFY(!window->isVisible());
        auto *recognition = qobject_cast<Recognition *>(controller.recognition());
        QVERIFY(recognition);
        QTRY_VERIFY(!recognition->busy());
        QCOMPARE(recognition->state(), "Done");
        QVERIFY(recognition->codes().isEmpty());
        QCOMPARE(QGuiApplication::clipboard()->text(), "Keep existing clipboard");
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());

        ResultUrls urls;
        QDesktopServices::setUrlHandler("https", &urls, "open");
        const auto bits = ZXing::MultiFormatWriter(ZXing::BarcodeFormat::QRCode).setMargin(4)
                              .encode(L"https://example.com/explicit-only", 320, 320);
        QImage image(bits.width(), bits.height(), QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int y = 0; y < bits.height(); ++y)
            for (int x = 0; x < bits.width(); ++x) if (bits.get(x, y)) image.setPixelColor(x, y, Qt::black);
        const auto path = directory.filePath("saved.png");
        QVERIFY(image.save(path));
        QGuiApplication::clipboard()->setText("Replace automatically");
        QVERIFY(controller.Recognize("qr", {}, path).isEmpty());
        QTRY_VERIFY(!recognition->busy());
        QCOMPARE(recognition->codes().size(), 1);
        QCOMPARE(QGuiApplication::clipboard()->text(), "https://example.com/explicit-only");
        QCOMPARE(urls.urls.size(), 0);
        QVERIFY(controller.canOpenCode(0));
        QVERIFY(!controller.canOpenCode(-1));
        QVERIFY(controller.copyRecognition(0));
        QCOMPARE(QGuiApplication::clipboard()->text(), "https://example.com/explicit-only");
        QVERIFY(controller.openCode(0));
        QCOMPARE(urls.urls.size(), 1);
        for (const auto &payload : {QString("file:///etc/passwd"), QString("javascript:alert(1)"),
                                    QString("https://user:password@example.com/"), QString("https://example.com/ white")}) {
            const auto unsafeBits = ZXing::MultiFormatWriter(ZXing::BarcodeFormat::QRCode).setMargin(4)
                                        .encode(payload.toStdWString(), 320, 320);
            QImage unsafeImage(unsafeBits.width(), unsafeBits.height(), QImage::Format_RGB32);
            unsafeImage.fill(Qt::white);
            for (int y = 0; y < unsafeBits.height(); ++y)
                for (int x = 0; x < unsafeBits.width(); ++x)
                    if (unsafeBits.get(x, y)) unsafeImage.setPixelColor(x, y, Qt::black);
            recognition->startImage("qr", unsafeImage);
            QTRY_VERIFY(!recognition->busy());
            QCOMPARE(recognition->codes().size(), 1);
            QVERIFY(!controller.canOpenCode(0));
            QVERIFY(!controller.openCode(0));
            QCOMPARE(urls.urls.size(), 1);
        }
        QDesktopServices::unsetUrlHandler("https");
        controller.SessionLocked();
        for (auto *window : QGuiApplication::topLevelWindows()) QVERIFY(!window->isVisible());
        QVERIFY(!recognition->hasInput());
        QVERIFY(recognition->codes().isEmpty());
        QVERIFY(!controller.openCode(0));
        QVERIFY(!controller.Recognize("qr", "region", path).isEmpty());
        controller.ScreenSaverActive(false);
        QVERIFY(controller.Capture("monitor").isEmpty());
        controller.selectMonitor(0);
        controller.confirmWithAction("save");
        QVERIFY(!controller.lastCapture().isEmpty());
        QVERIFY(controller.Recognize("ocr", "monitor", {}).isEmpty());
        auto *ocrView = selectorView();
        QVERIFY(ocrView);
        ocrView->rootObject()->setProperty("optionsExpanded", true);
        QTRY_VERIFY(!recognition->busy());
        auto *languages = ocrView->rootObject()->findChild<QQuickItem *>("ocrLanguageSelector");
        QVERIFY(languages);
        QVERIFY(languages->isVisible());
        QVERIFY(!ocrView->rootObject()->findChild<QQuickItem *>("showCursor")->isVisible());
        QCOMPARE(languages->width(), qreal(146));
        QCOMPARE(languages->height(), qreal(29));
        QCOMPARE(languages->findChildren<QObject *>("dropdownArrow").size(), 1);
        if (!recognition->languages().isEmpty()) {
            const auto language = recognition->languages().first();
            const auto before = recognition->selectedLanguages();
            auto *popup = ocrView->rootObject()->findChild<QObject *>("ocrLanguagesPopup");
            QVERIFY(popup);
            QVERIFY(QMetaObject::invokeMethod(popup, "open"));
            QTRY_VERIFY(popup->property("opened").toBool());
            QTest::qWait(100);
            const auto anchor = languages->mapToScene({0, 0});
            const qreal px = popup->property("panelX").toReal(), py = popup->property("panelY").toReal();
            const qreal pw = popup->property("panelWidth").toReal(), ph = popup->property("panelHeight").toReal();
            QVERIFY(px >= 12 && px + pw <= ocrView->width() - 12);
            QVERIFY(py >= 12 && py + ph <= ocrView->height() - 12);
            QVERIFY(py + ph <= anchor.y() - 8 || py >= anchor.y() + languages->height() + 8);
            QQuickItem *choice = nullptr;
            QTRY_VERIFY((choice = visualItem(ocrView->contentItem(), "ocrLanguage_" + language)));
            QTRY_VERIFY(choice->isVisible() && choice->width() > 0 && choice->height() > 0);
            QTest::mouseClick(ocrView, Qt::LeftButton, Qt::NoModifier,
                choice->mapToScene({choice->width() / 2, choice->height() / 2}).toPoint());
            QTRY_COMPARE(recognition->selectedLanguages().contains(language), !before.contains(language));
            QVERIFY(popup->property("visible").toBool()); // Multi-select stays open.
            recognition->toggleLanguage(language);
            QCOMPARE(recognition->selectedLanguages(), before);
            QVERIFY(QMetaObject::invokeMethod(popup, "close"));
        }
        controller.selectMonitor(0);
        QVERIFY(controller.canConfirm());
        controller.confirm();
        QTRY_VERIFY_WITH_TIMEOUT(!recognition->busy(), 70000);
        for (auto *window : QGuiApplication::topLevelWindows()) QVERIFY(!window->isVisible());
        QVERIFY(controller.Recognize("ocr", {}, controller.lastCapture()).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!recognition->busy(), 70000);
        QCOMPARE(recognition->sourceFile(), controller.lastCapture());
        // Exercise the controller's completion path without depending on installed OCR models.
        recognition->setText("Automatically copied OCR result");
        emit recognition->finished(true);
        QCOMPARE(QGuiApplication::clipboard()->text(), "Automatically copied OCR result");
        recognition->setText("Edited OCR result");
        QCOMPARE(QGuiApplication::clipboard()->text(), "Automatically copied OCR result");
        QVERIFY(controller.copyRecognition());
        QCOMPARE(QGuiApplication::clipboard()->text(), "Edited OCR result");
        controller.closeRecognition();
        for (auto *window : QGuiApplication::topLevelWindows()) QVERIFY(!window->isVisible());
        QVERIFY(!recognition->hasInput());
    }
    void initTestCase() { QQuickStyle::setStyle("Basic"); }
    void sessionLockFinalizesWithoutLaunchingSelectedResultAction()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakeRecorder recorder;
        ResultUrls urls;
        QDesktopServices::setUrlHandler("file", &urls, "open");
        const auto clearHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, nullptr, &recorder);
        QVERIFY(controller.Record("monitor", "mp4", false, false).isEmpty());
        controller.setCountdownSeconds(0);
        controller.confirmWithAction("open");
        barrier.acknowledge();
        pixels.finish(Qt::green);
        controller.SessionLocked();
        QCOMPARE(controller.recordingState(), QString("Finalizing"));
        recorder.finish(directory.filePath("locked.mp4"));
        QVERIFY(urls.urls.isEmpty());
        QVERIFY(controller.Status().contains("Session locked."));
    }
    void nativeRecordingRevocationAndLayoutErrorsFinalizeWithoutFallback_data()
    {
        QTest::addColumn<QString>("message");
        QTest::newRow("revocation") << QString("Native capture permission revoked");
        QTest::newRow("layout") << QString("Output layout changed");
    }
    void nativeRecordingRevocationAndLayoutErrorsFinalizeWithoutFallback()
    {
        QFETCH(QString, message);
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, nullptr, &recorder);
        QVERIFY(controller.Record("monitor", "mp4", false, false).isEmpty());
        controller.setCountdownSeconds(0);
        controller.confirm();
        barrier.acknowledge();
        pixels.finish(Qt::green);
        emit pixels.failed(message);
        QCOMPARE(controller.recordingState(), QString("Finalizing"));
        QCOMPARE(recorder.stops, 1);
        const auto acquisitions = pixels.acquisitions;
        QTest::qWait(100);
        QCOMPARE(pixels.acquisitions, acquisitions);
        QVERIFY(controller.Status().contains(message));
        recorder.finish(directory.filePath("recovered.mp4"));
        QVERIFY(controller.Status().contains("recovered.mp4"));
        QVERIFY(controller.Status().contains(message));
    }
    void audioSourceLossFinalizesInsteadOfDiscardingOrSubstituting()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, nullptr, &recorder);
        QVERIFY(controller.Record("monitor", "mp4", false, false).isEmpty());
        controller.setCountdownSeconds(0);
        controller.confirm();
        barrier.acknowledge();
        pixels.finish(Qt::green);
        QCOMPARE(controller.recordingState(), QString("Recording"));
        QVERIFY(controller.Pause().isEmpty());
        const auto pausedElapsed = controller.recordingElapsedMs();
        QTest::qWait(50);
        QCOMPARE(controller.recordingElapsedMs(), pausedElapsed);
        emit recorder.sourceLost(recorder.token, "Selected audio device removed");
        QCOMPARE(controller.recordingState(), QString("Finalizing"));
        QCOMPARE(recorder.stops, 1);
        QVERIFY(controller.Status().contains("Selected audio device removed"));
        const auto path = directory.filePath("partial.mp4");
        recorder.finish(path);
        QVERIFY(!controller.recording());
        QVERIFY(controller.Status().contains("partial.mp4"));
        QVERIFY(controller.Status().contains("Selected audio device removed"));
    }
    void recordingCodecAndEncoderSelectionAreContainerAwareAndLockAtConfirmation()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, nullptr, &recorder);
        QVERIFY(controller.Record("monitor", "mp4", false, false).isEmpty());
        auto *root = selectorView()->rootObject();
        root->setProperty("optionsExpanded", true);
        QVERIFY(root->findChild<QObject *>("videoCodecSelector"));
        QVERIFY(root->findChild<QObject *>("videoEncoderSelector"));
        controller.setRecordingCodec("h265");
        QCOMPARE(controller.recordingCodec(), QString("h265"));
        controller.setRecordingFormat("webm");
        QCOMPARE(controller.recordingCodec(), QString("auto"));
        QVERIFY(controller.recordingCodecs().contains("vp9"));
        QVERIFY(!controller.recordingCodecs().contains("h264"));
        controller.setRecordingCodec("vp9");
        controller.setRecordingFormat("mp4");
        QCOMPARE(controller.recordingCodec(), QString("auto"));
        controller.setRecordingCodec("h264");
        controller.setRecordingEncoder("auto");
        controller.setCountdownSeconds(0);
        controller.confirm();
        barrier.acknowledge();
        pixels.finish(Qt::green);
        QCOMPARE(recorder.starts, 1);
        QCOMPARE(recorder.options.codec, QString("h264"));
        QCOMPARE(recorder.options.encoder, QString("auto"));
        controller.setRecordingCodec("av1");
        controller.setRecordingEncoder("software");
        QCOMPARE(controller.recordingCodec(), QString("h264"));
        QCOMPARE(controller.recordingEncoder(), QString("auto"));
        controller.cancel();
    }
    void screenshotAndRecordingDirectoriesAreIndependent()
    {
        QTemporaryDir directory;
        const auto screenshots = directory.filePath("screenshots");
        const auto videos = directory.filePath("videos");
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakeRecorder recorder;
        Controller controller(screenshots, "png", {}, nullptr, &pixels, &windows, &barrier, nullptr, &recorder);
        Config config;
        config.recordingDirectory = videos;
        config.countdown = 0;
        config.notifications = false;
        controller.configure(config);
        QVERIFY(controller.Capture("monitor").isEmpty());
        QCOMPARE(controller.saveDirectory(), screenshots);
        controller.confirmWithAction("save");
        QCOMPARE(QDir(screenshots).entryList(QDir::Files).size(), 1);
        QVERIFY(!QFileInfo::exists(videos));
        QVERIFY(controller.CaptureWithMode("monitor", true).isEmpty());
        controller.setAction("record");
        QCOMPARE(controller.saveDirectory(), videos);
        const auto changedVideos = directory.filePath("other-videos");
        controller.setSaveDirectory(changedVideos);
        controller.setAction("capture");
        QCOMPARE(controller.saveDirectory(), screenshots);
        controller.setAction("record");
        QCOMPARE(controller.saveDirectory(), changedVideos);
        controller.confirm();
        barrier.acknowledge();
        pixels.finish(Qt::green);
        QCOMPARE(recorder.starts, 1);
        QCOMPARE(recorder.options.directory, changedVideos);
        QCOMPARE(QDir(screenshots).entryList(QDir::Files).size(), 1);
        controller.cancel();
    }
    void errorsFloatAboveToolbarAndFadeWithoutClearingBackendState()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        QVERIFY(controller.Capture("region").isEmpty());
        selectionModel()->setRect({10, 10, 100, 100});
        auto *root = selectorView()->rootObject();
        auto *toast = root->findChild<QQuickItem *>("errorToast");
        auto *bar = root->findChild<QQuickItem *>("multiTierToolbar");
        QVERIFY(toast && bar);
        QVERIFY(!toast->isVisible());
        QTest::qWait(30);
        const auto barHeight = bar->height();
        emit pixels.failed("Synthetic capture error");
        QTRY_COMPARE(toast->opacity(), 1.0);
        QCOMPARE(toast->property("message").toString(), QString("Synthetic capture error"));
        QCOMPARE(bar->height(), barHeight);
        QVERIFY(toast->y() + toast->height() <= bar->y() - 8);
        QVERIFY(!controller.canConfirm());
        QTest::qWait(4600);
        QVERIFY(toast->isVisible());
        // Unrelated controller updates must not restart the notification timer.
        controller.configure(Config{});
        QTRY_COMPARE_WITH_TIMEOUT(toast->opacity(), 0.0, 1200);
        QVERIFY(!toast->isVisible());
        QCOMPARE(controller.error(), QString("Synthetic capture error"));
        QVERIFY(!controller.canConfirm());
    }
    void splitButtonsAndResultMenusHaveConsistentGeometry()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, nullptr, &recorder);
        QVERIFY(controller.Capture("region").isEmpty());
        selectionModel()->setRect({10, 10, 100, 100});
        auto *view = selectorView();
        QVERIFY(view);
        auto *root = view->rootObject();
        auto *lens = root->findChild<QQuickItem *>("lensAction");
        QVERIFY(lens && lens->isVisible());
        QCOMPARE(lens->isEnabled(), qobject_cast<Uploader *>(controller.upload())->available());
        QCOMPARE(lens->property("glyph").toString(), QString("lens"));
        for (const auto &action : {"capture", "record"}) {
            controller.setAction(action);
            QTest::qWait(30);
            auto *group = root->findChild<QQuickItem *>("confirmSplitButton");
            auto *button = root->findChild<QQuickItem *>("confirmAction");
            auto *arrow = root->findChild<QQuickItem *>("advancedCapture");
            QVERIFY(group && button && arrow && arrow->isVisible());
            QCOMPARE(group->width(), button->width() + arrow->width());
            QCOMPARE(button->mapToItem(group, {button->width(), 0}).x(),
                     arrow->mapToItem(group, {0, 0}).x());
            auto *divider = root->findChild<QQuickItem *>("splitButtonDivider");
            QVERIFY(divider && divider->height() < group->height() / 2);
            auto *menu = root->findChild<QObject *>("advancedResultMenu");
            QVERIFY(menu);
            QVERIFY(QMetaObject::invokeMethod(arrow, "clicked"));
            QTRY_VERIFY(menu->property("visible").toBool());
            QQuickItem *upload = nullptr;
            QTRY_VERIFY((upload = visualItem(view->contentItem(), "resultAction_upload")));
            QVERIFY(upload->isVisible());
            QCOMPARE(upload->isEnabled(), qobject_cast<Uploader *>(controller.upload())->available());
            for (const auto &name : {"resultAction_open", "resultAction_upload"}) {
                auto *row = visualItem(view->contentItem(), name);
                QVERIFY(row);
                 for (const auto &part : {"resultItemIcon", "resultItemLabel", "resultItemShortcut"}) {
                    auto *item = row->findChild<QQuickItem *>(part);
                    QVERIFY(item);
                    const auto center = item->mapToItem(row, {item->width() / 2, item->height() / 2});
                    QVERIFY(qAbs(center.y() - row->height() / 2) <= 0.5);
                }
            }
            QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        }
    }
    void captureResultActionsAndDisabledArrow()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        ResultUrls urls;
        QDesktopServices::setUrlHandler("file", &urls, "open");
        const auto clearHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        for (const auto &action : {"copy", "save", "folder", "open"}) {
            QVERIFY(controller.Capture("region").isEmpty());
            auto *root = selectorView()->rootObject();
            auto *arrow = root->findChild<QQuickItem *>("advancedCapture");
            QVERIFY(arrow);
            QVERIFY(!arrow->isEnabled());
            selectionModel()->setRect({10, 10, 100, 100});
            QVERIFY(arrow->isEnabled());
            QGuiApplication::clipboard()->clear();
            const auto before = QDir(directory.path()).entryList(QDir::Files).size();
            controller.confirmWithAction(action);
            const auto after = QDir(directory.path()).entryList(QDir::Files).size();
            QCOMPARE(after - before, QString(action) == "copy" ? 0 : 1);
            if (QString(action) == "copy") QVERIFY(!QGuiApplication::clipboard()->image().isNull());
            else QVERIFY(QGuiApplication::clipboard()->image().isNull());
        }
        QCOMPARE(urls.urls.size(), 2);
        QCOMPARE(urls.urls.first().toLocalFile(), directory.path());
        QVERIFY(urls.urls.last().toLocalFile().endsWith(".png"));
    }
    void recordingResultActionsWaitForSaveAndDiscardNeverOpens()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakeRecorder recorder;
        ResultUrls urls;
        QDesktopServices::setUrlHandler("file", &urls, "open");
        const auto clearHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, nullptr, &recorder);
        QVERIFY(controller.Record("monitor", "mp4", false, false).isEmpty());
        controller.setCountdownSeconds(0);
        controller.confirmWithAction("edit"); // Editor must be explicitly configured.
        QCOMPARE(controller.recordingState(), QString("Selecting"));
        QVERIFY(controller.pickMessage().contains("editor"));
        controller.confirmWithAction("folder");
        barrier.acknowledge();
        pixels.finish(Qt::green);
        QCOMPARE(controller.recordingState(), QString("Recording"));
        QVERIFY(urls.urls.isEmpty());
        QCOMPARE(controller.recordingResultAction(), QString("folder"));
        QVERIFY(controller.Capture("monitor").isEmpty());
        auto *root = selectorView()->rootObject();
        QVERIFY(!root->findChild<QObject *>("recordingResultButton"));
        auto *finish = root->findChild<QQuickItem *>("recordingStop");
        QVERIFY(finish);
        QVERIFY(QMetaObject::invokeMethod(finish, "clicked"));
        QCOMPARE(recorder.stops, 1);
        QCOMPARE(controller.recordingState(), QString("Finalizing"));
        QVERIFY(urls.urls.isEmpty());
        recorder.finish(directory.filePath("test.mp4"));
        QCOMPARE(urls.urls.size(), 1);
        QCOMPARE(urls.urls.first().toLocalFile(), directory.path());
        QVERIFY(controller.Record("monitor", "mp4", false, false).isEmpty());
        controller.setCountdownSeconds(0);
        controller.confirmWithAction("open");
        barrier.acknowledge();
        pixels.finish(Qt::green);
        controller.cancel();
        recorder.finish(directory.filePath("discarded.mp4"));
        QCOMPARE(urls.urls.size(), 1);
    }
    void countdownAndReopenedRecordingControls()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, nullptr, &recorder);
        QVERIFY(controller.Record("region", "webm", false, false).isEmpty());
        selectionModel()->setRect({100, 100, 200, 120});
        controller.setCountdownSeconds(3);
        auto *view = selectorView();
        QVERIFY(view);
        auto *root = view->rootObject();
        controller.confirm();
        auto *countdown = root->findChild<QQuickItem *>("recordingCountdown");
        auto *number = root->findChild<QQuickItem *>("countdownNumber");
        QVERIFY(countdown && number);
        QVERIFY(countdown->isVisible());
        QCOMPARE(countdown->x() + countdown->width() / 2,
                 root->property("rx").toDouble() + root->property("rw").toDouble() / 2);
        QCOMPARE(countdown->y() + countdown->height() / 2,
                 root->property("ry").toDouble() + root->property("rh").toDouble() / 2);
        auto *dimming = root->findChild<QQuickItem *>("selectionDimming");
        QVERIFY(dimming && dimming->isVisible());
        QVERIFY(!root->property("selecting").toBool());
        QCOMPARE(root->property("targetRect").toRectF(), QRectF(100, 100, 200, 120));
        QCOMPARE(number->property("text").toString(), QString("3"));
        QTRY_COMPARE_WITH_TIMEOUT(number->property("text").toString(), QString("2"), 1600);
        QTRY_COMPARE_WITH_TIMEOUT(number->property("text").toString(), QString("1"), 1600);
        QCOMPARE(pixels.acquisitions, 0);
        QTRY_COMPARE_WITH_TIMEOUT(barrier.starts, 1, 1600);
        QVERIFY(barrier.allHidden);
        barrier.acknowledge();
        pixels.finish(Qt::green);
        QCOMPARE(controller.recordingState(), QString("Recording"));
        QVERIFY(!selectorView());
        QVERIFY(controller.Capture("region").isEmpty());
        QCOMPARE(controller.recordingState(), QString("Recording"));
        QCOMPARE(selectorView(), view);
        QVERIFY(controller.recordingControls());
        QVERIFY(dimming->isVisible());
        QCOMPARE(root->property("targetRect").toRectF(), QRectF(100, 100, 200, 120));
        QVERIFY(root->findChild<QQuickItem *>("recordingControls")->isVisible());
        auto *pauseResume = root->findChild<QQuickItem *>("recordingPauseResume");
        QVERIFY(pauseResume);
        QCOMPARE(pauseResume->property("glyph").toString(), QString("pause"));
        QVERIFY(QMetaObject::invokeMethod(pauseResume, "clicked"));
        QCOMPARE(controller.recordingState(), QString("Paused"));
        QCOMPARE(pauseResume->property("glyph").toString(), QString("play"));
        QVERIFY(QMetaObject::invokeMethod(pauseResume, "clicked"));
        QCOMPARE(controller.recordingState(), QString("Recording"));
        QCOMPARE(selectorView(), view);
        QCOMPARE(barrier.starts, 1); // No hide/pause barrier on reopening or resume.
        auto *hide = root->findChild<QQuickItem *>("recordingHideControls");
        QVERIFY(QMetaObject::invokeMethod(hide, "clicked"));
        QVERIFY(!selectorView());
        QCOMPARE(controller.recordingState(), QString("Recording"));
        QVERIFY(controller.Record("window", "mp4", true, true).isEmpty());
        QCOMPARE(controller.recordingState(), QString("Recording"));
        QCOMPARE(recorder.starts, 1);
        auto *stop = root->findChild<QQuickItem *>("recordingStop");
        QVERIFY(QMetaObject::invokeMethod(stop, "clicked"));
        QCOMPARE(recorder.stops, 1);
        QCOMPARE(controller.recordingState(), QString("Finalizing"));
        controller.cancel();
        QVERIFY(!controller.recording());
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void customSvgAssetsArePackaged()
    {
        QDir icons(":/qml/icons");
        const auto files = icons.entryList({"*.svg"}, QDir::Files);
        QVERIFY(files.size() >= 32);
        for (const auto &file : files) {
            QSvgRenderer renderer(icons.filePath(file));
            QVERIFY2(renderer.isValid(), qPrintable(file));
        }
    }
    void customFrameRateEditorAppliesWithoutConfirming()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker, &recorder);
        QVERIFY(controller.CaptureWithMode("region", true).isEmpty());
        selectionModel()->setRect({50, 50, 100, 80});
        controller.setAction("record");
        auto *view = selectorView();
        QVERIFY(view);
        view->requestActivate();
        auto *root = view->rootObject();
        root->setProperty("optionsExpanded", true);
        QTest::qWait(50);
        auto *spin = root->findChild<QObject *>("recordingFps");
        QVERIFY(spin);
        auto *editor = spin->property("contentItem").value<QQuickItem *>();
        QVERIFY(editor);
        editor->forceActiveFocus();
        editor->setProperty("text", "24");
        QTest::keyClick(view, Qt::Key_Return);
        QTRY_COMPARE(controller.recordingFps(), 24);
        QCOMPARE(controller.recordingState(), QString("Selecting"));
        QCOMPARE(barrier.starts, 0);
        QCOMPARE(recorder.starts, 0);
        QCOMPARE(pixels.acquisitions, 0);
        controller.cancel();
    }
    void multiTierToolbarSwitchesActionsWithoutConfirming()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        FakeRecorder recorder;
        FakeAudio audioSources;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker, &recorder, &audioSources);
        QVERIFY(controller.Capture("region").isEmpty());
        auto *selection = selectionModel();
        QVERIFY(selection);
        selection->setRect({100, 100, 120, 80});
        controller.setImageFormat("webp");
        auto *view = selectorView();
        QVERIFY(view);
        QTest::qWait(50);
        auto *root = view->rootObject();
        root->setProperty("optionsExpanded", true);
        QTest::qWait(25);
        auto *tier = root->findChild<QQuickItem *>("modeTier");
        auto *actions = root->findChild<QQuickItem *>("actionTier");
        auto *record = root->findChild<QQuickItem *>("recordAction");
        auto *capture = root->findChild<QQuickItem *>("captureAction");
        QVERIFY(tier && actions && record && capture);
        auto *settingsButton = root->findChild<QQuickItem *>("sessionSettingsButton");
        QVERIFY(!settingsButton);
        QVERIFY(capture->property("text").toString().isEmpty());
        QVERIFY(tier->mapToScene({0, 0}).y() >= actions->mapToScene({0, actions->height()}).y() - 1);
        QVERIFY(root->findChild<QQuickItem *>("qrAction")->isEnabled());
        QVERIFY(root->findChild<QQuickItem *>("ocrAction")->isEnabled());
        controller.setAction("qr");
        QCOMPARE(controller.action(), "qr");
        QCOMPARE(selectorView(), view);
        QCOMPARE(selection->rect(), QRectF(100, 100, 120, 80));
        controller.setAction("ocr");
        QCOMPARE(controller.action(), "ocr");
        QCOMPARE(selectorView(), view);
        controller.setAction("capture");
        QVERIFY(root->findChild<QQuickItem *>("imageFormatSelector")->isVisible());
        QVERIFY(!root->findChild<QQuickItem *>("videoFormatSelector")->isVisible());
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                         record->mapToScene({record->width() / 2, record->height() / 2}).toPoint());
        QVERIFY(controller.recording());
        QVERIFY(controller.live());
        QCOMPARE(selection->rect(), QRectF(100, 100, 120, 80));
        QCOMPARE(pixels.acquisitions, 0);
        QCOMPARE(recorder.starts, 0);
        QTest::qWait(25);
        QVERIFY(!root->findChild<QQuickItem *>("imageFormatSelector")->isVisible());
        QVERIFY(root->findChild<QQuickItem *>("videoFormatSelector")->isVisible());
        controller.setRecordingFormat("webm");
        controller.setRecordingFps(12);
        controller.setCountdownSeconds(0);
        controller.setFilenameTemplate("Toolbar_{datetime:yyyyMMdd_HHmmss}");
        controller.setFilenameTemplate("../invalid");
        QCOMPARE(controller.filenameTemplate(), QString("Toolbar_{datetime:yyyyMMdd_HHmmss}"));
        controller.setSaveDirectory(directory.filePath("configured"));
        auto *audio = root->findChild<QObject *>("audioDropdown");
        QVERIFY(audio);
        QVERIFY(QMetaObject::invokeMethod(audio, "open"));
        QTRY_VERIFY(audio->property("visible").toBool());
        QQuickItem *source = nullptr;
        QTRY_VERIFY((source = visualItem(view->contentItem(), "audioChoice_default:mic")));
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                         source->mapToScene({source->width() / 2, source->height() / 2}).toPoint());
        QTRY_COMPARE(controller.selectedAudioCount(), 1);
        controller.toggleAudioSource("default:system");
        QCOMPARE(controller.selectedAudioCount(), 2);
        QVERIFY(audio->property("visible").toBool()); // Multi-select stays open.
        auto *audioButton = root->findChild<QQuickItem *>("audioSourcesButton");
        QCOMPARE(audioButton->findChildren<QObject *>("dropdownArrow").size(), 1);
        auto *arrow = audioButton->findChild<QObject *>("dropdownArrow");
        QTRY_COMPARE(arrow->property("rotation").toInt(), 180);
        QVERIFY(QMetaObject::invokeMethod(audio, "close"));
        QTRY_VERIFY(!audio->property("visible").toBool());
        QTRY_COMPARE(arrow->property("rotation").toInt(), 0);
        auto *settings = root->findChild<QObject *>("sessionSettings");
        QVERIFY(!settings);
        QVERIFY(controller.recording());
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                         capture->mapToScene({capture->width() / 2, capture->height() / 2}).toPoint());
        QVERIFY(!controller.recording());
        QVERIFY(controller.busy());
        QTRY_COMPARE(barrier.starts, 1);
        QVERIFY(!barrier.allHidden);
        QVERIFY(barrier.allTransparent);
        QCOMPARE(selectorView(), view);
        QCOMPARE(view->rootObject(), root);
        barrier.acknowledge();
        QCOMPARE(pixels.acquisitions, 1);
        pixels.finish(Qt::red);
        QVERIFY(!controller.busy());
        QVERIFY(!controller.live());
        QCOMPARE(selectionModel()->rect(), QRectF(100, 100, 120, 80));
        QCOMPARE(controller.imageFormat(), QString("webp"));
        QCOMPARE(pixels.frozenStarts, 1); // No teardown/restart during mode switches.
        QVERIFY(root->property("optionsExpanded").toBool());
        QCOMPARE(recorder.starts, 0);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        controller.cancel();
    }
    void actionSwitchKeepsWindowIdentityAndWrapsOnNarrowOutputs()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker);
        QVERIFY(controller.Capture("window").isEmpty());
        controller.selectWindow("one");
        windows.finish("one");
        controller.setAction("record");
        QCOMPARE(controller.selectedWindow(), QString("one"));
        QCOMPARE(windows.captures, 1);
        auto *view = selectorView();
        QVERIFY(view);
        view->resize(360, 640);
        QTest::qWait(50);
        auto *root = view->rootObject();
        auto *toolbar = root->findChild<QQuickItem *>("multiTierToolbar");
        QVERIFY(toolbar);
        root->setProperty("optionsExpanded", true);
        QTest::qWait(25);
        for (const auto &name : {"modeTier"}) {
            auto *flow = root->findChild<QQuickItem *>(name);
            QVERIFY(flow);
            for (auto *child : flow->childItems()) {
                if (!child->isVisible()) continue;
                QVERIFY2(child->x() >= 0 && child->x() + child->width() <= flow->width() + 1, name);
            }
        }
        QVERIFY(toolbar->height() > 0);
        view->resize(360, 240);
        QTest::qWait(50);
        auto *modes = root->findChild<QQuickItem *>("modeTier");
        QVERIFY(modes);
        QVERIFY(toolbar->y() >= 0);
        QVERIFY(modes->mapToScene({0, modes->height()}).y() <= view->height());
        controller.setAction("capture");
        QCOMPARE(controller.selectedWindow(), QString("one"));
        QCOMPARE(windows.captures, 2);
        QVERIFY(!controller.canConfirm());
        windows.finish("one");
        QVERIFY(controller.canConfirm());
        QCOMPARE(pixels.acquisitions, 0);
        controller.cancel();
    }
    void toolbarSettingsAffectExportAndLockAfterConfirmation()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker);
        QVERIFY(controller.CaptureWithMode("monitor", true).isEmpty());
        const auto destination = directory.filePath("toolbar-output");
        controller.setSaveDirectory(destination);
        controller.setFilenameTemplate("UI_capture");
        controller.setImageFormat("png");
        controller.setCopyToClipboard(false);
        controller.setNotificationsEnabled(false);
        controller.confirm();
        controller.setSaveDirectory(directory.filePath("wrong"));
        controller.setFilenameTemplate("wrong");
        controller.setImageFormat("jpeg");
        controller.setCopyToClipboard(true);
        QCOMPARE(controller.saveDirectory(), destination);
        QCOMPARE(controller.filenameTemplate(), QString("UI_capture"));
        QCOMPARE(controller.imageFormat(), QString("png"));
        QVERIFY(!controller.copyToClipboard());
        barrier.acknowledge();
        pixels.finish(Qt::green);
        QVERIFY(QFile::exists(QDir(destination).filePath("UI_capture.png")));
        QVERIFY(!QFile::exists(directory.filePath("wrong")));
        QVERIFY(!controller.busy());
    }
    void selectedWindowAutoSelectsOnlyItsAudioAndKeepsManualExtras()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        FakeRecorder recorder;
        FakeAudio audio;
        windows.list[0] = QVariantMap{{"id", "one"}, {"appId", "player"}, {"title", "Music"}, {"label", "Player"}};
        windows.list[1] = QVariantMap{{"id", "two"}, {"appId", "chat"}, {"title", "Call"}, {"label", "Chat"}};
        picker.identities = {{"one", {{"pid", 111}}}, {"two", {{"pid", 222}}}};
        audio.values += QVariantList{
          QVariantMap{{"id", "app:1:player"}, {"kind", "application"}, {"pid", "111"}, {"appId", "player"},
                      {"index", 1}, {"identity", "player"}, {"label", "Player — Music"}, {"icon", "audio"}},
          QVariantMap{{"id", "app:2:chat"}, {"kind", "application"}, {"pid", "222"}, {"appId", "chat"},
                      {"index", 2}, {"identity", "chat"}, {"label", "Chat — Call"}, {"icon", "audio"}}};
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker, &recorder, &audio);
        Config config;
        config.countdown = 0;
        controller.configure(config);
        QVERIFY(controller.Record("window", "mp4", false, false).isEmpty());
        controller.selectWindow("one");
        QCOMPARE(controller.selectedAudioCount(), 1);
        controller.toggleAudioSource("app:1:player"); // A manual uncheck survives metadata refresh.
        QCOMPARE(controller.selectedAudioCount(), 0);
        emit audio.changed();
        QCOMPARE(controller.selectedAudioCount(), 0);
        controller.toggleAudioSource("default:mic");
        controller.selectWindow("two");
        QCOMPARE(controller.selectedAudioCount(), 2);
        controller.selectWindow("one");
        QCOMPARE(controller.selectedAudioCount(), 2);
        controller.confirm();
        barrier.acknowledge();
        windows.finish("one");
        QVERIFY(recorder.options.explicitAudioInputs);
        QStringList selected;
        for (const auto &value : recorder.options.audioInputs) selected += value.toMap().value("id").toString();
        QVERIFY(selected.contains("app:1:player"));
        QVERIFY(selected.contains("default:mic"));
        QVERIFY(!selected.contains("app:2:chat"));
        QVERIFY(!selected.contains("default:system"));
        const auto count = controller.selectedAudioCount();
        controller.toggleAudioSource("default:system"); // Locked after confirmation.
        QCOMPARE(controller.selectedAudioCount(), count);
        controller.cancel();
    }
    void recordingUsesOnlySelectedWindowAndRequiresHiddenBarrier()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker, &recorder);
        Config config;
        config.countdown = 0;
        config.notifications = false;
        controller.configure(config);
        QVERIFY(controller.Record("window", "mp4", false, false).isEmpty());
        QVERIFY(controller.recording());
        controller.selectWindow("one");
        QCOMPARE(windows.captures, 0);
        QCOMPARE(recorder.starts, 0);
        controller.confirm();
        QCOMPARE(barrier.starts, 1);
        QVERIFY(barrier.allHidden);
        QCOMPARE(windows.captures, 0);
        barrier.acknowledge();
        QCOMPARE(windows.captures, 1);
        QCOMPARE(pixels.acquisitions, 0); // Never acquire desktop pixels for a window.
        windows.finish("one");
        QCOMPARE(recorder.starts, 1);
        QCOMPARE(controller.recordingState(), QString("Recording"));
        QTRY_VERIFY(windows.captures > 1);
        windows.finish("two");
        QCOMPARE(recorder.frames, 0);
        windows.finish("one");
        QCOMPARE(recorder.frames, 1);
        QVERIFY(controller.TogglePause().isEmpty());
        QCOMPARE(controller.recordingState(), QString("Paused"));
        const int captures = windows.captures;
        QTest::qWait(60);
        QCOMPARE(windows.captures, captures);
        QVERIFY(controller.TogglePause().isEmpty());
        QCOMPARE(controller.recordingState(), QString("Recording"));
        QVERIFY(windows.captures > captures);
        emit windows.closed("one");
        QCOMPARE(controller.recordingState(), QString("Finalizing"));
        QCOMPARE(recorder.stops, 1);
        QCOMPARE(pixels.acquisitions, 0);
        recorder.finish(directory.filePath("video.mp4"));
        QVERIFY(!controller.recording());
        QVERIFY(!controller.busy());
        QVERIFY(controller.Status().contains("video.mp4"));
        QVERIFY(!controller.Pause().isEmpty());
        QVERIFY(!controller.TogglePause().isEmpty());
    }
    void recordingMonitorLockAndDiscard()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker, &recorder);
        Config config;
        config.countdown = 0;
        config.notifications = false;
        controller.configure(config);
        QVERIFY(controller.Record("monitor", "webm", false, false).isEmpty());
        QVERIFY(controller.canConfirm());
        controller.confirm();
        QVERIFY2(controller.recordingState() == "Preparing", qPrintable(controller.pickMessage()));
        QCOMPARE(pixels.acquisitions, 0);
        barrier.acknowledge();
        QCOMPARE(pixels.acquisitions, 1);
        pixels.finish(Qt::red);
        QCOMPARE(recorder.starts, 1);
        QCOMPARE(recorder.firstFrame.size(), QSize(640, 480));
        QVERIFY(controller.Capture("monitor").isEmpty());
        QVERIFY(selectorView());
        controller.SessionLocked();
        QVERIFY(!selectorView());
        QCOMPARE(recorder.stops, 1);
        QCOMPARE(controller.recordingState(), QString("Finalizing"));
        recorder.finish(directory.filePath("monitor.webm"));
        QVERIFY(!controller.recording());
        QVERIFY(!controller.Record("monitor", "webm", false, false).isEmpty());
        controller.ScreenSaverActive(false);
        QVERIFY(controller.Record("monitor", "webm", false, false).isEmpty());
        controller.confirm();
        barrier.acknowledge();
        pixels.finish(Qt::blue);
        const auto generation = recorder.token;
        controller.cancel();
        QVERIFY(!controller.recording());
        QVERIFY(!controller.busy());
        emit recorder.saved(generation, "stale.webm");
        QVERIFY(!controller.Status().contains("stale.webm"));
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void recordingCountdownCancelsWithoutPixelsAndErrorsRelease()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker, &recorder);
        Config config;
        config.countdown = 3;
        config.notifications = false;
        controller.configure(config);
        QVERIFY(controller.Record("monitor", "mp4", false, false).isEmpty());
        QVERIFY(controller.canConfirm());
        controller.confirm();
        QVERIFY2(controller.recordingState() == "Countdown", qPrintable(controller.pickMessage()));
        QCOMPARE(controller.countdownRemaining(), 3);
        QCOMPARE(controller.recordingState(), QString("Countdown"));
        QCOMPARE(pixels.acquisitions, 0);
        controller.cancel();
        QVERIFY(!controller.recording());
        config.countdown = 0;
        controller.configure(config);
        QVERIFY(controller.Record("monitor", "mp4", false, false).isEmpty());
        controller.confirm();
        barrier.acknowledge();
        pixels.finish(Qt::red);
        emit recorder.failed(recorder.token, "Synthetic encoder failure");
        QVERIFY(!controller.recording());
        QVERIFY(!controller.busy());
        QVERIFY(controller.Status().contains("Synthetic encoder failure"));
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void recordingRejectsCrossOutputRegionButCropsAFixedRegion()
    {
        class TwoOutputs : public FakePixels {
        public:
            void startLive() override {
                emit ready({{{output.name, {0, 0, 320, 480}, 1}, {}},
                            {{output.name, {320, 0, 320, 480}, 1}, {}}});
            }
        } pixels;
        QTemporaryDir directory;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        FakeRecorder recorder;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker, &recorder);
        Config config;
        config.countdown = 0;
        config.notifications = false;
        controller.configure(config);
        QVERIFY(controller.Record("region", "mp4", false, false).isEmpty());
        auto *selection = selectionModel();
        QVERIFY(selection);
        selection->setRect({100, 100, 400, 100}); // Crosses the 320-pixel boundary.
        controller.confirm();
        QCOMPARE(controller.recordingState(), QString("Selecting"));
        QVERIFY(controller.pickMessage().contains("Cross-output"));
        QVERIFY(!controller.busy());
        QCOMPARE(barrier.starts, 0);
        QCOMPARE(pixels.acquisitions, 0);
        selection->setRect({10, 10, 100, 80});
        controller.confirm();
        barrier.acknowledge();
        pixels.finish(Qt::red);
        QCOMPARE(recorder.starts, 1);
        QCOMPARE(recorder.firstFrame.size(), QSize(100, 80));
        QCOMPARE(recorder.firstFrame.pixelColor(50, 40), QColor(Qt::red));
        controller.cancel();
    }
    void configuredCancelShortcutReplacesDefault()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker);
        Config config;
        QString error;
        QVERIFY(config.load(R"({"keybindings":{"cancel":["F8"]}})", directory.path(), &error));
        controller.configure(config);
        QVERIFY(controller.CaptureWithMode("window", true).isEmpty());
        QQuickView *view = nullptr;
        for (auto *window : QGuiApplication::topLevelWindows())
            if (auto *candidate = qobject_cast<QQuickView *>(window); candidate && candidate->isVisible())
                view = candidate;
        QVERIFY(view);
        view->requestActivate();
        QTest::qWait(50);
        QTest::keyClick(view, Qt::Key_Escape);
        QVERIFY(view->isVisible());
        QTest::keyClick(view, Qt::Key_F8);
        QVERIFY(!view->isVisible());
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void hoveringUndimsWithoutChangingTheCaptureTarget()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker);
        QVERIFY(controller.CaptureWithMode("window", true).isEmpty());
        picker.rectangles = {{"one", QRectF(100, 100, 100, 80)}, {"two", QRectF(250, 100, 100, 80)}};
        emit picker.geometryChanged();
        controller.selectWindow("one");
        QTest::qWait(50); // Settle initial offscreen enter/leave events.
        controller.hoverWindowAt(270, 120);
        QTRY_COMPARE(controller.hoveredWindowRect(), QRectF(250, 100, 100, 80));
        QTest::qWait(50);
        for (auto *window : QGuiApplication::topLevelWindows()) {
            auto *view = qobject_cast<QQuickView *>(window);
            if (!view || !view->isVisible()) continue;
            const auto image = view->grabWindow();
            QCOMPARE(image.pixelColor(120, 120).alpha(), 0); // Selection stays clear.
            QCOMPARE(image.pixelColor(270, 120).alpha(), 0); // Hover is also clear.
            QVERIFY(image.pixelColor(400, 220).alpha() > 0);
        }
        QCOMPARE(controller.selectedWindow(), QString("one"));
        QCOMPARE(windows.captures, 0);
        QCOMPARE(picker.clicks, 0); // Motion does not run the async click picker.
        QVERIFY(controller.canConfirm());
        picker.rectangles["two"] = QRectF(150, 100, 100, 80);
        emit picker.geometryChanged();
        controller.hoverWindowAt(220, 120);
        QTRY_COMPARE(controller.hoveredWindowRect(), QRectF(150, 100, 100, 80));
        QTest::qWait(25);
        for (auto *window : QGuiApplication::topLevelWindows()) {
            auto *view = qobject_cast<QQuickView *>(window);
            if (!view || !view->isVisible()) continue;
            const auto image = view->grabWindow();
            QCOMPARE(image.pixelColor(120, 120).alpha(), 0);
            QCOMPARE(image.pixelColor(170, 120).alpha(), 0); // Overlapping clear areas.
            QCOMPARE(image.pixelColor(220, 120).alpha(), 0);
        }
        picker.rectangles["two"] = QRectF(300, 200, 100, 80);
        emit picker.geometryChanged(); // Stationary pointer: recompute on metadata refresh.
        QVERIFY(controller.hoveredWindowRect().isEmpty());
        controller.hoverWindowAt(320, 220);
        QTRY_VERIFY(!controller.hoveredWindowRect().isEmpty());
        controller.clearWindowHover();
        QVERIFY(controller.hoveredWindowRect().isEmpty());
        QCOMPARE(controller.selectedWindow(), QString("one"));
        controller.cancel();
        QTest::qWait(25);
        QVERIFY(controller.hoveredWindowRect().isEmpty());
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
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
        controller.pickWindowAt(50, 60);
        QCOMPARE(picker.clicks, 2); // Retarget directly: no re-arm button.
        emit picker.picked({"one"});
        QCOMPARE(controller.selectedWindow(), QString("one"));
        controller.pickWindowAt(100, 110);
        QCOMPARE(picker.clicks, 3);
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
    void windowSelectionCutsOutDesktopWithoutAPreview()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker);
        QVERIFY(controller.CaptureWithMode("window", true).isEmpty());
        picker.rectangles = {{"one", QRectF(100, 100, 100, 80)}, {"two", QRectF(250, 100, 100, 80)}};
        emit picker.geometryChanged();
        controller.selectWindow("one");
        QCOMPARE(controller.windowRect(), QRectF(100, 100, 100, 80));
        QTest::qWait(50);
        for (auto *window : QGuiApplication::topLevelWindows()) {
            auto *view = qobject_cast<QQuickView *>(window);
            if (!view || !view->isVisible()) continue;
            auto rendered = view->grabWindow();
            QCOMPARE(rendered.pixelColor(120, 120).alpha(), 0);
            QVERIFY(rendered.pixelColor(270, 120).alpha() > 0);
            QVERIFY(rendered.pixelColor(270, 120).alpha() < 100); // Slight default dimming.
        }
        controller.pickWindowAt(270, 120);
        emit picker.picked({"two"});
        QTest::qWait(50);
        for (auto *window : QGuiApplication::topLevelWindows()) {
            auto *view = qobject_cast<QQuickView *>(window);
            if (!view || !view->isVisible()) continue;
            const auto rendered = view->grabWindow();
            QCOMPARE(rendered.pixelColor(270, 120).alpha(), 0);
            QVERIFY(rendered.pixelColor(120, 120).alpha() > 0);
        }
        QCOMPARE(windows.captures, 0);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void monitorsDimOnlyUnselectedOutputsAndHonorTheme()
    {
        QTemporaryDir directory;
        Controller controller(directory.path(), "png", {});
        controller.setMode("monitor");
        Selection selection;
        selection.setBounds({0, 0, 1280, 480});
        selection.setRect({0, 0, 640, 480});
        Theme theme;
        QString error;
        QVERIFY(theme.load(R"({"monitorDimOpacity":0.25})", &error));
        QQmlEngine engine;
        auto *provider = new FrameProvider;
        QImage red(640, 480, QImage::Format_RGB32), blue = red;
        red.fill(Qt::red);
        blue.fill(Qt::blue);
        provider->frames = {{{"left", {0, 0, 640, 480}, 1}, red},
                            {{"right", {640, 0, 640, 480}, 1}, blue}};
        engine.addImageProvider("frames", provider);
        QQuickView view(&engine, nullptr);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(640, 480);
        auto *context = view.rootContext();
        context->setContextProperty("controller", &controller);
        context->setContextProperty("selection", &selection);
        context->setContextProperty("theme", theme.tokens());
        context->setContextProperty("outputIndex", QVariant(0));
        context->setContextProperty("outputRect", QRectF(0, 0, 640, 480));
        view.setSource(QUrl("qrc:/qml/Overlay.qml"));
        QCOMPARE(view.status(), QQuickView::Ready);
        view.show();
        QTest::qWait(50);
        QCOMPARE(view.grabWindow().pixelColor(320, 200), QColor(Qt::red));
        context->setContextProperty("outputIndex", QVariant(1));
        context->setContextProperty("outputRect", QRectF(640, 0, 640, 480));
        QTest::qWait(50);
        const auto dimmed = view.grabWindow().pixelColor(320, 200);
        QVERIFY(dimmed.blue() > 150 && dimmed.blue() < 220);
        QVERIFY(theme.load(R"({"monitorDimOpacity":0.75})", &error));
        context->setContextProperty("theme", theme.tokens());
        QTest::qWait(50);
        QVERIFY(view.grabWindow().pixelColor(320, 200).blue() < dimmed.blue());
        selection.setRect({640, 0, 640, 480});
        QTest::qWait(50);
        QCOMPARE(view.grabWindow().pixelColor(320, 200), QColor(Qt::blue));
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
    }
    void canRetargetWhileAFrozenWindowIsBeingAcquired()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        FakePicker picker;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier, &picker);
        QVERIFY(controller.Capture("window").isEmpty());
        controller.selectWindow("one");
        QVERIFY(controller.busy());
        QVERIFY(controller.canRetarget());
        controller.pickWindowAt(50, 60);
        emit picker.picked({"two"});
        QCOMPARE(controller.selectedWindow(), QString("two"));
        windows.finish("one"); // Old pixels must not replace the new target.
        QVERIFY(!controller.canConfirm());
        windows.finish("two");
        QVERIFY(controller.canConfirm());
        controller.confirm();
        const auto files = QDir(directory.path()).entryList(QDir::Files);
        QCOMPARE(files.size(), 1);
    }
    void overlayLoadsWithSyntheticPixels()
    {
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
    void timingSwitchesKeepTheSameMappedToolbarAndRefreshPixels()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        QVERIFY(controller.Capture("monitor").isEmpty());
        auto *view = selectorView();
        QVERIFY(view);
        auto *root = view->rootObject();
        root->setProperty("optionsExpanded", true);
        root->setProperty("toolbarX", 42);
        root->setProperty("toolbarBottom", 31);
        QSignalSpy visibility(view, &QWindow::visibleChanged);
        QSignalSpy destroyed(root, &QObject::destroyed);
        const auto rect = selectionModel()->rect();
        controller.setLive(true);
        QCOMPARE(pixels.liveStarts, 0);
        QCOMPARE(barrier.starts, 0); // Live needs no capture, unmap or display barrier.
        QVERIFY(controller.live());
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
        controller.setLive(false);
        QCOMPARE(pixels.frozenStarts, 1);
        QCOMPARE(pixels.acquisitions, 0);
        QTRY_COMPARE(barrier.starts, 1);
        QVERIFY(barrier.allTransparent);
        QVERIFY(!barrier.allHidden);
        QCOMPARE(selectorView(), view);
        barrier.acknowledge();
        QCOMPARE(pixels.acquisitions, 1);
        pixels.finish(Qt::green);
        QVERIFY(!controller.live());
        QCOMPARE(controller.frameRevision(), 1);
        QCOMPARE(view->contentItem()->opacity(), 1.0);
        QCOMPARE(view->rootObject(), root);
        QCOMPARE(selectionModel()->rect(), rect);
        QVERIFY(root->property("optionsExpanded").toBool());
        QCOMPARE(root->property("toolbarX").toInt(), 42);
        QCOMPARE(root->property("toolbarBottom").toInt(), 31);
        QTest::qWait(50);
        QCOMPARE(view->grabWindow().pixelColor(5, 5), QColor(Qt::green));
        controller.setLive(true);
        controller.setLive(false);
        QTRY_COMPARE(barrier.starts, 2);
        barrier.acknowledge();
        pixels.finish(Qt::blue);
        QTest::qWait(50);
        QCOMPARE(controller.frameRevision(), 2);
        QCOMPARE(view->grabWindow().pixelColor(5, 5), QColor(Qt::blue));
        QCOMPARE(visibility.count(), 0);
        QCOMPARE(destroyed.count(), 0);
    }
    void failedOrCancelledFreezeRestoresTheExistingSurface()
    {
        QTemporaryDir directory;
        FakePixels pixels;
        FakeWindows windows;
        FakeBarrier barrier;
        Controller controller(directory.path(), "png", {}, nullptr, &pixels, &windows, &barrier);
        QVERIFY(controller.CaptureWithMode("region", true).isEmpty());
        auto *view = selectorView();
        QVERIFY(view);
        auto *root = view->rootObject();
        controller.setLive(false);
        QTRY_COMPARE(barrier.starts, 1);
        emit barrier.failed("Synthetic frame barrier failure");
        QVERIFY(!controller.busy());
        QCOMPARE(view->contentItem()->opacity(), 1.0);
        QCOMPARE(view->rootObject(), root);
        QVERIFY(view->isVisible());
        QCOMPARE(pixels.acquisitions, 0);
        controller.cancel();
        QVERIFY(controller.CaptureWithMode("region", true).isEmpty());
        controller.setLive(false);
        QTRY_COMPARE(barrier.starts, 2);
        controller.cancel();
        barrier.acknowledge();
        QCOMPARE(pixels.acquisitions, 0);
        QVERIFY(QDir(directory.path()).entryList(QDir::Files).isEmpty());
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
