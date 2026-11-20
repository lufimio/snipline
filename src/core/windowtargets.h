#pragma once

#include "outputs.h"
#include <QPointF>
#include <QVariantList>

namespace snip {

// Compositor geometry is used for picking only, never to crop window pixels.
// Hyprland's clients array is bottom-to-top stacking order, not focus order.
// Return only the uppermost hit, respecting rendering layers.
QStringList windowTargetsAt(Compositor compositor, QPointF point,
                           const QVariantList &catalog, const QByteArray &outputs,
                           const QByteArray &windows, const QByteArray &workspaces,
                           QString *error);
QStringList niriPickedWindow(const QByteArray &json, const QVariantList &catalog, QString *error);
}
