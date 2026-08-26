#pragma once

#include "pros/domain/command_contract.h"

#include <string>

namespace pros::domain {

/** V0.1 仅存储和展示的不可变操作计划。 */
struct OperationPlan final {
  std::string id;
  std::string summary;
  std::string digest;
  Revision revision{0};

  friend bool operator==(const OperationPlan &, const OperationPlan &) = default;
};

/** 审批决定只表达保存事实，不授予本版本任何执行能力。 */
enum class ApprovalDecision { pending, approved, rejected };

/** 对确切计划 revision 与摘要的不可变审批记录。 */
struct Approval final {
  std::string id;
  std::string planId;
  Revision planRevision{0};
  std::string planDigest;
  ApprovalDecision decision{ApprovalDecision::pending};
  std::string note;
  Revision revision{0};

  friend bool operator==(const Approval &, const Approval &) = default;
};

} // namespace pros::domain
