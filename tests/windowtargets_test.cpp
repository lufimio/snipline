#include "core/windowtargets.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace snip;

class WindowTargetsTest : public QObject {
  Q_OBJECT
  QVariantList catalog{
      QVariantMap{{"id", "bottom"}, {"address", "10000000a"}},
      QVariantMap{{"id", "top"}, {"address", "20000000a"}}};
  QByteArray monitors = R"([{"name":"test","x":-200,"y":-100,"width":400,"height":300,
    "scale":1,"transform":0,"activeWorkspace":{"id":1},"specialWorkspace":{"id":0}}])";
  QJsonObject window(QString address, bool floating = true) {
    return {{"address", address}, {"at", QJsonArray{-50, -50}}, {"size", QJsonArray{100, 100}},
            {"mapped", true}, {"hidden", false}, {"floating", floating},
            {"workspace", QJsonObject{{"id", 1}}}};
  }
  QStringList pick(QJsonArray windows, const QVariantList &entries, QString *error) {
    return windowTargetsAt(Compositor::Hyprland, {-10, 10}, entries, monitors,
                           QJsonDocument(windows).toJson(), {}, error);
  }
private slots:
  void uppermostStackEntryWinsNotFocusHistory()
  {
    auto bottom = window("0x10000000a"), top = window("0x20000000a");
    bottom["focusHistoryID"] = 0;
    top["focusHistoryID"] = 99;
    QString error;
    QCOMPARE(pick({bottom, top}, catalog, &error), QStringList{"top"});
    QVERIFY(error.isEmpty());
    QCOMPARE(pick({top, bottom}, catalog, &error), QStringList{"bottom"});
  }
  void floatingPinnedAndFullscreenLayers()
  {
    auto bottom = window("0x10000000a"), top = window("0x20000000a", false);
    QString error;
    QCOMPARE(pick({bottom, top}, catalog, &error), QStringList{"bottom"}); // Floating over tiled.
    top["fullscreen"] = 2;
    QCOMPARE(pick({bottom, top}, catalog, &error), QStringList{"top"});
    top["floating"] = true; // A floating window can itself enter fullscreen.
    QCOMPARE(pick({bottom, top}, catalog, &error), QStringList{"top"});
    bottom["allowedOverFullscreen"] = true;
    QCOMPARE(pick({bottom, top}, catalog, &error), QStringList{"bottom"});
    bottom["allowedOverFullscreen"] = false;
    bottom["pinned"] = true;
    QCOMPARE(pick({bottom, top}, catalog, &error), QStringList{"bottom"});
  }
  void invisibleWindowsAreNotHit()
  {
    auto bottom = window("0x10000000a"), top = window("0x20000000a");
    QString error;
    top["hidden"] = true;
    QCOMPARE(pick({bottom, top}, catalog, &error), QStringList{"bottom"});
    top["hidden"] = false;
    top["workspace"] = QJsonObject{{"id", 2}};
    QCOMPARE(pick({bottom, top}, catalog, &error), QStringList{"bottom"});
    top["workspace"] = QJsonObject{{"id", 1}};
    top["mapped"] = false;
    QCOMPARE(pick({bottom, top}, catalog, &error), QStringList{"bottom"});
  }
  void unavailableUppermostNeverFallsThrough()
  {
    QString error;
    const QVariantList onlyBottom{catalog.first()};
    QVERIFY(pick({window("0x10000000a"), window("0x20000000a")}, onlyBottom, &error).isEmpty());
    QVERIFY(error.contains("topmost"));
    QVERIFY(pick({window("0x10000000a")}, {QVariantMap{{"id", "wrong"}, {"title", "same title"}}}, &error).isEmpty());
    QVERIFY(error.contains("mapping"));
  }
  void backgroundAndInvalidMetadata()
  {
    QString error;
    QCOMPARE(pick({}, catalog, &error).size(), 0);
    QVERIFY(error.isEmpty());
    QVERIFY(windowTargetsAt(Compositor::Hyprland, {0, 0}, catalog, monitors, "not JSON", {}, &error).isEmpty());
    QVERIFY(!error.isEmpty());
  }
  void niriUsesExactIdAndCancellationIsNotAnError()
  {
    const QVariantList windows{QVariantMap{{"id", "retained"}, {"identifier", "9007199254740993"}}};
    QString error;
    QCOMPARE(niriPickedWindow(R"({"id":9007199254740993,"title":"not used"})", windows, &error),
             QStringList{"retained"});
    QVERIFY(error.isEmpty());
    QVERIFY(niriPickedWindow("null", windows, &error).isEmpty());
    QVERIFY(error.isEmpty());
    QVERIFY(niriPickedWindow(R"({"id":42})", windows, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(niriPickedWindow("{", windows, &error).isEmpty());
    QVERIFY(!error.isEmpty());
  }
};

QTEST_GUILESS_MAIN(WindowTargetsTest)
#include "windowtargets_test.moc"
