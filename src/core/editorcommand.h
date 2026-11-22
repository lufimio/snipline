#pragma once
#include <QString>

namespace snip {
// Empty commands are allowed in configuration (editing disabled).
bool validateEditorCommand(const QString &command, QString *error = nullptr);
// Launch independently of the resident. {} substitutes a filename after
// argument parsing; without it, the saved file is connected directly to stdin.
bool launchEditor(const QString &command, const QString &filename, QString *error = nullptr);
}
