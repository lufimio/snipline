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
#include <QSocketNotifier>
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr auto service = "io.github.snipline.Application";
constexpr auto objectPath = "/io/github/snipline/Application";
volatile std::sig_atomic_t terminationPipe = -1;
void terminationSignal(int) {
  const int savedErrno = errno;
  if (terminationPipe >= 0) {
    const char value = 1;
    const auto ignored = ::write(int(terminationPipe), &value, 1);
    (void)ignored;
  }
  errno = savedErrno;
}

// Async-signal-safe self-pipe; all actual cleanup runs on Qt's event loop.
class TerminationSignals {
public:
  explicit TerminationSignals(snip::Controller &controller) {
    if (::pipe2(m_fds, O_NONBLOCK | O_CLOEXEC) != 0) return;
    terminationPipe = m_fds[1];
    struct sigaction action{};
    action.sa_handler = terminationSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, &m_oldInt);
    sigaction(SIGTERM, &action, &m_oldTerm);
    m_notifier = std::make_unique<QSocketNotifier>(m_fds[0], QSocketNotifier::Read);
    QObject::connect(m_notifier.get(), &QSocketNotifier::activated, &controller, [&controller, this] {
      char buffer[32];
      while (::read(m_fds[0], buffer, sizeof(buffer)) > 0) {}
      controller.Quit(); // Finalize a running video, discard an unstarted selection.
    });
  }
  ~TerminationSignals() {
    if (m_fds[0] < 0) return;
    sigaction(SIGINT, &m_oldInt, nullptr);
    sigaction(SIGTERM, &m_oldTerm, nullptr);
    terminationPipe = -1;
    m_notifier.reset();
    ::close(m_fds[0]);
    ::close(m_fds[1]);
  }
private:
  int m_fds[2]{-1, -1};
  struct sigaction m_oldInt{}, m_oldTerm{};
  std::unique_ptr<QSocketNotifier> m_notifier;
};

void configure(QCommandLineParser &parser, const QString &scope = {}) {
  parser.setApplicationDescription(
      "Snipline — Wayland screenshots and recording");
  parser.addHelpOption();
  parser.addVersionOption();
  if (scope.isEmpty() || scope == "capture") {
    parser.addOption({"live", "Select over the live desktop; acquire pixels only "
                             "after confirmation."});
    parser.addOption(
        {"frozen", "Select over a frozen frame, overriding config."});
  }
  parser.addOption({"config",
                    "JSON config (default: ~/.config/snipline/config.json).",
                    "path"});
  parser.addOption(
      {"directory", "Capture directory (resident instance setting).", "path"});
  if (scope.isEmpty() || scope == "capture" || scope == "daemon")
    parser.addOption(
        {"format", "png, jpeg, or webp (resident instance setting).", "format"});
  parser.addOption(
      {"theme", "Data-only JSON theme (resident instance setting).", "path"});
  if (scope.isEmpty() || scope == "record") {
    parser.addOption({"video-format", "Recording format: mp4, webm or gif.", "format"});
    parser.addOption({"system-audio", "Record system audio (record only)."});
    parser.addOption({"microphone", "Record microphone (record only)."});
    parser.addOption({"no-system-audio", "Disable configured system audio (record only)."});
    parser.addOption({"no-microphone", "Disable configured microphone (record only)."});
  }
}

bool showCommandHelp(QStringList path) {
  if (path.value(0) == "help") path.removeFirst();
  const auto root = path.value(0);
  const auto child = path.value(1);
  const QStringList targets{"region", "monitor", "window"};
  const QStringList controls{"pause", "resume", "stop", "discard", "status"};
  const QStringList roots{"capture", "record", "daemon", "cancel", "quit", "doctor"};
  if (path.size() > 2 || (!root.isEmpty() && !roots.contains(root)) ||
      (!child.isEmpty() && (root != "capture" && root != "record")) ||
      (!child.isEmpty() && !targets.contains(child) && !(root == "record" && controls.contains(child))))
    return false;
  QTextStream out(stdout);
  if (root.isEmpty()) {
    out << "Usage: snipline <command> [options]\n"
           "Snipline — Wayland screenshots and recording\n\n"
           "Commands:\n"
           "  capture                 Select a screenshot target (default command)\n"
           "    region                Capture a region\n"
           "    monitor               Capture a monitor\n"
           "    window                Capture an isolated window\n"
           "  record                  Select a recording target\n"
           "    region                Record a fixed, single-output region\n"
           "    monitor               Record a monitor\n"
           "    window                Record an isolated window\n"
           "    pause                 Toggle recording between running and paused\n"
           "    resume                Alias for record pause (also a toggle)\n"
           "    stop                  Finalize and save the recording\n"
           "    discard               Discard the recording or its selection\n"
           "    status                Show recording state and elapsed time as JSON\n"
           "  daemon                  Start the resident instance\n"
           "  cancel                  Cancel the active selection or discard recording\n"
           "  quit                    Finalize recording and quit the resident instance\n"
           "  doctor                  Print capability diagnostics\n"
           "  help <command>          Show command-specific help\n\n";
  } else if ((root == "capture" || root == "record") && child.isEmpty()) {
    out << "Usage: snipline " << root << " <subcommand> [options]\n\nSubcommands:\n"
           "  region                  Select a region\n"
           "  monitor                 Select a monitor\n"
           "  window                  Select an isolated window\n";
    if (root == "record")
      out << "  pause                   Toggle recording between running and paused\n"
             "  resume                  Alias for pause (also a toggle)\n"
             "  stop                    Finalize and save\n"
             "  discard                 Discard recording or recording selection\n"
             "  status                  Show recording state as JSON\n";
    out << "\nOmitting the target uses the configured default.\n\n";
  } else {
    out << "Usage: snipline " << root;
    if (!child.isEmpty()) out << ' ' << child;
    out << " [options]\n\n";
    if (child == "pause" || child == "resume")
      out << "Toggle the active recording: running → paused, paused → running.\n"
             "Both pause and resume invoke the same toggle. No active recording is an error.\n\n";
    if (child == "stop") out << "Finalize and save the active recording.\n\n";
    if (child == "discard") out << "Discard the recording or recording selection without saving.\n\n";
    if (child == "status") out << "Print JSON recording state, elapsed time and output/error details.\n\n";
  }
  if (root.isEmpty() || root == "daemon" || root == "capture" ||
      (root == "record" && !controls.contains(child))) {
    QCommandLineParser options;
    configure(options, root);
    const auto nativeHelp = options.helpText();
    out << nativeHelp.mid(nativeHelp.indexOf("Options:\n"));
  } else out << "Options:\n  -h, --help              Show this help\n";
  out << "\nUse 'snipline help record' or 'snipline record <subcommand> --help' for details.\n";
  return true;
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication::setApplicationName("snipline");
  QCoreApplication::setApplicationVersion("0.1.0");
  QCoreApplication::setOrganizationName("Snipline");
  auto fail = [](const QString &message) {
    QTextStream(stderr) << message << '\n';
    return 1;
  };
  QCommandLineParser parser;
  configure(parser);
  QStringList arguments;
  for (int i = 0; i < argc; ++i) arguments.append(QString::fromLocal8Bit(argv[i]));
  if (!parser.parse(arguments)) return fail(parser.errorText());
  const auto args = parser.positionalArguments();
  const auto root = args.value(0, "capture");
  const auto action = args.value(1);
  const bool help = parser.isSet("help") || parser.isSet("help-all") || root == "help";
  const QStringList recordControls{"pause", "resume", "stop", "discard", "status"};
  const bool recordControl = root == "record" && recordControls.contains(action);
  const QString command = recordControl ? ((action == "pause" || action == "resume") ? "toggle" : action) : root;
  const bool selecting = command == "capture" || (command == "record" && !recordControl);
  const bool control = command == "quit" || command == "cancel" || recordControl;
  if (!help && !parser.isSet("version")) {
    if (!QStringList{"capture", "record", "daemon", "cancel", "quit", "doctor"}.contains(root))
      return fail("Unknown command. See 'snipline --help'.");
    if (args.size() > 2 || ((root != "capture" && root != "record") && args.size() > 1))
      return fail("Unexpected arguments. See 'snipline help " + root + "'.");
    if (selecting && !action.isEmpty() && !QStringList{"region", "monitor", "window"}.contains(action))
      return fail("Unknown " + root + " subcommand. See 'snipline help " + root + "'.");
  }
  // Parse first so command names used as option values do not affect whether
  // the process needs a graphical application.
  const bool headless = help || parser.isSet("version") || control || command == "doctor";
  std::unique_ptr<QCoreApplication> app;
  if (headless)
    app = std::make_unique<QCoreApplication>(argc, argv);
  else {
    QQuickStyle::setStyle("Basic");
    app = std::make_unique<QGuiApplication>(argc, argv);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QGuiApplication::setDesktopFileName("snipline");
  }
  if (help) return showCommandHelp(args) ? 0 : fail("Unknown help command. See 'snipline --help'.");
  if (parser.isSet("version")) {
    QTextStream(stdout) << "snipline " << QCoreApplication::applicationVersion() << '\n';
    return 0;
  }
  if (command == "doctor")
    return snip::doctor();
  snip::Config config;
  QString configError;
  if (!control &&
      !config.loadFile(parser.isSet("config") ? parser.value("config")
                                              : snip::Config::defaultPath(),
                       !parser.isSet("config"), &configError))
    return fail(configError);
  const QString target = args.value(1, config.target);
  if (selecting && target != "region" && target != "monitor" &&
      target != "window")
    return fail("Capture targets are region, monitor and window.");
  if (parser.isSet("live") && parser.isSet("frozen"))
    return fail("--live and --frozen are mutually exclusive.");
  if (command != "capture" && (parser.isSet("live") || parser.isSet("frozen")))
    return fail("--live/--frozen are only valid with capture.");
  const bool live =
      parser.isSet("live") || (!parser.isSet("frozen") && config.live);
  const auto format = parser.isSet("format") ? parser.value("format").toLatin1()
                                             : config.format;
  if (format != "png" && format != "jpeg" && format != "webp")
    return fail("Format must be png, jpeg, or webp.");
  if ((!selecting || command != "record") && (parser.isSet("video-format") || parser.isSet("system-audio") ||
      parser.isSet("microphone") || parser.isSet("no-system-audio") || parser.isSet("no-microphone")))
    return fail("Recording options are only valid with record.");
  if ((parser.isSet("system-audio") && parser.isSet("no-system-audio")) ||
      (parser.isSet("microphone") && parser.isSet("no-microphone")))
    return fail("Conflicting audio options.");
  const auto videoFormat = parser.isSet("video-format") ? parser.value("video-format") : QString{};
  const int systemAudio = parser.isSet("system-audio") ? 1 : parser.isSet("no-system-audio") ? 0 : -1;
  const int microphone = parser.isSet("microphone") ? 1 : parser.isSet("no-microphone") ? 0 : -1;
  auto bus = QDBusConnection::sessionBus();
  if (!bus.isConnected())
    return fail("A session D-Bus is required for single-instance operation.");
  if (bus.interface()->isServiceRegistered(service).value()) {
    if (parser.isSet("directory") || parser.isSet("format") ||
        parser.isSet("theme") || parser.isSet("config"))
      return fail("An instance is running. Quit it before changing directory, "
                  "format, theme, or config.");
    if (command == "daemon")
      return 0;
    QDBusInterface remote(service, objectPath, service, bus);
    if (command == "quit" || command == "cancel") {
      const auto reply = remote.call(command == "quit" ? "Quit" : "Cancel");
      return reply.type() == QDBusMessage::ErrorMessage
                 ? fail(reply.errorMessage())
                 : 0;
    }
    if (command == "discard") {
      const auto reply = remote.call("Cancel");
      return reply.type() == QDBusMessage::ErrorMessage ? fail(reply.errorMessage()) : 0;
    }
    if (command == "toggle" || command == "stop" || command == "status") {
      const QString method = command == "toggle" ? "TogglePause" : command == "stop" ? "Stop" : "Status";
      const QDBusReply<QString> reply = remote.call(method);
      if (!reply.isValid()) return fail(reply.error().message());
      if (command == "status") { QTextStream(stdout) << reply.value() << '\n'; return 0; }
      return reply.value().isEmpty() ? 0 : fail(reply.value());
    }
    if (command == "record") {
      const QDBusReply<QString> reply = remote.call("RecordConfigured", args.size() > 1 ? target : QString{},
                                                   videoFormat, systemAudio, microphone);
      if (!reply.isValid()) return fail(reply.error().message());
      return reply.value().isEmpty() ? 0 : fail(reply.value());
    }
    const QDBusReply<QString> reply =
        remote.call("CaptureWithMode", target, live);
    if (!reply.isValid())
      return fail(reply.error().message());
    return reply.value().isEmpty() ? 0 : fail(reply.value());
  }
  if (control) {
    if (command == "status") { QTextStream(stdout) << "{\"recording\":false,\"state\":\"Idle\"}\n"; return 0; }
    if (command == "toggle" || command == "stop") return fail("No running Snipline instance.");
    return 0;
  }
  if (!qobject_cast<QGuiApplication *>(app.get()))
    return fail("Capture requires a graphical session.");

  QString directory = parser.value("directory");
  if (directory.isEmpty())
    directory = config.directory;
  snip::Controller controller(QDir(directory).absolutePath(), format,
                              parser.isSet("theme") ? parser.value("theme")
                                                    : config.theme);
  controller.configure(config);
  TerminationSignals terminationSignals(controller);
  if (!bus.registerObject(objectPath, &controller,
                          QDBusConnection::ExportScriptableSlots) ||
      !bus.registerService(service))
    return fail("Could not register the single-instance service. Try again.");
  if (selecting) {
    QTimer::singleShot(0, &controller, [&controller, command, target, live, videoFormat, systemAudio, microphone] {
      const auto error = command == "record" ? controller.RecordConfigured(target, videoFormat, systemAudio, microphone)
                                             : controller.CaptureWithMode(target, live);
      if (!error.isEmpty())
        QTextStream(stderr) << error << '\n';
    });
  }
  return app->exec();
}
