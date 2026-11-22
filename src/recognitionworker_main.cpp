#include "recognitionprotocol.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTextStream>
#include <ZXing/ReadBarcode.h>
#include <ZXing/ContentType.h>
#include <sys/resource.h>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto reject = [](const QString &message) { QTextStream(stderr) << message << '\n'; return 1; };
  const rlimit memory{1024ULL * 1024 * 1024, 1024ULL * 1024 * 1024};
  setrlimit(RLIMIT_AS, &memory);
  const rlimit core{0, 0};
  setrlimit(RLIMIT_CORE, &core);
  const auto args = app.arguments();
  if (args.size() < 3 || (args[1] != "qr" && args[1] != "image"))
    return reject("Expected qr/image and raw/file input.");
  QImage image;
  QByteArray pixels;
  if (args[2] == "raw" && args.size() == 3) {
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) return reject("Cannot read image input.");
    const auto fields = input.readLine(256).trimmed().split(' ');
    if (fields.size() != 5 || fields[0] != "SNIPLINE_IMAGE") return reject("Invalid image header.");
    bool ok[4]{};
    const auto width = fields[1].toInt(&ok[0]), height = fields[2].toInt(&ok[1]);
    const auto stride = fields[3].toInt(&ok[2]);
    const auto format = QImage::Format(fields[4].toInt(&ok[3]));
    const auto bpp = snip::recognitionPixelSize(format);
    const qint64 expected = qint64(stride) * height;
    if (!ok[0] || !ok[1] || !ok[2] || !ok[3] || !snip::recognitionImageSize({width, height}) ||
        !bpp || stride < width * bpp || stride > width * bpp + 4096 ||
        expected <= 0 || expected > snip::RecognitionMaxBytes) return reject("Image exceeds recognition limits.");
    while (true) {
      auto chunk = input.read(65536);
      if (chunk.isEmpty()) break;
      pixels += chunk;
      if (pixels.size() > expected) return reject("Unexpected image bytes.");
    }
    if (pixels.size() != expected) return reject("Incomplete image input.");
    image = QImage(reinterpret_cast<const uchar *>(pixels.constData()), width, height, stride, format);
  } else if (args[2] == "file" && args.size() == 4) {
    const QFileInfo file(args[3]);
    if (!file.isFile() || file.size() > snip::RecognitionMaxFileBytes) return reject("Image is missing or exceeds 64 MiB.");
    QImageReader::setAllocationLimit(128);
    QImageReader reader(args[3]);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    if (!QList<QByteArray>{"png", "jpeg", "jpg", "webp", "bmp", "tiff", "gif", "ppm", "pgm", "pbm"}.contains(reader.format()))
      return reject("Unsupported image type. Choose a local raster image.");
    if (!snip::recognitionImageSize(reader.size())) return reject("Image exceeds 16 megapixels or has invalid dimensions.");
    image = reader.read();
    if (image.isNull()) return reject("Cannot decode image: " + reader.errorString());
  } else return reject("Invalid recognition input.");
  if (image.isNull() || !snip::recognitionImageSize(image.size())) return reject("Invalid image.");
  image.setDevicePixelRatio(1);
  QImage opaque(image.size(), QImage::Format_RGB32);
  if (opaque.isNull()) return reject("Cannot allocate recognition image.");
  opaque.fill(Qt::white);
  { QPainter painter(&opaque); painter.drawImage(0, 0, image); }
  QFile output;
  if (!output.open(stdout, QIODevice::WriteOnly)) return reject("Cannot write recognition result.");
  if (args[1] == "image") {
    if (!opaque.save(&output, "PNG")) return reject("Cannot prepare OCR image.");
    return 0;
  }
  try {
    const auto gray = opaque.convertToFormat(QImage::Format_Grayscale8);
    const ZXing::ImageView view(gray.constBits(), gray.width(), gray.height(), ZXing::ImageFormat::Lum, gray.bytesPerLine());
    auto options = ZXing::ReaderOptions().setTryHarder(true).setTryRotate(true).setTryInvert(true)
                       .setMaxNumberOfSymbols(128).setTextMode(ZXing::TextMode::Plain);
    QJsonArray codes;
    for (const auto &barcode : ZXing::ReadBarcodes(view, options)) {
      if (!barcode.isValid()) continue;
      const auto &bytes = barcode.bytes();
      const QByteArray raw(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
      QJsonArray position;
      for (const auto &point : {barcode.position().topLeft(), barcode.position().topRight(),
                               barcode.position().bottomRight(), barcode.position().bottomLeft()})
        position.append(QJsonObject{{"x", point.x}, {"y", point.y}});
      const bool binary = barcode.contentType() == ZXing::ContentType::Binary;
      codes.append(QJsonObject{{"format", QString::fromStdString(ZXing::ToString(barcode.format()))},
          {"text", binary ? QString::fromLatin1(raw.toBase64()) : QString::fromStdString(barcode.text())},
          {"rawBase64", QString::fromLatin1(raw.toBase64())}, {"binary", binary}, {"position", position}});
    }
    const auto result = QJsonDocument(QJsonObject{{"codes", codes}}).toJson(QJsonDocument::Compact);
    if (result.size() > snip::RecognitionMaxResultBytes) return reject("Recognition result exceeds 1 MiB.");
    output.write(result);
  } catch (const std::exception &error) { return reject(QString::fromUtf8(error.what())); }
  return 0;
}
