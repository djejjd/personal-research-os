#pragma once

#include <QString>

namespace pros::infrastructure {

class SchemaMigrator final {
 public:
  bool migrate(const QString& databasePath, QString* errorMessage) const;
  [[nodiscard]] int schemaVersion(const QString& databasePath, QString* errorMessage) const;
};

}  // namespace pros::infrastructure
