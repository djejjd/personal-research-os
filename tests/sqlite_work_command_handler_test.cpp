#include "pros/application/work_commands.h"
#include "pros/infrastructure/schema_migrator.h"
#include "pros/infrastructure/sqlite_work_command_handler.h"
#include "pros/infrastructure/sqlite_work_query_service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

#include <string>

namespace {

using pros::application::CreateDirection;
using pros::application::CreateMilestone;
using pros::application::CreateProject;
using pros::application::CreateTask;
using pros::application::UpdateTask;
using pros::application::WorkCommandEnvelope;
using pros::application::WorkQueryStatus;
using pros::domain::CommandErrorCode;
using pros::domain::CommandResult;
using pros::domain::OperationKey;
using pros::domain::Revision;
using pros::domain::TaskStatus;
using pros::infrastructure::SchemaMigrator;
using pros::infrastructure::SqliteWorkCommandHandler;
using pros::infrastructure::SqliteWorkQueryService;

QString initializedDatabase(QTemporaryDir &directory) {
  if (!directory.isValid())
    return {};
  const QString path = directory.filePath("work.sqlite3");
  QString error;
  return SchemaMigrator().migrate(path, &error) ? path : QString{};
}

int scalar(const QString &path, const std::string &sql) {
  sqlite3 *database = nullptr;
  const QByteArray encoded = path.toUtf8();
  if (sqlite3_open_v2(encoded.constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (database != nullptr)
      sqlite3_close(database);
    return -1;
  }
  sqlite3_stmt *statement = nullptr;
  int value = -1;
  if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_INTEGER)
    value = sqlite3_column_int(statement, 0);
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return value;
}

bool executeSql(const QString &path, const char *sql) {
  sqlite3 *database = nullptr;
  const QByteArray encoded = path.toUtf8();
  if (sqlite3_open_v2(encoded.constData(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    if (database != nullptr)
      sqlite3_close(database);
    return false;
  }
  const bool succeeded = sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
  sqlite3_close(database);
  return succeeded;
}

WorkCommandEnvelope envelope(const char *operationId, std::int64_t revision = 0) {
  return {OperationKey("test-caller", operationId), Revision(revision)};
}

void compareSuccess(const CommandResult &actual, const char *id, std::int64_t revision) {
  QCOMPARE(actual, CommandResult::succeeded(id, Revision(revision)));
}

void compareRejected(const CommandResult &actual, CommandErrorCode code) {
  QCOMPARE(actual, CommandResult::rejected(code));
}

template <typename Aggregate> const Aggregate *queryValue(const pros::application::WorkQueryResult<Aggregate> &result) {
  const auto &value = result.value();
  if (!value.has_value())
    return nullptr;
  return &*value;
}

} // namespace

class SqliteWorkCommandHandlerTest final : public QObject {
  Q_OBJECT

private slots:
  void writesAllFourAggregatesWithoutCompletingProject();
  void enforcesTaskCompareAndSwap();
  void replaysSuccessWithoutDuplicatingFacts();
  void rejectsReusedOperationWhenAnyDigestFieldChanges();
  void replaysFailureAfterPrerequisiteChanges();
  void recordsPrimaryKeyConstraintAsReplayableRejection();
  void mapsEveryWriteStorageFailureToStableResult();
  void rollsBackTriggerFailuresWithoutRecordingOperation();
  void validatesUtf8AndEmbeddedNulBeforePersistence();
  void rejectsInvalidOperationKeysBeforeTransaction();
  void queriesAggregatesAndDistinguishesFailureStates();
};

void SqliteWorkCommandHandlerTest::writesAllFourAggregatesWithoutCompletingProject() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  const SqliteWorkCommandHandler handler(path);
  const SqliteWorkQueryService queries(path);

  compareSuccess(handler.handle(CreateProject{envelope("project"), "project-1", "研究项目"}), "project-1", 1);
  compareSuccess(handler.handle(CreateTask{envelope("task"), "task-1", "project-1", "验证任务"}), "task-1", 1);
  compareSuccess(handler.handle(UpdateTask{envelope("task-update", 1), "task-1", TaskStatus::completed}), "task-1", 2);
  compareSuccess(handler.handle(CreateMilestone{envelope("milestone"), "milestone-1", "project-1", "首个里程碑"}),
                 "milestone-1", 1);
  compareSuccess(handler.handle(CreateDirection{envelope("direction"), "direction-1", "本地优先"}), "direction-1", 1);

  const auto project = queries.project("project-1");
  QCOMPARE(project.status(), WorkQueryStatus::found);
  const auto *projectValue = queryValue(project);
  QVERIFY(projectValue != nullptr);
  QCOMPARE(projectValue->title(), std::string("研究项目"));
  QCOMPARE(projectValue->status(), pros::domain::ProjectStatus::active);
  QCOMPARE(projectValue->revision(), Revision(1));

  const auto task = queries.task("task-1");
  QCOMPARE(task.status(), WorkQueryStatus::found);
  const auto *taskValue = queryValue(task);
  QVERIFY(taskValue != nullptr);
  QCOMPARE(taskValue->projectId(), std::string("project-1"));
  QCOMPARE(taskValue->status(), TaskStatus::completed);
  QCOMPARE(taskValue->revision(), Revision(2));

  QCOMPARE(queries.milestone("milestone-1").status(), WorkQueryStatus::found);
  QCOMPARE(queries.direction("direction-1").status(), WorkQueryStatus::found);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM domain_events;"), 5);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM outbox_records;"), 5);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM activity_facts;"), 5);
}

void SqliteWorkCommandHandlerTest::enforcesTaskCompareAndSwap() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  const SqliteWorkCommandHandler handler(path);
  const SqliteWorkQueryService queries(path);
  compareSuccess(handler.handle(CreateProject{envelope("project"), "project-1", "研究项目"}), "project-1", 1);
  compareSuccess(handler.handle(CreateTask{envelope("task"), "task-1", "project-1", "验证任务"}), "task-1", 1);

  compareRejected(handler.handle(UpdateTask{envelope("stale", 0), "task-1", TaskStatus::completed}),
                  CommandErrorCode::revision_conflict);
  const auto task = queries.task("task-1");
  QCOMPARE(task.status(), WorkQueryStatus::found);
  const auto *taskValue = queryValue(task);
  QVERIFY(taskValue != nullptr);
  QCOMPARE(taskValue->status(), TaskStatus::open);
  QCOMPARE(taskValue->revision(), Revision(1));
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM operation_records WHERE operation_id = 'stale';"), 1);
}

void SqliteWorkCommandHandlerTest::replaysSuccessWithoutDuplicatingFacts() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  const SqliteWorkCommandHandler handler(path);
  const SqliteWorkQueryService queries(path);
  const CreateDirection command{envelope("same"), "direction-1", "本地优先"};

  compareSuccess(handler.handle(command), "direction-1", 1);
  compareSuccess(handler.handle(command), "direction-1", 1);
  QCOMPARE(queries.direction("direction-1").status(), WorkQueryStatus::found);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM operation_records;"), 1);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM domain_events;"), 1);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM outbox_records;"), 1);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM activity_facts;"), 1);
  QCOMPARE(scalar(path, "SELECT COUNT(DISTINCT event_id) FROM domain_events;"), 1);
}

void SqliteWorkCommandHandlerTest::rejectsReusedOperationWhenAnyDigestFieldChanges() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  const SqliteWorkCommandHandler handler(path);
  const SqliteWorkQueryService queries(path);
  compareSuccess(handler.handle(CreateProject{envelope("same"), "project-1", "标题一"}), "project-1", 1);

  compareRejected(handler.handle(CreateProject{envelope("same"), "project-1", "标题二"}),
                  CommandErrorCode::idempotency_key_reused);
  compareRejected(handler.handle(CreateProject{envelope("same", 1), "project-1", "标题一"}),
                  CommandErrorCode::idempotency_key_reused);
  compareRejected(handler.handle(CreateProject{envelope("same"), "project-2", "标题一"}),
                  CommandErrorCode::idempotency_key_reused);
  QCOMPARE(queries.project("project-1").status(), WorkQueryStatus::found);
  QCOMPARE(queries.project("project-2").status(), WorkQueryStatus::not_found);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM domain_events;"), 1);
}

void SqliteWorkCommandHandlerTest::replaysFailureAfterPrerequisiteChanges() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  const SqliteWorkCommandHandler handler(path);
  const SqliteWorkQueryService queries(path);
  const CreateTask command{envelope("missing-parent"), "task-1", "project-1", "验证任务"};

  compareRejected(handler.handle(command), CommandErrorCode::invalid_argument);
  QCOMPARE(queries.task("task-1").status(), WorkQueryStatus::not_found);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM operation_records WHERE operation_id = 'missing-parent';"), 1);

  compareSuccess(handler.handle(CreateProject{envelope("create-parent"), "project-1", "研究项目"}), "project-1", 1);
  compareRejected(handler.handle(command), CommandErrorCode::invalid_argument);
  QCOMPARE(queries.task("task-1").status(), WorkQueryStatus::not_found);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM domain_events;"), 1);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM outbox_records;"), 1);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM activity_facts;"), 1);
}

void SqliteWorkCommandHandlerTest::recordsPrimaryKeyConstraintAsReplayableRejection() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  const SqliteWorkCommandHandler handler(path);
  const SqliteWorkQueryService queries(path);
  compareSuccess(handler.handle(CreateProject{envelope("first"), "project-1", "原始标题"}), "project-1", 1);
  const CreateProject duplicate{envelope("duplicate"), "project-1", "重复标题"};

  compareRejected(handler.handle(duplicate), CommandErrorCode::invalid_argument);
  compareRejected(handler.handle(duplicate), CommandErrorCode::invalid_argument);
  const auto project = queries.project("project-1");
  QCOMPARE(project.status(), WorkQueryStatus::found);
  const auto *projectValue = queryValue(project);
  QVERIFY(projectValue != nullptr);
  QCOMPARE(projectValue->title(), std::string("原始标题"));
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM operation_records WHERE operation_id = 'duplicate';"), 1);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM domain_events;"), 1);
}

void SqliteWorkCommandHandlerTest::mapsEveryWriteStorageFailureToStableResult() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString missingPath = directory.filePath("missing.sqlite3");
  const SqliteWorkCommandHandler handler(missingPath);

  compareRejected(handler.handle(CreateProject{envelope("project"), "project-1", "研究项目"}),
                  CommandErrorCode::storage_unavailable);
  compareRejected(handler.handle(CreateTask{envelope("task"), "task-1", "project-1", "验证任务"}),
                  CommandErrorCode::storage_unavailable);
  compareRejected(handler.handle(UpdateTask{envelope("update", 1), "task-1", TaskStatus::completed}),
                  CommandErrorCode::storage_unavailable);
  compareRejected(handler.handle(CreateMilestone{envelope("milestone"), "milestone-1", "project-1", "里程碑"}),
                  CommandErrorCode::storage_unavailable);
  compareRejected(handler.handle(CreateDirection{envelope("direction"), "direction-1", "本地优先"}),
                  CommandErrorCode::storage_unavailable);
}

void SqliteWorkCommandHandlerTest::rollsBackTriggerFailuresWithoutRecordingOperation() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  QVERIFY(executeSql(path, "CREATE TRIGGER reject_direction BEFORE INSERT ON directions "
                           "BEGIN SELECT RAISE(FAIL, 'injected'); END;"));
  const SqliteWorkCommandHandler handler(path);
  const SqliteWorkQueryService queries(path);

  const CreateDirection command{envelope("trigger-failure"), "direction-1", "本地优先"};
  compareRejected(handler.handle(command), CommandErrorCode::storage_unavailable);
  compareRejected(handler.handle(command), CommandErrorCode::storage_unavailable);
  QCOMPARE(queries.direction("direction-1").status(), WorkQueryStatus::not_found);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM operation_records WHERE operation_id = 'trigger-failure';"), 0);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM domain_events;"), 0);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM outbox_records;"), 0);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM activity_facts;"), 0);
}

void SqliteWorkCommandHandlerTest::validatesUtf8AndEmbeddedNulBeforePersistence() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  const SqliteWorkCommandHandler handler(path);
  const SqliteWorkQueryService queries(path);

  compareSuccess(handler.handle(CreateProject{envelope("unicode"), "项目-α", "研究计划"}), "项目-α", 1);
  const auto unicode = queries.project("项目-α");
  QCOMPARE(unicode.status(), WorkQueryStatus::found);
  const auto *unicodeValue = queryValue(unicode);
  QVERIFY(unicodeValue != nullptr);
  QCOMPARE(unicodeValue->title(), std::string("研究计划"));

  const std::string nulTitle("bad\0title", 9);
  const CreateDirection nulCommand{envelope("nul-field"), "direction-1", nulTitle};
  compareRejected(handler.handle(nulCommand), CommandErrorCode::invalid_argument);
  compareRejected(handler.handle(nulCommand), CommandErrorCode::invalid_argument);

  const std::string invalidUtf8("\xC3\x28", 2);
  const CreateDirection utf8Command{envelope("bad-utf8"), "direction-2", invalidUtf8};
  compareRejected(handler.handle(utf8Command), CommandErrorCode::invalid_argument);
  compareRejected(handler.handle(utf8Command), CommandErrorCode::invalid_argument);
  QCOMPARE(queries.direction("direction-1").status(), WorkQueryStatus::not_found);
  QCOMPARE(queries.direction("direction-2").status(), WorkQueryStatus::not_found);
  QCOMPARE(queries.direction(nulTitle).status(), WorkQueryStatus::invalid_argument);
  QCOMPARE(queries.direction(invalidUtf8).status(), WorkQueryStatus::invalid_argument);
  const std::string truncatedUtf8("\xE2\x82", 2);
  const CreateDirection truncatedCommand{envelope("truncated-utf8"), "direction-3", truncatedUtf8};
  compareRejected(handler.handle(truncatedCommand), CommandErrorCode::invalid_argument);
  compareRejected(handler.handle(truncatedCommand), CommandErrorCode::invalid_argument);
  QCOMPARE(queries.direction("direction-3").status(), WorkQueryStatus::not_found);
  QCOMPARE(queries.direction(truncatedUtf8).status(), WorkQueryStatus::invalid_argument);
  QCOMPARE(
      scalar(path,
             "SELECT COUNT(*) FROM operation_records WHERE operation_id IN ('nul-field','bad-utf8','truncated-utf8');"),
      3);
}

void SqliteWorkCommandHandlerTest::rejectsInvalidOperationKeysBeforeTransaction() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  const SqliteWorkCommandHandler handler(path);
  const SqliteWorkQueryService queries(path);

  const std::string nulCaller("caller\0shadow", 13);
  const CreateDirection nulKey{{OperationKey(nulCaller, "operation"), Revision(0)}, "direction-1", "标题"};
  compareRejected(handler.handle(nulKey), CommandErrorCode::invalid_argument);

  const std::string invalidUtf8("\xF0\x28\x8C\x28", 4);
  const CreateDirection utf8Key{{OperationKey("caller", invalidUtf8), Revision(0)}, "direction-2", "标题"};
  compareRejected(handler.handle(utf8Key), CommandErrorCode::invalid_argument);
  const std::string truncatedUtf8("\xE2\x82", 2);
  const CreateDirection truncatedKey{{OperationKey("caller", truncatedUtf8), Revision(0)}, "direction-3", "标题"};
  compareRejected(handler.handle(truncatedKey), CommandErrorCode::invalid_argument);
  QCOMPARE(queries.direction("direction-1").status(), WorkQueryStatus::not_found);
  QCOMPARE(queries.direction("direction-2").status(), WorkQueryStatus::not_found);
  QCOMPARE(queries.direction("direction-3").status(), WorkQueryStatus::not_found);
  QCOMPARE(scalar(path, "SELECT COUNT(*) FROM operation_records;"), 0);
}

void SqliteWorkCommandHandlerTest::queriesAggregatesAndDistinguishesFailureStates() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  const SqliteWorkCommandHandler handler(path);
  const SqliteWorkQueryService queries(path);
  compareSuccess(handler.handle(CreateProject{envelope("project"), "project-1", "研究项目"}), "project-1", 1);

  QCOMPARE(queries.project("missing").status(), WorkQueryStatus::not_found);
  QCOMPARE(queries.project(std::string("bad\0id", 6)).status(), WorkQueryStatus::invalid_argument);

  QTemporaryDir missingDirectory;
  QVERIFY(missingDirectory.isValid());
  QString error;
  const SqliteWorkQueryService missing(missingDirectory.filePath("missing.sqlite3"));
  QCOMPARE(missing.project("project-1", &error).status(), WorkQueryStatus::storage_unavailable);
  QVERIFY(!error.isEmpty());

  QVERIFY(executeSql(path, "UPDATE projects SET title = CAST(X'E282' AS TEXT) WHERE id = 'project-1';"));
  error.clear();
  QCOMPARE(queries.project("project-1", &error).status(), WorkQueryStatus::storage_unavailable);
  QVERIFY(!error.isEmpty());
}

QTEST_MAIN(SqliteWorkCommandHandlerTest)
#include "sqlite_work_command_handler_test.moc"
