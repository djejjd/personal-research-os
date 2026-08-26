#pragma once

#include "pros/application/approval_commands.h"
#include "pros/application/governance_command_handler.h"
#include "pros/application/work_commands.h"

namespace pros::application {

/**
 * S1 所有领域写命令的统一应用入口。
 *
 * @return 成功、确定性业务拒绝或稳定的 `storage_unavailable`；不以空值表达失败。
 * @note 实现负责在适配器内部生成请求摘要，并保证每个命令只进入一次共享事务边界。
 */
class CommandFacade {
public:
  virtual ~CommandFacade() = default;

  [[nodiscard]] virtual domain::CommandResult execute(const CreateProject &command) = 0;
  [[nodiscard]] virtual domain::CommandResult execute(const CreateTask &command) = 0;
  [[nodiscard]] virtual domain::CommandResult execute(const UpdateTask &command) = 0;
  [[nodiscard]] virtual domain::CommandResult execute(const CreateMilestone &command) = 0;
  [[nodiscard]] virtual domain::CommandResult execute(const CreateDirection &command) = 0;
  [[nodiscard]] virtual domain::CommandResult execute(const LinkNoteToTask &command) = 0;
  [[nodiscard]] virtual domain::CommandResult execute(const RecordEvidence &command) = 0;
  [[nodiscard]] virtual domain::CommandResult execute(const RecordAcceptance &command) = 0;
  [[nodiscard]] virtual domain::CommandResult execute(const CreateOperationPlan &command) = 0;
  [[nodiscard]] virtual domain::CommandResult execute(const RecordApproval &command) = 0;
  [[nodiscard]] virtual domain::CommandResult execute(const DispatchOperationPlan &command) = 0;
};

} // namespace pros::application
