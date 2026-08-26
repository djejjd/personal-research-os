#include "pros/infrastructure/schema_migrator.h"

#include "pros/domain/schema_version.h"

#include <sqlite3.h>

#include <optional>

namespace pros::infrastructure {
namespace {

bool execute(sqlite3 *database, const char *statement, QString *errorMessage) {
  char *sqliteError = nullptr;
  const int result = sqlite3_exec(database, statement, nullptr, nullptr, &sqliteError);
  if (result == SQLITE_OK) {
    return true;
  }

  if (errorMessage != nullptr) {
    *errorMessage = QString::fromUtf8(sqliteError == nullptr ? sqlite3_errmsg(database) : sqliteError);
  }
  sqlite3_free(sqliteError);
  return false;
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
      *errorMessage = QString::fromUtf8(database == nullptr ? "无法打开数据库" : sqlite3_errmsg(database));
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
    } else if (*storedVersion != domain::kCurrentSchemaVersion) {
      if (errorMessage != nullptr)
        *errorMessage = "不支持的 schema 版本";
      migrated = false;
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
      *errorMessage = QString::fromUtf8(database == nullptr ? "无法读取数据库" : sqlite3_errmsg(database));
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
