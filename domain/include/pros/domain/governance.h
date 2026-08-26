#pragma once

#include "pros/domain/command_contract.h"

#include <optional>
#include <string>
#include <vector>

namespace pros::domain {

/** 文档的稳定引用；章节引用存在时必须与文档身份共同使用，不能以标题或路径替代。 */
class DocumentReference final {
public:
  DocumentReference(std::string documentId, std::optional<std::string> sectionId = std::nullopt);
  [[nodiscard]] const std::string &documentId() const;
  [[nodiscard]] const std::optional<std::string> &sectionId() const;
  friend bool operator==(const DocumentReference &, const DocumentReference &) = default;

private:
  std::string documentId_;
  std::optional<std::string> sectionId_;
};

/** 证据观察的不可变版本引用。 */
struct EvidenceObservationReference final {
  std::string evidenceId;
  Revision revision;
  friend bool operator==(const EvidenceObservationReference &, const EvidenceObservationReference &) = default;
};

/** 验收结论。`passed` 必须拥有至少一个仍有效的证据观察。 */
enum class AcceptanceConclusion { passed, failed, inconclusive };

/** 一次验收的冻结目标与证据选择。Evidence 的适用关系由本对象唯一拥有。 */
class Acceptance final {
public:
  Acceptance(std::string acceptanceId, std::string targetId, Revision targetCandidateRevision,
             Revision acceptanceSpecRevision, AcceptanceConclusion conclusion,
             std::vector<EvidenceObservationReference> evidence);
  [[nodiscard]] const std::string &id() const;
  [[nodiscard]] const std::string &targetId() const;
  [[nodiscard]] Revision targetCandidateRevision() const;
  [[nodiscard]] Revision acceptanceSpecRevision() const;
  [[nodiscard]] AcceptanceConclusion conclusion() const;
  [[nodiscard]] const std::vector<EvidenceObservationReference> &evidence() const;

private:
  std::string id_;
  std::string targetId_;
  Revision targetCandidateRevision_;
  Revision acceptanceSpecRevision_;
  AcceptanceConclusion conclusion_;
  std::vector<EvidenceObservationReference> evidence_;
};

/** 已持久化的证据观察，revision 是验收引用必须冻结的版本。 */
struct EvidenceObservation final {
  std::string evidenceId;
  std::string taskId;
  std::string locator;
  Revision revision;
};

/** 面向用户解释的不可变活动事实；它不能作为恢复流程的权威记录。 */
struct Activity final {
  std::string id;
  std::string eventType;
  std::string subjectId;
  std::string summary;
};

/** 从任务反查得到的治理事实链；各集合只包含同一 task 的已提交事实。 */
struct GovernanceTrace final {
  std::string taskId;
  Revision revision;
  std::vector<DocumentReference> notes;
  std::vector<EvidenceObservation> evidence;
  std::vector<Acceptance> acceptances;
  std::vector<Activity> activities;
};

} // namespace pros::domain
