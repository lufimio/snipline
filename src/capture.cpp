#include "capture.h"

#include <QTimer>
#include <memory>

namespace snip {

GrimCapture::GrimCapture(QObject *parent)
    : CaptureBackend(parent), m_compositor(currentCompositor()) {}

void GrimCapture::run(const QString &program, const QStringList &arguments,
                      Completion complete, qsizetype limit) {
  auto *process = new QProcess(this);
  m_process = process;
  struct Result {
    QByteArray bytes;
    bool finished = false;
    QString error;
  };
  auto result = std::make_shared<Result>();
  auto finish = [this, process, result, complete](QString error) {
    if (result->finished || m_process != process)
      return;
    result->finished = true;
    m_process = nullptr;
    complete(std::move(result->bytes), std::move(error));
  };
  connect(process, &QProcess::readyReadStandardOutput, this,
          [process, result, limit] {
            const auto bytes = process->readAllStandardOutput();
            if (result->bytes.size() + bytes.size() > limit) {
              result->error =
                  QStringLiteral("Capture backend exceeded its output limit.");
              process->kill();
            } else {
              result->bytes += bytes;
            }
          });
  connect(process, &QProcess::readyReadStandardError, process,
          [process] { process->readAllStandardError(); });
  connect(
      process, &QProcess::finished, this,
      [process, result, finish](int code, QProcess::ExitStatus status) {
        QString error = result->error;
        if (error.isEmpty() && (status != QProcess::NormalExit || code != 0))
          error = QStringLiteral("Native capture command failed. Run snipline "
                                 "doctor; this protocol may be unsupported.");
        finish(error);
        process->deleteLater();
      });
  connect(process, &QProcess::errorOccurred, this,
          [process, finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
              finish(
                  QStringLiteral("Cannot start %1. Check that it is installed.")
                      .arg(process->program()));
              process->deleteLater();
            }
          });
  auto *timeout = new QTimer(process);
  timeout->setSingleShot(true);
  connect(timeout, &QTimer::timeout, process, [process, result] {
    result->error = QStringLiteral("Capture backend timed out.");
    process->kill();
  });
  timeout->start(15000);
  process->start(program, arguments, QIODevice::ReadOnly);
}

void GrimCapture::discover(std::function<void(QVector<Output>)> complete) {
  if (m_compositor == Compositor::Unsupported) {
    emit failed(QStringLiteral(
        "Only Hyprland and niri are supported by this prototype."));
    return;
  }
  run(discoveryProgram(m_compositor), discoveryArguments(m_compositor),
      [this, complete](QByteArray bytes, QString error) {
        if (!error.isEmpty()) {
          emit failed(error);
          return;
        }
        auto outputs = parseOutputs(m_compositor, bytes, &error);
        if (!error.isEmpty()) {
          emit failed(error);
          return;
        }
        complete(std::move(outputs));
      });
}

void GrimCapture::start() {
  cancel();
  discover([this](QVector<Output> outputs) {
    m_outputs = std::move(outputs);
    nextFrame();
  });
}

void GrimCapture::startLive() {
  cancel();
  discover([this](QVector<Output> outputs) {
    QVector<Frame> placeholders;
    for (const auto &output : outputs) placeholders.push_back({output, {}});
    emit ready(std::move(placeholders));
  });
}

void GrimCapture::captureLive(const QVector<Output> &expected) {
  cancel();
  discover([this, expected](QVector<Output> outputs) {
    if (outputs != expected) {
      emit failed("Output layout changed before live capture. No image was saved.");
      return;
    }
    m_outputs = std::move(outputs);
    nextFrame(); // Also verifies the layout after the final frame.
  });
}

void GrimCapture::nextFrame() {
  if (m_frames.size() == m_outputs.size()) {
    discover([this](QVector<Output> outputs) {
      if (outputs != m_outputs) {
        cancel();
        emit failed(
            QStringLiteral("Output layout changed. Start a new selection."));
        return;
      }
      auto frames = std::move(m_frames);
      m_frames.clear();
      emit ready(std::move(frames));
    });
    return;
  }
  const auto output = m_outputs.at(m_frames.size());
  if (output.logical.width() * output.logical.height() * output.scale *
          output.scale >
      100'000'000) {
    emit failed(
        QStringLiteral("Output exceeds the 100 megapixel capture limit."));
    return;
  }
  run(
      QStringLiteral("grim"),
      {"-o", output.name, "-s", QString::number(output.scale, 'g', 12), "-t",
       "ppm", "-"},
      [this, output](QByteArray bytes, QString error) {
        if (!error.isEmpty()) {
          m_frames.clear();
          emit failed(error);
          return;
        }
        auto image = QImage::fromData(bytes, "PPM");
        if (image.isNull()) {
          m_frames.clear();
          emit failed(
              QStringLiteral("Capture backend returned an invalid image."));
          return;
        }
        const QSizeF expected = output.logical.size() * output.scale;
        if (qAbs(image.width() - expected.width()) > output.scale + 1 ||
            qAbs(image.height() - expected.height()) > output.scale + 1) {
          m_frames.clear();
          emit failed(QStringLiteral(
              "Capture dimensions do not match the output layout."));
          return;
        }
        m_frames.push_back({output, std::move(image)});
        nextFrame();
      },
      400 * 1024 * 1024);
}

void GrimCapture::verifyLayout(const QVector<Output> &expected) {
  discover([this, expected](QVector<Output> outputs) {
    if (outputs != expected)
      emit failed(QStringLiteral(
          "Output layout changed. Cancel and start a new selection."));
    else
      emit layoutVerified();
  });
}

void GrimCapture::cancel() {
  if (auto *process = m_process.data()) {
    m_process = nullptr;
    process->kill();
  }
  m_frames.clear();
  m_outputs.clear();
}

} // namespace snip
