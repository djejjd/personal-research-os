#pragma once

#include "pros/application/work_queries.h"

#include <QString>

namespace pros::infrastructure {

/** 以只读 SQLite 连接查询工作聚合；构造不打开数据库，也不运行迁移。 */
class SqliteWorkQueryService final : public application::WorkQueryService {
public:
  explicit SqliteWorkQueryService(QString databasePath);

  [[nodiscard]] application::WorkQueryResult<domain::Project> project(const std::string &id,
                                                                      QString *errorMessage = nullptr) const override;
  [[nodiscard]] application::WorkQueryResult<domain::Task> task(const std::string &id,
                                                                QString *errorMessage = nullptr) const override;
  [[nodiscard]] application::WorkQueryResult<domain::Milestone>
  milestone(const std::string &id, QString *errorMessage = nullptr) const override;
  [[nodiscard]] application::WorkQueryResult<domain::Direction>
  direction(const std::string &id, QString *errorMessage = nullptr) const override;

private:
  QString databasePath_;
};

} // namespace pros::infrastructure
