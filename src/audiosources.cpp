#include "audiosources.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QSet>
#include <memory>
#include <cmath>

namespace snip {
AudioSources::~AudioSources() {
  cancel();
  for (auto *process : findChildren<QProcess *>()) {
    if (process->state() != QProcess::NotRunning) {
      process->kill();
      process->waitForFinished(1000);
    }
  }
}
namespace {
QString appKey(QString value) {
  value = value.toLower();
  if (value.endsWith(".desktop")) value.chop(8);
  return value;
}
bool indexValue(const QJsonValue &value, quint32 &index) {
  if (value.isDouble()) {
    const auto number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > 4294967294.0 || std::floor(number) != number) return false;
    index = quint32(number);
    return true;
  }
  if (value.isString()) {
    bool ok = false;
    const auto number = value.toString().toULongLong(&ok);
    if (!ok || number > 4294967294ULL) return false;
    index = quint32(number);
    return true;
  }
  return false;
}
}
QString AudioSources::streamIdentity(const QVariantMap &properties, quint32 client) {
  QJsonArray values{int(client)};
  for (const auto *key : {"application.process.id", "application.process.binary",
                          "application.id", "application.name"})
    values.append(properties.value(key).toString());
  return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(values).toJson(QJsonDocument::Compact),
                                                     QCryptographicHash::Sha256).toHex());
}
QVariantList AudioSources::parse(const QByteArray &devices, const QByteArray &inputs, QString *error) {
  QJsonParseError deviceError, inputError;
  const auto sourceDoc = QJsonDocument::fromJson(devices, &deviceError);
  const auto inputDoc = QJsonDocument::fromJson(inputs, &inputError);
  if (deviceError.error || inputError.error || !sourceDoc.isArray() || !inputDoc.isArray() ||
      sourceDoc.array().size() > 1024 || inputDoc.array().size() > 1024) {
    if (error) *error = "Invalid or oversized pactl JSON response.";
    return {};
  }
  QVariantList result{
      QVariantMap{{"id", "default:system"}, {"kind", "device"}, {"device", "@DEFAULT_MONITOR@"},
                  {"label", "All apps — default speakers"}, {"icon", "monitor"}},
      QVariantMap{{"id", "default:mic"}, {"kind", "device"}, {"device", ""},
                  {"label", "Default input device"}, {"icon", "mic"}}};
  QSet<QString> seen{"default:system", "default:mic"};
  for (const auto &value : sourceDoc.array()) {
    const auto source = value.toObject();
    const auto name = source["name"].toString();
    if (name.isEmpty()) continue;
    const bool monitor = source["monitor_of_sink"].toDouble(-1) >= 0 &&
                         source["monitor_of_sink"].toDouble() != 4294967295.0;
    const bool output = monitor || name.endsWith(".monitor") ||
                        !source["monitor_of_sink_name"].toString().isEmpty();
    const auto id = "source:" + name;
    if (seen.contains(id)) continue;
    seen.insert(id);
    const auto description = source["description"].toString(name);
    result.append(QVariantMap{{"id", id}, {"kind", "device"}, {"device", name},
        {"label", (output ? "All apps — " : "Input — ") + description},
        {"icon", output ? "monitor" : "mic"}});
  }
  for (const auto &value : inputDoc.array()) {
    const auto input = value.toObject();
    quint32 index = 0, client = 0;
    if (!indexValue(input["index"], index) || !indexValue(input["client"], client)) continue;
    const auto properties = input["properties"].toObject().toVariantMap();
    const auto name = properties.value("application.name").toString();
    const auto media = properties.value("media.name").toString();
    const auto identity = streamIdentity(properties, client);
    const auto id = QString("app:%1:%2").arg(index).arg(identity.left(16));
    if (seen.contains(id)) continue;
    seen.insert(id);
    QString label = name.isEmpty() ? properties.value("application.process.binary").toString() : name;
    if (label.isEmpty()) label = QString("Application stream %1").arg(index);
    if (!media.isEmpty() && media != label) label += " — " + media;
    if (input["mute"].toBool()) label += " (muted)";
    result.append(QVariantMap{{"id", id}, {"kind", "application"}, {"index", index},
        {"identity", identity}, {"label", label}, {"icon", "audio"},
        {"pid", properties.value("application.process.id").toString()},
        {"appId", properties.value("application.id").toString()},
        {"binary", properties.value("application.process.binary").toString()},
        {"appName", name}, {"mediaName", media}, {"windowName", properties.value("window.name")}});
  }
  if (error) error->clear();
  return result;
}
QStringList AudioSources::forWindow(const QVariantList &sources, const QVariantMap &window,
                                    const QVariantList &windows) {
  const auto pid = window.value("pid").toString();
  const auto app = appKey(window.value("appId").toString());
  const auto title = window.value("title").toString();
  if ((pid.isEmpty() || pid == "0") && app.isEmpty()) return {};
  QVariantList candidates;
  for (const auto &value : sources) {
    const auto source = value.toMap();
    if (source.value("kind") != "application") continue;
    const bool pidMatch = !pid.isEmpty() && pid != "0" && source.value("pid").toString() == pid;
    const bool appMatch = !app.isEmpty() &&
        (appKey(source.value("appId").toString()) == app ||
         appKey(source.value("binary").toString()) == app);
    // A known, conflicting process ID is never replaced by a name guess.
    if ((!pid.isEmpty() && pid != "0") ? pidMatch : appMatch) candidates.append(source);
  }
  int similarWindows = 0;
  int identicalTitles = 0;
  for (const auto &value : windows) {
    const auto other = value.toMap();
    const auto otherPid = other.value("pid").toString();
    const bool havePids = !pid.isEmpty() && pid != "0" && !otherPid.isEmpty() && otherPid != "0";
    const bool same = havePids ? otherPid == pid
        : (!app.isEmpty() && appKey(other.value("appId").toString()) == app);
    if (same) {
      ++similarWindows;
      if (!title.isEmpty() && other.value("title").toString() == title) ++identicalTitles;
    }
  }
  QStringList exact;
  for (const auto &value : candidates) {
    const auto source = value.toMap();
    if (!title.isEmpty() && (source.value("windowName").toString() == title ||
                             source.value("mediaName").toString() == title))
      exact.append(source.value("id").toString());
  }
  if (identicalTitles > 1 || (similarWindows > 1 && exact.size() > 1)) return {};
  if (!exact.isEmpty()) return exact;
  if (similarWindows > 1) return {}; // E.g. browser windows/tabs: do not guess.
  QStringList result;
  for (const auto &value : candidates) result.append(value.toMap().value("id").toString());
  return result;
}
void AudioSources::refresh() {
  if (m_process) return;
  m_devices.clear();
  query(false);
}
void AudioSources::query(bool inputs) {
  auto *process = new QProcess(this);
  m_process = process;
  auto bytes = std::make_shared<QByteArray>();
  auto problem = std::make_shared<QString>();
  connect(process, &QProcess::readyReadStandardOutput, this, [this, process, bytes, problem] {
    const auto chunk = process->readAllStandardOutput();
    if (m_process != process) return;
    if (bytes->size() + chunk.size() > 4 * 1024 * 1024) {
      *problem = "pactl metadata exceeded 4 MiB.";
      process->kill();
    } else *bytes += chunk;
  });
  connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
  connect(process, &QProcess::finished, this, [this, process, bytes, problem, inputs](int code, QProcess::ExitStatus status) {
    process->deleteLater();
    if (m_process != process) return;
    m_process = nullptr;
    if (status != QProcess::NormalExit || code || !problem->isEmpty()) {
      m_error = problem->isEmpty() ? "Cannot query pactl. Check PulseAudio/PipeWire-Pulse." : *problem;
      m_sources.clear();
      emit changed();
      return;
    }
    if (!inputs) { m_devices = *bytes; query(true); return; }
    m_sources = parse(m_devices, *bytes, &m_error);
    emit changed();
  });
  connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart || m_process != process) return;
    m_process = nullptr;
    process->deleteLater();
    m_sources.clear();
    m_error = "Cannot start pactl. Install pulseaudio-utils or your distribution's pactl package.";
    emit changed();
  });
  auto *timeout = new QTimer(process);
  timeout->setSingleShot(true);
  connect(timeout, &QTimer::timeout, process, [process, problem] {
    *problem = "pactl query timed out.";
    process->kill();
  });
  timeout->start(3000);
  process->start("pactl", {"--format=json", "list", inputs ? "sink-inputs" : "sources"}, QIODevice::ReadOnly);
  emit changed();
}
void AudioSources::cancel() {
  if (m_process) { auto *process = m_process.data(); m_process = nullptr; process->kill(); }
}
}
