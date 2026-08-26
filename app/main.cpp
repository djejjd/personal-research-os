#include "pros/application/app_config.h"
#include "pros/infrastructure/local_data_directory.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include <exception>

int main(int argc, char* argv[]) {
  QApplication application(argc, argv);

  try {
    const pros::application::AppConfig config = pros::application::AppConfig::fromArguments(application.arguments());
    pros::infrastructure::LocalDataDirectory dataDirectory;
    QString errorMessage;
    if (!dataDirectory.ensureExists(config.dataDirectory(), &errorMessage)) {
      qCritical().noquote() << errorMessage;
      return 1;
    }

    const QString databasePath = config.dataDirectory() + "/personal-research-os.sqlite";
    pros::infrastructure::SchemaMigrator migrator;
    if (!migrator.migrate(databasePath, &errorMessage)) {
      qCritical().noquote() << "数据库迁移失败：" << errorMessage;
      return 1;
    }

    const int schemaVersion = migrator.schemaVersion(databasePath, &errorMessage);
    if (schemaVersion < 0) {
      qCritical().noquote() << "无法读取数据库版本：" << errorMessage;
      return 1;
    }

    if (application.arguments().contains("--smoke-test")) {
      QTimer::singleShot(0, &application, &QCoreApplication::quit);
      return application.exec();
    }

    QMainWindow window;
    window.setWindowTitle("Personal Research OS");
    window.setCentralWidget(new QLabel(QString("本地数据目录：%1\nSchema 版本：%2")
                                            .arg(config.dataDirectory())
                                            .arg(schemaVersion)));
    window.resize(560, 160);
    window.show();
    return application.exec();
  } catch (const std::exception& error) {
    qCritical().noquote() << "启动参数无效：" << error.what();
    return 1;
  }
}
