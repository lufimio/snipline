#pragma once

#include <QImage>
#include <QString>

namespace snip {

struct ExportResult {
  QString path;
  QString error;
};

// Unique names and atomic replacement of our own reserved path only.
ExportResult saveImage(const QImage &image, const QString &directory,
                       const QByteArray &format = "png");

} // namespace snip
