#include "pros/infrastructure/document_reconciler.h"
#include "pros/infrastructure/knowledge_index.h"
#include "pros/infrastructure/resource_resolver.h"
#include "pros/infrastructure/schema_migrator.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

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
  const QString path = directory.path() + "/knowledge.sqlite";
  pros::infrastructure::SchemaMigrator migrator;
  QString error;
  return migrator.migrate(path, &error) ? path : QString();
}

bool hasAction(const pros::infrastructure::KnowledgeQueryEnvelope &result,
               pros::infrastructure::KnowledgeRecoveryAction action) {
  return result.actions.contains(action);
}

} // namespace

class KnowledgeIndexTest final : public QObject {
  Q_OBJECT

private slots:
  void rebuildsExactTagLinkAndDirectoryFixtures_V01_KNOW_04();
  void hidesAdvancedSourceUntilRebuiltAndRemovesDeletedDocument_V01_KNOW_04();
  void exposesStaleAndUnavailableRecoveryWithoutPartialResults_V01_KNOW_04();
};

void KnowledgeIndexTest::rebuildsExactTagLinkAndDirectoryFixtures_V01_KNOW_04() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const QString notes = directory.path() + "/notes";
  QVERIFY(QDir().mkpath(notes));
  QVERIFY(writeFile(notes + "/alpha.md", "#focus\nexact needle [Guide](guide.md) [[Research]]\n"));
  QVERIFY(writeFile(notes + "/guide.md", "#reference\nA guide without the fixture term.\n"));
  QVERIFY(writeFile(notes + "/broken.md", QByteArray("#ignored\n", 9) + QByteArray("\xFF", 1)));

  pros::infrastructure::ResourceResolver resolver;
  const auto root = resolver.registerRoot(notes, pros::infrastructure::ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  pros::infrastructure::DocumentReconciler reconciler(databasePath, resolver);
  QVERIFY(reconciler.registerDocument("doc-alpha", rootId(root), "alpha.md").isSucceeded());
  QVERIFY(reconciler.registerDocument("doc-guide", rootId(root), "guide.md").isSucceeded());
  QVERIFY(reconciler.registerDocument("doc-broken", rootId(root), "broken.md").isSucceeded());

  pros::infrastructure::KnowledgeIndex index(databasePath, resolver);
  const auto rebuilt = index.rebuild();
  QVERIFY(rebuilt.isSucceeded());
  QVERIFY(rebuilt.asOf >= 3);
  const auto search = index.searchExact("exact needle");
  QVERIFY(search.isReady());
  QCOMPARE(search.items.size(), 1);
  QCOMPARE(search.items.front().documentId, QString("doc-alpha"));
  const auto tags = index.queryTag("#focus");
  QVERIFY(tags.isReady());
  QCOMPARE(tags.items.size(), 1);
  QCOMPARE(tags.items.front().relativePath, QString("alpha.md"));
  const auto markdownLinks = index.queryLinkTarget("guide.md");
  QVERIFY(markdownLinks.isReady());
  QCOMPARE(markdownLinks.items.size(), 1);
  const auto wikiLinks = index.queryLinkTarget("Research");
  QVERIFY(wikiLinks.isReady());
  QCOMPARE(wikiLinks.items.size(), 1);
  const auto directoryItems = index.listDirectory(rootId(root));
  QVERIFY(directoryItems.isReady());
  QCOMPARE(directoryItems.items.size(), 3);
  QCOMPARE(directoryItems.items.front().relativePath, QString("alpha.md"));
  QVERIFY(!directoryItems.items.at(1).structuredViewAvailable);
  QVERIFY(index.queryTag("ignored").items.isEmpty());
}

void KnowledgeIndexTest::hidesAdvancedSourceUntilRebuiltAndRemovesDeletedDocument_V01_KNOW_04() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const QString notes = directory.path() + "/notes";
  QVERIFY(QDir().mkpath(notes));
  QVERIFY(writeFile(notes + "/obsolete.md", "#old\nvanishing phrase\n"));

  pros::infrastructure::ResourceResolver resolver;
  const auto root = resolver.registerRoot(notes, pros::infrastructure::ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  pros::infrastructure::DocumentReconciler reconciler(databasePath, resolver);
  QVERIFY(reconciler.registerDocument("doc-obsolete", rootId(root), "obsolete.md").isSucceeded());
  pros::infrastructure::KnowledgeIndex index(databasePath, resolver);
  QVERIFY(index.rebuild().isSucceeded());
  QVERIFY(index.searchExact("vanishing phrase").isReady());

  QVERIFY(QFile::remove(notes + "/obsolete.md"));
  QVERIFY(reconciler.reconcile(rootId(root), "reconcile-deleted-note").isSucceeded());
  const auto stale = index.searchExact("vanishing phrase");
  QCOMPARE(stale.health, pros::infrastructure::KnowledgeIndexHealth::stale);
  QVERIFY(stale.items.isEmpty());
  QVERIFY(hasAction(stale, pros::infrastructure::KnowledgeRecoveryAction::open_original));
  QVERIFY(hasAction(stale, pros::infrastructure::KnowledgeRecoveryAction::rebuild_index));

  const auto rebuilt = index.rebuild();
  QVERIFY(rebuilt.isSucceeded());
  const auto afterDelete = index.searchExact("vanishing phrase");
  QVERIFY(afterDelete.isReady());
  QVERIFY(afterDelete.items.isEmpty());
}

void KnowledgeIndexTest::exposesStaleAndUnavailableRecoveryWithoutPartialResults_V01_KNOW_04() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString databasePath = initializedDatabase(directory);
  QVERIFY(!databasePath.isEmpty());
  const QString notes = directory.path() + "/notes";
  QVERIFY(QDir().mkpath(notes));
  QVERIFY(writeFile(notes + "/entry.md", "#topic\ninitial fact\n"));

  pros::infrastructure::ResourceResolver resolver;
  const auto root = resolver.registerRoot(notes, pros::infrastructure::ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  pros::infrastructure::DocumentReconciler reconciler(databasePath, resolver);
  QVERIFY(reconciler.registerDocument("doc-entry", rootId(root), "entry.md").isSucceeded());
  pros::infrastructure::KnowledgeIndex index(databasePath, resolver);
  QVERIFY(index.rebuild().isSucceeded());

  QCOMPARE(reconciler.enqueueRawEvent({"watch-pending-index", rootId(root), "entry.md"}),
           pros::infrastructure::ReconcileCode::none);
  const auto pending = index.searchExact("initial fact");
  QCOMPARE(pending.health, pros::infrastructure::KnowledgeIndexHealth::stale);
  QVERIFY(pending.items.isEmpty());
  QVERIFY(hasAction(pending, pros::infrastructure::KnowledgeRecoveryAction::rebuild_index));
  QVERIFY(reconciler.reconcile(rootId(root), "reconcile-pending-index").isSucceeded());
  QVERIFY(index.rebuild().isSucceeded());

  QVERIFY(writeFile(notes + "/entry.md", "#topic\nchanged outside reconcile\n"));
  const auto interrupted = index.rebuild();
  QCOMPARE(interrupted.code, pros::infrastructure::KnowledgeIndexCode::source_stale);
  QCOMPARE(interrupted.health, pros::infrastructure::KnowledgeIndexHealth::stale);
  const auto stale = index.queryTag("topic");
  QCOMPARE(stale.health, pros::infrastructure::KnowledgeIndexHealth::stale);
  QVERIFY(stale.items.isEmpty());
  QVERIFY(hasAction(stale, pros::infrastructure::KnowledgeRecoveryAction::browse_directory));

  QVERIFY(reconciler.reconcile(rootId(root), "reconcile-index-interruption").isSucceeded());
  QVERIFY(index.rebuild().isSucceeded());
  QVERIFY(index.searchExact("changed outside reconcile").isReady());
  QVERIFY(resolver.revokeRoot(rootId(root)).isAccepted());
  const auto unavailable = index.rebuild();
  QCOMPARE(unavailable.code, pros::infrastructure::KnowledgeIndexCode::resource_unavailable);
  QCOMPARE(unavailable.health, pros::infrastructure::KnowledgeIndexHealth::unavailable);
  const auto degraded = index.searchExact("changed outside reconcile");
  QCOMPARE(degraded.health, pros::infrastructure::KnowledgeIndexHealth::unavailable);
  QVERIFY(degraded.items.isEmpty());
  QVERIFY(hasAction(degraded, pros::infrastructure::KnowledgeRecoveryAction::open_original));
}

QTEST_APPLESS_MAIN(KnowledgeIndexTest)

#include "knowledge_index_test.moc"
