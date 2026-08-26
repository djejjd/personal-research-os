#pragma once

#include <QString>

namespace pros::application {

class DataDirectoryPort {
public:
  virtual ~DataDirectoryPort() = default;

  /**
   * 确保受控数据目录存在且可供本地应用使用。
   *
   * @param path 调用方已验证的目标路径。
   * @param errorMessage 失败时接收面向用户的安全说明，可为 nullptr。
   * @return 目录已存在或已成功创建时返回 true；重复调用必须保持幂等。
   */
  virtual bool ensureExists(const QString &path, QString *errorMessage) = 0;
};

} // namespace pros::application
