#include "pros/infrastructure/project_provisioning.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

#include <sqlite3.h>

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace pros::infrastructure {
namespace {

struct DatabaseCloser final {
  void operator()(sqlite3 *database) const { sqlite3_close(database); }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

struct Record final {
  QString operationId;
  std::string projectId;
  std::string title;
  QString assetName;
  QByteArray assetDigest;
  ProjectProvisioningState state;
  QString failureCode;
};

bool validText(const QString &value) { return !value.isEmpty() && !value.contains(QChar::Null); }

bool validText(const std::string &value) {
  if (value.empty() || value.find('\0') != std::string::npos)
    return false;
  QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
  static_cast<void>(decoder.decode(QByteArrayView(value.data(), static_cast<qsizetype>(value.size()))));
  return !decoder.hasError();
}

bool validAssetName(const QString &value) {
  return validText(value) && !QDir::isAbsolutePath(value) && !value.contains('/') && !value.contains('\\') &&
         value != "." && value != "..";
}

bool isDigest(const QByteArray &value) {
  if (value.size() != 64)
    return false;
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
      return false;
  }
  return true;
}

std::optional<QString> canonicalRoot(const QString &path) {
  if (!validText(path) || !QDir::isAbsolutePath(path))
    return std::nullopt;
  const QFileInfo information(path);
  if (!information.exists() || !information.isDir() || information.isSymLink() ||
      information.canonicalFilePath().isEmpty())
    return std::nullopt;
  return information.canonicalFilePath();
}

std::optional<Database> openDatabase(const QString &path, int flags) {
  sqlite3 *raw = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &raw, flags, nullptr) != SQLITE_OK) {
    if (raw != nullptr)
      sqlite3_close(raw);
    return std::nullopt;
  }
  Database database(raw);
  return sqlite3_busy_timeout(database.get(), 5000) == SQLITE_OK ? std::optional<Database>(std::move(database))
                                                                 : std::nullopt;
}

bool execute(sqlite3 *database, const char *sql) {
  return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool bindText(sqlite3_stmt *statement, int index, const QString &value) {
  const QByteArray encoded = value.toUtf8();
  return sqlite3_bind_text64(statement, index, encoded.constData(), static_cast<sqlite3_uint64>(encoded.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}

bool bindText(sqlite3_stmt *statement, int index, const std::string &value) {
  return sqlite3_bind_text64(statement, index, value.data(), static_cast<sqlite3_uint64>(value.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}

std::optional<QString> text(sqlite3_stmt *statement, int index) {
  if (sqlite3_column_type(statement, index) != SQLITE_TEXT)
    return std::nullopt;
  const auto *data = reinterpret_cast<const char *>(sqlite3_column_text(statement, index));
  const int size = sqlite3_column_bytes(statement, index);
  if (data == nullptr || size <= 0)
    return std::nullopt;
  const QByteArray encoded(data, size);
  const QString decoded = QString::fromUtf8(encoded);
  return !decoded.isNull() && decoded.toUtf8() == encoded && validText(decoded) ? std::optional<QString>(decoded)
                                                                                : std::nullopt;
}

std::optional<QString> optionalText(sqlite3_stmt *statement, int index) {
  if (sqlite3_column_type(statement, index) != SQLITE_TEXT)
    return std::nullopt;
  const auto *data = reinterpret_cast<const char *>(sqlite3_column_text(statement, index));
  const int size = sqlite3_column_bytes(statement, index);
  if (data == nullptr || size < 0)
    return std::nullopt;
  const QByteArray encoded(data, size);
  const QString decoded = QString::fromUtf8(encoded);
  return !decoded.isNull() && decoded.toUtf8() == encoded ? std::optional<QString>(decoded) : std::nullopt;
}

std::optional<ProjectProvisioningState> state(const QString &value) {
  if (value == "provisioning")
    return ProjectProvisioningState::provisioning;
  if (value == "ready")
    return ProjectProvisioningState::ready;
  if (value == "failed")
    return ProjectProvisioningState::failed;
  return std::nullopt;
}

const char *stateName(ProjectProvisioningState value) {
  switch (value) {
  case ProjectProvisioningState::provisioning:
    return "provisioning";
  case ProjectProvisioningState::ready:
    return "ready";
  case ProjectProvisioningState::failed:
    return "failed";
  }
  return "failed";
}

std::optional<Record> readRecord(sqlite3 *database, const QString &operationId, bool *found) {
  *found = false;
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "SELECT operation_id, project_id, title, asset_name, asset_digest, state, "
                              "COALESCE(failure_code, '') FROM project_provisioning_operations WHERE operation_id=?;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (!bindText(statement.get(), 1, operationId))
    return std::nullopt;
  const int stepped = sqlite3_step(statement.get());
  if (stepped == SQLITE_DONE)
    return Record{};
  if (stepped != SQLITE_ROW)
    return std::nullopt;
  const auto operation = text(statement.get(), 0);
  const auto project = text(statement.get(), 1);
  const auto title = text(statement.get(), 2);
  const auto asset = text(statement.get(), 3);
  const auto encodedDigest = sqlite3_column_type(statement.get(), 4) == SQLITE_NULL
                                 ? std::optional<QByteArray>(QByteArray())
                                 : std::optional<QByteArray>(QByteArray(
                                       reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 4)),
                                       sqlite3_column_bytes(statement.get(), 4)));
  const auto encodedState = text(statement.get(), 5);
  const auto failure = optionalText(statement.get(), 6);
  const auto parsedState = encodedState ? state(*encodedState) : std::nullopt;
  if (!operation || !project || !title || !asset || !encodedDigest || !encodedState || !failure ||
      (!encodedDigest->isEmpty() && !isDigest(*encodedDigest)) || !parsedState ||
      sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::nullopt;
  *found = true;
  return Record{*operation, project->toStdString(), title->toStdString(), *asset, *encodedDigest, *parsedState,
                *failure};
}

bool updateRecord(sqlite3 *database, const QString &operationId, ProjectProvisioningState newState,
                  const QByteArray &digest, const char *failureCode) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "UPDATE project_provisioning_operations SET state=?, asset_digest=?, failure_code=? "
                              "WHERE operation_id=?;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  const bool digestBound = digest.isEmpty() ? sqlite3_bind_null(statement.get(), 2) == SQLITE_OK
                                            : sqlite3_bind_text64(statement.get(), 2, digest.constData(),
                                                                  static_cast<sqlite3_uint64>(digest.size()),
                                                                  SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
  const bool failureBound = failureCode == nullptr ? sqlite3_bind_null(statement.get(), 3) == SQLITE_OK
                                                   : bindText(statement.get(), 3, QString::fromLatin1(failureCode));
  return bindText(statement.get(), 1, QString::fromLatin1(stateName(newState))) && digestBound && failureBound &&
         bindText(statement.get(), 4, operationId) && sqlite3_step(statement.get()) == SQLITE_DONE &&
         sqlite3_changes(database) == 1;
}

bool createInitialRecord(sqlite3 *database, const ProjectProvisioningRequest &request) {
  if (!execute(database, "BEGIN IMMEDIATE;"))
    return false;
  sqlite3_stmt *projectRaw = nullptr;
  sqlite3_stmt *operationRaw = nullptr;
  const bool prepared =
      sqlite3_prepare_v2(database, "INSERT INTO projects (id, title, status, revision) VALUES (?, ?, 0, 1);", -1,
                         &projectRaw, nullptr) == SQLITE_OK &&
      sqlite3_prepare_v2(database,
                         "INSERT INTO project_provisioning_operations "
                         "(operation_id, project_id, title, asset_name, asset_digest, state, failure_code) "
                         "VALUES (?, ?, ?, ?, NULL, 'provisioning', NULL);",
                         -1, &operationRaw, nullptr) == SQLITE_OK;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> project(projectRaw, sqlite3_finalize);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> operation(operationRaw, sqlite3_finalize);
  const bool written = prepared && bindText(project.get(), 1, request.projectId) &&
                       bindText(project.get(), 2, request.title) && sqlite3_step(project.get()) == SQLITE_DONE &&
                       bindText(operation.get(), 1, request.operationId) &&
                       bindText(operation.get(), 2, request.projectId) && bindText(operation.get(), 3, request.title) &&
                       bindText(operation.get(), 4, request.assetName) && sqlite3_step(operation.get()) == SQLITE_DONE;
  if (!written) {
    execute(database, "ROLLBACK;");
    return false;
  }
  return execute(database, "COMMIT;");
}

bool readDigest(const QString &path, QByteArray *output) {
  if (output == nullptr)
    return false;
  const QByteArray encoded = QFile::encodeName(path);
  const int descriptor = open(encoded.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat status{};
  if (descriptor < 0 || fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
    if (descriptor >= 0)
      close(descriptor);
    return false;
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  std::array<char, 8192> buffer{};
  for (;;) {
    const ssize_t count = read(descriptor, buffer.data(), buffer.size());
    if (count == 0)
      break;
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      close(descriptor);
      return false;
    }
    hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(count)));
  }
  close(descriptor);
  *output = hash.result().toHex();
  return true;
}

bool createFile(const QString &path, const QByteArray &contents) {
  const QByteArray encoded = QFile::encodeName(path);
  const int descriptor = open(encoded.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return false;
  qsizetype offset = 0;
  while (offset < contents.size()) {
    const ssize_t written =
        write(descriptor, contents.constData() + offset, static_cast<size_t>(contents.size() - offset));
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      close(descriptor);
      return false;
    }
    offset += static_cast<qsizetype>(written);
  }
  const bool synced = fsync(descriptor) == 0;
  close(descriptor);
  return synced;
}

ProjectProvisioningResult failed(ProjectProvisioningCode code, const Record &record) {
  return {code, record.operationId, ProjectProvisioningState::failed};
}

} // namespace

const char *projectProvisioningCodeName(ProjectProvisioningCode code) {
  switch (code) {
  case ProjectProvisioningCode::none:
    return "none";
  case ProjectProvisioningCode::invalid_argument:
    return "invalid_argument";
  case ProjectProvisioningCode::storage_unavailable:
    return "storage_unavailable";
  case ProjectProvisioningCode::asset_collision:
    return "asset_collision";
  case ProjectProvisioningCode::recovery_required:
    return "recovery_required";
  case ProjectProvisioningCode::manual_intervention_required:
    return "manual_intervention_required";
  case ProjectProvisioningCode::not_found:
    return "not_found";
  }
  return "storage_unavailable";
}

bool ProjectProvisioningResult::isSucceeded() const { return code == ProjectProvisioningCode::none; }

ProjectProvisioningSaga::ProjectProvisioningSaga(QString databasePath, QString authorizedRootPath,
                                                 ProjectProvisioningFault fault)
    : databasePath_(std::move(databasePath)), authorizedRootPath_(std::move(authorizedRootPath)), fault_(fault) {}

ProjectProvisioningResult ProjectProvisioningSaga::provision(const ProjectProvisioningRequest &request) const {
  if (!validText(request.operationId) || !validText(request.projectId) || !validText(request.title) ||
      !validAssetName(request.assetName))
    return {ProjectProvisioningCode::invalid_argument, request.operationId, ProjectProvisioningState::failed};
  const auto root = canonicalRoot(authorizedRootPath_);
  if (!root)
    return {ProjectProvisioningCode::invalid_argument, request.operationId, ProjectProvisioningState::failed};
  const auto database = openDatabase(databasePath_, SQLITE_OPEN_READWRITE);
  if (!database)
    return {ProjectProvisioningCode::storage_unavailable, request.operationId, ProjectProvisioningState::failed};
  bool found = false;
  auto existing = readRecord(database->get(), request.operationId, &found);
  if (!existing)
    return {ProjectProvisioningCode::storage_unavailable, request.operationId, ProjectProvisioningState::failed};
  if (!found) {
    if (!createInitialRecord(database->get(), request))
      return {ProjectProvisioningCode::storage_unavailable, request.operationId, ProjectProvisioningState::failed};
    existing = readRecord(database->get(), request.operationId, &found);
    if (!existing || !found)
      return {ProjectProvisioningCode::storage_unavailable, request.operationId, ProjectProvisioningState::failed};
  }
  Record record = *existing;
  if (record.projectId != request.projectId || record.title != request.title || record.assetName != request.assetName)
    return {ProjectProvisioningCode::invalid_argument, request.operationId, record.state};
  if (record.state == ProjectProvisioningState::ready)
    return {ProjectProvisioningCode::none, request.operationId, record.state};

  const QString assetPath = QDir(*root).filePath(record.assetName);
  if (!record.assetDigest.isEmpty()) {
    QByteArray actual;
    if (!readDigest(assetPath, &actual) || actual != record.assetDigest ||
        !updateRecord(database->get(), record.operationId, ProjectProvisioningState::failed, record.assetDigest,
                      "manual_intervention_required"))
      return failed(ProjectProvisioningCode::manual_intervention_required, record);
    return updateRecord(database->get(), record.operationId, ProjectProvisioningState::ready, record.assetDigest,
                        nullptr)
               ? ProjectProvisioningResult{ProjectProvisioningCode::none, record.operationId,
                                           ProjectProvisioningState::ready}
               : ProjectProvisioningResult{ProjectProvisioningCode::storage_unavailable, record.operationId,
                                           ProjectProvisioningState::provisioning};
  }

  struct stat status{};
  if (lstat(QFile::encodeName(assetPath).constData(), &status) == 0) {
    updateRecord(database->get(), record.operationId, ProjectProvisioningState::failed, {}, "asset_collision");
    return failed(ProjectProvisioningCode::asset_collision, record);
  }
  const QByteArray contents = "# " + QByteArray::fromStdString(record.title) + "\n";
  const QByteArray digest = QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex();
  if (!createFile(assetPath, contents)) {
    updateRecord(database->get(), record.operationId, ProjectProvisioningState::failed, {}, "asset_collision");
    return failed(ProjectProvisioningCode::asset_collision, record);
  }
  if (!updateRecord(database->get(), record.operationId, ProjectProvisioningState::provisioning, digest, nullptr))
    return {ProjectProvisioningCode::storage_unavailable, record.operationId, ProjectProvisioningState::provisioning};
  if (fault_ == ProjectProvisioningFault::after_asset_recorded)
    return {ProjectProvisioningCode::recovery_required, record.operationId, ProjectProvisioningState::provisioning};
  return updateRecord(database->get(), record.operationId, ProjectProvisioningState::ready, digest, nullptr)
             ? ProjectProvisioningResult{ProjectProvisioningCode::none, record.operationId,
                                         ProjectProvisioningState::ready}
             : ProjectProvisioningResult{ProjectProvisioningCode::storage_unavailable, record.operationId,
                                         ProjectProvisioningState::provisioning};
}

ProjectProvisioningResult ProjectProvisioningSaga::abandon(const QString &operationId) const {
  if (!validText(operationId))
    return {ProjectProvisioningCode::invalid_argument, operationId, ProjectProvisioningState::failed};
  const auto root = canonicalRoot(authorizedRootPath_);
  const auto database = openDatabase(databasePath_, SQLITE_OPEN_READWRITE);
  if (!root)
    return {ProjectProvisioningCode::invalid_argument, operationId, ProjectProvisioningState::failed};
  if (!database)
    return {ProjectProvisioningCode::storage_unavailable, operationId, ProjectProvisioningState::failed};
  bool found = false;
  const auto read = readRecord(database->get(), operationId, &found);
  if (!read)
    return {ProjectProvisioningCode::storage_unavailable, operationId, ProjectProvisioningState::failed};
  if (!found)
    return {ProjectProvisioningCode::not_found, operationId, ProjectProvisioningState::failed};
  const Record &record = *read;
  if (record.state == ProjectProvisioningState::ready)
    return failed(ProjectProvisioningCode::manual_intervention_required, record);
  if (record.assetDigest.isEmpty()) {
    struct stat status{};
    const QString assetPath = QDir(*root).filePath(record.assetName);
    if (lstat(QFile::encodeName(assetPath).constData(), &status) == 0) {
      updateRecord(database->get(), operationId, ProjectProvisioningState::failed, {}, "manual_intervention_required");
      return failed(ProjectProvisioningCode::manual_intervention_required, record);
    }
    return updateRecord(database->get(), operationId, ProjectProvisioningState::failed, {}, "safe_abandoned")
               ? failed(ProjectProvisioningCode::none, record)
               : failed(ProjectProvisioningCode::storage_unavailable, record);
  }
  const QString assetPath = QDir(*root).filePath(record.assetName);
  QByteArray actual;
  if (!readDigest(assetPath, &actual) || actual != record.assetDigest || !QFile::remove(assetPath)) {
    updateRecord(database->get(), operationId, ProjectProvisioningState::failed, record.assetDigest,
                 "manual_intervention_required");
    return failed(ProjectProvisioningCode::manual_intervention_required, record);
  }
  return updateRecord(database->get(), operationId, ProjectProvisioningState::failed, record.assetDigest,
                      "safe_abandoned")
             ? failed(ProjectProvisioningCode::none, record)
             : failed(ProjectProvisioningCode::storage_unavailable, record);
}

ProjectProvisioningQueryResult ProjectProvisioningSaga::query(const QString &operationId) const {
  if (!validText(operationId))
    return {ProjectProvisioningCode::invalid_argument, std::nullopt};
  const auto database = openDatabase(databasePath_, SQLITE_OPEN_READONLY);
  if (!database)
    return {ProjectProvisioningCode::storage_unavailable, std::nullopt};
  bool found = false;
  const auto record = readRecord(database->get(), operationId, &found);
  if (!record)
    return {ProjectProvisioningCode::storage_unavailable, std::nullopt};
  if (!found)
    return {ProjectProvisioningCode::not_found, std::nullopt};
  return {ProjectProvisioningCode::none,
          ProjectProvisioningSnapshot{record->operationId, record->projectId, record->title, record->assetName,
                                      record->state, record->failureCode}};
}

} // namespace pros::infrastructure
