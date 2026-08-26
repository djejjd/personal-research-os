#pragma once

#include "pros/application/command_facade.h"

#include <QString>

#include <memory>

namespace pros::infrastructure {

/**
 * 为已经迁移到当前 schema 的数据库组装唯一 S1 命令门面。
 *
 * @pre `databasePath` 指向由 `SchemaMigrator` 成功迁移的 SQLite 数据库。
 * @note 构造不运行迁移；命令执行不提供网络、终端、自动化或外部执行能力。
 */
[[nodiscard]] std::unique_ptr<application::CommandFacade> makeCommandFacade(const QString &databasePath);

} // namespace pros::infrastructure
