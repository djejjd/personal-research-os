#include "pros/infrastructure/sqlite_work_query_service.h"

#include <QByteArrayView>
#include <QStringDecoder>

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace pros::infrastructure {
namespace {

using application::WorkQueryResult;

struct DatabaseCloser final {
  void operator()(sqlite3 *database) const { sqlite3_close(database); }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

bool validText(const std::string &value) {
  if (value.empty() || value.find('\0') != std::string::npos)
    return false;
  QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
  const QString decoded = decoder.decode(QByteArrayView(value.data(), static_cast<qsizetype>(value.size())));
  return !decoded.isNull() && !decoder.hasError();
}

bool bindText(sqlite3_stmt *statement, int index, const std::string &value) {
  return sqlite3_bind_text64(statement, index, value.data(), static_cast<sqlite3_uint64>(value.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}

std::optional<std::string> readText(sqlite3_stmt *statement, int column) {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
    return std::nullopt;
  const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
  const int bytes = sqlite3_column_bytes(statement, column);
  if (value == nullptr || bytes <= 0)
    return std::nullopt;
  std::string text(value, static_cast<std::size_t>(bytes));
  return validText(text) ? std::optional<std::string>(std::move(text)) : std::nullopt;
}

void setStorageError(QString *errorMessage) {
  if (errorMessage != nullptr)
    *errorMessage = "本地工作聚合查询失败";
}

template <typename Aggregate, typename Factory>
WorkQueryResult<Aggregate> query(const QString &path, const std::string &id, const char *sql, int fieldCount,
                                 Factory factory, QString *errorMessage) {
  if (!validText(id))
    return WorkQueryResult<Aggregate>::invalidArgument();

  sqlite3 *rawDatabase = nullptr;
  const QByteArray encodedPath = path.toUtf8();
  if (sqlite3_open_v2(encodedPath.constData(), &rawDatabase, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (rawDatabase != nullptr)
      sqlite3_close(rawDatabase);
    setStorageError(errorMessage);
    return WorkQueryResult<Aggregate>::storageUnavailable();
  }
  Database database(rawDatabase);
  sqlite3_stmt *rawStatement = nullptr;
  if (sqlite3_prepare_v2(database.get(), sql, -1, &rawStatement, nullptr) != SQLITE_OK) {
    setStorageError(errorMessage);
    return WorkQueryResult<Aggregate>::storageUnavailable();
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  if (!bindText(statement.get(), 1, id)) {
    setStorageError(errorMessage);
    return WorkQueryResult<Aggregate>::storageUnavailable();
  }

  const int step = sqlite3_step(statement.get());
  if (step == SQLITE_DONE)
    return WorkQueryResult<Aggregate>::notFound();
  if (step != SQLITE_ROW) {
    setStorageError(errorMessage);
    return WorkQueryResult<Aggregate>::storageUnavailable();
  }

  try {
    const auto first = readText(statement.get(), 0);
    const auto second = readText(statement.get(), 1);
    const auto third = fieldCount == 5 ? readText(statement.get(), 2) : std::optional<std::string>{};
    const int statusColumn = fieldCount - 2;
    const int revisionColumn = fieldCount - 1;
    const int status = sqlite3_column_int(statement.get(), statusColumn);
    const sqlite3_int64 revision = sqlite3_column_int64(statement.get(), revisionColumn);
    if (!first || !second || (fieldCount == 5 && !third) ||
        sqlite3_column_type(statement.get(), statusColumn) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), revisionColumn) != SQLITE_INTEGER || status < 0 || status > 1 ||
        revision < 0 || sqlite3_step(statement.get()) != SQLITE_DONE) {
      setStorageError(errorMessage);
      return WorkQueryResult<Aggregate>::storageUnavailable();
    }
    return WorkQueryResult<Aggregate>::found(factory(*first, *second, third, status, revision));
  } catch (const std::exception &) {
    setStorageError(errorMessage);
    return WorkQueryResult<Aggregate>::storageUnavailable();
  }
}

} // namespace

SqliteWorkQueryService::SqliteWorkQueryService(QString databasePath) : databasePath_(std::move(databasePath)) {}

WorkQueryResult<domain::Project> SqliteWorkQueryService::project(const std::string &id, QString *errorMessage) const {
  return query<domain::Project>(
      databasePath_, id, "SELECT id, title, status, revision FROM projects WHERE id = ?;", 4,
      [](const std::string &aggregateId, const std::string &title, const std::optional<std::string> &, int status,
         sqlite3_int64 revision) {
        return domain::Project(aggregateId, title, static_cast<domain::ProjectStatus>(status),
                               domain::Revision(revision));
      },
      errorMessage);
}

WorkQueryResult<domain::Task> SqliteWorkQueryService::task(const std::string &id, QString *errorMessage) const {
  return query<domain::Task>(
      databasePath_, id, "SELECT id, project_id, title, status, revision FROM tasks WHERE id = ?;", 5,
      [](const std::string &aggregateId, const std::string &projectId, const std::optional<std::string> &title,
         int status, sqlite3_int64 revision) {
        return domain::Task(aggregateId, projectId, *title, static_cast<domain::TaskStatus>(status),
                            domain::Revision(revision));
      },
      errorMessage);
}

WorkQueryResult<domain::Milestone> SqliteWorkQueryService::milestone(const std::string &id,
                                                                     QString *errorMessage) const {
  return query<domain::Milestone>(
      databasePath_, id, "SELECT id, project_id, title, status, revision FROM milestones WHERE id = ?;", 5,
      [](const std::string &aggregateId, const std::string &projectId, const std::optional<std::string> &title,
         int status, sqlite3_int64 revision) {
        return domain::Milestone(aggregateId, projectId, *title, static_cast<domain::MilestoneStatus>(status),
                                 domain::Revision(revision));
      },
      errorMessage);
}

WorkQueryResult<domain::Direction> SqliteWorkQueryService::direction(const std::string &id,
                                                                     QString *errorMessage) const {
  return query<domain::Direction>(
      databasePath_, id, "SELECT id, title, status, revision FROM directions WHERE id = ?;", 4,
      [](const std::string &aggregateId, const std::string &title, const std::optional<std::string> &, int status,
         sqlite3_int64 revision) {
        return domain::Direction(aggregateId, title, static_cast<domain::DirectionStatus>(status),
                                 domain::Revision(revision));
      },
      errorMessage);
}

} // namespace pros::infrastructure
