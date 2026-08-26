#include "pros/domain/governance.h"

#include <stdexcept>
#include <utility>

namespace pros::domain {
namespace {
void requireNonEmpty(const std::string &value, const char *name) {
  if (value.empty())
    throw std::invalid_argument(name);
}
} // namespace

DocumentReference::DocumentReference(std::string documentId, std::optional<std::string> sectionId)
    : documentId_(std::move(documentId)), sectionId_(std::move(sectionId)) {
  requireNonEmpty(documentId_, "documentId must not be empty");
  if (sectionId_)
    requireNonEmpty(*sectionId_, "sectionId must not be empty");
}

const std::string &DocumentReference::documentId() const { return documentId_; }
const std::optional<std::string> &DocumentReference::sectionId() const { return sectionId_; }

Acceptance::Acceptance(std::string acceptanceId, std::string targetId, Revision targetCandidateRevision,
                       Revision acceptanceSpecRevision, AcceptanceConclusion conclusion,
                       std::vector<EvidenceObservationReference> evidence)
    : id_(std::move(acceptanceId)), targetId_(std::move(targetId)), targetCandidateRevision_(targetCandidateRevision),
      acceptanceSpecRevision_(acceptanceSpecRevision), conclusion_(conclusion), evidence_(std::move(evidence)) {
  requireNonEmpty(id_, "acceptanceId must not be empty");
  requireNonEmpty(targetId_, "targetId must not be empty");
  if (conclusion_ == AcceptanceConclusion::passed && evidence_.empty())
    throw std::invalid_argument("passed acceptance requires evidence");
  for (const EvidenceObservationReference &reference : evidence_)
    requireNonEmpty(reference.evidenceId, "evidenceId must not be empty");
}

const std::string &Acceptance::id() const { return id_; }
const std::string &Acceptance::targetId() const { return targetId_; }
Revision Acceptance::targetCandidateRevision() const { return targetCandidateRevision_; }
Revision Acceptance::acceptanceSpecRevision() const { return acceptanceSpecRevision_; }
AcceptanceConclusion Acceptance::conclusion() const { return conclusion_; }
const std::vector<EvidenceObservationReference> &Acceptance::evidence() const { return evidence_; }

} // namespace pros::domain
