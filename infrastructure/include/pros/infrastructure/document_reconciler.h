#pragma once

#include "pros/infrastructure/resource_resolver.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace pros::infrastructure {

/** reconcile 的稳定结果码；调用方必须按枚举而非展示文本恢复或提示。 */
enum class ReconcileCode {
  none,
  invalid_argument,
  storage_unavailable,
  resource_unavailable,
  identity_conflict,
};

/** 将 reconcile 结果码编码为稳定英文协议值。 */
[[nodiscard]] const char *reconcileCodeName(ReconcileCode code);

/** 已注册文档根的可解释协调健康状态。 */
enum class ReconcileHealth { ready, stale, unavailable, conflict };

/** 将协调健康状态编码为稳定英文协议值。 */
[[nodiscard]] const char *reconcileHealthName(ReconcileHealth health);

/** watcher 仅能提交的原始提示；relativePath 不会被当作移动或删除事实。 */
struct RawWatcherEvent final {
  QString eventId;
  QString rootId;
  QString relativePath;
};

/** 已注册文档的真实文件事实，不包含文件正文或物理路径。 */
struct RegisteredDocument final {
  QString documentId;
  QString rootId;
  QString relativePath;
  ResourceIdentity identity;
  QByteArray contentDigest;
  quint64 contentRevision = 0;
  bool tombstoned = false;
};

/** 文档注册和查询的结构化结果。 */
struct RegisteredDocumentResult final {
  ReconcileCode code = ReconcileCode::storage_unavailable;
  std::optional<RegisteredDocument> document;
  ResourceRejectCode resourceRejection = ResourceRejectCode::none;

  [[nodiscard]] bool isSucceeded() const;
};

/** 一次 reconcile 命令的稳定结果；同一 operationId 重放返回同一结果。 */
struct ReconcileResult final {
  ReconcileCode code = ReconcileCode::storage_unavailable;
  ReconcileHealth health = ReconcileHealth::unavailable;
  ResourceRejectCode resourceRejection = ResourceRejectCode::none;
  QString operationId;
  int updatedDocumentCount = 0;
  int tombstonedDocumentCount = 0;
  int conflictDocumentCount = 0;

  [[nodiscard]] bool isSucceeded() const;
};

/** 持久化读取的根协调状态；没有协调记录时返回 `stale`。 */
struct ReconcileHealthResult final {
  ReconcileCode code = ReconcileCode::storage_unavailable;
  ReconcileHealth health = ReconcileHealth::unavailable;
  ResourceRejectCode resourceRejection = ResourceRejectCode::none;
};

/**
 * watcher 原始事件与文档注册事实的幂等协调器。
 *
 * @pre `databasePath` 已由 `SchemaMigrator` 迁移到当前版本，`resolver` 在协调期间保持存活。
 * @note watcher 路径只会令对应根进入 `stale` 并触发扫描；实际正文、路径和文件身份仅经
 * `ResourceResolver` 重新读取。协调不写 Markdown，不自行创建文档或项目对象。
 */
class DocumentReconciler final {
public:
  DocumentReconciler(QString databasePath, const ResourceResolver &resolver);

  /**
   * 将已存在的授权普通文件登记为稳定文档身份。
   *
   * @return 成功时保存当前文件身份、内容 SHA-256 和 revision 1；同一事实重复登记幂等。不同文档
   * 竞争同一真实文件返回 `identity_conflict`，不会覆盖已有注册。
   */
  [[nodiscard]] RegisteredDocumentResult registerDocument(const QString &documentId, const QString &rootId,
                                                          const QString &relativePath) const;

  /**
   * 持久化 watcher 原始提示并将根健康置为 `stale`。
   *
   * @return 相同 eventId 与相同原始材料重放成功；同一 eventId 绑定不同材料返回 `invalid_argument`。
   * 原始路径不访问文件系统，也不作为身份、移动或删除结论。
   */
  [[nodiscard]] ReconcileCode enqueueRawEvent(const RawWatcherEvent &event) const;

  /**
   * 扫描授权根并以真实文件身份更新全部已注册文档。
   *
   * @return 同一 operationId 重放首次已持久化结果。移动保留 document ID；找不到身份会形成 tombstone；
   * 重复文件身份或路径竞争进入 `conflict`。根撤销、替换或不可读时不修改文档事实并返回可解释状态。
   */
  [[nodiscard]] ReconcileResult reconcile(const QString &rootId, const QString &operationId) const;

  /** 查询指定文档的最近注册事实；查询不读取文件系统且不产生副作用。 */
  [[nodiscard]] RegisteredDocumentResult document(const QString &documentId) const;

  /** 查询根的最近协调健康；没有记录的根为 `stale`，不能被解释为已同步。 */
  [[nodiscard]] ReconcileHealthResult health(const QString &rootId) const;

private:
  QString databasePath_;
  const ResourceResolver &resolver_;
};

} // namespace pros::infrastructure
