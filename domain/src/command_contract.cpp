#include "pros/domain/command_contract.h"

#include <stdexcept>
#include <utility>

namespace pros::domain {
namespace {

void requireNotEmpty(const std::string &value, const char *fieldName) {
  if (value.empty()) {
    throw std::invalid_argument(fieldName);
  }
}

} // namespace

Revision::Revision(std::int64_t value) : value_(value) {
  if (value < 0) {
    throw std::invalid_argument("revision must not be negative");
  }
}

std::int64_t Revision::value() const { return value_; }

OperationKey::OperationKey(std::string callerId, std::string operationId)
    : callerId_(std::move(callerId)), operationId_(std::move(operationId)) {
  requireNotEmpty(callerId_, "caller id must not be empty");
  requireNotEmpty(operationId_, "operation id must not be empty");
}

const std::string &OperationKey::callerId() const { return callerId_; }

const std::string &OperationKey::operationId() const { return operationId_; }

CommandResult CommandResult::succeeded(std::string aggregateId, Revision revision) {
  requireNotEmpty(aggregateId, "aggregate id must not be empty");
  return CommandResult(CommandSuccess{std::move(aggregateId), revision}, std::nullopt);
}

CommandResult CommandResult::rejected(CommandErrorCode errorCode) { return CommandResult(std::nullopt, errorCode); }

bool CommandResult::isSuccess() const { return success_.has_value(); }

const std::optional<CommandSuccess> &CommandResult::success() const { return success_; }

const std::optional<CommandErrorCode> &CommandResult::errorCode() const { return errorCode_; }

CommandResult::CommandResult(std::optional<CommandSuccess> success, std::optional<CommandErrorCode> errorCode)
    : success_(std::move(success)), errorCode_(errorCode) {}

std::optional<CommandErrorCode> verifyExpectedRevision(Revision expected, Revision actual) {
  if (expected == actual) {
    return std::nullopt;
  }
  return CommandErrorCode::revision_conflict;
}

OperationReplayDecision decideOperationReplay(const OperationKey &key, const std::string &requestDigest,
                                              const std::optional<RecordedOperation> &recordedOperation) {
  requireNotEmpty(requestDigest, "request digest must not be empty");
  if (!recordedOperation.has_value()) {
    return {OperationReplayAction::execute, std::nullopt};
  }
  if (recordedOperation->key != key) {
    throw std::invalid_argument("recorded operation key does not match request key");
  }
  if (recordedOperation->requestDigest != requestDigest) {
    return {OperationReplayAction::reject_reused_key,
            CommandResult::rejected(CommandErrorCode::idempotency_key_reused)};
  }
  return {OperationReplayAction::replay, recordedOperation->result};
}

} // namespace pros::domain
