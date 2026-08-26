#pragma once

#include "pros/application/data_directory_port.h"

namespace pros::infrastructure {

class LocalDataDirectory final : public application::DataDirectoryPort {
 public:
  bool ensureExists(const QString& path, QString* errorMessage) override;
};

}  // namespace pros::infrastructure
