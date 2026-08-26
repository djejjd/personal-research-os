#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

class AppLogContractTest final : public QObject {
  Q_OBJECT

private slots:
  void emitsStructuredStartupEventsWithoutDataDirectory();
};

void AppLogContractTest::emitsStructuredStartupEventsWithoutDataDirectory() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  const QString sensitiveDirectory = temporaryDirectory.path() + "/token_sk_example_sensitive_note_body";

  QProcess process;
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert("QT_QPA_PLATFORM", "offscreen");
  process.setProcessEnvironment(environment);
  process.setProgram(QString::fromUtf8(PROS_APP_EXECUTABLE));
  process.setArguments({"--data-dir", sensitiveDirectory, "--smoke-test"});
  process.start();
  QVERIFY(process.waitForFinished(15000));
  QCOMPARE(process.exitStatus(), QProcess::NormalExit);
  QCOMPARE(process.exitCode(), 0);

  const QByteArray output = process.readAllStandardOutput() + process.readAllStandardError();
  QVERIFY(!output.contains(sensitiveDirectory.toUtf8()));
  QVERIFY(!output.contains("sk_example_sensitive_note_body"));

  quint64 previousSequence = 0;
  int eventCount = 0;
  for (const QByteArray &line : output.split('\n')) {
    if (line.isEmpty()) {
      continue;
    }
    QJsonParseError parseError;
    const QJsonDocument event = QJsonDocument::fromJson(line, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(event.isObject());

    const QJsonObject fields = event.object();
    for (const char *requiredField : {"timestamp", "level", "event_code", "message", "module", "result", "app_version",
                                      "schema_version", "process_instance_id", "sequence"}) {
      QVERIFY2(fields.contains(QLatin1String(requiredField)), requiredField);
    }
    const quint64 sequence = fields.value("sequence").toVariant().toULongLong();
    QVERIFY(sequence > previousSequence);
    previousSequence = sequence;
    ++eventCount;
  }
  QCOMPARE(eventCount, 9);
}

QTEST_APPLESS_MAIN(AppLogContractTest)

#include "app_log_contract_test.moc"
