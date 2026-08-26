#include "pros/infrastructure/file_operation_log.h"

#include <QCryptographicHash>

#include <sqlite3.h>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace pros::infrastructure {
namespace {

struct DatabaseCloser final {
  void operator()(sqlite3 *database) const { sqlite3_close(database); }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

struct PendingOperation final {
  QString operationId;
  QString rootId;
  QString relativePath;
  QByteArray expectedDigest;
  QByteArray replacementDigest;
  QString state;
  QString failureCode;
};

bool isDigest(const QByteArray &value) {
  if (value.size() != 64)
    return false;
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
      return false;
  }
  return true;
}

bool isValidText(const QString &value) { return !value.isEmpty() && !value.contains(QChar::Null); }

std::optional<Database> openDatabase(const QString &databasePath) {
  sqlite3 *rawDatabase = nullptr;
  const QByteArray encoded = databasePath.toUtf8();
  if (sqlite3_open_v2(encoded.constData(), &rawDatabase, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    if (rawDatabase != nullptr)
      sqlite3_close(rawDatabase);
    return std::nullopt;
  }
  Database database(rawDatabase);
  if (sqlite3_busy_timeout(database.get(), 5000) != SQLITE_OK)
    return std::nullopt;
  return database;
}

bool bindText(sqlite3_stmt *statement, int index, const QString &value) {
  const QByteArray encoded = value.toUtf8();
  return sqlite3_bind_text64(statement, index, encoded.constData(), static_cast<sqlite3_uint64>(encoded.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}

bool bindDigest(sqlite3_stmt *statement, int index, const QByteArray &value) {
  return sqlite3_bind_text64(statement, index, value.constData(), static_cast<sqlite3_uint64>(value.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}

std::optional<QString> textColumn(sqlite3_stmt *statement, int column, bool allowEmpty = false) {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
    return std::nullopt;
  const auto *data = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
  const int size = sqlite3_column_bytes(statement, column);
  if (data == nullptr || size < 0 || (!allowEmpty && size == 0))
    return std::nullopt;
  const QByteArray encoded(data, size);
  QString decoded = QString::fromUtf8(encoded);
  if (decoded.isNull() || decoded.contains(QChar::ReplacementCharacter) || decoded.toUtf8() != encoded ||
      (!allowEmpty && !isValidText(decoded))) {
    return std::nullopt;
  }
  return decoded;
}

std::optional<QByteArray> digestColumn(sqlite3_stmt *statement, int column) {
  const auto text = textColumn(statement, column);
  if (!text)
    return std::nullopt;
  const QByteArray value = text->toLatin1();
  return isDigest(value) ? std::optional<QByteArray>(value) : std::nullopt;
}

bool insertPrepared(sqlite3 *database, const PendingOperation &operation) {
  sqlite3_stmt *rawStatement = nullptr;
  constexpr const char *sql = "INSERT INTO file_operation_log "
                              "(operation_id, root_id, relative_path, expected_digest, replacement_digest, state) "
                              "VALUES (?, ?, ?, ?, ?, 'prepared');";
  if (sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  return bindText(statement.get(), 1, operation.operationId) && bindText(statement.get(), 2, operation.rootId) &&
         bindText(statement.get(), 3, operation.relativePath) &&
         bindDigest(statement.get(), 4, operation.expectedDigest) &&
         bindDigest(statement.get(), 5, operation.replacementDigest) && sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool setState(sqlite3 *database, const QString &operationId, const char *state, const char *failureCode = nullptr) {
  sqlite3_stmt *rawStatement = nullptr;
  constexpr const char *sql = "UPDATE file_operation_log SET state = ?, failure_code = ? "
                              "WHERE operation_id = ? AND state <> 'completed';";
  if (sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  const bool failureBound = failureCode == nullptr ? sqlite3_bind_null(statement.get(), 2) == SQLITE_OK
                                                   : bindText(statement.get(), 2, QString::fromLatin1(failureCode));
  return bindText(statement.get(), 1, QString::fromLatin1(state)) && failureBound &&
         bindText(statement.get(), 3, operationId) && sqlite3_step(statement.get()) == SQLITE_DONE &&
         sqlite3_changes(database) == 1;
}

bool markManual(sqlite3 *database, const QString &operationId) {
  return setState(database, operationId, "manual_intervention_required", "manual_intervention_required");
}

bool markTemporaryWritten(sqlite3 *database, const QString &operationId) {
  return setState(database, operationId, "temporary_written");
}

bool markCompleted(sqlite3 *database, const QString &operationId, const char *failureCode = nullptr) {
  return setState(database, operationId, "completed", failureCode);
}

std::optional<std::optional<PendingOperation>> readOperation(sqlite3 *database, const QString &operationId) {
  sqlite3_stmt *rawStatement = nullptr;
  constexpr const char *sql =
      "SELECT operation_id, root_id, relative_path, expected_digest, replacement_digest, state, "
      "COALESCE(failure_code, '') FROM file_operation_log WHERE operation_id = ?;";
  if (sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  if (!bindText(statement.get(), 1, operationId))
    return std::nullopt;
  const int result = sqlite3_step(statement.get());
  if (result == SQLITE_DONE)
    return std::optional<PendingOperation>{};
  if (result != SQLITE_ROW)
    return std::nullopt;
  const auto id = textColumn(statement.get(), 0);
  const auto rootId = textColumn(statement.get(), 1);
  const auto relativePath = textColumn(statement.get(), 2);
  const auto expectedDigest = digestColumn(statement.get(), 3);
  const auto replacementDigest = digestColumn(statement.get(), 4);
  const auto state = textColumn(statement.get(), 5);
  const auto failureCode = textColumn(statement.get(), 6, true);
  if (!id || !rootId || !relativePath || !expectedDigest || !replacementDigest || !state || !failureCode ||
      sqlite3_step(statement.get()) != SQLITE_DONE) {
    return std::nullopt;
  }
  return PendingOperation{*id, *rootId, *relativePath, *expectedDigest, *replacementDigest, *state, *failureCode};
}

std::optional<std::vector<PendingOperation>> readPending(sqlite3 *database) {
  sqlite3_stmt *rawStatement = nullptr;
  constexpr const char *sql =
      "SELECT operation_id, root_id, relative_path, expected_digest, replacement_digest, state, "
      "COALESCE(failure_code, '') FROM file_operation_log WHERE state <> 'completed' "
      "ORDER BY operation_id;";
  if (sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  std::vector<PendingOperation> operations;
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const auto id = textColumn(statement.get(), 0);
    const auto rootId = textColumn(statement.get(), 1);
    const auto relativePath = textColumn(statement.get(), 2);
    const auto expectedDigest = digestColumn(statement.get(), 3);
    const auto replacementDigest = digestColumn(statement.get(), 4);
    const auto state = textColumn(statement.get(), 5);
    const auto failureCode = textColumn(statement.get(), 6, true);
    if (!id || !rootId || !relativePath || !expectedDigest || !replacementDigest || !state || !failureCode)
      return std::nullopt;
    operations.push_back({*id, *rootId, *relativePath, *expectedDigest, *replacementDigest, *state, *failureCode});
  }
  return result == SQLITE_DONE ? std::optional<std::vector<PendingOperation>>(std::move(operations)) : std::nullopt;
}

QByteArray digest(const QByteArray &contents) {
  return QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex();
}

bool sameIntent(const PendingOperation &operation, const QString &rootId, const QString &relativePath,
                const QByteArray &expectedDigest, const QByteArray &replacementDigest) {
  return operation.rootId == rootId && operation.relativePath == relativePath &&
         operation.expectedDigest == expectedDigest && operation.replacementDigest == replacementDigest;
}

FileOperationResult persistedResult(const PendingOperation &operation) {
  if (operation.state == "completed") {
    if (operation.failureCode == "baseline_conflict")
      return {FileOperationCode::baseline_conflict, operation.operationId};
    if (operation.failureCode == "write_failed")
      return {FileOperationCode::write_failed, operation.operationId};
    return {FileOperationCode::none, operation.operationId};
  }
  if (operation.state == "temporary_written")
    return {FileOperationCode::recovery_required, operation.operationId};
  if (operation.state == "manual_intervention_required")
    return {FileOperationCode::manual_intervention_required, operation.operationId};
  return {FileOperationCode::storage_unavailable, operation.operationId};
}

FileOperationResult executePrepared(sqlite3 *database, const ResourceResolver &resolver,
                                    const PendingOperation &operation, const QByteArray &replacementContents,
                                    FileOperationFault fault) {
  const auto replacement =
      resolver.openForAtomicReplacement(operation.rootId, operation.relativePath, operation.operationId);
  if (!replacement.isAccepted()) {
    markManual(database, operation.operationId);
    return {FileOperationCode::resource_rejected, operation.operationId, replacement.rejection};
  }
  QByteArray currentContents;
  ResourceRejectCode rejection = ResourceRejectCode::none;
  if (!replacement.handle->readTargetAll(&currentContents, &rejection)) {
    markManual(database, operation.operationId);
    return {FileOperationCode::resource_rejected, operation.operationId, rejection};
  }
  if (digest(currentContents) != operation.expectedDigest) {
    return {markCompleted(database, operation.operationId, "baseline_conflict")
                ? FileOperationCode::baseline_conflict
                : FileOperationCode::manual_intervention_required,
            operation.operationId};
  }
  if (!replacement.handle->writeTemporaryAndSync(replacementContents, &rejection)) {
    QByteArray temporaryContents;
    if (!replacement.handle->readTemporaryAll(&temporaryContents, &rejection) ||
        digest(temporaryContents) != operation.replacementDigest) {
      return {markCompleted(database, operation.operationId, "write_failed")
                  ? FileOperationCode::write_failed
                  : FileOperationCode::manual_intervention_required,
              operation.operationId};
    }
  }
  if (!markTemporaryWritten(database, operation.operationId))
    return {FileOperationCode::manual_intervention_required, operation.operationId};
  if (fault == FileOperationFault::after_temporary_written)
    return {FileOperationCode::recovery_required, operation.operationId};
  if (!replacement.handle->readTargetAll(&currentContents, &rejection) ||
      digest(currentContents) != operation.expectedDigest || !replacement.handle->replaceTemporaryAndSync(&rejection) ||
      !markCompleted(database, operation.operationId)) {
    return {FileOperationCode::manual_intervention_required, operation.operationId, rejection};
  }
  return {FileOperationCode::none, operation.operationId};
}

} // namespace

const char *fileOperationCodeName(FileOperationCode code) {
  switch (code) {
  case FileOperationCode::none:
    return "none";
  case FileOperationCode::invalid_argument:
    return "invalid_argument";
  case FileOperationCode::storage_unavailable:
    return "storage_unavailable";
  case FileOperationCode::baseline_conflict:
    return "baseline_conflict";
  case FileOperationCode::write_failed:
    return "write_failed";
  case FileOperationCode::recovery_required:
    return "recovery_required";
  case FileOperationCode::manual_intervention_required:
    return "manual_intervention_required";
  case FileOperationCode::operation_id_conflict:
    return "operation_id_conflict";
  case FileOperationCode::resource_rejected:
    return "resource_rejected";
  }
  return "storage_unavailable";
}

bool FileOperationResult::isSucceeded() const { return code == FileOperationCode::none; }

bool FileRecoveryReport::isSucceeded() const { return code == FileOperationCode::none; }

FileOperationLog::FileOperationLog(QString databasePath, FileOperationFault fault)
    : databasePath_(std::move(databasePath)), fault_(fault) {}

FileOperationResult FileOperationLog::replaceIfUnchanged(const ResourceResolver &resolver, const QString &rootId,
                                                         const QString &relativePath, const QString &operationId,
                                                         const QByteArray &expectedBaselineSha256,
                                                         const QByteArray &replacementContents) const {
  if (!isValidText(rootId) || !isValidText(relativePath) || !isValidText(operationId) ||
      !isDigest(expectedBaselineSha256)) {
    return {FileOperationCode::invalid_argument, operationId};
  }
  const QByteArray replacementDigest = digest(replacementContents);
  const auto database = openDatabase(databasePath_);
  if (!database)
    return {FileOperationCode::storage_unavailable, operationId};
  if (sqlite3_exec(database->get(), "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK)
    return {FileOperationCode::storage_unavailable, operationId};
  const auto finish = [&database, &operationId](FileOperationResult result) {
    if (sqlite3_exec(database->get(), "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK)
      return result;
    sqlite3_exec(database->get(), "ROLLBACK;", nullptr, nullptr, nullptr);
    return FileOperationResult{FileOperationCode::storage_unavailable, operationId};
  };
  auto stored = readOperation(database->get(), operationId);
  if (!stored)
    return finish({FileOperationCode::storage_unavailable, operationId});
  if (!stored->has_value()) {
    const PendingOperation prepared{operationId, rootId, relativePath, expectedBaselineSha256, replacementDigest,
                                    "prepared",  {}};
    if (!insertPrepared(database->get(), prepared)) {
      stored = readOperation(database->get(), operationId);
      if (!stored || !stored->has_value())
        return finish({FileOperationCode::storage_unavailable, operationId});
    } else {
      stored = prepared;
    }
  }
  if (!stored || !stored->has_value())
    return finish({FileOperationCode::storage_unavailable, operationId});
  const PendingOperation &operation = stored->value();
  if (!sameIntent(operation, rootId, relativePath, expectedBaselineSha256, replacementDigest))
    return finish({FileOperationCode::operation_id_conflict, operationId});
  if (operation.state != "prepared")
    return finish(persistedResult(operation));
  return finish(executePrepared(database->get(), resolver, operation, replacementContents, fault_));
}

FileRecoveryReport FileOperationLog::recoverPending(const ResourceResolver &resolver) const {
  const auto database = openDatabase(databasePath_);
  if (!database)
    return {FileOperationCode::storage_unavailable, 0, 0, {}};
  const auto pending = readPending(database->get());
  if (!pending)
    return {FileOperationCode::storage_unavailable, 0, 0, {}};

  FileRecoveryReport report;
  for (const PendingOperation &operation : *pending) {
    bool recovered = false;
    if (operation.state == "temporary_written") {
      const auto replacement =
          resolver.openForAtomicReplacement(operation.rootId, operation.relativePath, operation.operationId);
      QByteArray targetContents;
      QByteArray temporaryContents;
      ResourceRejectCode rejection = ResourceRejectCode::none;
      if (replacement.isAccepted() && replacement.handle->readTargetAll(&targetContents, &rejection) &&
          replacement.handle->readTemporaryAll(&temporaryContents, &rejection) &&
          digest(targetContents) == operation.expectedDigest &&
          digest(temporaryContents) == operation.replacementDigest &&
          replacement.handle->replaceTemporaryAndSync(&rejection) &&
          markCompleted(database->get(), operation.operationId)) {
        recovered = true;
      }
    }
    if (recovered) {
      ++report.recoveredCount;
      continue;
    }
    if (!markManual(database->get(), operation.operationId))
      return {FileOperationCode::storage_unavailable, report.recoveredCount, report.manualInterventionCount,
              report.operationIds};
    ++report.manualInterventionCount;
    report.operationIds.append(operation.operationId);
  }
  if (report.manualInterventionCount > 0)
    report.code = FileOperationCode::manual_intervention_required;
  return report;
}

} // namespace pros::infrastructure
