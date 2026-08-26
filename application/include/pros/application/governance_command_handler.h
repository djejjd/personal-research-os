#pragma once

#include "pros/domain/command_contract.h"
#include "pros/domain/governance.h"

#include <QString>

#include <optional>
#include <string>
#include <vector>

namespace pros::application {

struct LinkNoteToTask final {
  domain::OperationKey operation;
  domain::Revision expectedRevision;
  std::string taskId;
  domain::DocumentReference note;
};

struct RecordEvidence final {
  domain::OperationKey operation;
  domain::Revision expectedRevision;
  std::string evidenceId;
  std::string taskId;
  std::string locator;
};

/** 原始验收命令允许表达无证据 passed，由 handler 作为确定性失败记录并重放。 */
struct RecordAcceptance final {
  domain::OperationKey operation;
  domain::Revision expectedRevision;
  std::string acceptanceId;
  std::string taskId;
  domain::Revision candidateRevision;
  domain::Revision specificationRevision;
  domain::AcceptanceConclusion conclusion;
  std::vector<domain::EvidenceObservationReference> evidence;
};

/**
 * S1 治理写命令的唯一公开入口。
 *
 * @return 返回已提交、重放或 `storage_unavailable` 结构化结果；存储故障不得写入 operation ledger。
 * @note 实现必须在内部生成包含全部命令字段与 expected revision 的稳定摘要，调用方不能提供摘要。
 */
class GovernanceCommandHandler {
public:
  virtual ~GovernanceCommandHandler() = default;
  [[nodiscard]] virtual domain::CommandResult handle(const LinkNoteToTask &command,
                                                     QString *errorMessage = nullptr) = 0;
  [[nodiscard]] virtual domain::CommandResult handle(const RecordEvidence &command,
                                                     QString *errorMessage = nullptr) = 0;
  [[nodiscard]] virtual domain::CommandResult handle(const RecordAcceptance &command,
                                                     QString *errorMessage = nullptr) = 0;
};

} // namespace pros::application
