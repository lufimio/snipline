#pragma once
#include "exporter.h"
#include <QVariantMap>

namespace snip {
struct Config {
  QString filenameTemplate = DefaultFilenameTemplate;
  QString directory, theme;
  QByteArray format = "png";
  QString target = "region";
  bool live = false, clipboard = true, notifications = true;
  int notificationTimeout = 5000;
  QVariantMap keybindings{{"confirm", QStringList{"Return", "Enter"}},
                         {"cancel", QStringList{"Escape"}},
                         {"region", QStringList{"Ctrl+1"}},
                         {"monitor", QStringList{"Ctrl+2"}},
                         {"window", QStringList{"Ctrl+3"}}};
  QString recordingFormat = "mp4";
  int recordingFps = 30, countdown = 3, maxDuration = 3600;
  bool systemAudio = false, microphone = false;
  QString systemAudioDevice = "@DEFAULT_MONITOR@", microphoneDevice;
  Config();
  // Transactional: an invalid document leaves all settings unchanged.
  bool load(const QByteArray &json, const QString &baseDirectory, QString *error);
  bool loadFile(const QString &path, bool optional, QString *error);
  static QString defaultPath();
};
}
