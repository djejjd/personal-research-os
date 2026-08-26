#pragma once

#include "pros/application/approval_commands.h"
#include "pros/infrastructure/sqlite_command_transaction.h"

#include <QString>

namespace pros::infrastructure {

/** SQLite 计划审批命令适配器；不提供任何执行、网络、终端或自动化端口。 */
class SqliteApprovalCommandHandler final : public application::ApprovalCommandPort {
public:
  explicit SqliteApprovalCommandHandler(QString databasePath);

  [[nodiscard]] domain::CommandResult createOperationPlan(const application::CreateOperationPlan &command) override;
  [[nodiscard]] domain::CommandResult recordApproval(const application::RecordApproval &command) override;
  [[nodiscard]] domain::CommandResult dispatchOperationPlan(const application::DispatchOperationPlan &command) override;

private:
  SqliteCommandTransaction transaction_;
};

} // namespace pros::infrastructure
