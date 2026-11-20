#include "controller.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickItem>
#include <QScreen>
#include <QDir>
#include <QSvgRenderer>
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
    void initTestCase() { QQuickStyle::setStyle("Basic"); }
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
        QVERIFY(settingsButton && settingsButton->parentItem() == tier);
        QVERIFY(capture->property("text").toString().isEmpty());
        QVERIFY(tier->mapToScene({0, 0}).y() >= actions->mapToScene({0, actions->height()}).y() - 1);
        QVERIFY(!root->findChild<QQuickItem *>("qrAction")->isEnabled());
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
        QVERIFY(settings);
        QVERIFY(QMetaObject::invokeMethod(settings, "open"));
        QTRY_VERIFY(settings->property("visible").toBool());
        QTRY_VERIFY(settings->property("activeFocus").toBool());
        QTest::keyClick(view, Qt::Key_Escape); // Close settings, not the entire capture.
        QTRY_VERIFY(!settings->property("visible").toBool());
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
        controller.SessionLocked();
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
