#include "pros/infrastructure/project_provisioning.h"

#include <QCryptographicHash>
#include <QStringDecoder>

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

struct Record final {
  QString operationId;
  std::string projectId;
  std::string title;
  QString rootId;
  quint64 authorizationRevision = 0;
  ResourceIdentity rootIdentity;
  QString assetName;
  std::optional<ResourceIdentity> assetIdentity;
  QByteArray assetDigest;
  ProjectProvisioningState state = ProjectProvisioningState::failed;
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
  return validText(value) && !value.contains('/') && !value.contains('\\') && value != "." && value != "..";
}
bool isDigest(const QByteArray &value) {
  if (value.size() != 64)
    return false;
  for (const char character : value)
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')))
      return false;
  return true;
}
std::optional<Database> openDatabase(const QString &path, int flags) {
  sqlite3 *raw = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &raw, flags, nullptr) != SQLITE_OK) {
    if (raw != nullptr)
      sqlite3_close(raw);
    return std::nullopt;
  }
  Database database(raw);
  if (sqlite3_busy_timeout(database.get(), 5000) != SQLITE_OK)
    return std::nullopt;
  return database;
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
std::optional<QString> readText(sqlite3_stmt *statement, int index) {
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
std::optional<QString> readOptionalText(sqlite3_stmt *statement, int index) {
  return sqlite3_column_type(statement, index) == SQLITE_NULL ? std::optional<QString>(QString{})
                                                              : readText(statement, index);
}
std::optional<ProjectProvisioningState> parseState(const QString &value) {
  if (value == "provisioning")
    return ProjectProvisioningState::provisioning;
  if (value == "ready")
    return ProjectProvisioningState::ready;
  if (value == "failed")
    return ProjectProvisioningState::failed;
  return std::nullopt;
}
std::optional<Record> readRecord(sqlite3 *database, const QString &operationId, bool *found) {
  *found = false;
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "SELECT operation_id, project_id, title, root_id, authorization_revision, root_device, "
                              "root_inode, relative_path, "
                              "asset_device, asset_inode, asset_digest, state, failure_code FROM "
                              "project_provisioning_operations WHERE operation_id=?;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (!bindText(statement.get(), 1, operationId))
    return std::nullopt;
  const int step = sqlite3_step(statement.get());
  if (step == SQLITE_DONE)
    return Record{};
  if (step != SQLITE_ROW)
    return std::nullopt;
  const auto operation = readText(statement.get(), 0);
  const auto project = readText(statement.get(), 1);
  const auto title = readText(statement.get(), 2);
  const auto rootId = readText(statement.get(), 3);
  const auto asset = readText(statement.get(), 7);
  const auto state = readText(statement.get(), 11);
  const auto failure = readOptionalText(statement.get(), 12);
  const bool rootIntegers = sqlite3_column_type(statement.get(), 4) == SQLITE_INTEGER &&
                            sqlite3_column_type(statement.get(), 5) == SQLITE_INTEGER &&
                            sqlite3_column_type(statement.get(), 6) == SQLITE_INTEGER;
  const sqlite3_int64 revision = rootIntegers ? sqlite3_column_int64(statement.get(), 4) : 0;
  const sqlite3_int64 rootDevice = rootIntegers ? sqlite3_column_int64(statement.get(), 5) : -1;
  const sqlite3_int64 rootInode = rootIntegers ? sqlite3_column_int64(statement.get(), 6) : -1;
  std::optional<ResourceIdentity> assetIdentity;
  QByteArray digest;
  const bool anyAssetProof = sqlite3_column_type(statement.get(), 8) != SQLITE_NULL ||
                             sqlite3_column_type(statement.get(), 9) != SQLITE_NULL ||
                             sqlite3_column_type(statement.get(), 10) != SQLITE_NULL;
  if (anyAssetProof) {
    if (sqlite3_column_type(statement.get(), 8) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 9) != SQLITE_INTEGER ||
        sqlite3_column_type(statement.get(), 10) != SQLITE_TEXT)
      return std::nullopt;
    const sqlite3_int64 device = sqlite3_column_int64(statement.get(), 8);
    const sqlite3_int64 inode = sqlite3_column_int64(statement.get(), 9);
    digest = QByteArray(reinterpret_cast<const char *>(sqlite3_column_text(statement.get(), 10)),
                        sqlite3_column_bytes(statement.get(), 10));
    if (device < 0 || inode < 0 || !isDigest(digest))
      return std::nullopt;
    assetIdentity = ResourceIdentity{.device = static_cast<quint64>(device), .inode = static_cast<quint64>(inode)};
  }
  const auto parsedState = state ? parseState(*state) : std::nullopt;
  if (!operation || !project || !title || !rootId || !asset || !state || !failure || !rootIntegers || revision < 1 ||
      rootDevice < 0 || rootInode < 0 || !parsedState || sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::nullopt;
  *found = true;
  return Record{*operation,
                project->toStdString(),
                title->toStdString(),
                *rootId,
                static_cast<quint64>(revision),
                ResourceIdentity{.device = static_cast<quint64>(rootDevice), .inode = static_cast<quint64>(rootInode)},
                *asset,
                assetIdentity,
                digest,
                *parsedState,
                *failure};
}

bool createInitialRecord(sqlite3 *database, const ProjectProvisioningRequest &request, const ResourceRootProof &root) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql =
      "INSERT INTO project_provisioning_operations (operation_id, project_id, title, root_id, authorization_revision, "
      "root_device, root_inode, relative_path, asset_device, asset_inode, asset_digest, state, failure_code) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, NULL, NULL, NULL, 'provisioning', NULL);";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  return bindText(statement.get(), 1, request.operationId) && bindText(statement.get(), 2, request.projectId) &&
         bindText(statement.get(), 3, request.title) && bindText(statement.get(), 4, root.id) &&
         sqlite3_bind_int64(statement.get(), 5, static_cast<sqlite3_int64>(root.authorizationRevision)) == SQLITE_OK &&
         sqlite3_bind_int64(statement.get(), 6, static_cast<sqlite3_int64>(root.identity.device)) == SQLITE_OK &&
         sqlite3_bind_int64(statement.get(), 7, static_cast<sqlite3_int64>(root.identity.inode)) == SQLITE_OK &&
         bindText(statement.get(), 8, request.assetName) && sqlite3_step(statement.get()) == SQLITE_DONE;
}
bool markFailure(sqlite3 *database, const Record &record, const char *failureCode) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql =
      "UPDATE project_provisioning_operations SET state='failed', failure_code=? WHERE operation_id=? "
      "AND state <> 'ready';";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  return bindText(statement.get(), 1, QString::fromLatin1(failureCode)) &&
         bindText(statement.get(), 2, record.operationId) && sqlite3_step(statement.get()) == SQLITE_DONE &&
         sqlite3_changes(database) == 1;
}
bool recordAssetProof(sqlite3 *database, const Record &record, ResourceIdentity identity, const QByteArray &digest) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql =
      "UPDATE project_provisioning_operations SET asset_device=?, asset_inode=?, asset_digest=? "
      "WHERE operation_id=? AND state='provisioning' AND asset_digest IS NULL;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  return sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(identity.device)) == SQLITE_OK &&
         sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(identity.inode)) == SQLITE_OK &&
         sqlite3_bind_text64(statement.get(), 3, digest.constData(), static_cast<sqlite3_uint64>(digest.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK &&
         bindText(statement.get(), 4, record.operationId) && sqlite3_step(statement.get()) == SQLITE_DONE &&
         sqlite3_changes(database) == 1;
}
bool activateProject(sqlite3 *database, const Record &record) {
  if (!execute(database, "BEGIN IMMEDIATE;"))
    return false;
  sqlite3_stmt *projectRaw = nullptr;
  sqlite3_stmt *operationRaw = nullptr;
  const bool prepared =
      sqlite3_prepare_v2(database, "INSERT INTO projects (id, title, status, revision) VALUES (?, ?, 0, 1);", -1,
                         &projectRaw, nullptr) == SQLITE_OK &&
      sqlite3_prepare_v2(database,
                         "UPDATE project_provisioning_operations SET state='ready' "
                         "WHERE operation_id=? AND state='provisioning';",
                         -1, &operationRaw, nullptr) == SQLITE_OK;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> project(projectRaw, sqlite3_finalize);
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> operation(operationRaw, sqlite3_finalize);
  const bool written = prepared && bindText(project.get(), 1, record.projectId) &&
                       bindText(project.get(), 2, record.title) && sqlite3_step(project.get()) == SQLITE_DONE &&
                       bindText(operation.get(), 1, record.operationId) &&
                       sqlite3_step(operation.get()) == SQLITE_DONE && sqlite3_changes(database) == 1;
  if (!written) {
    execute(database, "ROLLBACK;");
    return false;
  }
  return execute(database, "COMMIT;");
}
bool rootMatches(const ResourceRootProof &proof, const Record &record) {
  return proof.id == record.rootId && proof.authorizationRevision == record.authorizationRevision &&
         proof.identity == record.rootIdentity;
}
bool readAndProveAsset(const ResourceResolver &resolver, const Record &record) {
  if (!record.assetIdentity || !isDigest(record.assetDigest))
    return false;
  const ResourceOpenResult opened =
      resolver.resolveAndOpen(record.rootId, record.assetName, ResourceOpenMode::read_only);
  if (!opened.isAccepted() || opened.handle->identity() != *record.assetIdentity)
    return false;
  QByteArray contents;
  ResourceRejectCode rejection = ResourceRejectCode::none;
  return opened.handle->readAll(&contents, &rejection) &&
         QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex() == record.assetDigest;
}
ProjectProvisioningResult failure(ProjectProvisioningCode code, const Record &record) {
  return {code, record.operationId, ProjectProvisioningState::failed};
}
std::optional<std::vector<Record>> pendingRecords(sqlite3 *database) {
  sqlite3_stmt *raw = nullptr;
  if (sqlite3_prepare_v2(database,
                         "SELECT operation_id FROM project_provisioning_operations WHERE state='provisioning' "
                         "ORDER BY operation_id;",
                         -1, &raw, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  std::vector<Record> records;
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    const auto id = readText(statement.get(), 0);
    bool found = false;
    const auto record = id ? readRecord(database, *id, &found) : std::optional<Record>{};
    if (!record || !found)
      return std::nullopt;
    records.push_back(*record);
  }
  return records;
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
ProjectProvisioningSaga::ProjectProvisioningSaga(QString databasePath, const ResourceResolver &resolver, QString rootId,
                                                 ProjectProvisioningFault fault)
    : databasePath_(std::move(databasePath)), resolver_(resolver), rootId_(std::move(rootId)), fault_(fault) {}

ProjectProvisioningResult ProjectProvisioningSaga::provision(const ProjectProvisioningRequest &request) const {
  if (!validText(request.operationId) || !validText(request.projectId) || !validText(request.title) ||
      !validAssetName(request.assetName) || !validText(rootId_))
    return {ProjectProvisioningCode::invalid_argument, request.operationId, ProjectProvisioningState::failed};
  const auto database = openDatabase(databasePath_, SQLITE_OPEN_READWRITE);
  if (!database)
    return {ProjectProvisioningCode::storage_unavailable, request.operationId, ProjectProvisioningState::failed};
  bool found = false;
  auto loaded = readRecord(database->get(), request.operationId, &found);
  if (!loaded)
    return {ProjectProvisioningCode::storage_unavailable, request.operationId, ProjectProvisioningState::failed};
  if (!found) {
    const auto proof = resolver_.rootProof(rootId_);
    if (!proof || !createInitialRecord(database->get(), request, *proof))
      return {proof ? ProjectProvisioningCode::storage_unavailable
                    : ProjectProvisioningCode::manual_intervention_required,
              request.operationId, ProjectProvisioningState::failed};
    loaded = readRecord(database->get(), request.operationId, &found);
    if (!loaded || !found)
      return {ProjectProvisioningCode::storage_unavailable, request.operationId, ProjectProvisioningState::failed};
  }
  Record record = *loaded;
  if (record.projectId != request.projectId || record.title != request.title || record.assetName != request.assetName ||
      record.rootId != rootId_)
    return {ProjectProvisioningCode::invalid_argument, request.operationId, record.state};
  if (record.state == ProjectProvisioningState::ready)
    return {ProjectProvisioningCode::none, record.operationId, record.state};
  if (record.state == ProjectProvisioningState::failed)
    return failure(record.failureCode == "asset_collision" ? ProjectProvisioningCode::asset_collision
                                                           : ProjectProvisioningCode::manual_intervention_required,
                   record);
  const auto proof = resolver_.rootProof(record.rootId);
  if (!proof || !rootMatches(*proof, record)) {
    markFailure(database->get(), record, "manual_intervention_required");
    return failure(ProjectProvisioningCode::manual_intervention_required, record);
  }
  if (!record.assetIdentity) {
    const QByteArray contents = "# " + QByteArray::fromStdString(record.title) + "\n";
    const QByteArray digest = QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex();
    const ResourceCreateResult created = resolver_.createExclusive(record.rootId, record.assetName, contents);
    if (!created.isAccepted()) {
      const bool collision = created.rejection == ResourceRejectCode::resource_already_exists;
      markFailure(database->get(), record, collision ? "asset_collision" : "manual_intervention_required");
      return failure(collision ? ProjectProvisioningCode::asset_collision
                               : ProjectProvisioningCode::manual_intervention_required,
                     record);
    }
    if (!recordAssetProof(database->get(), record, *created.identity, digest))
      return {ProjectProvisioningCode::storage_unavailable, record.operationId, ProjectProvisioningState::provisioning};
    record.assetIdentity = created.identity;
    record.assetDigest = digest;
    if (fault_ == ProjectProvisioningFault::after_asset_recorded)
      return {ProjectProvisioningCode::recovery_required, record.operationId, ProjectProvisioningState::provisioning};
  }
  if (!readAndProveAsset(resolver_, record)) {
    markFailure(database->get(), record, "manual_intervention_required");
    return failure(ProjectProvisioningCode::manual_intervention_required, record);
  }
  return activateProject(database->get(), record)
             ? ProjectProvisioningResult{ProjectProvisioningCode::none, record.operationId,
                                         ProjectProvisioningState::ready}
             : ProjectProvisioningResult{ProjectProvisioningCode::storage_unavailable, record.operationId,
                                         ProjectProvisioningState::provisioning};
}

ProjectProvisioningResult ProjectProvisioningSaga::abandon(const QString &operationId) const {
  if (!validText(operationId))
    return {ProjectProvisioningCode::invalid_argument, operationId, ProjectProvisioningState::failed};
  const auto database = openDatabase(databasePath_, SQLITE_OPEN_READWRITE);
  if (!database)
    return {ProjectProvisioningCode::storage_unavailable, operationId, ProjectProvisioningState::failed};
  bool found = false;
  const auto loaded = readRecord(database->get(), operationId, &found);
  if (!loaded)
    return {ProjectProvisioningCode::storage_unavailable, operationId, ProjectProvisioningState::failed};
  if (!found)
    return {ProjectProvisioningCode::not_found, operationId, ProjectProvisioningState::failed};
  const Record &record = *loaded;
  if (record.state == ProjectProvisioningState::ready)
    return failure(ProjectProvisioningCode::manual_intervention_required, record);
  if (record.state == ProjectProvisioningState::failed)
    return failure(record.failureCode == "safe_abandoned" ? ProjectProvisioningCode::none
                                                          : ProjectProvisioningCode::manual_intervention_required,
                   record);
  const auto proof = resolver_.rootProof(record.rootId);
  if (!proof || !rootMatches(*proof, record) || !record.assetIdentity || !readAndProveAsset(resolver_, record) ||
      !resolver_.removeIfIdentity(record.rootId, record.assetName, *record.assetIdentity).isAccepted()) {
    markFailure(database->get(), record, "manual_intervention_required");
    return failure(ProjectProvisioningCode::manual_intervention_required, record);
  }
  return markFailure(database->get(), record, "safe_abandoned")
             ? failure(ProjectProvisioningCode::none, record)
             : ProjectProvisioningResult{ProjectProvisioningCode::storage_unavailable, record.operationId,
                                         ProjectProvisioningState::provisioning};
}

ProjectProvisioningQueryResult ProjectProvisioningSaga::query(const QString &operationId) const {
  if (!validText(operationId))
    return {ProjectProvisioningCode::invalid_argument, std::nullopt};
  const auto database = openDatabase(databasePath_, SQLITE_OPEN_READWRITE);
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
ProjectProvisioningRecoveryCoordinator::ProjectProvisioningRecoveryCoordinator(QString databasePath,
                                                                               const ResourceResolver &resolver)
    : databasePath_(std::move(databasePath)), resolver_(resolver) {}
ProjectProvisioningCode ProjectProvisioningRecoveryCoordinator::recoverPending() const {
  const auto database = openDatabase(databasePath_, SQLITE_OPEN_READONLY);
  if (!database)
    return ProjectProvisioningCode::storage_unavailable;
  const auto pending = pendingRecords(database->get());
  if (!pending)
    return ProjectProvisioningCode::storage_unavailable;
  for (const Record &record : *pending) {
    const ProjectProvisioningResult result =
        ProjectProvisioningSaga(databasePath_, resolver_, record.rootId)
            .provision({record.operationId, record.projectId, record.title, record.assetName});
    if (result.code != ProjectProvisioningCode::none &&
        result.code != ProjectProvisioningCode::manual_intervention_required &&
        result.code != ProjectProvisioningCode::asset_collision)
      return result.code;
  }
  return ProjectProvisioningCode::none;
}
} // namespace pros::infrastructure
