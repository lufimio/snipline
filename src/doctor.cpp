#include "doctor.h"
#include "core/outputs.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <cerrno>
#include <poll.h>
#include <wayland-client.h>

namespace snip {
namespace {

QJsonObject protocols() {
  QJsonObject found;
  auto *display = wl_display_connect(nullptr);
  if (!display)
    return {{"connected", false}};
  auto *registry = wl_display_get_registry(display);
  static const wl_registry_listener listener = {
      [](void *data, wl_registry *, uint32_t, const char *name,
         uint32_t version) {
        auto *object = static_cast<QJsonObject *>(data);
        const QString interface = QString::fromUtf8(name);
        if (interface == "zwlr_layer_shell_v1" ||
            interface == "zwlr_screencopy_manager_v1" ||
            interface == "ext_image_copy_capture_manager_v1" ||
            interface == "ext_foreign_toplevel_image_capture_source_manager_v1" ||
            interface == "hyprland_toplevel_export_manager_v1" ||
            interface == "hyprland_toplevel_mapping_manager_v1" ||
            interface == "ext_foreign_toplevel_list_v1" ||
            interface == "zwlr_foreign_toplevel_manager_v1")
          object->insert(interface, int(version));
      },
      [](void *, wl_registry *, uint32_t) {}};
  wl_registry_add_listener(registry, &listener, &found);
  bool done = false;
  auto *sync = wl_display_sync(display);
  static const wl_callback_listener callback = {
      [](void *data, wl_callback *, uint32_t) {
        *static_cast<bool *>(data) = true;
      }};
  wl_callback_add_listener(sync, &callback, &done);
  QElapsedTimer deadline;
  deadline.start();
  while (!done && deadline.elapsed() < 1500) {
    while (wl_display_prepare_read(display) != 0) {
      if (wl_display_dispatch_pending(display) < 0)
        break;
    }
    if (wl_display_get_error(display))
      break;
    const int flush = wl_display_flush(display);
    pollfd fd{wl_display_get_fd(display),
              short(POLLIN | (flush < 0 && errno == EAGAIN ? POLLOUT : 0)), 0};
    const int result =
        poll(&fd, 1, int(qMax<qint64>(0, 1500 - deadline.elapsed())));
    if (result > 0 && (fd.revents & POLLIN)) {
      if (wl_display_read_events(display) < 0)
        break;
      if (wl_display_dispatch_pending(display) < 0)
        break;
    } else {
      wl_display_cancel_read(display);
      if (result <= 0 || (fd.revents & (POLLERR | POLLHUP)))
        break;
    }
  }
  found["connected"] = done;
  wl_callback_destroy(sync);
  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return found;
}

bool hasElement(const QString &name) {
  if (QStandardPaths::findExecutable("gst-inspect-1.0").isEmpty())
    return false;
  QProcess process;
  process.start("gst-inspect-1.0", {name}, QIODevice::ReadOnly);
  if (!process.waitForFinished(2000)) {
    process.kill();
    process.waitForFinished(1000);
    return false;
  }
  return process.exitStatus() == QProcess::NormalExit &&
         process.exitCode() == 0;
}

} // namespace

int doctor() {
  const auto compositor = currentCompositor();
  QJsonObject dependencies;
  for (const auto *program : {"grim", "hyprctl", "niri", "gst-inspect-1.0",
                              "tesseract", "satty", "ffmpeg", "ffprobe", "pactl"})
    dependencies[program] = !QStandardPaths::findExecutable(program).isEmpty();
  QJsonObject codecs;
  for (const auto *element :
        {"pipewiresrc", "appsrc", "pulsesrc", "audiomixer", "x264enc", "h264parse",
         "mp4mux", "vp8enc", "webmmux", "opusenc", "avenc_aac",
         "x265enc", "h265parse", "vp9enc", "av1enc", "av1parse",
         "vaapih264enc", "vah264enc", "vah265enc", "vavp8enc", "vavp9enc", "vaav1enc",
         "nvh264enc", "nvh265enc", "nvav1enc", "qsvh264enc", "qsvh265enc", "qsvav1enc", "qsvvp9enc"})
    codecs[element] = hasElement(element);
  QJsonArray formats;
  for (const auto &format : QImageWriter::supportedImageFormats())
    formats.append(QString::fromLatin1(format));
  const auto wayland = protocols();
  QProcess grimHelp;
  grimHelp.start("grim", {"-h"}, QIODevice::ReadOnly);
  const bool helpFinished = grimHelp.waitForFinished(1500);
  const bool grimToplevel = helpFinished && grimHelp.exitCode() == 0
      && grimHelp.readAllStandardOutput().contains("-T <identifier>");
  if (!helpFinished) { grimHelp.kill(); grimHelp.waitForFinished(1000); }
  const QJsonObject windowPaths{
      {"hyprlandNative", wayland.value("hyprland_toplevel_export_manager_v1").toInt() >= 2
          && wayland.value("zwlr_foreign_toplevel_manager_v1").toInt() >= 3},
      {"standardExt", wayland.contains("ext_foreign_toplevel_list_v1")
          && wayland.contains("ext_foreign_toplevel_image_capture_source_manager_v1")
          && wayland.contains("ext_image_copy_capture_manager_v1") && grimToplevel},
      {"grimToplevelOption", grimToplevel}};
  auto bus = QDBusConnection::sessionBus();
  bool portalRegistered = false, portalActivatable = false;
  if (bus.isConnected() && bus.interface()) {
    portalRegistered =
        bus.interface()
            ->isServiceRegistered("org.freedesktop.portal.Desktop")
            .value();
    const QDBusReply<QStringList> reply =
        bus.interface()->call("ListActivatableNames");
    portalActivatable =
        reply.isValid() &&
        reply.value().contains("org.freedesktop.portal.Desktop");
  }
  const QJsonObject report{
      {"version", "0.1.0"},
      {"compositor", compositorName(compositor)},
      {"dependencies", dependencies},
      {"wayland", wayland},
      {"advertisedWindowCapturePaths", windowPaths},
      {"sessionBus", bus.isConnected()},
      {"portalRegistered", portalRegistered},
      {"portalActivatable", portalActivatable},
      {"desktopEntryFound", !QStandardPaths::locate(QStandardPaths::ApplicationsLocation, "snipline.desktop").isEmpty()},
      {"imageEncoders", formats},
      {"gstreamerElements", codecs},
      {"implemented", QJsonObject{{"frozenRegion", true},
                                  {"frozenMonitor", true},
                                   {"live", true},
                                   {"window", true},
                                   {"portal", false},
                                   {"qr", true}, {"ocr", true}, {"editors", true},
                                    {"uploads", true}, {"lens", true},
                                    {"crossOutputScreenshots", true},
                                    {"optionalTray", true}, {"recentMetadata", true},
                                    {"recording", true}}},
        {"desktopIntegration", QJsonObject{{"tray", "opt-in --tray; requires a desktop tray host"},
                                          {"recentLimit", 32}, {"recentPersistence", false},
                                          {"automaticStartupEnabled", false}}},
        {"recognition", QJsonObject{{"barcodeBackend", "ZXing-C++ in separate worker"},
                                   {"workerFound", QFileInfo(QDir(QCoreApplication::applicationDirPath())
                                                              .filePath("snipline-recognize")).isExecutable()},
                                   {"ocrBackend", "asynchronous Tesseract CLI; installed language selection"},
                                   {"automaticLinkOpening", false}, {"networkTransmission", false}}},
       {"uploads", QJsonObject{{"backend", "libcurl multipart file upload"},
                                {"automaticUploads", false}, {"extraConfirmationDialogs", false},
                                {"profiles", QJsonArray{"catbox", "litterbox-1h", "litterbox-12h", "litterbox-24h", "litterbox-72h", "pomf"}},
                                {"lens", "explicit image upload followed by lens.google.com/uploadbyurl"}}},
       {"recording", QJsonObject{{"source", "repeated native capture; no desktop fallback for windows"},
                                  {"crossOutput", false}, {"hardwareEncoding", true},
                                  {"hardwareProbe", "separate synthetic probe process before recording; software fallback only in auto mode"},
                                  {"encoderProbeFound", QFileInfo(QDir(QCoreApplication::applicationDirPath())
                                                                     .filePath("snipline-encoder-probe")).isExecutable()},
                                  {"encoders", QJsonArray{"software", "auto", "vaapi", "nvenc", "qsv"}},
                                  {"codecs", QJsonArray{"h264", "h265", "vp8", "vp9", "av1"}},
                                 {"formats", QJsonArray{"mp4", "webm", "gif"}},
                                  {"audio", "PulseAudio/PipeWire-Pulse devices and verified application streams"},
                                  {"applicationAudioResampling", "native output-rate float PCM; each input normalized to 48 kHz"},
                                  {"pausedApplicationAudio", "clocked silence; no replacement audio device"}}},
      {"note", "Protocol advertisement and installed encoders are not "
               "end-to-end validation. No pixels or streams were requested."}};
  QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
  return 0;
}

} // namespace snip
