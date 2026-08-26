#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace pros::domain {

/** 命令处理可稳定返回的失败原因；值是跨层协议的一部分，不得按展示文案判断。 */
enum class CommandErrorCode {
  invalid_argument,
  revision_conflict,
  idempotency_key_reused,
  unsupported_in_version,
  /** 仅由 handler 将事务存储故障映射后返回，不得写入 operation ledger。 */
  storage_unavailable,
};

/**
 * 聚合的单调版本号。
 *
 * @pre `value` 必须不小于零；零表示新建前的空版本。
 * @throws std::invalid_argument 当值为负时抛出；不执行持久化或其他副作用。
 */
class Revision final {
public:
  explicit Revision(std::int64_t value);

  [[nodiscard]] std::int64_t value() const;

  friend bool operator==(Revision left, Revision right) = default;

private:
  std::int64_t value_;
};

/**
 * 调用方在一次可重试命令中提供的幂等键。
 *
 * `callerId + operationId` 共同定义重放边界；两个字段均为非空稳定标识，不能由展示名称替代。
 */
class OperationKey final {
public:
  OperationKey(std::string callerId, std::string operationId);

  [[nodiscard]] const std::string &callerId() const;
  [[nodiscard]] const std::string &operationId() const;

  friend bool operator==(const OperationKey &, const OperationKey &) = default;

private:
  std::string callerId_;
  std::string operationId_;
};

/** 单次命令成功后可供重放的稳定结果摘要。 */
struct CommandSuccess {
  std::string aggregateId;
  Revision revision;

  friend bool operator==(const CommandSuccess &, const CommandSuccess &) = default;
};

/**
 * 命令的结构化结果。
 *
 * 成功结果必须携带对象 ID 和提交后的 revision；失败结果只公开稳定错误码，诊断信息由基础设施日志保存。
 */
class CommandResult final {
public:
  [[nodiscard]] static CommandResult succeeded(std::string aggregateId, Revision revision);
  [[nodiscard]] static CommandResult rejected(CommandErrorCode errorCode);

  [[nodiscard]] bool isSuccess() const;
  [[nodiscard]] const std::optional<CommandSuccess> &success() const;
  [[nodiscard]] const std::optional<CommandErrorCode> &errorCode() const;

  friend bool operator==(const CommandResult &, const CommandResult &) = default;

private:
  CommandResult(std::optional<CommandSuccess> success, std::optional<CommandErrorCode> errorCode);

  std::optional<CommandSuccess> success_;
  std::optional<CommandErrorCode> errorCode_;
};

/** 已持久化幂等记录在重试时提供的最小事实。 */
struct RecordedOperation {
  OperationKey key;
  std::string requestDigest;
  CommandResult result;
};

/** 幂等检查的确定性动作；Repository 必须在同一事务中据此读取或写入记录。 */
enum class OperationReplayAction {
  execute,
  replay,
  reject_reused_key,
};

/** 幂等判定结果；`replay` 携带首次结果，`reject_reused_key` 携带固定的键复用拒绝结果。 */
struct OperationReplayDecision {
  OperationReplayAction action;
  std::optional<CommandResult> result;
};

/**
 * 校验调用方的乐观并发条件。
 *
 * @return 版本相同返回 `std::nullopt`；不同时返回 `revision_conflict`，且不改变任何状态。
 */
[[nodiscard]] std::optional<CommandErrorCode> verifyExpectedRevision(Revision expected, Revision actual);

/**
 * 判定某请求是否可执行、重放或必须拒绝。
 *
 * @pre `requestDigest` 非空，且是覆盖全部命令输入的稳定摘要。
 * @return 无既有记录时执行；摘要相同则重放原结果；摘要不同固定拒绝 `idempotency_key_reused`。
 * @throws std::invalid_argument 输入摘要为空或记录键与当前键不一致时抛出；函数无副作用且可重复调用。
 */
[[nodiscard]] OperationReplayDecision decideOperationReplay(const OperationKey &key, const std::string &requestDigest,
                                                            const std::optional<RecordedOperation> &recordedOperation);

} // namespace pros::domain
