#include "core/exporter.h"
#include "core/geometry.h"
#include "core/outputs.h"
#include "core/selection.h"
#include "core/theme.h"

#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>

using namespace snip;

class CoreTest : public QObject {
    Q_OBJECT
private slots:
    void mixedScale();
    void negativeAndRotated();
    void gapsAndLimits();
    void selectionDrags();
    void selectionResize();
    void themesAreTransactional();
    void selectionDimmingThemeTokens();
    void exportIsUnique();
    void exportFilenameTemplates();
    void filenameDateTimeFormats();
    void exportFailures();
    void parseHyprland();
    void parseNiri();
    void invalidOutputs();
};

static Frame solid(Output output, QColor color)
{
    QImage image(QSize(qRound(output.logical.width() * output.scale),
                       qRound(output.logical.height() * output.scale)), QImage::Format_ARGB32);
    image.fill(color);
    return {output, image};
}

void CoreTest::mixedScale()
{
    const QVector<Frame> frames{solid({"left", {-100, 0, 100, 100}, 1}, Qt::red),
                                solid({"right", {0, 0, 100, 100}, 2}, Qt::blue)};
    const auto result = compose(frames, {-50, 10, 100, 50});
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.scale, 2);
    QCOMPARE(result.image.size(), QSize(200, 100));
    QCOMPARE(result.image.pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(result.image.pixelColor(99, 99), QColor(Qt::red));
    QCOMPARE(result.image.pixelColor(100, 0), QColor(Qt::blue));
    QCOMPARE(result.image.pixelColor(199, 99), QColor(Qt::blue));
    // A high-density output outside the selection must not influence density.
    QCOMPARE(compose(frames, {-100, 0, 50, 50}).scale, 1);
    QCOMPARE(compose({solid({"small", {0, 0, 100, 100}, 0.5}, Qt::red)},
                     {0, 0, 100, 100}).image.size(), QSize(50, 50));
}

void CoreTest::negativeAndRotated()
{
    // Backends deliver display-oriented pixels; composition must not rotate twice.
    Frame frame = solid({"portrait", {-40, -80, 40, 80}, 1.5, 1}, Qt::green);
    frame.image.setPixelColor(0, 0, Qt::red);
    const auto result = compose({frame}, frame.output.logical);
    QCOMPARE(result.image.size(), QSize(60, 120));
    QCOMPARE(result.image.pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(result.image.pixelColor(59, 119), QColor(Qt::green));
}

void CoreTest::gapsAndLimits()
{
    const QVector<Frame> frames{solid({"a", {0, 0, 10, 10}, 1}, Qt::red),
                                solid({"b", {20, 0, 10, 10}, 1}, Qt::blue)};
    auto result = compose(frames, {0, 0, 30, 10});
    QCOMPARE(result.image.pixelColor(15, 5).alpha(), 0);
    QVERIFY(!compose(frames, {12, 0, 5, 5}).error.isEmpty());
    QVERIFY(!compose(frames, {}).error.isEmpty());
    QVERIFY(!compose(frames, {0, 0, 1e10, 100}).error.isEmpty());
    QVERIFY(!compose(frames, {0, 0, std::numeric_limits<double>::infinity(), 1}).error.isEmpty());
}

void CoreTest::selectionDrags()
{
    Selection model;
    model.setBounds({-100, -50, 200, 150});
    model.begin(50, 50);
    model.update(-50, -20);
    model.end();
    QCOMPARE(model.rect(), QRectF(-50, -20, 100, 70));
    model.begin(0, 10); // Move, clamped at desktop edge.
    model.update(300, 300);
    model.end();
    QCOMPARE(model.rect(), QRectF(0, 30, 100, 70));
    model.clear();
    QVERIFY(!model.valid());
}

void CoreTest::selectionResize()
{
    Selection model;
    model.setBounds({0, 0, 200, 200});
    model.setRect({20, 20, 80, 80});
    model.begin(20, 20);
    model.update(10, 5);
    model.end();
    QCOMPARE(model.rect(), QRectF(10, 5, 90, 95));
    model.begin(100, 100);
    model.update(180, 170);
    model.end();
    QCOMPARE(model.rect(), QRectF(10, 5, 170, 165));
}

void CoreTest::themesAreTransactional()
{
    Theme theme;
    QString error;
    QVERIFY(theme.load(R"({"accent":"#ff0000","spacing":12})", &error));
    const auto valid = theme.tokens();
    for (const QByteArray bad : {QByteArray(R"({"accent":"bad-color"})"),
                                QByteArray(R"({"spacing":-1})"),
                                QByteArray(R"({"dimOpacity":2})"),
                                QByteArray(R"({"fontSize":"16"})"),
                                QByteArray(R"({"command":"rm -rf ~"})"),
                                QByteArray("[]"), QByteArray("{")}) {
        QVERIFY(!theme.load(bad, &error));
        QCOMPARE(theme.tokens(), valid);
    }
    QVERIFY(!theme.load(QByteArray(65537, ' '), &error));
    QCOMPARE(theme.tokens(), valid);
}

void CoreTest::selectionDimmingThemeTokens()
{
    Theme theme;
    QString error;
    QVERIFY(theme.load(R"({"windowDimColor":"#334455","windowDimOpacity":0.2,"monitorDimOpacity":0.3})", &error));
    const auto valid = theme.tokens();
    QCOMPARE(valid.value("windowDimOpacity").toDouble(), 0.2);
    QCOMPARE(valid.value("monitorDimOpacity").toDouble(), 0.3);
    for (const auto bad : {R"({"windowDimOpacity":1.01})", R"({"monitorDimOpacity":-0.1})",
                           R"({"windowDimColor":"invalid"})"}) {
        QVERIFY(!theme.load(bad, &error));
        QCOMPARE(theme.tokens(), valid);
    }
}

void CoreTest::exportIsUnique()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto frame = solid({"a", {0, 0, 4, 4}, 1}, Qt::red);
    const auto first = saveImage(frame.image, directory.path());
    const auto second = saveImage(frame.image, directory.path());
    QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
    QVERIFY(second.error.isEmpty());
    QVERIFY(first.path != second.path);
    QCOMPARE(QImage(first.path), frame.image);
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files).size(), 2);
    QVERIFY(!(QFile::permissions(first.path) & QFileDevice::ReadOther));
    QVERIFY(!(QFile::permissions(first.path) & QFileDevice::WriteOther));
}

void CoreTest::exportFailures()
{
    QTemporaryDir directory;
    auto image = solid({"a", {0, 0, 2, 2}, 1}, Qt::blue).image;
    QVERIFY(!saveImage({}, directory.path()).error.isEmpty());
    QVERIFY(!saveImage(image, directory.path(), "svg").error.isEmpty());
    QFile obstruction(directory.filePath("not-a-directory"));
    QVERIFY(obstruction.open(QIODevice::WriteOnly));
    obstruction.close();
    QVERIFY(!saveImage(image, obstruction.fileName() + "/child").error.isEmpty());
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files).size(), 1);
}

void CoreTest::exportFilenameTemplates()
{
    QTemporaryDir directory;
    const auto image = solid({"a", {0, 0, 2, 2}, 1}, Qt::red).image;
    const auto first = saveImage(image, directory.path(), "png", "My screenshot");
    QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
    QCOMPARE(first.path, directory.filePath("My screenshot.png"));
    const auto second = saveImage(image, directory.path(), "png", "My screenshot");
    QVERIFY(second.error.isEmpty());
    QCOMPARE(second.path, directory.filePath("My screenshot_1.png"));
    QCOMPARE(QImage(first.path), image);
    const auto expanded = saveImage(image, directory.path(), "png",
                                   "{date}_{time}_{timestamp}_{uuid}");
    QVERIFY2(expanded.error.isEmpty(), qPrintable(expanded.error));
    const QRegularExpression pattern(
        R"(^\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}-\d{3}_[0-9a-f]{32}\.png$)");
    QVERIFY(pattern.match(QFileInfo(expanded.path).fileName()).hasMatch());
    for (const auto &bad : QStringList{"", " ", ".", "..", "../outside", "/absolute",
             "sub\\file", "line\nbreak", "{unknown}", "unclosed{", QString(221, 'a')}) {
        QVERIFY(!saveImage(image, directory.path(), "png", bad).error.isEmpty());
    }
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files).size(), 3);
}

void CoreTest::filenameDateTimeFormats()
{
    const auto now = QDateTime::fromString("2026-11-20T14:05:09.007+05:30", Qt::ISODateWithMs);
    QVERIFY(now.isValid());
    QString error;
    for (const auto &pattern : QStringList{
             "yyyyMMdd_HHmmss_zzz", "d dd ddd dddd M MM MMM MMMM yy yyyy",
             "h hh H HH m mm s ss z zzz AP ap aP Ap", "t tt ttt tttt",
             "yyyy-MM-dd'T'HH:mm:ss.zzzttt", "'Taken on' dddd 'at' hh:mm AP",
             "'o''clock' HH", "'{literal}' yyyy", "yyyy'}'MM'{'dd",
             "'{uuid}' yyyy", "''yyyy''", "'{{}}' yyyy"}) {
        QCOMPARE(formatFilenameTemplate("Shot_{datetime:" + pattern + "}", now, &error),
                 "Shot_" + now.toString(pattern));
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    QCOMPARE(formatFilenameTemplate("{datetime:yyyy}_{datetime:MM}_{datetime:dd}", now),
             QString("2026_11_20"));
    QCOMPARE(formatFilenameTemplate("{{shot}}_{datetime:yyyy}", now), QString("{shot}_2026"));
    QCOMPARE(formatFilenameTemplate("{date}_{time}_{timestamp}", now),
             QString("2026-11-20_14-05-09_2026-11-20_14-05-09-007"));
    for (const auto &bad : QStringList{"{datetime:}", "{datetime:yyyy",
             "{datetime:'unclosed}", "{datetime:yyyy/MM/dd}", "{datetime:'../'yyyy}",
             "{datetime:'\n'yyyy}", "{datetime:'" + QString(221, 'x') + "'}",
             "unmatched}", "{other:yyyy}"}) {
        QVERIFY(formatFilenameTemplate(bad, now, &error).isEmpty());
        QVERIFY2(!error.isEmpty(), qPrintable(bad));
    }
}

void CoreTest::parseHyprland()
{
    QString error;
    const auto outputs = parseOutputs(Compositor::Hyprland, R"([
      {"name":"DP-1","x":-1080,"y":-40,"width":2560,"height":1440,"scale":1.3333333333,"transform":0},
      {"name":"DP-2","x":840,"y":0,"width":1920,"height":1080,"scale":1,"transform":1}
    ])", &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(outputs.size(), 2);
    QCOMPARE(outputs[0].logical.topLeft(), QPointF(-1080, -40));
    QCOMPARE(outputs[1].logical.size(), QSizeF(1080, 1920));
}

void CoreTest::parseNiri()
{
    QString error;
    const auto outputs = parseOutputs(Compositor::Niri, R"({
      "DP-1":{"logical":{"x":-100,"y":0,"width":100,"height":200,"scale":1.5,"transform":"90"}},
      "HDMI-A-1":{"logical":null}
    })", &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(outputs.size(), 1);
    QCOMPARE(outputs[0].logical, QRectF(-100, 0, 100, 200));
    QCOMPARE(outputs[0].scale, 1.5);
}

void CoreTest::invalidOutputs()
{
    QString error;
    for (const QByteArray bytes : {QByteArray("{}"), QByteArray("[]"),
                                  QByteArray(R"([{"name":"a","scale":0}])"),
                                  QByteArray("invalid")}) {
        QVERIFY(parseOutputs(Compositor::Hyprland, bytes, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
}

QTEST_GUILESS_MAIN(CoreTest)
#include "core_test.moc"
