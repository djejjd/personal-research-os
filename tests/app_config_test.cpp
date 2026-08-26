#include "pros/application/app_config.h"

#include <QtTest>

#include <stdexcept>

class AppConfigTest final : public QObject {
  Q_OBJECT

private slots:
  void usesExplicitDataDirectory();
  void rejectsMissingDataDirectoryValue();
};

void AppConfigTest::usesExplicitDataDirectory() {
  const pros::application::AppConfig config =
      pros::application::AppConfig::fromArguments({"app", "--data-dir", "/tmp/pros-data"});

  QCOMPARE(config.dataDirectory(), "/tmp/pros-data");
}

void AppConfigTest::rejectsMissingDataDirectoryValue() {
  QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pros::application::AppConfig::fromArguments({"app", "--data-dir"}));
}

QTEST_APPLESS_MAIN(AppConfigTest)

#include "app_config_test.moc"
