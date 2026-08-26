#pragma once

#include <QString>

namespace pros::infrastructure {

class SchemaMigrator final {
public:
  /**
   * 将本地数据库迁移到当前支持的唯一 schema 版本。
   *
   * @param databasePath 待迁移 SQLite 数据库路径；调用方必须保证其属于受控数据目录。
   * @param errorMessage 失败时接收不含路径或 SQLite 原始错误的用户安全说明，可为 nullptr。
   * @return 迁移成功返回 true。对当前版本重复调用是幂等的；未知版本或损坏 metadata 会失败且回滚事务。
   */
  bool migrate(const QString &databasePath, QString *errorMessage) const;

  /**
   * 读取经过唯一性校验的已存储 schema 版本。
   *
   * @return 成功返回唯一版本；数据库不可读、metadata 缺失或损坏时返回 -1，并在提供时填写安全错误说明。
   */
  [[nodiscard]] int schemaVersion(const QString &databasePath, QString *errorMessage) const;
};

} // namespace pros::infrastructure
