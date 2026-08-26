#pragma once

#include "pros/domain/command_contract.h"

#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace pros::infrastructure {

/** 命令成功或确定性拒绝时，与 operation 一起提交的事件、outbox 和活动事实。 */
struct CommandFact final {
  std::string eventId;
  std::string eventType;
  std::string aggregateType;
  std::string aggregateId;
  domain::Revision revision;
  int eventIndex;
  int schemaVersion;
  std::string payload;
  std::string activityKind;
  std::string activitySummary;
};

/** 业务写入回调的结果；存储故障与可稳定重放的业务拒绝必须显式区分。 */
struct CommandWorkResult final {
  domain::CommandResult result;
  std::vector<CommandFact> facts;
  bool storageSucceeded;

  [[nodiscard]] static CommandWorkResult completed(domain::CommandResult result, std::vector<CommandFact> facts = {});
  [[nodiscard]] static CommandWorkResult storageFailure();
};

/**
 * 在共享 SQLite 事务内执行一次幂等写命令。
 *
 * @pre 数据库已经由 `SchemaMigrator` 升级到当前版本；摘要和事实字段非空。`requestDigest` 只能由可信
 * application digester 对命令类型、全部业务字段及 expected revision 做版本化规范编码后生成，不能直接采用外部输入。
 * @param work 首次执行时在当前事务连接上写聚合，并返回确定性业务结果；回调不得提交、回滚或关闭连接。
 * @return 成功提交或重放时返回命令结果；打开数据库、业务写入、共享事实写入或提交失败时返回 `std::nullopt`。
 * @note 使用 `BEGIN IMMEDIATE` 串行化写入。普通成功至少提交一个事实；确定性失败可只提交 operation，或同时提交审计事实。
 * @note 相同 caller/operation 与摘要重放首次成功或失败；摘要不同固定返回 `idempotency_key_reused`，不执行回调。
 */
class SqliteCommandTransaction final {
public:
  using Work = std::function<CommandWorkResult(sqlite3 *)>;

  explicit SqliteCommandTransaction(QString databasePath);

  [[nodiscard]] std::optional<domain::CommandResult> execute(const domain::OperationKey &key,
                                                             const std::string &requestDigest, const Work &work,
                                                             QString *errorMessage = nullptr) const;

private:
  QString databasePath_;
};

} // namespace pros::infrastructure
