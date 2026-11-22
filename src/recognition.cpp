#include "recognition.h"
#include "recognitionprotocol.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStandardPaths>

namespace snip {
Recognition::Recognition(QObject *parent, QString worker, QString tesseract)
    : QObject(parent), m_worker(std::move(worker)), m_tesseract(std::move(tesseract)) {
  m_timeout.setSingleShot(true);
  m_timeout.setTimerType(Qt::PreciseTimer);
  connect(&m_timeout, &QTimer::timeout, this, [this] { complete("Recognition timed out. Try a smaller image."); });
}
Recognition::~Recognition() { clearProcess(); }
void Recognition::clearProcess() {
  m_timeout.stop();
  if (m_process) {
    m_process->disconnect(this);
    if (m_process->state() != QProcess::NotRunning) {
      m_process->kill();
      m_process->waitForFinished(1000);
    }
    m_process->deleteLater();
    m_process = nullptr;
  }
  m_input.clear();
  m_output.clear();
  m_diagnostic.clear();
}
void Recognition::cancel() {
  clearProcess();
  m_busy = false;
  m_state = "Cancelled";
  m_error.clear();
  emit changed();
}
void Recognition::clear() {
  cancel();
  m_image = {};
  m_file.clear();
  m_text.clear();
  m_codes.clear();
  m_state = "Idle";
  emit changed();
}
void Recognition::prepare(const QString &kind) { clear(); m_kind = kind; emit changed(); }
void Recognition::setText(const QString &text) {
  if (m_busy || text.size() > RecognitionMaxResultBytes || text == m_text) return;
  m_text = text;
  emit changed();
}
void Recognition::startImage(const QString &kind, const QImage &image) {
  prepare(kind);
  m_image = image;
  begin();
}
void Recognition::startFile(const QString &kind, const QString &file) {
  prepare(kind);
  m_file = file;
  begin();
}
void Recognition::rerun() { if (!m_busy) begin(); }
void Recognition::toggleLanguage(const QString &language) {
  if (m_busy || !m_languages.contains(language)) return;
  if (m_selectedLanguages.contains(language)) m_selectedLanguages.removeAll(language);
  else m_selectedLanguages.append(language);
  emit changed();
}
void Recognition::refreshLanguages() {
  if (m_busy) return;
  m_runAfterDiscovery = false;
  m_stage = Stage::Languages;
  m_error.clear();
  m_busy = true;
  m_state = "Languages";
  const auto program = m_tesseract.isEmpty() ? QStandardPaths::findExecutable("tesseract") : m_tesseract;
  if (program.isEmpty()) { complete("Tesseract is not installed. Install tesseract and language data to use OCR."); return; }
  launch(program, {"--list-langs"}, 65536, 5000);
}
void Recognition::begin() {
  clearProcess();
  m_codes.clear();
  m_text.clear();
  m_error.clear();
  m_busy = true;
  m_state = "Scanning";
  if (m_kind != "qr" && m_kind != "ocr") { complete("Unsupported recognition action."); return; }
  if (!hasInput()) { complete("Choose an image or capture a selection first."); return; }
  if (m_kind == "ocr" && m_languages.isEmpty()) {
    m_busy = false;
    refreshLanguages();
    m_runAfterDiscovery = m_busy;
    return;
  }
  if (m_kind == "ocr" && m_selectedLanguages.isEmpty()) { complete("Select at least one installed OCR language."); return; }
  if (m_kind == "ocr") {
    for (const auto &language : m_selectedLanguages)
      if (!m_languages.contains(language)) { complete("OCR language is not installed: " + language); return; }
  }
  m_stage = m_kind == "qr" ? Stage::Qr : Stage::Image;
  QStringList args{m_kind == "qr" ? "qr" : "image"};
  if (!m_image.isNull()) {
    if (!recognitionImageSize(m_image.size()) || m_image.sizeInBytes() > RecognitionMaxBytes ||
        !recognitionPixelSize(m_image.format())) { complete("Image exceeds recognition limits or has an unsupported pixel format."); return; }
    m_input = {QString("SNIPLINE_IMAGE %1 %2 %3 %4\n").arg(m_image.width()).arg(m_image.height())
                   .arg(m_image.bytesPerLine()).arg(int(m_image.format())).toLatin1(),
               QByteArray::fromRawData(reinterpret_cast<const char *>(m_image.constBits()), m_image.sizeInBytes())};
    args += "raw";
  } else args += QStringList{"file", m_file};
  const auto program = m_worker.isEmpty() ? QDir(QCoreApplication::applicationDirPath()).filePath("snipline-recognize") : m_worker;
  launch(program, args, m_stage == Stage::Qr ? RecognitionMaxResultBytes : RecognitionMaxBytes, 15000);
}
void Recognition::launch(const QString &program, const QStringList &args, qint64 limit, int timeout) {
  m_output.clear();
  m_diagnostic.clear();
  m_part = 0;
  m_offset = 0;
  m_writeClosed = false;
  m_outputLimit = limit;
  auto *process = new QProcess(this);
  m_process = process;
  connect(process, &QProcess::started, this, &Recognition::pump);
  connect(process, &QProcess::bytesWritten, this, [this] { pump(); });
  connect(process, &QProcess::readyReadStandardOutput, this, &Recognition::drain);
  connect(process, &QProcess::readyReadStandardError, this, [this, process] {
    if (process == m_process) m_diagnostic = (m_diagnostic + process->readAllStandardError()).right(8192);
  });
  connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
    if (process == m_process && error == QProcess::FailedToStart)
      complete(m_stage == Stage::Languages || m_stage == Stage::Ocr
          ? "Cannot start Tesseract. Install tesseract and language data to use OCR."
          : "Cannot start recognition worker. Reinstall Snipline with snipline-recognize.");
  });
  connect(process, &QProcess::finished, this, &Recognition::processFinished);
  m_timeout.start(timeout);
  emit changed();
  process->start(program, args);
}
void Recognition::pump() {
  if (!m_process || m_process->state() != QProcess::Running || m_writeClosed) return;
  while (m_part < m_input.size() && m_process->bytesToWrite() < 131072) {
    const auto &part = m_input[m_part];
    const auto remaining = part.size() - m_offset;
    if (!remaining) { ++m_part; m_offset = 0; continue; }
    const auto written = m_process->write(part.constData() + m_offset, qMin<qsizetype>(65536, remaining));
    if (written < 0) { complete("Could not send image to recognition worker."); return; }
    if (!written) return;
    m_offset += written;
  }
  if (m_part == m_input.size()) { m_writeClosed = true; m_process->closeWriteChannel(); }
}
void Recognition::drain() {
  if (!m_process) return;
  m_output += m_process->readAllStandardOutput();
  if (m_output.size() > m_outputLimit) complete("Recognition output exceeds the allowed size.");
}
void Recognition::processFinished(int code, QProcess::ExitStatus status) {
  auto *process = m_process;
  drain();
  if (!m_process || process != m_process) return;
  m_diagnostic = (m_diagnostic + process->readAllStandardError()).right(8192);
  if (status != QProcess::NormalExit || code != 0) { complete("Recognition failed: " + QString::fromUtf8(m_diagnostic)); return; }
  if (m_stage == Stage::Languages) {
    const auto output = QString::fromUtf8(m_output);
    m_languages.clear();
    const QRegularExpression valid("^[A-Za-z0-9_]+(?:/[A-Za-z0-9_]+)*$");
    for (const auto &line : output.split('\n')) {
      const auto language = line.trimmed();
      if (language != "osd" && valid.match(language).hasMatch() && !m_languages.contains(language))
        m_languages.append(language);
    }
    m_languages.sort();
    QStringList selected, missing;
    for (const auto &language : m_selectedLanguages) {
      if (m_languages.contains(language)) selected.append(language);
      else missing.append(language);
    }
    m_selectedLanguages = selected;
    if (m_languages.isEmpty()) { complete("No installed OCR languages found. Install Tesseract language data."); return; }
    if (!missing.isEmpty()) {
      complete("OCR languages not installed: " + missing.join(", ") + ". Install them or select available languages and run OCR again.");
      return;
    }
    const bool run = m_runAfterDiscovery;
    m_runAfterDiscovery = false;
    clearProcess();
    m_busy = false;
    if (run) begin();
    else { m_state = "Ready"; emit changed(); }
    return;
  }
  if (m_stage == Stage::Image) {
    const auto png = m_output;
    clearProcess();
    m_input = {png};
    m_stage = Stage::Ocr;
    const auto program = m_tesseract.isEmpty() ? QStandardPaths::findExecutable("tesseract") : m_tesseract;
    if (program.isEmpty()) { complete("Tesseract is not installed."); return; }
    launch(program, {"stdin", "stdout", "-l", m_selectedLanguages.join('+'), "--psm", "3"},
           RecognitionMaxResultBytes, 60000);
    return;
  }
  if (m_stage == Stage::Ocr) {
    m_text = QString::fromUtf8(m_output);
    complete();
    return;
  }
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(m_output, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject() || !document.object()["codes"].isArray()) {
    complete("Invalid recognition worker response.");
    return;
  }
  const auto codes = document.object()["codes"].toArray();
  if (codes.size() > 128) { complete("Too many recognized codes."); return; }
  for (const auto &value : codes) {
    if (!value.isObject() || !value.toObject()["text"].isString() || !value.toObject()["rawBase64"].isString()) {
      complete("Invalid barcode payload.");
      return;
    }
    m_codes.append(value.toObject().toVariantMap());
  }
  complete();
}
void Recognition::complete(const QString &error) {
  clearProcess();
  m_busy = false;
  m_error = error;
  m_state = error.isEmpty() ? "Done" : "Failed";
  if (!error.isEmpty()) { m_codes.clear(); m_text.clear(); }
  emit changed();
  emit finished(error.isEmpty());
}
}
