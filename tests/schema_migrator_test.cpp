#include "pros/domain/schema_version.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>
#include <sqlite3.h>

namespace {
void createMetadata(const QString &path, const QString &values) {
  sqlite3 *database = nullptr;
  QVERIFY(sqlite3_open(path.toUtf8().constData(), &database) == SQLITE_OK);
  QVERIFY(sqlite3_exec(database, "CREATE TABLE schema_metadata (schema_version INTEGER NOT NULL);", nullptr, nullptr,
                       nullptr) == SQLITE_OK);
  const QString sql = "INSERT INTO schema_metadata (schema_version) VALUES " + values + ";";
  QVERIFY(sqlite3_exec(database, sql.toUtf8().constData(), nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(database);
}

bool tableExists(const QString &path, const char *name) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    return false;
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", -1, &statement,
                         nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    return false;
  }
  sqlite3_bind_text(statement, 1, name, -1, SQLITE_STATIC);
  const bool exists = sqlite3_step(statement) == SQLITE_ROW;
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return exists;
}

QString tableSql(const QString &path, const char *name) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    return {};
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, "SELECT sql FROM sqlite_master WHERE type='table' AND name=?;", -1, &statement,
                         nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    return {};
  }
  sqlite3_bind_text(statement, 1, name, -1, SQLITE_STATIC);
  QString result;
  if (sqlite3_step(statement) == SQLITE_ROW)
    result = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)));
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return result;
}

bool executeSql(const QString &path, const QString &sql) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
    return false;
  const bool succeeded = sqlite3_exec(database, sql.toUtf8().constData(), nullptr, nullptr, nullptr) == SQLITE_OK;
  sqlite3_close(database);
  return succeeded;
}
} // namespace

class SchemaMigratorTest final : public QObject {
  Q_OBJECT
private slots:
  void createsCompleteSchemaFromEmptyDatabase();
  void canRunCurrentMigrationMoreThanOnce();
  void upgradesV1DatabaseToCurrentSchema();
  void rejectsDirectoryAsDatabasePath();
  void rejectsUnsupportedOrAmbiguousMetadata();
  void rejectsDamagedV1SchemaWithoutPromotingVersion();
  void rejectsDamagedCurrentSchema();
  void rejectsDamagedFileOperationLogInCurrentSchema();
  void rejectsChangedCheckLiteralCaseFromV1();
  void rejectsChangedCheckLiteralCaseInCurrentSchema();
  void rejectsMissingAllocatorWhenEventsExistFromV1();
  void rejectsMissingAllocatorWhenEventsExistInCurrentSchema();
  void rejectsAllocatorBehindEventsInCurrentSchema();
  void rejectsGappedDeliveryPositionsFromV1();
  void rejectsGappedDeliveryPositionsInCurrentSchema();
  void bindsApprovalToExactPlanRevisionAndDigest();
  void rejectsAmbiguousMetadataWhenReadingVersion();
};

void SchemaMigratorTest::createsCompleteSchemaFromEmptyDatabase() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/empty.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  QCOMPARE(migrator.schemaVersion(path, &error), pros::domain::kCurrentSchemaVersion);
  for (const char *table :
       {"operation_records", "delivery_sequence", "domain_events", "outbox_records", "activity_facts", "projects",
        "tasks", "milestones", "directions", "governance_targets", "governance_note_links", "governance_evidence",
        "governance_acceptance", "governance_acceptance_evidence", "operation_plans", "approvals", "file_operation_log",
        "project_provisioning_operations"})
    QVERIFY(tableExists(path, table));
  for (const char *table : {"document_registry", "watcher_event_queue", "reconcile_operations", "reconcile_health"})
    QVERIFY(tableExists(path, table));
}

void SchemaMigratorTest::canRunCurrentMigrationMoreThanOnce() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/repeated.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  QCOMPARE(migrator.schemaVersion(path, &error), 4);
}

void SchemaMigratorTest::upgradesV1DatabaseToCurrentSchema() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/v1.sqlite";
  createMetadata(path, "(1)");
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  QCOMPARE(migrator.schemaVersion(path, &error), 4);
  QVERIFY(tableExists(path, "operation_records"));
  QVERIFY(tableExists(path, "approvals"));
}

void SchemaMigratorTest::rejectsDirectoryAsDatabasePath() {
  QTemporaryDir directory;
  const QString sensitive = directory.path() + "/token_sk_example_sensitive_note_body";
  QVERIFY(QDir().mkpath(sensitive));
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY(!migrator.migrate(sensitive, &error));
  QVERIFY(!error.isEmpty());
  QVERIFY(!error.contains(sensitive));
  QVERIFY(!error.contains("sk_example_sensitive_note_body"));
}

void SchemaMigratorTest::rejectsUnsupportedOrAmbiguousMetadata() {
  QTemporaryDir directory;
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  const QString future = directory.path() + "/future.sqlite";
  createMetadata(future, "(5)");
  QVERIFY(!migrator.migrate(future, &error));
  const QString duplicate = directory.path() + "/duplicate.sqlite";
  createMetadata(duplicate, "(1), (1)");
  QVERIFY(!migrator.migrate(duplicate, &error));
}

void SchemaMigratorTest::rejectsDamagedV1SchemaWithoutPromotingVersion() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/damaged-v1.sqlite";
  createMetadata(path, "(1)");
  sqlite3 *database = nullptr;
  QVERIFY(sqlite3_open(path.toUtf8().constData(), &database) == SQLITE_OK);
  QVERIFY(sqlite3_exec(database,
                       "CREATE TABLE projects (id TEXT PRIMARY KEY, title TEXT NOT NULL, status INTEGER NOT NULL "
                       "CHECK(status IN (0, 1, 2)), revision INTEGER NOT NULL CHECK(revision >= 0));",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(database);
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY(!migrator.migrate(path, &error));
  QCOMPARE(migrator.schemaVersion(path, &error), 1);
  QVERIFY(!tableExists(path, "tasks"));
}

void SchemaMigratorTest::rejectsDamagedCurrentSchema() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/damaged-v2.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  sqlite3 *database = nullptr;
  QVERIFY(sqlite3_open(path.toUtf8().constData(), &database) == SQLITE_OK);
  QVERIFY(sqlite3_exec(database,
                       "PRAGMA foreign_keys=OFF; DROP TABLE approvals; CREATE TABLE approvals (id TEXT PRIMARY KEY, "
                       "plan_id TEXT NOT NULL, plan_revision INTEGER NOT NULL CHECK(plan_revision >= 0), plan_digest "
                       "TEXT NOT NULL CHECK(length(plan_digest) > 0), decision INTEGER NOT NULL CHECK(decision IN "
                       "(0, 1, 2, 3)), note TEXT NOT NULL, revision INTEGER NOT NULL CHECK(revision >= 0), FOREIGN KEY"
                       "(plan_id, plan_revision, plan_digest) REFERENCES operation_plans(id, revision, plan_digest));",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(database);
  QVERIFY(!migrator.migrate(path, &error));
  QCOMPARE(migrator.schemaVersion(path, &error), 4);
  QVERIFY(tableExists(path, "domain_events"));
}

void SchemaMigratorTest::rejectsDamagedFileOperationLogInCurrentSchema() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/damaged-file-operation-log.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  QVERIFY(executeSql(
      path, "DROP TABLE file_operation_log; CREATE TABLE file_operation_log (operation_id TEXT PRIMARY KEY);"));
  QVERIFY(!migrator.migrate(path, &error));
  QCOMPARE(migrator.schemaVersion(path, &error), 4);
}

void SchemaMigratorTest::rejectsChangedCheckLiteralCaseFromV1() {
  QTemporaryDir directory;
  const QString canonicalPath = directory.path() + "/canonical.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(canonicalPath, &error), qPrintable(error));
  QString changedSql = tableSql(canonicalPath, "domain_events");
  QVERIFY(changedSql.replace("'global'", "'GLOBAL'") != tableSql(canonicalPath, "domain_events"));

  const QString path = directory.path() + "/changed-v1.sqlite";
  createMetadata(path, "(1)");
  QVERIFY(executeSql(path, changedSql));
  QVERIFY(!migrator.migrate(path, &error));
  QCOMPARE(migrator.schemaVersion(path, &error), 1);
  QVERIFY(!tableExists(path, "outbox_records"));
}

void SchemaMigratorTest::rejectsChangedCheckLiteralCaseInCurrentSchema() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/changed-v2.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  QString changedSql = tableSql(path, "domain_events");
  changedSql.replace("'global'", "'GLOBAL'");
  QVERIFY(executeSql(path, "PRAGMA foreign_keys=OFF; DROP TABLE domain_events; " + changedSql + ";"));
  QVERIFY(!migrator.migrate(path, &error));
  QCOMPARE(migrator.schemaVersion(path, &error), 4);
}

void SchemaMigratorTest::rejectsMissingAllocatorWhenEventsExistFromV1() {
  QTemporaryDir directory;
  const QString canonicalPath = directory.path() + "/canonical.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(canonicalPath, &error), qPrintable(error));
  const QString path = directory.path() + "/allocator-v1.sqlite";
  createMetadata(path, "(1)");
  QVERIFY(executeSql(path, tableSql(canonicalPath, "domain_events")));
  QVERIFY(executeSql(path, "INSERT INTO domain_events VALUES "
                           "('event-1','global',7,'test','aggregate','id',1,0,1,'caller','operation','{}');"));
  QVERIFY(!migrator.migrate(path, &error));
  QCOMPARE(migrator.schemaVersion(path, &error), 1);
  QVERIFY(!tableExists(path, "delivery_sequence"));
}

void SchemaMigratorTest::rejectsMissingAllocatorWhenEventsExistInCurrentSchema() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/allocator-v2.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  QVERIFY(executeSql(path, "INSERT INTO domain_events VALUES "
                           "('event-1','global',1,'test','aggregate','id',1,0,1,'caller','operation','{}'); "
                           "DELETE FROM delivery_sequence;"));
  QVERIFY(!migrator.migrate(path, &error));
  QCOMPARE(migrator.schemaVersion(path, &error), 4);
  QCOMPARE(tableSql(path, "delivery_sequence").isEmpty(), false);
}

void SchemaMigratorTest::rejectsAllocatorBehindEventsInCurrentSchema() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/allocator-behind-v2.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  QVERIFY(executeSql(path, "INSERT INTO domain_events VALUES "
                           "('event-1','global',1,'test','aggregate','id',1,0,1,'caller','operation','{}');"));
  QVERIFY(!migrator.migrate(path, &error));
  QCOMPARE(migrator.schemaVersion(path, &error), 4);
}

void SchemaMigratorTest::rejectsGappedDeliveryPositionsFromV1() {
  QTemporaryDir directory;
  const QString canonicalPath = directory.path() + "/canonical.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(canonicalPath, &error), qPrintable(error));
  const QString path = directory.path() + "/gapped-v1.sqlite";
  createMetadata(path, "(1)");
  QVERIFY(executeSql(path, tableSql(canonicalPath, "delivery_sequence")));
  QVERIFY(executeSql(path, tableSql(canonicalPath, "domain_events")));
  QVERIFY(executeSql(path, tableSql(canonicalPath, "outbox_records")));
  QVERIFY(executeSql(path, "INSERT INTO delivery_sequence VALUES ('global',4); "
                           "INSERT INTO domain_events VALUES "
                           "('event-1','global',1,'test','aggregate','id-1',1,0,1,'caller','operation-1','{}'),"
                           "('event-3','global',3,'test','aggregate','id-3',1,0,1,'caller','operation-3','{}'); "
                           "INSERT INTO outbox_records VALUES "
                           "('event-1','global',1,'pending',NULL,NULL,NULL),"
                           "('event-3','global',3,'pending',NULL,NULL,NULL);"));
  QVERIFY(!migrator.migrate(path, &error));
  QCOMPARE(migrator.schemaVersion(path, &error), 1);
}

void SchemaMigratorTest::rejectsGappedDeliveryPositionsInCurrentSchema() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/gapped-v2.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  QVERIFY(executeSql(path, "UPDATE delivery_sequence SET next_position=4 WHERE delivery_partition='global'; "
                           "INSERT INTO domain_events VALUES "
                           "('event-1','global',1,'test','aggregate','id-1',1,0,1,'caller','operation-1','{}'),"
                           "('event-3','global',3,'test','aggregate','id-3',1,0,1,'caller','operation-3','{}'); "
                           "INSERT INTO outbox_records VALUES "
                           "('event-1','global',1,'pending',NULL,NULL,NULL),"
                           "('event-3','global',3,'pending',NULL,NULL,NULL);"));
  QVERIFY(!migrator.migrate(path, &error));
  QCOMPARE(migrator.schemaVersion(path, &error), 4);
}

void SchemaMigratorTest::bindsApprovalToExactPlanRevisionAndDigest() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/approval-binding.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  QVERIFY(executeSql(path, "PRAGMA foreign_keys=ON; INSERT INTO operation_plans VALUES "
                           "('plan-1','summary','digest-1',3); INSERT INTO approvals VALUES "
                           "('approval-1','plan-1',3,'digest-1',1,'approved',1);"));
  QVERIFY(!executeSql(path, "PRAGMA foreign_keys=ON; INSERT INTO approvals VALUES "
                            "('approval-2','plan-1',3,'other-digest',1,'approved',1);"));
  QVERIFY(!executeSql(path, "PRAGMA foreign_keys=ON; UPDATE operation_plans SET plan_digest='changed' "
                            "WHERE id='plan-1';"));
}

void SchemaMigratorTest::rejectsAmbiguousMetadataWhenReadingVersion() {
  QTemporaryDir directory;
  const QString path = directory.path() + "/duplicate-read.sqlite";
  createMetadata(path, "(1), (1)");
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QCOMPARE(migrator.schemaVersion(path, &error), -1);
  QVERIFY(!error.isEmpty());
}

QTEST_APPLESS_MAIN(SchemaMigratorTest)
#include "schema_migrator_test.moc"
