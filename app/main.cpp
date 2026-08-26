#include "pros/application/app_config.h"
#include "pros/domain/schema_version.h"
#include "pros/infrastructure/file_operation_log.h"
#include "pros/infrastructure/local_data_directory.h"
#include "pros/infrastructure/resource_resolver.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <cstring>
#include <exception>

namespace {

struct LogEventDefinition {
  const char *eventCode;
  const char *message;
  const char *module;
};

constexpr LogEventDefinition kApplicationStartup{"app.startup", "应用启动状态变化", "app_startup"};
constexpr LogEventDefinition kDataDirectory{"storage.data_directory", "本地数据目录初始化", "local_data_directory"};
constexpr LogEventDefinition kSchemaMigration{"storage.schema_migration", "本地 schema 迁移", "schema_migrator"};
constexpr LogEventDefinition kSchemaVersion{"storage.schema_version", "本地 schema 版本读取", "schema_migrator"};
constexpr LogEventDefinition kFileRecovery{"storage.file_recovery", "本地文件恢复扫描", "file_operation_log"};

void logEvent(const LogEventDefinition &definition, const char *result, const char *level = "INFO",
              const char *reasonCode = nullptr) {
  static const QString processInstanceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  static std::atomic_uint64_t sequence{0};
  QJsonObject event{{"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                    {"level", QString::fromLatin1(level)},
                    {"event_code", QString::fromLatin1(definition.eventCode)},
                    {"message", QString::fromUtf8(definition.message)},
                    {"module", QString::fromLatin1(definition.module)},
                    {"result", QString::fromLatin1(result)},
                    {"app_version", "0.1.0-dev"},
                    {"schema_version", pros::domain::kCurrentSchemaVersion},
                    {"process_instance_id", processInstanceId},
                    {"sequence", static_cast<qint64>(++sequence)}};
  if (reasonCode != nullptr) {
    event.insert("reason_code", QString::fromLatin1(reasonCode));
  }
  const QByteArray rendered = QJsonDocument(event).toJson(QJsonDocument::Compact);
  if (std::strcmp(level, "ERROR") == 0)
    qCritical().noquote() << rendered;
  else
    qInfo().noquote() << rendered;
}

} // namespace

int main(int argc, char *argv[]) {
  QApplication application(argc, argv);
  logEvent(kApplicationStartup, "started");

  try {
    const pros::application::AppConfig config = pros::application::AppConfig::fromArguments(application.arguments());
    pros::infrastructure::LocalDataDirectory dataDirectory;
    QString errorMessage;
    if (!dataDirectory.ensureExists(config.dataDirectory(), &errorMessage)) {
      logEvent(kDataDirectory, "failed", "ERROR", "data_directory_unavailable");
      return 1;
    }
    logEvent(kDataDirectory, "succeeded");

    const QString databasePath = config.dataDirectory() + "/personal-research-os.sqlite";
    pros::infrastructure::SchemaMigrator migrator;
    logEvent(kSchemaMigration, "started");
    if (!migrator.migrate(databasePath, &errorMessage)) {
      logEvent(kSchemaMigration, "failed", "ERROR", "schema_migration_failed");
      return 1;
    }
    logEvent(kSchemaMigration, "succeeded");

    pros::infrastructure::ResourceResolver resourceResolver;
    pros::infrastructure::FileOperationLog fileOperationLog(databasePath);
    logEvent(kFileRecovery, "started");
    const pros::infrastructure::FileRecoveryReport recovery = fileOperationLog.recoverPending(resourceResolver);
    if (!recovery.isSucceeded()) {
      logEvent(kFileRecovery, "failed", "ERROR", pros::infrastructure::fileOperationCodeName(recovery.code));
      return 1;
    }
    logEvent(kFileRecovery, "succeeded");

    const int schemaVersion = migrator.schemaVersion(databasePath, &errorMessage);
    if (schemaVersion < 0) {
      logEvent(kSchemaVersion, "failed", "ERROR", "schema_version_unavailable");
      return 1;
    }

    if (application.arguments().contains("--smoke-test")) {
      logEvent(kApplicationStartup, "succeeded");
      QTimer::singleShot(0, &application, &QCoreApplication::quit);
      return application.exec();
    }

    QMainWindow window;
    window.setWindowTitle("Personal Research OS");
    window.setCentralWidget(
        new QLabel(QString("本地数据目录：%1\nSchema 版本：%2").arg(config.dataDirectory()).arg(schemaVersion)));
    window.resize(560, 160);
    window.show();
    logEvent(kApplicationStartup, "succeeded");
    return application.exec();
  } catch (const std::exception &error) {
    logEvent(kApplicationStartup, "failed", "ERROR", "invalid_startup_arguments");
    return 1;
  }
}
