#pragma once

#include "pros/infrastructure/resource_resolver.h"

#include <QString>
#include <QVector>

namespace pros::infrastructure {

/** 索引刷新和查询的稳定结果码；展示层必须结合 health 处理降级。 */
enum class KnowledgeIndexCode { none, invalid_argument, storage_unavailable, resource_unavailable, source_stale };

/** 将索引结果码编码为稳定英文协议值。 */
[[nodiscard]] const char *knowledgeIndexCodeName(KnowledgeIndexCode code);

/** 知识投影对调用方可见的可用性。 */
enum class KnowledgeIndexHealth { ready, rebuilding, stale, unavailable };

/** 将投影健康状态编码为稳定英文协议值。 */
[[nodiscard]] const char *knowledgeIndexHealthName(KnowledgeIndexHealth health);

/** 降级查询可以提供的、不会伪装成完整搜索结果的后续动作。 */
enum class KnowledgeRecoveryAction { browse_directory, open_original, rebuild_index };

/** 将恢复动作编码为稳定英文协议值。 */
[[nodiscard]] const char *knowledgeRecoveryActionName(KnowledgeRecoveryAction action);

/** 一个可打开但不暴露物理路径的知识投影条目。 */
struct KnowledgeItem final {
  QString documentId;
  QString rootId;
  QString relativePath;
  quint64 contentRevision = 0;
  bool structuredViewAvailable = false;
};

/**
 * 知识查询信封。
 *
 * @note 当 health 不是 ready 时 items 必为空，避免将旧世代或不完整投影表述为完整查询结果。
 */
struct KnowledgeQueryEnvelope final {
  KnowledgeIndexCode code = KnowledgeIndexCode::storage_unavailable;
  KnowledgeIndexHealth health = KnowledgeIndexHealth::unavailable;
  quint64 asOf = 0;
  QString reason;
  QVector<KnowledgeRecoveryAction> actions;
  QVector<KnowledgeItem> items;

  [[nodiscard]] bool isReady() const;
};

/** 单次投影刷新结果，包含已连续应用的 source watermark。 */
struct KnowledgeRebuildResult final {
  KnowledgeIndexCode code = KnowledgeIndexCode::storage_unavailable;
  KnowledgeIndexHealth health = KnowledgeIndexHealth::unavailable;
  quint64 asOf = 0;
  quint64 generation = 0;
  QString reason;

  [[nodiscard]] bool isSucceeded() const;
};

/**
 * 从受协调文档事实构建的可重建知识索引。
 *
 * @pre databasePath 已迁移至当前 schema，resolver 与受授权根在调用期间保持有效。
 * @note 索引只读取 DocumentReconciler 已提交的登记事实和授权文件；不会写 Markdown、业务聚合或
 * reconcile 表。刷新在新的 generation 中完成，只有 source watermark 连续且文件摘要与登记事实一致时
 * 才原子切换。重复刷新幂等地给出当前事实的投影。
 */
class KnowledgeIndex final {
public:
  KnowledgeIndex(QString databasePath, const ResourceResolver &resolver);

  /**
   * 以当前连续 source watermark 重建精确词、目录、标签和链接投影。
   *
   * @return 成功时 health 为 ready，asOf 是已连续处理的登记事实位置。根不可用、内容未完成
   * reconcile 或存储失败时不切换新世代，并返回可解释的降级状态。
   */
  [[nodiscard]] KnowledgeRebuildResult rebuild() const;

  /** 按 UTF-8 精确子串检索正文；不承诺分词、模糊匹配或大小写折叠。 */
  [[nodiscard]] KnowledgeQueryEnvelope searchExact(const QString &term) const;

  /** 按 V0.1 行首标签投影检索；tag 可以带或不带前缀 #。 */
  [[nodiscard]] KnowledgeQueryEnvelope queryTag(const QString &tag) const;

  /** 按标准相对链接或 wiki 链接的原始目标检索来源文档。 */
  [[nodiscard]] KnowledgeQueryEnvelope queryLinkTarget(const QString &target) const;

  /** 列出指定授权根当前已投影的文档目录。 */
  [[nodiscard]] KnowledgeQueryEnvelope listDirectory(const QString &rootId) const;

private:
  QString databasePath_;
  const ResourceResolver &resolver_;
};

} // namespace pros::infrastructure
