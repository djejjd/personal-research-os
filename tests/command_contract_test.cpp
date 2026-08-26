#include "pros/domain/command_contract.h"

#include <QtTest>

#include <stdexcept>

class CommandContractTest final : public QObject {
  Q_OBJECT

private slots:
  void rejectsNegativeRevisionAndEmptyStableIds();
  void detectsRevisionConflictWithoutChangingEitherRevision();
  void executesWhenOperationHasNotBeenRecorded();
  void replaysOriginalResultForSameOperationAndDigest();
  void rejectsReusedOperationKeyWithDifferentDigest();
  void rejectsOperationRecordForAnotherCaller();
  void exposesStableStorageUnavailableResult();
};

void CommandContractTest::rejectsNegativeRevisionAndEmptyStableIds() {
  QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pros::domain::Revision(-1));
  QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pros::domain::OperationKey("", "operation-1"));
  QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pros::domain::OperationKey("caller-1", ""));
  QVERIFY_THROWS_EXCEPTION(std::invalid_argument, {
    static_cast<void>(pros::domain::CommandResult::succeeded("", pros::domain::Revision(1)));
  });
}

void CommandContractTest::detectsRevisionConflictWithoutChangingEitherRevision() {
  const pros::domain::Revision expected(3);
  const pros::domain::Revision actual(4);

  QCOMPARE(pros::domain::verifyExpectedRevision(expected, actual),
           std::optional(pros::domain::CommandErrorCode::revision_conflict));
  QCOMPARE(expected.value(), std::int64_t(3));
  QCOMPARE(actual.value(), std::int64_t(4));
  QVERIFY(!pros::domain::verifyExpectedRevision(actual, actual).has_value());
}

void CommandContractTest::executesWhenOperationHasNotBeenRecorded() {
  const pros::domain::OperationKey key("desktop-user", "operation-1");
  const pros::domain::OperationReplayDecision decision =
      pros::domain::decideOperationReplay(key, "request-a", std::nullopt);

  QCOMPARE(decision.action, pros::domain::OperationReplayAction::execute);
  QVERIFY(!decision.result.has_value());
}

void CommandContractTest::replaysOriginalResultForSameOperationAndDigest() {
  const pros::domain::OperationKey key("desktop-user", "operation-1");
  const pros::domain::CommandResult original =
      pros::domain::CommandResult::succeeded("task-1", pros::domain::Revision(2));
  const pros::domain::RecordedOperation recorded{key, "request-a", original};

  const pros::domain::OperationReplayDecision decision =
      pros::domain::decideOperationReplay(key, "request-a", recorded);

  QCOMPARE(decision.action, pros::domain::OperationReplayAction::replay);
  QCOMPARE(decision.result, std::optional(original));
}

void CommandContractTest::rejectsReusedOperationKeyWithDifferentDigest() {
  const pros::domain::OperationKey key("desktop-user", "operation-1");
  const pros::domain::RecordedOperation recorded{
      key, "request-a", pros::domain::CommandResult::succeeded("task-1", pros::domain::Revision(2))};

  const pros::domain::OperationReplayDecision decision =
      pros::domain::decideOperationReplay(key, "request-b", recorded);

  QCOMPARE(decision.action, pros::domain::OperationReplayAction::reject_reused_key);
  QCOMPARE(decision.result, std::optional(pros::domain::CommandResult::rejected(
                                pros::domain::CommandErrorCode::idempotency_key_reused)));
}

void CommandContractTest::rejectsOperationRecordForAnotherCaller() {
  const pros::domain::OperationKey requestedKey("desktop-user", "operation-1");
  const pros::domain::RecordedOperation recorded{
      pros::domain::OperationKey("another-user", "operation-1"), "request-a",
      pros::domain::CommandResult::succeeded("task-1", pros::domain::Revision(2))};

  QVERIFY_THROWS_EXCEPTION(std::invalid_argument, {
    static_cast<void>(pros::domain::decideOperationReplay(requestedKey, "request-a", recorded));
  });
}

void CommandContractTest::exposesStableStorageUnavailableResult() {
  const pros::domain::CommandResult result =
      pros::domain::CommandResult::rejected(pros::domain::CommandErrorCode::storage_unavailable);
  QVERIFY(!result.isSuccess());
  QCOMPARE(result.errorCode(), std::optional(pros::domain::CommandErrorCode::storage_unavailable));
}

QTEST_APPLESS_MAIN(CommandContractTest)

#include "command_contract_test.moc"
