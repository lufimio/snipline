#include "exporter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImageWriter>
#include <QSaveFile>
#include <QUuid>

namespace snip {
QString formatFilenameTemplate(const QString &filenameTemplate, const QDateTime &now,
                               QString *error) {
  auto reject = [error](const QString &message) {
    if (error) *error = message;
    return QString{};
  };
  if (filenameTemplate.size() > 4096)
    return reject("filenameTemplate must be at most 4096 characters.");
  if (!now.isValid()) return reject("Cannot format an invalid date/time.");
  QString expanded;
  for (qsizetype i = 0; i < filenameTemplate.size();) {
    const auto ch = filenameTemplate[i];
    // Doubled braces outside placeholders represent literal braces.
    if ((ch == '{' || ch == '}') && i + 1 < filenameTemplate.size() && filenameTemplate[i + 1] == ch) {
      expanded += ch;
      i += 2;
      continue;
    }
    if (ch == '}') return reject("Unmatched closing brace in filenameTemplate.");
    if (ch != '{') { expanded += ch; ++i; continue; }
    const auto start = ++i;
    const bool datetime = filenameTemplate.mid(start, 9) == "datetime:";
    // QDateTime quoted literals may themselves contain braces. Doubled single
    // quotes represent a literal quote and do not change the quoting state.
    bool quoted = false;
    while (i < filenameTemplate.size()) {
      const auto current = filenameTemplate[i];
      if (datetime && current == '\'') {
        if (i + 1 < filenameTemplate.size() && filenameTemplate[i + 1] == '\'') {
          i += 2;
          continue;
        }
        quoted = !quoted;
      }
      if (current == '}' && !quoted) break;
      ++i;
    }
    if (i == filenameTemplate.size())
      return reject("Unclosed placeholder or quoted literal in filenameTemplate.");
    const auto token = filenameTemplate.mid(start, i - start);
    ++i;
    if (datetime) {
      const auto pattern = token.mid(9);
      if (pattern.isEmpty()) return reject("{datetime:...} requires a QDateTime format.");
      expanded += now.toString(pattern);
    } else if (token == "timestamp") expanded += now.toString("yyyy-MM-dd_HH-mm-ss-zzz");
    else if (token == "date") expanded += now.toString("yyyy-MM-dd");
    else if (token == "time") expanded += now.toString("HH-mm-ss");
    else if (token == "uuid") expanded += QUuid::createUuid().toString(QUuid::Id128);
    else return reject("Unknown filename placeholder. Supported: {datetime:...}, {timestamp}, {date}, {time}, {uuid}.");
  }
  if (expanded.trimmed().isEmpty() || expanded == "." || expanded == "..")
    return reject("filenameTemplate must produce a nonempty filename stem.");
  for (const auto ch : expanded) {
    if (ch == '/' || ch == '\\' || !ch.isPrint())
      return reject("filenameTemplate cannot produce path separators or control characters.");
  }
  // Leave room for the extension and a collision suffix on common filesystems.
  if (expanded.toUtf8().size() > 220)
    return reject("Expanded filenameTemplate must be at most 220 UTF-8 bytes.");
  if (error) error->clear();
  return expanded;
}

bool validateFilenameTemplate(const QString &filenameTemplate, QString *error) {
  return !formatFilenameTemplate(filenameTemplate, QDateTime::currentDateTime(), error).isEmpty();
}

ExportResult saveImage(const QImage &image, const QString &directory,
                       const QByteArray &format, const QString &filenameTemplate) {
  QString templateError;
  const QString stem = formatFilenameTemplate(filenameTemplate, QDateTime::currentDateTime(), &templateError);
  if (stem.isEmpty())
    return {{}, templateError};
  if (image.isNull())
    return {{}, QStringLiteral("Cannot save an empty image.")};
  if (format != "png" && format != "jpeg" && format != "webp")
    return {{}, QStringLiteral("Supported formats: png, jpeg, webp.")};
  if (!QImageWriter::supportedImageFormats().contains(format))
    return {{},
            QStringLiteral("No Qt image encoder for %1.")
                .arg(QString::fromLatin1(format))};
  if (!QDir().mkpath(directory))
    return {{}, QStringLiteral("Cannot create capture directory.")};

  QString path;
  QFile reservation;
  for (int attempt = 0; attempt < 10000; ++attempt) {
    path = QDir(directory).filePath(
        stem + (attempt ? QStringLiteral("_%1").arg(attempt) : QString{}) +
        "." + QString::fromLatin1(format));
    reservation.setFileName(path);
    if (reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly))
      break;
    // Only a name collision is retryable; permission and I/O errors are not.
    if (!QFile::exists(path) || attempt == 9999)
      return {{},
              QStringLiteral("Cannot reserve a unique capture filename: %1")
                  .arg(reservation.errorString())};
  }
  reservation.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  reservation.close();
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) {
    QFile::remove(path);
    return {{}, file.errorString()};
  }
  // JPEG has no alpha; flatten desktop gaps onto black explicitly.
  QImage encoded = image;
  if (format == "jpeg")
    encoded = image.convertToFormat(QImage::Format_RGB32);
  QImageWriter writer(&file, format);
  if (!writer.write(encoded)) {
    file.cancelWriting();
    QFile::remove(path);
    return {{}, writer.errorString()};
  }
  if (!file.commit()) {
    QFile::remove(path);
    return {{}, file.errorString()};
  }
  return {path, {}};
}

} // namespace snip
