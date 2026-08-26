#pragma once

#include "pros/domain/command_contract.h"
#include "pros/domain/work_aggregates.h"

#include <QString>

#include <string>

namespace pros::application {

/** 所有工作命令的公开信封；幂等键和期望版本由调用者提供，请求摘要始终由处理器内部生成。 */
struct WorkCommandEnvelope final {
  domain::OperationKey operation;
  domain::Revision expectedRevision;
};

struct CreateProject final {
  WorkCommandEnvelope envelope;
  std::string projectId;
  std::string title;
};

struct CreateTask final {
  WorkCommandEnvelope envelope;
  std::string taskId;
  std::string projectId;
  std::string title;
};

struct UpdateTask final {
  WorkCommandEnvelope envelope;
  std::string taskId;
  domain::TaskStatus status;
};

struct CreateMilestone final {
  WorkCommandEnvelope envelope;
  std::string milestoneId;
  std::string projectId;
  std::string title;
};

struct CreateDirection final {
  WorkCommandEnvelope envelope;
  std::string directionId;
  std::string title;
};

/**
 * 工作聚合的唯一公开写入口。
 *
 * @return 业务成功或可重放拒绝；存储、事务或提交失败返回 `storage_unavailable` 并填写中文诊断。
 * @note 同一 caller/operation 的相同命令重放首次结果；任何字段或 expected revision 改变均返回
 * `idempotency_key_reused`。
 */
class WorkCommandHandler {
public:
  virtual ~WorkCommandHandler() = default;
  [[nodiscard]] virtual domain::CommandResult handle(const CreateProject &command,
                                                     QString *errorMessage = nullptr) const = 0;
  [[nodiscard]] virtual domain::CommandResult handle(const CreateTask &command,
                                                     QString *errorMessage = nullptr) const = 0;
  [[nodiscard]] virtual domain::CommandResult handle(const UpdateTask &command,
                                                     QString *errorMessage = nullptr) const = 0;
  [[nodiscard]] virtual domain::CommandResult handle(const CreateMilestone &command,
                                                     QString *errorMessage = nullptr) const = 0;
  [[nodiscard]] virtual domain::CommandResult handle(const CreateDirection &command,
                                                     QString *errorMessage = nullptr) const = 0;
};

} // namespace pros::application
