#pragma once

#include "pros/application/data_directory_port.h"

namespace pros::infrastructure {

class LocalDataDirectory final : public application::DataDirectoryPort {
public:
  /**
   * 在本地文件系统中创建或验证数据目录。
   *
   * @return 成功时返回 true；失败时不记录路径或底层原始错误，并通过 errorMessage 返回安全摘要。
   */
  bool ensureExists(const QString &path, QString *errorMessage) override;
};

} // namespace pros::infrastructure
