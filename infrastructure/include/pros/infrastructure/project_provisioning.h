#pragma once

#include <QByteArray>
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
  not_found,
};

/** 将项目创建结果码编码为稳定英文协议值；展示层不得依赖中文诊断判断分支。 */
[[nodiscard]] const char *projectProvisioningCodeName(ProjectProvisioningCode code);

/** 项目创建 saga 的可查询生命周期；与 Project 聚合的业务状态独立。 */
enum class ProjectProvisioningState { provisioning, ready, failed };

/** 仅用于故障注入，模拟资产基线已入账但尚未标记 ready 的可恢复中断。 */
enum class ProjectProvisioningFault { none, after_asset_recorded };

/** 创建一个项目及其首个 Markdown 资产所需的稳定输入。 */
struct ProjectProvisioningRequest final {
  QString operationId;
  std::string projectId;
  std::string title;
  QString assetName;
};

/** saga 执行或放弃的结构化结果。 */
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

/** provisioning 查询的结构化结果。 */
struct ProjectProvisioningQueryResult final {
  ProjectProvisioningCode code = ProjectProvisioningCode::storage_unavailable;
  std::optional<ProjectProvisioningSnapshot> value;
};

/**
 * 项目首个受管资产的可恢复创建 saga。
 *
 * @pre `databasePath` 已迁移到当前 schema；`authorizedRootPath` 是已授权、存在且非软链接的本地目录。
 * @note 操作先持久化 provisioning 意图，再以 O_EXCL 创建单个 Markdown 资产并保存 SHA-256 创建基线。重复提交
 * 同一 operation_id 只会核验或推进同一资产，绝不会生成第二个对象。放弃仅删除本操作已记录且仍等于创建基线的文件；
 * 碰撞、基线不符或任何无法证明归属的状态都保留文件并要求人工处置。
 */
class ProjectProvisioningSaga final {
public:
  /** 构造不访问数据库或文件系统；故障注入仅供 FI-V01-PROJ-01 测试。 */
  explicit ProjectProvisioningSaga(QString databasePath, QString authorizedRootPath,
                                   ProjectProvisioningFault fault = ProjectProvisioningFault::none);

  /**
   * 创建或继续同一项目创建操作。
   *
   * @return ready 仅在资产基线与磁盘内容可证明一致时返回成功；碰撞和用户修改保持 failed 并保留资产。
   * @note 同一 operation_id 的输入必须完全一致；同一调用可重复执行，因中断进入 provisioning 的操作会继续安全步骤。
   */
  [[nodiscard]] ProjectProvisioningResult provision(const ProjectProvisioningRequest &request) const;

  /**
   * 安全放弃尚未 ready 的创建操作。
   *
   * @return 仅删除本操作创建且仍等于创建 SHA-256 基线的资产；既有、丢失或已变化资产不会删除，并返回人工介入码。
   * @note 重复放弃不创建或覆盖资产；ready 操作不能通过此接口撤销。
   */
  [[nodiscard]] ProjectProvisioningResult abandon(const QString &operationId) const;

  /**
   * 读取 provisioning 页面所需的恢复状态。
   *
   * @return 找到时返回完整快照；查询无持久化或文件系统副作用，存储异常与未找到分别编码。
   */
  [[nodiscard]] ProjectProvisioningQueryResult query(const QString &operationId) const;

private:
  QString databasePath_;
  QString authorizedRootPath_;
  ProjectProvisioningFault fault_;
};

} // namespace pros::infrastructure
