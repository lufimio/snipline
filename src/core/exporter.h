#pragma once

#include <QImage>
#include <QDateTime>
#include <QString>

namespace snip {

struct ExportResult {
  QString path;
  QString error;
};

inline const QString DefaultFilenameTemplate = QStringLiteral("Snipline_{datetime:yyyy-MM-dd_HH-mm-ss-zzz}_{uuid}");
// Returns an empty string and sets error for invalid syntax or unsafe filenames.
QString formatFilenameTemplate(const QString &filenameTemplate, const QDateTime &now,
                               QString *error = nullptr);
bool validateFilenameTemplate(const QString &filenameTemplate, QString *error = nullptr);

// Unique names and atomic replacement of our own reserved path only.
ExportResult saveImage(const QImage &image, const QString &directory,
                       const QByteArray &format = "png",
                       const QString &filenameTemplate = DefaultFilenameTemplate);

} // namespace snip
