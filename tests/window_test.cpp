#include "windows.h"
#include "ext-toplevel-server.h"
#include "hypr-export-server.h"
#include "hypr-mapping-server.h"
#include "wlr-toplevel-server.h"

#include <QFile>
#include <QSignalSpy>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/socket.h>
#include <wayland-server.h>
#include <vector>

using namespace snip;

// A private synthetic compositor: no connection to the real desktop, no real
// windows, and no output-capture global to fall back to.
class TestCompositor {
public:
    wl_display *display = wl_display_create();
    struct Handle {
        TestCompositor *server;
        int index;
        wl_resource *resource;
    };
    std::vector<std::unique_ptr<Handle>> handles;
    std::unique_ptr<QSocketNotifier> notifier;
    QList<int> captured;
    bool reject = false;
    bool revokeList = false;

    enum class Kind { Unsupported, Hyprland, Ext };
    explicit TestCompositor(Kind kind) {
        wl_display_init_shm(display);
        if (kind == Kind::Hyprland) {
            wl_global_create(display, &hyprland_toplevel_mapping_manager_v1_interface, 1, this,
                             [](wl_client *client, void *, uint32_t, uint32_t id) {
                auto *manager = wl_resource_create(client, &hyprland_toplevel_mapping_manager_v1_interface, 1, id);
                static const struct hyprland_toplevel_mapping_manager_v1_interface impl{
                    .get_window_for_toplevel_wlr = [](wl_client *client, wl_resource *, uint32_t id, wl_resource *toplevel) {
                        auto *handle = static_cast<Handle *>(wl_resource_get_user_data(toplevel));
                        auto *mapping = wl_resource_create(client, &hyprland_toplevel_window_mapping_handle_v1_interface, 1, id);
                        static const struct hyprland_toplevel_window_mapping_handle_v1_interface mappingImpl{
                            .destroy = [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};
                        wl_resource_set_implementation(mapping, &mappingImpl, nullptr, nullptr);
                        hyprland_toplevel_window_mapping_handle_v1_send_window_address(mapping, 0x12345678, uint32_t(handle->index + 1));
                    },
                    .destroy = [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};
                wl_resource_set_implementation(manager, &impl, nullptr, nullptr);
            });
            wl_global_create(display, &zwlr_foreign_toplevel_manager_v1_interface, 3, this,
                             [](wl_client *client, void *data, uint32_t version, uint32_t id) {
                auto *self = static_cast<TestCompositor *>(data);
                auto *manager = wl_resource_create(client, &zwlr_foreign_toplevel_manager_v1_interface, int(version), id);
                static const struct zwlr_foreign_toplevel_manager_v1_interface impl{
                    .stop = [](wl_client *, wl_resource *resource) {
                        zwlr_foreign_toplevel_manager_v1_send_finished(resource);
                        wl_resource_destroy(resource);
                    }};
                wl_resource_set_implementation(manager, &impl, self, nullptr);
                if (self->revokeList) {
                    zwlr_foreign_toplevel_manager_v1_send_finished(manager);
                    wl_resource_destroy(manager);
                    return;
                }
                for (int index = 0; index < 2; ++index) {
                    auto *resource = wl_resource_create(client, &zwlr_foreign_toplevel_handle_v1_interface, int(version), 0);
                    auto handle = std::make_unique<Handle>(Handle{self, index, resource});
                    static const struct zwlr_foreign_toplevel_handle_v1_interface handleImpl{
                        .destroy = [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};
                    wl_resource_set_implementation(resource, &handleImpl, handle.get(), nullptr);
                    zwlr_foreign_toplevel_manager_v1_send_toplevel(manager, resource);
                    zwlr_foreign_toplevel_handle_v1_send_title(resource, "identical title");
                    zwlr_foreign_toplevel_handle_v1_send_app_id(resource, "test");
                    zwlr_foreign_toplevel_handle_v1_send_done(resource);
                    self->handles.push_back(std::move(handle));
                }
            });
            wl_global_create(display, &hyprland_toplevel_export_manager_v1_interface, 2, this,
                             [](wl_client *client, void *data, uint32_t version, uint32_t id) {
                auto *manager = wl_resource_create(client, &hyprland_toplevel_export_manager_v1_interface, int(version), id);
                static const struct hyprland_toplevel_export_manager_v1_interface impl{
                    .destroy = [](wl_client *, wl_resource *r) { wl_resource_destroy(r); },
                    .capture_toplevel_with_wlr_toplevel_handle =
                        [](wl_client *client, wl_resource *, uint32_t id, int32_t, wl_resource *handleResource) {
                        auto *handle = static_cast<Handle *>(wl_resource_get_user_data(handleResource));
                        auto *frame = wl_resource_create(client, &hyprland_toplevel_export_frame_v1_interface, 2, id);
                        static const struct hyprland_toplevel_export_frame_v1_interface frameImpl{
                            .copy = [](wl_client *, wl_resource *frame, wl_resource *buffer, int32_t) {
                                auto *handle = static_cast<Handle *>(wl_resource_get_user_data(frame));
                                handle->server->captured.append(handle->index);
                                if (handle->server->reject) {
                                    hyprland_toplevel_export_frame_v1_send_failed(frame);
                                    return;
                                }
                                auto *shm = wl_shm_buffer_get(buffer);
                                if (!shm) { hyprland_toplevel_export_frame_v1_send_failed(frame); return; }
                                wl_shm_buffer_begin_access(shm);
                                auto *pixels = static_cast<quint32 *>(wl_shm_buffer_get_data(shm));
                                std::fill(pixels, pixels + 4, handle->index ? 0x0000ff00 : 0x00ff0000);
                                wl_shm_buffer_end_access(shm);
                                wl_buffer_send_release(buffer);
                                hyprland_toplevel_export_frame_v1_send_flags(frame, 0);
                                hyprland_toplevel_export_frame_v1_send_ready(frame, 0, 0, 0);
                            },
                            .destroy = [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};
                        wl_resource_set_implementation(frame, &frameImpl, handle, nullptr);
                        hyprland_toplevel_export_frame_v1_send_buffer(frame, WL_SHM_FORMAT_XRGB8888, 2, 2, 8);
                        hyprland_toplevel_export_frame_v1_send_buffer_done(frame);
                    }};
                wl_resource_set_implementation(manager, &impl, data, nullptr);
            });
        } else if (kind == Kind::Ext) {
            // Only advertisement is needed: grim owns the source/session path.
            static const wl_interface source{"ext_foreign_toplevel_image_capture_source_manager_v1", 1, 0, nullptr, 0, nullptr};
            static const wl_interface copy{"ext_image_copy_capture_manager_v1", 1, 0, nullptr, 0, nullptr};
            auto unusedBind = [](wl_client *, void *, uint32_t, uint32_t) {};
            wl_global_create(display, &source, 1, this, unusedBind);
            wl_global_create(display, &copy, 1, this, unusedBind);
            wl_global_create(display, &ext_foreign_toplevel_list_v1_interface, 1, this,
                             [](wl_client *client, void *, uint32_t, uint32_t id) {
                auto *manager = wl_resource_create(client, &ext_foreign_toplevel_list_v1_interface, 1, id);
                static const struct ext_foreign_toplevel_list_v1_interface impl{
                    .stop = [](wl_client *, wl_resource *r) { ext_foreign_toplevel_list_v1_send_finished(r); },
                    .destroy = [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};
                wl_resource_set_implementation(manager, &impl, nullptr, nullptr);
                auto *handle = wl_resource_create(client, &ext_foreign_toplevel_handle_v1_interface, 1, 0);
                static const struct ext_foreign_toplevel_handle_v1_interface handleImpl{
                    .destroy = [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};
                wl_resource_set_implementation(handle, &handleImpl, nullptr, nullptr);
                ext_foreign_toplevel_list_v1_send_toplevel(manager, handle);
                ext_foreign_toplevel_handle_v1_send_identifier(handle, "opaque-target-1");
                ext_foreign_toplevel_handle_v1_send_title(handle, "<b>untrusted title</b>");
                ext_foreign_toplevel_handle_v1_send_app_id(handle, "test");
                ext_foreign_toplevel_handle_v1_send_done(handle);
            });
        }
        auto *loop = wl_display_get_event_loop(display);
        notifier = std::make_unique<QSocketNotifier>(wl_event_loop_get_fd(loop), QSocketNotifier::Read);
        QObject::connect(notifier.get(), &QSocketNotifier::activated, notifier.get(), [this, loop] {
            wl_event_loop_dispatch(loop, 0);
            wl_display_flush_clients(display);
        });
    }
    void connectClient() {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) qFatal("socketpair failed");
        wl_client_create(display, sockets[0]);
        qputenv("WAYLAND_SOCKET", QByteArray::number(sockets[1]));
    }
    ~TestCompositor() {
        notifier.reset();
        wl_display_destroy_clients(display);
        wl_display_destroy(display);
        qunsetenv("WAYLAND_SOCKET");
    }
};

class WindowTest : public QObject {
    Q_OBJECT
private slots:
    void revocationDuringDiscoveryIsNotClearedBySync()
    {
        TestCompositor compositor(TestCompositor::Kind::Hyprland);
        compositor.revokeList = true;
        compositor.connectClient();
        WaylandWindows windows;
        windows.start();
        QTRY_VERIFY(windows.unavailableReason().contains("revoked"));
        QTest::qWait(50);
        QVERIFY(windows.unavailableReason().contains("revoked"));
        QVERIFY(windows.windows().isEmpty());
    }
    void unsupportedProtocolsFailClosed()
    {
        TestCompositor compositor(TestCompositor::Kind::Unsupported);
        compositor.connectClient();
        WaylandWindows windows;
        windows.start();
        QTRY_VERIFY(windows.unavailableReason().startsWith("Isolated windows require"));
        QVERIFY(windows.windows().isEmpty());
        QSignalSpy failed(&windows, &WindowBackend::failed);
        windows.capture("nonexistent");
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(compositor.captured.isEmpty());
    }
    void nativeHyprlandUsesExactHandle()
    {
        TestCompositor compositor(TestCompositor::Kind::Hyprland);
        compositor.connectClient();
        WaylandWindows windows;
        windows.start();
        QTRY_COMPARE(windows.windows().size(), 2);
        QVERIFY(windows.unavailableReason().isEmpty());
        QTRY_COMPARE(windows.windows()[1].toMap().value("address").toString(), QString("1234567800000002"));
        const auto target = windows.windows()[1].toMap().value("id").toString();
        QSignalSpy images(&windows, &WindowBackend::imageReady);
        QSignalSpy failures(&windows, &WindowBackend::failed);
        windows.capture(target);
        QTRY_COMPARE(images.count(), 1);
        QCOMPARE(compositor.captured, QList<int>{1});
        QCOMPARE(qvariant_cast<QImage>(images.first()[1]).pixelColor(0, 0), QColor(Qt::green));
        QCOMPARE(failures.count(), 0);
        windows.capture(target);
        windows.cancelCapture();
        QTest::qWait(50);
        QCOMPARE(images.count(), 1);
        // Permission/encoder-style failure must never yield another source.
        compositor.reject = true;
        windows.capture(target);
        QTRY_COMPARE(failures.count(), 1);
        QCOMPARE(images.count(), 1);
        QSignalSpy closed(&windows, &WindowBackend::closed);
        zwlr_foreign_toplevel_handle_v1_send_closed(compositor.handles[1]->resource);
        wl_display_flush_clients(compositor.display);
        QTRY_COMPARE(closed.count(), 1);
        QCOMPARE(windows.windows().size(), 1);
        windows.capture(target);
        QTRY_COMPARE(failures.count(), 2);
        QCOMPARE(compositor.captured.size(), 2);
        windows.stop();
    }
    void unsupportedProtocolsAreExplicit()
    {
        TestCompositor compositor(TestCompositor::Kind::Unsupported);
        compositor.connectClient();
        WaylandWindows windows;
        windows.start();
        QTRY_VERIFY(windows.unavailableReason().contains("require"));
        QVERIFY(windows.windows().isEmpty());
        QSignalSpy failures(&windows, &WindowBackend::failed);
        windows.capture("anything");
        QTRY_COMPARE(failures.count(), 1);
    }
    void extUsesStableIdentifierAndArgumentList()
    {
        TestCompositor compositor(TestCompositor::Kind::Ext);
        compositor.connectClient();
        WaylandWindows windows;
        windows.start();
        QTRY_COMPARE(windows.windows().size(), 1);
        QTemporaryDir directory;
        QFile executable(directory.filePath("grim"));
        QVERIFY(executable.open(QIODevice::WriteOnly));
        executable.write("#!/bin/sh\n"
                         "[ \"$1\" = '-T' ] && [ \"$2\" = 'opaque-target-1' ] && "
                         "[ \"$3\" = '-t' ] && [ \"$4\" = 'ppm' ] && [ \"$5\" = '-' ] || exit 9\n"
                         "printf 'P3\\n1 1\\n255\\n0 0 255\\n'\n");
        executable.close();
        executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const auto oldPath = qgetenv("PATH");
        qputenv("PATH", directory.path().toUtf8());
        QSignalSpy images(&windows, &WindowBackend::imageReady);
        windows.capture(windows.windows().first().toMap().value("id").toString());
        QTRY_COMPARE(images.count(), 1);
        qputenv("PATH", oldPath);
        QCOMPARE(qvariant_cast<QImage>(images.first()[1]).pixelColor(0, 0), QColor(Qt::blue));
        windows.stop();
    }
};

QTEST_GUILESS_MAIN(WindowTest)
#include "window_test.moc"
