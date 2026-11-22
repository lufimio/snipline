#pragma once
#include <QImage>

namespace snip {
inline constexpr qint64 RecognitionMaxPixels = 16'777'216;
inline constexpr qint64 RecognitionMaxBytes = 128 * 1024 * 1024;
inline constexpr qint64 RecognitionMaxFileBytes = 64 * 1024 * 1024;
inline constexpr qint64 RecognitionMaxResultBytes = 1024 * 1024;
inline bool recognitionImageSize(QSize size) {
  return size.width() > 0 && size.height() > 0 && size.width() <= 16384 && size.height() <= 16384 &&
         qint64(size.width()) * size.height() <= RecognitionMaxPixels;
}
inline int recognitionPixelSize(QImage::Format format) {
  switch (format) {
  case QImage::Format_RGB32: case QImage::Format_ARGB32: case QImage::Format_ARGB32_Premultiplied:
  case QImage::Format_RGBA8888: case QImage::Format_RGBA8888_Premultiplied: case QImage::Format_RGBX8888: return 4;
  case QImage::Format_RGB888: return 3;
  case QImage::Format_Grayscale8: return 1;
  default: return 0;
  }
}
}
