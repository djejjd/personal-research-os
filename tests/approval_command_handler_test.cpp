#include "pros/infrastructure/schema_migrator.h"
#include "pros/infrastructure/sqlite_approval_command_handler.h"

#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

#include <cstring>
#include <type_traits>

namespace {

int countRows(const QString &path, const char *table) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    return -1;
  const std::string sql = "SELECT COUNT(*) FROM " + std::string(table) + ";";
  sqlite3_stmt *statement = nullptr;
  sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr);
  const int count = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return count;
}

std::string textValue(const QString &path, const char *sql) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    return {};
  sqlite3_stmt *statement = nullptr;
  sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
  std::string value;
  if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_TEXT)
    value = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return value;
}

bool executeSql(const QString &path, const char *sql) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
    return false;
  const bool succeeded = sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
  sqlite3_close(database);
  return succeeded;
}

} // namespace

class ApprovalCommandHandlerTest final : public QObject {
  Q_OBJECT

private slots:
  void storesPlanAndApprovalWithoutExecutionCapability();
  void dispatchAlwaysRefusesAndReplaysAudit();
  void approvalRequiresExactPlanCoordinate();
  void damagedPlanDigestIsStorageFailureAndRetryable_data();
  void damagedPlanDigestIsStorageFailureAndRetryable();
  void rejectsInvalidDecisionAndReplays();
  void rejectedOperationReplaysAfterStateChanges();
  void digestCoversAllCommandFields();
  void storageFailureRollsBackLedgerAndFacts();
};

void ApprovalCommandHandlerTest::storesPlanAndApprovalWithoutExecutionCapability() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.path() + "/approval.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  pros::infrastructure::SqliteApprovalCommandHandler handler(path);

  const auto plan =
      handler.createOperationPlan({{"user", "create-plan"}, pros::domain::Revision(0), "plan-1", "仅保存的研究计划"});
  QVERIFY(plan.isSuccess());
  const std::string planDigest = textValue(path, "SELECT plan_digest FROM operation_plans WHERE id='plan-1';");
  QVERIFY(!planDigest.empty());
  const auto approval = handler.recordApproval({{"user", "record-approval"},
                                                pros::domain::Revision(0),
                                                "approval-1",
                                                "plan-1",
                                                pros::domain::Revision(1),
                                                planDigest,
                                                pros::domain::ApprovalDecision::approved,
                                                "人工批准，仅保存"});
  QVERIFY(approval.isSuccess());
  QCOMPARE(handler.createOperationPlan({{"user", "duplicate-plan"}, pros::domain::Revision(0), "plan-1", "duplicate"})
               .errorCode(),
           std::optional(pros::domain::CommandErrorCode::invalid_argument));
  QCOMPARE(handler
               .recordApproval({{"user", "duplicate-approval"},
                                pros::domain::Revision(0),
                                "approval-1",
                                "plan-1",
                                pros::domain::Revision(1),
                                planDigest,
                                pros::domain::ApprovalDecision::approved,
                                "duplicate"})
               .errorCode(),
           std::optional(pros::domain::CommandErrorCode::invalid_argument));
  QCOMPARE(countRows(path, "operation_plans"), 1);
  QCOMPARE(countRows(path, "approvals"), 1);
  QCOMPARE(countRows(path, "domain_events"), 2);
  QCOMPARE(countRows(path, "outbox_records"), 2);
  QCOMPARE(countRows(path, "activity_facts"), 2);
}

void ApprovalCommandHandlerTest::dispatchAlwaysRefusesAndReplaysAudit() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.path() + "/dispatch.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  pros::infrastructure::SqliteApprovalCommandHandler handler(path);
  QVERIFY(handler
              .createOperationPlan(
                  {{"user", "create-known-plan"}, pros::domain::Revision(0), "known-plan", "known summary"})
              .isSuccess());
  const pros::application::DispatchOperationPlan unknown{
      {"user", "dispatch-1"}, pros::domain::Revision(0), "missing-plan"};
  const pros::application::DispatchOperationPlan anotherUnknown{
      {"user", "dispatch-2"}, pros::domain::Revision(0), "missing-plan"};
  const pros::application::DispatchOperationPlan known{{"user", "dispatch-3"}, pros::domain::Revision(1), "known-plan"};

  const auto first = handler.dispatchOperationPlan(unknown);
  QCOMPARE(first.errorCode(), std::optional(pros::domain::CommandErrorCode::unsupported_in_version));
  QCOMPARE(handler.dispatchOperationPlan(anotherUnknown).errorCode(),
           std::optional(pros::domain::CommandErrorCode::unsupported_in_version));
  QCOMPARE(handler.dispatchOperationPlan(known).errorCode(),
           std::optional(pros::domain::CommandErrorCode::unsupported_in_version));
  QCOMPARE(handler.dispatchOperationPlan(unknown), first);
  auto reusedPlan = unknown;
  reusedPlan.planId = "another-plan";
  QCOMPARE(handler.dispatchOperationPlan(reusedPlan).errorCode(),
           std::optional(pros::domain::CommandErrorCode::idempotency_key_reused));
  auto reusedRevision = unknown;
  reusedRevision.expectedRevision = pros::domain::Revision(1);
  QCOMPARE(handler.dispatchOperationPlan(reusedRevision).errorCode(),
           std::optional(pros::domain::CommandErrorCode::idempotency_key_reused));
  QCOMPARE(countRows(path, "domain_events"), 4);
  QCOMPARE(countRows(path, "outbox_records"), 4);
  QCOMPARE(countRows(path, "activity_facts"), 4);
  QCOMPARE(countRows(path, "operation_records"), 4);
  QCOMPARE(textValue(path, "SELECT aggregate_type FROM domain_events WHERE operation_id='dispatch-1';"),
           std::string("DispatchAudit"));
  const auto knownPayload = QJsonDocument::fromJson(
      QByteArray::fromStdString(textValue(path, "SELECT payload FROM domain_events WHERE operation_id='dispatch-3';")));
  QVERIFY(knownPayload.isObject());
  QCOMPARE(knownPayload.object().value("plan_id").toString(), QString("known-plan"));
  QCOMPARE(knownPayload.object().value("expected_revision").toInteger(), 1);
}

void ApprovalCommandHandlerTest::rejectsInvalidDecisionAndReplays() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.path() + "/invalid-decision.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  pros::infrastructure::SqliteApprovalCommandHandler handler(path);
  QVERIFY(handler.createOperationPlan({{"user", "create-plan"}, pros::domain::Revision(0), "plan-1", "summary"})
              .isSuccess());
  const std::string digest = textValue(path, "SELECT plan_digest FROM operation_plans WHERE id='plan-1';");
  using DecisionValue = std::underlying_type_t<pros::domain::ApprovalDecision>;
  const DecisionValue invalidValue = 99;
  pros::domain::ApprovalDecision invalidDecision = pros::domain::ApprovalDecision::pending;
  static_assert(sizeof(invalidDecision) == sizeof(invalidValue));
  std::memcpy(&invalidDecision, &invalidValue, sizeof(invalidDecision));
  const pros::application::RecordApproval invalid{{"user", "invalid-decision"},
                                                  pros::domain::Revision(0),
                                                  "approval-1",
                                                  "plan-1",
                                                  pros::domain::Revision(1),
                                                  digest,
                                                  invalidDecision,
                                                  "note"};
  const auto first = handler.recordApproval(invalid);
  QCOMPARE(first.errorCode(), std::optional(pros::domain::CommandErrorCode::invalid_argument));
  QCOMPARE(handler.recordApproval(invalid), first);
  QCOMPARE(countRows(path, "approvals"), 0);
  QCOMPARE(countRows(path, "operation_records"), 2);
}

void ApprovalCommandHandlerTest::approvalRequiresExactPlanCoordinate() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.path() + "/approval-coordinate.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  pros::infrastructure::SqliteApprovalCommandHandler handler(path);
  QVERIFY(handler.createOperationPlan({{"user", "create-plan"}, pros::domain::Revision(0), "plan-1", "summary"})
              .isSuccess());
  const std::string digest = textValue(path, "SELECT plan_digest FROM operation_plans WHERE id='plan-1';");
  QVERIFY(!digest.empty());

  const auto wrongRevision = handler.recordApproval({{"user", "wrong-revision"},
                                                     pros::domain::Revision(0),
                                                     "approval-1",
                                                     "plan-1",
                                                     pros::domain::Revision(2),
                                                     digest,
                                                     pros::domain::ApprovalDecision::approved,
                                                     "note"});
  QCOMPARE(wrongRevision.errorCode(), std::optional(pros::domain::CommandErrorCode::revision_conflict));
  const auto wrongDigest = handler.recordApproval({{"user", "wrong-digest"},
                                                   pros::domain::Revision(0),
                                                   "approval-2",
                                                   "plan-1",
                                                   pros::domain::Revision(1),
                                                   "wrong-digest",
                                                   pros::domain::ApprovalDecision::approved,
                                                   "note"});
  QCOMPARE(wrongDigest.errorCode(), std::optional(pros::domain::CommandErrorCode::revision_conflict));
  QCOMPARE(countRows(path, "approvals"), 0);
}

void ApprovalCommandHandlerTest::damagedPlanDigestIsStorageFailureAndRetryable_data() {
  QTest::addColumn<QString>("corruptSql");
  QTest::newRow("embedded-nul") << QString(
      "UPDATE operation_plans SET plan_digest=CAST(X'62616400646967657374' AS TEXT) WHERE id='plan-1';");
  QTest::newRow("truncated-utf8") << QString(
      "UPDATE operation_plans SET plan_digest=CAST(X'E282' AS TEXT) WHERE id='plan-1';");
}

void ApprovalCommandHandlerTest::damagedPlanDigestIsStorageFailureAndRetryable() {
  QFETCH(QString, corruptSql);
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.path() + "/damaged-plan.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  pros::infrastructure::SqliteApprovalCommandHandler handler(path);
  QVERIFY(handler.createOperationPlan({{"user", "create-plan"}, pros::domain::Revision(0), "plan-1", "summary"})
              .isSuccess());
  const std::string digest = textValue(path, "SELECT plan_digest FROM operation_plans WHERE id='plan-1';");
  QVERIFY(!digest.empty());
  QVERIFY(executeSql(path, corruptSql.toUtf8().constData()));
  const pros::application::RecordApproval command{{"user", "record-after-repair"},
                                                  pros::domain::Revision(0),
                                                  "approval-1",
                                                  "plan-1",
                                                  pros::domain::Revision(1),
                                                  digest,
                                                  pros::domain::ApprovalDecision::approved,
                                                  "note"};
  QCOMPARE(handler.recordApproval(command).errorCode(),
           std::optional(pros::domain::CommandErrorCode::storage_unavailable));
  QCOMPARE(countRows(path, "operation_records"), 1);
  const std::string repair = "UPDATE operation_plans SET plan_digest='" + digest + "' WHERE id='plan-1';";
  QVERIFY(executeSql(path, repair.c_str()));
  QVERIFY(handler.recordApproval(command).isSuccess());
  QCOMPARE(countRows(path, "approvals"), 1);
  QCOMPARE(countRows(path, "operation_records"), 2);
}

void ApprovalCommandHandlerTest::rejectedOperationReplaysAfterStateChanges() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.path() + "/rejected-replay.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  pros::infrastructure::SqliteApprovalCommandHandler handler(path);
  const pros::application::RecordApproval command{{"user", "approval-before-plan"},
                                                  pros::domain::Revision(0),
                                                  "approval-1",
                                                  "plan-1",
                                                  pros::domain::Revision(1),
                                                  "digest-before-plan",
                                                  pros::domain::ApprovalDecision::approved,
                                                  "note"};

  const auto first = handler.recordApproval(command);
  QCOMPARE(first.errorCode(), std::optional(pros::domain::CommandErrorCode::invalid_argument));
  QVERIFY(handler.createOperationPlan({{"user", "create-plan"}, pros::domain::Revision(0), "plan-1", "summary"})
              .isSuccess());
  QCOMPARE(handler.recordApproval(command), first);
  QCOMPARE(countRows(path, "approvals"), 0);
  QCOMPARE(countRows(path, "operation_records"), 2);
}

void ApprovalCommandHandlerTest::digestCoversAllCommandFields() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.path() + "/digest.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  pros::infrastructure::SqliteApprovalCommandHandler handler(path);
  const auto createDigestChanged = [&handler](const pros::application::CreateOperationPlan &original,
                                              const pros::application::CreateOperationPlan &changed) {
    return handler.createOperationPlan(original).isSuccess() &&
           handler.createOperationPlan(changed).errorCode() == pros::domain::CommandErrorCode::idempotency_key_reused;
  };
  const pros::application::CreateOperationPlan createBase{
      {"user", "create-plan-id"}, pros::domain::Revision(0), "plan-id-a", "summary"};
  auto createChanged = createBase;
  createChanged.planId = "plan-id-b";
  QVERIFY(createDigestChanged(createBase, createChanged));
  const pros::application::CreateOperationPlan summaryBase{
      {"user", "create-summary"}, pros::domain::Revision(0), "plan-summary", "summary-a"};
  createChanged = summaryBase;
  createChanged.summary = "summary-b";
  QVERIFY(createDigestChanged(summaryBase, createChanged));
  const pros::application::CreateOperationPlan revisionBase{
      {"user", "create-revision"}, pros::domain::Revision(0), "plan-revision", "summary"};
  createChanged = revisionBase;
  createChanged.expectedRevision = pros::domain::Revision(1);
  QVERIFY(createDigestChanged(revisionBase, createChanged));

  QVERIFY(handler
              .createOperationPlan(
                  {{"user", "create-approval-plan"}, pros::domain::Revision(0), "approval-plan", "summary"})
              .isSuccess());
  const std::string planDigest = textValue(path, "SELECT plan_digest FROM operation_plans WHERE id='approval-plan';");
  QVERIFY(!planDigest.empty());
  const auto approvalDigestChanged = [&handler](const pros::application::RecordApproval &original,
                                                const pros::application::RecordApproval &changed) {
    return handler.recordApproval(original).isSuccess() &&
           handler.recordApproval(changed).errorCode() == pros::domain::CommandErrorCode::idempotency_key_reused;
  };
  const auto approvalBase = [&planDigest](std::string operationId, std::string approvalId) {
    return pros::application::RecordApproval{{"user", std::move(operationId)},
                                             pros::domain::Revision(0),
                                             std::move(approvalId),
                                             "approval-plan",
                                             pros::domain::Revision(1),
                                             planDigest,
                                             pros::domain::ApprovalDecision::approved,
                                             "note"};
  };
  auto originalApproval = approvalBase("approval-id", "approval-id-a");
  auto changedApproval = originalApproval;
  changedApproval.approvalId = "approval-id-b";
  QVERIFY(approvalDigestChanged(originalApproval, changedApproval));
  originalApproval = approvalBase("approval-plan-id", "approval-plan-id");
  changedApproval = originalApproval;
  changedApproval.planId = "another-plan";
  QVERIFY(approvalDigestChanged(originalApproval, changedApproval));
  originalApproval = approvalBase("approval-plan-revision", "approval-plan-revision");
  changedApproval = originalApproval;
  changedApproval.planRevision = pros::domain::Revision(2);
  QVERIFY(approvalDigestChanged(originalApproval, changedApproval));
  originalApproval = approvalBase("approval-plan-digest", "approval-plan-digest");
  changedApproval = originalApproval;
  changedApproval.planDigest = "another-digest";
  QVERIFY(approvalDigestChanged(originalApproval, changedApproval));
  originalApproval = approvalBase("approval-decision", "approval-decision");
  changedApproval = originalApproval;
  changedApproval.decision = pros::domain::ApprovalDecision::rejected;
  QVERIFY(approvalDigestChanged(originalApproval, changedApproval));
  originalApproval = approvalBase("approval-note", "approval-note");
  changedApproval = originalApproval;
  changedApproval.note = "another-note";
  QVERIFY(approvalDigestChanged(originalApproval, changedApproval));
  originalApproval = approvalBase("approval-revision", "approval-revision");
  changedApproval = originalApproval;
  changedApproval.expectedRevision = pros::domain::Revision(1);
  QVERIFY(approvalDigestChanged(originalApproval, changedApproval));

  const auto dispatchDigestChanged = [&handler](const pros::application::DispatchOperationPlan &original,
                                                const pros::application::DispatchOperationPlan &changed) {
    return handler.dispatchOperationPlan(original).errorCode() ==
               pros::domain::CommandErrorCode::unsupported_in_version &&
           handler.dispatchOperationPlan(changed).errorCode() == pros::domain::CommandErrorCode::idempotency_key_reused;
  };
  const pros::application::DispatchOperationPlan dispatchPlanBase{
      {"user", "dispatch-plan"}, pros::domain::Revision(0), "plan-a"};
  auto changedDispatch = dispatchPlanBase;
  changedDispatch.planId = "plan-b";
  QVERIFY(dispatchDigestChanged(dispatchPlanBase, changedDispatch));
  const pros::application::DispatchOperationPlan dispatchRevisionBase{
      {"user", "dispatch-revision"}, pros::domain::Revision(0), "plan-a"};
  changedDispatch = dispatchRevisionBase;
  changedDispatch.expectedRevision = pros::domain::Revision(1);
  QVERIFY(dispatchDigestChanged(dispatchRevisionBase, changedDispatch));
}

void ApprovalCommandHandlerTest::storageFailureRollsBackLedgerAndFacts() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.path() + "/storage-failure.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  pros::infrastructure::SqliteApprovalCommandHandler handler(path);

  QVERIFY(executeSql(path, "CREATE TRIGGER fail_plan BEFORE INSERT ON operation_plans "
                           "BEGIN SELECT RAISE(ABORT, 'fail plan'); END;"));
  QCOMPARE(handler.createOperationPlan({{"user", "failed-plan"}, pros::domain::Revision(0), "plan-1", "summary"})
               .errorCode(),
           std::optional(pros::domain::CommandErrorCode::storage_unavailable));
  QVERIFY(executeSql(path, "DROP TRIGGER fail_plan;"));
  QVERIFY(handler.createOperationPlan({{"user", "create-plan"}, pros::domain::Revision(0), "plan-1", "summary"})
              .isSuccess());
  const std::string digest = textValue(path, "SELECT plan_digest FROM operation_plans WHERE id='plan-1';");
  QVERIFY(executeSql(path, "CREATE TRIGGER fail_approval BEFORE INSERT ON approvals "
                           "BEGIN SELECT RAISE(ABORT, 'fail approval'); END;"));
  QCOMPARE(handler
               .recordApproval({{"user", "failed-approval"},
                                pros::domain::Revision(0),
                                "approval-1",
                                "plan-1",
                                pros::domain::Revision(1),
                                digest,
                                pros::domain::ApprovalDecision::approved,
                                "note"})
               .errorCode(),
           std::optional(pros::domain::CommandErrorCode::storage_unavailable));
  QVERIFY(executeSql(path, "DROP TRIGGER fail_approval; CREATE TRIGGER fail_outbox BEFORE INSERT ON outbox_records "
                           "BEGIN SELECT RAISE(ABORT, 'fail outbox'); END;"));
  QCOMPARE(
      handler.dispatchOperationPlan({{"user", "failed-dispatch"}, pros::domain::Revision(0), "plan-1"}).errorCode(),
      std::optional(pros::domain::CommandErrorCode::storage_unavailable));
  QCOMPARE(countRows(path, "operation_plans"), 1);
  QCOMPARE(countRows(path, "approvals"), 0);
  QCOMPARE(countRows(path, "operation_records"), 1);
  QCOMPARE(countRows(path, "domain_events"), 1);
  QCOMPARE(countRows(path, "outbox_records"), 1);
  QCOMPARE(countRows(path, "activity_facts"), 1);
}

QTEST_APPLESS_MAIN(ApprovalCommandHandlerTest)

#include "approval_command_handler_test.moc"
