#pragma once

#include "pros/application/governance_command_handler.h"
#include "pros/domain/governance.h"

#include <QString>

#include <memory>
#include <optional>
#include <string>

namespace pros::infrastructure {

/** 为已迁移到当前 schema 的数据库组装治理命令处理器。 */
[[nodiscard]] std::unique_ptr<application::GovernanceCommandHandler>
makeGovernanceCommandHandler(const QString &databasePath);

/** 治理事实只读查询；不创建 schema、不写入数据，也不参与命令幂等边界。 */
class GovernanceQuery final {
public:
  explicit GovernanceQuery(QString databasePath);

  /**
   * 按 task 反查 Note、Evidence、Acceptance 与 Activity 链。
   *
   * @return task 及治理目标存在时返回已提交事实；目标不存在或读取失败返回空，并通过 errorMessage 区分安全摘要。
   */
  [[nodiscard]] std::optional<domain::GovernanceTrace> traceForTask(const std::string &taskId,
                                                                    QString *errorMessage = nullptr) const;

private:
  QString databasePath_;
};

} // namespace pros::infrastructure
