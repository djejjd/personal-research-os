#pragma once

#include "pros/infrastructure/resource_resolver.h"

#include <QString>

#include <optional>
#include <string>

namespace pros::infrastructure {

/** 项目跨 SQLite 与文件系统创建操作的稳定结果码。 */
enum class ProjectProvisioningCode {
  none,
  invalid_argument,
  storage_unavailable,
  asset_collision,
  recovery_required,
  manual_intervention_required,
  not_found
};
[[nodiscard]] const char *projectProvisioningCodeName(ProjectProvisioningCode code);

/** 项目创建 saga 的可查询生命周期；与 Project 聚合的业务状态独立。 */
enum class ProjectProvisioningState { provisioning, ready, failed };
/** 仅用于故障注入，模拟资产基线入账或核验完成但尚未标记 ready 的可恢复中断。 */
enum class ProjectProvisioningFault { none, after_asset_recorded, after_asset_proven };

struct ProjectProvisioningRequest final {
  QString operationId;
  std::string projectId;
  std::string title;
  QString assetName;
};

struct ProjectProvisioningResult final {
  ProjectProvisioningCode code = ProjectProvisioningCode::storage_unavailable;
  QString operationId;
  ProjectProvisioningState state = ProjectProvisioningState::failed;
  [[nodiscard]] bool isSucceeded() const;
};

/** provisioning 页面所需的只读事实；不泄露授权根的物理绝对路径。 */
struct ProjectProvisioningSnapshot final {
  QString operationId;
  std::string projectId;
  std::string title;
  QString assetName;
  ProjectProvisioningState state = ProjectProvisioningState::failed;
  QString failureCode;
};

struct ProjectProvisioningQueryResult final {
  ProjectProvisioningCode code = ProjectProvisioningCode::storage_unavailable;
  std::optional<ProjectProvisioningSnapshot> value;
};

/**
 * 项目首个受管资产的可恢复创建 saga。
 *
 * @pre `databasePath` 已迁移到当前 schema，`resolver` 存活且 `rootId` 已由调用方显式授权。
 * @note 先持久化 root ID、授权 revision、根 identity 与相对路径；仅在同一受限根内的资产 identity 和摘要
 * 均可证明时，才在 SQLite 事务中创建 active Project 并标记 ready。provisioning/failed 永不写入 projects。
 */
class ProjectProvisioningSaga final {
public:
  /** 构造不访问数据库或文件系统；故障注入仅供 FI-V01-PROJ-01 测试。 */
  explicit ProjectProvisioningSaga(QString databasePath, const ResourceResolver &resolver, QString rootId,
                                   ProjectProvisioningFault fault = ProjectProvisioningFault::none);

  /** 根证明、授权 revision、资产 identity 或内容不符时永久隔离操作；不切换目录或授权。 */
  [[nodiscard]] ProjectProvisioningResult provision(const ProjectProvisioningRequest &request) const;

  /** 不自动删除 provisioning 资产；外部写者不受 flock 约束，所有放弃请求均转为人工处置。 */
  [[nodiscard]] ProjectProvisioningResult abandon(const QString &operationId) const;

  /** 读取 provisioning 页面所需的恢复状态；查询无持久化或文件系统副作用。 */
  [[nodiscard]] ProjectProvisioningQueryResult query(const QString &operationId) const;

private:
  QString databasePath_;
  const ResourceResolver &resolver_;
  QString rootId_;
  ProjectProvisioningFault fault_;
};

/**
 * 在应用启动时扫描所有未决项目创建操作。
 *
 * @note 每项只能通过其持久化的 root ID 在同一 resolver 中恢复；缺失授权、根替换或证明不符会转为人工介入。
 */
class ProjectProvisioningRecoveryCoordinator final {
public:
  explicit ProjectProvisioningRecoveryCoordinator(QString databasePath, const ResourceResolver &resolver);
  /** @return 仅存储故障阻断启动；不可证明项标记人工介入后继续扫描并返回 none。 */
  [[nodiscard]] ProjectProvisioningCode recoverPending() const;

private:
  QString databasePath_;
  const ResourceResolver &resolver_;
};

} // namespace pros::infrastructure
