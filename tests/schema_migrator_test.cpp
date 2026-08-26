#include "pros/domain/schema_version.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

namespace {
void createMetadata(const QString &path, const QString &values) {
  sqlite3 *database = nullptr;
  QVERIFY(sqlite3_open(path.toUtf8().constData(), &database) == SQLITE_OK);
  QVERIFY(sqlite3_exec(database, "CREATE TABLE schema_metadata (schema_version INTEGER NOT NULL);", nullptr, nullptr,
                       nullptr) == SQLITE_OK);
  const QString statement = "INSERT INTO schema_metadata (schema_version) VALUES " + values + ";";
  QVERIFY(sqlite3_exec(database, statement.toUtf8().constData(), nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(database);
}
} // namespace

class SchemaMigratorTest final : public QObject {
  Q_OBJECT

private slots:
  void createsCurrentSchemaFromEmptyDatabase();
  void canRunCurrentMigrationMoreThanOnce();
  void rejectsDirectoryAsDatabasePath();
  void rejectsUnsupportedOrAmbiguousMetadata();
  void rejectsAmbiguousMetadataWhenReadingVersion();
};

void SchemaMigratorTest::createsCurrentSchemaFromEmptyDatabase() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  const QString databasePath = temporaryDirectory.path() + "/empty.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString errorMessage;

  QVERIFY2(migrator.migrate(databasePath, &errorMessage), qPrintable(errorMessage));
  QCOMPARE(migrator.schemaVersion(databasePath, &errorMessage), pros::domain::kCurrentSchemaVersion);
}

void SchemaMigratorTest::canRunCurrentMigrationMoreThanOnce() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  const QString databasePath = temporaryDirectory.path() + "/repeated.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString errorMessage;

  QVERIFY2(migrator.migrate(databasePath, &errorMessage), qPrintable(errorMessage));
  QVERIFY2(migrator.migrate(databasePath, &errorMessage), qPrintable(errorMessage));
  QCOMPARE(migrator.schemaVersion(databasePath, &errorMessage), pros::domain::kCurrentSchemaVersion);
}

void SchemaMigratorTest::rejectsDirectoryAsDatabasePath() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  pros::infrastructure::SchemaMigrator migrator;
  QString errorMessage;

  QVERIFY(!migrator.migrate(temporaryDirectory.path(), &errorMessage));
  QVERIFY(!errorMessage.isEmpty());
}

void SchemaMigratorTest::rejectsUnsupportedOrAmbiguousMetadata() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  pros::infrastructure::SchemaMigrator migrator;
  QString errorMessage;

  const QString futureDatabase = temporaryDirectory.path() + "/future.sqlite";
  createMetadata(futureDatabase, "(2)");
  QVERIFY(!migrator.migrate(futureDatabase, &errorMessage));

  const QString duplicateDatabase = temporaryDirectory.path() + "/duplicate.sqlite";
  createMetadata(duplicateDatabase, "(1), (1)");
  QVERIFY(!migrator.migrate(duplicateDatabase, &errorMessage));
}

void SchemaMigratorTest::rejectsAmbiguousMetadataWhenReadingVersion() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  const QString databasePath = temporaryDirectory.path() + "/duplicate-read.sqlite";
  createMetadata(databasePath, "(1), (1)");

  pros::infrastructure::SchemaMigrator migrator;
  QString errorMessage;
  QCOMPARE(migrator.schemaVersion(databasePath, &errorMessage), -1);
  QVERIFY(!errorMessage.isEmpty());
}

QTEST_APPLESS_MAIN(SchemaMigratorTest)

#include "schema_migrator_test.moc"
