#include "core/editorcommand.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace snip;
class EditorTest : public QObject {
  Q_OBJECT
  QByteArray read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
  }
  void write(const QString &path, const QByteArray &bytes, bool executable = false) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
    if (executable)
      QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
  }
private slots:
  void substitutesAfterParsingWithoutAShell() {
    QTemporaryDir dir;
    const auto editor = dir.filePath("editor with spaces");
    const auto input = dir.filePath("image with spaces;$(touch injected)'\".png");
    const QByteArray bytes("original screenshot");
    write(input, bytes);
    write(editor, "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$0.args\"\n", true);
    QString error;
    QVERIFY2(launchEditor("\"" + editor + "\" --input={} \"two words\" {}", input, &error), qPrintable(error));
    const auto expected = "--input=" + input.toUtf8() + "\ntwo words\n" + input.toUtf8() + "\n";
    QTRY_COMPARE(read(editor + ".args"), expected);
    QCOMPARE(read(input), bytes);
    QVERIFY(!QFile::exists(dir.filePath("injected")));
  }
  void noPlaceholderConnectsFileContentsToStdin() {
    QTemporaryDir dir;
    const auto editor = dir.filePath("stdin-editor");
    const auto input = dir.filePath("recording.mp4");
    QByteArray bytes(300000, '\0');
    for (qsizetype i = 0; i < bytes.size(); ++i) bytes[i] = char(i % 251);
    write(input, bytes);
    write(editor, "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$0.args\"\ncat > \"$0.stdin\"\n", true);
    QString error;
    QVERIFY2(launchEditor(editor + " --stdin", input, &error), qPrintable(error));
    QTRY_COMPARE(read(editor + ".stdin"), bytes);
    QCOMPARE(read(editor + ".args"), QByteArray("--stdin\n")); // No filename appended.
    QCOMPARE(read(input), bytes);
  }
  void validatesAndReportsStartupErrors() {
    QString error;
    QVERIFY(validateEditorCommand({}, &error));
    QVERIFY(validateEditorCommand("losslesscut {}", &error));
    QVERIFY(validateEditorCommand("satty --filename -", &error));
    QVERIFY(!validateEditorCommand("{} --bad", &error));
    QVERIFY(!validateEditorCommand("editor\n--bad", &error));
    QVERIFY(!validateEditorCommand(QString(4097, 'x'), &error));
    QTemporaryDir dir;
    const auto input = dir.filePath("saved.png");
    write(input, "saved");
    QVERIFY(!launchEditor({}, input, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!launchEditor(dir.filePath("missing-editor") + " {}", input, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!launchEditor("editor {}", dir.filePath("missing.png"), &error));
    QCOMPARE(read(input), QByteArray("saved"));
  }
};
QTEST_GUILESS_MAIN(EditorTest)
#include "editor_test.moc"
