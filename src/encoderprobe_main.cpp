#include <QCoreApplication>
#include <QTextStream>
#include <gst/app/gstappsrc.h>
#include <atomic>

// Internal helper: synthetic encoder preflight only. No Wayland, audio, shell
// execution or output files. The parent kills this process after five seconds.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  const auto reject = [](const QString &message) {
    QTextStream(stderr) << message << '\n';
    return 1;
  };
  if (args.size() != 7) return reject("Expected encoder, codec, container, width, height and FPS.");
  const auto factory = args[1], codec = args[2], container = args[3];
  if (!QStringList{"h264", "h265", "vp8", "vp9", "av1"}.contains(codec) ||
      !QStringList{"va" + codec + "enc", "nv" + codec + "enc", "qsv" + codec + "enc"}.contains(factory) ||
      !QStringList{"mp4", "webm"}.contains(container))
    return reject("Unsupported encoder preflight arguments.");
  bool widthOk, heightOk, fpsOk;
  const auto width = args[4].toInt(&widthOk), height = args[5].toInt(&heightOk), fps = args[6].toInt(&fpsOk);
  if (!widthOk || !heightOk || !fpsOk || width < 2 || height < 2 ||
      width > 8192 || height > 8192 || qint64(width) * height > 16'777'216 ||
      width % 2 || height % 2 || fps < 1 || fps > 120) return reject("Invalid encoder preflight dimensions/FPS.");
  if (width < 32 || height < 32)
    return reject("Hardware encoding requires at least 32x32. Use software for smaller regions.");
  // Some VA-API HEVC plugin/driver combinations assert for sub-CTU frames,
  // despite advertising smaller sizes. Never risk that path in the resident.
  if (codec == "h265" && (width < 128 || height < 128))
    return reject("Hardware HEVC requires at least 128x128. Use software for smaller regions.");
  gst_init(nullptr, nullptr);
  QString chain = factory;
  if (codec == "h264") chain += " ! h264parse";
  if (codec == "h265") chain += " ! h265parse";
  if (codec == "av1") chain += " ! av1parse";
  if (codec == "h265" && container == "mp4") chain += " ! video/x-h265,stream-format=hvc1";
  const auto description = QString(
      "appsrc name=probe format=time caps=video/x-raw,format=RGBA,width=%1,height=%2,framerate=%3/1 "
      "! videoconvert ! video/x-raw,format=NV12 ! %4 ! fakesink name=probeOutput sync=false")
      .arg(width).arg(height).arg(fps).arg(chain);
  GError *error = nullptr;
  auto *pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
  if (error || !pipeline) {
    const auto message = error ? QString::fromUtf8(error->message) : "Cannot create hardware probe.";
    if (error) g_error_free(error);
    if (pipeline) gst_object_unref(pipeline);
    return reject(message);
  }
  auto *source = gst_bin_get_by_name(GST_BIN(pipeline), "probe");
  auto *output = gst_bin_get_by_name(GST_BIN(pipeline), "probeOutput");
  auto *pad = gst_element_get_static_pad(output, "sink");
  std::atomic_bool encoded{false};
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, [](GstPad *, GstPadProbeInfo *, gpointer data) {
    static_cast<std::atomic_bool *>(data)->store(true);
    return GST_PAD_PROBE_OK;
  }, &encoded, nullptr);
  gst_object_unref(pad);
  gst_object_unref(output);
  auto *bus = gst_element_get_bus(pipeline);
  const auto state = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  auto *buffer = gst_buffer_new_allocate(nullptr, gsize(width) * height * 4, nullptr);
  if (buffer) {
    gst_buffer_memset(buffer, 0, 0, gst_buffer_get_size(buffer));
    GST_BUFFER_PTS(buffer) = 0;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / fps;
  }
  const auto flow = buffer ? gst_app_src_push_buffer(GST_APP_SRC(source), buffer) : GST_FLOW_ERROR;
  gst_app_src_end_of_stream(GST_APP_SRC(source));
  gst_object_unref(source);
  auto *message = state != GST_STATE_CHANGE_FAILURE && flow == GST_FLOW_OK
      ? gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)) : nullptr;
  const bool success = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS && encoded.load();
  QString reason = "Hardware encoder initialization failed or timed out.";
  if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    gst_message_parse_error(message, &error, nullptr);
    if (error) { reason = QString::fromUtf8(error->message); g_error_free(error); }
  }
  if (message) gst_message_unref(message);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  return success ? 0 : reject(reason);
}
