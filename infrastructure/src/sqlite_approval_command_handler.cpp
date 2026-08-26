#include "pros/infrastructure/sqlite_approval_command_handler.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>

#include <sqlite3.h>

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace pros::infrastructure {
namespace {

using domain::CommandErrorCode;
using domain::CommandResult;
using domain::Revision;

bool bindText(sqlite3_stmt *statement, int index, const std::string &value) {
  return sqlite3_bind_text(statement, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

std::string stableDigest(std::initializer_list<std::string_view> fields) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData("pros-command-v1\n");
  for (const std::string_view field : fields) {
    const QByteArray size = QByteArray::number(static_cast<qsizetype>(field.size()));
    hash.addData(size);
    hash.addData(":");
    hash.addData(QByteArrayView(field.data(), static_cast<qsizetype>(field.size())));
    hash.addData("\n");
  }
  return hash.result().toHex().toStdString();
}

std::string revisionText(Revision revision) { return std::to_string(revision.value()); }

std::string eventId(const domain::OperationKey &key, std::string_view eventType) {
  return stableDigest({"event", key.callerId(), key.operationId(), eventType});
}

std::string dispatchAuditId(const domain::OperationKey &key) {
  return stableDigest({"dispatch-audit", key.callerId(), key.operationId()});
}

bool validText(std::string_view value, bool allowEmpty = false) {
  if ((!allowEmpty && value.empty()) || value.find('\0') != std::string_view::npos)
    return false;
  QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
  const QString decoded = decoder.decode(QByteArrayView(value.data(), static_cast<qsizetype>(value.size())));
  return !decoded.isNull() && !decoder.hasError();
}

bool validOperationKey(const domain::OperationKey &key) {
  return validText(key.callerId()) && validText(key.operationId());
}

bool validDecision(domain::ApprovalDecision decision) {
  return decision == domain::ApprovalDecision::pending || decision == domain::ApprovalDecision::approved ||
         decision == domain::ApprovalDecision::rejected;
}

std::string dispatchPayload(const application::DispatchOperationPlan &command) {
  const QJsonObject payload{{"result", "unsupported_in_version"},
                            {"plan_id", QString::fromUtf8(command.planId)},
                            {"expected_revision", command.expectedRevision.value()}};
  return QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString();
}

CommandFact fact(const domain::OperationKey &key, std::string eventType, std::string aggregateType,
                 std::string aggregateId, Revision revision, std::string payload, std::string summary) {
  return {
      eventId(key, eventType), std::move(eventType), std::move(aggregateType), std::move(aggregateId), revision, 0, 1,
      std::move(payload),      "approval",           std::move(summary)};
}

struct PlanLookup final {
  bool storageSucceeded;
  std::optional<Revision> revision;
  std::string digest;
};

PlanLookup findPlan(sqlite3 *database, const std::string &planId) {
  sqlite3_stmt *raw = nullptr;
  if (sqlite3_prepare_v2(database, "SELECT revision, plan_digest FROM operation_plans WHERE id = ?;", -1, &raw,
                         nullptr) != SQLITE_OK)
    return {false, std::nullopt, {}};
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (!bindText(statement.get(), 1, planId))
    return {false, std::nullopt, {}};
  const int step = sqlite3_step(statement.get());
  if (step == SQLITE_DONE)
    return {true, std::nullopt, {}};
  if (step != SQLITE_ROW || sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
      sqlite3_column_type(statement.get(), 1) != SQLITE_TEXT)
    return {false, std::nullopt, {}};
  const auto *storedDigest = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 1));
  const int digestSize = sqlite3_column_bytes(statement.get(), 1);
  if (storedDigest == nullptr || digestSize <= 0)
    return {false, std::nullopt, {}};
  std::string digest(storedDigest, static_cast<std::size_t>(digestSize));
  if (!validText(digest))
    return {false, std::nullopt, {}};
  return {true, Revision(sqlite3_column_int64(statement.get(), 0)), std::move(digest)};
}

CommandWorkResult rejected(CommandErrorCode code) {
  return CommandWorkResult::completed(CommandResult::rejected(code));
}

CommandWorkResult insertFailure(sqlite3 *database) {
  const int errorCode = sqlite3_extended_errcode(database);
  if (errorCode == SQLITE_CONSTRAINT_PRIMARYKEY || errorCode == SQLITE_CONSTRAINT_UNIQUE)
    return rejected(CommandErrorCode::invalid_argument);
  return CommandWorkResult::storageFailure();
}

} // namespace

SqliteApprovalCommandHandler::SqliteApprovalCommandHandler(QString databasePath)
    : transaction_(std::move(databasePath)) {}

CommandResult SqliteApprovalCommandHandler::createOperationPlan(const application::CreateOperationPlan &command) {
  if (!validOperationKey(command.operationKey))
    return CommandResult::rejected(CommandErrorCode::invalid_argument);
  const std::string expected = revisionText(command.expectedRevision);
  const std::string digest = stableDigest({"CreateOperationPlan", command.planId, command.summary, expected});
  QString error;
  const auto result = transaction_.execute(
      command.operationKey, digest,
      [&command, &digest](sqlite3 *database) {
        if (!validText(command.planId) || !validText(command.summary) || command.expectedRevision.value() != 0)
          return rejected(CommandErrorCode::invalid_argument);
        sqlite3_stmt *raw = nullptr;
        if (sqlite3_prepare_v2(database,
                               "INSERT INTO operation_plans(id,summary,plan_digest,revision) VALUES(?,?,?,1);", -1,
                               &raw, nullptr) != SQLITE_OK)
          return CommandWorkResult::storageFailure();
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
        if (!bindText(statement.get(), 1, command.planId) || !bindText(statement.get(), 2, command.summary) ||
            !bindText(statement.get(), 3, digest))
          return CommandWorkResult::storageFailure();
        const int step = sqlite3_step(statement.get());
        if (step != SQLITE_DONE)
          return insertFailure(database);
        const Revision revision(1);
        return CommandWorkResult::completed(
            CommandResult::succeeded(command.planId, revision),
            {fact(command.operationKey, "OperationPlanCreated", "OperationPlan", command.planId, revision,
                  "operation_plan_created", "操作计划已保存")});
      },
      &error);
  return result.value_or(CommandResult::rejected(CommandErrorCode::storage_unavailable));
}

CommandResult SqliteApprovalCommandHandler::recordApproval(const application::RecordApproval &command) {
  if (!validOperationKey(command.operationKey))
    return CommandResult::rejected(CommandErrorCode::invalid_argument);
  const std::string expected = revisionText(command.expectedRevision);
  const std::string planRevisionValue = revisionText(command.planRevision);
  const std::string decision = std::to_string(static_cast<int>(command.decision));
  const std::string digest = stableDigest({"RecordApproval", command.approvalId, command.planId, planRevisionValue,
                                           command.planDigest, decision, command.note, expected});
  QString error;
  const auto result = transaction_.execute(
      command.operationKey, digest,
      [&command](sqlite3 *database) {
        if (!validText(command.approvalId) || !validText(command.planId) || !validText(command.planDigest) ||
            !validText(command.note, true) || !validDecision(command.decision) || command.expectedRevision.value() != 0)
          return rejected(CommandErrorCode::invalid_argument);
        const PlanLookup plan = findPlan(database, command.planId);
        if (!plan.storageSucceeded)
          return CommandWorkResult::storageFailure();
        if (!plan.revision.has_value())
          return rejected(CommandErrorCode::invalid_argument);
        if (*plan.revision != command.planRevision || plan.digest != command.planDigest)
          return rejected(CommandErrorCode::revision_conflict);
        sqlite3_stmt *raw = nullptr;
        constexpr const char *sql =
            "INSERT INTO approvals"
            "(id,plan_id,plan_revision,plan_digest,decision,note,revision) VALUES(?,?,?,?,?,?,1);";
        if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
          return CommandWorkResult::storageFailure();
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
        if (!bindText(statement.get(), 1, command.approvalId) || !bindText(statement.get(), 2, command.planId) ||
            sqlite3_bind_int64(statement.get(), 3, command.planRevision.value()) != SQLITE_OK ||
            !bindText(statement.get(), 4, command.planDigest) ||
            sqlite3_bind_int(statement.get(), 5, static_cast<int>(command.decision)) != SQLITE_OK ||
            !bindText(statement.get(), 6, command.note))
          return CommandWorkResult::storageFailure();
        const int step = sqlite3_step(statement.get());
        if (step != SQLITE_DONE)
          return insertFailure(database);
        const Revision revision(1);
        return CommandWorkResult::completed(
            CommandResult::succeeded(command.approvalId, revision),
            {fact(command.operationKey, "ApprovalRecorded", "Approval", command.approvalId, revision,
                  "approval_recorded", "审批决定已保存")});
      },
      &error);
  return result.value_or(CommandResult::rejected(CommandErrorCode::storage_unavailable));
}

CommandResult SqliteApprovalCommandHandler::dispatchOperationPlan(const application::DispatchOperationPlan &command) {
  if (!validOperationKey(command.operationKey) || !validText(command.planId, true))
    return CommandResult::rejected(CommandErrorCode::invalid_argument);
  const std::string expected = revisionText(command.expectedRevision);
  const std::string digest = stableDigest({"DispatchOperationPlan", command.planId, expected});
  QString error;
  const auto result = transaction_.execute(
      command.operationKey, digest,
      [&command](sqlite3 *) {
        return CommandWorkResult::completed(
            CommandResult::rejected(CommandErrorCode::unsupported_in_version),
            {fact(command.operationKey, "OperationDispatchRefused", "DispatchAudit",
                  dispatchAuditId(command.operationKey), Revision(1), dispatchPayload(command), "当前版本不支持派发")});
      },
      &error);
  return result.value_or(CommandResult::rejected(CommandErrorCode::storage_unavailable));
}

} // namespace pros::infrastructure
