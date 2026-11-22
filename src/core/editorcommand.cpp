#include "editorcommand.h"
#include <QFileInfo>
#include <QProcess>

namespace snip {
bool validateEditorCommand(const QString &command, QString *error) {
  const auto reject = [error](const QString &message) {
    if (error) *error = message;
    return false;
  };
  if (command.size() > 4096) return reject("Editor command must not exceed 4096 characters.");
  for (const auto ch : command)
    if (!ch.isPrint()) return reject("Editor command cannot contain control characters.");
  if (command.trimmed().isEmpty()) {
    if (error) error->clear();
    return true;
  }
  const auto parts = QProcess::splitCommand(command);
  if (parts.isEmpty() || parts.first().isEmpty()) return reject("Editor command must name an executable.");
  if (parts.first().contains("{}")) return reject("The editor executable cannot contain a filename placeholder.");
  if (error) error->clear();
  return true;
}
bool launchEditor(const QString &command, const QString &filename, QString *error) {
  if (!validateEditorCommand(command, error)) return false;
  auto parts = QProcess::splitCommand(command);
  if (parts.isEmpty()) {
    if (error) *error = "No editor command is configured.";
    return false;
  }
  if (!QFileInfo(filename).isFile()) {
    if (error) *error = "The saved file is unavailable.";
    return false;
  }
  QProcess process;
  process.setProgram(parts.takeFirst());
  bool placeholder = false;
  const auto absolute = QFileInfo(filename).absoluteFilePath();
  for (auto &part : parts) {
    if (!part.contains("{}")) continue;
    placeholder = true;
    // Substitute after splitting so spaces, quotes and shell punctuation in a
    // filename never become additional arguments or executable syntax.
    part.replace("{}", absolute);
  }
  process.setArguments(parts);
  if (!placeholder) process.setStandardInputFile(absolute);
  // Qt opens stdin read-only and passes the descriptor to the detached child.
  // This handles large recordings without buffers, temporary copies or a
  // long-running QProcess owned by the resident.
  if (!process.startDetached()) {
    if (error) *error = process.errorString();
    return false;
  }
  if (error) error->clear();
  return true;
}
}
