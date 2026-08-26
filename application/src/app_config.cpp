#include "pros/application/app_config.h"

#include <QDir>
#include <QStandardPaths>

#include <stdexcept>

namespace pros::application {

AppConfig::AppConfig(QString dataDirectory) : dataDirectory_(std::move(dataDirectory)) {}

AppConfig AppConfig::fromArguments(const QStringList &arguments) {
  QString dataDirectory;

  for (qsizetype index = 1; index < arguments.size(); ++index) {
    if (arguments.at(index) != "--data-dir") {
      continue;
    }

    if (index + 1 >= arguments.size() || arguments.at(index + 1).isEmpty()) {
      throw std::invalid_argument("--data-dir requires a non-empty path");
    }

    dataDirectory = arguments.at(++index);
  }

  if (dataDirectory.isEmpty()) {
    dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  }

  return AppConfig(QDir::cleanPath(dataDirectory));
}

const QString &AppConfig::dataDirectory() const { return dataDirectory_; }

} // namespace pros::application
