#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "pros/infrastructure/resource_resolver.h"

namespace pros::infrastructure {

/** 文件替换与恢复可持久化使用的稳定结果码。 */
enum class FileOperationCode {
  none,
  invalid_argument,
  storage_unavailable,
  baseline_conflict,
  write_failed,
  recovery_required,
  manual_intervention_required,
  operation_id_conflict,
  resource_rejected,
};

/** 将结果码编码为稳定英文协议值；调用方不得依赖展示文案判断结果。 */
[[nodiscard]] const char *fileOperationCodeName(FileOperationCode code);

/** 受控故障注入点，仅用于验证崩溃后恢复契约。 */
enum class FileOperationFault { none, after_temporary_written };

/** 一次比较并替换写入的结果；失败不暴露本地物理路径。 */
struct FileOperationResult final {
  FileOperationCode code = FileOperationCode::storage_unavailable;
  QString operationId;
  ResourceRejectCode resourceRejection = ResourceRejectCode::none;

  [[nodiscard]] bool isSucceeded() const;
};

/** 启动恢复扫描的摘要；人工介入项不会自动覆盖或删除。 */
struct FileRecoveryReport final {
  FileOperationCode code = FileOperationCode::none;
  int recoveredCount = 0;
  int manualInterventionCount = 0;
  QStringList operationIds;

  [[nodiscard]] bool isSucceeded() const;
};

/**
 * 记录并执行可恢复的本地文件 CAS 替换。
 *
 * @pre `databasePath` 已由 `SchemaMigrator` 迁移到当前 schema；目标只能以已授权 `rootId + relativePath` 定位。
 * @note 写入前先持久化期望 SHA-256 基线、临时文件与替换摘要。临时文件同步后才标记可恢复，随后以同目录原子替换
 * 完成写入并写入完成标记。出现任何无法证明结果的中断，恢复扫描会标为人工介入，不会猜测成功。
 */
class FileOperationLog final {
public:
  /**
   * 创建针对唯一 SQLite 日志库的文件操作记录器。
   *
   * @param fault 仅测试可设置 `after_temporary_written`，模拟临时文件已持久化但原子替换尚未执行的进程中断。
   * @note 构造不访问数据库或文件系统；不运行恢复扫描。
   */
  explicit FileOperationLog(QString databasePath, FileOperationFault fault = FileOperationFault::none);

  /**
   * 仅在目标当前 SHA-256 等于期望基线时，以新内容原子替换目标。
   *
   * @param resolver 唯一允许接触资源根的受限解析器；所有文件操作经其句柄相对能力完成。
   * @param rootId 当前写授权根的稳定 ID。
   * @param relativePath 根内既有普通文件的相对路径；拒绝绝对路径、空段、软链接和逃逸。
   * @param operationId 调用方持久化的幂等 ID；同 ID 的意图必须完全一致，才会继续或返回已持久化结果。
   * @param expectedBaselineSha256 64 个小写十六进制字符的可信基线摘要，不接受裸文件内容或用户展示值。
   * @return 成功仅在完成标记已持久化后返回；同 ID 篡改返回 `operation_id_conflict`；资源拒绝同时提供稳定
   * `resourceRejection`。中断或无法证明结果时返回恢复或人工介入码。
   * @note 同一目标的协作写入由 ResourceResolver 持有的受限旁路锁串行化；调用方必须把外部非协作写入视为基线冲突风险。
   */
  [[nodiscard]] FileOperationResult replaceIfUnchanged(const ResourceResolver &resolver, const QString &rootId,
                                                       const QString &relativePath, const QString &operationId,
                                                       const QByteArray &expectedBaselineSha256,
                                                       const QByteArray &replacementContents) const;

  /**
   * 扫描并处理未完成的文件替换记录。
   *
   * @param resolver 用于重新取得每条记录写授权的资源根能力；根撤销、替换或路径失效会转人工，不会退化为绝对路径访问。
   * @return 仅当全部记录已完成或不存在时返回成功。只有临时文件摘要与替换摘要一致且目标仍等于期望基线时才自动完成；
   * 其他状态均写为 `manual_intervention_required` 并保留相关文件供人工核对。重复扫描是幂等的。
   */
  [[nodiscard]] FileRecoveryReport recoverPending(const ResourceResolver &resolver) const;

private:
  QString databasePath_;
  FileOperationFault fault_;
};

} // namespace pros::infrastructure
