#include "recognition.h"
#include <QPainter>
#include <QTemporaryDir>
#include <QFile>
#include <QStandardPaths>
#include <QtTest>
#include <ZXing/MultiFormatWriter.h>
#include <ZXing/BitMatrix.h>
#include <ZXing/CharacterSet.h>

using namespace snip;
class RecognitionTest : public QObject {
  Q_OBJECT
  QImage code(const std::wstring &text, ZXing::BarcodeFormat format = ZXing::BarcodeFormat::QRCode) {
    const auto bits = ZXing::MultiFormatWriter(format).setEncoding(ZXing::CharacterSet::UTF8)
                          .setMargin(4).encode(text, 320, 320);
    QImage image(bits.width(), bits.height(), QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int y = 0; y < bits.height(); ++y)
      for (int x = 0; x < bits.width(); ++x) if (bits.get(x, y)) image.setPixelColor(x, y, Qt::black);
    return image;
  }
  QString script(QTemporaryDir &dir, const QByteArray &body) {
    const auto path = dir.filePath("fake");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    file.write("#!/bin/sh\n" + body);
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
  }
private slots:
  void qrAndBarcode() {
    Recognition recognition;
    QImage image(760, 400, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
      QPainter painter(&image);
      painter.drawImage(20, 20, code(L"https://example.com/test?a=1&b=2"));
      painter.drawImage(400, 20, code(L"SNIPLINE123", ZXing::BarcodeFormat::Code128));
    }
    recognition.startImage("qr", image);
    QTRY_VERIFY_WITH_TIMEOUT(!recognition.busy(), 20000);
    QCOMPARE(recognition.error(), QString{});
    QCOMPARE(recognition.codes().size(), 2);
    QStringList texts;
    for (const auto &value : recognition.codes()) {
      const auto result = value.toMap();
      texts.append(result["text"].toString());
      QCOMPARE(QByteArray::fromBase64(result["rawBase64"].toByteArray()), result["text"].toString().toUtf8());
      QCOMPARE(result["position"].toList().size(), 4);
    }
    QVERIFY(texts.contains("https://example.com/test?a=1&b=2"));
    QVERIFY(texts.contains("SNIPLINE123"));
  }
  void unicodeRotatedFile() {
    QTemporaryDir dir;
    const auto image = code(L"Zażółć 日本語").transformed(QTransform().rotate(90));
    const auto path = dir.filePath("image with spaces.png");
    QVERIFY(image.save(path));
    Recognition recognition;
    recognition.startFile("qr", path);
    QTRY_VERIFY_WITH_TIMEOUT(!recognition.busy(), 20000);
    QCOMPARE(recognition.error(), QString{});
    QCOMPARE(recognition.codes().size(), 1);
    QCOMPARE(recognition.codes().first().toMap()["text"].toString(), QString::fromUtf8("Zażółć 日本語"));
  }
  void blankAndInvalidFile() {
    Recognition recognition;
    QImage image(320, 240, QImage::Format_RGB32);
    image.fill(Qt::white);
    recognition.startImage("qr", image);
    QTRY_VERIFY(!recognition.busy());
    QCOMPARE(recognition.state(), "Done");
    QVERIFY(recognition.codes().isEmpty());
    recognition.startFile("qr", "/nonexistent/image.png");
    QTRY_VERIFY(!recognition.busy());
    QCOMPARE(recognition.state(), "Failed");
    QVERIFY(!recognition.error().isEmpty());
    QTemporaryDir dir;
    QFile svg(dir.filePath("unsafe.svg"));
    QVERIFY(svg.open(QIODevice::WriteOnly));
    svg.write("<svg xmlns='http://www.w3.org/2000/svg' width='20' height='20'/>");
    svg.close();
    recognition.startFile("qr", svg.fileName());
    QTRY_VERIFY(!recognition.busy());
    QCOMPARE(recognition.state(), "Failed");
  }
  void workerFailureAndCancellation() {
    QTemporaryDir dir;
    const auto path = script(dir, "while :; do :; done\n");
    QVERIFY(!path.isEmpty());
    Recognition recognition(nullptr, path);
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::white);
    recognition.startImage("qr", image);
    QVERIFY(recognition.busy());
    recognition.cancel();
    QVERIFY(!recognition.busy());
    QCOMPARE(recognition.state(), "Cancelled");
    QVERIFY(recognition.codes().isEmpty());
    Recognition missing(nullptr, dir.filePath("missing"));
    missing.startImage("qr", image);
    QTRY_VERIFY(!missing.busy());
    QCOMPARE(missing.state(), "Failed");
    const auto crash = script(dir, "kill -KILL $$\n");
    Recognition crashing(nullptr, crash);
    crashing.startImage("qr", image);
    QTRY_VERIFY(!crashing.busy());
    QCOMPARE(crashing.state(), "Failed");
  }
  void actualOcr() {
    if (QStandardPaths::findExecutable("tesseract").isEmpty()) QSKIP("Tesseract unavailable");
    QImage image(1000, 200, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
      QPainter painter(&image);
      painter.setPen(Qt::black);
      painter.setFont(QFont("DejaVu Sans", 42));
      painter.drawText(image.rect(), Qt::AlignCenter, "SNIPLINE TEST 123");
    }
    Recognition recognition;
    recognition.startImage("ocr", image);
    QTRY_VERIFY_WITH_TIMEOUT(!recognition.busy(), 70000);
    if (!recognition.languages().contains("eng")) QSKIP("English Tesseract language data unavailable");
    QCOMPARE(recognition.error(), QString{});
    QVERIFY2(recognition.text().contains("SNIPLINE TEST 123"), qPrintable(recognition.text()));
    QVERIFY(!recognition.selectedLanguages().isEmpty());
    recognition.setText("Edited result");
    QCOMPARE(recognition.text(), "Edited result");
    QTemporaryDir dir;
    const auto file = dir.filePath("ocr.png");
    QVERIFY(image.save(file));
    recognition.startFile("ocr", file);
    QTRY_VERIFY_WITH_TIMEOUT(!recognition.busy(), 70000);
    QCOMPARE(recognition.error(), QString{});
    QVERIFY(recognition.text().contains("SNIPLINE TEST 123"));
    image.fill(Qt::white);
    recognition.startImage("ocr", image);
    QTRY_VERIFY_WITH_TIMEOUT(!recognition.busy(), 70000);
    QCOMPARE(recognition.state(), "Done");
    QVERIFY(recognition.text().trimmed().isEmpty());
  }
  void languageSelectionAndErrors() {
    QTemporaryDir dir;
    auto path = script(dir, "if [ \"$1\" = --list-langs ]; then printf 'List of available languages (3):\\neng\\npol\\nosd\\n'; else cat >/dev/null; printf 'recognized text'; fi\n");
    Recognition recognition(nullptr, {}, path);
    recognition.refreshLanguages();
    QTRY_VERIFY(!recognition.busy());
    QCOMPARE(recognition.languages(), (QStringList{"eng", "pol"}));
    QCOMPARE(recognition.selectedLanguages(), (QStringList{"eng"}));
    recognition.toggleLanguage("pol");
    recognition.toggleLanguage("--bad");
    QCOMPARE(recognition.selectedLanguages(), (QStringList{"eng", "pol"}));
    recognition.toggleLanguage("eng");
    recognition.toggleLanguage("pol");
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::white);
    recognition.startImage("ocr", image);
    QVERIFY(!recognition.busy());
    QVERIFY(recognition.error().contains("Select at least"));
    recognition.toggleLanguage("eng");
    recognition.rerun();
    QTRY_VERIFY(!recognition.busy());
    QCOMPARE(recognition.text(), "recognized text");
    Recognition missing(nullptr, {}, dir.filePath("missing"));
    missing.startImage("ocr", image);
    QTRY_VERIFY(!missing.busy());
    QVERIFY(missing.error().contains("Tesseract"));
    Recognition absentLanguage(nullptr, {}, path);
    absentLanguage.setPreferredLanguages({"jpn"});
    absentLanguage.startImage("ocr", image);
    QTRY_VERIFY(!absentLanguage.busy());
    QVERIFY(absentLanguage.error().contains("not installed"));
    QVERIFY(absentLanguage.selectedLanguages().isEmpty());
    absentLanguage.toggleLanguage("pol");
    absentLanguage.rerun();
    QTRY_VERIFY(!absentLanguage.busy());
    QCOMPARE(absentLanguage.text(), "recognized text");
  }
  void malformedWorkerAndOutputLimits() {
    QTemporaryDir dir;
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (const auto &body : {QByteArray("cat >/dev/null; printf 'not-json'\n"),
                             QByteArray("cat >/dev/null; head -c 2097152 /dev/zero\n")}) {
      const auto path = script(dir, body);
      Recognition recognition(nullptr, path);
      recognition.startImage("qr", image);
      QTRY_VERIFY(!recognition.busy());
      QCOMPARE(recognition.state(), "Failed");
      QVERIFY(recognition.codes().isEmpty());
    }
  }
  void timeout() {
    QTemporaryDir dir;
    Recognition recognition(nullptr, script(dir, "exec sleep 30\n"));
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::white);
    recognition.startImage("qr", image);
    QTRY_VERIFY_WITH_TIMEOUT(!recognition.busy(), 18000);
    QVERIFY(recognition.error().contains("timed out"));
  }
};
QTEST_MAIN(RecognitionTest)
#include "recognition_test.moc"
