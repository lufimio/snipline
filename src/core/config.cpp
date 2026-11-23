#include "config.h"
#include "editorcommand.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <cmath>

namespace snip {
namespace {
QString resolve(QString value, const QString &base) {
  if (value == "~") {
    value = QDir::homePath();
  } else if (value.startsWith("~/")) {
    value = QDir::home().filePath(value.mid(2));
  }
  return QDir::cleanPath(QDir(base).absoluteFilePath(value));
}
} // namespace

Config::Config() {
  directory =
      QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).filePath("Snipline");
  recordingDirectory =
      QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath("Snipline");
}

QString Config::defaultPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
      .filePath("snipline/config.json");
}

bool Config::loadFile(const QString &path, bool optional, QString *error) {
  if (optional && !QFile::exists(path)) {
    return true;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (error) {
      *error = QString("Cannot read config %1: %2").arg(path, file.errorString());
    }
    return false;
  }
  return load(file.read(65537), QFileInfo(file).absolutePath(), error);
}

bool Config::load(const QByteArray &json, const QString &base, QString *error) {
  auto reject = [error](const QString &message) {
    if (error) {
      *error = "Invalid config: " + message;
    }
    return false;
  };
  if (json.size() > 65536) {
    return reject("maximum size is 64 KiB.");
  }
  QJsonParseError parse;
  const auto doc = QJsonDocument::fromJson(json, &parse);
  if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
    return reject("expected a JSON object. " + parse.errorString());
  }
  Config next = *this;
  auto object = doc.object();
  // Normalize the public screenshots block into the existing internal fields.
  // Legacy layouts remain readable, but duplicate definitions are ambiguous.
  if (object.contains("screenshots")) {
    if (!object["screenshots"].isObject()) {
      return reject("screenshots must be an object.");
    }
    const auto screenshots = object["screenshots"].toObject();
    const QSet<QString> keys{"directory", "target", "live", "format", "editor"};
    for (auto it = screenshots.begin(); it != screenshots.end(); ++it) {
      if (!keys.contains(it.key())) {
        return reject("unknown screenshots key " + it.key());
      }
      if (it.key() == "editor") {
        QString editorError;
        if (!it.value().isString()) {
          return reject("screenshots.editor must be a command string.");
        }
        if (!validateEditorCommand(it.value().toString(), &editorError)) {
          return reject(editorError);
        }
        next.screenshotEditor = it.value().toString().trimmed();
      } else if (it.key() == "directory") {
        if (!it.value().isString() || it.value().toString().trimmed().isEmpty()) {
          return reject("screenshots.directory must be a nonempty path string.");
        }
        const auto legacy = object.value("directory");
        if (legacy.isString() || legacy.toObject().contains("screenshots")) {
          return reject("use screenshots.directory without a legacy screenshot directory.");
        }
        if (!legacy.isUndefined() && !legacy.isObject()) {
          return reject("legacy directory must be a path string or directory object.");
        }
        auto paths = legacy.toObject();
        paths.insert("screenshots", it.value());
        object.insert("directory", paths);
      } else {
        if (object.contains(it.key())) {
          return reject("use screenshots." + it.key() + " without the legacy top-level " +
                        it.key() + ".");
        }
        object.insert(it.key(), it.value());
      }
    }
  }
  const QSet<QString> known{"directory",
                            "format",
                            "theme",
                            "target",
                            "live",
                            "clipboard",
                            "notifications",
                            "notificationTimeout",
                            "keybindings",
                            "recording",
                            "filenameTemplate",
                            "ocr",
                            "screenshots",
                            "uploaders",
                            "lens"};
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (!known.contains(it.key())) {
      return reject("unknown key " + it.key());
    }
  }
  if (object.contains("directory")) {
    const auto directories = object["directory"];
    if (directories.isString()) {
      // Keep existing screenshot-only configs readable.
      if (directories.toString().trimmed().isEmpty()) {
        return reject("directory must not be empty.");
      }
      next.directory = resolve(directories.toString(), base);
    } else if (directories.isObject()) {
      const auto paths = directories.toObject();
      for (auto it = paths.begin(); it != paths.end(); ++it) {
        if (it.key() != "screenshots" && it.key() != "recordings") {
          return reject("unknown directory key " + it.key());
        }
        if (!it.value().isString() || it.value().toString().trimmed().isEmpty()) {
          return reject("directory." + it.key() + " must be a nonempty path string.");
        }
        const auto path = resolve(it.value().toString(), base);
        if (it.key() == "screenshots") {
          next.directory = path;
        } else {
          next.recordingDirectory = path;
        }
      }
    } else {
      return reject("directory must be an object with screenshots and/or recordings paths.");
    }
  }
  for (const auto &key : {"format", "theme", "target", "filenameTemplate"}) {
    if (!object.contains(key)) {
      continue;
    }
    if (!object[key].isString() || object[key].toString().isEmpty()) {
      return reject(QString(key) + " must be a nonempty string.");
    }
    const auto value = object[key].toString();
    if (QString(key) == "theme") {
      next.theme = resolve(value, base);
    }
    if (QString(key) == "format") {
      next.format = value.toLatin1();
    }
    if (QString(key) == "target") {
      next.target = value;
    }
    if (QString(key) == "filenameTemplate") {
      next.filenameTemplate = value;
    }
  }
  QString templateError;
  if (!validateFilenameTemplate(next.filenameTemplate, &templateError)) {
    return reject(templateError);
  }
  if (next.format != "png" && next.format != "jpeg" && next.format != "webp") {
    return reject("format must be png, jpeg or webp.");
  }
  if (next.target != "region" && next.target != "monitor" && next.target != "window") {
    return reject("target must be region, monitor or window.");
  }
  for (const auto &key : {"live", "clipboard", "notifications"}) {
    if (!object.contains(key)) {
      continue;
    }
    if (!object[key].isBool()) {
      return reject(QString(key) + " must be boolean.");
    }
    if (QString(key) == "live") {
      next.live = object[key].toBool();
    }
    if (QString(key) == "clipboard") {
      next.clipboard = object[key].toBool();
    }
    if (QString(key) == "notifications") {
      next.notifications = object[key].toBool();
    }
  }
  auto integer = [](QJsonValue value, int low, int high, int &destination) {
    const double n = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(n) || std::floor(n) != n || n < low || n > high) {
      return false;
    }
    destination = int(n);
    return true;
  };
  for (const auto &section : {QString("uploaders"), QString("lens")}) {
    if (!object.contains(section)) {
      continue;
    }
    if (!object[section].isObject()) {
      return reject(section + " must be an object.");
    }
    const auto settings = object[section].toObject();
    for (auto it = settings.begin(); it != settings.end(); ++it) {
      if (it.key() == (section == "uploaders" ? "default" : "destination")) {
        if (!it.value().isString() || it.value().toString().trimmed().isEmpty() ||
            it.value().toString().size() > 160) {
          return reject(section + ".destination must be a nonempty destination name.");
        }
        for (const auto ch : it.value().toString()) {
          if (!ch.isPrint()) {
            return reject("Destination names cannot contain control characters.");
          }
        }
        if (section == "uploaders") {
          next.uploadDestination = it.value().toString();
        } else {
          next.lensDestination = it.value().toString();
        }
      } else if (section == "uploaders" && it.key() == "timeout") {
        if (!integer(it.value(), 5, 3600, next.uploadTimeout)) {
          return reject("upload.timeout must be 5–3600 seconds.");
        }
      } else if (section == "uploaders" && it.key() == "custom") {
        if (!it.value().isArray() || it.value().toArray().size() > 32) {
          return reject("upload.uploaders must be an array of at most 32 local .sxcu paths.");
        }
        next.uploaders.clear();
        for (const auto &path : it.value().toArray()) {
          if (!path.isString() || path.toString().trimmed().isEmpty()) {
            return reject("Uploader paths must be nonempty strings.");
          }
          next.uploaders.append(resolve(path.toString(), base));
        }
      } else {
        return reject("unknown " + section + " key " + it.key());
      }
    }
  }
  if (object.contains("ocr")) {
    if (!object["ocr"].isObject()) {
      return reject("ocr must be an object.");
    }
    const auto ocr = object["ocr"].toObject();
    for (auto it = ocr.begin(); it != ocr.end(); ++it) {
      if (it.key() != "languages") {
        return reject("unknown ocr key " + it.key());
      }
    }
    if (ocr.contains("languages")) {
      if (!ocr["languages"].isArray() || ocr["languages"].toArray().isEmpty() ||
          ocr["languages"].toArray().size() > 32) {
        return reject("ocr.languages must contain 1–32 language names.");
      }
      QStringList languages;
      const QRegularExpression valid("^[A-Za-z0-9_]+(?:/[A-Za-z0-9_]+)*$");
      for (const auto &value : ocr["languages"].toArray()) {
        const auto language = value.toString();
        if (!value.isString() || language.size() > 128 || language == "osd" ||
            !valid.match(language).hasMatch()) {
          return reject("invalid OCR language name.");
        }
        if (!languages.contains(language)) {
          languages.append(language);
        }
      }
      next.ocrLanguages = languages;
    }
  }
  if (object.contains("notificationTimeout") &&
      !integer(object["notificationTimeout"], 0, 60000, next.notificationTimeout)) {
    return reject("notificationTimeout must be an integer from 0 to 60000 ms.");
  }
  if (object.contains("keybindings")) {
    if (!object["keybindings"].isObject()) {
      return reject("keybindings must be an object.");
    }
    const auto keys = object["keybindings"].toObject();
    for (auto it = keys.begin(); it != keys.end(); ++it) {
      if (!next.keybindings.contains(it.key()) || !it.value().isArray()) {
        return reject(
            "keybindings require known actions with arrays of portable Qt key sequences.");
      }
      QStringList sequences;
      for (const auto &entry : it.value().toArray()) {
        if (!entry.isString()) {
          return reject("key sequences must be strings.");
        }
        const auto sequence =
            QKeySequence::fromString(entry.toString(), QKeySequence::PortableText);
        if (sequence.isEmpty() || sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown ||
            sequence[0].key() == 0) {
          return reject("invalid or multi-step key sequence: " + entry.toString());
        }
        sequences.append(sequence.toString(QKeySequence::PortableText));
      }
      next.keybindings[it.key()] = sequences;
    }
    QSet<QString> used;
    for (auto it = next.keybindings.begin(); it != next.keybindings.end(); ++it) {
      for (const auto &sequence : it.value().toStringList()) {
        if (used.contains(sequence)) {
          return reject("duplicate key sequence: " + sequence);
        }
        used.insert(sequence);
      }
    }
  }
  if (object.contains("recording")) {
    if (!object["recording"].isObject()) {
      return reject("recording must be an object.");
    }
    const auto recording = object["recording"].toObject();
    for (auto it = recording.begin(); it != recording.end(); ++it) {
      if (it.key() == "codec" || it.key() == "encoder") {
        if (!it.value().isString()) {
          return reject(it.key() + " must be a string.");
        }
        const auto value = it.value().toString();
        if (it.key() == "codec") {
          if (!QStringList{"auto", "h264", "h265", "vp8", "vp9", "av1"}.contains(value)) {
            return reject("recording codec must be auto, h264, h265, vp8, vp9 or av1.");
          }
          next.recordingCodec = value;
        } else {
          if (!QStringList{"software", "auto", "vaapi", "nvenc", "qsv"}.contains(value)) {
            return reject("recording encoder must be software, auto, vaapi, nvenc or qsv.");
          }
          next.recordingEncoder = value;
        }
      } else if (it.key() == "directory") {
        if (object["directory"].toObject().contains("recordings")) {
          return reject(
              "use recording.directory without also specifying legacy directory.recordings.");
        }
        if (!it.value().isString() || it.value().toString().trimmed().isEmpty()) {
          return reject("recording directory must be a nonempty path string.");
        }
        next.recordingDirectory = resolve(it.value().toString(), base);
      } else if (it.key() == "editor") {
        QString editorError;
        if (!it.value().isString()) {
          return reject("recording.editor must be a command string.");
        }
        if (!validateEditorCommand(it.value().toString(), &editorError)) {
          return reject(editorError);
        }
        next.videoEditor = it.value().toString().trimmed();
      } else if (it.key() == "format") {
        next.recordingFormat = it.value().toString();
        if (next.recordingFormat != "mp4" && next.recordingFormat != "webm" &&
            next.recordingFormat != "gif") {
          return reject("recording format must be mp4, webm or gif.");
        }
      } else if (it.key() == "fps") {
        if (!integer(it.value(), 1, 120, next.recordingFps)) {
          return reject("recording fps must be 1–120.");
        }
      } else if (it.key() == "countdown") {
        if (!integer(it.value(), 0, 30, next.countdown)) {
          return reject("countdown must be 0–30 seconds.");
        }
      } else if (it.key() == "maxDuration") {
        if (!integer(it.value(), 1, 86400, next.maxDuration)) {
          return reject("maxDuration must be 1–86400 seconds.");
        }
      } else {
        return reject("unknown recording key " + it.key());
      }
    }
  }
  if (next.systemAudioDevice.isEmpty()) {
    return reject("systemAudioDevice must name a monitor source (default: @DEFAULT_MONITOR@).");
  }
  const auto codecs = next.recordingFormat == "mp4"    ? QStringList{"auto", "h264", "h265", "av1"}
                      : next.recordingFormat == "webm" ? QStringList{"auto", "vp8", "vp9", "av1"}
                                                       : QStringList{"auto", "vp8"};
  if (!codecs.contains(next.recordingCodec)) {
    return reject("recording codec is incompatible with its format.");
  }
  if (next.recordingFormat == "gif" && next.recordingEncoder != "software" &&
      next.recordingEncoder != "auto") {
    return reject("GIF requires software or auto encoding.");
  }
  *this = next;
  if (error) {
    error->clear();
  }
  return true;
}
} // namespace snip
