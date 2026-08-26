#include "pros/infrastructure/schema_migrator.h"

#include "pros/domain/schema_version.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pros::infrastructure {
namespace {

bool execute(sqlite3 *database, const char *statement, QString *errorMessage) {
  char *sqliteError = nullptr;
  const int result = sqlite3_exec(database, statement, nullptr, nullptr, &sqliteError);
  if (result == SQLITE_OK) {
    return true;
  }

  if (errorMessage != nullptr) {
    *errorMessage = "数据库迁移操作失败";
  }
  sqlite3_free(sqliteError);
  return false;
}

struct TableDefinition final {
  const char *name;
  const char *createSql;
};

const std::array<TableDefinition, 16> &s1Tables() {
  static const std::array<TableDefinition, 16> tables{{
      {"operation_records",
       "CREATE TABLE IF NOT EXISTS operation_records (caller_id TEXT NOT NULL, operation_id TEXT NOT NULL, "
       "request_digest TEXT NOT NULL, succeeded INTEGER NOT NULL CHECK(succeeded IN (0, 1)), aggregate_id TEXT, "
       "revision INTEGER CHECK(revision >= 0), error_code TEXT, PRIMARY KEY(caller_id, operation_id), "
       "CHECK((succeeded = 1 AND aggregate_id IS NOT NULL AND revision IS NOT NULL AND error_code IS NULL) OR "
       "(succeeded = 0 AND aggregate_id IS NULL AND revision IS NULL AND error_code IS NOT NULL)));"},
      {"delivery_sequence",
       "CREATE TABLE IF NOT EXISTS delivery_sequence (delivery_partition TEXT PRIMARY KEY "
       "CHECK(delivery_partition = 'global'), next_position INTEGER NOT NULL CHECK(next_position > 0));"},
      {"domain_events",
       "CREATE TABLE IF NOT EXISTS domain_events (event_id TEXT PRIMARY KEY, delivery_partition TEXT NOT NULL "
       "CHECK(delivery_partition = 'global'), delivery_position INTEGER NOT NULL CHECK(delivery_position > 0), "
       "event_type "
       "TEXT NOT NULL, aggregate_type TEXT NOT NULL, aggregate_id TEXT NOT NULL, aggregate_revision INTEGER NOT NULL "
       "CHECK(aggregate_revision >= 0), event_index INTEGER NOT NULL CHECK(event_index >= 0), schema_version INTEGER "
       "NOT "
       "NULL CHECK(schema_version > 0), caller_id TEXT NOT NULL, operation_id TEXT NOT NULL, payload TEXT NOT NULL, "
       "UNIQUE(delivery_partition, delivery_position), UNIQUE(caller_id, operation_id, event_index), "
       "UNIQUE(event_id, delivery_partition, delivery_position), "
       "UNIQUE(aggregate_type, aggregate_id, aggregate_revision, event_index));"},
      {"outbox_records",
       "CREATE TABLE IF NOT EXISTS outbox_records (event_id TEXT PRIMARY KEY, "
       "delivery_partition TEXT NOT NULL CHECK(delivery_partition = 'global'), delivery_position INTEGER NOT NULL "
       "CHECK(delivery_position > 0), delivery_state TEXT NOT NULL CHECK(delivery_state IN ('pending', 'delivered', "
       "'failed')), first_attempt_at TEXT, last_attempt_at TEXT, error_summary TEXT, UNIQUE(delivery_partition, "
       "delivery_position), CHECK(last_attempt_at IS NULL OR first_attempt_at IS NOT NULL), CHECK(error_summary IS "
       "NULL OR last_attempt_at IS NOT NULL), FOREIGN KEY(event_id, delivery_partition, delivery_position) REFERENCES "
       "domain_events(event_id, delivery_partition, delivery_position));"},
      {"activity_facts",
       "CREATE TABLE IF NOT EXISTS activity_facts (id INTEGER PRIMARY KEY, event_id TEXT NOT NULL UNIQUE REFERENCES "
       "domain_events(event_id), kind TEXT NOT NULL, aggregate_id TEXT NOT NULL, revision INTEGER NOT NULL "
       "CHECK(revision >= 0), caller_id TEXT NOT NULL, operation_id TEXT NOT NULL, summary TEXT NOT NULL);"},
      {"projects",
       "CREATE TABLE IF NOT EXISTS projects (id TEXT PRIMARY KEY, title TEXT NOT NULL, status INTEGER NOT NULL "
       "CHECK(status IN (0, 1)), revision INTEGER NOT NULL CHECK(revision >= 0));"},
      {"tasks",
       "CREATE TABLE IF NOT EXISTS tasks (id TEXT PRIMARY KEY, project_id TEXT NOT NULL REFERENCES projects(id), title "
       "TEXT NOT NULL, status INTEGER NOT NULL CHECK(status IN (0, 1)), revision INTEGER NOT NULL CHECK(revision >= "
       "0));"},
      {"milestones",
       "CREATE TABLE IF NOT EXISTS milestones (id TEXT PRIMARY KEY, project_id TEXT NOT NULL REFERENCES projects(id), "
       "title TEXT NOT NULL, status INTEGER NOT NULL CHECK(status IN (0, 1)), revision INTEGER NOT NULL CHECK(revision "
       ">= 0));"},
      {"directions",
       "CREATE TABLE IF NOT EXISTS directions (id TEXT PRIMARY KEY, title TEXT NOT NULL, status INTEGER NOT NULL "
       "CHECK(status IN (0, 1)), revision INTEGER NOT NULL CHECK(revision >= 0));"},
      {"governance_targets",
       "CREATE TABLE IF NOT EXISTS governance_targets (task_id TEXT PRIMARY KEY REFERENCES tasks(id), revision "
       "INTEGER NOT NULL CHECK(revision >= 0));"},
      {"governance_note_links",
       "CREATE TABLE IF NOT EXISTS governance_note_links (task_id TEXT NOT NULL REFERENCES "
       "governance_targets(task_id), document_id TEXT NOT NULL, section_id TEXT NOT NULL, PRIMARY KEY(task_id, "
       "document_id, section_id));"},
      {"governance_evidence",
       "CREATE TABLE IF NOT EXISTS governance_evidence (evidence_id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES "
       "governance_targets(task_id), locator TEXT NOT NULL, revision INTEGER NOT NULL CHECK(revision >= 0), "
       "UNIQUE(evidence_id, task_id, revision));"},
      {"governance_acceptance",
       "CREATE TABLE IF NOT EXISTS governance_acceptance (acceptance_id TEXT PRIMARY KEY, task_id TEXT NOT NULL "
       "REFERENCES governance_targets(task_id), "
       "candidate_revision INTEGER NOT NULL CHECK(candidate_revision >= 0), spec_revision INTEGER NOT NULL "
       "CHECK(spec_revision >= 0), conclusion INTEGER NOT NULL CHECK(conclusion IN (0, 1, 2)), "
       "UNIQUE(acceptance_id, task_id));"},
      {"governance_acceptance_evidence",
       "CREATE TABLE IF NOT EXISTS governance_acceptance_evidence (acceptance_id TEXT NOT NULL, task_id TEXT NOT "
       "NULL, evidence_id TEXT NOT NULL, evidence_revision INTEGER NOT NULL CHECK(evidence_revision >= 0), "
       "PRIMARY KEY(acceptance_id, evidence_id, evidence_revision), FOREIGN KEY(acceptance_id, task_id) REFERENCES "
       "governance_acceptance(acceptance_id, task_id), FOREIGN KEY(evidence_id, task_id, evidence_revision) "
       "REFERENCES governance_evidence(evidence_id, task_id, revision));"},
      {"operation_plans",
       "CREATE TABLE IF NOT EXISTS operation_plans (id TEXT PRIMARY KEY, summary TEXT NOT NULL, plan_digest TEXT NOT "
       "NULL CHECK(length(plan_digest) > 0), revision INTEGER NOT NULL CHECK(revision >= 0), UNIQUE(id, revision, "
       "plan_digest));"},
      {"approvals",
       "CREATE TABLE IF NOT EXISTS approvals (id TEXT PRIMARY KEY, plan_id TEXT NOT NULL, plan_revision INTEGER NOT "
       "NULL CHECK(plan_revision >= 0), plan_digest TEXT NOT NULL CHECK(length(plan_digest) > 0), decision INTEGER NOT "
       "NULL CHECK(decision IN (0, 1, 2)), note TEXT NOT NULL, revision INTEGER NOT NULL CHECK(revision >= 0), "
       "FOREIGN KEY(plan_id, plan_revision, plan_digest) REFERENCES operation_plans(id, revision, plan_digest));"},
  }};
  return tables;
}

const TableDefinition &fileOperationLogV4Table() {
  static const TableDefinition table{
      "file_operation_log",
      "CREATE TABLE IF NOT EXISTS file_operation_log (operation_id TEXT PRIMARY KEY, target_path TEXT NOT NULL, "
      "temporary_path TEXT NOT NULL UNIQUE, expected_digest TEXT NOT NULL CHECK(length(expected_digest) = 64), "
      "replacement_digest TEXT NOT NULL CHECK(length(replacement_digest) = 64), state TEXT NOT NULL CHECK(state IN "
      "('prepared', 'temporary_written', 'completed', 'manual_intervention_required')), failure_code TEXT, "
      "CHECK((state = 'completed' AND (failure_code IS NULL OR failure_code IN ('baseline_conflict', 'write_failed'))) "
      "OR (state = 'manual_intervention_required' AND "
      "failure_code = 'manual_intervention_required') OR (state IN ('prepared', 'temporary_written') AND "
      "failure_code IS NULL)));"};
  return table;
}

const TableDefinition &fileOperationLogTable() {
  static const TableDefinition table{
      "file_operation_log",
      "CREATE TABLE IF NOT EXISTS file_operation_log (operation_id TEXT PRIMARY KEY CHECK(length(operation_id) > 0), "
      "root_id TEXT NOT NULL CHECK(length(root_id) > 0), relative_path TEXT NOT NULL CHECK(length(relative_path) > 0), "
      "expected_digest TEXT NOT NULL CHECK(length(expected_digest) = 64), replacement_digest TEXT NOT NULL "
      "CHECK(length(replacement_digest) = 64), state TEXT NOT NULL CHECK(state IN "
      "('prepared', 'temporary_written', 'completed', 'manual_intervention_required')), failure_code TEXT, "
      "CHECK((state = 'completed' AND (failure_code IS NULL OR failure_code IN ('baseline_conflict', 'write_failed'))) "
      "OR (state = 'manual_intervention_required' AND failure_code = 'manual_intervention_required') OR "
      "(state IN ('prepared', 'temporary_written') AND failure_code IS NULL)));"};
  return table;
}

const TableDefinition &projectProvisioningV5Table() {
  static const TableDefinition table{
      "project_provisioning_operations",
      "CREATE TABLE IF NOT EXISTS project_provisioning_operations (operation_id TEXT PRIMARY KEY, project_id TEXT NOT "
      "NULL UNIQUE REFERENCES projects(id), title TEXT NOT NULL, asset_name TEXT NOT NULL, asset_digest TEXT CHECK("
      "asset_digest IS NULL OR length(asset_digest) = 64), state TEXT NOT NULL CHECK(state IN ('provisioning', "
      "'ready', "
      "'failed')), failure_code TEXT, CHECK((state IN ('provisioning', 'ready') AND failure_code IS NULL) OR (state = "
      "'failed' AND failure_code IN ('asset_collision', 'manual_intervention_required', 'safe_abandoned'))));"};
  return table;
}

const TableDefinition &projectProvisioningTable() {
  static const TableDefinition table{
      "project_provisioning_operations",
      "CREATE TABLE IF NOT EXISTS project_provisioning_operations (operation_id TEXT PRIMARY KEY CHECK(length("
      "operation_id) > 0), project_id TEXT NOT NULL UNIQUE CHECK(length(project_id) > 0), title TEXT NOT NULL, root_id "
      "TEXT NOT NULL CHECK(length(root_id) > 0), authorization_revision INTEGER NOT NULL CHECK(authorization_revision "
      ">= 1), root_device INTEGER NOT NULL CHECK(root_device >= 0), root_inode INTEGER NOT NULL CHECK(root_inode >= "
      "0), "
      "relative_path TEXT NOT NULL CHECK(length(relative_path) > 0), asset_device INTEGER, asset_inode INTEGER, "
      "asset_digest TEXT CHECK(asset_digest IS NULL OR length(asset_digest) = 64), state TEXT NOT NULL CHECK(state IN "
      "('provisioning', 'ready', 'failed')), failure_code TEXT, CHECK((asset_device IS NULL AND asset_inode IS NULL "
      "AND "
      "asset_digest IS NULL) OR (asset_device >= 0 AND asset_inode >= 0 AND length(asset_digest) = 64)), "
      "CHECK((state IN ('provisioning', 'ready') AND failure_code IS NULL) OR (state = 'failed' AND failure_code IN "
      "('asset_collision', 'manual_intervention_required', 'safe_abandoned'))));"};
  return table;
}

const std::array<TableDefinition, 4> &reconcileTables() {
  static const std::array<TableDefinition, 4> tables{{
      {"document_registry",
       "CREATE TABLE IF NOT EXISTS document_registry (document_id TEXT PRIMARY KEY, root_id TEXT NOT NULL, "
       "relative_path TEXT NOT NULL, device INTEGER NOT NULL CHECK(device >= 0), inode INTEGER NOT NULL CHECK(inode >= "
       "0), "
       "content_digest TEXT NOT NULL CHECK(length(content_digest) = 64), content_revision INTEGER NOT NULL "
       "CHECK(content_revision >= 1), state TEXT NOT NULL CHECK(state IN ('active', 'tombstoned', 'conflict')), "
       "UNIQUE(root_id, relative_path), UNIQUE(root_id, device, inode));"},
      {"watcher_event_queue",
       "CREATE TABLE IF NOT EXISTS watcher_event_queue (event_id TEXT PRIMARY KEY, root_id TEXT NOT NULL, "
       "relative_path TEXT NOT NULL, state TEXT NOT NULL CHECK(state IN ('queued', 'reconciled')));"},
      {"reconcile_operations",
       "CREATE TABLE IF NOT EXISTS reconcile_operations (operation_id TEXT PRIMARY KEY, root_id TEXT NOT NULL, "
       "result_code TEXT NOT NULL CHECK(result_code IN ('none', 'resource_unavailable', 'identity_conflict')), "
       "health TEXT NOT NULL CHECK(health IN ('ready', 'unavailable', 'conflict')), resource_rejection TEXT NOT NULL, "
       "updated_count INTEGER NOT NULL CHECK(updated_count >= 0), tombstoned_count INTEGER NOT NULL "
       "CHECK(tombstoned_count >= 0), conflict_count INTEGER NOT NULL CHECK(conflict_count >= 0));"},
      {"reconcile_health",
       "CREATE TABLE IF NOT EXISTS reconcile_health (root_id TEXT PRIMARY KEY, health TEXT NOT NULL "
       "CHECK(health IN ('ready', 'stale', 'unavailable', 'conflict')), resource_rejection TEXT NOT NULL);"},
  }};
  return tables;
}

std::optional<std::vector<std::string>> queryRows(sqlite3 *database, const std::string &sql, int columnCount) {
  sqlite3_stmt *rawStatement = nullptr;
  if (sqlite3_prepare_v2(database, sql.c_str(), -1, &rawStatement, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  std::vector<std::string> rows;
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    std::string row;
    for (int column = 0; column < columnCount; ++column) {
      if (column > 0)
        row += '|';
      if (sqlite3_column_type(statement.get(), column) == SQLITE_NULL) {
        row += "<null>";
      } else {
        const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), column));
        if (value == nullptr)
          return std::nullopt;
        row += value;
      }
    }
    rows.push_back(std::move(row));
  }
  if (result != SQLITE_DONE)
    return std::nullopt;
  return rows;
}

std::optional<std::vector<std::string>> uniqueIndexColumns(sqlite3 *database, const char *tableName) {
  const std::string listSql = "PRAGMA index_list(" + std::string(tableName) + ");";
  sqlite3_stmt *rawList = nullptr;
  if (sqlite3_prepare_v2(database, listSql.c_str(), -1, &rawList, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> list(rawList, sqlite3_finalize);
  std::vector<std::string> indexes;
  int result = SQLITE_OK;
  while ((result = sqlite3_step(list.get())) == SQLITE_ROW) {
    if (sqlite3_column_int(list.get(), 2) == 0)
      continue;
    const auto *indexName = reinterpret_cast<const char *>(sqlite3_column_text(list.get(), 1));
    if (indexName == nullptr)
      return std::nullopt;
    const std::string infoSql = "PRAGMA index_info('" + std::string(indexName) + "');";
    const auto columns = queryRows(database, infoSql, 3);
    if (!columns)
      return std::nullopt;
    std::string signature;
    for (const std::string &column : *columns) {
      const std::size_t separator = column.rfind('|');
      if (separator == std::string::npos)
        return std::nullopt;
      if (!signature.empty())
        signature += ',';
      signature += column.substr(separator + 1);
    }
    indexes.push_back(std::move(signature));
  }
  if (result != SQLITE_DONE)
    return std::nullopt;
  std::ranges::sort(indexes);
  return indexes;
}

std::string normalizedSql(const std::string &sql) {
  std::string result;
  result.reserve(sql.size());
  char quote = '\0';
  for (std::size_t index = 0; index < sql.size(); ++index) {
    const unsigned char character = static_cast<unsigned char>(sql[index]);
    if (quote != '\0') {
      result.push_back(static_cast<char>(character));
      if (character == static_cast<unsigned char>(quote)) {
        if (index + 1 < sql.size() && sql[index + 1] == quote) {
          result.push_back(sql[++index]);
        } else {
          quote = '\0';
        }
      }
      continue;
    }
    if (character == '\'' || character == '"') {
      quote = static_cast<char>(character);
      result.push_back(static_cast<char>(character));
    } else if (!std::isspace(character)) {
      result.push_back(character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                            : static_cast<char>(character));
    }
  }
  const std::string optionalClause = "ifnotexists";
  if (const std::size_t position = result.find(optionalClause); position != std::string::npos)
    result.erase(position, optionalClause.size());
  return result;
}

bool tableMatchesDefinition(sqlite3 *database, const TableDefinition &table, QString *errorMessage) {
  sqlite3 *expectedRaw = nullptr;
  if (sqlite3_open(":memory:", &expectedRaw) != SQLITE_OK) {
    if (expectedRaw != nullptr)
      sqlite3_close(expectedRaw);
    if (errorMessage != nullptr)
      *errorMessage = "无法校验 schema 表结构";
    return false;
  }
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> expected(expectedRaw, sqlite3_close);
  if (sqlite3_exec(expected.get(), table.createSql, nullptr, nullptr, nullptr) != SQLITE_OK) {
    if (errorMessage != nullptr)
      *errorMessage = "无法校验 schema 表结构";
    return false;
  }

  const std::string tableInfo = "PRAGMA table_info(" + std::string(table.name) + ");";
  const std::string foreignKeys = "PRAGMA foreign_key_list(" + std::string(table.name) + ");";
  const std::string masterSql =
      "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = '" + std::string(table.name) + "';";
  const auto actualColumns = queryRows(database, tableInfo, 6);
  const auto expectedColumns = queryRows(expected.get(), tableInfo, 6);
  const auto actualForeignKeys = queryRows(database, foreignKeys, 8);
  const auto expectedForeignKeys = queryRows(expected.get(), foreignKeys, 8);
  const auto actualIndexes = uniqueIndexColumns(database, table.name);
  const auto expectedIndexes = uniqueIndexColumns(expected.get(), table.name);
  const auto actualSql = queryRows(database, masterSql, 1);
  const auto expectedSql = queryRows(expected.get(), masterSql, 1);
  const bool valid = actualColumns && expectedColumns && *actualColumns == *expectedColumns && actualForeignKeys &&
                     expectedForeignKeys && *actualForeignKeys == *expectedForeignKeys && actualIndexes &&
                     expectedIndexes && *actualIndexes == *expectedIndexes && actualSql && expectedSql &&
                     actualSql->size() == 1 && expectedSql->size() == 1 &&
                     normalizedSql(actualSql->front()) == normalizedSql(expectedSql->front());
  if (!valid && errorMessage != nullptr)
    *errorMessage = "schema 表结构或约束损坏";
  return valid;
}

bool ensureS1Schema(sqlite3 *database, QString *errorMessage) {
  for (const TableDefinition &table : s1Tables()) {
    if (!execute(database, table.createSql, errorMessage) || !tableMatchesDefinition(database, table, errorMessage))
      return false;
  }
  const auto allocatorBefore =
      queryRows(database, "SELECT delivery_partition, next_position FROM delivery_sequence;", 2);
  const auto eventRange = queryRows(database,
                                    "SELECT COUNT(*), COALESCE(MIN(delivery_position), 0), "
                                    "COALESCE(MAX(delivery_position), 0) FROM domain_events "
                                    "WHERE delivery_partition = 'global';",
                                    3);
  if (!allocatorBefore || !eventRange || eventRange->size() != 1) {
    if (errorMessage != nullptr)
      *errorMessage = "delivery allocator 状态损坏";
    return false;
  }
  if (allocatorBefore->empty() && eventRange->front() != "0|0|0") {
    if (errorMessage != nullptr)
      *errorMessage = "delivery allocator 缺失且已有事件";
    return false;
  }
  if (!execute(database,
               "INSERT OR IGNORE INTO delivery_sequence (delivery_partition, next_position) VALUES ('global', 1);",
               errorMessage))
    return false;
  const auto allocator = queryRows(
      database,
      "SELECT CASE WHEN "
      "(SELECT COUNT(*) FROM delivery_sequence) = 1 AND "
      "EXISTS (SELECT 1 FROM delivery_sequence WHERE delivery_partition = 'global' AND "
      "typeof(next_position) = 'integer' AND (("
      "(SELECT COUNT(*) FROM domain_events WHERE delivery_partition = 'global') = 0 AND "
      "COALESCE((SELECT MIN(delivery_position) FROM domain_events WHERE delivery_partition = 'global'), 0) = 0 AND "
      "COALESCE((SELECT MAX(delivery_position) FROM domain_events WHERE delivery_partition = 'global'), 0) = 0 AND "
      "next_position = 1) OR ("
      "(SELECT COUNT(*) FROM domain_events WHERE delivery_partition = 'global') > 0 AND "
      "(SELECT MIN(delivery_position) FROM domain_events WHERE delivery_partition = 'global') = 1 AND "
      "(SELECT COUNT(*) FROM domain_events WHERE delivery_partition = 'global') = "
      "(SELECT MAX(delivery_position) FROM domain_events WHERE delivery_partition = 'global') AND next_position = "
      "(SELECT MAX(delivery_position) + 1 FROM domain_events WHERE delivery_partition = 'global')))) AND "
      "NOT EXISTS (SELECT 1 FROM domain_events WHERE delivery_partition <> 'global') AND "
      "NOT EXISTS (SELECT 1 FROM outbox_records WHERE delivery_partition <> 'global') AND "
      "NOT EXISTS (SELECT 1 FROM domain_events e LEFT JOIN outbox_records o ON o.event_id = e.event_id AND "
      "o.delivery_partition = e.delivery_partition AND o.delivery_position = e.delivery_position "
      "WHERE o.event_id IS NULL) AND "
      "NOT EXISTS (SELECT 1 FROM outbox_records o LEFT JOIN domain_events e ON e.event_id = o.event_id AND "
      "e.delivery_partition = o.delivery_partition AND e.delivery_position = o.delivery_position "
      "WHERE e.event_id IS NULL) THEN 1 ELSE 0 END;",
      1);
  if (!allocator || *allocator != std::vector<std::string>{"1"}) {
    if (errorMessage != nullptr)
      *errorMessage = "delivery allocator 状态损坏";
    return false;
  }
  return true;
}

bool ensureS2Schema(sqlite3 *database, QString *errorMessage) {
  return ensureS1Schema(database, errorMessage) &&
         execute(database, fileOperationLogV4Table().createSql, errorMessage) &&
         tableMatchesDefinition(database, fileOperationLogV4Table(), errorMessage);
}

bool ensureS3Schema(sqlite3 *database, QString *errorMessage) {
  if (!ensureS2Schema(database, errorMessage) ||
      !execute(database, projectProvisioningV5Table().createSql, errorMessage) ||
      !tableMatchesDefinition(database, projectProvisioningV5Table(), errorMessage)) {
    return false;
  }
  for (const TableDefinition &table : reconcileTables()) {
    if (!execute(database, table.createSql, errorMessage) || !tableMatchesDefinition(database, table, errorMessage))
      return false;
  }
  return true;
}

bool ensureCurrentSchema(sqlite3 *database, QString *errorMessage) {
  if (!ensureS1Schema(database, errorMessage) || !execute(database, fileOperationLogTable().createSql, errorMessage) ||
      !tableMatchesDefinition(database, fileOperationLogTable(), errorMessage) ||
      !execute(database, projectProvisioningTable().createSql, errorMessage) ||
      !tableMatchesDefinition(database, projectProvisioningTable(), errorMessage)) {
    return false;
  }
  for (const TableDefinition &table : reconcileTables()) {
    if (!execute(database, table.createSql, errorMessage) || !tableMatchesDefinition(database, table, errorMessage))
      return false;
  }
  return true;
}

bool migrateV1ToV2(sqlite3 *database, QString *errorMessage) {
  return ensureS1Schema(database, errorMessage) &&
         execute(database, "UPDATE schema_metadata SET schema_version = 2 WHERE schema_version = 1;", errorMessage) &&
         sqlite3_changes(database) == 1;
}

bool migrateV2ToV3(sqlite3 *database, QString *errorMessage) {
  return ensureS2Schema(database, errorMessage) &&
         execute(database, "UPDATE schema_metadata SET schema_version = 3 WHERE schema_version = 2;", errorMessage) &&
         sqlite3_changes(database) == 1;
}

bool migrateV3ToV4(sqlite3 *database, QString *errorMessage) {
  return ensureS3Schema(database, errorMessage) &&
         execute(database, "UPDATE schema_metadata SET schema_version = 4 WHERE schema_version = 3;", errorMessage) &&
         sqlite3_changes(database) == 1;
}

bool migrateV4ToV5(sqlite3 *database, QString *errorMessage) {
  return ensureS3Schema(database, errorMessage) &&
         execute(database, "ALTER TABLE file_operation_log RENAME TO legacy_file_operation_log_v4;", errorMessage) &&
         execute(database, fileOperationLogTable().createSql, errorMessage) &&
         tableMatchesDefinition(database, fileOperationLogTable(), errorMessage) &&
         execute(database, "UPDATE schema_metadata SET schema_version = 5 WHERE schema_version = 4;", errorMessage) &&
         sqlite3_changes(database) == 1;
}

/**
 * 将 v5 的无根绑定 saga 表升级为 v6。
 *
 * v5 记录没有可验证的 root/authorization/identity 证据，含记录时拒绝提升版本而非猜测目录或删除项目；空表可原子重建。
 */
bool migrateV5ToV6(sqlite3 *database, QString *errorMessage) {
  const auto count = queryRows(database, "SELECT COUNT(*) FROM project_provisioning_operations;", 1);
  if (!count || *count != std::vector<std::string>{"0"}) {
    if (errorMessage != nullptr)
      *errorMessage = "旧版项目创建记录缺少资源证明，需要人工迁移";
    return false;
  }
  if (!ensureS1Schema(database, errorMessage) || !execute(database, fileOperationLogTable().createSql, errorMessage) ||
      !tableMatchesDefinition(database, fileOperationLogTable(), errorMessage) ||
      !execute(database, projectProvisioningV5Table().createSql, errorMessage) ||
      !tableMatchesDefinition(database, projectProvisioningV5Table(), errorMessage))
    return false;
  for (const TableDefinition &table : reconcileTables()) {
    if (!execute(database, table.createSql, errorMessage) || !tableMatchesDefinition(database, table, errorMessage))
      return false;
  }
  return execute(database, "DROP TABLE project_provisioning_operations;", errorMessage) &&
         execute(database, projectProvisioningTable().createSql, errorMessage) &&
         tableMatchesDefinition(database, projectProvisioningTable(), errorMessage) &&
         execute(database, "UPDATE schema_metadata SET schema_version = 6 WHERE schema_version = 5;", errorMessage) &&
         sqlite3_changes(database) == 1;
}

void closeDatabase(sqlite3 *database) {
  if (database != nullptr) {
    sqlite3_close(database);
  }
}

std::optional<int> storedSchemaVersion(sqlite3 *database, QString *errorMessage, bool *queryValid) {
  *queryValid = false;
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, "SELECT schema_version FROM schema_metadata;", -1, &statement, nullptr) !=
      SQLITE_OK) {
    if (errorMessage != nullptr)
      *errorMessage = "无法读取 schema 元数据";
    return std::nullopt;
  }

  int rowCount = 0;
  int version = -1;
  int stepResult = SQLITE_OK;
  while ((stepResult = sqlite3_step(statement)) == SQLITE_ROW) {
    ++rowCount;
    version = sqlite3_column_int(statement, 0);
  }
  sqlite3_finalize(statement);
  if (stepResult != SQLITE_DONE || rowCount > 1) {
    if (errorMessage != nullptr)
      *errorMessage = "schema 元数据损坏或不唯一";
    return std::nullopt;
  }
  *queryValid = true;
  return rowCount == 0 ? std::optional<int>{} : std::optional<int>{version};
}

} // namespace

bool SchemaMigrator::migrate(const QString &databasePath, QString *errorMessage) const {
  sqlite3 *database = nullptr;
  const QByteArray encodedPath = databasePath.toUtf8();
  if (sqlite3_open_v2(encodedPath.constData(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
      SQLITE_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = "无法打开本地数据库";
    }
    closeDatabase(database);
    return false;
  }

  bool migrated =
      execute(database, "BEGIN IMMEDIATE;", errorMessage) &&
      execute(database, "CREATE TABLE IF NOT EXISTS schema_metadata (schema_version INTEGER NOT NULL);", errorMessage);
  if (migrated) {
    bool metadataValid = false;
    const std::optional<int> storedVersion = storedSchemaVersion(database, errorMessage, &metadataValid);
    if (!metadataValid) {
      migrated = false;
    } else if (!storedVersion.has_value()) {
      migrated = execute(database, "INSERT INTO schema_metadata (schema_version) VALUES (1);", errorMessage);
    } else if (*storedVersion > domain::kCurrentSchemaVersion || *storedVersion < 1) {
      if (errorMessage != nullptr)
        *errorMessage = "不支持的 schema 版本";
      migrated = false;
    }
    while (migrated) {
      bool versionValid = false;
      const std::optional<int> version = storedSchemaVersion(database, errorMessage, &versionValid);
      if (!versionValid || !version.has_value()) {
        migrated = false;
      } else if (*version == domain::kCurrentSchemaVersion) {
        migrated = ensureCurrentSchema(database, errorMessage);
        break;
      } else if (*version == 1) {
        migrated = migrateV1ToV2(database, errorMessage);
      } else if (*version == 2) {
        migrated = migrateV2ToV3(database, errorMessage);
      } else if (*version == 3) {
        migrated = migrateV3ToV4(database, errorMessage);
      } else if (*version == 4) {
        migrated = migrateV4ToV5(database, errorMessage);
      } else if (*version == 5) {
        migrated = migrateV5ToV6(database, errorMessage);
      } else {
        if (errorMessage != nullptr)
          *errorMessage = "不支持的 schema 版本";
        migrated = false;
      }
    }
  }
  if (migrated)
    migrated = execute(database, "COMMIT;", errorMessage);

  if (!migrated) {
    execute(database, "ROLLBACK;", nullptr);
  }
  closeDatabase(database);
  return migrated;
}

int SchemaMigrator::schemaVersion(const QString &databasePath, QString *errorMessage) const {
  sqlite3 *database = nullptr;
  const QByteArray encodedPath = databasePath.toUtf8();
  if (sqlite3_open_v2(encodedPath.constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = "无法读取本地数据库";
    }
    closeDatabase(database);
    return -1;
  }

  bool metadataValid = false;
  const std::optional<int> storedVersion = storedSchemaVersion(database, errorMessage, &metadataValid);
  if (!metadataValid || !storedVersion.has_value()) {
    if (metadataValid && errorMessage != nullptr) {
      *errorMessage = "schema 元数据不存在";
    }
    closeDatabase(database);
    return -1;
  }

  closeDatabase(database);
  return *storedVersion;
}

} // namespace pros::infrastructure
