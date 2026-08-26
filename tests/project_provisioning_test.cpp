#include "pros/infrastructure/project_provisioning.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {

using pros::infrastructure::ProjectProvisioningCode;
using pros::infrastructure::ProjectProvisioningFault;
using pros::infrastructure::ProjectProvisioningRequest;
using pros::infrastructure::ProjectProvisioningSaga;
using pros::infrastructure::ProjectProvisioningState;
using pros::infrastructure::SchemaMigrator;

QString initializedDatabase(QTemporaryDir &directory) {
  QString error;
  const QString path = directory.filePath("project-provisioning.sqlite");
  return SchemaMigrator().migrate(path, &error) ? path : QString();
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

ProjectProvisioningRequest request(const QString &operationId = "provision-1") {
  return {operationId, "project-1", "研究项目", "project.md"};
}

} // namespace

class ProjectProvisioningTest final : public QObject {
  Q_OBJECT

private slots:
  void resumesInterruptedCreationWithSameOperationId_FI_V01_PROJ_01();
  void retainsPreexistingAssetOnCollision_FI_V01_PROJ_01();
  void retainsUserModifiedAssetAndRequiresManualIntervention_FI_V01_PROJ_01();
  void abandonsOnlyUnchangedOperationCreatedAsset_FI_V01_PROJ_01();
  void repeatsReadyOperationWithoutSecondAsset_FI_V01_PROJ_01();
};

void ProjectProvisioningTest::resumesInterruptedCreationWithSameOperationId_FI_V01_PROJ_01() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const ProjectProvisioningRequest operation = request();
  ProjectProvisioningSaga interrupted(databasePath, directory.path(), ProjectProvisioningFault::after_asset_recorded);

  QCOMPARE(interrupted.provision(operation).code, ProjectProvisioningCode::recovery_required);
  QVERIFY(QFile::exists(directory.filePath("project.md")));
  const ProjectProvisioningSaga recovered(databasePath, directory.path());
  const auto completed = recovered.provision(operation);
  QVERIFY(completed.isSucceeded());
  QCOMPARE(completed.state, ProjectProvisioningState::ready);
  const auto snapshot = recovered.query(operation.operationId);
  QCOMPARE(snapshot.code, ProjectProvisioningCode::none);
  QVERIFY(snapshot.value.has_value());
  QCOMPARE(snapshot.value.value_or({}).state, ProjectProvisioningState::ready);
}

void ProjectProvisioningTest::retainsPreexistingAssetOnCollision_FI_V01_PROJ_01() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const QString assetPath = directory.filePath("project.md");
  QVERIFY(writeFile(assetPath, "already exists"));

  const ProjectProvisioningSaga saga(databasePath, directory.path());
  QCOMPARE(saga.provision(request()).code, ProjectProvisioningCode::asset_collision);
  QCOMPARE(readFile(assetPath), QByteArray("already exists"));
  QCOMPARE(saga.abandon("provision-1").code, ProjectProvisioningCode::manual_intervention_required);
  QCOMPARE(readFile(assetPath), QByteArray("already exists"));
  const auto snapshot = saga.query("provision-1");
  QVERIFY(snapshot.value.has_value());
  QCOMPARE(snapshot.value.value_or({}).state, ProjectProvisioningState::failed);
}

void ProjectProvisioningTest::retainsUserModifiedAssetAndRequiresManualIntervention_FI_V01_PROJ_01() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const ProjectProvisioningRequest operation = request();
  ProjectProvisioningSaga interrupted(databasePath, directory.path(), ProjectProvisioningFault::after_asset_recorded);
  QCOMPARE(interrupted.provision(operation).code, ProjectProvisioningCode::recovery_required);
  const QString assetPath = directory.filePath("project.md");
  QVERIFY(writeFile(assetPath, "user content"));

  const ProjectProvisioningSaga recovered(databasePath, directory.path());
  QCOMPARE(recovered.provision(operation).code, ProjectProvisioningCode::manual_intervention_required);
  QCOMPARE(recovered.abandon(operation.operationId).code, ProjectProvisioningCode::manual_intervention_required);
  QCOMPARE(readFile(assetPath), QByteArray("user content"));
}

void ProjectProvisioningTest::abandonsOnlyUnchangedOperationCreatedAsset_FI_V01_PROJ_01() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const ProjectProvisioningRequest operation = request();
  ProjectProvisioningSaga interrupted(databasePath, directory.path(), ProjectProvisioningFault::after_asset_recorded);
  QCOMPARE(interrupted.provision(operation).code, ProjectProvisioningCode::recovery_required);

  const ProjectProvisioningSaga saga(databasePath, directory.path());
  QVERIFY(saga.abandon(operation.operationId).isSucceeded());
  QVERIFY(!QFile::exists(directory.filePath("project.md")));
  const auto snapshot = saga.query(operation.operationId);
  QVERIFY(snapshot.value.has_value());
  QCOMPARE(snapshot.value.value_or({}).state, ProjectProvisioningState::failed);
  QCOMPARE(snapshot.value.value_or({}).failureCode, QString("safe_abandoned"));
}

void ProjectProvisioningTest::repeatsReadyOperationWithoutSecondAsset_FI_V01_PROJ_01() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const ProjectProvisioningSaga saga(databasePath, directory.path());
  const ProjectProvisioningRequest operation = request();

  QVERIFY(saga.provision(operation).isSucceeded());
  const QByteArray firstContents = readFile(directory.filePath("project.md"));
  QVERIFY(saga.provision(operation).isSucceeded());
  QCOMPARE(readFile(directory.filePath("project.md")), firstContents);
  QCOMPARE(saga.provision({"provision-1", "project-1", "研究项目", "another.md"}).code,
           ProjectProvisioningCode::invalid_argument);
  QVERIFY(!QFile::exists(directory.filePath("another.md")));
}

QTEST_APPLESS_MAIN(ProjectProvisioningTest)

#include "project_provisioning_test.moc"
