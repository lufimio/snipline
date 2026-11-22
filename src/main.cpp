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

void configure(QCommandLineParser &parser, const QString &scope = {}, bool targetSpecified = false) {
  parser.setApplicationDescription(
      "Snipline — Wayland screenshots and recording");
  parser.addHelpOption();
  parser.addVersionOption();
  if (scope == "control" || scope == "doctor") return;
  if (scope.isEmpty() || scope == "upload" || ((scope == "qr" || scope == "ocr" || scope == "lens") && !targetSpecified))
    parser.addOption({"file", "Use an existing file (upload) or image (qr/ocr/lens).", "path"});
  if (scope.isEmpty() || scope == "upload")
    parser.addOption({"service", "Upload destination: catbox, litterbox-1h/12h/24h/72h, pomf or sxcu:Name.", "destination"});
  if (scope.isEmpty() || scope == "capture" || scope == "qr" || scope == "ocr" || scope == "lens") {
    parser.addOption({"live", "Select over the live desktop; acquire pixels only "
                             "after confirmation."});
    parser.addOption(
         {"frozen", "Select over a frozen frame."});
  }
  parser.addOption({"config",
                    "Load a JSON configuration file.",
                    "path"});
  parser.addOption(
      {"directory", "Set the screenshot and recording output directories.", "path"});
  if (scope.isEmpty() || scope == "capture" || scope == "daemon")
    parser.addOption(
        {"format", "Screenshot format: png, jpeg or webp.", "format"});
  parser.addOption(
      {"theme", "Load a JSON theme file.", "path"});
  if (scope.isEmpty() || scope == "record") {
    parser.addOption({"video-format", "Recording format: mp4, webm or gif.", "format"});
    parser.addOption({"codec", "Video codec: auto, h264, h265, vp8, vp9 or av1.", "codec"});
    parser.addOption({"encoder", "Encoder: software, auto, vaapi, nvenc or qsv.", "backend"});
    parser.addOption({"system-audio", "Enable system audio."});
    parser.addOption({"microphone", "Enable microphone."});
    parser.addOption({"no-system-audio", "Disable system audio."});
    parser.addOption({"no-microphone", "Disable microphone."});
  }
}

bool showCommandHelp(QStringList path) {
  if (path.value(0) == "help") path.removeFirst();
  const auto root = path.value(0);
  const auto child = path.value(1);
  const QStringList targets{"region", "monitor", "window"};
  const QStringList controls{"pause", "resume", "stop", "discard", "status"};
  const QStringList roots{"capture", "record", "qr", "ocr", "lens", "upload", "daemon", "cancel", "quit", "doctor"};
  if (path.size() > 2 || (!root.isEmpty() && !roots.contains(root)) ||
        (!child.isEmpty() && (root != "capture" && root != "record" && root != "qr" && root != "ocr" && root != "lens")) ||
      (!child.isEmpty() && !targets.contains(child) && !(root == "record" && controls.contains(child))))
    return false;
  QTextStream out(stdout);
  const bool recognition = root == "qr" || root == "ocr";
  const bool lens = root == "lens";
  const bool recordingControl = root == "record" && controls.contains(child);
  const bool selecting = root == "capture" || recognition || lens || (root == "record" && !recordingControl);
  if (root.isEmpty()) {
    out << "Usage: snipline <command> [options]\n"
           "       snipline [capture-options]  (defaults to capture)\n"
           "       snipline help <command> [target-or-control]\n\n"
           "Commands:\n"
           "  capture [target]        Select and confirm a screenshot\n"
           "  record [target]         Select and confirm a recording\n"
           "  record <control>        Control an active recording\n"
           "  qr [target]             Scan QR/barcodes and copy the first link/payload\n"
           "  ocr [target]            Recognize text and copy it\n"
           "  qr|ocr --file <path>    Recognize a saved capture or existing image\n"
           "  upload --file <path>    Upload an existing file and copy its URL\n"
           "  lens [target]           Upload an image and search with Google Lens\n"
           "  lens --file <path>      Search with an existing local image\n"
           "  daemon                  Start the resident instance without a selector\n"
           "  cancel                  Cancel selection/recognition/upload or discard recording\n"
           "  quit                    Finalize recording and quit the resident instance\n"
           "  doctor                  Print JSON capability diagnostics; no capture\n"
           "  help [command] [child]  Show help without a graphical session\n\n"
           "Targets: region, monitor, window. All selections require explicit confirmation.\n"
           "Recording controls: pause, resume, stop, discard, status.\n"
           "  pause/resume: Toggle recording between running and paused (same toggle).\n\n"
           "Options below are an overview, not valid with every command. Use\n"
           "'snipline <command> --help' for applicable options.\n\n";
  } else if (selecting) {
    out << "Usage: snipline " << root << (child.isEmpty() ? " [target]" : " " + child) << " [options]\n";
    if ((recognition || lens) && child.isEmpty()) out << "       snipline " << root << " --file <path> [options]\n";
    out << "\nPositional arguments:\n";
    if (child.isEmpty()) {
      out << "  target                  region | monitor | window (optional)\n"
             "    region                Drag/select a rectangle\n"
             "    monitor               Select one output\n"
             "    window                Select an isolated application window\n\n";
    } else {
      out << "  None after '" << child << "'. This command "
          << (child == "region" ? "selects a rectangle." : child == "monitor" ? "selects one output." : "selects an isolated application window.")
          << "\n\n";
    }
    if (root == "record" && child.isEmpty())
      out << "Subcommands:\n"
             "  Use instead of a target; '<control> --help' shows details.\n"
             "  pause, resume           Toggle running/paused state (both the same toggle)\n"
             "  stop                    Finalize and save\n"
             "  discard                 Discard without saving\n"
             "  status                  Print recording state/elapsed time as JSON\n\n";
    out << "Behavior:\n  ";
    if (root == "record") {
      out << "Recording selection is always live.\n"
             "  Record must fit one output. Reinvoking record shows active controls.\n";
      out << "  Audio on/off switches conflict for the same source. Codec/container combinations:\n"
             "    MP4: H.264/H.265/AV1; WebM: VP8/VP9/AV1; GIF: auto/VP8, no audio.\n"
             "  GIF uses software; hardware is optional and preflighted before recording.\n";
    } else {
      out << "--live and --frozen select timing for this request and conflict.\n";
      if (recognition)
        out << "  No cursor is captured. Results copy automatically; no results window.\n"
               "  Links open only through an explicit notification action.\n"
               "  --file cannot be combined with a target, --live or --frozen.\n";
      if (lens)
        out << "  Sends the image to the selected host, then its public URL to Google.\n"
               "  Opens Lens without an extra confirmation dialog. No automatic searches.\n"
               "  --file cannot be combined with a target, --live or --frozen.\n"
               "  Existing images must be PNG, JPEG, WebP or GIF, at most 20 MiB.\n";
    }
    out << "  Explicitly confirm to save, recognize or start recording; Escape cancels.\n\n";
  } else {
    out << "Usage: snipline " << root << (child.isEmpty() ? "" : " " + child)
        << (root == "upload" ? " --file <path>" : "") << " [options]\n\n"
           "Positional arguments: none after the command shown above.\n\n";
    if (child == "pause" || child == "resume")
      out << "Toggle running → paused or paused → running. Both use the same toggle.\n"
             "No active recording is an error.\n\n";
    else if (child == "stop") out << "Finalize/save the active recording. No active recording is an error.\n\n";
    else if (child == "discard" || root == "cancel")
      out << "Cancel selection/recognition/upload or discard recording without saving.\n"
             "No running instance is a successful no-op.\n\n";
    else if (child == "status")
      out << "Print JSON recording state, elapsed time and output/error details.\n"
             "Without a running instance, reports Idle; does not start one.\n\n";
    else if (root == "quit") out << "Finalize recording and quit. No instance is a successful no-op.\n\n";
    else if (root == "daemon") out << "Start the resident instance; no selector opens. Already running: no-op.\n\n";
    else if (root == "doctor") out << "Print JSON capabilities without requesting pixels or recording streams.\n\n";
    else if (root == "upload")
      out << "Upload a local file and copy its HTTP(S) URL. The local file is retained.\n"
             "Runs immediately without an extra confirmation dialog; never uploads on import.\n"
             "Use --service to select a destination for this request.\n"
             "Cancel from the progress notification, recording UI or 'snipline cancel'.\n\n";
  }
  QCommandLineParser options;
  configure(options, recordingControl || root == "cancel" || root == "quit" ? "control" : root,
            !child.isEmpty());
  const auto nativeHelp = options.helpText();
  out << nativeHelp.mid(nativeHelp.indexOf("Options:\n"));
  out << "\nHelp: snipline help <command> [target-or-control], or append --help.\n";
  return true;
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication::setApplicationName("snipline");
  QCoreApplication::setApplicationVersion(QStringLiteral(SNIPLINE_VERSION));
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
  const bool recognition = command == "qr" || command == "ocr";
  const bool lens = command == "lens";
  const bool uploading = command == "upload";
  const bool selecting = command == "capture" || (command == "record" && !recordControl);
  const bool control = command == "quit" || command == "cancel" || recordControl;
  if (!help && !parser.isSet("version")) {
    if (!QStringList{"capture", "record", "qr", "ocr", "lens", "upload", "daemon", "cancel", "quit", "doctor"}.contains(root))
      return fail("Unknown command. See 'snipline --help'.");
    if (args.size() > 2 || ((root != "capture" && root != "record" && !recognition && !lens) && args.size() > 1))
      return fail("Unexpected arguments. See 'snipline help " + root + "'.");
    if (selecting && !action.isEmpty() && !QStringList{"region", "monitor", "window"}.contains(action))
      return fail("Unknown " + root + " subcommand. See 'snipline help " + root + "'.");
    if ((recognition || lens) && !action.isEmpty() && !QStringList{"region", "monitor", "window"}.contains(action))
      return fail("Image targets are region, monitor and window.");
    if (parser.isSet("file") && ((!recognition && !lens && !uploading) || !action.isEmpty()))
      return fail("--file is only valid with qr/ocr/lens/upload without a target.");
    if (uploading && !parser.isSet("file")) return fail("Upload requires --file <path>.");
    QCommandLineParser scoped;
    configure(scoped, recordControl || command == "cancel" || command == "quit" ? "control" : command,
              !action.isEmpty());
    if (!scoped.parse(arguments)) return fail(scoped.errorText());
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
  const QString recognitionTarget = action.isEmpty() ? "region" : action;
  const QString recognitionFile = parser.isSet("file") ? QFileInfo(parser.value("file")).absoluteFilePath() : QString{};
  if (selecting && target != "region" && target != "monitor" &&
      target != "window")
    return fail("Capture targets are region, monitor and window.");
  if (parser.isSet("live") && parser.isSet("frozen"))
    return fail("--live and --frozen are mutually exclusive.");
  if (command != "capture" && !recognition && !lens && (parser.isSet("live") || parser.isSet("frozen")))
    return fail("--live/--frozen are only valid with capture, qr, ocr or lens.");
  if (parser.isSet("file") && (parser.isSet("live") || parser.isSet("frozen")))
    return fail("--live/--frozen cannot be used with --file.");
  const int recognitionLive = parser.isSet("live") ? 1 : parser.isSet("frozen") ? 0 : -1;
  const bool live =
      parser.isSet("live") || (!parser.isSet("frozen") && config.live);
  const auto format = parser.isSet("format") ? parser.value("format").toLatin1()
                                             : config.format;
  if (format != "png" && format != "jpeg" && format != "webp")
    return fail("Format must be png, jpeg, or webp.");
  if ((!selecting || command != "record") && (parser.isSet("video-format") || parser.isSet("system-audio") ||
       parser.isSet("microphone") || parser.isSet("no-system-audio") || parser.isSet("no-microphone") ||
       parser.isSet("codec") || parser.isSet("encoder")))
    return fail("Recording options are only valid with record.");
  if ((parser.isSet("system-audio") && parser.isSet("no-system-audio")) ||
      (parser.isSet("microphone") && parser.isSet("no-microphone")))
    return fail("Conflicting audio options.");
  const auto videoFormat = parser.isSet("video-format") ? parser.value("video-format") : QString{};
  const auto codec = parser.value("codec");
  const auto encoder = parser.value("encoder");
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
    if (uploading || lens) {
      const QDBusReply<QString> reply = uploading
          ? remote.call("Upload", recognitionFile, parser.value("service"))
          : remote.call("Lens", recognitionTarget, recognitionFile, recognitionLive);
      if (!reply.isValid()) return fail(reply.error().message());
      return reply.value().isEmpty() ? 0 : fail(reply.value());
    }
    if (recognition) {
      const QDBusReply<QString> reply = remote.call("RecognizeWithMode", command, recognitionTarget, recognitionFile, recognitionLive);
      if (!reply.isValid()) return fail(reply.error().message());
      return reply.value().isEmpty() ? 0 : fail(reply.value());
    }
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
      const QDBusReply<QString> reply = remote.call("RecordEncoding", args.size() > 1 ? target : QString{},
                                                   videoFormat, systemAudio, microphone, codec, encoder);
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
  if (parser.isSet("directory"))
    config.recordingDirectory = QDir(directory).absolutePath();
  snip::Controller controller(QDir(directory).absolutePath(), format,
                              parser.isSet("theme") ? parser.value("theme")
                                                    : config.theme);
  controller.configure(config);
  TerminationSignals terminationSignals(controller);
  if (!bus.registerObject(objectPath, &controller,
                          QDBusConnection::ExportScriptableSlots) ||
      !bus.registerService(service))
    return fail("Could not register the single-instance service. Try again.");
  if (uploading || lens) {
    const auto service = parser.value("service");
    QTimer::singleShot(0, &controller, [&controller, uploading, recognitionTarget, recognitionFile, recognitionLive, service] {
      const auto error = uploading ? controller.Upload(recognitionFile, service)
                                  : controller.Lens(recognitionTarget, recognitionFile, recognitionLive);
      if (!error.isEmpty()) QTextStream(stderr) << error << '\n';
    });
  } else if (recognition) {
    QTimer::singleShot(0, &controller, [&controller, command, recognitionTarget, recognitionFile, recognitionLive] {
      const auto error = controller.RecognizeWithMode(command, recognitionTarget, recognitionFile, recognitionLive);
      if (!error.isEmpty()) QTextStream(stderr) << error << '\n';
    });
  } else if (selecting) {
    QTimer::singleShot(0, &controller, [&controller, command, target, live, videoFormat, systemAudio, microphone, codec, encoder] {
      const auto error = command == "record" ? controller.RecordEncoding(target, videoFormat, systemAudio, microphone, codec, encoder)
                                             : controller.CaptureWithMode(target, live);
      if (!error.isEmpty())
        QTextStream(stderr) << error << '\n';
    });
  }
  return app->exec();
}
