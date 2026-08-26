#include "pros/infrastructure/file_operation_log.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

namespace {

using pros::infrastructure::ResourceAccess;
using pros::infrastructure::ResourceRejectCode;
using pros::infrastructure::ResourceResolver;

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
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
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

QString registerWritableRoot(ResourceResolver &resolver, const QString &path) {
  const auto root = resolver.registerRoot(path, ResourceAccess::read_write);
  return root.root.has_value() ? root.root->id : QString();
}

} // namespace

class FileOperationLogTest final : public QObject {
  Q_OBJECT

private slots:
  void replacesOnlyMatchingBaselineAndWritesCompletionMarker();
  void recoversAfterTemporaryWriteInterruption_FI_V01_01_V01_REC_02();
  void rejectsAbsoluteAndEscapingPathsWithoutWritingOutsideRoot();
  void marksRevokedRootInterruptedOperationForManualIntervention();
  void marksReplacedRootInterruptedOperationForManualIntervention();
  void reusesCallerOperationIdOnlyForIdenticalIntent();
};

void FileOperationLogTest::replacesOnlyMatchingBaselineAndWritesCompletionMarker() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  QVERIFY(writeFile(directory.path() + "/entry.md", "before"));
  ResourceResolver resolver;
  const QString rootId = registerWritableRoot(resolver, directory.path());
  QVERIFY(!rootId.isEmpty());

  pros::infrastructure::FileOperationLog log(databasePath);
  const auto completed =
      log.replaceIfUnchanged(resolver, rootId, "entry.md", "operation-complete", digest("before"), "after");
  QVERIFY(completed.isSucceeded());
  QCOMPARE(readFile(directory.path() + "/entry.md"), QByteArray("after"));
  QCOMPARE(scalarText(databasePath, "SELECT state FROM file_operation_log;"), QString("completed"));

  const auto conflict =
      log.replaceIfUnchanged(resolver, rootId, "entry.md", "operation-conflict", digest("before"), "unexpected");
  QCOMPARE(conflict.code, pros::infrastructure::FileOperationCode::baseline_conflict);
  QCOMPARE(readFile(directory.path() + "/entry.md"), QByteArray("after"));
}

void FileOperationLogTest::recoversAfterTemporaryWriteInterruption_FI_V01_01_V01_REC_02() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  QVERIFY(writeFile(directory.path() + "/entry.md", "before"));
  ResourceResolver resolver;
  const QString rootId = registerWritableRoot(resolver, directory.path());
  QVERIFY(!rootId.isEmpty());

  pros::infrastructure::FileOperationLog interrupted(databasePath,
                                                     pros::infrastructure::FileOperationFault::after_temporary_written);
  const auto interruptedResult =
      interrupted.replaceIfUnchanged(resolver, rootId, "entry.md", "operation-recover", digest("before"), "after");
  QCOMPARE(interruptedResult.code, pros::infrastructure::FileOperationCode::recovery_required);
  QCOMPARE(readFile(directory.path() + "/entry.md"), QByteArray("before"));

  pros::infrastructure::FileOperationLog recovered(databasePath);
  const auto report = recovered.recoverPending(resolver);
  QVERIFY(report.isSucceeded());
  QCOMPARE(report.recoveredCount, 1);
  QCOMPARE(readFile(directory.path() + "/entry.md"), QByteArray("after"));
}

void FileOperationLogTest::rejectsAbsoluteAndEscapingPathsWithoutWritingOutsideRoot() {
  QTemporaryDir directory;
  QTemporaryDir outsideDirectory;
  QVERIFY(directory.isValid());
  QVERIFY(outsideDirectory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  QVERIFY(writeFile(directory.path() + "/entry.md", "inside"));
  QVERIFY(writeFile(outsideDirectory.path() + "/outside.md", "outside"));
  ResourceResolver resolver;
  const QString rootId = registerWritableRoot(resolver, directory.path());
  QVERIFY(!rootId.isEmpty());
  pros::infrastructure::FileOperationLog log(databasePath);

  const auto absolute = log.replaceIfUnchanged(resolver, rootId, outsideDirectory.path() + "/outside.md", "absolute",
                                               digest("outside"), "changed");
  QCOMPARE(absolute.code, pros::infrastructure::FileOperationCode::resource_rejected);
  QCOMPARE(absolute.resourceRejection, ResourceRejectCode::invalid_relative_path);
  const auto escape = log.replaceIfUnchanged(resolver, rootId, "../outside.md", "escape", digest("outside"), "changed");
  QCOMPARE(escape.code, pros::infrastructure::FileOperationCode::resource_rejected);
  QCOMPARE(escape.resourceRejection, ResourceRejectCode::path_escape);
  QCOMPARE(readFile(outsideDirectory.path() + "/outside.md"), QByteArray("outside"));
}

void FileOperationLogTest::marksRevokedRootInterruptedOperationForManualIntervention() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  QVERIFY(writeFile(directory.path() + "/entry.md", "before"));
  ResourceResolver resolver;
  const QString rootId = registerWritableRoot(resolver, directory.path());
  QVERIFY(!rootId.isEmpty());
  pros::infrastructure::FileOperationLog interrupted(databasePath,
                                                     pros::infrastructure::FileOperationFault::after_temporary_written);
  QCOMPARE(
      interrupted.replaceIfUnchanged(resolver, rootId, "entry.md", "operation-revoked", digest("before"), "after").code,
      pros::infrastructure::FileOperationCode::recovery_required);
  QVERIFY(resolver.revokeRoot(rootId).isAccepted());

  pros::infrastructure::FileOperationLog recovered(databasePath);
  const auto report = recovered.recoverPending(resolver);
  QCOMPARE(report.code, pros::infrastructure::FileOperationCode::manual_intervention_required);
  QCOMPARE(readFile(directory.path() + "/entry.md"), QByteArray("before"));
  QCOMPARE(recovered.recoverPending(resolver).code,
           pros::infrastructure::FileOperationCode::manual_intervention_required);
}

void FileOperationLogTest::marksReplacedRootInterruptedOperationForManualIntervention() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const QString rootPath = directory.path() + "/root";
  QVERIFY(QDir().mkpath(rootPath));
  QVERIFY(writeFile(rootPath + "/entry.md", "before"));
  ResourceResolver resolver;
  const QString rootId = registerWritableRoot(resolver, rootPath);
  QVERIFY(!rootId.isEmpty());
  pros::infrastructure::FileOperationLog interrupted(databasePath,
                                                     pros::infrastructure::FileOperationFault::after_temporary_written);
  QCOMPARE(interrupted.replaceIfUnchanged(resolver, rootId, "entry.md", "operation-replaced", digest("before"), "after")
               .code,
           pros::infrastructure::FileOperationCode::recovery_required);
  QVERIFY(QDir().rename(rootPath, directory.path() + "/previous-root"));
  QVERIFY(QDir().mkpath(rootPath));
  QVERIFY(writeFile(rootPath + "/entry.md", "replacement"));

  pros::infrastructure::FileOperationLog recovered(databasePath);
  const auto report = recovered.recoverPending(resolver);
  QCOMPARE(report.code, pros::infrastructure::FileOperationCode::manual_intervention_required);
  QCOMPARE(readFile(rootPath + "/entry.md"), QByteArray("replacement"));
}

void FileOperationLogTest::reusesCallerOperationIdOnlyForIdenticalIntent() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  QVERIFY(writeFile(directory.path() + "/entry.md", "before"));
  ResourceResolver resolver;
  const QString rootId = registerWritableRoot(resolver, directory.path());
  QVERIFY(!rootId.isEmpty());
  pros::infrastructure::FileOperationLog log(databasePath);

  const auto first =
      log.replaceIfUnchanged(resolver, rootId, "entry.md", "operation-replay", digest("before"), "after");
  QVERIFY(first.isSucceeded());
  const auto replay =
      log.replaceIfUnchanged(resolver, rootId, "entry.md", "operation-replay", digest("before"), "after");
  QVERIFY(replay.isSucceeded());
  const auto changedContents =
      log.replaceIfUnchanged(resolver, rootId, "entry.md", "operation-replay", digest("before"), "tampered");
  QCOMPARE(changedContents.code, pros::infrastructure::FileOperationCode::operation_id_conflict);
  const auto changedPath =
      log.replaceIfUnchanged(resolver, rootId, "other.md", "operation-replay", digest("before"), "after");
  QCOMPARE(changedPath.code, pros::infrastructure::FileOperationCode::operation_id_conflict);
  QCOMPARE(readFile(directory.path() + "/entry.md"), QByteArray("after"));
}

QTEST_APPLESS_MAIN(FileOperationLogTest)

#include "file_operation_log_test.moc"
