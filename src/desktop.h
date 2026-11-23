#pragma once
#include <QMenu>
#include <QSystemTrayIcon>

namespace snip {
class Controller;

// Optional desktop shell integration. No captures or transfers on construction.
class DesktopIntegration final {
public:
  explicit DesktopIntegration(Controller &controller);

private:
  QMenu m_menu;
  QSystemTrayIcon m_tray;
};
} // namespace snip
