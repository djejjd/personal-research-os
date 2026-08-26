#include "pros/infrastructure/knowledge_index.h"

#include "pros/infrastructure/markdown_fidelity.h"

#include <QCryptographicHash>
#include <QStringDecoder>

#include <sqlite3.h>

#include <cstdint>
#include <limits>
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

struct ProjectionState final {
  quint64 generation = 0;
  KnowledgeIndexHealth health = KnowledgeIndexHealth::unavailable;
  quint64 checkpoint = 0;
  quint64 target = 0;
  QString reason;
};

struct RegisteredSnapshot final {
  QString documentId;
  QString rootId;
  QString relativePath;
  QByteArray digest;
  quint64 revision = 0;
  QString state;
};

bool validText(const QString &value) {
  if (value.isEmpty() || value.contains(QChar::Null))
    return false;
  const QByteArray encoded = value.toUtf8();
  QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
  static_cast<void>(decoder.decode(encoded));
  return !decoder.hasError();
}

bool bindText(sqlite3_stmt *statement, int index, const QString &value) {
  const QByteArray encoded = value.toUtf8();
  return sqlite3_bind_text64(statement, index, encoded.constData(), static_cast<sqlite3_uint64>(encoded.size()),
                             SQLITE_TRANSIENT, SQLITE_UTF8) == SQLITE_OK;
}

bool bindInteger(sqlite3_stmt *statement, int index, quint64 value) {
  return value <= static_cast<quint64>(std::numeric_limits<sqlite3_int64>::max()) &&
         sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

bool execute(sqlite3 *database, const char *sql) {
  return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::optional<Database> openDatabase(const QString &path, int flags) {
  sqlite3 *raw = nullptr;
  const QByteArray encoded = path.toUtf8();
  if (sqlite3_open_v2(encoded.constData(), &raw, flags, nullptr) != SQLITE_OK) {
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
  const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(statement, column));
  const int size = sqlite3_column_bytes(statement, column);
  if (text == nullptr || size < 0)
    return std::nullopt;
  const QByteArray encoded(text, size);
  QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
  QString value = decoder.decode(encoded);
  if (decoder.hasError() || value.contains(QChar::Null) || (!allowEmpty && value.isEmpty()))
    return std::nullopt;
  return value;
}

std::optional<QByteArray> digestColumn(sqlite3_stmt *statement, int column) {
  const auto value = textColumn(statement, column);
  if (!value || value->size() != 64)
    return std::nullopt;
  return value->toLatin1();
}

std::optional<KnowledgeIndexHealth> healthFromName(const QString &name) {
  constexpr KnowledgeIndexHealth values[] = {KnowledgeIndexHealth::ready, KnowledgeIndexHealth::rebuilding,
                                             KnowledgeIndexHealth::stale, KnowledgeIndexHealth::unavailable};
  for (const auto value : values) {
    if (name == QLatin1String(knowledgeIndexHealthName(value)))
      return value;
  }
  return std::nullopt;
}

std::optional<ProjectionState> readState(sqlite3 *database) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "SELECT generation, health, checkpoint_position, target_position, reason "
                              "FROM knowledge_projection_state WHERE singleton_id = 1;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (sqlite3_step(statement.get()) != SQLITE_ROW)
    return std::nullopt;
  const sqlite3_int64 generation = sqlite3_column_int64(statement.get(), 0);
  const auto health = textColumn(statement.get(), 1);
  const sqlite3_int64 checkpoint = sqlite3_column_int64(statement.get(), 2);
  const sqlite3_int64 target = sqlite3_column_int64(statement.get(), 3);
  const auto reason = textColumn(statement.get(), 4, true);
  if (generation < 0 || checkpoint < 0 || target < 0 || !health || !reason ||
      sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::nullopt;
  const auto parsedHealth = healthFromName(*health);
  if (!parsedHealth)
    return std::nullopt;
  return ProjectionState{static_cast<quint64>(generation), *parsedHealth, static_cast<quint64>(checkpoint),
                         static_cast<quint64>(target), *reason};
}

std::optional<quint64> sourceWatermark(sqlite3 *database) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "SELECT COUNT(*), COALESCE(MIN(position), 0), COALESCE(MAX(position), 0) "
                              "FROM knowledge_source_events;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (sqlite3_step(statement.get()) != SQLITE_ROW || sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER ||
      sqlite3_column_type(statement.get(), 1) != SQLITE_INTEGER ||
      sqlite3_column_type(statement.get(), 2) != SQLITE_INTEGER)
    return std::nullopt;
  const sqlite3_int64 count = sqlite3_column_int64(statement.get(), 0);
  const sqlite3_int64 minimum = sqlite3_column_int64(statement.get(), 1);
  const sqlite3_int64 maximum = sqlite3_column_int64(statement.get(), 2);
  if (count < 0 || minimum < 0 || maximum < 0 || sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::nullopt;
  if ((count == 0 && (minimum != 0 || maximum != 0)) || (count > 0 && (minimum != 1 || count != maximum)))
    return std::nullopt;
  return static_cast<quint64>(maximum);
}

std::optional<std::pair<KnowledgeIndexHealth, QString>> sourceReadiness(sqlite3 *database) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql =
      "SELECT CASE "
      "WHEN EXISTS (SELECT 1 FROM reconcile_health WHERE health = 'unavailable') THEN 'unavailable' "
      "WHEN EXISTS (SELECT 1 FROM watcher_event_queue WHERE state = 'queued') THEN 'stale' "
      "WHEN EXISTS (SELECT 1 FROM reconcile_health WHERE health IN ('stale', 'conflict')) THEN 'stale' "
      "ELSE 'ready' END;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (sqlite3_step(statement.get()) != SQLITE_ROW)
    return std::nullopt;
  const auto state = textColumn(statement.get(), 0);
  if (!state || sqlite3_step(statement.get()) != SQLITE_DONE)
    return std::nullopt;
  if (*state == "ready")
    return std::pair{KnowledgeIndexHealth::ready, QString()};
  if (*state == "unavailable")
    return std::pair{KnowledgeIndexHealth::unavailable, QStringLiteral("resource_unavailable")};
  if (*state == "stale")
    return std::pair{KnowledgeIndexHealth::stale, QStringLiteral("reconcile_pending")};
  return std::nullopt;
}

bool writeState(sqlite3 *database, KnowledgeIndexHealth health, quint64 checkpoint, quint64 target,
                const QString &reason, quint64 generation) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql =
      "UPDATE knowledge_projection_state SET generation = ?, health = ?, checkpoint_position = ?, "
      "target_position = ?, reason = ? WHERE singleton_id = 1;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  return bindInteger(statement.get(), 1, generation) &&
         bindText(statement.get(), 2, QLatin1String(knowledgeIndexHealthName(health))) &&
         bindInteger(statement.get(), 3, checkpoint) && bindInteger(statement.get(), 4, target) &&
         bindText(statement.get(), 5, reason) && sqlite3_step(statement.get()) == SQLITE_DONE &&
         sqlite3_changes(database) == 1;
}

std::optional<std::vector<RegisteredSnapshot>> loadSnapshots(sqlite3 *database) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "SELECT document_id, root_id, relative_path, content_digest, content_revision, state "
                              "FROM document_registry ORDER BY document_id;";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return std::nullopt;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  std::vector<RegisteredSnapshot> snapshots;
  int step = SQLITE_OK;
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const auto documentId = textColumn(statement.get(), 0);
    const auto rootId = textColumn(statement.get(), 1);
    const auto relativePath = textColumn(statement.get(), 2);
    const auto digest = digestColumn(statement.get(), 3);
    const sqlite3_int64 revision = sqlite3_column_int64(statement.get(), 4);
    const auto state = textColumn(statement.get(), 5);
    if (!documentId || !rootId || !relativePath || !digest || !state || revision < 1 ||
        (*state != "active" && *state != "tombstoned" && *state != "conflict"))
      return std::nullopt;
    snapshots.push_back({*documentId, *rootId, *relativePath, *digest, static_cast<quint64>(revision), *state});
  }
  return step == SQLITE_DONE ? std::optional<std::vector<RegisteredSnapshot>>(std::move(snapshots)) : std::nullopt;
}

QVector<KnowledgeRecoveryAction> actionsFor(KnowledgeIndexHealth health) {
  if (health == KnowledgeIndexHealth::ready)
    return {};
  return {KnowledgeRecoveryAction::browse_directory, KnowledgeRecoveryAction::open_original,
          KnowledgeRecoveryAction::rebuild_index};
}

KnowledgeRebuildResult failedRebuild(KnowledgeIndexCode code, KnowledgeIndexHealth health, quint64 checkpoint,
                                     quint64 generation, const QString &reason) {
  return {code, health, checkpoint, generation, reason};
}

KnowledgeQueryEnvelope unavailableQuery(KnowledgeIndexCode code = KnowledgeIndexCode::storage_unavailable) {
  return {code,
          KnowledgeIndexHealth::unavailable,
          0,
          QStringLiteral("storage_unavailable"),
          actionsFor(KnowledgeIndexHealth::unavailable),
          {}};
}

enum class QueryKind : std::uint8_t { exact, tag, link, directory };

KnowledgeQueryEnvelope executeQuery(const QString &databasePath, QueryKind kind, const QString &argument) {
  if (!validText(argument))
    return {KnowledgeIndexCode::invalid_argument,
            KnowledgeIndexHealth::unavailable,
            0,
            QStringLiteral("invalid_argument"),
            {},
            {}};
  auto opened = openDatabase(databasePath, SQLITE_OPEN_READONLY);
  if (!opened)
    return unavailableQuery();
  Database database = std::move(*opened);
  if (!execute(database.get(), "BEGIN;"))
    return unavailableQuery();
  const auto finish = [&](KnowledgeQueryEnvelope envelope) {
    return execute(database.get(), "COMMIT;") ? std::move(envelope) : unavailableQuery();
  };
  const auto state = readState(database.get());
  const auto watermark = sourceWatermark(database.get());
  const auto upstream = sourceReadiness(database.get());
  if (!state || !watermark || !upstream)
    return unavailableQuery();

  KnowledgeIndexHealth health = state->health;
  QString reason = state->reason;
  if (health == KnowledgeIndexHealth::ready && upstream->first != KnowledgeIndexHealth::ready) {
    health = upstream->first;
    reason = upstream->second;
  } else if (health == KnowledgeIndexHealth::ready && *watermark > state->checkpoint) {
    health = KnowledgeIndexHealth::stale;
    reason = QStringLiteral("source_advanced");
  }
  if (health != KnowledgeIndexHealth::ready)
    return finish({KnowledgeIndexCode::none, health, state->checkpoint, reason, actionsFor(health), {}});

  const char *sql = nullptr;
  switch (kind) {
  case QueryKind::exact:
    sql = "SELECT document_id, root_id, relative_path, content_revision, parse_status "
          "FROM knowledge_projection_documents WHERE generation = ? AND instr(body, ?) > 0 "
          "ORDER BY relative_path, document_id;";
    break;
  case QueryKind::tag:
    sql = "SELECT d.document_id, d.root_id, d.relative_path, d.content_revision, d.parse_status "
          "FROM knowledge_projection_documents d JOIN knowledge_projection_tags t "
          "ON t.generation = d.generation AND t.document_id = d.document_id "
          "WHERE d.generation = ? AND t.tag = ? ORDER BY d.relative_path, d.document_id;";
    break;
  case QueryKind::link:
    sql = "SELECT d.document_id, d.root_id, d.relative_path, d.content_revision, d.parse_status "
          "FROM knowledge_projection_documents d JOIN knowledge_projection_links l "
          "ON l.generation = d.generation AND l.document_id = d.document_id "
          "WHERE d.generation = ? AND l.target = ? ORDER BY d.relative_path, d.document_id;";
    break;
  case QueryKind::directory:
    sql =
        "SELECT document_id, root_id, relative_path, content_revision, parse_status "
        "FROM knowledge_projection_documents WHERE generation = ? AND root_id = ? ORDER BY relative_path, document_id;";
    break;
  }
  sqlite3_stmt *raw = nullptr;
  if (sqlite3_prepare_v2(database.get(), sql, -1, &raw, nullptr) != SQLITE_OK)
    return unavailableQuery();
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (!bindInteger(statement.get(), 1, state->generation) || !bindText(statement.get(), 2, argument))
    return unavailableQuery();
  QVector<KnowledgeItem> items;
  int step = SQLITE_OK;
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const auto documentId = textColumn(statement.get(), 0);
    const auto rootId = textColumn(statement.get(), 1);
    const auto relativePath = textColumn(statement.get(), 2);
    const sqlite3_int64 revision = sqlite3_column_int64(statement.get(), 3);
    const auto parseStatus = textColumn(statement.get(), 4);
    if (!documentId || !rootId || !relativePath || !parseStatus || revision < 1 ||
        (*parseStatus != "parsed" && *parseStatus != "degraded_invalid_utf8"))
      return unavailableQuery();
    items.append({*documentId, *rootId, *relativePath, static_cast<quint64>(revision), *parseStatus == "parsed"});
  }
  if (step != SQLITE_DONE)
    return unavailableQuery();
  return finish({KnowledgeIndexCode::none, KnowledgeIndexHealth::ready, state->checkpoint, {}, {}, std::move(items)});
}

bool insertDocument(sqlite3 *database, quint64 generation, const RegisteredSnapshot &snapshot,
                    const MarkdownDocument &markdown) {
  sqlite3_stmt *raw = nullptr;
  constexpr const char *sql = "INSERT INTO knowledge_projection_documents "
                              "(generation, document_id, root_id, relative_path, content_revision, parse_status, body) "
                              "VALUES (?, ?, ?, ?, ?, ?, ?);";
  if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
    return false;
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  return bindInteger(statement.get(), 1, generation) && bindText(statement.get(), 2, snapshot.documentId) &&
         bindText(statement.get(), 3, snapshot.rootId) && bindText(statement.get(), 4, snapshot.relativePath) &&
         bindInteger(statement.get(), 5, snapshot.revision) &&
         bindText(statement.get(), 6, QLatin1String(markdownParseStatusName(markdown.status))) &&
         bindText(statement.get(), 7, markdown.hasStructuredView() ? QString::fromUtf8(markdown.source) : QString()) &&
         sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool insertTagsAndLinks(sqlite3 *database, quint64 generation, const QString &documentId,
                        const MarkdownDocument &markdown) {
  for (const QString &tag : markdown.tags) {
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(database,
                           "INSERT OR IGNORE INTO knowledge_projection_tags (generation, document_id, tag) "
                           "VALUES (?, ?, ?);",
                           -1, &raw, nullptr) != SQLITE_OK)
      return false;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    if (!bindInteger(statement.get(), 1, generation) || !bindText(statement.get(), 2, documentId) ||
        !bindText(statement.get(), 3, tag) || sqlite3_step(statement.get()) != SQLITE_DONE)
      return false;
  }
  for (const MarkdownLink &link : markdown.links) {
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(database,
                           "INSERT OR IGNORE INTO knowledge_projection_links "
                           "(generation, document_id, target, kind) VALUES (?, ?, ?, ?);",
                           -1, &raw, nullptr) != SQLITE_OK)
      return false;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    const char *kind = link.kind == MarkdownLinkKind::relative_markdown ? "relative_markdown" : "wiki";
    if (!bindInteger(statement.get(), 1, generation) || !bindText(statement.get(), 2, documentId) ||
        !bindText(statement.get(), 3, link.target) || !bindText(statement.get(), 4, QLatin1String(kind)) ||
        sqlite3_step(statement.get()) != SQLITE_DONE)
      return false;
  }
  return true;
}

} // namespace

const char *knowledgeIndexCodeName(KnowledgeIndexCode code) {
  switch (code) {
  case KnowledgeIndexCode::none:
    return "none";
  case KnowledgeIndexCode::invalid_argument:
    return "invalid_argument";
  case KnowledgeIndexCode::storage_unavailable:
    return "storage_unavailable";
  case KnowledgeIndexCode::resource_unavailable:
    return "resource_unavailable";
  case KnowledgeIndexCode::source_stale:
    return "source_stale";
  }
  return "storage_unavailable";
}

const char *knowledgeIndexHealthName(KnowledgeIndexHealth health) {
  switch (health) {
  case KnowledgeIndexHealth::ready:
    return "ready";
  case KnowledgeIndexHealth::rebuilding:
    return "rebuilding";
  case KnowledgeIndexHealth::stale:
    return "stale";
  case KnowledgeIndexHealth::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

const char *knowledgeRecoveryActionName(KnowledgeRecoveryAction action) {
  switch (action) {
  case KnowledgeRecoveryAction::browse_directory:
    return "browse_directory";
  case KnowledgeRecoveryAction::open_original:
    return "open_original";
  case KnowledgeRecoveryAction::rebuild_index:
    return "rebuild_index";
  }
  return "rebuild_index";
}

bool KnowledgeQueryEnvelope::isReady() const {
  return code == KnowledgeIndexCode::none && health == KnowledgeIndexHealth::ready;
}

bool KnowledgeRebuildResult::isSucceeded() const {
  return code == KnowledgeIndexCode::none && health == KnowledgeIndexHealth::ready;
}

KnowledgeIndex::KnowledgeIndex(QString databasePath, const ResourceResolver &resolver)
    : databasePath_(std::move(databasePath)), resolver_(resolver) {}

KnowledgeRebuildResult KnowledgeIndex::rebuild() const {
  auto opened = openDatabase(databasePath_, SQLITE_OPEN_READWRITE);
  if (!opened)
    return failedRebuild(KnowledgeIndexCode::storage_unavailable, KnowledgeIndexHealth::unavailable, 0, 0,
                         QStringLiteral("storage_unavailable"));
  Database database = std::move(*opened);
  if (!execute(database.get(), "BEGIN IMMEDIATE;"))
    return failedRebuild(KnowledgeIndexCode::storage_unavailable, KnowledgeIndexHealth::unavailable, 0, 0,
                         QStringLiteral("storage_unavailable"));
  const auto state = readState(database.get());
  const auto watermark = sourceWatermark(database.get());
  const auto snapshots = loadSnapshots(database.get());
  const auto upstream = sourceReadiness(database.get());
  if (!state || !watermark || !snapshots || !upstream) {
    execute(database.get(), "ROLLBACK;");
    return failedRebuild(KnowledgeIndexCode::storage_unavailable, KnowledgeIndexHealth::unavailable, 0, 0,
                         QStringLiteral("storage_unavailable"));
  }

  const auto fail = [&](KnowledgeIndexCode code, KnowledgeIndexHealth health, const QString &reason) {
    const bool stored = writeState(database.get(), health, state->checkpoint, *watermark, reason, state->generation) &&
                        execute(database.get(), "COMMIT;");
    if (!stored)
      execute(database.get(), "ROLLBACK;");
    return failedRebuild(stored ? code : KnowledgeIndexCode::storage_unavailable,
                         stored ? health : KnowledgeIndexHealth::unavailable, state->checkpoint, state->generation,
                         stored ? reason : QStringLiteral("storage_unavailable"));
  };

  if (upstream->first != KnowledgeIndexHealth::ready)
    return fail(upstream->first == KnowledgeIndexHealth::unavailable ? KnowledgeIndexCode::resource_unavailable
                                                                     : KnowledgeIndexCode::source_stale,
                upstream->first, upstream->second);

  const quint64 generation = state->generation + 1;
  for (const RegisteredSnapshot &snapshot : *snapshots) {
    if (snapshot.state == "tombstoned")
      continue;
    if (snapshot.state == "conflict")
      return fail(KnowledgeIndexCode::source_stale, KnowledgeIndexHealth::stale, QStringLiteral("reconcile_conflict"));
    const auto openedResource =
        resolver_.resolveAndOpen(snapshot.rootId, snapshot.relativePath, ResourceOpenMode::read_only);
    if (!openedResource.isAccepted())
      return fail(KnowledgeIndexCode::resource_unavailable, KnowledgeIndexHealth::unavailable,
                  QStringLiteral("resource_unavailable"));
    QByteArray source;
    ResourceRejectCode rejection = ResourceRejectCode::none;
    if (!openedResource.handle->readAll(&source, &rejection))
      return fail(KnowledgeIndexCode::resource_unavailable, KnowledgeIndexHealth::unavailable,
                  QStringLiteral("resource_unavailable"));
    const QByteArray digest = QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex();
    if (digest != snapshot.digest)
      return fail(KnowledgeIndexCode::source_stale, KnowledgeIndexHealth::stale,
                  QStringLiteral("source_not_reconciled"));
    const MarkdownDocument markdown = MarkdownParser::parse(source);
    if (!insertDocument(database.get(), generation, snapshot, markdown) ||
        !insertTagsAndLinks(database.get(), generation, snapshot.documentId, markdown)) {
      execute(database.get(), "ROLLBACK;");
      return failedRebuild(KnowledgeIndexCode::storage_unavailable, KnowledgeIndexHealth::unavailable,
                           state->checkpoint, state->generation, QStringLiteral("storage_unavailable"));
    }
  }
  if (!execute(database.get(), "DELETE FROM knowledge_projection_links WHERE generation <> (SELECT generation + 1 "
                               "FROM knowledge_projection_state WHERE singleton_id = 1);") ||
      !execute(database.get(), "DELETE FROM knowledge_projection_tags WHERE generation <> (SELECT generation + 1 "
                               "FROM knowledge_projection_state WHERE singleton_id = 1);") ||
      !execute(database.get(), "DELETE FROM knowledge_projection_documents WHERE generation <> (SELECT generation + 1 "
                               "FROM knowledge_projection_state WHERE singleton_id = 1);") ||
      !writeState(database.get(), KnowledgeIndexHealth::ready, *watermark, *watermark, {}, generation) ||
      !execute(database.get(), "COMMIT;")) {
    execute(database.get(), "ROLLBACK;");
    return failedRebuild(KnowledgeIndexCode::storage_unavailable, KnowledgeIndexHealth::unavailable, state->checkpoint,
                         state->generation, QStringLiteral("storage_unavailable"));
  }
  return {KnowledgeIndexCode::none, KnowledgeIndexHealth::ready, *watermark, generation, {}};
}

KnowledgeQueryEnvelope KnowledgeIndex::searchExact(const QString &term) const {
  return executeQuery(databasePath_, QueryKind::exact, term);
}

KnowledgeQueryEnvelope KnowledgeIndex::queryTag(const QString &tag) const {
  QString normalized = tag;
  if (normalized.startsWith(u'#'))
    normalized.remove(0, 1);
  return executeQuery(databasePath_, QueryKind::tag, normalized);
}

KnowledgeQueryEnvelope KnowledgeIndex::queryLinkTarget(const QString &target) const {
  return executeQuery(databasePath_, QueryKind::link, target);
}

KnowledgeQueryEnvelope KnowledgeIndex::listDirectory(const QString &rootId) const {
  return executeQuery(databasePath_, QueryKind::directory, rootId);
}

} // namespace pros::infrastructure
