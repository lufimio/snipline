#include "config.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QKeySequence>
#include <QSet>
#include <QStandardPaths>
#include <cmath>

namespace snip {
namespace {
QString resolve(QString value, const QString &base) {
  if (value == "~") value = QDir::homePath();
  else if (value.startsWith("~/")) value = QDir::home().filePath(value.mid(2));
  return QDir::cleanPath(QDir(base).absoluteFilePath(value));
}
}
Config::Config() {
  directory = QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
                  .filePath("Snipline");
}
QString Config::defaultPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
      .filePath("snipline/config.json");
}
bool Config::loadFile(const QString &path, bool optional, QString *error) {
  if (optional && !QFile::exists(path)) return true;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (error) *error = QString("Cannot read config %1: %2").arg(path, file.errorString());
    return false;
  }
  return load(file.read(65537), QFileInfo(file).absolutePath(), error);
}
bool Config::load(const QByteArray &json, const QString &base, QString *error) {
  auto reject = [error](const QString &message) {
    if (error) *error = "Invalid config: " + message;
    return false;
  };
  if (json.size() > 65536) return reject("maximum size is 64 KiB.");
  QJsonParseError parse;
  const auto doc = QJsonDocument::fromJson(json, &parse);
  if (parse.error != QJsonParseError::NoError || !doc.isObject())
    return reject("expected a JSON object. " + parse.errorString());
  Config next = *this;
  const auto object = doc.object();
  const QSet<QString> known{"directory", "format", "theme", "target", "live",
      "clipboard", "notifications", "notificationTimeout", "keybindings", "recording", "filenameTemplate"};
  for (auto it = object.begin(); it != object.end(); ++it)
    if (!known.contains(it.key())) return reject("unknown key " + it.key());
  for (const auto &key : {"directory", "format", "theme", "target", "filenameTemplate"}) {
    if (!object.contains(key)) continue;
    if (!object[key].isString() || object[key].toString().isEmpty())
      return reject(QString(key) + " must be a nonempty string.");
    const auto value = object[key].toString();
    if (QString(key) == "directory") next.directory = resolve(value, base);
    if (QString(key) == "theme") next.theme = resolve(value, base);
    if (QString(key) == "format") next.format = value.toLatin1();
    if (QString(key) == "target") next.target = value;
    if (QString(key) == "filenameTemplate") next.filenameTemplate = value;
  }
  QString templateError;
  if (!validateFilenameTemplate(next.filenameTemplate, &templateError))
    return reject(templateError);
  if (next.format != "png" && next.format != "jpeg" && next.format != "webp")
    return reject("format must be png, jpeg or webp.");
  if (next.target != "region" && next.target != "monitor" && next.target != "window")
    return reject("target must be region, monitor or window.");
  for (const auto &key : {"live", "clipboard", "notifications"}) {
    if (!object.contains(key)) continue;
    if (!object[key].isBool()) return reject(QString(key) + " must be boolean.");
    if (QString(key) == "live") next.live = object[key].toBool();
    if (QString(key) == "clipboard") next.clipboard = object[key].toBool();
    if (QString(key) == "notifications") next.notifications = object[key].toBool();
  }
  auto integer = [](QJsonValue value, int low, int high, int &destination) {
    const double n = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(n) || std::floor(n) != n || n < low || n > high)
      return false;
    destination = int(n);
    return true;
  };
  if (object.contains("notificationTimeout") &&
      !integer(object["notificationTimeout"], 0, 60000, next.notificationTimeout))
    return reject("notificationTimeout must be an integer from 0 to 60000 ms.");
  if (object.contains("keybindings")) {
    if (!object["keybindings"].isObject()) return reject("keybindings must be an object.");
    const auto keys = object["keybindings"].toObject();
    for (auto it = keys.begin(); it != keys.end(); ++it) {
      if (!next.keybindings.contains(it.key()) || !it.value().isArray())
        return reject("keybindings require known actions with arrays of portable Qt key sequences.");
      QStringList sequences;
      for (const auto &entry : it.value().toArray()) {
        if (!entry.isString()) return reject("key sequences must be strings.");
        const auto sequence = QKeySequence::fromString(entry.toString(), QKeySequence::PortableText);
        if (sequence.isEmpty() || sequence.count() != 1 ||
            sequence[0].key() == Qt::Key_unknown || sequence[0].key() == 0)
          return reject("invalid or multi-step key sequence: " + entry.toString());
        sequences.append(sequence.toString(QKeySequence::PortableText));
      }
      next.keybindings[it.key()] = sequences;
    }
    QSet<QString> used;
    for (auto it = next.keybindings.begin(); it != next.keybindings.end(); ++it)
      for (const auto &sequence : it.value().toStringList()) {
        if (used.contains(sequence)) return reject("duplicate key sequence: " + sequence);
        used.insert(sequence);
      }
  }
  if (object.contains("recording")) {
    if (!object["recording"].isObject()) return reject("recording must be an object.");
    const auto recording = object["recording"].toObject();
    for (auto it = recording.begin(); it != recording.end(); ++it) {
      if (it.key() == "systemAudio" || it.key() == "microphone") {
        if (!it.value().isBool()) return reject(it.key() + " must be boolean.");
        if (it.key() == "systemAudio") next.systemAudio = it.value().toBool();
        else next.microphone = it.value().toBool();
      } else if (it.key() == "systemAudioDevice" || it.key() == "microphoneDevice") {
        if (!it.value().isString()) return reject(it.key() + " must be a source name string.");
        const auto device = it.value().toString();
        for (const auto ch : device)
          if (!ch.isPrint()) return reject("Audio source names cannot contain control characters.");
        if (it.key() == "systemAudioDevice") next.systemAudioDevice = device;
        else next.microphoneDevice = device;
      } else if (it.key() == "format") {
        next.recordingFormat = it.value().toString();
        if (next.recordingFormat != "mp4" && next.recordingFormat != "webm" && next.recordingFormat != "gif")
          return reject("recording format must be mp4, webm or gif.");
      } else if (it.key() == "fps") {
        if (!integer(it.value(), 1, 120, next.recordingFps)) return reject("recording fps must be 1–120.");
      } else if (it.key() == "countdown") {
        if (!integer(it.value(), 0, 30, next.countdown)) return reject("countdown must be 0–30 seconds.");
      } else if (it.key() == "maxDuration") {
        if (!integer(it.value(), 1, 86400, next.maxDuration)) return reject("maxDuration must be 1–86400 seconds.");
      } else return reject("unknown recording key " + it.key());
    }
  }
  if (next.systemAudioDevice.isEmpty())
    return reject("systemAudioDevice must name a monitor source (default: @DEFAULT_MONITOR@).");
  *this = next;
  if (error) error->clear();
  return true;
}
}
