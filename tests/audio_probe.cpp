#include "audiosources.h"
#include "recorder.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QTimer>

using namespace snip;

// Explicit manual diagnostic, never registered with CTest. Captures only one
// unambiguously named application; video is synthetic, not the user's screen.
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOptions({{{"a", "app"}, "Exact application name or ID.", "name"},
                     {{"o", "output"}, "Output directory.", "directory"},
                     {{"s", "seconds"}, "Duration (1–30 seconds).", "seconds", "5"},
                     {"codec", "MP4 video codec: auto, h264, h265 or av1.", "codec", "auto"},
                     {"encoder", "Encoder: software, auto, vaapi, nvenc or qsv.", "backend", "software"}});
  parser.process(app);
  bool valid = false;
  const auto seconds = parser.value("seconds").toInt(&valid);
  if (!valid || seconds < 1 || seconds > 30 || parser.value("app").isEmpty() ||
      parser.value("output").isEmpty()) parser.showHelp(1);
  AudioSources catalog;
  VideoRecorder recorder;
  bool selected = false;
  QObject::connect(&catalog, &AudioSources::changed, &app, [&] {
    if (selected || catalog.loading()) return;
    QVariantList matches;
    for (const auto &entry : catalog.sources()) {
      const auto row = entry.toMap();
      if (row.value("kind") == "application" &&
          (row.value("appName").toString() == parser.value("app") ||
           row.value("appId").toString() == parser.value("app"))) matches.append(row);
    }
    selected = true;
    if (matches.size() != 1) {
      qCritical() << "Expected exactly one matching application stream, found" << matches.size();
      app.exit(1);
      return;
    }
    RecordingOptions options;
    options.directory = parser.value("output");
    options.filenameTemplate = "AudioProbe_{datetime:yyyyMMdd_HHmmss}";
    options.format = "mp4";
    options.codec = parser.value("codec");
    options.encoder = parser.value("encoder");
    options.explicitAudioInputs = true;
    options.audioInputs = matches;
    QImage frame(320, 240, QImage::Format_RGB32);
    frame.fill(Qt::black);
    recorder.start(1, options, frame);
  });
  QObject::connect(&recorder, &VideoRecorder::started, &app, [&] {
    QTimer::singleShot(seconds * 1000, &app, [&] {
      qInfo() << recorder.audioDiagnostics();
      recorder.stop();
    });
  });
  QObject::connect(&recorder, &VideoRecorder::saved, &app, [&](quint64, const QString &path) {
    qInfo().noquote() << path;
    app.quit();
  });
  QObject::connect(&recorder, &VideoRecorder::sourceLost, &app, [&](quint64, const QString &warning) {
    qWarning().noquote() << "Source lost; finalizing:" << warning;
  });
  QObject::connect(&recorder, &VideoRecorder::failed, &app, [&](quint64, const QString &error) {
    qCritical().noquote() << error;
    app.exit(1);
  });
  QTimer::singleShot(45000, &app, [&] { qCritical() << "Probe timed out"; app.exit(1); });
  QTimer::singleShot(0, &catalog, &AudioSources::refresh);
  return app.exec();
}
