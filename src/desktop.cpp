#include "desktop.h"
#include "controller.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

namespace snip {
DesktopIntegration::DesktopIntegration(Controller &controller) {
  m_tray.setIcon(QIcon::fromTheme("camera-photo", QIcon(":/qml/icons/capture-dark.svg")));
  m_tray.setToolTip("Snipline");
  const auto report = [](const QString &error) {
    if (!error.isEmpty()) qWarning().noquote() << error;
  };
  m_menu.addAction("Screenshot", &controller, [&controller, report] { report(controller.Capture("region")); });
  m_menu.addAction("Record", &controller, [&controller, report] {
    report(controller.RecordConfigured("region", {}, -1, -1));
  });
  m_menu.addAction("Scan QR", &controller, [&controller, report] { report(controller.Recognize("qr", "region", {})); });
  m_menu.addAction("Recognize text", &controller, [&controller, report] { report(controller.Recognize("ocr", "region", {})); });
  auto *recent = m_menu.addMenu("Recent files");
  QObject::connect(recent, &QMenu::aboutToShow, recent, [recent, &controller] {
    recent->clear();
    const auto entries = QJsonDocument::fromJson(controller.Recent().toUtf8()).array();
    for (const auto &value : entries) {
      const auto path = value.toObject()["path"].toString();
      auto *action = recent->addAction(QFileInfo(path).fileName(), &controller,
                                      [&controller, path] { controller.openRecent(path); });
      action->setToolTip(path);
      action->setEnabled(QFileInfo(path).isFile());
    }
    if (entries.isEmpty()) recent->addAction("No recent files")->setEnabled(false);
    else {
      recent->addSeparator();
      recent->addAction("Forget recent files", &controller, &Controller::ClearRecent);
    }
  });
  m_menu.addSeparator();
  m_menu.addAction("Pause / resume recording", &controller, [&controller, report] { report(controller.TogglePause()); });
  m_menu.addAction("Finish recording", &controller, [&controller, report] { report(controller.Stop()); });
  m_menu.addAction("Cancel upload", &controller, &Controller::cancelUpload);
  m_menu.addSeparator();
  m_menu.addAction("Quit", &controller, &Controller::Quit);
  m_tray.setContextMenu(&m_menu);
  if (!QSystemTrayIcon::isSystemTrayAvailable())
    qWarning() << "No system tray host detected; Snipline remains available through the CLI.";
  m_tray.show();
}
}
