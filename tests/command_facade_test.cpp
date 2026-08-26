#include "pros/infrastructure/command_composition.h"
#include "pros/infrastructure/governance_composition.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

#include <optional>
#include <string>

namespace {

std::string textValue(const QString &path, const char *sql) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    return {};
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    return {};
  }
  std::string value;
  if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_TEXT)
    value = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return value;
}

std::optional<pros::domain::CommandErrorCode> errorOf(const pros::domain::CommandResult &result) {
  return result.errorCode();
}

} // namespace

class CommandFacadeTest final : public QObject {
  Q_OBJECT

private slots:
  void runsCompleteS1FlowThroughOneFacade();
  void mapsUnavailableDatabaseToStructuredError();
};

void CommandFacadeTest::runsCompleteS1FlowThroughOneFacade() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.path() + QStringLiteral("/s1.sqlite");
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  QVERIFY2(migrator.migrate(path, &error), qPrintable(error));
  auto facade = pros::infrastructure::makeCommandFacade(path);

  const pros::application::WorkCommandEnvelope createProject{{"user", "create-project"}, pros::domain::Revision(0)};
  QVERIFY(facade->execute(pros::application::CreateProject{createProject, "project-1", "研究项目"}).isSuccess());
  const pros::application::WorkCommandEnvelope createTask{{"user", "create-task"}, pros::domain::Revision(0)};
  QVERIFY(facade->execute(pros::application::CreateTask{createTask, "task-1", "project-1", "验证任务"}).isSuccess());
  const pros::application::WorkCommandEnvelope createMilestone{{"user", "create-milestone"}, pros::domain::Revision(0)};
  QVERIFY(facade->execute(pros::application::CreateMilestone{createMilestone, "milestone-1", "project-1", "阶段目标"})
              .isSuccess());
  const pros::application::WorkCommandEnvelope createDirection{{"user", "create-direction"}, pros::domain::Revision(0)};
  QVERIFY(facade->execute(pros::application::CreateDirection{createDirection, "direction-1", "后续方向"}).isSuccess());

  QVERIFY(facade
              ->execute(pros::application::LinkNoteToTask{{"user", "link-note"},
                                                          pros::domain::Revision(0),
                                                          "task-1",
                                                          pros::domain::DocumentReference("note-1", "section-1")})
              .isSuccess());
  QVERIFY(facade
              ->execute(pros::application::RecordEvidence{{"user", "record-evidence"},
                                                          pros::domain::Revision(1),
                                                          "evidence-1",
                                                          "task-1",
                                                          "local://synthetic-evidence"})
              .isSuccess());
  QVERIFY(facade
              ->execute(pros::application::RecordAcceptance{{"user", "record-acceptance"},
                                                            pros::domain::Revision(2),
                                                            "acceptance-1",
                                                            "task-1",
                                                            pros::domain::Revision(1),
                                                            pros::domain::Revision(1),
                                                            pros::domain::AcceptanceConclusion::passed,
                                                            {{"evidence-1", pros::domain::Revision(1)}}})
              .isSuccess());

  const auto stale = facade->execute(pros::application::UpdateTask{
      {{"user", "stale-update"}, pros::domain::Revision(0)}, "task-1", pros::domain::TaskStatus::completed});
  QCOMPARE(errorOf(stale), std::optional(pros::domain::CommandErrorCode::revision_conflict));

  QVERIFY(facade
              ->execute(pros::application::CreateOperationPlan{
                  {"user", "create-plan"}, pros::domain::Revision(0), "plan-1", "仅保存的操作计划"})
              .isSuccess());
  const std::string planDigest = textValue(path, "SELECT plan_digest FROM operation_plans WHERE id='plan-1';");
  QVERIFY(!planDigest.empty());
  QVERIFY(facade
              ->execute(pros::application::RecordApproval{{"user", "record-approval"},
                                                          pros::domain::Revision(0),
                                                          "approval-1",
                                                          "plan-1",
                                                          pros::domain::Revision(1),
                                                          planDigest,
                                                          pros::domain::ApprovalDecision::approved,
                                                          "人工批准，仅保存"})
              .isSuccess());
  const auto dispatch = facade->execute(
      pros::application::DispatchOperationPlan{{"user", "dispatch"}, pros::domain::Revision(1), "plan-1"});
  QCOMPARE(errorOf(dispatch), std::optional(pros::domain::CommandErrorCode::unsupported_in_version));

  pros::infrastructure::GovernanceQuery query(path);
  const auto trace = query.traceForTask("task-1", &error);
  QVERIFY2(trace.has_value(), qPrintable(error));
  const auto value = trace.value_or(pros::domain::GovernanceTrace{"", pros::domain::Revision(0), {}, {}, {}, {}});
  QCOMPARE(value.revision.value(), 3);
  QCOMPARE(value.notes.size(), 1);
  QCOMPARE(value.evidence.size(), 1);
  QCOMPARE(value.acceptances.size(), 1);
  QCOMPARE(value.activities.size(), 3);
}

void CommandFacadeTest::mapsUnavailableDatabaseToStructuredError() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto facade = pros::infrastructure::makeCommandFacade(directory.path());
  const auto result = facade->execute(
      pros::application::CreateProject{{{"user", "unavailable"}, pros::domain::Revision(0)}, "project-1", "不会写入"});
  QCOMPARE(errorOf(result), std::optional(pros::domain::CommandErrorCode::storage_unavailable));
}

QTEST_APPLESS_MAIN(CommandFacadeTest)

#include "command_facade_test.moc"
