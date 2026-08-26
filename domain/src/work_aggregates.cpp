#include "pros/domain/work_aggregates.h"

#include <stdexcept>
#include <utility>

namespace pros::domain {
namespace {

void requireValue(const std::string &value, const char *field) {
  if (value.empty())
    throw std::invalid_argument(field);
}

} // namespace

Project::Project(std::string id, std::string title, ProjectStatus status, Revision revision)
    : id_(std::move(id)), title_(std::move(title)), status_(status), revision_(revision) {
  requireValue(id_, "project id must not be empty");
  requireValue(title_, "project title must not be empty");
}
const std::string &Project::id() const { return id_; }
const std::string &Project::title() const { return title_; }
ProjectStatus Project::status() const { return status_; }
Revision Project::revision() const { return revision_; }
Project Project::complete() const {
  return Project(id_, title_, ProjectStatus::completed, Revision(revision_.value() + 1));
}

Task::Task(std::string id, std::string projectId, std::string title, TaskStatus status, Revision revision)
    : id_(std::move(id)), projectId_(std::move(projectId)), title_(std::move(title)), status_(status),
      revision_(revision) {
  requireValue(id_, "task id must not be empty");
  requireValue(projectId_, "task project id must not be empty");
  requireValue(title_, "task title must not be empty");
}
const std::string &Task::id() const { return id_; }
const std::string &Task::projectId() const { return projectId_; }
const std::string &Task::title() const { return title_; }
TaskStatus Task::status() const { return status_; }
Revision Task::revision() const { return revision_; }
Task Task::complete() const {
  return Task(id_, projectId_, title_, TaskStatus::completed, Revision(revision_.value() + 1));
}

Milestone::Milestone(std::string id, std::string projectId, std::string title, MilestoneStatus status,
                     Revision revision)
    : id_(std::move(id)), projectId_(std::move(projectId)), title_(std::move(title)), status_(status),
      revision_(revision) {
  requireValue(id_, "milestone id must not be empty");
  requireValue(projectId_, "milestone project id must not be empty");
  requireValue(title_, "milestone title must not be empty");
}
const std::string &Milestone::id() const { return id_; }
const std::string &Milestone::projectId() const { return projectId_; }
const std::string &Milestone::title() const { return title_; }
MilestoneStatus Milestone::status() const { return status_; }
Revision Milestone::revision() const { return revision_; }

Direction::Direction(std::string id, std::string title, DirectionStatus status, Revision revision)
    : id_(std::move(id)), title_(std::move(title)), status_(status), revision_(revision) {
  requireValue(id_, "direction id must not be empty");
  requireValue(title_, "direction title must not be empty");
}
const std::string &Direction::id() const { return id_; }
const std::string &Direction::title() const { return title_; }
DirectionStatus Direction::status() const { return status_; }
Revision Direction::revision() const { return revision_; }

} // namespace pros::domain
