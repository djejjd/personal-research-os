#include "pros/infrastructure/document_reconciler.h"
#include "pros/infrastructure/resource_resolver.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdlib>

#include <array>
#include <barrier>
#include <thread>

namespace {

bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size() &&
         file.flush();
}

QString rootId(const pros::infrastructure::ResourceRootResult &result) {
  return result.root.has_value() ? result.root->id : QString();
}

QString initializedDatabase(QTemporaryDir &directory) {
  const QString path = directory.path() + "/reconcile.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  return migrator.migrate(path, &error) ? path : QString();
}

const pros::infrastructure::RegisteredDocument &
requireDocument(const pros::infrastructure::RegisteredDocumentResult &result) {
  if (!result.document.has_value())
    std::abort();
  return *result.document;
}

} // namespace

class DocumentReconcilerTest final : public QObject {
  Q_OBJECT

private slots:
  void reconcilesExternalEditMoveDeleteAndOperationReplay_V01_KNOW_02_03();
  void ignoresReplayedAndOutOfOrderWatcherSignals_V01_KNOW_02_03();
  void exposesUnavailableRootHealthWithoutChangingRegisteredFact();
  void serializesConcurrentSameOperationIdReplay();
};

void DocumentReconcilerTest::reconcilesExternalEditMoveDeleteAndOperationReplay_V01_KNOW_02_03() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  QVERIFY(QDir().mkpath(directory.path() + "/notes/archive"));
  const QString entry = directory.path() + "/notes/entry.md";
  QVERIFY(writeFile(entry, "first"));

  pros::infrastructure::ResourceResolver resolver;
  const auto root = resolver.registerRoot(directory.path() + "/notes", pros::infrastructure::ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  pros::infrastructure::DocumentReconciler reconciler(databasePath, resolver);
  QVERIFY(reconciler.registerDocument("doc-001", rootId(root), "entry.md").isSucceeded());

  QVERIFY(writeFile(entry, "externally edited"));
  QCOMPARE(reconciler.enqueueRawEvent({"watch-edit", rootId(root), "entry.md"}),
           pros::infrastructure::ReconcileCode::none);
  QCOMPARE(reconciler.health(rootId(root)).health, pros::infrastructure::ReconcileHealth::stale);
  const auto edited = reconciler.reconcile(rootId(root), "reconcile-edit");
  QVERIFY(edited.isSucceeded());
  QCOMPARE(edited.updatedDocumentCount, 1);
  const auto afterEdit = reconciler.document("doc-001");
  QVERIFY(afterEdit.isSucceeded());
  const auto &editedDocument = requireDocument(afterEdit);
  QCOMPARE(editedDocument.relativePath, QString("entry.md"));
  QCOMPARE(editedDocument.contentRevision, quint64(2));

  const QString moved = directory.path() + "/notes/archive/entry.md";
  QVERIFY(QFile::rename(entry, moved));
  QCOMPARE(reconciler.enqueueRawEvent({"watch-move", rootId(root), "archive/entry.md"}),
           pros::infrastructure::ReconcileCode::none);
  const auto movedResult = reconciler.reconcile(rootId(root), "reconcile-move");
  QVERIFY(movedResult.isSucceeded());
  QCOMPARE(movedResult.updatedDocumentCount, 1);
  const auto afterMove = reconciler.document("doc-001");
  QVERIFY(afterMove.isSucceeded());
  const auto &movedDocument = requireDocument(afterMove);
  QCOMPARE(movedDocument.documentId, QString("doc-001"));
  QCOMPARE(movedDocument.relativePath, QString("archive/entry.md"));
  QCOMPARE(movedDocument.contentRevision, quint64(3));

  const auto replay = reconciler.reconcile(rootId(root), "reconcile-move");
  QVERIFY(replay.isSucceeded());
  QCOMPARE(replay.updatedDocumentCount, 1);
  const auto replayedDocument = reconciler.document("doc-001");
  QVERIFY(replayedDocument.isSucceeded());
  QCOMPARE(requireDocument(replayedDocument).contentRevision, quint64(3));

  QVERIFY(QFile::remove(moved));
  QCOMPARE(reconciler.enqueueRawEvent({"watch-delete", rootId(root), "archive/entry.md"}),
           pros::infrastructure::ReconcileCode::none);
  const auto deleted = reconciler.reconcile(rootId(root), "reconcile-delete");
  QVERIFY(deleted.isSucceeded());
  QCOMPARE(deleted.tombstonedDocumentCount, 1);
  const auto afterDelete = reconciler.document("doc-001");
  QVERIFY(afterDelete.isSucceeded());
  const auto &deletedDocument = requireDocument(afterDelete);
  QVERIFY(deletedDocument.tombstoned);
  QCOMPARE(deletedDocument.contentRevision, quint64(4));
  QCOMPARE(reconciler.health(rootId(root)).health, pros::infrastructure::ReconcileHealth::ready);
}

void DocumentReconcilerTest::ignoresReplayedAndOutOfOrderWatcherSignals_V01_KNOW_02_03() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  QVERIFY(QDir().mkpath(directory.path() + "/notes/moved"));
  const QString entry = directory.path() + "/notes/entry.md";
  QVERIFY(writeFile(entry, "before"));

  pros::infrastructure::ResourceResolver resolver;
  const auto root = resolver.registerRoot(directory.path() + "/notes", pros::infrastructure::ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  pros::infrastructure::DocumentReconciler reconciler(databasePath, resolver);
  QVERIFY(reconciler.registerDocument("doc-002", rootId(root), "entry.md").isSucceeded());

  const QString moved = directory.path() + "/notes/moved/entry.md";
  QVERIFY(QFile::rename(entry, moved));
  QVERIFY(writeFile(moved, "after move and edit"));
  QCOMPARE(reconciler.enqueueRawEvent({"late-delete", rootId(root), "entry.md"}),
           pros::infrastructure::ReconcileCode::none);
  QCOMPARE(reconciler.enqueueRawEvent({"early-create", rootId(root), "moved/entry.md"}),
           pros::infrastructure::ReconcileCode::none);
  QCOMPARE(reconciler.enqueueRawEvent({"late-delete", rootId(root), "entry.md"}),
           pros::infrastructure::ReconcileCode::none);

  const auto reconciled = reconciler.reconcile(rootId(root), "reconcile-out-of-order");
  QVERIFY(reconciled.isSucceeded());
  QCOMPARE(reconciled.updatedDocumentCount, 1);
  const auto document = reconciler.document("doc-002");
  QVERIFY(document.isSucceeded());
  const auto &reconciledDocument = requireDocument(document);
  QCOMPARE(reconciledDocument.relativePath, QString("moved/entry.md"));
  QCOMPARE(reconciledDocument.contentRevision, quint64(2));
  QVERIFY(!reconciledDocument.tombstoned);

  QVERIFY(QFile::remove(moved));
  const auto afterLostDelete = reconciler.reconcile(rootId(root), "reconcile-lost-event");
  QVERIFY(afterLostDelete.isSucceeded());
  QCOMPARE(afterLostDelete.tombstonedDocumentCount, 1);
  const auto lostDocument = reconciler.document("doc-002");
  QVERIFY(lostDocument.isSucceeded());
  QVERIFY(requireDocument(lostDocument).tombstoned);
}

void DocumentReconcilerTest::exposesUnavailableRootHealthWithoutChangingRegisteredFact() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const QString rootPath = directory.path() + "/notes";
  QVERIFY(QDir().mkpath(rootPath));
  QVERIFY(writeFile(rootPath + "/entry.md", "body"));

  pros::infrastructure::ResourceResolver resolver;
  const auto root = resolver.registerRoot(rootPath, pros::infrastructure::ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  pros::infrastructure::DocumentReconciler reconciler(databasePath, resolver);
  QVERIFY(reconciler.registerDocument("doc-003", rootId(root), "entry.md").isSucceeded());
  QVERIFY(resolver.revokeRoot(rootId(root)).isAccepted());

  const auto unavailable = reconciler.reconcile(rootId(root), "reconcile-revoked-root");
  QCOMPARE(unavailable.code, pros::infrastructure::ReconcileCode::resource_unavailable);
  QCOMPARE(unavailable.health, pros::infrastructure::ReconcileHealth::unavailable);
  QCOMPARE(unavailable.resourceRejection, pros::infrastructure::ResourceRejectCode::root_revoked);
  const auto health = reconciler.health(rootId(root));
  QCOMPARE(health.code, pros::infrastructure::ReconcileCode::resource_unavailable);
  QCOMPARE(health.resourceRejection, pros::infrastructure::ResourceRejectCode::root_revoked);
  const auto document = reconciler.document("doc-003");
  QVERIFY(document.isSucceeded());
  const auto &unchangedDocument = requireDocument(document);
  QVERIFY(!unchangedDocument.tombstoned);
  QCOMPARE(unchangedDocument.contentRevision, quint64(1));
}

void DocumentReconcilerTest::serializesConcurrentSameOperationIdReplay() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const QString rootPath = directory.path() + "/notes";
  QVERIFY(QDir().mkpath(rootPath));
  QVERIFY(writeFile(rootPath + "/entry.md", "before"));
  pros::infrastructure::ResourceResolver resolver;
  const auto root = resolver.registerRoot(rootPath, pros::infrastructure::ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  pros::infrastructure::DocumentReconciler reconciler(databasePath, resolver);
  QVERIFY(reconciler.registerDocument("doc-concurrent", rootId(root), "entry.md").isSucceeded());
  QVERIFY(writeFile(rootPath + "/entry.md", "after"));
  QCOMPARE(reconciler.enqueueRawEvent({"watch-concurrent", rootId(root), "entry.md"}),
           pros::infrastructure::ReconcileCode::none);
  std::barrier start(3);
  std::array<pros::infrastructure::ReconcileResult, 2> results;
  std::array<std::thread, 2> workers;
  for (std::size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::thread([&, index] {
      start.arrive_and_wait();
      results[index] = reconciler.reconcile(rootId(root), "same-operation");
    });
  }
  start.arrive_and_wait();
  for (std::thread &worker : workers)
    worker.join();
  QCOMPARE(results[0].code, pros::infrastructure::ReconcileCode::none);
  QCOMPARE(results[1].code, pros::infrastructure::ReconcileCode::none);
  QCOMPARE(results[0].updatedDocumentCount, 1);
  QCOMPARE(results[1].updatedDocumentCount, 1);
}

QTEST_APPLESS_MAIN(DocumentReconcilerTest)

#include "document_reconciler_test.moc"
