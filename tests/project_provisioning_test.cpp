#include "pros/infrastructure/project_provisioning.h"
#include "pros/infrastructure/schema_migrator.h"
#include "pros/infrastructure/sqlite_work_query_service.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <barrier>
#include <thread>

namespace {

using pros::infrastructure::ProjectProvisioningCode;
using pros::infrastructure::ProjectProvisioningFault;
using pros::infrastructure::ProjectProvisioningRecoveryCoordinator;
using pros::infrastructure::ProjectProvisioningRequest;
using pros::infrastructure::ProjectProvisioningSaga;
using pros::infrastructure::ProjectProvisioningState;
using pros::infrastructure::ResourceAccess;
using pros::infrastructure::ResourceResolver;
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

QString registerWritableRoot(ResourceResolver &resolver, const QString &path) {
  const auto root = resolver.registerRoot(path, ResourceAccess::read_write);
  if (!root.isAccepted() || !root.root.has_value())
    return {};
  return root.root->id;
}

bool projectIsInvisible(const QString &databasePath, const std::string &projectId) {
  QString error;
  return pros::infrastructure::SqliteWorkQueryService(databasePath).project(projectId, &error).status() ==
         pros::application::WorkQueryStatus::not_found;
}

} // namespace

class ProjectProvisioningTest final : public QObject {
  Q_OBJECT

private slots:
  void resumesInterruptedCreationWithSameOperationId_FI_V01_PROJ_01();
  void retainsPreexistingAssetOnCollision_FI_V01_PROJ_01();
  void retainsUserModifiedAssetAndRequiresManualIntervention_FI_V01_PROJ_01();
  void retainsOperationCreatedAssetForManualIntervention_FI_V01_PROJ_01();
  void repeatsReadyOperationWithoutSecondAsset_FI_V01_PROJ_01();
  void startupCoordinatorRecoversPendingAfterResolverRestart_FI_V01_PROJ_01();
  void revokedRootRequiresManualInterventionAndKeepsProjectInvisible_FI_V01_PROJ_01();
  void serializesConcurrentSameOperationIdReplay();
};

void ProjectProvisioningTest::resumesInterruptedCreationWithSameOperationId_FI_V01_PROJ_01() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const ProjectProvisioningRequest operation = request();
  ResourceResolver firstResolver;
  const QString rootId = registerWritableRoot(firstResolver, directory.path());
  QVERIFY(!rootId.isEmpty());
  ProjectProvisioningSaga interrupted(databasePath, firstResolver, rootId,
                                      ProjectProvisioningFault::after_asset_recorded);

  QCOMPARE(interrupted.provision(operation).code, ProjectProvisioningCode::recovery_required);
  QVERIFY(QFile::exists(directory.filePath("project.md")));
  QVERIFY(projectIsInvisible(databasePath, operation.projectId));
  ResourceResolver restartedResolver;
  const QString restartedRootId = registerWritableRoot(restartedResolver, directory.path());
  QCOMPARE(restartedRootId, rootId);
  const ProjectProvisioningSaga recovered(databasePath, restartedResolver, restartedRootId);
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

  ResourceResolver resolver;
  const ProjectProvisioningSaga saga(databasePath, resolver, registerWritableRoot(resolver, directory.path()));
  QCOMPARE(saga.provision(request()).code, ProjectProvisioningCode::asset_collision);
  QCOMPARE(readFile(assetPath), QByteArray("already exists"));
  QCOMPARE(saga.abandon("provision-1").code, ProjectProvisioningCode::manual_intervention_required);
  QCOMPARE(readFile(assetPath), QByteArray("already exists"));
  const auto snapshot = saga.query("provision-1");
  QVERIFY(snapshot.value.has_value());
  QCOMPARE(snapshot.value.value_or({}).state, ProjectProvisioningState::failed);
  QVERIFY(projectIsInvisible(databasePath, "project-1"));
}

void ProjectProvisioningTest::retainsUserModifiedAssetAndRequiresManualIntervention_FI_V01_PROJ_01() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const ProjectProvisioningRequest operation = request();
  ResourceResolver resolver;
  const QString rootId = registerWritableRoot(resolver, directory.path());
  ProjectProvisioningSaga interrupted(databasePath, resolver, rootId, ProjectProvisioningFault::after_asset_recorded);
  QCOMPARE(interrupted.provision(operation).code, ProjectProvisioningCode::recovery_required);
  const QString assetPath = directory.filePath("project.md");
  ProjectProvisioningSaga verified(databasePath, resolver, rootId, ProjectProvisioningFault::after_asset_proven);
  QCOMPARE(verified.provision(operation).code, ProjectProvisioningCode::recovery_required);

  // 普通文件写入不参与 resolver 的 flock，模拟摘要核验完成后的外部原地改写。
  QVERIFY(writeFile(assetPath, "user content"));
  QCOMPARE(verified.abandon(operation.operationId).code, ProjectProvisioningCode::manual_intervention_required);
  QCOMPARE(readFile(assetPath), QByteArray("user content"));
}

void ProjectProvisioningTest::retainsOperationCreatedAssetForManualIntervention_FI_V01_PROJ_01() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const ProjectProvisioningRequest operation = request();
  ResourceResolver resolver;
  const QString rootId = registerWritableRoot(resolver, directory.path());
  ProjectProvisioningSaga interrupted(databasePath, resolver, rootId, ProjectProvisioningFault::after_asset_recorded);
  QCOMPARE(interrupted.provision(operation).code, ProjectProvisioningCode::recovery_required);

  const ProjectProvisioningSaga saga(databasePath, resolver, rootId);
  QCOMPARE(saga.abandon(operation.operationId).code, ProjectProvisioningCode::manual_intervention_required);
  QCOMPARE(readFile(directory.filePath("project.md")), QByteArray("# 研究项目\n"));
  const auto snapshot = saga.query(operation.operationId);
  QVERIFY(snapshot.value.has_value());
  QCOMPARE(snapshot.value.value_or({}).state, ProjectProvisioningState::failed);
  QCOMPARE(snapshot.value.value_or({}).failureCode, QString("manual_intervention_required"));
  QVERIFY(projectIsInvisible(databasePath, operation.projectId));
}

void ProjectProvisioningTest::repeatsReadyOperationWithoutSecondAsset_FI_V01_PROJ_01() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  ResourceResolver resolver;
  const ProjectProvisioningSaga saga(databasePath, resolver, registerWritableRoot(resolver, directory.path()));
  const ProjectProvisioningRequest operation = request();

  QVERIFY(saga.provision(operation).isSucceeded());
  const QByteArray firstContents = readFile(directory.filePath("project.md"));
  QVERIFY(saga.provision(operation).isSucceeded());
  QCOMPARE(readFile(directory.filePath("project.md")), firstContents);
  QCOMPARE(saga.provision({"provision-1", "project-1", "研究项目", "another.md"}).code,
           ProjectProvisioningCode::invalid_argument);
  QVERIFY(!QFile::exists(directory.filePath("another.md")));
}

void ProjectProvisioningTest::startupCoordinatorRecoversPendingAfterResolverRestart_FI_V01_PROJ_01() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  ResourceResolver initialResolver;
  const QString rootId = registerWritableRoot(initialResolver, directory.path());
  QVERIFY(!rootId.isEmpty());
  const ProjectProvisioningRequest operation = request("restart-coordinator");
  QCOMPARE(
      ProjectProvisioningSaga(databasePath, initialResolver, rootId, ProjectProvisioningFault::after_asset_recorded)
          .provision(operation)
          .code,
      ProjectProvisioningCode::recovery_required);
  QVERIFY(projectIsInvisible(databasePath, operation.projectId));

  ResourceResolver restartedResolver;
  QCOMPARE(registerWritableRoot(restartedResolver, directory.path()), rootId);
  QCOMPARE(ProjectProvisioningRecoveryCoordinator(databasePath, restartedResolver).recoverPending(),
           ProjectProvisioningCode::none);
  QVERIFY(!projectIsInvisible(databasePath, operation.projectId));
}

void ProjectProvisioningTest::revokedRootRequiresManualInterventionAndKeepsProjectInvisible_FI_V01_PROJ_01() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  ResourceResolver resolver;
  const QString rootId = registerWritableRoot(resolver, directory.path());
  QVERIFY(!rootId.isEmpty());
  const ProjectProvisioningRequest operation = request("revoked-root");
  QCOMPARE(ProjectProvisioningSaga(databasePath, resolver, rootId, ProjectProvisioningFault::after_asset_recorded)
               .provision(operation)
               .code,
           ProjectProvisioningCode::recovery_required);
  QVERIFY(resolver.revokeRoot(rootId).isAccepted());
  QCOMPARE(ProjectProvisioningSaga(databasePath, resolver, rootId).provision(operation).code,
           ProjectProvisioningCode::manual_intervention_required);
  QVERIFY(projectIsInvisible(databasePath, operation.projectId));
  QVERIFY(QFile::exists(directory.filePath(operation.assetName)));
}

void ProjectProvisioningTest::serializesConcurrentSameOperationIdReplay() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  ResourceResolver resolver;
  const QString rootId = registerWritableRoot(resolver, directory.path());
  QVERIFY(!rootId.isEmpty());
  std::barrier start(3);
  std::array<ProjectProvisioningCode, 2> results{};
  std::array<std::thread, 2> workers;
  for (std::size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::thread([&, index] {
      start.arrive_and_wait();
      results[index] =
          ProjectProvisioningSaga(databasePath, resolver, rootId).provision(request("same-operation")).code;
    });
  }
  start.arrive_and_wait();
  for (std::thread &worker : workers)
    worker.join();
  QCOMPARE(results[0], ProjectProvisioningCode::none);
  QCOMPARE(results[1], ProjectProvisioningCode::none);
  QVERIFY(!projectIsInvisible(databasePath, "project-1"));
}

QTEST_APPLESS_MAIN(ProjectProvisioningTest)

#include "project_provisioning_test.moc"
