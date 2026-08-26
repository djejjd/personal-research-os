#include "pros/infrastructure/sqlite_command_transaction.h"

#include <QStringDecoder>

#include <sqlite3.h>

#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace pros::infrastructure {
namespace {

using domain::CommandErrorCode;
using domain::CommandResult;
using domain::OperationReplayAction;
using domain::RecordedOperation;
using domain::Revision;

struct DatabaseCloser final {
  void operator()(sqlite3 *database) const { sqlite3_close(database); }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

class Transaction final {
public:
  explicit Transaction(sqlite3 *database)
      : database_(database),
        active_(sqlite3_exec(database, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK) {}
  ~Transaction() {
    if (active_)
      sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
  }

  [[nodiscard]] bool isActive() const { return active_; }
  [[nodiscard]] bool commit() {
    if (!active_ || sqlite3_exec(database_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
      return false;
    active_ = false;
    return true;
  }

private:
  sqlite3 *database_;
  bool active_;
};

void setStorageError(QString *errorMessage) {
  if (errorMessage != nullptr)
    *errorMessage = "本地命令事务失败";
}

bool bindText(sqlite3_stmt *statement, int index, const std::string &value) {
  return sqlite3_bind_text64(statement, index, value.data(), static_cast<sqlite3_uint64>(value.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}

bool validStoredText(std::string_view value) {
  if (value.empty() || value.find('\0') != std::string_view::npos)
    return false;
  QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
  const QString decoded = decoder.decode(QByteArrayView(value.data(), static_cast<qsizetype>(value.size())));
  return !decoded.isNull() && !decoder.hasError();
}

std::optional<std::string> textColumn(sqlite3_stmt *statement, int column) {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
    return std::nullopt;
  const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
  const int size = sqlite3_column_bytes(statement, column);
  if (value == nullptr || size < 0)
    return std::nullopt;
  std::string text(value, static_cast<std::size_t>(size));
  return validStoredText(text) ? std::optional<std::string>(std::move(text)) : std::nullopt;
}

const char *encodeErrorCode(CommandErrorCode errorCode) {
  switch (errorCode) {
  case CommandErrorCode::invalid_argument:
    return "invalid_argument";
  case CommandErrorCode::revision_conflict:
    return "revision_conflict";
  case CommandErrorCode::idempotency_key_reused:
    return "idempotency_key_reused";
  case CommandErrorCode::unsupported_in_version:
    return "unsupported_in_version";
  case CommandErrorCode::storage_unavailable:
    return "storage_unavailable";
  }
  throw std::invalid_argument("unknown command error code");
}

std::optional<CommandErrorCode> decodeErrorCode(std::string_view code) {
  if (code == "invalid_argument")
    return CommandErrorCode::invalid_argument;
  if (code == "revision_conflict")
    return CommandErrorCode::revision_conflict;
  if (code == "idempotency_key_reused")
    return CommandErrorCode::idempotency_key_reused;
  if (code == "unsupported_in_version")
    return CommandErrorCode::unsupported_in_version;
  if (code == "storage_unavailable")
    return CommandErrorCode::storage_unavailable;
  return std::nullopt;
}

bool readOperation(sqlite3 *database, const domain::OperationKey &key, std::optional<RecordedOperation> *recorded) {
  sqlite3_stmt *rawStatement = nullptr;
  constexpr const char *sql = "SELECT request_digest, succeeded, aggregate_id, revision, error_code "
                              "FROM operation_records WHERE caller_id = ? AND operation_id = ?;";
  if (sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  if (!bindText(statement.get(), 1, key.callerId()) || !bindText(statement.get(), 2, key.operationId()))
    return false;

  const int step = sqlite3_step(statement.get());
  if (step == SQLITE_DONE) {
    *recorded = std::nullopt;
    return true;
  }
  if (step != SQLITE_ROW)
    return false;

  if (sqlite3_column_type(statement.get(), 1) != SQLITE_INTEGER)
    return false;
  const auto digest = textColumn(statement.get(), 0);
  const int succeeded = sqlite3_column_int(statement.get(), 1);
  if (!digest || (succeeded != 0 && succeeded != 1))
    return false;

  if (succeeded == 1) {
    const auto aggregateId = textColumn(statement.get(), 2);
    if (!aggregateId || sqlite3_column_type(statement.get(), 3) != SQLITE_INTEGER ||
        sqlite3_column_int64(statement.get(), 3) < 0 || sqlite3_column_type(statement.get(), 4) != SQLITE_NULL)
      return false;
    *recorded = RecordedOperation{
        key, *digest, CommandResult::succeeded(*aggregateId, Revision(sqlite3_column_int64(statement.get(), 3)))};
    return true;
  }

  const auto storedErrorCode = textColumn(statement.get(), 4);
  const auto errorCode = storedErrorCode ? decodeErrorCode(*storedErrorCode) : std::nullopt;
  if (sqlite3_column_type(statement.get(), 2) != SQLITE_NULL ||
      sqlite3_column_type(statement.get(), 3) != SQLITE_NULL || !errorCode.has_value())
    return false;
  if (*errorCode == CommandErrorCode::storage_unavailable)
    return false;
  *recorded = RecordedOperation{key, *digest, CommandResult::rejected(*errorCode)};
  return true;
}

bool insertOperation(sqlite3 *database, const domain::OperationKey &key, const std::string &digest,
                     const CommandResult &result) {
  sqlite3_stmt *rawStatement = nullptr;
  constexpr const char *sql =
      "INSERT INTO operation_records "
      "(caller_id, operation_id, request_digest, succeeded, aggregate_id, revision, error_code) "
      "VALUES (?, ?, ?, ?, ?, ?, ?);";
  if (sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  const bool succeeded = result.isSuccess();
  if (!bindText(statement.get(), 1, key.callerId()) || !bindText(statement.get(), 2, key.operationId()) ||
      !bindText(statement.get(), 3, digest) || sqlite3_bind_int(statement.get(), 4, succeeded ? 1 : 0) != SQLITE_OK)
    return false;

  if (succeeded) {
    const auto &success = result.success();
    if (!success)
      return false;
    if (!bindText(statement.get(), 5, success->aggregateId) ||
        sqlite3_bind_int64(statement.get(), 6, success->revision.value()) != SQLITE_OK ||
        sqlite3_bind_null(statement.get(), 7) != SQLITE_OK)
      return false;
  } else {
    const auto &errorCode = result.errorCode();
    if (!errorCode)
      return false;
    if (sqlite3_bind_null(statement.get(), 5) != SQLITE_OK || sqlite3_bind_null(statement.get(), 6) != SQLITE_OK ||
        !bindText(statement.get(), 7, encodeErrorCode(*errorCode)))
      return false;
  }
  return sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool allocatePositions(sqlite3 *database, std::size_t count, sqlite3_int64 *firstPosition) {
  if (count == 0) {
    *firstPosition = 0;
    return true;
  }
  sqlite3_stmt *rawPosition = nullptr;
  constexpr const char *positionSql =
      "SELECT next_position FROM delivery_sequence WHERE delivery_partition = 'global';";
  if (sqlite3_prepare_v2(database, positionSql, -1, &rawPosition, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> position(rawPosition, sqlite3_finalize);
  if (sqlite3_step(position.get()) != SQLITE_ROW)
    return false;
  if (sqlite3_column_type(position.get(), 0) != SQLITE_INTEGER)
    return false;
  const sqlite3_int64 first = sqlite3_column_int64(position.get(), 0);
  if (sqlite3_step(position.get()) != SQLITE_DONE || first <= 0 ||
      count > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max() - first))
    return false;
  position.reset();

  sqlite3_stmt *rawUpdate = nullptr;
  constexpr const char *updateSql = "UPDATE delivery_sequence SET next_position = ? "
                                    "WHERE delivery_partition = 'global' AND next_position = ?;";
  if (sqlite3_prepare_v2(database, updateSql, -1, &rawUpdate, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> update(rawUpdate, sqlite3_finalize);
  const sqlite3_int64 next = first + static_cast<sqlite3_int64>(count);
  if (sqlite3_bind_int64(update.get(), 1, next) != SQLITE_OK ||
      sqlite3_bind_int64(update.get(), 2, first) != SQLITE_OK || sqlite3_step(update.get()) != SQLITE_DONE ||
      sqlite3_changes(database) != 1)
    return false;
  *firstPosition = first;
  return true;
}

bool insertFact(sqlite3 *database, const domain::OperationKey &key, const CommandFact &fact,
                sqlite3_int64 deliveryPosition) {

  sqlite3_stmt *rawEvent = nullptr;
  constexpr const char *eventSql =
      "INSERT INTO domain_events "
      "(event_id, delivery_partition, delivery_position, event_type, aggregate_type, aggregate_id, "
      "aggregate_revision, event_index, schema_version, caller_id, operation_id, payload) "
      "VALUES (?, 'global', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
  if (sqlite3_prepare_v2(database, eventSql, -1, &rawEvent, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> event(rawEvent, sqlite3_finalize);
  const bool eventBound =
      bindText(event.get(), 1, fact.eventId) && sqlite3_bind_int64(event.get(), 2, deliveryPosition) == SQLITE_OK &&
      bindText(event.get(), 3, fact.eventType) && bindText(event.get(), 4, fact.aggregateType) &&
      bindText(event.get(), 5, fact.aggregateId) &&
      sqlite3_bind_int64(event.get(), 6, fact.revision.value()) == SQLITE_OK &&
      sqlite3_bind_int(event.get(), 7, fact.eventIndex) == SQLITE_OK &&
      sqlite3_bind_int(event.get(), 8, fact.schemaVersion) == SQLITE_OK && bindText(event.get(), 9, key.callerId()) &&
      bindText(event.get(), 10, key.operationId()) && bindText(event.get(), 11, fact.payload);
  if (!eventBound || sqlite3_step(event.get()) != SQLITE_DONE)
    return false;

  sqlite3_stmt *rawOutbox = nullptr;
  constexpr const char *outboxSql =
      "INSERT INTO outbox_records (event_id, delivery_partition, delivery_position, delivery_state) "
      "VALUES (?, 'global', ?, 'pending');";
  if (sqlite3_prepare_v2(database, outboxSql, -1, &rawOutbox, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> outbox(rawOutbox, sqlite3_finalize);
  const bool outboxBound =
      bindText(outbox.get(), 1, fact.eventId) && sqlite3_bind_int64(outbox.get(), 2, deliveryPosition) == SQLITE_OK;
  if (!outboxBound || sqlite3_step(outbox.get()) != SQLITE_DONE)
    return false;

  sqlite3_stmt *rawActivity = nullptr;
  constexpr const char *activitySql = "INSERT INTO activity_facts "
                                      "(event_id, kind, aggregate_id, revision, caller_id, operation_id, summary) "
                                      "VALUES (?, ?, ?, ?, ?, ?, ?);";
  if (sqlite3_prepare_v2(database, activitySql, -1, &rawActivity, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> activity(rawActivity, sqlite3_finalize);
  const bool activityBound =
      bindText(activity.get(), 1, fact.eventId) && bindText(activity.get(), 2, fact.activityKind) &&
      bindText(activity.get(), 3, fact.aggregateId) &&
      sqlite3_bind_int64(activity.get(), 4, fact.revision.value()) == SQLITE_OK &&
      bindText(activity.get(), 5, key.callerId()) && bindText(activity.get(), 6, key.operationId()) &&
      bindText(activity.get(), 7, fact.activitySummary);
  return activityBound && sqlite3_step(activity.get()) == SQLITE_DONE;
}

bool validFacts(const CommandResult &result, const std::vector<CommandFact> &facts) {
  if (result.errorCode() == std::optional(CommandErrorCode::storage_unavailable))
    return false;
  if (result.isSuccess() && facts.empty())
    return false;
  if (facts.empty())
    return true;
  if (facts.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return false;
  std::set<std::string> eventIds;
  const CommandFact &first = facts.front();
  for (std::size_t index = 0; index < facts.size(); ++index) {
    const CommandFact &fact = facts[index];
    if (fact.eventId.empty() || fact.eventType.empty() || fact.aggregateType.empty() || fact.aggregateId.empty() ||
        fact.payload.empty() || fact.activityKind.empty() || fact.activitySummary.empty() ||
        fact.eventIndex != static_cast<int>(index) || fact.schemaVersion <= 0 ||
        fact.aggregateType != first.aggregateType || fact.aggregateId != first.aggregateId ||
        fact.revision != first.revision || !eventIds.insert(fact.eventId).second)
      return false;
  }
  if (!result.isSuccess())
    return true;
  const auto &success = result.success();
  return success && first.aggregateId == success->aggregateId && first.revision == success->revision;
}

} // namespace

CommandWorkResult CommandWorkResult::completed(CommandResult result, std::vector<CommandFact> facts) {
  return {std::move(result), std::move(facts), true};
}

CommandWorkResult CommandWorkResult::storageFailure() {
  return {CommandResult::rejected(CommandErrorCode::invalid_argument), {}, false};
}

SqliteCommandTransaction::SqliteCommandTransaction(QString databasePath) : databasePath_(std::move(databasePath)) {}

std::optional<CommandResult> SqliteCommandTransaction::execute(const domain::OperationKey &key,
                                                               const std::string &requestDigest, const Work &work,
                                                               QString *errorMessage) const {
  if (!validStoredText(key.callerId()) || !validStoredText(key.operationId()) || !validStoredText(requestDigest) ||
      !work) {
    setStorageError(errorMessage);
    return std::nullopt;
  }
  sqlite3 *rawDatabase = nullptr;
  const QByteArray encodedPath = databasePath_.toUtf8();
  if (sqlite3_open_v2(encodedPath.constData(), &rawDatabase, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    setStorageError(errorMessage);
    if (rawDatabase != nullptr)
      sqlite3_close(rawDatabase);
    return std::nullopt;
  }
  Database database(rawDatabase);
  if (sqlite3_busy_timeout(database.get(), 5000) != SQLITE_OK ||
      sqlite3_exec(database.get(), "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    setStorageError(errorMessage);
    return std::nullopt;
  }
  Transaction transaction(database.get());
  if (!transaction.isActive()) {
    setStorageError(errorMessage);
    return std::nullopt;
  }

  try {
    std::optional<RecordedOperation> recorded;
    if (!readOperation(database.get(), key, &recorded)) {
      setStorageError(errorMessage);
      return std::nullopt;
    }
    const domain::OperationReplayDecision decision = domain::decideOperationReplay(key, requestDigest, recorded);
    if (decision.action != OperationReplayAction::execute) {
      if (!transaction.commit()) {
        setStorageError(errorMessage);
        return std::nullopt;
      }
      return decision.result;
    }

    CommandWorkResult workResult = work(database.get());
    if (!workResult.storageSucceeded || !validFacts(workResult.result, workResult.facts)) {
      setStorageError(errorMessage);
      return std::nullopt;
    }
    sqlite3_int64 firstPosition = 0;
    if (!allocatePositions(database.get(), workResult.facts.size(), &firstPosition)) {
      setStorageError(errorMessage);
      return std::nullopt;
    }
    for (std::size_t index = 0; index < workResult.facts.size(); ++index) {
      if (!insertFact(database.get(), key, workResult.facts[index],
                      firstPosition + static_cast<sqlite3_int64>(index))) {
        setStorageError(errorMessage);
        return std::nullopt;
      }
    }
    if (!insertOperation(database.get(), key, requestDigest, workResult.result) || !transaction.commit()) {
      setStorageError(errorMessage);
      return std::nullopt;
    }
    return workResult.result;
  } catch (const std::exception &) {
    setStorageError(errorMessage);
    return std::nullopt;
  } catch (...) {
    setStorageError(errorMessage);
    return std::nullopt;
  }
}

} // namespace pros::infrastructure
