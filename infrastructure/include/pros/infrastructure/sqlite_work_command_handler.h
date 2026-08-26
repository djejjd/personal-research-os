#pragma once

#include "pros/application/work_commands.h"

#include <QString>

namespace pros::infrastructure {

/** 通过共享 SQLite 命令事务执行工作聚合命令；构造不会打开数据库或运行迁移。 */
class SqliteWorkCommandHandler final : public application::WorkCommandHandler {
public:
  explicit SqliteWorkCommandHandler(QString databasePath);

  [[nodiscard]] domain::CommandResult handle(const application::CreateProject &command,
                                             QString *errorMessage = nullptr) const override;
  [[nodiscard]] domain::CommandResult handle(const application::CreateTask &command,
                                             QString *errorMessage = nullptr) const override;
  [[nodiscard]] domain::CommandResult handle(const application::UpdateTask &command,
                                             QString *errorMessage = nullptr) const override;
  [[nodiscard]] domain::CommandResult handle(const application::CreateMilestone &command,
                                             QString *errorMessage = nullptr) const override;
  [[nodiscard]] domain::CommandResult handle(const application::CreateDirection &command,
                                             QString *errorMessage = nullptr) const override;

private:
  QString databasePath_;
};

} // namespace pros::infrastructure
