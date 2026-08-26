#pragma once

#include "pros/domain/work_aggregates.h"

#include <QString>

#include <optional>
#include <string>
#include <utility>

namespace pros::application {

/** 工作聚合查询的稳定结果状态；未找到与存储不可用必须由调用方分别处理。 */
enum class WorkQueryStatus { found, not_found, invalid_argument, storage_unavailable };

/**
 * 单个工作聚合的只读查询结果。
 *
 * @note 只有 `found` 携带聚合；查询不产生持久化或其他外部副作用。
 */
template <typename Aggregate> class WorkQueryResult final {
public:
  [[nodiscard]] static WorkQueryResult found(Aggregate aggregate) {
    return WorkQueryResult(WorkQueryStatus::found, std::move(aggregate));
  }
  [[nodiscard]] static WorkQueryResult notFound() { return WorkQueryResult(WorkQueryStatus::not_found); }
  [[nodiscard]] static WorkQueryResult invalidArgument() { return WorkQueryResult(WorkQueryStatus::invalid_argument); }
  [[nodiscard]] static WorkQueryResult storageUnavailable() {
    return WorkQueryResult(WorkQueryStatus::storage_unavailable);
  }

  [[nodiscard]] WorkQueryStatus status() const { return status_; }
  [[nodiscard]] const std::optional<Aggregate> &value() const { return value_; }

private:
  explicit WorkQueryResult(WorkQueryStatus status, std::optional<Aggregate> value = std::nullopt)
      : status_(status), value_(std::move(value)) {}

  WorkQueryStatus status_;
  std::optional<Aggregate> value_;
};

/**
 * 工作聚合的公开只读查询端口。
 *
 * @pre `id` 必须是非空、无内嵌 NUL 的合法 UTF-8。
 * @return 找到、未找到、参数非法或存储不可用；失败诊断通过 `errorMessage` 返回中文摘要。
 */
class WorkQueryService {
public:
  virtual ~WorkQueryService() = default;
  [[nodiscard]] virtual WorkQueryResult<domain::Project> project(const std::string &id,
                                                                 QString *errorMessage = nullptr) const = 0;
  [[nodiscard]] virtual WorkQueryResult<domain::Task> task(const std::string &id,
                                                           QString *errorMessage = nullptr) const = 0;
  [[nodiscard]] virtual WorkQueryResult<domain::Milestone> milestone(const std::string &id,
                                                                     QString *errorMessage = nullptr) const = 0;
  [[nodiscard]] virtual WorkQueryResult<domain::Direction> direction(const std::string &id,
                                                                     QString *errorMessage = nullptr) const = 0;
};

} // namespace pros::application
