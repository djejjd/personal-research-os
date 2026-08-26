#include "pros/domain/schema_version.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QTemporaryDir>
#include <QtTest>

class SchemaMigratorTest final : public QObject {
  Q_OBJECT

 private slots:
  void createsCurrentSchemaFromEmptyDatabase();
  void canRunCurrentMigrationMoreThanOnce();
  void rejectsDirectoryAsDatabasePath();
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

QTEST_APPLESS_MAIN(SchemaMigratorTest)

#include "schema_migrator_test.moc"
