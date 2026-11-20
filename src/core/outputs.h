#pragma once

#include "geometry.h"

namespace snip {

enum class Compositor { Unsupported, Hyprland, Niri };
Compositor currentCompositor();
QString compositorName(Compositor compositor);
QString discoveryProgram(Compositor compositor);
QStringList discoveryArguments(Compositor compositor);
QVector<Output> parseOutputs(Compositor compositor, const QByteArray &json,
                             QString *error);

} // namespace snip
