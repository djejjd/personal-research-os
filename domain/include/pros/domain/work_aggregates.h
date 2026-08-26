#pragma once

#include "pros/domain/command_contract.h"

#include <string>

namespace pros::domain {

/** 项目的显式生命周期状态；任务数量或任务状态不会改变此状态。 */
enum class ProjectStatus { active, completed };
/** 任务的独立生命周期状态；仅任务命令可改变它。 */
enum class TaskStatus { open, completed };
/** 里程碑的显式生命周期状态。 */
enum class MilestoneStatus { planned, achieved };
/** 方向的显式生命周期状态。 */
enum class DirectionStatus { active, archived };

/** 项目聚合；新建时 revision 为 1，任务变化不会隐式改变项目状态。 */
class Project final {
public:
  Project(std::string id, std::string title, ProjectStatus status, Revision revision);
  [[nodiscard]] const std::string &id() const;
  [[nodiscard]] const std::string &title() const;
  [[nodiscard]] ProjectStatus status() const;
  [[nodiscard]] Revision revision() const;
  [[nodiscard]] Project complete() const;

private:
  std::string id_;
  std::string title_;
  ProjectStatus status_;
  Revision revision_;
};

/** 独立任务聚合；其完成状态不写回所属项目。 */
class Task final {
public:
  Task(std::string id, std::string projectId, std::string title, TaskStatus status, Revision revision);
  [[nodiscard]] const std::string &id() const;
  [[nodiscard]] const std::string &projectId() const;
  [[nodiscard]] const std::string &title() const;
  [[nodiscard]] TaskStatus status() const;
  [[nodiscard]] Revision revision() const;
  [[nodiscard]] Task complete() const;

private:
  std::string id_;
  std::string projectId_;
  std::string title_;
  TaskStatus status_;
  Revision revision_;
};

/** 里程碑聚合；关联项目仅用于归属查询。 */
class Milestone final {
public:
  Milestone(std::string id, std::string projectId, std::string title, MilestoneStatus status, Revision revision);
  [[nodiscard]] const std::string &id() const;
  [[nodiscard]] const std::string &projectId() const;
  [[nodiscard]] const std::string &title() const;
  [[nodiscard]] MilestoneStatus status() const;
  [[nodiscard]] Revision revision() const;

private:
  std::string id_;
  std::string projectId_;
  std::string title_;
  MilestoneStatus status_;
  Revision revision_;
};

/** 研究方向聚合，可作为项目分类的稳定引用。 */
class Direction final {
public:
  Direction(std::string id, std::string title, DirectionStatus status, Revision revision);
  [[nodiscard]] const std::string &id() const;
  [[nodiscard]] const std::string &title() const;
  [[nodiscard]] DirectionStatus status() const;
  [[nodiscard]] Revision revision() const;

private:
  std::string id_;
  std::string title_;
  DirectionStatus status_;
  Revision revision_;
};

} // namespace pros::domain
