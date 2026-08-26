#include "pros/infrastructure/sqlite_work_command_handler.h"

#include "pros/infrastructure/sqlite_command_transaction.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace pros::infrastructure {
namespace {

using application::WorkCommandEnvelope;
using domain::CommandErrorCode;
using domain::CommandResult;
using domain::Revision;

enum class WriteState : std::uint8_t { succeeded, rejected, storageFailure };

struct WriteResult final {
  WriteState state;
  Revision revision;
};

bool bindText(sqlite3_stmt *statement, int index, const std::string &value) {
  return sqlite3_bind_text64(statement, index, value.data(), static_cast<sqlite3_uint64>(value.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}

WriteResult insertResult(sqlite3 *database, int step) {
  if (step == SQLITE_DONE && sqlite3_changes(database) == 1)
    return {WriteState::succeeded, Revision(1)};
  const int code = sqlite3_extended_errcode(database);
  if (code == SQLITE_CONSTRAINT_PRIMARYKEY || code == SQLITE_CONSTRAINT_UNIQUE ||
      code == SQLITE_CONSTRAINT_FOREIGNKEY || code == SQLITE_CONSTRAINT_CHECK)
    return {WriteState::rejected, Revision(0)};
  return {WriteState::storageFailure, Revision(0)};
}

/** 仅在调用方持有的共享事务连接中写工作表；不拥有连接，也不管理 schema、事务或共享事实。 */
class WorkStore final {
public:
  explicit WorkStore(sqlite3 *database) : database_(database) {}

  [[nodiscard]] WriteResult createProject(const std::string &id, const std::string &title) const {
    return insertRoot("INSERT INTO projects (id, title, status, revision) VALUES (?, ?, 0, 1);", id, title);
  }

  [[nodiscard]] WriteResult createTask(const std::string &id, const std::string &projectId,
                                       const std::string &title) const {
    return insertChild("INSERT INTO tasks (id, project_id, title, status, revision) VALUES (?, ?, ?, 0, 1);", id,
                       projectId, title);
  }

  [[nodiscard]] WriteResult updateTask(const std::string &id, domain::TaskStatus status,
                                       Revision expectedRevision) const {
    sqlite3_stmt *rawStatement = nullptr;
    constexpr const char *sql = "UPDATE tasks SET status = ?, revision = revision + 1 WHERE id = ? AND revision = ?;";
    if (sqlite3_prepare_v2(database_, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
      return {WriteState::storageFailure, Revision(0)};
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
    const bool bound = sqlite3_bind_int(statement.get(), 1, static_cast<int>(status)) == SQLITE_OK &&
                       bindText(statement.get(), 2, id) &&
                       sqlite3_bind_int64(statement.get(), 3, expectedRevision.value()) == SQLITE_OK;
    if (!bound || sqlite3_step(statement.get()) != SQLITE_DONE)
      return {WriteState::storageFailure, Revision(0)};
    if (sqlite3_changes(database_) != 1)
      return {WriteState::rejected, Revision(0)};
    return {WriteState::succeeded, Revision(expectedRevision.value() + 1)};
  }

  [[nodiscard]] WriteResult createMilestone(const std::string &id, const std::string &projectId,
                                            const std::string &title) const {
    return insertChild("INSERT INTO milestones (id, project_id, title, status, revision) VALUES (?, ?, ?, 0, 1);", id,
                       projectId, title);
  }

  [[nodiscard]] WriteResult createDirection(const std::string &id, const std::string &title) const {
    return insertRoot("INSERT INTO directions (id, title, status, revision) VALUES (?, ?, 0, 1);", id, title);
  }

private:
  [[nodiscard]] WriteResult insertRoot(const char *sql, const std::string &id, const std::string &title) const {
    sqlite3_stmt *rawStatement = nullptr;
    if (sqlite3_prepare_v2(database_, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
      return {WriteState::storageFailure, Revision(0)};
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
    if (!bindText(statement.get(), 1, id) || !bindText(statement.get(), 2, title))
      return {WriteState::storageFailure, Revision(0)};
    return insertResult(database_, sqlite3_step(statement.get()));
  }

  [[nodiscard]] WriteResult insertChild(const char *sql, const std::string &id, const std::string &parentId,
                                        const std::string &title) const {
    sqlite3_stmt *rawStatement = nullptr;
    if (sqlite3_prepare_v2(database_, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
      return {WriteState::storageFailure, Revision(0)};
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
    if (!bindText(statement.get(), 1, id) || !bindText(statement.get(), 2, parentId) ||
        !bindText(statement.get(), 3, title))
      return {WriteState::storageFailure, Revision(0)};
    return insertResult(database_, sqlite3_step(statement.get()));
  }

  sqlite3 *database_;
};

void appendField(QByteArray *encoded, std::string_view value) {
  encoded->append(QByteArray::number(static_cast<qsizetype>(value.size())));
  encoded->append(':');
  encoded->append(value.data(), static_cast<qsizetype>(value.size()));
  encoded->append(';');
}

std::string digest(std::string_view commandType, const WorkCommandEnvelope &envelope,
                   const std::vector<std::string_view> &fields) {
  QByteArray encoded("pros.work-command.v1;");
  appendField(&encoded, commandType);
  appendField(&encoded, std::to_string(envelope.expectedRevision.value()));
  for (const std::string_view field : fields)
    appendField(&encoded, field);
  return QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex().toStdString();
}

std::string eventId(const domain::OperationKey &key, std::string_view commandDigest) {
  QByteArray encoded("pros.work-event.v1;");
  appendField(&encoded, key.callerId());
  appendField(&encoded, key.operationId());
  appendField(&encoded, commandDigest);
  return "evt-" + QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex().toStdString();
}

std::string payload(const QJsonObject &object) {
  return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

CommandFact fact(const WorkCommandEnvelope &envelope, const std::string &commandDigest, std::string eventType,
                 std::string aggregateType, const std::string &aggregateId, Revision revision, std::string body,
                 std::string summary) {
  return {eventId(envelope.operation, commandDigest),
          std::move(eventType),
          std::move(aggregateType),
          aggregateId,
          revision,
          0,
          1,
          std::move(body),
          "work.changed",
          std::move(summary)};
}

bool validText(std::string_view value) {
  if (value.empty() || value.find('\0') != std::string_view::npos)
    return false;
  QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
  const QString decoded = decoder.decode(QByteArrayView(value.data(), static_cast<qsizetype>(value.size())));
  return !decoded.isNull() && !decoder.hasError();
}

bool validEnvelope(const WorkCommandEnvelope &envelope) {
  return validText(envelope.operation.callerId()) && validText(envelope.operation.operationId());
}

bool validCreate(const WorkCommandEnvelope &envelope, const std::initializer_list<std::string_view> &fields) {
  if (envelope.expectedRevision != Revision(0))
    return false;
  for (const auto field : fields) {
    if (!validText(field))
      return false;
  }
  return true;
}

CommandWorkResult rejected(CommandErrorCode code) {
  return CommandWorkResult::completed(CommandResult::rejected(code));
}

CommandWorkResult completedWrite(const WriteResult &write, std::string aggregateId, CommandFact commandFact) {
  if (write.state == WriteState::storageFailure)
    return CommandWorkResult::storageFailure();
  if (write.state == WriteState::rejected)
    return rejected(CommandErrorCode::invalid_argument);
  return CommandWorkResult::completed(CommandResult::succeeded(std::move(aggregateId), write.revision),
                                      {std::move(commandFact)});
}

CommandResult storageUnavailable() { return CommandResult::rejected(CommandErrorCode::storage_unavailable); }

CommandResult publicResult(const std::optional<CommandResult> &result) { return result.value_or(storageUnavailable()); }

} // namespace

SqliteWorkCommandHandler::SqliteWorkCommandHandler(QString databasePath) : databasePath_(std::move(databasePath)) {}

CommandResult SqliteWorkCommandHandler::handle(const application::CreateProject &command, QString *errorMessage) const {
  if (!validEnvelope(command.envelope))
    return CommandResult::rejected(CommandErrorCode::invalid_argument);
  const std::string commandDigest = digest("CreateProject", command.envelope, {command.projectId, command.title});
  SqliteCommandTransaction transaction(databasePath_);
  return publicResult(transaction.execute(
      command.envelope.operation, commandDigest,
      [&](sqlite3 *database) {
        if (!validCreate(command.envelope, {command.projectId, command.title}))
          return rejected(CommandErrorCode::invalid_argument);
        const WriteResult write = WorkStore(database).createProject(command.projectId, command.title);
        return completedWrite(write, command.projectId,
                              fact(command.envelope, commandDigest, "project.created", "project", command.projectId,
                                   write.revision,
                                   payload({{"project_id", QString::fromStdString(command.projectId)},
                                            {"title", QString::fromStdString(command.title)}}),
                                   "创建项目"));
      },
      errorMessage));
}

CommandResult SqliteWorkCommandHandler::handle(const application::CreateTask &command, QString *errorMessage) const {
  if (!validEnvelope(command.envelope))
    return CommandResult::rejected(CommandErrorCode::invalid_argument);
  const std::string commandDigest =
      digest("CreateTask", command.envelope, {command.taskId, command.projectId, command.title});
  SqliteCommandTransaction transaction(databasePath_);
  return publicResult(transaction.execute(
      command.envelope.operation, commandDigest,
      [&](sqlite3 *database) {
        if (!validCreate(command.envelope, {command.taskId, command.projectId, command.title}))
          return rejected(CommandErrorCode::invalid_argument);
        const WriteResult write = WorkStore(database).createTask(command.taskId, command.projectId, command.title);
        return completedWrite(write, command.taskId,
                              fact(command.envelope, commandDigest, "task.created", "task", command.taskId,
                                   write.revision,
                                   payload({{"task_id", QString::fromStdString(command.taskId)},
                                            {"project_id", QString::fromStdString(command.projectId)},
                                            {"title", QString::fromStdString(command.title)}}),
                                   "创建任务"));
      },
      errorMessage));
}

CommandResult SqliteWorkCommandHandler::handle(const application::UpdateTask &command, QString *errorMessage) const {
  if (!validEnvelope(command.envelope))
    return CommandResult::rejected(CommandErrorCode::invalid_argument);
  const std::string status = std::to_string(static_cast<int>(command.status));
  const std::string commandDigest = digest("UpdateTask", command.envelope, {command.taskId, status});
  SqliteCommandTransaction transaction(databasePath_);
  return publicResult(transaction.execute(
      command.envelope.operation, commandDigest,
      [&](sqlite3 *database) {
        if (!validText(command.taskId) ||
            (command.status != domain::TaskStatus::open && command.status != domain::TaskStatus::completed))
          return rejected(CommandErrorCode::invalid_argument);
        const WriteResult write =
            WorkStore(database).updateTask(command.taskId, command.status, command.envelope.expectedRevision);
        if (write.state == WriteState::storageFailure)
          return CommandWorkResult::storageFailure();
        if (write.state == WriteState::rejected)
          return rejected(CommandErrorCode::revision_conflict);
        return completedWrite(write, command.taskId,
                              fact(command.envelope, commandDigest, "task.updated", "task", command.taskId,
                                   write.revision,
                                   payload({{"task_id", QString::fromStdString(command.taskId)},
                                            {"status", static_cast<int>(command.status)}}),
                                   "更新任务状态"));
      },
      errorMessage));
}

CommandResult SqliteWorkCommandHandler::handle(const application::CreateMilestone &command,
                                               QString *errorMessage) const {
  if (!validEnvelope(command.envelope))
    return CommandResult::rejected(CommandErrorCode::invalid_argument);
  const std::string commandDigest =
      digest("CreateMilestone", command.envelope, {command.milestoneId, command.projectId, command.title});
  SqliteCommandTransaction transaction(databasePath_);
  return publicResult(transaction.execute(
      command.envelope.operation, commandDigest,
      [&](sqlite3 *database) {
        if (!validCreate(command.envelope, {command.milestoneId, command.projectId, command.title}))
          return rejected(CommandErrorCode::invalid_argument);
        const WriteResult write =
            WorkStore(database).createMilestone(command.milestoneId, command.projectId, command.title);
        return completedWrite(write, command.milestoneId,
                              fact(command.envelope, commandDigest, "milestone.created", "milestone",
                                   command.milestoneId, write.revision,
                                   payload({{"milestone_id", QString::fromStdString(command.milestoneId)},
                                            {"project_id", QString::fromStdString(command.projectId)},
                                            {"title", QString::fromStdString(command.title)}}),
                                   "创建里程碑"));
      },
      errorMessage));
}

CommandResult SqliteWorkCommandHandler::handle(const application::CreateDirection &command,
                                               QString *errorMessage) const {
  if (!validEnvelope(command.envelope))
    return CommandResult::rejected(CommandErrorCode::invalid_argument);
  const std::string commandDigest = digest("CreateDirection", command.envelope, {command.directionId, command.title});
  SqliteCommandTransaction transaction(databasePath_);
  return publicResult(transaction.execute(
      command.envelope.operation, commandDigest,
      [&](sqlite3 *database) {
        if (!validCreate(command.envelope, {command.directionId, command.title}))
          return rejected(CommandErrorCode::invalid_argument);
        const WriteResult write = WorkStore(database).createDirection(command.directionId, command.title);
        return completedWrite(write, command.directionId,
                              fact(command.envelope, commandDigest, "direction.created", "direction",
                                   command.directionId, write.revision,
                                   payload({{"direction_id", QString::fromStdString(command.directionId)},
                                            {"title", QString::fromStdString(command.title)}}),
                                   "创建研究方向"));
      },
      errorMessage));
}

} // namespace pros::infrastructure
