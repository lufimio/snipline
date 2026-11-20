#include "controller.h"
#include "doctor.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QGuiApplication>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

namespace {
constexpr auto service = "io.github.snipline.Application";
constexpr auto objectPath = "/io/github/snipline/Application";

void configure(QCommandLineParser &parser) {
  parser.setApplicationDescription(
      "Snipline — an early Wayland screenshot selector");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(
      "command", "capture (default), daemon, cancel, quit, doctor",
      "[command]");
  parser.addPositionalArgument(
      "target", "region (default), monitor or window; capture only", "[target]");
  parser.addOption({"live", "Select over the live desktop; acquire pixels only after confirmation."});
  parser.addOption(
      {"directory", "Capture directory (resident instance setting).", "path"});
  parser.addOption({"format", "png, jpeg, or webp (resident instance setting).",
                    "format", "png"});
  parser.addOption(
      {"theme", "Data-only JSON theme (resident instance setting).", "path"});
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication::setApplicationName("snipline");
  QCoreApplication::setApplicationVersion("0.1.0");
  QCoreApplication::setOrganizationName("Snipline");
  // Diagnostics and help must also work without a graphical display.
  bool headless = false;
  for (int i = 1; i < argc; ++i) {
    const QByteArray arg(argv[i]);
    if (arg == "doctor" || arg == "quit" || arg == "cancel" ||
        arg == "--help" || arg == "--help-all" || arg == "-h" ||
        arg == "--version" || arg == "-v")
      headless = true;
  }
  std::unique_ptr<QCoreApplication> app;
  if (headless)
    app = std::make_unique<QCoreApplication>(argc, argv);
  else {
    QQuickStyle::setStyle("Basic");
    app = std::make_unique<QGuiApplication>(argc, argv);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QGuiApplication::setDesktopFileName("snipline");
  }
  QCommandLineParser parser;
  configure(parser);
  parser.process(*app);
  auto fail = [](const QString &message) {
    QTextStream(stderr) << message << '\n';
    return 1;
  };
  const auto args = parser.positionalArguments();
  const QString command = args.value(0, "capture");
  if (args.size() > 2 || (command != "capture" && args.size() > 1))
    return fail("Unexpected arguments. See --help.");
  if (command == "doctor")
    return snip::doctor();
  if (command != "capture" && command != "daemon" && command != "quit" &&
      command != "cancel")
    return fail("Unknown command. See --help.");
  const QString target = args.value(1, "region");
  if (command == "capture" && target != "region" && target != "monitor" && target != "window")
    return fail("Capture targets are region, monitor and window.");
  if (command != "capture" && parser.isSet("live"))
    return fail("--live is only valid with capture.");
  const auto format = parser.value("format").toLatin1();
  if (format != "png" && format != "jpeg" && format != "webp")
    return fail("Format must be png, jpeg, or webp.");
  auto bus = QDBusConnection::sessionBus();
  if (!bus.isConnected())
    return fail("A session D-Bus is required for single-instance operation.");
  if (bus.interface()->isServiceRegistered(service).value()) {
    if (parser.isSet("directory") || parser.isSet("format") ||
        parser.isSet("theme"))
      return fail("An instance is running. Quit it before changing directory, "
                  "format, or theme.");
    if (command == "daemon")
      return 0;
    QDBusInterface remote(service, objectPath, service, bus);
    if (command == "quit" || command == "cancel") {
      const auto reply = remote.call(command == "quit" ? "Quit" : "Cancel");
      return reply.type() == QDBusMessage::ErrorMessage
                 ? fail(reply.errorMessage())
                 : 0;
    }
    const QDBusReply<QString> reply = remote.call("CaptureWithMode", target, parser.isSet("live"));
    if (!reply.isValid())
      return fail(reply.error().message());
    return reply.value().isEmpty() ? 0 : fail(reply.value());
  }
  if (command == "quit" || command == "cancel")
    return 0;
  if (!qobject_cast<QGuiApplication *>(app.get()))
    return fail("Capture requires a graphical session.");

  QString directory = parser.value("directory");
  if (directory.isEmpty())
    directory =
        QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
            .filePath("Snipline");
  snip::Controller controller(QDir(directory).absolutePath(), format,
                              parser.value("theme"));
  if (!bus.registerObject(objectPath, &controller,
                          QDBusConnection::ExportScriptableSlots) ||
      !bus.registerService(service))
    return fail("Could not register the single-instance service. Try again.");
  if (command == "capture") {
    QTimer::singleShot(0, &controller, [&controller, target, live = parser.isSet("live")] {
      const auto error = controller.CaptureWithMode(target, live);
      if (!error.isEmpty())
        QTextStream(stderr) << error << '\n';
    });
  }
  return app->exec();
}
