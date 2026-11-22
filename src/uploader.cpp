#include "uploader.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QTextStream>
#include <QSaveFile>
#include <QStandardPaths>
#include <curl/curl.h>

namespace snip {
namespace {
bool writePrivate(const QString &path, const QByteArray &data) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
    return false;
  return file.write(data) == data.size() && file.commit();
}
QUrl webUrl(const QString &text) {
  for (const auto ch : text)
    if (!ch.isPrint() || ch.isSpace())
      return {};
  QUrl url(text, QUrl::StrictMode);
  if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
      (url.scheme() != "https" && url.scheme() != "http"))
    return {};
  return url;
}
} // namespace
Uploader::Uploader(QObject *parent, QString program, QString storage)
    : QObject(parent), m_program(std::move(program)),
      m_storage(std::move(storage)) {
  if (m_storage.isEmpty())
    m_storage = QDir(QStandardPaths::writableLocation(
                         QStandardPaths::GenericConfigLocation))
                    .filePath("snipline/uploaders");
  addBuiltins();
  m_selected = "catbox";
  m_timeout.setSingleShot(true);
  m_timeout.setTimerType(Qt::PreciseTimer);
  connect(&m_timeout, &QTimer::timeout, this,
          [this] { fail("Upload timed out. The saved file was kept."); });
}
Uploader::~Uploader() { cleanup(); }
bool Uploader::available() const { return true; }
void Uploader::addBuiltins() {
  m_destinations.clear();
  const auto add = [this](const QString &id, const QString &label,
                          const QString &endpoint, const QString &field,
                          const QJsonObject &arguments,
                          const QString &response) {
    m_destinations.append({id, id, label,
                           QJsonObject{{"Name", "snipline-upload"},
                                       {"RequestURL", endpoint},
                                       {"RequestMethod", "POST"},
                                       {"Body", "MultipartFormData"},
                                       {"FileFormName", field},
                                       {"Arguments", arguments},
                                       {"URL", response}},
                           QUrl(endpoint)});
  };
  add("catbox", "Catbox", "https://catbox.moe/user/api.php", "fileToUpload",
      {{"reqtype", "fileupload"}}, "{response}");
  for (const auto hours : {1, 12, 24, 72})
    add(QString("litterbox-%1h").arg(hours),
        QString("Litterbox (%1h)").arg(hours),
        "https://litterbox.catbox.moe/resources/internals/api.php",
        "fileToUpload",
        {{"reqtype", "fileupload"}, {"time", QString("%1h").arg(hours)}},
        "{response}");
  add("pomf", "pomf.lain.la", "https://pomf.lain.la/upload.php", "files[]",
      {{"output", "json"}}, "{json:files[0].url}");
}
QString Uploader::resolveDestination(const QString &id) const {
  for (const auto &destination : m_destinations)
    if (destination.id == id || destination.alias == id)
      return destination.id;
  return {};
}
QString Uploader::destinationLabel(const QString &id) const {
  const auto resolved = resolveDestination(id);
  for (const auto &destination : m_destinations)
    if (destination.id == resolved)
      return destination.label;
  return "Unavailable destination";
}
QVariantList Uploader::destinations() const {
  QVariantList values;
  for (const auto &destination : m_destinations)
    values.append(QVariantMap{
        {"id", destination.id},
        {"label", destination.label},
        {"endpoint",
         destination.endpoint.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment)
             .toString()}});
  return values;
}
bool Uploader::readDefinition(const QString &path, Destination *destination,
                              QString *error) const {
  const auto reject = [error](const QString &message) {
    if (error)
      *error = message;
    return false;
  };
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return reject("Cannot read uploader definition: " +
                  QFileInfo(path).fileName());
  const auto bytes = file.read(65537);
  if (bytes.size() > 65536)
    return reject("Uploader definitions must not exceed 64 KiB.");
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(bytes, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject())
    return reject("Expected a JSON .sxcu uploader definition.");
  auto object = document.object();
  const auto request = object.value("RequestURL").toString();
  const auto endpoint = webUrl(request);
  if (endpoint.isEmpty() || request.contains('{') || request.contains('}') ||
      !endpoint.fragment().isEmpty())
    return reject("Uploader RequestURL must be a fixed HTTP(S) endpoint "
                  "without credentials or templates.");
  const auto host = endpoint.host();
  if (endpoint.scheme() != "https" && host != "127.0.0.1" && host != "::1" &&
      host != "localhost")
    return reject("Plain HTTP uploader endpoints are only supported on "
                  "loopback addresses.");
  const auto method =
      object.value("RequestMethod")
          .toString(object.value("RequestType").toString("POST"))
          .toUpper();
  for (const auto &key : {"RequestMethod", "RequestType"})
    if (object.contains(key) && !object.value(key).isString())
      return reject("Uploader request methods must be strings.");
  if (method != "POST" && method != "PUT")
    return reject("Only POST/PUT uploader definitions are supported.");
  object["RequestMethod"] = method;
  object.remove("RequestType");
  if (object.value("Body").toString("MultipartFormData") != "MultipartFormData")
    return reject("Only MultipartFormData .sxcu bodies are supported.");
  if (object.value("FileFormName").toString().isEmpty())
    return reject("Uploader FileFormName must be a nonempty string.");
  if (object.contains("Arguments")) {
    if (!object["Arguments"].isObject())
      return reject("Uploader Arguments must be an object.");
    const auto args = object["Arguments"].toObject();
    for (auto it = args.begin(); it != args.end(); ++it)
      if (!it.value().isString() || it.value().toString().contains('{'))
        return reject(
            "Uploader Arguments must contain literal strings, not templates.");
  }
  if (object.contains("Headers")) {
    if (!object.value("Headers").isObject())
      return reject("Uploader Headers must be an object.");
    const auto headers = object.value("Headers").toObject();
    for (auto it = headers.begin(); it != headers.end(); ++it) {
      if (!it.value().isString())
        return reject("Uploader header values must be strings.");
      for (const auto ch : it.key() + it.value().toString())
        if (!ch.isPrint())
          return reject("Uploader headers cannot contain control characters.");
    }
  }
  if (!object.value("URL").isString() ||
      object.value("URL").toString().isEmpty())
    return reject("Uploader definitions must declare a response URL template.");
  const auto responseTemplate = object["URL"].toString();
  if (responseTemplate != "{response}" &&
      !QRegularExpression("^\\{json:[A-Za-z0-9_.\\[\\]-]+\\}$")
           .match(responseTemplate)
           .hasMatch())
    return reject("Supported URL templates are {response} and "
                  "{json:path.to.array[0].url}.");
  const auto name = object.value("Name")
                        .toString(QFileInfo(path).completeBaseName())
                        .trimmed();
  if (name.isEmpty() || name.size() > 80)
    return reject("Uploader names must contain 1–80 characters.");
  for (const auto ch : name)
    if (!ch.isPrint() || ch == '<' || ch == '>')
      return reject("Uploader names must be plain text without markup or "
                    "control characters.");
  object["Name"] = name;
  const auto normalized = QJsonDocument(object).toJson(QJsonDocument::Compact);
  const auto digest = QString::fromLatin1(
      QCryptographicHash::hash(normalized, QCryptographicHash::Sha256).toHex());
  *destination = {"sxcu:" + digest, "sxcu:" + name, name, object, endpoint};
  return true;
}
void Uploader::configure(const QString &destination,
                         const QStringList &definitions) {
  if (busy())
    return;
  addBuiltins();
  QStringList paths = definitions;
  paths.removeDuplicates();
  QString warning;
  if (paths.size() > 64)
    warning = "Only the first 64 uploader definitions were loaded.";
  for (const auto &path : paths.mid(0, 64)) {
    Destination imported;
    QString error;
    if (!readDefinition(path, &imported, &error)) {
      warning = error;
      continue;
    }
    if (!resolveDestination(imported.id).isEmpty())
      continue;
    bool duplicate = false;
    for (const auto &entry : m_destinations)
      if (entry.alias == imported.alias)
        duplicate = true;
    if (duplicate) {
      warning =
          "Duplicate uploader name; rename one definition before using it.";
      continue;
    }
    m_destinations.append(std::move(imported));
  }
  m_selected = resolveDestination(destination);
  m_error = m_selected.isEmpty()
                ? "The configured upload destination is unavailable. Select a "
                  "destination explicitly."
                : warning;
  emit changed();
}
void Uploader::setDestination(const QString &id) {
  if (busy())
    return;
  const auto resolved = resolveDestination(id);
  if (resolved.isEmpty()) {
    m_error = "Unknown upload destination.";
    emit changed();
    return;
  }
  m_selected = resolved;
  m_error.clear();
  emit changed();
}
bool Uploader::importDefinition(const QUrl &file) {
  if (busy() || !file.isLocalFile())
    return false;
  Destination imported;
  QString error;
  if (!readDefinition(file.toLocalFile(), &imported, &error)) {
    m_error = error;
    emit changed();
    return false;
  }
  for (const auto &entry : m_destinations) {
    if (entry.alias == imported.alias && entry.id != imported.id) {
      m_error = "An uploader with that name already exists. Rename the "
                "definition to avoid replacing its destination.";
      emit changed();
      return false;
    }
  }
  if (resolveDestination(imported.id).isEmpty() &&
      m_destinations.size() >= 70) {
    m_error = "Too many uploader definitions.";
    emit changed();
    return false;
  }
  if (!QDir().mkpath(m_storage) ||
      !writePrivate(QDir(m_storage).filePath(imported.id.mid(5) + ".sxcu"),
                    QJsonDocument(imported.definition).toJson())) {
    m_error = "Could not save uploader definition.";
    emit changed();
    return false;
  }
  if (resolveDestination(imported.id).isEmpty())
    m_destinations.append(imported);
  m_selected = imported.id;
  m_error.clear();
  emit changed();
  return true; // Importing/selecting never starts a network request.
}
void Uploader::cleanup() {
  ++m_generation;
  m_timeout.stop();
  if (m_worker) {
    m_cancelled = true;
    m_worker->quit();
    m_worker->wait();
    delete m_worker;
    m_worker = nullptr;
  }
  m_temporary.reset();
}
void Uploader::fail(const QString &message) {
  QTextStream(stdout) << "Upload failed: " << message << Qt::endl;
  cleanup();
  m_status = "Failed";
  m_error = message;
  emit changed();
  emit failed(message);
}
void Uploader::cancel() {
  if (!busy())
    return;
  cleanup();
  m_status = "Cancelled";
  QTextStream(stdout) << "Upload cancelled; local file retained." << Qt::endl;
  m_error.clear();
  emit changed();
  emit cancelled();
}
bool Uploader::start(const QString &filename, const QString &destination,
                     bool lens, int timeoutMs) {
  if (busy())
    return false;
  m_error.clear();
  m_output.clear();
  m_url = QUrl{};
  const auto resolved = resolveDestination(destination);
  const QFileInfo source(filename);
  if (resolved.isEmpty()) {
    fail("The requested upload destination is unavailable. No fallback host "
         "was used.");
    return false;
  }
  if (!source.isFile() || !source.isReadable() || source.size() <= 0) {
    fail("Choose an existing, readable, nonempty file.");
    return false;
  }
  if (lens) {
    const auto mime =
        QMimeDatabase()
            .mimeTypeForFile(filename, QMimeDatabase::MatchContent)
            .name();
    if (!QStringList{"image/png", "image/jpeg", "image/webp", "image/gif"}
             .contains(mime) ||
        source.size() > 20 * 1024 * 1024) {
      fail("Lens requires a PNG, JPEG, WebP or GIF image no larger than 20 "
           "MiB.");
      return false;
    }
  }
  QJsonObject definition;
  for (const auto &entry : m_destinations)
    if (entry.id == resolved)
      definition = entry.definition;
  m_filename = source.absoluteFilePath();
  m_destination = resolved;
  m_lens = lens;
  m_status = "Uploading to " + destinationLabel(resolved);
  QTextStream(stdout) << "Upload starting: " << source.fileName()
                     << " (" << source.size() << " bytes), destination: "
                     << destinationLabel(resolved) << (lens ? " (Google Lens)" : "") << Qt::endl;
  m_cancelled = false;
  const auto generation = ++m_generation;
  m_worker = QThread::create([this, definition, timeoutMs, generation] {
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    Q_UNUSED(initialized);
    QByteArray response;
    QString error;
    auto *curl = curl_easy_init();
    if (!curl)
      error = "Cannot initialize libcurl.";
    else {
      char details[CURL_ERROR_SIZE]{};
      const auto endpoint = definition["RequestURL"].toString().toUtf8();
      const QUrl endpointUrl(QString::fromUtf8(endpoint));
      QTextStream(stdout) << "Upload request: " << definition["RequestMethod"].toString("POST")
                         << " to " << endpointUrl.scheme() << "://" << endpointUrl.host()
                         << " (multipart; timeout " << timeoutMs << " ms)" << Qt::endl;
      curl_easy_setopt(curl, CURLOPT_URL, endpoint.constData());
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https,http");
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                       long(qBound(100, timeoutMs, 3600000)));
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
      curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, details);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "snipline");
      curl_easy_setopt(
          curl, CURLOPT_WRITEFUNCTION,
          +[](char *data, size_t size, size_t count, void *context) -> size_t {
            auto &bytes = *static_cast<QByteArray *>(context);
            const auto n = size * count;
            if (n > 1048576 || size_t(bytes.size()) + n > 1048576)
              return 0;
            bytes.append(data, qsizetype(n));
            return n;
          });
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(
          curl, CURLOPT_XFERINFOFUNCTION,
          +[](void *context, curl_off_t, curl_off_t, curl_off_t,
              curl_off_t) -> int {
            return static_cast<Uploader *>(context)->m_cancelled ? 1 : 0;
          });
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
      auto *mime = curl_mime_init(curl);
      curl_mimepart *part = nullptr;
      const auto arguments = definition["Arguments"].toObject();
      for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, it.key().toUtf8().constData());
        curl_mime_data(part, it.value().toString().toUtf8().constData(),
                       CURL_ZERO_TERMINATED);
      }
      part = curl_mime_addpart(mime);
      curl_mime_name(part, definition["FileFormName"].toString("file").toUtf8().constData());
      curl_mime_filedata(part, QFile::encodeName(m_filename).constData());
      curl_slist *headers = nullptr;
      const auto customHeaders = definition["Headers"].toObject();
      for (auto it = customHeaders.begin(); it != customHeaders.end(); ++it)
        headers = curl_slist_append(
            headers,
            (it.key() + ": " + it.value().toString()).toUtf8().constData());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
      const auto method = definition["RequestMethod"].toString("POST").toUtf8();
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.constData());
      const auto result = curl_easy_perform(curl);
      long http = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
      double elapsed = 0;
      curl_off_t uploaded = 0;
      curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &elapsed);
      curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &uploaded);
      QTextStream(stdout) << "Upload response: HTTP " << http << ", curl " << int(result)
                         << " (" << curl_easy_strerror(result) << "), " << uploaded
                         << " bytes sent, " << response.size() << " response bytes, "
                         << elapsed << " seconds" << Qt::endl;
      if (result != CURLE_OK)
        error = QString("libcurl %1: %2 — %3 (HTTP %4). Saved file kept.")
                    .arg(int(result))
                    .arg(curl_easy_strerror(result), QString::fromUtf8(details))
                    .arg(http);
      else if (http < 200 || http >= 300)
        error = QString("Upload returned HTTP %1. Saved file kept.").arg(http);
      // Built-in hosts return useful short plain-text rejections (for example,
      // "No file!"). Never echo arbitrary custom responses or HTML error pages:
      // these can contain credentials, markup or terminal control sequences.
      if (!error.isEmpty() && !m_destination.startsWith("sxcu:")) {
        const auto reason = QString::fromUtf8(response).trimmed();
        bool plain = !reason.isEmpty() && reason.size() <= 256
                     && !reason.contains('<') && !reason.contains('>');
        for (const auto ch : reason)
          if (!ch.isPrint()) plain = false;
        if (plain) error += " Host response: " + reason;
      }
      curl_slist_free_all(headers);
      curl_mime_free(mime);
      curl_easy_cleanup(curl);
    }
    QMetaObject::invokeMethod(
        this,
        [this, response, error, generation] {
          if (generation != m_generation || m_cancelled)
            return;
          cleanup();
          if (!error.isEmpty()) {
            fail(error);
            return;
          }
          QString text = QString::fromUtf8(response).trimmed();
          const auto pattern = [this] {
            for (const auto &entry : m_destinations)
              if (entry.id == m_destination)
                return entry.definition["URL"].toString();
            return QString{};
          }();
          if (pattern.startsWith("{json:") && pattern.endsWith('}')) {
            QJsonValue value = QJsonDocument::fromJson(response).object();
            QString path = pattern.mid(6, pattern.size() - 7);
            path.replace('[', '.');
            path.remove(']');
            for (const auto &key : path.split('.', Qt::SkipEmptyParts)) {
              if (value.isArray()) {
                bool ok = false;
                const auto index = key.toInt(&ok);
                const auto array = value.toArray();
                value = ok && index >= 0 && index < array.size()
                            ? array.at(index)
                            : QJsonValue{};
              } else
                value = value.toObject().value(key);
            }
            text = value.toString();
          } else if (pattern != "{response}")
            text.clear();
          const auto url = webUrl(text);
          if (url.isEmpty()) {
            QString message = "The host response did not contain a valid HTTP(S) URL "
                              "(expected configured response template).";
            if (!m_destination.startsWith("sxcu:") && pattern == "{response}") {
              const auto reason = QString::fromUtf8(response).trimmed();
              bool plain = !reason.isEmpty() && reason.size() <= 256
                           && !reason.contains('<') && !reason.contains('>');
              for (const auto ch : reason)
                if (!ch.isPrint()) plain = false;
              if (plain) message += " Host response: " + reason;
            }
            fail(message);
            return;
          }
          m_url = url;
          m_status = "Done";
          QTextStream(stdout) << "Upload complete: " << destinationLabel(m_destination)
                             << "; public URL received; local file retained." << Qt::endl;
          emit changed();
          emit completed(url, m_filename, m_destination, m_lens);
        },
        Qt::QueuedConnection);
  });
  m_worker->start();
  emit changed();
  return busy();
}
} // namespace snip
