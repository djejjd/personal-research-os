#include "pros/infrastructure/project_provisioning.h"
#include "pros/infrastructure/resource_resolver.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

class AppRecoveryStartupTest final : public QObject {
  Q_OBJECT

private slots:
  void startsWhenPendingProvisioningRootRequiresFreshAuthorization();
};

void AppRecoveryStartupTest::startsWhenPendingProvisioningRootRequiresFreshAuthorization() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = directory.filePath("personal-research-os.sqlite");
  QString error;
  QVERIFY(pros::infrastructure::SchemaMigrator().migrate(databasePath, &error));
  pros::infrastructure::ResourceResolver resolver;
  const auto root = resolver.registerRoot(directory.path(), pros::infrastructure::ResourceAccess::read_write);
  QVERIFY(root.isAccepted());
  const QString rootId = root.root.has_value() ? root.root->id : QString();
  QVERIFY(!rootId.isEmpty());
  const pros::infrastructure::ProjectProvisioningRequest request{"startup-pending", "startup-project", "启动恢复",
                                                                 "startup-project.md"};
  QCOMPARE(pros::infrastructure::ProjectProvisioningSaga(
               databasePath, resolver, rootId, pros::infrastructure::ProjectProvisioningFault::after_asset_recorded)
               .provision(request)
               .code,
           pros::infrastructure::ProjectProvisioningCode::recovery_required);

  QProcess application;
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert("QT_QPA_PLATFORM", "offscreen");
  application.setProcessEnvironment(environment);
  application.start(QString::fromUtf8(PROS_APP_EXECUTABLE), {"--data-dir", directory.path(), "--smoke-test"});
  QVERIFY(application.waitForFinished(10000));
  QCOMPARE(application.exitStatus(), QProcess::NormalExit);
  QCOMPARE(application.exitCode(), 0);
}

QTEST_APPLESS_MAIN(AppRecoveryStartupTest)

#include "app_recovery_startup_test.moc"
