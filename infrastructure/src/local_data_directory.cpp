#include "pros/infrastructure/local_data_directory.h"

#include <QDir>
#include <QFileInfo>

namespace pros::infrastructure {

bool LocalDataDirectory::ensureExists(const QString &path, QString *errorMessage) {
  const QFileInfo target(path);
  if (target.exists() && !target.isDir()) {
    if (errorMessage != nullptr) {
      *errorMessage = "数据目录路径不是目录";
    }
    return false;
  }

  if (!QDir().mkpath(path)) {
    if (errorMessage != nullptr) {
      *errorMessage = "无法创建数据目录";
    }
    return false;
  }

  const QFileInfo created(path);
  if (!created.isWritable()) {
    if (errorMessage != nullptr) {
      *errorMessage = "数据目录不可写";
    }
    return false;
  }

  return true;
}

} // namespace pros::infrastructure
