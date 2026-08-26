#include "pros/application/governance_command_handler.h"
#include "pros/infrastructure/governance_composition.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

#include <future>
#include <memory>
#include <ranges>
#include <stdexcept>

namespace {

struct DatabaseCloser final {
  void operator()(sqlite3 *database) const { sqlite3_close(database); }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

Database openDatabase(const QString &path) {
  sqlite3 *raw = nullptr;
  if (sqlite3_open(path.toUtf8().constData(), &raw) != SQLITE_OK) {
    if (raw)
      sqlite3_close(raw);
    return {};
  }
  return Database(raw);
}

bool execute(const QString &path, const char *sql) {
  const Database database = openDatabase(path);
  return database && sqlite3_exec(database.get(), sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

int countRows(const QString &path, const char *table) {
  const Database database = openDatabase(path);
  if (!database)
    return -1;
  const std::string sql = "SELECT COUNT(*) FROM " + std::string(table) + ';';
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database.get(), sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    return -1;
  const int count = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
  sqlite3_finalize(statement);
  return count;
}

sqlite3_int64 integerValue(const QString &path, const char *sql) {
  const Database database = openDatabase(path);
  if (!database)
    return -1;
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database.get(), sql, -1, &statement, nullptr) != SQLITE_OK)
    return -1;
  const sqlite3_int64 value = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
  sqlite3_finalize(statement);
  return value;
}

QString preparedDatabase(QTemporaryDir &directory) {
  QString path = directory.path() + QStringLiteral("/governance.sqlite");
  pros::infrastructure::SchemaMigrator migrator;
  if (!migrator.migrate(path, nullptr))
    return {};
  if (!execute(path, "PRAGMA foreign_keys=ON; INSERT INTO projects(id,title,status,revision) "
                     "VALUES('project-1','Project',0,0); INSERT INTO tasks(id,project_id,title,status,revision) "
                     "VALUES('task-1','project-1','Task 1',0,0),('task-2','project-1','Task 2',0,0);"))
    return {};
  return path;
}

pros::application::RecordAcceptance acceptanceCommand(std::string operationId, std::string acceptanceId,
                                                      std::string taskId, pros::domain::Revision expected,
                                                      std::string evidenceId, pros::domain::Revision evidenceRevision) {
  return {{"user-1", std::move(operationId)},
          expected,
          std::move(acceptanceId),
          std::move(taskId),
          pros::domain::Revision(7),
          pros::domain::Revision(3),
          pros::domain::AcceptanceConclusion::passed,
          {{std::move(evidenceId), evidenceRevision}}};
}

pros::domain::CommandErrorCode errorOf(const pros::domain::CommandResult &result) {
  return result.errorCode().value_or(pros::domain::CommandErrorCode::unsupported_in_version);
}

} // namespace

class GovernanceCommandHandlerTest final : public QObject {
  Q_OBJECT

private slots:
  void domainPreservesStableReferences();
  void writesAndReadsCompleteGovernanceTrace();
  void replaysSuccessAndRejectsChangedRequest();
  void canonicalizesAcceptanceEvidenceOrder();
  void digestCoversEveryCommandField();
  void rejectsStaleAndFutureRevisions();
  void serializesConcurrentExpectedRevision();
  void rejectsUnsafeTextAndInvalidConclusion();
  void replaysDeterministicEvidenceFailureAfterStateChanges();
  void rejectsMissingWrongTaskAndOldEvidence();
  void recordsPassedWithoutEvidenceAsStableFailure();
  void rollsBackAggregateWhenSharedFactWriteFails();
  void rejectsDamagedGovernanceTrace();
  void requiresMigratedSchemaWithoutCreatingTables();
};

void GovernanceCommandHandlerTest::domainPreservesStableReferences() {
  QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pros::domain::DocumentReference(""));
  QVERIFY_THROWS_EXCEPTION(std::invalid_argument,
                           pros::domain::Acceptance("acceptance-1", "task-1", pros::domain::Revision(3),
                                                    pros::domain::Revision(2),
                                                    pros::domain::AcceptanceConclusion::passed, {}));
  const pros::domain::EvidenceObservationReference evidence{"evidence-1", pros::domain::Revision(4)};
  const pros::domain::Acceptance acceptance("acceptance-1", "task-1", pros::domain::Revision(3),
                                            pros::domain::Revision(2), pros::domain::AcceptanceConclusion::passed,
                                            {evidence});
  QCOMPARE(acceptance.evidence().front(), evidence);
}

void GovernanceCommandHandlerTest::writesAndReadsCompleteGovernanceTrace() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = preparedDatabase(directory);
  QVERIFY(!path.isEmpty());
  auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);

  const auto note = handler->handle({{"user-1", "op-note"},
                                     pros::domain::Revision(0),
                                     "task-1",
                                     pros::domain::DocumentReference("document-1", "section-1")});
  QVERIFY(note.isSuccess());
  const auto noteSuccess = note.success().value_or(pros::domain::CommandSuccess{"", pros::domain::Revision(0)});
  QCOMPARE(noteSuccess.aggregateId, "task-1");
  QCOMPARE(noteSuccess.revision.value(), 1);
  const auto evidence = handler->handle(
      {{"user-1", "op-evidence"}, pros::domain::Revision(1), "evidence-1", "task-1", "file://evidence"});
  QVERIFY(evidence.isSuccess());
  const auto acceptance = handler->handle(acceptanceCommand(
      "op-acceptance", "acceptance-1", "task-1", pros::domain::Revision(2), "evidence-1", pros::domain::Revision(1)));
  QVERIFY(acceptance.isSuccess());
  const auto acceptanceSuccess =
      acceptance.success().value_or(pros::domain::CommandSuccess{"", pros::domain::Revision(0)});
  QCOMPARE(acceptanceSuccess.revision.value(), 3);

  pros::infrastructure::GovernanceQuery query(path);
  QString error = QStringLiteral("旧错误");
  const auto trace = query.traceForTask("task-1", &error);
  QVERIFY2(trace.has_value(), qPrintable(error));
  QVERIFY(error.isEmpty());
  const auto traceValue = trace.value_or(pros::domain::GovernanceTrace{"", pros::domain::Revision(0), {}, {}, {}, {}});
  QCOMPARE(traceValue.revision.value(), 3);
  QCOMPARE(traceValue.notes.size(), 1);
  QCOMPARE(traceValue.notes.front().documentId(), "document-1");
  QCOMPARE(traceValue.evidence.size(), 1);
  QCOMPARE(traceValue.evidence.front().locator, "file://evidence");
  QCOMPARE(traceValue.acceptances.size(), 1);
  QCOMPARE(traceValue.acceptances.front().evidence().front().revision.value(), 1);
  QCOMPARE(traceValue.activities.size(), 3);
  QCOMPARE(traceValue.activities.at(0).eventType, "NoteLinked");
  QCOMPARE(traceValue.activities.at(1).eventType, "EvidenceRecorded");
  QCOMPARE(traceValue.activities.at(2).eventType, "AcceptanceConcluded");
  QCOMPARE(countRows(path, "operation_records"), 3);
  QCOMPARE(countRows(path, "domain_events"), 3);
  QCOMPARE(countRows(path, "outbox_records"), 3);
  QCOMPARE(countRows(path, "activity_facts"), 3);
}

void GovernanceCommandHandlerTest::replaysSuccessAndRejectsChangedRequest() {
  QTemporaryDir directory;
  const QString path = preparedDatabase(directory);
  QVERIFY(!path.isEmpty());
  auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);
  const pros::application::RecordEvidence command{
      {"user-1", "op-evidence"}, pros::domain::Revision(0), "evidence-1", "task-1", "file://one"};
  const auto first = handler->handle(command);
  QVERIFY(first.isSuccess());
  const auto replay = handler->handle(command);
  QVERIFY(replay.isSuccess());
  QCOMPARE(replay, first);
  const auto changed =
      handler->handle({{"user-1", "op-evidence"}, pros::domain::Revision(0), "evidence-1", "task-1", "file://changed"});
  QCOMPARE(errorOf(changed), pros::domain::CommandErrorCode::idempotency_key_reused);
  QCOMPARE(countRows(path, "governance_evidence"), 1);
  QCOMPARE(countRows(path, "domain_events"), 1);
}

void GovernanceCommandHandlerTest::canonicalizesAcceptanceEvidenceOrder() {
  QTemporaryDir directory;
  const QString path = preparedDatabase(directory);
  QVERIFY(!path.isEmpty());
  auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);
  QVERIFY(handler->handle({{"user-1", "op-e1"}, pros::domain::Revision(0), "evidence-1", "task-1", "one"}).isSuccess());
  QVERIFY(handler->handle({{"user-1", "op-e2"}, pros::domain::Revision(1), "evidence-2", "task-1", "two"}).isSuccess());
  pros::application::RecordAcceptance command{
      {"user-1", "op-acceptance"},
      pros::domain::Revision(2),
      "acceptance-1",
      "task-1",
      pros::domain::Revision(2),
      pros::domain::Revision(1),
      pros::domain::AcceptanceConclusion::passed,
      {{"evidence-1", pros::domain::Revision(1)}, {"evidence-2", pros::domain::Revision(1)}}};
  const auto first = handler->handle(command);
  QVERIFY(first.isSuccess());
  std::ranges::reverse(command.evidence);
  const auto replay = handler->handle(command);
  QVERIFY(replay.isSuccess());
  QCOMPARE(replay, first);
}

void GovernanceCommandHandlerTest::digestCoversEveryCommandField() {
  QTemporaryDir directory;
  const QString path = preparedDatabase(directory);
  QVERIFY(!path.isEmpty());
  auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);
  std::int64_t revision = 0;

  const auto verifyLink = [&handler, &revision](std::string operationId, auto change) {
    const std::string documentId = operationId;
    pros::application::LinkNoteToTask original{{"user-1", std::move(operationId)},
                                               pros::domain::Revision(revision),
                                               "task-1",
                                               pros::domain::DocumentReference(documentId, "section")};
    auto changed = original;
    change(changed);
    const bool covered = handler->handle(original).isSuccess() &&
                         errorOf(handler->handle(changed)) == pros::domain::CommandErrorCode::idempotency_key_reused;
    ++revision;
    return covered;
  };
  QVERIFY(verifyLink("digest-link-expected", [](auto &value) { value.expectedRevision = pros::domain::Revision(9); }));
  QVERIFY(verifyLink("digest-link-task", [](auto &value) { value.taskId = "task-2"; }));
  QVERIFY(verifyLink("digest-link-document",
                     [](auto &value) { value.note = pros::domain::DocumentReference("other", "section"); }));
  QVERIFY(verifyLink("digest-link-section", [](auto &value) {
    value.note = pros::domain::DocumentReference(value.note.documentId(), "other");
  }));

  const auto verifyEvidence = [&handler, &revision](std::string operationId, std::string evidenceId, auto change) {
    pros::application::RecordEvidence original{{"user-1", std::move(operationId)},
                                               pros::domain::Revision(revision),
                                               std::move(evidenceId),
                                               "task-1",
                                               "locator"};
    auto changed = original;
    change(changed);
    const bool covered = handler->handle(original).isSuccess() &&
                         errorOf(handler->handle(changed)) == pros::domain::CommandErrorCode::idempotency_key_reused;
    ++revision;
    return covered;
  };
  QVERIFY(verifyEvidence("digest-evidence-expected", "evidence-expected",
                         [](auto &value) { value.expectedRevision = pros::domain::Revision(19); }));
  QVERIFY(verifyEvidence("digest-evidence-id", "evidence-id", [](auto &value) { value.evidenceId = "other"; }));
  QVERIFY(verifyEvidence("digest-evidence-task", "evidence-task", [](auto &value) { value.taskId = "task-2"; }));
  QVERIFY(verifyEvidence("digest-evidence-locator", "evidence-locator", [](auto &value) { value.locator = "other"; }));

  const std::string evidenceId = "acceptance-evidence";
  QVERIFY(handler
              ->handle({{"user-1", "create-acceptance-evidence"},
                        pros::domain::Revision(revision),
                        evidenceId,
                        "task-1",
                        "locator"})
              .isSuccess());
  ++revision;
  const auto verifyAcceptance = [&handler, &revision, &evidenceId](std::string operationId, std::string acceptanceId,
                                                                   auto change) {
    pros::application::RecordAcceptance original{{"user-1", std::move(operationId)},
                                                 pros::domain::Revision(revision),
                                                 std::move(acceptanceId),
                                                 "task-1",
                                                 pros::domain::Revision(7),
                                                 pros::domain::Revision(3),
                                                 pros::domain::AcceptanceConclusion::passed,
                                                 {{evidenceId, pros::domain::Revision(1)}}};
    auto changed = original;
    change(changed);
    const bool covered = handler->handle(original).isSuccess() &&
                         errorOf(handler->handle(changed)) == pros::domain::CommandErrorCode::idempotency_key_reused;
    ++revision;
    return covered;
  };
  QVERIFY(verifyAcceptance("digest-acceptance-expected", "acceptance-expected",
                           [](auto &value) { value.expectedRevision = pros::domain::Revision(29); }));
  QVERIFY(verifyAcceptance("digest-acceptance-id", "acceptance-id", [](auto &value) { value.acceptanceId = "other"; }));
  QVERIFY(verifyAcceptance("digest-acceptance-task", "acceptance-task", [](auto &value) { value.taskId = "task-2"; }));
  QVERIFY(verifyAcceptance("digest-acceptance-candidate", "acceptance-candidate",
                           [](auto &value) { value.candidateRevision = pros::domain::Revision(8); }));
  QVERIFY(verifyAcceptance("digest-acceptance-spec", "acceptance-spec",
                           [](auto &value) { value.specificationRevision = pros::domain::Revision(4); }));
  QVERIFY(verifyAcceptance("digest-acceptance-conclusion", "acceptance-conclusion",
                           [](auto &value) { value.conclusion = pros::domain::AcceptanceConclusion::failed; }));
  QVERIFY(verifyAcceptance("digest-acceptance-evidence-id", "acceptance-evidence-id",
                           [](auto &value) { value.evidence.front().evidenceId = "other"; }));
  QVERIFY(verifyAcceptance("digest-acceptance-evidence-revision", "acceptance-evidence-revision",
                           [](auto &value) { value.evidence.front().revision = pros::domain::Revision(2); }));
}

void GovernanceCommandHandlerTest::rejectsStaleAndFutureRevisions() {
  QTemporaryDir directory;
  const QString path = preparedDatabase(directory);
  QVERIFY(!path.isEmpty());
  auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);
  QVERIFY(handler
              ->handle({{"user-1", "revision-note"},
                        pros::domain::Revision(0),
                        "task-1",
                        pros::domain::DocumentReference("document")})
              .isSuccess());
  QCOMPARE(
      errorOf(handler->handle(
          {{"user-1", "stale-note"}, pros::domain::Revision(0), "task-1", pros::domain::DocumentReference("stale")})),
      pros::domain::CommandErrorCode::revision_conflict);
  QCOMPARE(errorOf(handler->handle(
               {{"user-1", "future-evidence"}, pros::domain::Revision(2), "evidence-future", "task-1", "locator"})),
           pros::domain::CommandErrorCode::revision_conflict);
  QVERIFY(
      handler->handle({{"user-1", "revision-evidence"}, pros::domain::Revision(1), "evidence-1", "task-1", "locator"})
          .isSuccess());
  QCOMPARE(
      errorOf(handler->handle(acceptanceCommand("stale-acceptance", "acceptance-stale", "task-1",
                                                pros::domain::Revision(1), "evidence-1", pros::domain::Revision(1)))),
      pros::domain::CommandErrorCode::revision_conflict);
  QCOMPARE(
      errorOf(handler->handle(acceptanceCommand("future-acceptance", "acceptance-future", "task-1",
                                                pros::domain::Revision(3), "evidence-1", pros::domain::Revision(1)))),
      pros::domain::CommandErrorCode::revision_conflict);
  QCOMPARE(countRows(path, "domain_events"), 2);
}

void GovernanceCommandHandlerTest::serializesConcurrentExpectedRevision() {
  QTemporaryDir directory;
  const QString path = preparedDatabase(directory);
  QVERIFY(!path.isEmpty());
  std::promise<void> start;
  const std::shared_future<void> gate = start.get_future().share();
  const auto launch = [&path, gate](std::string operationId, std::string documentId) {
    return std::async(std::launch::async, [&path, gate, operationId = std::move(operationId),
                                           documentId = std::move(documentId)] {
      auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);
      gate.wait();
      return handler->handle(
          {{"user-1", operationId}, pros::domain::Revision(0), "task-1", pros::domain::DocumentReference(documentId)});
    });
  };
  auto first = launch("concurrent-1", "document-1");
  auto second = launch("concurrent-2", "document-2");
  start.set_value();
  const auto firstResult = first.get();
  const auto secondResult = second.get();
  QCOMPARE(static_cast<int>(firstResult.isSuccess()) + static_cast<int>(secondResult.isSuccess()), 1);
  const auto &rejected = firstResult.isSuccess() ? secondResult : firstResult;
  QCOMPARE(errorOf(rejected), pros::domain::CommandErrorCode::revision_conflict);
  QCOMPARE(countRows(path, "governance_note_links"), 1);
  QCOMPARE(countRows(path, "domain_events"), 1);
}

void GovernanceCommandHandlerTest::rejectsUnsafeTextAndInvalidConclusion() {
  QTemporaryDir directory;
  const QString path = preparedDatabase(directory);
  QVERIFY(!path.isEmpty());
  auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);
  const std::string nulTask("task-1\0alias", 12);
  const pros::application::RecordEvidence nulCommand{
      {"user-1", "nul-task"}, pros::domain::Revision(0), "evidence-1", nulTask, "locator"};
  const auto nulResult = handler->handle(nulCommand);
  QCOMPARE(errorOf(nulResult), pros::domain::CommandErrorCode::invalid_argument);
  QCOMPARE(handler->handle(nulCommand), nulResult);
  const std::string invalidUtf8("\xC3\x28", 2);
  QCOMPARE(errorOf(handler->handle(
               {{"user-1", "invalid-utf8"}, pros::domain::Revision(0), "evidence-2", "task-1", invalidUtf8})),
           pros::domain::CommandErrorCode::invalid_argument);
  const std::string truncatedUtf8("\xE2\x82", 2);
  const pros::application::RecordEvidence truncatedCommand{
      {"user-1", "truncated-utf8"}, pros::domain::Revision(0), "evidence-3", "task-1", truncatedUtf8};
  const auto truncatedResult = handler->handle(truncatedCommand);
  QCOMPARE(errorOf(truncatedResult), pros::domain::CommandErrorCode::invalid_argument);
  QCOMPARE(handler->handle(truncatedCommand), truncatedResult);
  auto invalidConclusion = acceptanceCommand("invalid-conclusion", "acceptance-1", "task-1", pros::domain::Revision(0),
                                             "missing", pros::domain::Revision(1));
  // 故意模拟不可信协议解码产生的越界枚举值。
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  invalidConclusion.conclusion = static_cast<pros::domain::AcceptanceConclusion>(99);
  QCOMPARE(errorOf(handler->handle(invalidConclusion)), pros::domain::CommandErrorCode::invalid_argument);
  const pros::domain::OperationKey invalidOperation(std::string("user\0alias", 10), "invalid-operation");
  QString error = QStringLiteral("旧错误");
  QCOMPARE(errorOf(handler->handle({invalidOperation, pros::domain::Revision(0), "evidence-3", "task-1", "locator"},
                                   &error)),
           pros::domain::CommandErrorCode::invalid_argument);
  QVERIFY(error.isEmpty());
  const pros::domain::OperationKey truncatedOperation("user", truncatedUtf8);
  QCOMPARE(errorOf(handler->handle({truncatedOperation, pros::domain::Revision(0), "evidence-4", "task-1", "locator"},
                                   &error)),
           pros::domain::CommandErrorCode::invalid_argument);
  QVERIFY(error.isEmpty());
  QCOMPARE(countRows(path, "operation_records"), 4);
  QCOMPARE(countRows(path, "governance_evidence"), 0);
  QCOMPARE(countRows(path, "domain_events"), 0);
}

void GovernanceCommandHandlerTest::replaysDeterministicEvidenceFailureAfterStateChanges() {
  QTemporaryDir directory;
  const QString path = preparedDatabase(directory);
  QVERIFY(!path.isEmpty());
  auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);
  const auto failedCommand = acceptanceCommand("op-failed", "acceptance-1", "task-1", pros::domain::Revision(0),
                                               "evidence-later", pros::domain::Revision(1));
  const auto first = handler->handle(failedCommand);
  QCOMPARE(errorOf(first), pros::domain::CommandErrorCode::invalid_argument);
  const auto evidence =
      handler->handle({{"user-1", "op-create"}, pros::domain::Revision(0), "evidence-later", "task-1", "file://later"});
  QVERIFY(evidence.isSuccess());
  const auto replay = handler->handle(failedCommand);
  QCOMPARE(errorOf(replay), pros::domain::CommandErrorCode::invalid_argument);
  QCOMPARE(countRows(path, "governance_acceptance"), 0);
  QCOMPARE(countRows(path, "operation_records"), 2);
}

void GovernanceCommandHandlerTest::rejectsMissingWrongTaskAndOldEvidence() {
  QTemporaryDir directory;
  const QString path = preparedDatabase(directory);
  QVERIFY(!path.isEmpty());
  auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);
  QVERIFY(handler->handle({{"user-1", "op-e1"}, pros::domain::Revision(0), "evidence-1", "task-1", "one"}).isSuccess());
  QVERIFY(handler->handle({{"user-1", "op-e2"}, pros::domain::Revision(0), "evidence-2", "task-2", "two"}).isSuccess());
  QCOMPARE(errorOf(handler->handle(acceptanceCommand("op-missing", "a-missing", "task-1", pros::domain::Revision(1),
                                                     "missing", pros::domain::Revision(1)))),
           pros::domain::CommandErrorCode::invalid_argument);
  QCOMPARE(errorOf(handler->handle(acceptanceCommand("op-old", "a-old", "task-1", pros::domain::Revision(1),
                                                     "evidence-1", pros::domain::Revision(0)))),
           pros::domain::CommandErrorCode::invalid_argument);
  QCOMPARE(errorOf(handler->handle(acceptanceCommand("op-other", "a-other", "task-1", pros::domain::Revision(1),
                                                     "evidence-2", pros::domain::Revision(1)))),
           pros::domain::CommandErrorCode::invalid_argument);
  QCOMPARE(countRows(path, "governance_acceptance"), 0);
}

void GovernanceCommandHandlerTest::recordsPassedWithoutEvidenceAsStableFailure() {
  QTemporaryDir directory;
  const QString path = preparedDatabase(directory);
  QVERIFY(!path.isEmpty());
  auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);
  pros::application::RecordAcceptance command{{"user-1", "op-empty"},
                                              pros::domain::Revision(0),
                                              "acceptance-1",
                                              "task-1",
                                              pros::domain::Revision(1),
                                              pros::domain::Revision(1),
                                              pros::domain::AcceptanceConclusion::passed,
                                              {}};
  QCOMPARE(errorOf(handler->handle(command)), pros::domain::CommandErrorCode::invalid_argument);
  QCOMPARE(errorOf(handler->handle(command)), pros::domain::CommandErrorCode::invalid_argument);
  command.conclusion = pros::domain::AcceptanceConclusion::failed;
  QCOMPARE(errorOf(handler->handle(command)), pros::domain::CommandErrorCode::idempotency_key_reused);
  QCOMPARE(countRows(path, "operation_records"), 1);
  QCOMPARE(countRows(path, "domain_events"), 0);
}

void GovernanceCommandHandlerTest::rollsBackAggregateWhenSharedFactWriteFails() {
  QTemporaryDir directory;
  const QString path = preparedDatabase(directory);
  QVERIFY(!path.isEmpty());
  QVERIFY(execute(path, "CREATE TRIGGER reject_evidence_activity BEFORE INSERT ON activity_facts "
                        "WHEN NEW.kind='EvidenceRecorded' BEGIN SELECT RAISE(ABORT,'blocked'); END;"));
  auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);
  QString error;
  const auto result = handler->handle(
      {{"user-1", "op-evidence"}, pros::domain::Revision(0), "evidence-1", "task-1", "file://evidence"}, &error);
  QCOMPARE(errorOf(result), pros::domain::CommandErrorCode::storage_unavailable);
  QVERIFY(!error.isEmpty());
  QCOMPARE(countRows(path, "governance_targets"), 0);
  QCOMPARE(countRows(path, "governance_evidence"), 0);
  QCOMPARE(countRows(path, "operation_records"), 0);
  QCOMPARE(countRows(path, "domain_events"), 0);
  QCOMPARE(countRows(path, "outbox_records"), 0);
  QCOMPARE(countRows(path, "activity_facts"), 0);
  QCOMPARE(integerValue(path, "SELECT next_position FROM delivery_sequence WHERE delivery_partition='global';"), 1);
}

void GovernanceCommandHandlerTest::rejectsDamagedGovernanceTrace() {
  QTemporaryDir crossTaskDirectory;
  const QString crossTaskPath = preparedDatabase(crossTaskDirectory);
  QVERIFY(!crossTaskPath.isEmpty());
  auto crossTaskHandler = pros::infrastructure::makeGovernanceCommandHandler(crossTaskPath);
  QVERIFY(crossTaskHandler
              ->handle({{"user-1", "cross-evidence"}, pros::domain::Revision(0), "evidence-2", "task-2", "locator"})
              .isSuccess());
  pros::application::RecordAcceptance failed{{"user-1", "cross-acceptance"},
                                             pros::domain::Revision(0),
                                             "acceptance-1",
                                             "task-1",
                                             pros::domain::Revision(1),
                                             pros::domain::Revision(1),
                                             pros::domain::AcceptanceConclusion::failed,
                                             {}};
  QVERIFY(crossTaskHandler->handle(failed).isSuccess());
  constexpr const char *crossTaskLink =
      "INSERT INTO governance_acceptance_evidence"
      "(acceptance_id,task_id,evidence_id,evidence_revision) VALUES('acceptance-1','task-2','evidence-2',1);";
  const std::string constrainedCrossTaskLink = std::string("PRAGMA foreign_keys=ON;") + crossTaskLink;
  QVERIFY(!execute(crossTaskPath, constrainedCrossTaskLink.c_str()));
  QVERIFY(execute(crossTaskPath, crossTaskLink));
  pros::infrastructure::GovernanceQuery crossTaskQuery(crossTaskPath);
  QString error;
  QVERIFY(!crossTaskQuery.traceForTask("task-1", &error).has_value());
  QVERIFY(!error.isEmpty());

  QTemporaryDir revisionDirectory;
  const QString revisionPath = preparedDatabase(revisionDirectory);
  QVERIFY(!revisionPath.isEmpty());
  auto revisionHandler = pros::infrastructure::makeGovernanceCommandHandler(revisionPath);
  QVERIFY(revisionHandler
              ->handle({{"user-1", "revision-evidence"}, pros::domain::Revision(0), "evidence-1", "task-1", "locator"})
              .isSuccess());
  failed.operation = pros::domain::OperationKey("user-1", "revision-acceptance");
  failed.expectedRevision = pros::domain::Revision(1);
  QVERIFY(revisionHandler->handle(failed).isSuccess());
  QVERIFY(execute(revisionPath, "INSERT INTO governance_acceptance_evidence"
                                "(acceptance_id,task_id,evidence_id,evidence_revision) "
                                "VALUES('acceptance-1','task-1','evidence-1',2);"));
  pros::infrastructure::GovernanceQuery revisionQuery(revisionPath);
  QVERIFY(!revisionQuery.traceForTask("task-1", &error).has_value());
  QVERIFY(!error.isEmpty());

  QTemporaryDir emptyDocumentDirectory;
  const QString emptyDocumentPath = preparedDatabase(emptyDocumentDirectory);
  QVERIFY(!emptyDocumentPath.isEmpty());
  QVERIFY(execute(emptyDocumentPath,
                  "INSERT INTO governance_targets(task_id,revision) VALUES('task-1',1);"
                  "INSERT INTO governance_note_links(task_id,document_id,section_id) VALUES('task-1','','');"));
  pros::infrastructure::GovernanceQuery emptyDocumentQuery(emptyDocumentPath);
  QVERIFY(!emptyDocumentQuery.traceForTask("task-1", &error).has_value());
  QVERIFY(!error.isEmpty());

  QTemporaryDir nulDocumentDirectory;
  const QString nulDocumentPath = preparedDatabase(nulDocumentDirectory);
  QVERIFY(!nulDocumentPath.isEmpty());
  QVERIFY(execute(nulDocumentPath, "INSERT INTO governance_targets(task_id,revision) VALUES('task-1',1);"
                                   "INSERT INTO governance_note_links(task_id,document_id,section_id) "
                                   "VALUES('task-1',CAST(X'610062' AS TEXT),'');"));
  pros::infrastructure::GovernanceQuery nulDocumentQuery(nulDocumentPath);
  error = QStringLiteral("旧错误");
  QVERIFY(!nulDocumentQuery.traceForTask("task-1", &error).has_value());
  QVERIFY(!error.isEmpty());

  QTemporaryDir truncatedDocumentDirectory;
  const QString truncatedDocumentPath = preparedDatabase(truncatedDocumentDirectory);
  QVERIFY(!truncatedDocumentPath.isEmpty());
  QVERIFY(execute(truncatedDocumentPath, "INSERT INTO governance_targets(task_id,revision) VALUES('task-1',1);"
                                         "INSERT INTO governance_note_links(task_id,document_id,section_id) "
                                         "VALUES('task-1',CAST(X'E282' AS TEXT),'');"));
  pros::infrastructure::GovernanceQuery truncatedDocumentQuery(truncatedDocumentPath);
  error = QStringLiteral("旧错误");
  QVERIFY(!truncatedDocumentQuery.traceForTask("task-1", &error).has_value());
  QVERIFY(!error.isEmpty());
}

void GovernanceCommandHandlerTest::requiresMigratedSchemaWithoutCreatingTables() {
  QTemporaryDir directory;
  const QString path = directory.path() + QStringLiteral("/empty.sqlite");
  QVERIFY(execute(path, "CREATE TABLE sentinel(id INTEGER);"));
  auto handler = pros::infrastructure::makeGovernanceCommandHandler(path);
  const auto result = handler->handle(
      {{"user-1", "op-note"}, pros::domain::Revision(0), "task-1", pros::domain::DocumentReference("document-1")});
  QCOMPARE(errorOf(result), pros::domain::CommandErrorCode::storage_unavailable);
  QCOMPARE(countRows(path, "sentinel"), 0);
  QCOMPARE(countRows(path, "governance_targets"), -1);
}

QTEST_APPLESS_MAIN(GovernanceCommandHandlerTest)

#include "governance_command_handler_test.moc"
