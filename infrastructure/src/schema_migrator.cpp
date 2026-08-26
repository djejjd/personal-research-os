#include "pros/infrastructure/schema_migrator.h"

#include "pros/domain/schema_version.h"

#include <sqlite3.h>

namespace pros::infrastructure {
namespace {

bool execute(sqlite3* database, const char* statement, QString* errorMessage) {
  char* sqliteError = nullptr;
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

void closeDatabase(sqlite3* database) {
  if (database != nullptr) {
    sqlite3_close(database);
  }
}

}  // namespace

bool SchemaMigrator::migrate(const QString& databasePath, QString* errorMessage) const {
  sqlite3* database = nullptr;
  const QByteArray encodedPath = databasePath.toUtf8();
  if (sqlite3_open_v2(encodedPath.constData(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) !=
      SQLITE_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = QString::fromUtf8(database == nullptr ? "无法打开数据库" : sqlite3_errmsg(database));
    }
    closeDatabase(database);
    return false;
  }

  const bool migrated =
      execute(database, "BEGIN IMMEDIATE;", errorMessage) &&
      execute(database, "CREATE TABLE IF NOT EXISTS schema_metadata (schema_version INTEGER NOT NULL);", errorMessage) &&
      execute(database,
              "INSERT INTO schema_metadata (schema_version) SELECT 1 WHERE NOT EXISTS "
              "(SELECT 1 FROM schema_metadata);",
              errorMessage) &&
      execute(database, "COMMIT;", errorMessage);

  if (!migrated) {
    execute(database, "ROLLBACK;", nullptr);
  }
  closeDatabase(database);
  return migrated;
}

int SchemaMigrator::schemaVersion(const QString& databasePath, QString* errorMessage) const {
  sqlite3* database = nullptr;
  const QByteArray encodedPath = databasePath.toUtf8();
  if (sqlite3_open_v2(encodedPath.constData(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = QString::fromUtf8(database == nullptr ? "无法读取数据库" : sqlite3_errmsg(database));
    }
    closeDatabase(database);
    return -1;
  }

  sqlite3_stmt* statement = nullptr;
  const int prepareResult = sqlite3_prepare_v2(database, "SELECT schema_version FROM schema_metadata LIMIT 1;", -1,
                                               &statement, nullptr);
  if (prepareResult != SQLITE_OK || statement == nullptr || sqlite3_step(statement) != SQLITE_ROW) {
    if (errorMessage != nullptr) {
      *errorMessage = QString::fromUtf8(sqlite3_errmsg(database));
    }
    sqlite3_finalize(statement);
    closeDatabase(database);
    return -1;
  }

  const int version = sqlite3_column_int(statement, 0);
  sqlite3_finalize(statement);
  closeDatabase(database);
  return version;
}

}  // namespace pros::infrastructure
