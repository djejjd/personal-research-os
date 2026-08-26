#include "pros/infrastructure/document_reconciler.h"

#include <QCryptographicHash>
#include <QStringDecoder>

#include <sqlite3.h>

#include <algorithm>
#include <map>
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

struct DocumentRow final {
  QString documentId;
  QString rootId;
  QString relativePath;
  ResourceIdentity identity;
  QByteArray digest;
  quint64 revision = 0;
  QString state;
};

struct Snapshot final {
  QString relativePath;
  ResourceIdentity identity;
  QByteArray digest;
};

struct ReconcileOperationRow final {
  ReconcileCode code = ReconcileCode::storage_unavailable;
  ReconcileHealth health = ReconcileHealth::unavailable;
  ResourceRejectCode resourceRejection = ResourceRejectCode::none;
  int updated = 0;
  int tombstoned = 0;
  int conflicts = 0;
};

bool validText(const QString &value) {
  if (value.isEmpty() || value.contains(QChar::Null))
    return false;
  const QByteArray encoded = value.toUtf8();
  QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
  static_cast<void>(decoder.decode(encoded));
  return !decoder.hasError();
}

bool validRawPath(const QString &value) { return !value.contains(QChar::Null); }

bool bindText(sqlite3_stmt *statement, int index, const QString &value) {
  const QByteArray encoded = value.toUtf8();
  return sqlite3_bind_text64(statement, index, encoded.constData(), static_cast<sqlite3_uint64>(encoded.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}

bool bindDigest(sqlite3_stmt *statement, int index, const QByteArray &value) {
  return sqlite3_bind_text64(statement, index, value.constData(), static_cast<sqlite3_uint64>(value.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}

bool execute(sqlite3 *database, const char *sql) {
  return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::optional<Database> openDatabase(const QString &databasePath) {
  sqlite3 *raw = nullptr;
  const QByteArray encoded = databasePath.toUtf8();
  if (sqlite3_open_v2(encoded.constData(), &raw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    if (raw != nullptr)
      sqlite3_close(raw);
    return std::nullopt;
  }
  Database database(raw);
  if (sqlite3_busy_timeout(database.get(), 5000) != SQLITE_OK)
    return std::nullopt;
  return database;
}

std::optional<QString> textColumn(sqlite3_stmt *statement, int column, bool allowEmpty = false) {
  if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
    return std::nullopt;
  const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
  const int bytes = sqlite3_column_bytes(statement, column);
  if (value == nullptr || bytes < 0)
    return std::nullopt;
  const QByteArray encoded(value, bytes);
  QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
  QString decoded = decoder.decode(encoded);
  if (decoder.hasError() || decoded.contains(QChar::Null) || (!allowEmpty && decoded.isEmpty()))
    return std::nullopt;
  return decoded;
}

std::optional<QByteArray> digestColumn(sqlite3_stmt *statement, int column) {
  const auto text = textColumn(statement, column);
  if (!text || text->size() != 64)
    return std::nullopt;
  const QByteArray digest = text->toLatin1();
  return std::all_of(digest.cbegin(), digest.cend(),
                     [](char character) {
                       return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
                     })
             ? std::optional<QByteArray>(digest)
             : std::nullopt;
}

std::optional<ResourceRejectCode> resourceRejectCodeFromName(const QString &name) {
  constexpr ResourceRejectCode codes[] = {
      ResourceRejectCode::none,
      ResourceRejectCode::invalid_root_path,
      ResourceRejectCode::root_overlap,
      ResourceRejectCode::root_not_found,
      ResourceRejectCode::root_revoked,
      ResourceRejectCode::root_identity_changed,
      ResourceRejectCode::invalid_relative_path,
      ResourceRejectCode::path_escape,
      ResourceRejectCode::access_denied,
      ResourceRejectCode::symlink_forbidden,
      ResourceRejectCode::resource_not_found,
      ResourceRejectCode::resource_open_failed,
      ResourceRejectCode::resource_not_regular_file,
  };
  for (const ResourceRejectCode code : codes) {
    if (name == QLatin1String(resourceRejectCodeName(code)))
      return code;
  }
  return std::nullopt;
}

std::optional<ReconcileCode> reconcileCodeFromName(const QString &name) {
  constexpr ReconcileCode codes[] = {ReconcileCode::none, ReconcileCode::invalid_argument,
                                     ReconcileCode::storage_unavailable, ReconcileCode::resource_unavailable,
                                     ReconcileCode::identity_conflict};
  for (const ReconcileCode code : codes) {
    if (name == QLatin1String(reconcileCodeName(code)))
      return code;
  }
  return std::nullopt;
}

std::optional<ReconcileHealth> reconcileHealthFromName(const QString &name) {
  constexpr ReconcileHealth values[] = {ReconcileHealth::ready, ReconcileHealth::stale, ReconcileHealth::unavailable,
                                        ReconcileHealth::conflict};
  for (const ReconcileHealth value : values) {
    if (name == QLatin1String(reconcileHealthName(value)))
      return value;
  }
  return std::nullopt;
}

bool isUnavailable(ResourceRejectCode code) {
  return code == ResourceRejectCode::root_not_found || code == ResourceRejectCode::root_revoked ||
         code == ResourceRejectCode::root_identity_changed || code == ResourceRejectCode::resource_open_failed ||
         code == ResourceRejectCode::access_denied;
}

std::optional<std::vector<DocumentRow>> loadDocuments(sqlite3 *database, const QString &rootId) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "SELECT document_id, root_id, relative_path, device, inode, content_digest, "
                              "content_revision, state FROM document_registry WHERE root_id = ? ORDER BY document_id;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (!bindText(statement.get(), 1, rootId))
    return std::nullopt;
  std::vector<DocumentRow> rows;
  int step = SQLITE_OK;
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const auto documentId = textColumn(statement.get(), 0);
    const auto storedRootId = textColumn(statement.get(), 1);
    const auto path = textColumn(statement.get(), 2);
    const auto digest = digestColumn(statement.get(), 5);
    const auto state = textColumn(statement.get(), 7);
    const sqlite3_int64 device = sqlite3_column_int64(statement.get(), 3);
    const sqlite3_int64 inode = sqlite3_column_int64(statement.get(), 4);
    const sqlite3_int64 revision = sqlite3_column_int64(statement.get(), 6);
    if (!documentId || !storedRootId || !path || !digest || !state || device < 0 || inode < 0 || revision < 1 ||
        (*state != "active" && *state != "tombstoned" && *state != "conflict"))
      return std::nullopt;
    rows.push_back({*documentId, *storedRootId, *path,
                    ResourceIdentity{.device = static_cast<quint64>(device), .inode = static_cast<quint64>(inode)},
                    *digest, static_cast<quint64>(revision), *state});
  }
  return step == SQLITE_DONE ? std::optional<std::vector<DocumentRow>>(std::move(rows)) : std::nullopt;
}

std::optional<DocumentRow> loadDocument(sqlite3 *database, const QString &documentId) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "SELECT document_id, root_id, relative_path, device, inode, content_digest, "
                              "content_revision, state FROM document_registry WHERE document_id = ?;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (!bindText(statement.get(), 1, documentId))
    return std::nullopt;
  if (sqlite3_step(statement.get()) != SQLITE_ROW)
    return std::nullopt;
  const auto id = textColumn(statement.get(), 0);
  const auto rootId = textColumn(statement.get(), 1);
  const auto path = textColumn(statement.get(), 2);
  const auto digest = digestColumn(statement.get(), 5);
  const auto state = textColumn(statement.get(), 7);
  const sqlite3_int64 device = sqlite3_column_int64(statement.get(), 3);
  const sqlite3_int64 inode = sqlite3_column_int64(statement.get(), 4);
  const sqlite3_int64 revision = sqlite3_column_int64(statement.get(), 6);
  if (!id || !rootId || !path || !digest || !state || device < 0 || inode < 0 || revision < 1 ||
      (*state != "active" && *state != "tombstoned" && *state != "conflict") ||
      sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::nullopt;
  return DocumentRow{
      *id,     *rootId,
      *path,   ResourceIdentity{.device = static_cast<quint64>(device), .inode = static_cast<quint64>(inode)},
      *digest, static_cast<quint64>(revision),
      *state};
}

RegisteredDocument toPublic(const DocumentRow &row) {
  return {.documentId = row.documentId,
          .rootId = row.rootId,
          .relativePath = row.relativePath,
          .identity = row.identity,
          .contentDigest = row.digest,
          .contentRevision = row.revision,
          .tombstoned = row.state == "tombstoned"};
}

RegisteredDocumentResult documentFailure(ReconcileCode code, ResourceRejectCode rejection = ResourceRejectCode::none) {
  return {.code = code, .document = std::nullopt, .resourceRejection = rejection};
}

bool updateHealth(sqlite3 *database, const QString &rootId, ReconcileHealth health, ResourceRejectCode rejection) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "INSERT INTO reconcile_health (root_id, health, resource_rejection) VALUES (?, ?, ?) "
                              "ON CONFLICT(root_id) DO UPDATE SET health = excluded.health, "
                              "resource_rejection = excluded.resource_rejection;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  return bindText(statement.get(), 1, rootId) &&
         bindText(statement.get(), 2, QLatin1String(reconcileHealthName(health))) &&
         bindText(statement.get(), 3, QLatin1String(resourceRejectCodeName(rejection))) &&
         sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool recordOperation(sqlite3 *database, const QString &operationId, const QString &rootId,
                     const ReconcileResult &result) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "INSERT INTO reconcile_operations (operation_id, root_id, result_code, health, "
                              "resource_rejection, updated_count, tombstoned_count, conflict_count) "
                              "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  return bindText(statement.get(), 1, operationId) && bindText(statement.get(), 2, rootId) &&
         bindText(statement.get(), 3, QLatin1String(reconcileCodeName(result.code))) &&
         bindText(statement.get(), 4, QLatin1String(reconcileHealthName(result.health))) &&
         bindText(statement.get(), 5, QLatin1String(resourceRejectCodeName(result.resourceRejection))) &&
         sqlite3_bind_int(statement.get(), 6, result.updatedDocumentCount) == SQLITE_OK &&
         sqlite3_bind_int(statement.get(), 7, result.tombstonedDocumentCount) == SQLITE_OK &&
         sqlite3_bind_int(statement.get(), 8, result.conflictDocumentCount) == SQLITE_OK &&
         sqlite3_step(statement.get()) == SQLITE_DONE;
}

std::optional<ReconcileOperationRow> loadOperation(sqlite3 *database, const QString &operationId, QString *rootId) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "SELECT root_id, result_code, health, resource_rejection, updated_count, "
                              "tombstoned_count, conflict_count FROM reconcile_operations WHERE operation_id = ?;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (!bindText(statement.get(), 1, operationId) || sqlite3_step(statement.get()) != SQLITE_ROW)
    return std::nullopt;
  const auto storedRoot = textColumn(statement.get(), 0);
  const auto code = textColumn(statement.get(), 1);
  const auto health = textColumn(statement.get(), 2);
  const auto rejection = textColumn(statement.get(), 3);
  const int updated = sqlite3_column_int(statement.get(), 4);
  const int tombstoned = sqlite3_column_int(statement.get(), 5);
  const int conflicts = sqlite3_column_int(statement.get(), 6);
  if (!storedRoot || !code || !health || !rejection || updated < 0 || tombstoned < 0 || conflicts < 0 ||
      sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::nullopt;
  const auto parsedCode = reconcileCodeFromName(*code);
  const auto parsedHealth = reconcileHealthFromName(*health);
  const auto parsedRejection = resourceRejectCodeFromName(*rejection);
  if (!parsedCode || !parsedHealth || !parsedRejection)
    return std::nullopt;
  *rootId = *storedRoot;
  return ReconcileOperationRow{*parsedCode, *parsedHealth, *parsedRejection, updated, tombstoned, conflicts};
}

/** 将已持久化的协调结果还原为调用方可重放的响应，不重新扫描或修改资源。 */
ReconcileResult replayedOperation(const ReconcileOperationRow &operation, const QString &operationId) {
  return {.code = operation.code,
          .health = operation.health,
          .resourceRejection = operation.resourceRejection,
          .operationId = operationId,
          .updatedDocumentCount = operation.updated,
          .tombstonedDocumentCount = operation.tombstoned,
          .conflictDocumentCount = operation.conflicts};
}

bool markQueuedEventsReconciled(sqlite3 *database, const QString &rootId) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql =
      "UPDATE watcher_event_queue SET state = 'reconciled' WHERE root_id = ? AND state = 'queued';";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  return bindText(statement.get(), 1, rootId) && sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool updateDocument(sqlite3 *database, const DocumentRow &row, const Snapshot &snapshot, const QString &state,
                    quint64 revision) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql =
      "UPDATE document_registry SET relative_path = ?, device = ?, inode = ?, content_digest = ?, "
      "content_revision = ?, state = ? WHERE document_id = ?;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  return bindText(statement.get(), 1, snapshot.relativePath) &&
         sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(snapshot.identity.device)) == SQLITE_OK &&
         sqlite3_bind_int64(statement.get(), 3, static_cast<sqlite3_int64>(snapshot.identity.inode)) == SQLITE_OK &&
         bindDigest(statement.get(), 4, snapshot.digest) &&
         sqlite3_bind_int64(statement.get(), 5, static_cast<sqlite3_int64>(revision)) == SQLITE_OK &&
         bindText(statement.get(), 6, state) && bindText(statement.get(), 7, row.documentId) &&
         sqlite3_step(statement.get()) == SQLITE_DONE && sqlite3_changes(database) == 1;
}

bool tombstoneDocument(sqlite3 *database, const DocumentRow &row) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql =
      "UPDATE document_registry SET state = 'tombstoned', content_revision = content_revision + 1 "
      "WHERE document_id = ?;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  return bindText(statement.get(), 1, row.documentId) && sqlite3_step(statement.get()) == SQLITE_DONE &&
         sqlite3_changes(database) == 1;
}

bool conflictDocument(sqlite3 *database, const QString &documentId) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "UPDATE document_registry SET state = 'conflict' WHERE document_id = ?;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  return bindText(statement.get(), 1, documentId) && sqlite3_step(statement.get()) == SQLITE_DONE &&
         sqlite3_changes(database) == 1;
}

std::optional<std::vector<Snapshot>> scanRoot(const ResourceResolver &resolver, const QString &rootId,
                                              ResourceRejectCode *rejection) {
  const ResourceListResult listed = resolver.listRegularFiles(rootId);
  if (!listed.isAccepted()) {
    *rejection = listed.rejection;
    return std::nullopt;
  }
  std::vector<Snapshot> snapshots;
  for (const QString &path : listed.relativePaths) {
    const ResourceOpenResult opened = resolver.resolveAndOpen(rootId, path, ResourceOpenMode::read_only);
    if (!opened.isAccepted()) {
      if (opened.rejection == ResourceRejectCode::resource_not_found)
        continue;
      if (isUnavailable(opened.rejection)) {
        *rejection = opened.rejection;
        return std::nullopt;
      }
      continue;
    }
    QByteArray contents;
    ResourceRejectCode readRejection = ResourceRejectCode::none;
    if (!opened.handle->readAll(&contents, &readRejection)) {
      if (isUnavailable(readRejection)) {
        *rejection = readRejection;
        return std::nullopt;
      }
      continue;
    }
    snapshots.push_back(
        {path, opened.handle->identity(), QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()});
  }
  *rejection = ResourceRejectCode::none;
  return snapshots;
}

} // namespace

const char *reconcileCodeName(ReconcileCode code) {
  switch (code) {
  case ReconcileCode::none:
    return "none";
  case ReconcileCode::invalid_argument:
    return "invalid_argument";
  case ReconcileCode::storage_unavailable:
    return "storage_unavailable";
  case ReconcileCode::resource_unavailable:
    return "resource_unavailable";
  case ReconcileCode::identity_conflict:
    return "identity_conflict";
  }
  return "storage_unavailable";
}

const char *reconcileHealthName(ReconcileHealth health) {
  switch (health) {
  case ReconcileHealth::ready:
    return "ready";
  case ReconcileHealth::stale:
    return "stale";
  case ReconcileHealth::unavailable:
    return "unavailable";
  case ReconcileHealth::conflict:
    return "conflict";
  }
  return "unavailable";
}

bool RegisteredDocumentResult::isSucceeded() const { return code == ReconcileCode::none && document.has_value(); }

bool ReconcileResult::isSucceeded() const { return code == ReconcileCode::none; }

DocumentReconciler::DocumentReconciler(QString databasePath, const ResourceResolver &resolver)
    : databasePath_(std::move(databasePath)), resolver_(resolver) {}

RegisteredDocumentResult DocumentReconciler::registerDocument(const QString &documentId, const QString &rootId,
                                                              const QString &relativePath) const {
  if (!validText(documentId) || !validText(rootId) || !validText(relativePath))
    return documentFailure(ReconcileCode::invalid_argument);
  const ResourceOpenResult opened = resolver_.resolveAndOpen(rootId, relativePath, ResourceOpenMode::read_only);
  if (!opened.isAccepted())
    return documentFailure(ReconcileCode::resource_unavailable, opened.rejection);
  QByteArray contents;
  ResourceRejectCode rejection = ResourceRejectCode::none;
  if (!opened.handle->readAll(&contents, &rejection))
    return documentFailure(ReconcileCode::resource_unavailable, rejection);
  const Snapshot snapshot{relativePath, opened.handle->identity(),
                          QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()};

  const auto database = openDatabase(databasePath_);
  if (!database || !execute(database->get(), "BEGIN IMMEDIATE;"))
    return documentFailure(ReconcileCode::storage_unavailable);
  const auto existing = loadDocument(database->get(), documentId);
  if (existing) {
    const bool same = existing->rootId == rootId && existing->relativePath == snapshot.relativePath &&
                      existing->identity == snapshot.identity && existing->digest == snapshot.digest &&
                      existing->state == "active";
    if (!same || !execute(database->get(), "COMMIT;")) {
      execute(database->get(), "ROLLBACK;");
      return documentFailure(ReconcileCode::identity_conflict);
    }
    return {.code = ReconcileCode::none, .document = toPublic(*existing)};
  }

  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql =
      "INSERT INTO document_registry "
      "(document_id, root_id, relative_path, device, inode, content_digest, content_revision, state) "
      "VALUES (?, ?, ?, ?, ?, ?, 1, 'active');";
  bool inserted = sqlite3_prepare_v2(database->get(), sql, -1, &raw, nullptr) == SQLITE_OK;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  inserted =
      inserted && bindText(statement.get(), 1, documentId) && bindText(statement.get(), 2, rootId) &&
      bindText(statement.get(), 3, snapshot.relativePath) &&
      sqlite3_bind_int64(statement.get(), 4, static_cast<sqlite3_int64>(snapshot.identity.device)) == SQLITE_OK &&
      sqlite3_bind_int64(statement.get(), 5, static_cast<sqlite3_int64>(snapshot.identity.inode)) == SQLITE_OK &&
      bindDigest(statement.get(), 6, snapshot.digest) && sqlite3_step(statement.get()) == SQLITE_DONE;
  if (!inserted || !execute(database->get(), "COMMIT;")) {
    execute(database->get(), "ROLLBACK;");
    return documentFailure(ReconcileCode::identity_conflict);
  }
  return {.code = ReconcileCode::none,
          .document = RegisteredDocument{.documentId = documentId,
                                         .rootId = rootId,
                                         .relativePath = snapshot.relativePath,
                                         .identity = snapshot.identity,
                                         .contentDigest = snapshot.digest,
                                         .contentRevision = 1,
                                         .tombstoned = false}};
}

ReconcileCode DocumentReconciler::enqueueRawEvent(const RawWatcherEvent &event) const {
  if (!validText(event.eventId) || !validText(event.rootId) || !validRawPath(event.relativePath))
    return ReconcileCode::invalid_argument;
  const auto database = openDatabase(databasePath_);
  if (!database || !execute(database->get(), "BEGIN IMMEDIATE;"))
    return ReconcileCode::storage_unavailable;
  sqlite3_stmt *raw = nullptr;
  constexpr const char *selectSql = "SELECT root_id, relative_path FROM watcher_event_queue WHERE event_id = ?;";
  bool valid = sqlite3_prepare_v2(database->get(), selectSql, -1, &raw, nullptr) == SQLITE_OK;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> selection(raw, sqlite3_finalize);
  valid = valid && bindText(selection.get(), 1, event.eventId);
  const int step = valid ? sqlite3_step(selection.get()) : SQLITE_ERROR;
  if (step == SQLITE_ROW) {
    const auto storedRoot = textColumn(selection.get(), 0);
    const auto storedPath = textColumn(selection.get(), 1, true);
    valid = storedRoot && storedPath && *storedRoot == event.rootId && *storedPath == event.relativePath &&
            sqlite3_step(selection.get()) == SQLITE_DONE;
    if (valid)
      valid = updateHealth(database->get(), event.rootId, ReconcileHealth::stale, ResourceRejectCode::none);
    if (!valid || !execute(database->get(), "COMMIT;")) {
      execute(database->get(), "ROLLBACK;");
      return ReconcileCode::invalid_argument;
    }
    return ReconcileCode::none;
  }
  if (step != SQLITE_DONE) {
    execute(database->get(), "ROLLBACK;");
    return ReconcileCode::storage_unavailable;
  }
  raw = nullptr;
  constexpr const char *insertSql = "INSERT INTO watcher_event_queue (event_id, root_id, relative_path, state) "
                                    "VALUES (?, ?, ?, 'queued');";
  valid = sqlite3_prepare_v2(database->get(), insertSql, -1, &raw, nullptr) == SQLITE_OK;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> insertion(raw, sqlite3_finalize);
  valid = valid && bindText(insertion.get(), 1, event.eventId) && bindText(insertion.get(), 2, event.rootId) &&
          bindText(insertion.get(), 3, event.relativePath) && sqlite3_step(insertion.get()) == SQLITE_DONE &&
          updateHealth(database->get(), event.rootId, ReconcileHealth::stale, ResourceRejectCode::none);
  if (!valid || !execute(database->get(), "COMMIT;")) {
    execute(database->get(), "ROLLBACK;");
    return ReconcileCode::storage_unavailable;
  }
  return ReconcileCode::none;
}

ReconcileResult DocumentReconciler::reconcile(const QString &rootId, const QString &operationId) const {
  if (!validText(rootId) || !validText(operationId))
    return {.code = ReconcileCode::invalid_argument, .health = ReconcileHealth::stale, .operationId = operationId};
  const auto database = openDatabase(databasePath_);
  if (!database)
    return {
        .code = ReconcileCode::storage_unavailable, .health = ReconcileHealth::unavailable, .operationId = operationId};
  QString operationRoot;
  if (const auto previous = loadOperation(database->get(), operationId, &operationRoot); previous.has_value()) {
    if (operationRoot != rootId)
      return {.code = ReconcileCode::invalid_argument, .health = ReconcileHealth::stale, .operationId = operationId};
    return replayedOperation(*previous, operationId);
  }

  ResourceRejectCode scanRejection = ResourceRejectCode::none;
  const auto snapshots = scanRoot(resolver_, rootId, &scanRejection);
  if (!snapshots) {
    ReconcileResult result{.code = ReconcileCode::resource_unavailable,
                           .health = ReconcileHealth::unavailable,
                           .resourceRejection = scanRejection,
                           .operationId = operationId};
    if (!execute(database->get(), "BEGIN IMMEDIATE;"))
      return {.code = ReconcileCode::storage_unavailable,
              .health = ReconcileHealth::unavailable,
              .operationId = operationId};
    operationRoot.clear();
    if (const auto previous = loadOperation(database->get(), operationId, &operationRoot); previous.has_value()) {
      if (!execute(database->get(), "COMMIT;"))
        return {.code = ReconcileCode::storage_unavailable,
                .health = ReconcileHealth::unavailable,
                .operationId = operationId};
      return operationRoot == rootId ? replayedOperation(*previous, operationId)
                                     : ReconcileResult{.code = ReconcileCode::invalid_argument,
                                                       .health = ReconcileHealth::stale,
                                                       .operationId = operationId};
    }
    if (!updateHealth(database->get(), rootId, result.health, scanRejection) ||
        !recordOperation(database->get(), operationId, rootId, result) || !execute(database->get(), "COMMIT;")) {
      execute(database->get(), "ROLLBACK;");
      return {.code = ReconcileCode::storage_unavailable,
              .health = ReconcileHealth::unavailable,
              .operationId = operationId};
    }
    return result;
  }

  if (!execute(database->get(), "BEGIN IMMEDIATE;"))
    return {
        .code = ReconcileCode::storage_unavailable, .health = ReconcileHealth::unavailable, .operationId = operationId};
  operationRoot.clear();
  if (const auto previous = loadOperation(database->get(), operationId, &operationRoot); previous.has_value()) {
    if (!execute(database->get(), "COMMIT;"))
      return {.code = ReconcileCode::storage_unavailable,
              .health = ReconcileHealth::unavailable,
              .operationId = operationId};
    return operationRoot == rootId ? replayedOperation(*previous, operationId)
                                   : ReconcileResult{.code = ReconcileCode::invalid_argument,
                                                     .health = ReconcileHealth::stale,
                                                     .operationId = operationId};
  }
  const auto rows = loadDocuments(database->get(), rootId);
  if (!rows) {
    execute(database->get(), "ROLLBACK;");
    return {
        .code = ReconcileCode::storage_unavailable, .health = ReconcileHealth::unavailable, .operationId = operationId};
  }
  std::map<ResourceIdentity, std::vector<const Snapshot *>> byIdentity;
  for (const Snapshot &snapshot : *snapshots)
    byIdentity[snapshot.identity].push_back(&snapshot);

  std::map<QString, QString> desiredPaths;
  std::vector<const DocumentRow *> conflicts;
  std::vector<std::pair<const DocumentRow *, const Snapshot *>> matched;
  std::vector<const DocumentRow *> missing;
  for (const DocumentRow &row : *rows) {
    const auto found = byIdentity.find(row.identity);
    if (found == byIdentity.end()) {
      missing.push_back(&row);
      continue;
    }
    if (found->second.size() != 1) {
      conflicts.push_back(&row);
      continue;
    }
    const Snapshot *snapshot = found->second.front();
    const auto claimant = desiredPaths.find(snapshot->relativePath);
    if (claimant != desiredPaths.end() && claimant->second != row.documentId) {
      conflicts.push_back(&row);
      continue;
    }
    desiredPaths.emplace(snapshot->relativePath, row.documentId);
    matched.emplace_back(&row, snapshot);
  }
  for (const DocumentRow &row : *rows) {
    const auto desired = desiredPaths.find(row.relativePath);
    if (desired != desiredPaths.end() && desired->second != row.documentId &&
        std::find(conflicts.cbegin(), conflicts.cend(), &row) == conflicts.cend())
      conflicts.push_back(&row);
  }
  const auto hasConflict = [&conflicts](const DocumentRow *row) {
    return std::find(conflicts.cbegin(), conflicts.cend(), row) != conflicts.cend();
  };

  bool valid = true;
  for (const auto &[row, snapshot] : matched) {
    if (hasConflict(row) || row->relativePath == snapshot->relativePath)
      continue;
    const Snapshot provisional{QStringLiteral(".pros-reconcile-internal-") + row->documentId, row->identity,
                               row->digest};
    valid = valid && updateDocument(database->get(), *row, provisional, row->state, row->revision);
  }
  ReconcileResult result{.operationId = operationId};
  for (const auto &[row, snapshot] : matched) {
    if (hasConflict(row))
      continue;
    const bool changed =
        row->relativePath != snapshot->relativePath || row->digest != snapshot->digest || row->state != "active";
    const quint64 revision = changed ? row->revision + 1 : row->revision;
    valid = valid && updateDocument(database->get(), *row, *snapshot, "active", revision);
    if (changed)
      ++result.updatedDocumentCount;
  }
  for (const DocumentRow *row : missing) {
    if (hasConflict(row) || row->state == "tombstoned")
      continue;
    valid = valid && tombstoneDocument(database->get(), *row);
    ++result.tombstonedDocumentCount;
  }
  for (const DocumentRow *row : conflicts) {
    valid = valid && conflictDocument(database->get(), row->documentId);
    ++result.conflictDocumentCount;
  }
  if (result.conflictDocumentCount > 0) {
    result.code = ReconcileCode::identity_conflict;
    result.health = ReconcileHealth::conflict;
  } else {
    result.code = ReconcileCode::none;
    result.health = ReconcileHealth::ready;
  }
  valid = valid && updateHealth(database->get(), rootId, result.health, ResourceRejectCode::none) &&
          markQueuedEventsReconciled(database->get(), rootId) &&
          recordOperation(database->get(), operationId, rootId, result) && execute(database->get(), "COMMIT;");
  if (!valid) {
    execute(database->get(), "ROLLBACK;");
    return {
        .code = ReconcileCode::storage_unavailable, .health = ReconcileHealth::unavailable, .operationId = operationId};
  }
  return result;
}

RegisteredDocumentResult DocumentReconciler::document(const QString &documentId) const {
  if (!validText(documentId))
    return documentFailure(ReconcileCode::invalid_argument);
  const auto database = openDatabase(databasePath_);
  if (!database)
    return documentFailure(ReconcileCode::storage_unavailable);
  const auto row = loadDocument(database->get(), documentId);
  if (!row)
    return documentFailure(ReconcileCode::invalid_argument);
  return {.code = ReconcileCode::none, .document = toPublic(*row)};
}

ReconcileHealthResult DocumentReconciler::health(const QString &rootId) const {
  if (!validText(rootId))
    return {.code = ReconcileCode::invalid_argument, .health = ReconcileHealth::stale};
  const auto database = openDatabase(databasePath_);
  if (!database)
    return {.code = ReconcileCode::storage_unavailable, .health = ReconcileHealth::unavailable};
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "SELECT health, resource_rejection FROM reconcile_health WHERE root_id = ?;";
  if (sqlite3_prepare_v2(database->get(), sql, -1, &raw, nullptr) != SQLITE_OK)
    return {.code = ReconcileCode::storage_unavailable, .health = ReconcileHealth::unavailable};
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (!bindText(statement.get(), 1, rootId))
    return {.code = ReconcileCode::storage_unavailable, .health = ReconcileHealth::unavailable};
  if (sqlite3_step(statement.get()) == SQLITE_DONE)
    return {.code = ReconcileCode::none, .health = ReconcileHealth::stale};
  const auto storedHealth = textColumn(statement.get(), 0);
  const auto storedRejection = textColumn(statement.get(), 1);
  if (!storedHealth || !storedRejection || sqlite3_step(statement.get()) != SQLITE_DONE)
    return {.code = ReconcileCode::storage_unavailable, .health = ReconcileHealth::unavailable};
  const auto health = reconcileHealthFromName(*storedHealth);
  const auto rejection = resourceRejectCodeFromName(*storedRejection);
  if (!health || !rejection)
    return {.code = ReconcileCode::storage_unavailable, .health = ReconcileHealth::unavailable};
  return {.code = *health == ReconcileHealth::unavailable ? ReconcileCode::resource_unavailable : ReconcileCode::none,
          .health = *health,
          .resourceRejection = *rejection};
}

} // namespace pros::infrastructure
