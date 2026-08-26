#include "pros/infrastructure/command_composition.h"

#include "pros/infrastructure/governance_composition.h"
#include "pros/infrastructure/sqlite_approval_command_handler.h"
#include "pros/infrastructure/sqlite_work_command_handler.h"

#include <utility>

namespace pros::infrastructure {
namespace {

class SqliteCommandFacade final : public application::CommandFacade {
public:
  explicit SqliteCommandFacade(QString databasePath)
      : work_(databasePath), governance_(makeGovernanceCommandHandler(databasePath)),
        approval_(std::move(databasePath)) {}

  domain::CommandResult execute(const application::CreateProject &command) override { return work_.handle(command); }

  domain::CommandResult execute(const application::CreateTask &command) override { return work_.handle(command); }

  domain::CommandResult execute(const application::UpdateTask &command) override { return work_.handle(command); }

  domain::CommandResult execute(const application::CreateMilestone &command) override { return work_.handle(command); }

  domain::CommandResult execute(const application::CreateDirection &command) override { return work_.handle(command); }

  domain::CommandResult execute(const application::LinkNoteToTask &command) override {
    return governance_->handle(command);
  }

  domain::CommandResult execute(const application::RecordEvidence &command) override {
    return governance_->handle(command);
  }

  domain::CommandResult execute(const application::RecordAcceptance &command) override {
    return governance_->handle(command);
  }

  domain::CommandResult execute(const application::CreateOperationPlan &command) override {
    return approval_.createOperationPlan(command);
  }

  domain::CommandResult execute(const application::RecordApproval &command) override {
    return approval_.recordApproval(command);
  }

  domain::CommandResult execute(const application::DispatchOperationPlan &command) override {
    return approval_.dispatchOperationPlan(command);
  }

private:
  SqliteWorkCommandHandler work_;
  std::unique_ptr<application::GovernanceCommandHandler> governance_;
  SqliteApprovalCommandHandler approval_;
};

} // namespace

std::unique_ptr<application::CommandFacade> makeCommandFacade(const QString &databasePath) {
  return std::make_unique<SqliteCommandFacade>(databasePath);
}

} // namespace pros::infrastructure
