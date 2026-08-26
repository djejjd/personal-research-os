#pragma once

#include "pros/domain/approval.h"

namespace pros::application {

struct CreateOperationPlan final {
  domain::OperationKey operationKey;
  domain::Revision expectedRevision{0};
  std::string planId;
  std::string summary;
};

struct RecordApproval final {
  domain::OperationKey operationKey;
  domain::Revision expectedRevision{0};
  std::string approvalId;
  std::string planId;
  domain::Revision planRevision{0};
  std::string planDigest;
  domain::ApprovalDecision decision{domain::ApprovalDecision::pending};
  std::string note;
};

struct DispatchOperationPlan final {
  domain::OperationKey operationKey;
  domain::Revision expectedRevision{0};
  std::string planId;
};

/** 计划与审批命令的应用端口；组合根只通过统一 CommandFacade 持有该端口。 */
class ApprovalCommandPort {
public:
  virtual ~ApprovalCommandPort() = default;
  [[nodiscard]] virtual domain::CommandResult createOperationPlan(const CreateOperationPlan &command) = 0;
  [[nodiscard]] virtual domain::CommandResult recordApproval(const RecordApproval &command) = 0;
  [[nodiscard]] virtual domain::CommandResult dispatchOperationPlan(const DispatchOperationPlan &command) = 0;
};

} // namespace pros::application
