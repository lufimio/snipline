#include "exporter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImageWriter>
#include <QSaveFile>
#include <QUuid>

namespace snip {

ExportResult saveImage(const QImage &image, const QString &directory,
                       const QByteArray &format) {
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

  const QString stamp =
      QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
  QString path;
  QFile reservation;
  for (int attempt = 0; attempt < 10; ++attempt) {
    path = QDir(directory).filePath(
        QStringLiteral("Snipline_%1_%2.%3")
            .arg(stamp, QUuid::createUuid().toString(QUuid::Id128),
                 QString::fromLatin1(format)));
    reservation.setFileName(path);
    if (reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly))
      break;
    if (attempt == 9)
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
