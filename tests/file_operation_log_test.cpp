#include "pros/infrastructure/file_operation_log.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

namespace {

QByteArray digest(const QByteArray &contents) {
  return QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex();
}

bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size() &&
         file.flush();
}

QByteArray readFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return file.readAll();
}

QString scalarText(const QString &databasePath, const char *sql) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(databasePath.toUtf8().constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    return {};
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    return {};
  }
  QString result;
  if (sqlite3_step(statement) == SQLITE_ROW) {
    const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
    if (value != nullptr)
      result = QString::fromUtf8(value);
  }
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return result;
}

QString initializedDatabase(QTemporaryDir &directory) {
  const QString databasePath = directory.path() + "/operations.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  return migrator.migrate(databasePath, &error) ? databasePath : QString();
}

} // namespace

class FileOperationLogTest final : public QObject {
  Q_OBJECT

private slots:
  void replacesOnlyMatchingBaselineAndWritesCompletionMarker();
  void recoversAfterTemporaryWriteInterruption_FI_V01_01_V01_REC_02();
  void marksUnknownInterruptedResultForManualIntervention();
};

void FileOperationLogTest::replacesOnlyMatchingBaselineAndWritesCompletionMarker() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const QString target = directory.path() + "/entry.md";
  QVERIFY(writeFile(target, "before"));

  pros::infrastructure::FileOperationLog log(databasePath);
  const auto completed = log.replaceIfUnchanged(target, digest("before"), "after");
  QVERIFY(completed.isSucceeded());
  QVERIFY(!completed.operationId.isEmpty());
  QCOMPARE(readFile(target), QByteArray("after"));
  QCOMPARE(scalarText(databasePath, "SELECT state FROM file_operation_log;"), QString("completed"));
  QCOMPARE(scalarText(databasePath, "SELECT failure_code FROM file_operation_log;"), QString());

  const auto conflict = log.replaceIfUnchanged(target, digest("before"), "unexpected");
  QCOMPARE(conflict.code, pros::infrastructure::FileOperationCode::baseline_conflict);
  QCOMPARE(readFile(target), QByteArray("after"));
  QCOMPARE(scalarText(databasePath, "SELECT failure_code FROM file_operation_log ORDER BY rowid DESC LIMIT 1;"),
           QString("baseline_conflict"));
}

void FileOperationLogTest::recoversAfterTemporaryWriteInterruption_FI_V01_01_V01_REC_02() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const QString target = directory.path() + "/entry.md";
  QVERIFY(writeFile(target, "before"));

  pros::infrastructure::FileOperationLog interrupted(databasePath,
                                                     pros::infrastructure::FileOperationFault::after_temporary_written);
  const auto interruptedResult = interrupted.replaceIfUnchanged(target, digest("before"), "after");
  QCOMPARE(interruptedResult.code, pros::infrastructure::FileOperationCode::recovery_required);
  QCOMPARE(readFile(target), QByteArray("before"));
  QCOMPARE(scalarText(databasePath, "SELECT state FROM file_operation_log;"), QString("temporary_written"));

  pros::infrastructure::FileOperationLog recovered(databasePath);
  const auto report = recovered.recoverPending();
  QVERIFY(report.isSucceeded());
  QCOMPARE(report.recoveredCount, 1);
  QCOMPARE(readFile(target), QByteArray("after"));
  QCOMPARE(scalarText(databasePath, "SELECT state FROM file_operation_log;"), QString("completed"));
}

void FileOperationLogTest::marksUnknownInterruptedResultForManualIntervention() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const QString target = directory.path() + "/entry.md";
  QVERIFY(writeFile(target, "before"));

  pros::infrastructure::FileOperationLog interrupted(databasePath,
                                                     pros::infrastructure::FileOperationFault::after_temporary_written);
  QVERIFY(interrupted.replaceIfUnchanged(target, digest("before"), "after").code ==
          pros::infrastructure::FileOperationCode::recovery_required);
  const QString temporaryPath = scalarText(databasePath, "SELECT temporary_path FROM file_operation_log;");
  QVERIFY(!temporaryPath.isEmpty());
  QVERIFY(QFile::remove(temporaryPath));

  pros::infrastructure::FileOperationLog recovered(databasePath);
  const auto report = recovered.recoverPending();
  QCOMPARE(report.code, pros::infrastructure::FileOperationCode::manual_intervention_required);
  QCOMPARE(report.manualInterventionCount, 1);
  QCOMPARE(readFile(target), QByteArray("before"));
  QCOMPARE(scalarText(databasePath, "SELECT state FROM file_operation_log;"), QString("manual_intervention_required"));
}

QTEST_APPLESS_MAIN(FileOperationLogTest)

#include "file_operation_log_test.moc"
