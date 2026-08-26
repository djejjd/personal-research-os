#include "pros/infrastructure/schema_migrator.h"
#include "pros/infrastructure/sqlite_command_transaction.h"

#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

#include <array>
#include <barrier>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

namespace {
using pros::domain::CommandErrorCode;
using pros::domain::CommandResult;
using pros::domain::OperationKey;
using pros::domain::Revision;
using pros::infrastructure::CommandFact;
using pros::infrastructure::CommandWorkResult;
using pros::infrastructure::SqliteCommandTransaction;

bool executeSql(const QString &path, const char *sql) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    if (database != nullptr)
      sqlite3_close(database);
    return false;
  }
  const bool succeeded = sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
  sqlite3_close(database);
  return succeeded;
}

int tableCount(const QString &path, const char *table) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    return -1;
  const std::string sql = "SELECT COUNT(*) FROM " + std::string(table) + ";";
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    return -1;
  }
  const int result = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return result;
}

QString scalarText(const QString &path, const char *sql) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    return {};
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    return {};
  }
  QString result;
  if (sqlite3_step(statement) == SQLITE_ROW) {
    const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
    if (value != nullptr)
      result = QString::fromUtf8(value);
  }
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return result;
}

QString initializedDatabase(QTemporaryDir &temporaryDirectory) {
  QString path = temporaryDirectory.path() + "/commands.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  if (!migrator.migrate(path, &error) ||
      !executeSql(path, "CREATE TABLE command_probe (id TEXT PRIMARY KEY, state INTEGER NOT NULL); "
                        "CREATE TABLE commit_parent (id TEXT PRIMARY KEY); "
                        "CREATE TABLE commit_probe (id TEXT PRIMARY KEY, parent_id TEXT NOT NULL REFERENCES "
                        "commit_parent(id) DEFERRABLE INITIALLY DEFERRED);"))
    return {};
  return path;
}

CommandFact fact(const char *eventId = "event-1") {
  return {eventId, "test.aggregate_written", "test.aggregate", "aggregate-1", Revision(1), 0, 1, "{}",
          "test",  "aggregate written"};
}

CommandWorkResult writeSuccess(sqlite3 *database, const char *eventId = "event-1") {
  if (sqlite3_exec(database, "INSERT INTO command_probe VALUES ('aggregate-1', 1);", nullptr, nullptr, nullptr) !=
      SQLITE_OK)
    return CommandWorkResult::storageFailure();
  return CommandWorkResult::completed(CommandResult::succeeded("aggregate-1", Revision(1)), {fact(eventId)});
}
} // namespace

class SqliteCommandTransactionTest final : public QObject {
  Q_OBJECT

private slots:
  void commitsSuccessAndReplaysWithoutCallingWork();
  void allocatesStableDeliveryCoordinates();
  void writesMultipleFactsAndReplaysOnce();
  void serializesConcurrentWriters();
  void recordsRejectedCommandAuditFact();
  void rejectsDuplicateAggregateEventCoordinate();
  void persistsAndReplaysDeterministicFailureAfterStateChanges();
  void rejectsReusedKeyWithDifferentDigest();
  void rejectsEmptyDigestWithoutCallingWork();
  void rejectsUnknownPersistedErrorCode();
  void rejectsDamagedOperationRows_data();
  void rejectsDamagedOperationRows();
  void rejectsInvalidFactSequences_data();
  void rejectsInvalidFactSequences();
  void doesNotPersistStorageUnavailable();
  void rollsBackCommitFailure();
  void reusesRolledBackDeliveryPositions();
  void rollsBackEveryWriteStepFailure_data();
  void rollsBackEveryWriteStepFailure();
};

void SqliteCommandTransactionTest::commitsSuccessAndReplaysWithoutCallingWork() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  SqliteCommandTransaction transaction(path);
  int calls = 0;
  const auto first = transaction.execute(OperationKey("caller", "success"), "digest-v1", [&](sqlite3 *database) {
    ++calls;
    return writeSuccess(database);
  });
  const auto expected = std::optional(CommandResult::succeeded("aggregate-1", Revision(1)));
  QCOMPARE(first, expected);
  const auto replay = transaction.execute(OperationKey("caller", "success"), "digest-v1", [&](sqlite3 *) {
    ++calls;
    return CommandWorkResult::storageFailure();
  });
  QCOMPARE(replay, first);
  QCOMPARE(calls, 1);
  for (const char *table : {"command_probe", "domain_events", "outbox_records", "activity_facts", "operation_records"})
    QCOMPARE(tableCount(path, table), 1);
}

void SqliteCommandTransactionTest::allocatesStableDeliveryCoordinates() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  SqliteCommandTransaction transaction(path);
  QVERIFY(transaction.execute(OperationKey("caller", "position-1"), "digest-1",
                              [](sqlite3 *database) { return writeSuccess(database, "event-position-1"); }));
  const auto second = transaction.execute(OperationKey("caller", "position-2"), "digest-2", [](sqlite3 *database) {
    if (sqlite3_exec(database, "INSERT INTO command_probe VALUES ('aggregate-2', 1);", nullptr, nullptr, nullptr) !=
        SQLITE_OK)
      return CommandWorkResult::storageFailure();
    return CommandWorkResult::completed(
        CommandResult::succeeded("aggregate-2", Revision(1)),
        {CommandFact{"event-position-2", "test.aggregate_written", "test.aggregate", "aggregate-2", Revision(1), 0, 1,
                     "{}", "test", "aggregate written"}});
  });
  QVERIFY(second.has_value());
  QCOMPARE(scalarText(path, "SELECT group_concat(delivery_position, ',') FROM "
                            "(SELECT delivery_position FROM domain_events ORDER BY delivery_position);"),
           QString("1,2"));
  QCOMPARE(scalarText(path, "SELECT group_concat(delivery_position, ',') FROM "
                            "(SELECT delivery_position FROM outbox_records ORDER BY delivery_position);"),
           QString("1,2"));
  QCOMPARE(scalarText(path, "SELECT group_concat(delivery_partition, ',') FROM "
                            "(SELECT delivery_partition FROM domain_events ORDER BY delivery_position);"),
           QString("global,global"));
}

void SqliteCommandTransactionTest::writesMultipleFactsAndReplaysOnce() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  SqliteCommandTransaction transaction(path);
  int calls = 0;
  const auto result = transaction.execute(OperationKey("caller", "multi"), "digest-multi", [&](sqlite3 *database) {
    ++calls;
    if (sqlite3_exec(database, "INSERT INTO command_probe VALUES ('aggregate-1', 2);", nullptr, nullptr, nullptr) !=
        SQLITE_OK)
      return CommandWorkResult::storageFailure();
    return CommandWorkResult::completed(CommandResult::succeeded("aggregate-1", Revision(2)),
                                        {CommandFact{"event-multi-0", "test.started", "test.aggregate", "aggregate-1",
                                                     Revision(2), 0, 1, "{}", "test", "started"},
                                         CommandFact{"event-multi-1", "test.finished", "test.aggregate", "aggregate-1",
                                                     Revision(2), 1, 1, "{}", "test", "finished"}});
  });
  QCOMPARE(result, std::optional(CommandResult::succeeded("aggregate-1", Revision(2))));
  const auto replay = transaction.execute(OperationKey("caller", "multi"), "digest-multi", [&](sqlite3 *) {
    ++calls;
    return CommandWorkResult::storageFailure();
  });
  QCOMPARE(replay, result);
  QCOMPARE(calls, 1);
  QCOMPARE(tableCount(path, "domain_events"), 2);
  QCOMPARE(tableCount(path, "outbox_records"), 2);
  QCOMPARE(tableCount(path, "activity_facts"), 2);
  QCOMPARE(tableCount(path, "operation_records"), 1);
  QCOMPARE(scalarText(path, "SELECT group_concat(event_index, ',') FROM "
                            "(SELECT event_index FROM domain_events ORDER BY delivery_position);"),
           QString("0,1"));
  QCOMPARE(scalarText(path, "SELECT next_position FROM delivery_sequence WHERE delivery_partition='global';"),
           QString("3"));
}

void SqliteCommandTransactionTest::serializesConcurrentWriters() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  std::barrier start(3);
  std::array<std::optional<CommandResult>, 2> results;
  std::array<std::thread, 2> writers;
  for (std::size_t index = 0; index < writers.size(); ++index) {
    writers[index] = std::thread([&, index] {
      try {
        start.arrive_and_wait();
        SqliteCommandTransaction transaction(path);
        const std::string suffix = std::to_string(index);
        results[index] = transaction.execute(
            OperationKey("concurrent", "operation-" + suffix), "digest-" + suffix, [&](sqlite3 *database) {
              std::this_thread::sleep_for(std::chrono::milliseconds(30));
              const std::string sql = "INSERT INTO command_probe VALUES ('aggregate-" + suffix + "', 1);";
              if (sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
                return CommandWorkResult::storageFailure();
              return CommandWorkResult::completed(
                  CommandResult::succeeded("aggregate-" + suffix, Revision(1)),
                  {CommandFact{"event-" + suffix, "test.written", "test.aggregate", "aggregate-" + suffix, Revision(1),
                               0, 1, "{}", "test", "written"}});
            });
      } catch (...) {
        results[index] = std::nullopt;
      }
    });
  }
  start.arrive_and_wait();
  for (std::thread &writer : writers)
    writer.join();
  QVERIFY(results[0].has_value());
  QVERIFY(results[1].has_value());
  QCOMPARE(scalarText(path, "SELECT group_concat(delivery_position, ',') FROM "
                            "(SELECT delivery_position FROM domain_events ORDER BY delivery_position);"),
           QString("1,2"));
  QCOMPARE(scalarText(path, "SELECT next_position FROM delivery_sequence WHERE delivery_partition='global';"),
           QString("3"));
}

void SqliteCommandTransactionTest::recordsRejectedCommandAuditFact() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  SqliteCommandTransaction transaction(path);
  int calls = 0;
  const auto rejected = transaction.execute(OperationKey("caller", "unsupported"), "digest-v1", [&](sqlite3 *) {
    ++calls;
    return CommandWorkResult::completed(
        CommandResult::rejected(CommandErrorCode::unsupported_in_version),
        {CommandFact{"event-rejected", "approval.dispatch_rejected", "operation_plan", "plan-1", Revision(1), 0, 1,
                     "{}", "dispatch_rejected", "unsupported_in_version"}});
  });
  QCOMPARE(rejected, std::optional(CommandResult::rejected(CommandErrorCode::unsupported_in_version)));
  const auto replay = transaction.execute(OperationKey("caller", "unsupported"), "digest-v1", [&](sqlite3 *) {
    ++calls;
    return CommandWorkResult::storageFailure();
  });
  QCOMPARE(replay, rejected);
  QCOMPARE(calls, 1);
  for (const char *table : {"domain_events", "outbox_records", "activity_facts", "operation_records"})
    QCOMPARE(tableCount(path, table), 1);
}

void SqliteCommandTransactionTest::rejectsDuplicateAggregateEventCoordinate() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  SqliteCommandTransaction transaction(path);
  QVERIFY(transaction.execute(OperationKey("caller", "first-coordinate"), "digest-1",
                              [](sqlite3 *database) { return writeSuccess(database, "event-coordinate-1"); }));
  QString error;
  const auto second = transaction.execute(
      OperationKey("caller", "second-coordinate"), "digest-2",
      [](sqlite3 *database) {
        if (sqlite3_exec(database, "INSERT INTO command_probe VALUES ('side-effect-2', 1);", nullptr, nullptr,
                         nullptr) != SQLITE_OK)
          return CommandWorkResult::storageFailure();
        return CommandWorkResult::completed(CommandResult::succeeded("aggregate-1", Revision(1)),
                                            {fact("event-coordinate-2")});
      },
      &error);
  QVERIFY(!second.has_value());
  QVERIFY(!error.isEmpty());
  QCOMPARE(tableCount(path, "command_probe"), 1);
  QCOMPARE(tableCount(path, "domain_events"), 1);
  QCOMPARE(tableCount(path, "operation_records"), 1);
  QCOMPARE(scalarText(path, "SELECT next_position FROM delivery_sequence WHERE delivery_partition='global';"),
           QString("2"));
}

void SqliteCommandTransactionTest::persistsAndReplaysDeterministicFailureAfterStateChanges() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  SqliteCommandTransaction transaction(path);
  int calls = 0;
  const auto first = transaction.execute(OperationKey("caller", "failure"), "digest-v1", [&](sqlite3 *) {
    ++calls;
    return CommandWorkResult::completed(CommandResult::rejected(CommandErrorCode::revision_conflict));
  });
  const auto expected = std::optional(CommandResult::rejected(CommandErrorCode::revision_conflict));
  QCOMPARE(first, expected);
  QCOMPARE(scalarText(path, "SELECT error_code FROM operation_records;"), QString("revision_conflict"));
  QVERIFY(executeSql(path, "INSERT INTO command_probe VALUES ('now-valid', 1);"));
  const auto replay = transaction.execute(OperationKey("caller", "failure"), "digest-v1", [&](sqlite3 *database) {
    ++calls;
    return writeSuccess(database, "event-after-change");
  });
  QCOMPARE(replay, first);
  QCOMPARE(calls, 1);
  QCOMPARE(tableCount(path, "operation_records"), 1);
  QCOMPARE(tableCount(path, "domain_events"), 0);
}

void SqliteCommandTransactionTest::rejectsReusedKeyWithDifferentDigest() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  SqliteCommandTransaction transaction(path);
  QVERIFY(transaction
              .execute(OperationKey("caller", "reuse"), "digest-v1",
                       [](sqlite3 *) {
                         return CommandWorkResult::completed(
                             CommandResult::rejected(CommandErrorCode::invalid_argument));
                       })
              .has_value());
  int calls = 0;
  const auto reused = transaction.execute(OperationKey("caller", "reuse"), "digest-v2", [&](sqlite3 *) {
    ++calls;
    return CommandWorkResult::storageFailure();
  });
  QCOMPARE(reused, std::optional(CommandResult::rejected(CommandErrorCode::idempotency_key_reused)));
  QCOMPARE(calls, 0);
}

void SqliteCommandTransactionTest::rejectsEmptyDigestWithoutCallingWork() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  SqliteCommandTransaction transaction(path);
  int calls = 0;
  QString error;
  const auto result = transaction.execute(
      OperationKey("caller", "empty"), "",
      [&](sqlite3 *) {
        ++calls;
        return CommandWorkResult::storageFailure();
      },
      &error);
  QVERIFY(!result.has_value());
  QVERIFY(!error.isEmpty());
  QCOMPARE(calls, 0);
  QCOMPARE(tableCount(path, "operation_records"), 0);
}

void SqliteCommandTransactionTest::rejectsUnknownPersistedErrorCode() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  QVERIFY(executeSql(path, "INSERT INTO operation_records VALUES "
                           "('caller', 'corrupt', 'digest-v1', 0, NULL, NULL, 'unknown_error');"));
  SqliteCommandTransaction transaction(path);
  QString error;
  int calls = 0;
  const auto result = transaction.execute(
      OperationKey("caller", "corrupt"), "digest-v1",
      [&](sqlite3 *) {
        ++calls;
        return CommandWorkResult::completed(CommandResult::rejected(CommandErrorCode::invalid_argument));
      },
      &error);
  QVERIFY(!result.has_value());
  QVERIFY(!error.isEmpty());
  QCOMPARE(calls, 0);
}

void SqliteCommandTransactionTest::rejectsDamagedOperationRows_data() {
  QTest::addColumn<QString>("insertSql");
  QTest::newRow("empty-digest") << QString("INSERT INTO operation_records VALUES "
                                           "('caller','damaged','',0,NULL,NULL,'invalid_argument');");
  QTest::newRow("nul-digest") << QString("INSERT INTO operation_records VALUES "
                                         "('caller','damaged',CAST(X'646967657374006576696C' AS TEXT),"
                                         "0,NULL,NULL,'invalid_argument');");
  QTest::newRow("truncated-digest") << QString("INSERT INTO operation_records VALUES "
                                               "('caller','damaged',CAST(X'E282' AS TEXT),"
                                               "0,NULL,NULL,'invalid_argument');");
  QTest::newRow("empty-aggregate") << QString("INSERT INTO operation_records VALUES "
                                              "('caller','damaged','digest',1,'',1,NULL);");
  QTest::newRow("nul-aggregate") << QString("INSERT INTO operation_records VALUES "
                                            "('caller','damaged','digest',1,CAST(X'616767006576696C' AS TEXT),"
                                            "1,NULL);");
  QTest::newRow("truncated-aggregate") << QString("INSERT INTO operation_records VALUES "
                                                  "('caller','damaged','digest',1,CAST(X'E282' AS TEXT),1,NULL);");
  QTest::newRow("wrong-succeeded-type") << QString(
      "PRAGMA ignore_check_constraints=ON; INSERT INTO operation_records VALUES "
      "('caller','damaged','digest','bad',NULL,NULL,'invalid_argument');");
  QTest::newRow("wrong-error-type") << QString("INSERT INTO operation_records VALUES "
                                               "('caller','damaged','digest',0,NULL,NULL,"
                                               "CAST('invalid_argument' AS BLOB));");
  QTest::newRow("stored-storage-error") << QString("INSERT INTO operation_records VALUES "
                                                   "('caller','damaged','digest',0,NULL,NULL,'storage_unavailable');");
  QTest::newRow("nul-error") << QString("INSERT INTO operation_records VALUES "
                                        "('caller','damaged','digest',0,NULL,NULL,"
                                        "CAST(X'696E76616C69645F617267756D656E74006576696C' AS TEXT));");
  QTest::newRow("truncated-error") << QString("INSERT INTO operation_records VALUES "
                                              "('caller','damaged','digest',0,NULL,NULL,CAST(X'E282' AS TEXT));");
  QTest::newRow("negative-revision") << QString(
      "PRAGMA ignore_check_constraints=ON; INSERT INTO operation_records VALUES "
      "('caller','damaged','digest',1,'aggregate',-1,NULL);");
}

void SqliteCommandTransactionTest::rejectsDamagedOperationRows() {
  QFETCH(QString, insertSql);
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  QVERIFY(executeSql(path, insertSql.toUtf8().constData()));
  SqliteCommandTransaction transaction(path);
  QString error;
  int calls = 0;
  const auto result = transaction.execute(
      OperationKey("caller", "damaged"), "digest",
      [&](sqlite3 *) {
        ++calls;
        return CommandWorkResult::completed(CommandResult::rejected(CommandErrorCode::invalid_argument));
      },
      &error);
  QVERIFY(!result.has_value());
  QVERIFY(!error.isEmpty());
  QCOMPARE(calls, 0);
  QVERIFY(executeSql(path, "DELETE FROM operation_records WHERE caller_id='caller' AND operation_id='damaged';"));
  const auto retry = transaction.execute(OperationKey("caller", "damaged"), "digest", [&](sqlite3 *) {
    ++calls;
    return CommandWorkResult::completed(CommandResult::rejected(CommandErrorCode::invalid_argument));
  });
  QCOMPARE(retry, std::optional(CommandResult::rejected(CommandErrorCode::invalid_argument)));
  QCOMPARE(calls, 1);
}

void SqliteCommandTransactionTest::rejectsInvalidFactSequences_data() {
  QTest::addColumn<QString>("mode");
  QTest::newRow("success-without-fact") << QString("empty");
  QTest::newRow("event-index-gap") << QString("gap");
  QTest::newRow("mixed-revisions") << QString("revision");
  QTest::newRow("empty-payload") << QString("payload");
  QTest::newRow("empty-summary") << QString("summary");
}

void SqliteCommandTransactionTest::rejectsInvalidFactSequences() {
  QFETCH(QString, mode);
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  SqliteCommandTransaction transaction(path);
  QString error;
  const auto result = transaction.execute(
      OperationKey("caller", "invalid-facts"), "digest",
      [&](sqlite3 *database) {
        if (sqlite3_exec(database, "INSERT INTO command_probe VALUES ('aggregate-1', 1);", nullptr, nullptr, nullptr) !=
            SQLITE_OK)
          return CommandWorkResult::storageFailure();
        if (mode == "empty")
          return CommandWorkResult::completed(CommandResult::succeeded("aggregate-1", Revision(1)));
        if (mode == "gap") {
          CommandFact invalid = fact();
          invalid.eventIndex = 1;
          return CommandWorkResult::completed(CommandResult::succeeded("aggregate-1", Revision(1)), {invalid});
        }
        if (mode == "payload") {
          CommandFact invalid = fact();
          invalid.payload.clear();
          return CommandWorkResult::completed(CommandResult::succeeded("aggregate-1", Revision(1)), {invalid});
        }
        if (mode == "summary") {
          CommandFact invalid = fact();
          invalid.activitySummary.clear();
          return CommandWorkResult::completed(CommandResult::succeeded("aggregate-1", Revision(1)), {invalid});
        }
        CommandFact first = fact("mixed-0");
        CommandFact second = fact("mixed-1");
        second.eventIndex = 1;
        second.revision = Revision(2);
        return CommandWorkResult::completed(CommandResult::succeeded("aggregate-1", Revision(1)), {first, second});
      },
      &error);
  QVERIFY(!result.has_value());
  QVERIFY(!error.isEmpty());
  QCOMPARE(tableCount(path, "command_probe"), 0);
  QCOMPARE(tableCount(path, "operation_records"), 0);
  QCOMPARE(scalarText(path, "SELECT next_position FROM delivery_sequence WHERE delivery_partition='global';"),
           QString("1"));
}

void SqliteCommandTransactionTest::doesNotPersistStorageUnavailable() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  SqliteCommandTransaction transaction(path);
  QString error;
  const auto result = transaction.execute(
      OperationKey("caller", "storage"), "digest",
      [](sqlite3 *) {
        return CommandWorkResult::completed(CommandResult::rejected(CommandErrorCode::storage_unavailable));
      },
      &error);
  QVERIFY(!result.has_value());
  QVERIFY(!error.isEmpty());
  QCOMPARE(tableCount(path, "operation_records"), 0);
}

void SqliteCommandTransactionTest::rollsBackCommitFailure() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  SqliteCommandTransaction transaction(path);
  QString error;
  const auto result = transaction.execute(
      OperationKey("caller", "commit-failure"), "digest",
      [](sqlite3 *database) {
        if (sqlite3_exec(database, "INSERT INTO commit_probe VALUES ('child', 'missing-parent');", nullptr, nullptr,
                         nullptr) != SQLITE_OK)
          return CommandWorkResult::storageFailure();
        return CommandWorkResult::completed(CommandResult::succeeded("aggregate-1", Revision(1)), {fact()});
      },
      &error);
  QVERIFY(!result.has_value());
  QVERIFY(!error.isEmpty());
  for (const char *table : {"commit_probe", "domain_events", "outbox_records", "activity_facts", "operation_records"})
    QCOMPARE(tableCount(path, table), 0);
  QCOMPARE(scalarText(path, "SELECT next_position FROM delivery_sequence WHERE delivery_partition='global';"),
           QString("1"));
}

void SqliteCommandTransactionTest::reusesRolledBackDeliveryPositions() {
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  QVERIFY(executeSql(path, "CREATE TRIGGER fail_outbox BEFORE INSERT ON outbox_records "
                           "BEGIN SELECT RAISE(FAIL, 'injected'); END;"));
  SqliteCommandTransaction transaction(path);
  QVERIFY(!transaction.execute(OperationKey("caller", "will-rollback"), "digest-1",
                               [](sqlite3 *database) { return writeSuccess(database); }));
  QVERIFY(executeSql(path, "DROP TRIGGER fail_outbox;"));
  const auto committed = transaction.execute(OperationKey("caller", "after-rollback"), "digest-2",
                                             [](sqlite3 *database) { return writeSuccess(database); });
  QVERIFY(committed.has_value());
  QCOMPARE(scalarText(path, "SELECT delivery_position FROM domain_events;"), QString("1"));
  QCOMPARE(scalarText(path, "SELECT next_position FROM delivery_sequence WHERE delivery_partition='global';"),
           QString("2"));
}

void SqliteCommandTransactionTest::rollsBackEveryWriteStepFailure_data() {
  QTest::addColumn<QString>("failedStep");
  for (const char *step : {"aggregate", "domain_events", "outbox_records", "activity_facts", "operation_records"})
    QTest::newRow(step) << QString::fromLatin1(step);
}

void SqliteCommandTransactionTest::rollsBackEveryWriteStepFailure() {
  QFETCH(QString, failedStep);
  QTemporaryDir directory;
  const QString path = initializedDatabase(directory);
  QVERIFY(!path.isEmpty());
  if (failedStep != "aggregate") {
    const QString trigger =
        "CREATE TRIGGER fail_step BEFORE INSERT ON " + failedStep + " BEGIN SELECT RAISE(FAIL, 'injected'); END;";
    QVERIFY(executeSql(path, trigger.toUtf8().constData()));
  }
  SqliteCommandTransaction transaction(path);
  QString error;
  const auto result = transaction.execute(
      OperationKey("caller", "rollback"), "digest-v1",
      [&](sqlite3 *database) {
        if (failedStep == "aggregate") {
          if (sqlite3_exec(database, "INSERT INTO command_probe VALUES ('partial', 1);", nullptr, nullptr, nullptr) !=
              SQLITE_OK)
            return CommandWorkResult::storageFailure();
          return CommandWorkResult::storageFailure();
        }
        return writeSuccess(database);
      },
      &error);
  QVERIFY(!result.has_value());
  QVERIFY(!error.isEmpty());
  for (const char *table : {"command_probe", "domain_events", "outbox_records", "activity_facts", "operation_records"})
    QCOMPARE(tableCount(path, table), 0);
}

QTEST_APPLESS_MAIN(SqliteCommandTransactionTest)

#include "sqlite_command_transaction_test.moc"
