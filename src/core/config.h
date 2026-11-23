#pragma once
#include "exporter.h"
#include <QVariantMap>

namespace snip {
struct Config {
  QString filenameTemplate = DefaultFilenameTemplate;
  QString directory, recordingDirectory, theme;
  QByteArray format = "png";
  QString target = "region";
  bool live = true, clipboard = true, notifications = true;
  int notificationTimeout = 5000;
  QVariantMap keybindings{{"confirm", QStringList{"Return", "Enter", "Space"}},
                          {"cancel", QStringList{"Escape"}},
                          {"live", QStringList{"Ctrl+L"}},
                          {"cursor", QStringList{"Ctrl+Shift+C"}},
                          {"save", QStringList{"Ctrl+S"}},
                          {"copy", QStringList{"Ctrl+C"}},
                          {"open", QStringList{"Ctrl+O"}},
                          {"folder", QStringList{"Ctrl+Shift+O"}},
                          {"edit", QStringList{"Ctrl+E"}},
                          {"upload", QStringList{"Ctrl+U"}},
                          {"capture", QStringList{"Ctrl+1"}},
                          {"record", QStringList{"Ctrl+2"}},
                          {"qr", QStringList{"Ctrl+3"}},
                          {"ocr", QStringList{"Ctrl+4"}},
                          {"lens", QStringList{"Ctrl+5"}},
                          {"region", QStringList{"Shift+1"}},
                          {"monitor", QStringList{"Shift+2"}},
                          {"window", QStringList{"Shift+3"}}};
  QString recordingFormat = "mp4";
  QString recordingCodec = "auto", recordingEncoder = "auto";
  QString screenshotEditor, videoEditor;
  QStringList ocrLanguages{"eng"};
  QString uploadDestination = "catbox", lensDestination = "litterbox-1h";
  QStringList uploaders;
  int uploadTimeout = 180;
  int recordingFps = 30, countdown = 3, maxDuration = 3600;
  // Session-only audio preferences; these are not JSON configuration keys.
  bool systemAudio = false, microphone = false;
  QString systemAudioDevice = "@DEFAULT_MONITOR@", microphoneDevice;
  Config();
  // Transactional: an invalid document leaves all settings unchanged.
  bool load(const QByteArray &json, const QString &baseDirectory, QString *error);
  bool loadFile(const QString &path, bool optional, QString *error);
  static QString defaultPath();
};
} // namespace snip
