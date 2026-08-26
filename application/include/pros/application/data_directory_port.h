#pragma once

#include <QString>

namespace pros::application {

class DataDirectoryPort {
 public:
  virtual ~DataDirectoryPort() = default;

  virtual bool ensureExists(const QString& path, QString* errorMessage) = 0;
};

}  // namespace pros::application
