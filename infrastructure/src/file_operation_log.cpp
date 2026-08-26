#include "pros/infrastructure/file_operation_log.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <sqlite3.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
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

struct FileDescriptor final {
  explicit FileDescriptor(int value = -1) : value(value) {}
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept : value(std::exchange(other.value, -1)) {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      reset();
      value = std::exchange(other.value, -1);
    }
    return *this;
  }
  ~FileDescriptor() { reset(); }

  void reset() {
    if (value >= 0) {
      close(value);
      value = -1;
    }
  }

  int value;
};

struct PendingOperation final {
  QString operationId;
  QString targetPath;
  QString temporaryPath;
  QByteArray expectedDigest;
  QByteArray replacementDigest;
  QString state;
};

struct TargetPaths final {
  QString target;
  QString temporary;
  QString lock;
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

std::optional<TargetPaths> targetPaths(const QString &targetPath, const QString &operationId) {
  if (!isValidText(targetPath) || !QDir::isAbsolutePath(targetPath) || !isValidText(operationId))
    return std::nullopt;
  const QFileInfo targetInformation(targetPath);
  if (!targetInformation.exists() || !targetInformation.isFile() || targetInformation.isSymLink())
    return std::nullopt;
  const QString target = targetInformation.canonicalFilePath();
  const QFileInfo canonicalInformation(target);
  const QString directory = canonicalInformation.dir().canonicalPath();
  if (!isValidText(target) || !isValidText(directory) || canonicalInformation.fileName().isEmpty())
    return std::nullopt;
  const QString temporary =
      QDir(directory).filePath("." + canonicalInformation.fileName() + ".pros-" + operationId + ".tmp");
  return TargetPaths{target, temporary, target + ".pros.lock"};
}

bool readDigest(const QString &path, QByteArray *digest) {
  if (digest == nullptr)
    return false;
  const QByteArray encodedPath = QFile::encodeName(path);
  FileDescriptor descriptor(open(encodedPath.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat status{};
  if (descriptor.value < 0 || fstat(descriptor.value, &status) != 0 || !S_ISREG(status.st_mode))
    return false;

  QCryptographicHash hash(QCryptographicHash::Sha256);
  std::array<char, 4096> buffer{};
  for (;;) {
    const ssize_t count = read(descriptor.value, buffer.data(), buffer.size());
    if (count == 0)
      break;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(count)));
  }
  *digest = hash.result().toHex();
  return true;
}

bool writeTemporaryFile(const QString &path, const QByteArray &contents) {
  const QByteArray encodedPath = QFile::encodeName(path);
  FileDescriptor descriptor(open(encodedPath.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (descriptor.value < 0)
    return false;
  qsizetype offset = 0;
  while (offset < contents.size()) {
    const ssize_t count =
        write(descriptor.value, contents.constData() + offset, static_cast<size_t>(contents.size() - offset));
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (count == 0)
      return false;
    offset += static_cast<qsizetype>(count);
  }
  return fsync(descriptor.value) == 0;
}

bool replaceAtomically(const QString &temporaryPath, const QString &targetPath) {
  const QByteArray temporary = QFile::encodeName(temporaryPath);
  const QByteArray target = QFile::encodeName(targetPath);
  if (rename(temporary.constData(), target.constData()) != 0)
    return false;
  const QByteArray directory = QFile::encodeName(QFileInfo(targetPath).dir().canonicalPath());
  FileDescriptor directoryDescriptor(open(directory.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  return directoryDescriptor.value >= 0 && fsync(directoryDescriptor.value) == 0;
}

std::optional<FileDescriptor> lockTarget(const QString &lockPath) {
  const QByteArray encoded = QFile::encodeName(lockPath);
  FileDescriptor descriptor(open(encoded.constData(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (descriptor.value < 0 || flock(descriptor.value, LOCK_EX) != 0)
    return std::nullopt;
  return descriptor;
}

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

bool insertPrepared(sqlite3 *database, const PendingOperation &operation) {
  sqlite3_stmt *rawStatement = nullptr;
  constexpr const char *sql = "INSERT INTO file_operation_log "
                              "(operation_id, target_path, temporary_path, expected_digest, replacement_digest, state) "
                              "VALUES (?, ?, ?, ?, ?, 'prepared');";
  if (sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  return bindText(statement.get(), 1, operation.operationId) && bindText(statement.get(), 2, operation.targetPath) &&
         bindText(statement.get(), 3, operation.temporaryPath) &&
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
  const QString encodedState = QString::fromLatin1(state);
  const bool failureBound = failureCode == nullptr ? sqlite3_bind_null(statement.get(), 2) == SQLITE_OK
                                                   : bindText(statement.get(), 2, QString::fromLatin1(failureCode));
  return bindText(statement.get(), 1, encodedState) && failureBound && bindText(statement.get(), 3, operationId) &&
         sqlite3_step(statement.get()) == SQLITE_DONE && sqlite3_changes(database) == 1;
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

std::optional<QString> textColumn(sqlite3_stmt *statement, int column) {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
    return std::nullopt;
  const auto *data = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
  const int size = sqlite3_column_bytes(statement, column);
  if (data == nullptr || size <= 0)
    return std::nullopt;
  const QByteArray encoded(data, size);
  QString decoded = QString::fromUtf8(encoded);
  if (decoded.isNull() || decoded.contains(QChar::ReplacementCharacter) || decoded.toUtf8() != encoded ||
      !isValidText(decoded))
    return std::nullopt;
  return decoded;
}

std::optional<QByteArray> digestColumn(sqlite3_stmt *statement, int column) {
  const auto text = textColumn(statement, column);
  if (!text)
    return std::nullopt;
  const QByteArray value = text->toLatin1();
  return isDigest(value) ? std::optional<QByteArray>(value) : std::nullopt;
}

std::optional<std::vector<PendingOperation>> readPending(sqlite3 *database) {
  sqlite3_stmt *rawStatement = nullptr;
  constexpr const char *sql =
      "SELECT operation_id, target_path, temporary_path, expected_digest, replacement_digest, state "
      "FROM file_operation_log WHERE state <> 'completed' ORDER BY operation_id;";
  if (sqlite3_prepare_v2(database, sql, -1, &rawStatement, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
  std::vector<PendingOperation> operations;
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const auto operationId = textColumn(statement.get(), 0);
    const auto target = textColumn(statement.get(), 1);
    const auto temporary = textColumn(statement.get(), 2);
    const auto expected = digestColumn(statement.get(), 3);
    const auto replacement = digestColumn(statement.get(), 4);
    const auto state = textColumn(statement.get(), 5);
    if (!operationId || !target || !temporary || !expected || !replacement || !state)
      return std::nullopt;
    operations.push_back({*operationId, *target, *temporary, *expected, *replacement, *state});
  }
  if (result != SQLITE_DONE)
    return std::nullopt;
  return operations;
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
  }
  return "storage_unavailable";
}

bool FileOperationResult::isSucceeded() const { return code == FileOperationCode::none; }

bool FileRecoveryReport::isSucceeded() const { return code == FileOperationCode::none; }

FileOperationLog::FileOperationLog(QString databasePath, FileOperationFault fault)
    : databasePath_(std::move(databasePath)), fault_(fault) {}

FileOperationResult FileOperationLog::replaceIfUnchanged(const QString &targetPath,
                                                         const QByteArray &expectedBaselineSha256,
                                                         const QByteArray &replacementContents) const {
  if (!isDigest(expectedBaselineSha256))
    return {FileOperationCode::invalid_argument, {}};
  const QString operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto paths = targetPaths(targetPath, operationId);
  if (!paths)
    return {FileOperationCode::invalid_argument, {}};
  const QByteArray replacementDigest =
      QCryptographicHash::hash(replacementContents, QCryptographicHash::Sha256).toHex();
  PendingOperation operation{operationId,       paths->target, paths->temporary, expectedBaselineSha256,
                             replacementDigest, "prepared"};
  const auto database = openDatabase(databasePath_);
  if (!database || !insertPrepared(database->get(), operation))
    return {FileOperationCode::storage_unavailable, operationId};
  const auto lock = lockTarget(paths->lock);
  QByteArray currentDigest;
  if (!lock || !readDigest(paths->target, &currentDigest)) {
    markManual(database->get(), operationId);
    return {FileOperationCode::manual_intervention_required, operationId};
  }
  if (currentDigest != expectedBaselineSha256) {
    return {markCompleted(database->get(), operationId, "baseline_conflict")
                ? FileOperationCode::baseline_conflict
                : FileOperationCode::manual_intervention_required,
            operationId};
  }
  if (!writeTemporaryFile(paths->temporary, replacementContents)) {
    return {markCompleted(database->get(), operationId, "write_failed")
                ? FileOperationCode::write_failed
                : FileOperationCode::manual_intervention_required,
            operationId};
  }
  if (!markTemporaryWritten(database->get(), operationId))
    return {FileOperationCode::manual_intervention_required, operationId};
  if (fault_ == FileOperationFault::after_temporary_written)
    return {FileOperationCode::recovery_required, operationId};
  QByteArray recheckedDigest;
  if (!readDigest(paths->target, &recheckedDigest) || recheckedDigest != expectedBaselineSha256 ||
      !replaceAtomically(paths->temporary, paths->target) || !markCompleted(database->get(), operationId))
    return {FileOperationCode::manual_intervention_required, operationId};
  return {FileOperationCode::none, operationId};
}

FileRecoveryReport FileOperationLog::recoverPending() const {
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
      const QFileInfo targetInformation(operation.targetPath);
      const QFileInfo temporaryInformation(operation.temporaryPath);
      const auto lock = targetPaths(operation.targetPath, operation.operationId);
      QByteArray targetDigest;
      QByteArray temporaryDigest;
      if (lock && lock->temporary == operation.temporaryPath && targetInformation.exists() &&
          targetInformation.isFile() && !targetInformation.isSymLink() && temporaryInformation.exists() &&
          temporaryInformation.isFile() && !temporaryInformation.isSymLink() && lockTarget(lock->lock) &&
          readDigest(operation.targetPath, &targetDigest) && readDigest(operation.temporaryPath, &temporaryDigest) &&
          targetDigest == operation.expectedDigest && temporaryDigest == operation.replacementDigest &&
          replaceAtomically(operation.temporaryPath, operation.targetPath) &&
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
