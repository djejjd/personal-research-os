#include "pros/infrastructure/governance_composition.h"

#include "pros/infrastructure/sqlite_command_transaction.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <sqlite3.h>

#include <charconv>
#include <limits>
#include <memory>
#include <ranges>
#include <set>
#include <utility>

namespace pros::infrastructure {
namespace {

using application::LinkNoteToTask;
using application::RecordAcceptance;
using application::RecordEvidence;
using domain::CommandErrorCode;
using domain::CommandResult;
using domain::Revision;

struct StatementCloser final {
  void operator()(sqlite3_stmt *statement) const { sqlite3_finalize(statement); }
};
using Statement = std::unique_ptr<sqlite3_stmt, StatementCloser>;

struct DatabaseCloser final {
  void operator()(sqlite3 *database) const { sqlite3_close(database); }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

bool bindText(sqlite3_stmt *statement, int index, const std::string &value) {
  return sqlite3_bind_text64(statement, index, value.data(), static_cast<sqlite3_uint64>(value.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}

bool validText(const std::string &value, bool allowEmpty = false) {
  if ((!allowEmpty && value.empty()) || value.find('\0') != std::string::npos ||
      value.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
    return false;
  const QByteArray encoded(value.data(), static_cast<qsizetype>(value.size()));
  return QString::fromUtf8(encoded).toUtf8() == encoded;
}

bool validOperation(const domain::OperationKey &operation) {
  return validText(operation.callerId()) && validText(operation.operationId());
}

bool validConclusion(domain::AcceptanceConclusion conclusion) {
  switch (conclusion) {
  case domain::AcceptanceConclusion::passed:
  case domain::AcceptanceConclusion::failed:
  case domain::AcceptanceConclusion::inconclusive:
    return true;
  }
  return false;
}

void appendField(QByteArray &bytes, const std::string &value) {
  bytes.append(QByteArray::number(static_cast<qsizetype>(value.size())));
  bytes.append(':');
  bytes.append(value.data(), static_cast<qsizetype>(value.size()));
}

void appendRevision(QByteArray &bytes, Revision revision) {
  bytes.append(QByteArray::number(revision.value()));
  bytes.append(';');
}

std::string digest(const QByteArray &canonical) {
  return QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex().toStdString();
}

QByteArray digestPrefix(const char *commandType, const domain::OperationKey &operation, Revision expected) {
  QByteArray bytes("governance-command-v1;");
  appendField(bytes, commandType);
  appendField(bytes, operation.callerId());
  appendField(bytes, operation.operationId());
  appendRevision(bytes, expected);
  return bytes;
}

std::string requestDigest(const LinkNoteToTask &command) {
  QByteArray bytes = digestPrefix("LinkNoteToTask", command.operation, command.expectedRevision);
  appendField(bytes, command.taskId);
  appendField(bytes, command.note.documentId());
  appendField(bytes, command.note.sectionId().value_or(""));
  return digest(bytes);
}

std::string requestDigest(const RecordEvidence &command) {
  QByteArray bytes = digestPrefix("RecordEvidence", command.operation, command.expectedRevision);
  appendField(bytes, command.evidenceId);
  appendField(bytes, command.taskId);
  appendField(bytes, command.locator);
  return digest(bytes);
}

std::string requestDigest(const RecordAcceptance &command) {
  QByteArray bytes = digestPrefix("RecordAcceptance", command.operation, command.expectedRevision);
  appendField(bytes, command.acceptanceId);
  appendField(bytes, command.taskId);
  appendRevision(bytes, command.candidateRevision);
  appendRevision(bytes, command.specificationRevision);
  bytes.append(QByteArray::number(static_cast<int>(command.conclusion)));
  bytes.append(';');
  auto evidenceReferences = command.evidence;
  std::ranges::sort(evidenceReferences, {},
                    [](const auto &reference) { return std::pair{reference.evidenceId, reference.revision.value()}; });
  for (const auto &evidence : evidenceReferences) {
    appendField(bytes, evidence.evidenceId);
    appendRevision(bytes, evidence.revision);
  }
  return digest(bytes);
}

std::string eventId(const domain::OperationKey &operation, const char *eventType) {
  QByteArray bytes("governance-event-v1;");
  appendField(bytes, operation.callerId());
  appendField(bytes, operation.operationId());
  appendField(bytes, eventType);
  return "gev-" + digest(bytes);
}

std::string jsonPayload(const std::string &taskId, const char *objectKey, const std::string &objectId) {
  QJsonObject object;
  object.insert(QStringLiteral("task_id"), QString::fromStdString(taskId));
  object.insert(QString::fromLatin1(objectKey), QString::fromStdString(objectId));
  return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

CommandFact fact(const domain::OperationKey &operation, const std::string &taskId, Revision revision,
                 const char *eventType, const std::string &summary, const char *objectKey) {
  return {eventId(operation, eventType),           eventType, "GovernanceTask", taskId, revision, 0, 1,
          jsonPayload(taskId, objectKey, summary), eventType, summary};
}

/** 事务作用域治理存储；连接所有权和 BEGIN/COMMIT 均属于 SqliteCommandTransaction。 */
class GovernanceStore final {
public:
  explicit GovernanceStore(sqlite3 *database) : database_(database) {}

  [[nodiscard]] bool taskRevision(const std::string &taskId, std::optional<Revision> *revision) const {
    sqlite3_stmt *raw = nullptr;
    constexpr const char *sql = "SELECT COALESCE(g.revision, 0) FROM tasks t LEFT JOIN governance_targets g "
                                "ON g.task_id = t.id WHERE t.id = ?;";
    if (sqlite3_prepare_v2(database_, sql, -1, &raw, nullptr) != SQLITE_OK)
      return false;
    Statement statement(raw);
    if (!bindText(statement.get(), 1, taskId))
      return false;
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
      *revision = std::nullopt;
      return true;
    }
    if (step != SQLITE_ROW || sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER)
      return false;
    const sqlite3_int64 value = sqlite3_column_int64(statement.get(), 0);
    if (value < 0 || sqlite3_step(statement.get()) != SQLITE_DONE)
      return false;
    *revision = Revision(value);
    return true;
  }

  [[nodiscard]] bool noteExists(const std::string &taskId, const domain::DocumentReference &note, bool *exists) const {
    sqlite3_stmt *raw = nullptr;
    constexpr const char *sql = "SELECT 1 FROM governance_note_links WHERE task_id=? AND document_id=? AND "
                                "section_id=?;";
    if (sqlite3_prepare_v2(database_, sql, -1, &raw, nullptr) != SQLITE_OK)
      return false;
    Statement statement(raw);
    if (!bindText(statement.get(), 1, taskId) || !bindText(statement.get(), 2, note.documentId()) ||
        !bindText(statement.get(), 3, note.sectionId().value_or("")))
      return false;
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_ROW && step != SQLITE_DONE)
      return false;
    *exists = step == SQLITE_ROW;
    return true;
  }

  [[nodiscard]] bool objectExists(const char *table, const char *column, const std::string &id, bool *exists) const {
    const std::string sql = "SELECT 1 FROM " + std::string(table) + " WHERE " + column + "=?;";
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(database_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
      return false;
    Statement statement(raw);
    if (!bindText(statement.get(), 1, id))
      return false;
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_ROW && step != SQLITE_DONE)
      return false;
    *exists = step == SQLITE_ROW;
    return true;
  }

  [[nodiscard]] bool evidenceMatches(const std::string &taskId, const domain::EvidenceObservationReference &reference,
                                     bool *matches) const {
    sqlite3_stmt *raw = nullptr;
    constexpr const char *sql = "SELECT 1 FROM governance_evidence WHERE evidence_id=? AND task_id=? AND revision=?;";
    if (sqlite3_prepare_v2(database_, sql, -1, &raw, nullptr) != SQLITE_OK)
      return false;
    Statement statement(raw);
    if (!bindText(statement.get(), 1, reference.evidenceId) || !bindText(statement.get(), 2, taskId) ||
        sqlite3_bind_int64(statement.get(), 3, reference.revision.value()) != SQLITE_OK)
      return false;
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_ROW && step != SQLITE_DONE)
      return false;
    *matches = step == SQLITE_ROW;
    return true;
  }

  [[nodiscard]] bool advance(const std::string &taskId, Revision revision) const {
    sqlite3_stmt *raw = nullptr;
    constexpr const char *sql = "INSERT INTO governance_targets(task_id, revision) VALUES(?, ?) "
                                "ON CONFLICT(task_id) DO UPDATE SET revision=excluded.revision;";
    if (sqlite3_prepare_v2(database_, sql, -1, &raw, nullptr) != SQLITE_OK)
      return false;
    Statement statement(raw);
    return bindText(statement.get(), 1, taskId) &&
           sqlite3_bind_int64(statement.get(), 2, revision.value()) == SQLITE_OK &&
           sqlite3_step(statement.get()) == SQLITE_DONE;
  }

  [[nodiscard]] bool linkNote(const LinkNoteToTask &command) const {
    sqlite3_stmt *raw = nullptr;
    constexpr const char *sql = "INSERT INTO governance_note_links(task_id, document_id, section_id) VALUES(?, ?, ?);";
    if (sqlite3_prepare_v2(database_, sql, -1, &raw, nullptr) != SQLITE_OK)
      return false;
    Statement statement(raw);
    return bindText(statement.get(), 1, command.taskId) && bindText(statement.get(), 2, command.note.documentId()) &&
           bindText(statement.get(), 3, command.note.sectionId().value_or("")) &&
           sqlite3_step(statement.get()) == SQLITE_DONE;
  }

  [[nodiscard]] bool recordEvidence(const RecordEvidence &command) const {
    sqlite3_stmt *raw = nullptr;
    constexpr const char *sql = "INSERT INTO governance_evidence(evidence_id, task_id, locator, revision) "
                                "VALUES(?, ?, ?, 1);";
    if (sqlite3_prepare_v2(database_, sql, -1, &raw, nullptr) != SQLITE_OK)
      return false;
    Statement statement(raw);
    return bindText(statement.get(), 1, command.evidenceId) && bindText(statement.get(), 2, command.taskId) &&
           bindText(statement.get(), 3, command.locator) && sqlite3_step(statement.get()) == SQLITE_DONE;
  }

  [[nodiscard]] bool recordAcceptance(const RecordAcceptance &command) const {
    sqlite3_stmt *raw = nullptr;
    constexpr const char *sql = "INSERT INTO governance_acceptance(acceptance_id, task_id, candidate_revision, "
                                "spec_revision, conclusion) VALUES(?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(database_, sql, -1, &raw, nullptr) != SQLITE_OK)
      return false;
    Statement statement(raw);
    if (!bindText(statement.get(), 1, command.acceptanceId) || !bindText(statement.get(), 2, command.taskId) ||
        sqlite3_bind_int64(statement.get(), 3, command.candidateRevision.value()) != SQLITE_OK ||
        sqlite3_bind_int64(statement.get(), 4, command.specificationRevision.value()) != SQLITE_OK ||
        sqlite3_bind_int(statement.get(), 5, static_cast<int>(command.conclusion)) != SQLITE_OK ||
        sqlite3_step(statement.get()) != SQLITE_DONE)
      return false;
    for (const auto &evidence : command.evidence) {
      sqlite3_stmt *rawLink = nullptr;
      constexpr const char *linkSql = "INSERT INTO governance_acceptance_evidence"
                                      "(acceptance_id, task_id, evidence_id, evidence_revision) VALUES(?, ?, ?, ?);";
      if (sqlite3_prepare_v2(database_, linkSql, -1, &rawLink, nullptr) != SQLITE_OK)
        return false;
      Statement link(rawLink);
      if (!bindText(link.get(), 1, command.acceptanceId) || !bindText(link.get(), 2, command.taskId) ||
          !bindText(link.get(), 3, evidence.evidenceId) ||
          sqlite3_bind_int64(link.get(), 4, evidence.revision.value()) != SQLITE_OK ||
          sqlite3_step(link.get()) != SQLITE_DONE)
        return false;
    }
    return true;
  }

private:
  sqlite3 *database_;
};

CommandWorkResult rejected(CommandErrorCode code) {
  return CommandWorkResult::completed(CommandResult::rejected(code));
}

template <typename Command>
std::optional<Revision> checkedRevision(GovernanceStore &store, const Command &command,
                                        CommandWorkResult *earlyResult) {
  std::optional<Revision> actual;
  if (!store.taskRevision(command.taskId, &actual)) {
    *earlyResult = CommandWorkResult::storageFailure();
    return std::nullopt;
  }
  if (!actual || *actual != command.expectedRevision) {
    *earlyResult = rejected(actual ? CommandErrorCode::revision_conflict : CommandErrorCode::invalid_argument);
    return std::nullopt;
  }
  if (actual->value() == std::numeric_limits<std::int64_t>::max()) {
    *earlyResult = rejected(CommandErrorCode::invalid_argument);
    return std::nullopt;
  }
  return Revision(actual->value() + 1);
}

class SqliteGovernanceCommandHandler final : public application::GovernanceCommandHandler {
public:
  explicit SqliteGovernanceCommandHandler(QString databasePath) : transaction_(std::move(databasePath)) {}

  CommandResult handle(const LinkNoteToTask &command, QString *errorMessage) override {
    if (errorMessage)
      errorMessage->clear();
    if (!validOperation(command.operation))
      return CommandResult::rejected(CommandErrorCode::invalid_argument);
    return mapStorageFailure(transaction_.execute(
        command.operation, requestDigest(command), [&](sqlite3 *database) { return execute(database, command); },
        errorMessage));
  }

  CommandResult handle(const RecordEvidence &command, QString *errorMessage) override {
    if (errorMessage)
      errorMessage->clear();
    if (!validOperation(command.operation))
      return CommandResult::rejected(CommandErrorCode::invalid_argument);
    return mapStorageFailure(transaction_.execute(
        command.operation, requestDigest(command), [&](sqlite3 *database) { return execute(database, command); },
        errorMessage));
  }

  CommandResult handle(const RecordAcceptance &command, QString *errorMessage) override {
    if (errorMessage)
      errorMessage->clear();
    if (!validOperation(command.operation))
      return CommandResult::rejected(CommandErrorCode::invalid_argument);
    return mapStorageFailure(transaction_.execute(
        command.operation, requestDigest(command), [&](sqlite3 *database) { return execute(database, command); },
        errorMessage));
  }

private:
  static CommandResult mapStorageFailure(const std::optional<CommandResult> &result) {
    return result.value_or(CommandResult::rejected(CommandErrorCode::storage_unavailable));
  }

  static CommandWorkResult execute(sqlite3 *database, const LinkNoteToTask &command) {
    if (!validText(command.taskId) || !validText(command.note.documentId()) ||
        (command.note.sectionId() && !validText(*command.note.sectionId())))
      return rejected(CommandErrorCode::invalid_argument);
    GovernanceStore store(database);
    CommandWorkResult early = rejected(CommandErrorCode::invalid_argument);
    const auto next = checkedRevision(store, command, &early);
    if (!next)
      return early;
    bool exists = false;
    if (!store.noteExists(command.taskId, command.note, &exists))
      return CommandWorkResult::storageFailure();
    if (exists)
      return rejected(CommandErrorCode::invalid_argument);
    if (!store.advance(command.taskId, *next) || !store.linkNote(command))
      return CommandWorkResult::storageFailure();
    return CommandWorkResult::completed(
        CommandResult::succeeded(command.taskId, *next),
        {fact(command.operation, command.taskId, *next, "NoteLinked", command.note.documentId(), "document_id")});
  }

  static CommandWorkResult execute(sqlite3 *database, const RecordEvidence &command) {
    if (!validText(command.taskId) || !validText(command.evidenceId) || !validText(command.locator))
      return rejected(CommandErrorCode::invalid_argument);
    GovernanceStore store(database);
    CommandWorkResult early = rejected(CommandErrorCode::invalid_argument);
    const auto next = checkedRevision(store, command, &early);
    if (!next)
      return early;
    bool exists = false;
    if (!store.objectExists("governance_evidence", "evidence_id", command.evidenceId, &exists))
      return CommandWorkResult::storageFailure();
    if (exists)
      return rejected(CommandErrorCode::invalid_argument);
    if (!store.advance(command.taskId, *next) || !store.recordEvidence(command))
      return CommandWorkResult::storageFailure();
    return CommandWorkResult::completed(
        CommandResult::succeeded(command.taskId, *next),
        {fact(command.operation, command.taskId, *next, "EvidenceRecorded", command.evidenceId, "evidence_id")});
  }

  static CommandWorkResult execute(sqlite3 *database, const RecordAcceptance &command) {
    if (!validText(command.taskId) || !validText(command.acceptanceId) || !validConclusion(command.conclusion) ||
        (command.conclusion == domain::AcceptanceConclusion::passed && command.evidence.empty()))
      return rejected(CommandErrorCode::invalid_argument);
    std::set<std::pair<std::string, std::int64_t>> references;
    for (const auto &evidence : command.evidence) {
      if (!validText(evidence.evidenceId) || !references.emplace(evidence.evidenceId, evidence.revision.value()).second)
        return rejected(CommandErrorCode::invalid_argument);
    }
    GovernanceStore store(database);
    CommandWorkResult early = rejected(CommandErrorCode::invalid_argument);
    const auto next = checkedRevision(store, command, &early);
    if (!next)
      return early;
    bool exists = false;
    if (!store.objectExists("governance_acceptance", "acceptance_id", command.acceptanceId, &exists))
      return CommandWorkResult::storageFailure();
    if (exists)
      return rejected(CommandErrorCode::invalid_argument);
    for (const auto &evidence : command.evidence) {
      bool matches = false;
      if (!store.evidenceMatches(command.taskId, evidence, &matches))
        return CommandWorkResult::storageFailure();
      if (!matches)
        return rejected(CommandErrorCode::invalid_argument);
    }
    if (!store.advance(command.taskId, *next) || !store.recordAcceptance(command))
      return CommandWorkResult::storageFailure();
    return CommandWorkResult::completed(
        CommandResult::succeeded(command.taskId, *next),
        {fact(command.operation, command.taskId, *next, "AcceptanceConcluded", command.acceptanceId, "acceptance_id")});
  }

  SqliteCommandTransaction transaction_;
};

void setReadError(QString *errorMessage, const char *message) {
  if (errorMessage)
    *errorMessage = QString::fromUtf8(message);
}

std::optional<std::string> textColumn(sqlite3_stmt *statement, int column, bool allowEmpty = false) {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
    return std::nullopt;
  const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
  const int size = sqlite3_column_bytes(statement, column);
  if (value == nullptr || size < 0)
    return std::nullopt;
  std::string result(value, static_cast<std::size_t>(size));
  return validText(result, allowEmpty) ? std::optional<std::string>{std::move(result)} : std::nullopt;
}

} // namespace

std::unique_ptr<application::GovernanceCommandHandler> makeGovernanceCommandHandler(const QString &databasePath) {
  return std::make_unique<SqliteGovernanceCommandHandler>(databasePath);
}

GovernanceQuery::GovernanceQuery(QString databasePath) : databasePath_(std::move(databasePath)) {}

std::optional<domain::GovernanceTrace> GovernanceQuery::traceForTask(const std::string &taskId,
                                                                     QString *errorMessage) const {
  if (errorMessage)
    errorMessage->clear();
  bool succeeded = false;
  struct ReadErrorGuard final {
    QString *message;
    bool *succeeded;
    ~ReadErrorGuard() {
      if (message && !*succeeded && message->isEmpty())
        *message = QStringLiteral("无法读取治理事实");
    }
  } guard{errorMessage, &succeeded};
  sqlite3 *rawDatabase = nullptr;
  if (!validText(taskId) ||
      sqlite3_open_v2(databasePath_.toUtf8().constData(), &rawDatabase, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (rawDatabase)
      sqlite3_close(rawDatabase);
    setReadError(errorMessage, "无法读取治理事实");
    return std::nullopt;
  }
  Database database(rawDatabase);
  if (sqlite3_exec(database.get(), "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    setReadError(errorMessage, "无法读取治理事实");
    return std::nullopt;
  }

  domain::GovernanceTrace trace{taskId, Revision(0), {}, {}, {}, {}};
  sqlite3_stmt *rawRevision = nullptr;
  if (sqlite3_prepare_v2(database.get(), "SELECT revision FROM governance_targets WHERE task_id=?;", -1, &rawRevision,
                         nullptr) != SQLITE_OK) {
    setReadError(errorMessage, "无法读取治理事实");
    return std::nullopt;
  }
  Statement revision(rawRevision);
  if (!bindText(revision.get(), 1, taskId) || sqlite3_step(revision.get()) != SQLITE_ROW ||
      sqlite3_column_type(revision.get(), 0) != SQLITE_INTEGER || sqlite3_column_int64(revision.get(), 0) < 0) {
    setReadError(errorMessage, "治理目标不存在");
    return std::nullopt;
  }
  trace.revision = Revision(sqlite3_column_int64(revision.get(), 0));

  sqlite3_stmt *rawNotes = nullptr;
  if (sqlite3_prepare_v2(database.get(),
                         "SELECT document_id,section_id FROM governance_note_links WHERE task_id=? "
                         "ORDER BY document_id,section_id;",
                         -1, &rawNotes, nullptr) != SQLITE_OK) {
    setReadError(errorMessage, "无法读取治理事实");
    return std::nullopt;
  }
  Statement notes(rawNotes);
  if (!bindText(notes.get(), 1, taskId))
    return std::nullopt;
  int step = SQLITE_OK;
  while ((step = sqlite3_step(notes.get())) == SQLITE_ROW) {
    const auto documentId = textColumn(notes.get(), 0);
    const auto sectionId = textColumn(notes.get(), 1, true);
    if (!documentId || !sectionId)
      return std::nullopt;
    auto optionalSectionId = sectionId;
    if (optionalSectionId->empty())
      optionalSectionId.reset();
    trace.notes.emplace_back(*documentId, std::move(optionalSectionId));
  }
  if (step != SQLITE_DONE)
    return std::nullopt;

  sqlite3_stmt *rawEvidence = nullptr;
  if (sqlite3_prepare_v2(database.get(),
                         "SELECT evidence_id,locator,revision FROM governance_evidence WHERE task_id=? "
                         "ORDER BY evidence_id;",
                         -1, &rawEvidence, nullptr) != SQLITE_OK)
    return std::nullopt;
  Statement evidence(rawEvidence);
  if (!bindText(evidence.get(), 1, taskId))
    return std::nullopt;
  while ((step = sqlite3_step(evidence.get())) == SQLITE_ROW) {
    const auto id = textColumn(evidence.get(), 0);
    const auto locator = textColumn(evidence.get(), 1);
    if (!id || !locator || sqlite3_column_type(evidence.get(), 2) != SQLITE_INTEGER ||
        sqlite3_column_int64(evidence.get(), 2) < 0)
      return std::nullopt;
    trace.evidence.push_back({*id, taskId, *locator, Revision(sqlite3_column_int64(evidence.get(), 2))});
  }
  if (step != SQLITE_DONE)
    return std::nullopt;

  sqlite3_stmt *rawAcceptance = nullptr;
  constexpr const char *acceptanceSql = "SELECT acceptance_id,candidate_revision,spec_revision,conclusion FROM "
                                        "governance_acceptance WHERE task_id=? ORDER BY acceptance_id;";
  if (sqlite3_prepare_v2(database.get(), acceptanceSql, -1, &rawAcceptance, nullptr) != SQLITE_OK)
    return std::nullopt;
  Statement acceptances(rawAcceptance);
  if (!bindText(acceptances.get(), 1, taskId))
    return std::nullopt;
  while ((step = sqlite3_step(acceptances.get())) == SQLITE_ROW) {
    const auto id = textColumn(acceptances.get(), 0);
    if (!id || sqlite3_column_type(acceptances.get(), 1) != SQLITE_INTEGER ||
        sqlite3_column_type(acceptances.get(), 2) != SQLITE_INTEGER ||
        sqlite3_column_type(acceptances.get(), 3) != SQLITE_INTEGER)
      return std::nullopt;
    sqlite3_stmt *rawLinks = nullptr;
    constexpr const char *linksSql =
        "SELECT l.evidence_id,l.evidence_revision,l.task_id,e.evidence_id FROM governance_acceptance_evidence l "
        "LEFT JOIN governance_evidence e ON e.evidence_id=l.evidence_id AND e.task_id=l.task_id AND "
        "e.revision=l.evidence_revision WHERE l.acceptance_id=? ORDER BY l.evidence_id,l.evidence_revision;";
    if (sqlite3_prepare_v2(database.get(), linksSql, -1, &rawLinks, nullptr) != SQLITE_OK)
      return std::nullopt;
    Statement links(rawLinks);
    if (!bindText(links.get(), 1, *id))
      return std::nullopt;
    std::vector<domain::EvidenceObservationReference> references;
    int linkStep = SQLITE_OK;
    while ((linkStep = sqlite3_step(links.get())) == SQLITE_ROW) {
      const auto evidenceId = textColumn(links.get(), 0);
      const auto linkTaskId = textColumn(links.get(), 2);
      const auto matchedEvidenceId = textColumn(links.get(), 3);
      if (!evidenceId || sqlite3_column_type(links.get(), 1) != SQLITE_INTEGER ||
          sqlite3_column_int64(links.get(), 1) < 0 || !linkTaskId || *linkTaskId != taskId || !matchedEvidenceId ||
          *matchedEvidenceId != *evidenceId)
        return std::nullopt;
      references.push_back({*evidenceId, Revision(sqlite3_column_int64(links.get(), 1))});
    }
    if (linkStep != SQLITE_DONE)
      return std::nullopt;
    const int conclusion = sqlite3_column_int(acceptances.get(), 3);
    if (conclusion < static_cast<int>(domain::AcceptanceConclusion::passed) ||
        conclusion > static_cast<int>(domain::AcceptanceConclusion::inconclusive))
      return std::nullopt;
    try {
      trace.acceptances.emplace_back(*id, taskId, Revision(sqlite3_column_int64(acceptances.get(), 1)),
                                     Revision(sqlite3_column_int64(acceptances.get(), 2)),
                                     static_cast<domain::AcceptanceConclusion>(conclusion), std::move(references));
    } catch (const std::exception &) {
      setReadError(errorMessage, "治理事实损坏");
      return std::nullopt;
    }
  }
  if (step != SQLITE_DONE)
    return std::nullopt;

  sqlite3_stmt *rawActivity = nullptr;
  constexpr const char *activitySql = "SELECT event_id,kind,summary FROM activity_facts WHERE aggregate_id=? AND "
                                      "kind IN ('NoteLinked','EvidenceRecorded','AcceptanceConcluded') ORDER BY id;";
  if (sqlite3_prepare_v2(database.get(), activitySql, -1, &rawActivity, nullptr) != SQLITE_OK)
    return std::nullopt;
  Statement activities(rawActivity);
  if (!bindText(activities.get(), 1, taskId))
    return std::nullopt;
  while ((step = sqlite3_step(activities.get())) == SQLITE_ROW) {
    const auto id = textColumn(activities.get(), 0);
    const auto kind = textColumn(activities.get(), 1);
    const auto summary = textColumn(activities.get(), 2);
    if (!id || !kind || !summary)
      return std::nullopt;
    trace.activities.push_back({*id, *kind, taskId, *summary});
  }
  if (step != SQLITE_DONE)
    return std::nullopt;
  succeeded = true;
  return trace;
}

} // namespace pros::infrastructure
