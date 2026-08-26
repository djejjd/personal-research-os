#include "pros/infrastructure/resource_resolver.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {

using pros::infrastructure::ResourceAccess;
using pros::infrastructure::ResourceOpenMode;
using pros::infrastructure::ResourceRejectCode;
using pros::infrastructure::ResourceResolver;

bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  return file.write(contents) == contents.size();
}

QString acceptedRootId(const pros::infrastructure::ResourceRootResult &result) {
  if (!result.root.has_value())
    return {};
  return result.root.value().id;
}

quint64 acceptedRootRevision(const pros::infrastructure::ResourceRootResult &result) {
  if (!result.root.has_value())
    return 0;
  return result.root.value().authorizationRevision;
}

} // namespace

class ResourceResolverTest final : public QObject {
  Q_OBJECT

private slots:
  void opensAuthorizedRelativeFile();
  void rejectsPathEscapeAndAbsolutePaths();
  void rejectsSymlinkToOutsideForReadAndWrite();
  void rejectsRevokedRootAndInvalidatesOpenedHandle();
  void detectsReplacedRoot();
  void rejectsRootReplacedByLink();
  void rejectsOverlappingAndWriteSymlinkRoots();
  void deniesWriteOnReadOnlyRoot();
  void listsOnlyAuthorizedNonSymlinkFiles();
  void exposesStableRejectCodeNames();
};

void ResourceResolverTest::opensAuthorizedRelativeFile() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  QVERIFY(QDir().mkpath(temporaryDirectory.path() + "/notes"));
  QVERIFY(writeFile(temporaryDirectory.path() + "/notes/entry.md", "authoritative body"));

  ResourceResolver resolver;
  const auto root = resolver.registerRoot(temporaryDirectory.path(), ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  const auto opened = resolver.resolveAndOpen(acceptedRootId(root), "notes/entry.md", ResourceOpenMode::read_only);

  QVERIFY(opened.isAccepted());
  QByteArray contents;
  ResourceRejectCode rejection = ResourceRejectCode::resource_open_failed;
  QVERIFY(opened.handle->readAll(&contents, &rejection));
  QCOMPARE(contents, QByteArray("authoritative body"));
  QCOMPARE(rejection, ResourceRejectCode::none);
}

void ResourceResolverTest::rejectsPathEscapeAndAbsolutePaths() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  QVERIFY(writeFile(temporaryDirectory.path() + "/entry.md", "body"));

  ResourceResolver resolver;
  const auto root = resolver.registerRoot(temporaryDirectory.path(), ResourceAccess::read_only);
  QVERIFY(root.isAccepted());

  QCOMPARE(resolver.resolveAndOpen(acceptedRootId(root), "../outside.md", ResourceOpenMode::read_only).rejection,
           ResourceRejectCode::path_escape);
  QCOMPARE(
      resolver
          .resolveAndOpen(acceptedRootId(root), temporaryDirectory.path() + "/entry.md", ResourceOpenMode::read_only)
          .rejection,
      ResourceRejectCode::invalid_relative_path);
  QCOMPARE(resolver.resolveAndOpen(acceptedRootId(root), "notes//entry.md", ResourceOpenMode::read_only).rejection,
           ResourceRejectCode::invalid_relative_path);
}

void ResourceResolverTest::rejectsSymlinkToOutsideForReadAndWrite() {
  QTemporaryDir rootDirectory;
  QTemporaryDir outsideDirectory;
  QVERIFY(rootDirectory.isValid());
  QVERIFY(outsideDirectory.isValid());
  QVERIFY(writeFile(outsideDirectory.path() + "/secret.md", "outside"));
  QVERIFY(QFile::link(outsideDirectory.path() + "/secret.md", rootDirectory.path() + "/linked.md"));

  ResourceResolver resolver;
  const auto readOnlyRoot = resolver.registerRoot(rootDirectory.path(), ResourceAccess::read_only);
  QVERIFY(readOnlyRoot.isAccepted());
  QCOMPARE(resolver.resolveAndOpen(acceptedRootId(readOnlyRoot), "linked.md", ResourceOpenMode::read_only).rejection,
           ResourceRejectCode::path_escape);

  ResourceResolver writableResolver;
  const auto writableRoot = writableResolver.registerRoot(rootDirectory.path(), ResourceAccess::read_write);
  QVERIFY(writableRoot.isAccepted());
  QCOMPARE(writableResolver.resolveAndOpen(acceptedRootId(writableRoot), "linked.md", ResourceOpenMode::read_write)
               .rejection,
           ResourceRejectCode::symlink_forbidden);
}

void ResourceResolverTest::rejectsRevokedRootAndInvalidatesOpenedHandle() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  QVERIFY(writeFile(temporaryDirectory.path() + "/entry.md", "body"));

  ResourceResolver resolver;
  const auto root = resolver.registerRoot(temporaryDirectory.path(), ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  const auto opened = resolver.resolveAndOpen(acceptedRootId(root), "entry.md", ResourceOpenMode::read_only);
  QVERIFY(opened.isAccepted());
  QVERIFY(resolver.revokeRoot(acceptedRootId(root)).isAccepted());
  const auto repeatedRevocation = resolver.revokeRoot(acceptedRootId(root));
  QVERIFY(repeatedRevocation.isAccepted());
  QCOMPARE(acceptedRootRevision(repeatedRevocation), quint64(2));

  QCOMPARE(resolver.resolveAndOpen(acceptedRootId(root), "entry.md", ResourceOpenMode::read_only).rejection,
           ResourceRejectCode::root_revoked);
  QByteArray contents;
  ResourceRejectCode rejection = ResourceRejectCode::none;
  QVERIFY(!opened.handle->readAll(&contents, &rejection));
  QCOMPARE(rejection, ResourceRejectCode::root_revoked);
}

void ResourceResolverTest::rejectsRootReplacedByLink() {
  QTemporaryDir temporaryDirectory;
  QTemporaryDir outsideDirectory;
  QVERIFY(temporaryDirectory.isValid());
  QVERIFY(outsideDirectory.isValid());
  const QString rootPath = temporaryDirectory.path() + "/root";
  QVERIFY(QDir().mkpath(rootPath));
  QVERIFY(writeFile(rootPath + "/entry.md", "body"));
  QVERIFY(writeFile(outsideDirectory.path() + "/entry.md", "outside"));

  ResourceResolver resolver;
  const auto root = resolver.registerRoot(rootPath, ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  QVERIFY(QDir().rename(rootPath, temporaryDirectory.path() + "/previous-root"));
  QVERIFY(QFile::link(outsideDirectory.path(), rootPath));

  QCOMPARE(resolver.resolveAndOpen(acceptedRootId(root), "entry.md", ResourceOpenMode::read_only).rejection,
           ResourceRejectCode::root_identity_changed);
}

void ResourceResolverTest::detectsReplacedRoot() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  const QString rootPath = temporaryDirectory.path() + "/root";
  QVERIFY(QDir().mkpath(rootPath));
  QVERIFY(writeFile(rootPath + "/entry.md", "body"));

  ResourceResolver resolver;
  const auto root = resolver.registerRoot(rootPath, ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  QVERIFY(QDir().rename(rootPath, temporaryDirectory.path() + "/previous-root"));
  QVERIFY(QDir().mkpath(rootPath));
  QVERIFY(writeFile(rootPath + "/entry.md", "replacement"));

  QCOMPARE(resolver.resolveAndOpen(acceptedRootId(root), "entry.md", ResourceOpenMode::read_only).rejection,
           ResourceRejectCode::root_identity_changed);
}

void ResourceResolverTest::rejectsOverlappingAndWriteSymlinkRoots() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  QVERIFY(QDir().mkpath(temporaryDirectory.path() + "/nested"));
  QVERIFY(writeFile(temporaryDirectory.path() + "/nested/entry.md", "body"));
  const QString alias = temporaryDirectory.path() + "/alias";
  const QString nestedFileAlias = temporaryDirectory.path() + "/entry-link.md";
  QVERIFY(QFile::link(temporaryDirectory.path() + "/nested", alias));
  QVERIFY(QFile::link(temporaryDirectory.path() + "/nested/entry.md", nestedFileAlias));

  ResourceResolver resolver;
  const auto root = resolver.registerRoot(temporaryDirectory.path(), ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  QCOMPARE(resolver.registerRoot(temporaryDirectory.path() + "/nested", ResourceAccess::read_only).rejection,
           ResourceRejectCode::root_overlap);
  QCOMPARE(resolver.registerRoot(alias, ResourceAccess::read_write).rejection, ResourceRejectCode::root_overlap);

  ResourceResolver writableResolver;
  const auto writableRoot = writableResolver.registerRoot(temporaryDirectory.path(), ResourceAccess::read_write);
  QVERIFY(writableRoot.isAccepted());
  QVERIFY(writableResolver.resolveAndOpen(acceptedRootId(writableRoot), "entry-link.md", ResourceOpenMode::read_only)
              .isAccepted());
  QCOMPARE(writableResolver.resolveAndOpen(acceptedRootId(writableRoot), "entry-link.md", ResourceOpenMode::read_write)
               .rejection,
           ResourceRejectCode::symlink_forbidden);
}

void ResourceResolverTest::deniesWriteOnReadOnlyRoot() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  QVERIFY(writeFile(temporaryDirectory.path() + "/entry.md", "body"));

  ResourceResolver resolver;
  const auto root = resolver.registerRoot(temporaryDirectory.path(), ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  QCOMPARE(resolver.resolveAndOpen(acceptedRootId(root), "entry.md", ResourceOpenMode::read_write).rejection,
           ResourceRejectCode::access_denied);
}

void ResourceResolverTest::listsOnlyAuthorizedNonSymlinkFiles() {
  QTemporaryDir rootDirectory;
  QTemporaryDir outsideDirectory;
  QVERIFY(rootDirectory.isValid());
  QVERIFY(outsideDirectory.isValid());
  QVERIFY(QDir().mkpath(rootDirectory.path() + "/nested"));
  QVERIFY(writeFile(rootDirectory.path() + "/nested/entry.md", "body"));
  QVERIFY(writeFile(outsideDirectory.path() + "/outside.md", "outside"));
  QVERIFY(QFile::link(outsideDirectory.path() + "/outside.md", rootDirectory.path() + "/linked.md"));

  ResourceResolver resolver;
  const auto root = resolver.registerRoot(rootDirectory.path(), ResourceAccess::read_only);
  QVERIFY(root.isAccepted());
  const auto listed = resolver.listRegularFiles(acceptedRootId(root));
  QVERIFY(listed.isAccepted());
  QCOMPARE(listed.relativePaths, std::vector<QString>{QString("nested/entry.md")});

  QVERIFY(resolver.revokeRoot(acceptedRootId(root)).isAccepted());
  QCOMPARE(resolver.listRegularFiles(acceptedRootId(root)).rejection, ResourceRejectCode::root_revoked);
}

void ResourceResolverTest::exposesStableRejectCodeNames() {
  QCOMPARE(QString::fromLatin1(pros::infrastructure::resourceRejectCodeName(ResourceRejectCode::root_revoked)),
           QString("root_revoked"));
  QCOMPARE(QString::fromLatin1(pros::infrastructure::resourceRejectCodeName(ResourceRejectCode::path_escape)),
           QString("path_escape"));
}

QTEST_APPLESS_MAIN(ResourceResolverTest)

#include "resource_resolver_test.moc"
