#include "pros/infrastructure/resource_resolver.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QUuid>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace pros::infrastructure {

struct ResourceRootState final {
  QString id;
  QString canonicalPath;
  dev_t device = 0;
  ino_t inode = 0;
  ResourceAccess access = ResourceAccess::read_only;
  std::atomic_bool revoked{false};
  std::atomic<quint64> authorizationRevision{1};
};

namespace {

struct FileDescriptor final {
  explicit FileDescriptor(int value = -1) : value(value) {}
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept : value(std::exchange(other.value, -1)) {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      reset();
      value = std::exchange(other.value, -1);
    }
    return *this;
  }
  ~FileDescriptor() { reset(); }

  [[nodiscard]] bool isValid() const { return value >= 0; }
  [[nodiscard]] int release() { return std::exchange(value, -1); }
  void reset() {
    if (value >= 0) {
      close(value);
      value = -1;
    }
  }

  int value;
};

bool hasForbiddenPathSyntax(const QString &path) {
  if (path.isEmpty() || !QDir::isAbsolutePath(path))
    return true;
  const QStringList components = path.split('/', Qt::KeepEmptyParts);
  for (const QString &component : components) {
    if (component == "." || component == "..")
      return true;
  }
  return false;
}

bool hasSymlinkComponent(const QString &absolutePath) {
  QString current = "/";
  const QStringList components = absolutePath.split('/', Qt::SkipEmptyParts);
  for (const QString &component : components) {
    current = QDir(current).filePath(component);
    const QByteArray encoded = QFile::encodeName(current);
    struct stat status{};
    if (lstat(encoded.constData(), &status) != 0)
      return false;
    if (S_ISLNK(status.st_mode))
      return true;
  }
  return false;
}

std::optional<ResourceRejectCode> verifyRootIdentity(const ResourceRootState &root) {
  const QByteArray encoded = QFile::encodeName(root.canonicalPath);
  struct stat status{};
  if (lstat(encoded.constData(), &status) != 0 || !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode))
    return ResourceRejectCode::root_identity_changed;
  if (status.st_dev != root.device || status.st_ino != root.inode)
    return ResourceRejectCode::root_identity_changed;
  return std::nullopt;
}

bool isInsideRoot(const QString &rootPath, const QString &candidatePath) {
  return candidatePath == rootPath || candidatePath.startsWith(rootPath + '/');
}

std::optional<QStringList> relativeComponents(const QString &relativePath, ResourceRejectCode *rejection) {
  if (relativePath.isNull() || relativePath.isEmpty() || QDir::isAbsolutePath(relativePath)) {
    *rejection = ResourceRejectCode::invalid_relative_path;
    return std::nullopt;
  }
  QStringList components = relativePath.split('/', Qt::KeepEmptyParts);
  for (const QString &component : components) {
    if (component.isEmpty() || component == "." || component == "..") {
      *rejection = component == ".." ? ResourceRejectCode::path_escape : ResourceRejectCode::invalid_relative_path;
      return std::nullopt;
    }
  }
  return components;
}

ResourceOpenResult rejectedOpen(ResourceRejectCode rejection) { return {.rejection = rejection, .handle = nullptr}; }

ResourceListResult rejectedList(ResourceRejectCode rejection) { return {.rejection = rejection, .relativePaths = {}}; }

ResourceRootResult rejectedRoot(ResourceRejectCode rejection) { return {.rejection = rejection, .root = std::nullopt}; }

FileDescriptor openDirectory(const ResourceRootState &root, ResourceRejectCode *rejection) {
  const QByteArray encoded = QFile::encodeName(root.canonicalPath);
  FileDescriptor descriptor(open(encoded.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!descriptor.isValid()) {
    *rejection = ResourceRejectCode::root_identity_changed;
    return descriptor;
  }
  struct stat status{};
  if (fstat(descriptor.value, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_dev != root.device ||
      status.st_ino != root.inode) {
    *rejection = ResourceRejectCode::root_identity_changed;
    return FileDescriptor();
  }
  return descriptor;
}

std::optional<QString> canonicalTarget(const ResourceRootState &root, const QStringList &components,
                                       ResourceRejectCode *rejection) {
  const QString candidate = QDir(root.canonicalPath).filePath(components.join('/'));
  const QFileInfo information(candidate);
  if (!information.exists()) {
    *rejection = ResourceRejectCode::resource_not_found;
    return std::nullopt;
  }
  QString canonical = information.canonicalFilePath();
  if (canonical.isEmpty()) {
    *rejection = ResourceRejectCode::resource_open_failed;
    return std::nullopt;
  }
  if (!isInsideRoot(root.canonicalPath, canonical)) {
    *rejection = ResourceRejectCode::path_escape;
    return std::nullopt;
  }
  return canonical;
}

std::optional<QStringList> componentsBelowRoot(const ResourceRootState &root, const QString &canonicalTarget) {
  if (!isInsideRoot(root.canonicalPath, canonicalTarget) || canonicalTarget == root.canonicalPath)
    return std::nullopt;
  return canonicalTarget.mid(root.canonicalPath.size() + 1).split('/', Qt::SkipEmptyParts);
}

FileDescriptor openRelative(FileDescriptor directory, const QStringList &components, ResourceOpenMode mode,
                            ResourceRejectCode *rejection) {
  for (qsizetype index = 0; index < components.size(); ++index) {
    const QByteArray name = QFile::encodeName(components.at(index));
    const bool finalComponent = index + 1 == components.size();
    int flags = O_CLOEXEC | O_NOFOLLOW;
    if (finalComponent) {
      flags |= mode == ResourceOpenMode::read_write ? O_RDWR : O_RDONLY;
    } else {
      flags |= O_RDONLY | O_DIRECTORY;
    }
    FileDescriptor next(openat(directory.value, name.constData(), flags));
    if (!next.isValid()) {
      if (errno == ELOOP) {
        *rejection = ResourceRejectCode::symlink_forbidden;
      } else if (errno == ENOENT) {
        *rejection = ResourceRejectCode::resource_not_found;
      } else {
        *rejection = ResourceRejectCode::resource_open_failed;
      }
      return FileDescriptor();
    }
    directory = std::move(next);
  }
  return directory;
}

bool verifyUsableRoot(const ResourceRootState &root, ResourceRejectCode *rejection) {
  if (root.revoked.load()) {
    *rejection = ResourceRejectCode::root_revoked;
    return false;
  }
  if (const auto rootRejection = verifyRootIdentity(root); rootRejection.has_value()) {
    *rejection = *rootRejection;
    return false;
  }
  return true;
}

FileDescriptor openParentDirectory(FileDescriptor directory, const QStringList &components,
                                   ResourceRejectCode *rejection) {
  for (qsizetype index = 0; index + 1 < components.size(); ++index) {
    const QByteArray name = QFile::encodeName(components.at(index));
    FileDescriptor next(openat(directory.value, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!next.isValid()) {
      *rejection = errno == ELOOP    ? ResourceRejectCode::symlink_forbidden
                   : errno == ENOENT ? ResourceRejectCode::resource_not_found
                                     : ResourceRejectCode::resource_open_failed;
      return FileDescriptor();
    }
    directory = std::move(next);
  }
  return directory;
}

bool readRegularAt(const ResourceRootState &root, int parentDescriptor, const QString &name, QByteArray *contents,
                   ResourceRejectCode *rejection) {
  if (contents == nullptr) {
    *rejection = ResourceRejectCode::resource_open_failed;
    return false;
  }
  if (!verifyUsableRoot(root, rejection))
    return false;
  const QByteArray encodedName = QFile::encodeName(name);
  FileDescriptor descriptor(openat(parentDescriptor, encodedName.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat status{};
  if (!descriptor.isValid()) {
    *rejection = errno == ELOOP    ? ResourceRejectCode::symlink_forbidden
                 : errno == ENOENT ? ResourceRejectCode::resource_not_found
                                   : ResourceRejectCode::resource_open_failed;
    return false;
  }
  if (fstat(descriptor.value, &status) != 0 || !S_ISREG(status.st_mode)) {
    *rejection = !S_ISREG(status.st_mode) ? ResourceRejectCode::resource_not_regular_file
                                          : ResourceRejectCode::resource_open_failed;
    return false;
  }
  QByteArray result;
  std::array<char, 4096> buffer{};
  for (;;) {
    const ssize_t count = read(descriptor.value, buffer.data(), buffer.size());
    if (count == 0)
      break;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      *rejection = ResourceRejectCode::resource_open_failed;
      return false;
    }
    result.append(buffer.data(), static_cast<qsizetype>(count));
  }
  if (!verifyUsableRoot(root, rejection))
    return false;
  *contents = std::move(result);
  *rejection = ResourceRejectCode::none;
  return true;
}

bool writeRegularExclusivelyAt(const ResourceRootState &root, int parentDescriptor, const QString &name,
                               const QByteArray &contents, ResourceRejectCode *rejection) {
  if (!verifyUsableRoot(root, rejection))
    return false;
  const QByteArray encodedName = QFile::encodeName(name);
  FileDescriptor descriptor(
      openat(parentDescriptor, encodedName.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (!descriptor.isValid()) {
    *rejection = errno == ELOOP ? ResourceRejectCode::symlink_forbidden : ResourceRejectCode::resource_open_failed;
    return false;
  }
  qsizetype offset = 0;
  while (offset < contents.size()) {
    const ssize_t count =
        write(descriptor.value, contents.constData() + offset, static_cast<size_t>(contents.size() - offset));
    if (count < 0) {
      if (errno == EINTR)
        continue;
      *rejection = ResourceRejectCode::resource_open_failed;
      return false;
    }
    if (count == 0) {
      *rejection = ResourceRejectCode::resource_open_failed;
      return false;
    }
    offset += static_cast<qsizetype>(count);
  }
  if (fsync(descriptor.value) != 0) {
    *rejection = ResourceRejectCode::resource_open_failed;
    return false;
  }
  if (!verifyUsableRoot(root, rejection))
    return false;
  *rejection = ResourceRejectCode::none;
  return true;
}

QString temporaryName(const QString &targetName, const QString &operationId) {
  const QByteArray suffix = QCryptographicHash::hash(operationId.toUtf8(), QCryptographicHash::Sha256).toHex();
  return "." + targetName + ".pros-" + QString::fromLatin1(suffix.left(24)) + ".tmp";
}

ResourceReplacementResult rejectedReplacement(ResourceRejectCode rejection) {
  return {.rejection = rejection, .handle = nullptr};
}

} // namespace

struct ResourceResolver::Impl final {
  mutable std::mutex mutex;
  std::map<QString, std::shared_ptr<ResourceRootState>> roots;
};

const char *resourceRejectCodeName(ResourceRejectCode code) {
  switch (code) {
  case ResourceRejectCode::none:
    return "none";
  case ResourceRejectCode::invalid_root_path:
    return "invalid_root_path";
  case ResourceRejectCode::root_overlap:
    return "root_overlap";
  case ResourceRejectCode::root_not_found:
    return "root_not_found";
  case ResourceRejectCode::root_revoked:
    return "root_revoked";
  case ResourceRejectCode::root_identity_changed:
    return "root_identity_changed";
  case ResourceRejectCode::invalid_relative_path:
    return "invalid_relative_path";
  case ResourceRejectCode::path_escape:
    return "path_escape";
  case ResourceRejectCode::access_denied:
    return "access_denied";
  case ResourceRejectCode::symlink_forbidden:
    return "symlink_forbidden";
  case ResourceRejectCode::resource_not_found:
    return "resource_not_found";
  case ResourceRejectCode::resource_open_failed:
    return "resource_open_failed";
  case ResourceRejectCode::resource_not_regular_file:
    return "resource_not_regular_file";
  }
  return "resource_open_failed";
}

ResourceHandle::ResourceHandle(std::shared_ptr<ResourceRootState> root, int descriptor, ResourceIdentity identity)
    : root_(std::move(root)), descriptor_(descriptor), identity_(identity) {}

ResourceHandle::~ResourceHandle() {
  if (descriptor_ >= 0)
    close(descriptor_);
}

bool ResourceHandle::readAll(QByteArray *contents, ResourceRejectCode *rejection) const {
  if (contents == nullptr) {
    if (rejection != nullptr)
      *rejection = ResourceRejectCode::resource_open_failed;
    return false;
  }
  if (root_->revoked.load()) {
    if (rejection != nullptr)
      *rejection = ResourceRejectCode::root_revoked;
    return false;
  }
  if (const auto rootRejection = verifyRootIdentity(*root_); rootRejection.has_value()) {
    if (rejection != nullptr)
      *rejection = *rootRejection;
    return false;
  }
  if (lseek(descriptor_, 0, SEEK_SET) < 0) {
    if (rejection != nullptr)
      *rejection = ResourceRejectCode::resource_open_failed;
    return false;
  }
  QByteArray result;
  std::array<char, 4096> buffer{};
  for (;;) {
    const ssize_t count = read(descriptor_, buffer.data(), buffer.size());
    if (count == 0)
      break;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      if (rejection != nullptr)
        *rejection = ResourceRejectCode::resource_open_failed;
      return false;
    }
    result.append(buffer.data(), static_cast<qsizetype>(count));
  }
  *contents = std::move(result);
  if (rejection != nullptr)
    *rejection = ResourceRejectCode::none;
  return true;
}

ResourceIdentity ResourceHandle::identity() const { return identity_; }

ResourceReplacementHandle::ResourceReplacementHandle(std::shared_ptr<ResourceRootState> root, int parentDescriptor,
                                                     int lockDescriptor, QString targetName, QString temporaryName)
    : root_(std::move(root)), parentDescriptor_(parentDescriptor), lockDescriptor_(lockDescriptor),
      targetName_(std::move(targetName)), temporaryName_(std::move(temporaryName)) {}

ResourceReplacementHandle::~ResourceReplacementHandle() {
  if (lockDescriptor_ >= 0)
    close(lockDescriptor_);
  if (parentDescriptor_ >= 0)
    close(parentDescriptor_);
}

bool ResourceReplacementHandle::readTargetAll(QByteArray *contents, ResourceRejectCode *rejection) const {
  ResourceRejectCode localRejection = ResourceRejectCode::none;
  const bool succeeded = readRegularAt(*root_, parentDescriptor_, targetName_, contents, &localRejection);
  if (rejection != nullptr)
    *rejection = localRejection;
  return succeeded;
}

bool ResourceReplacementHandle::writeTemporaryAndSync(const QByteArray &contents, ResourceRejectCode *rejection) const {
  ResourceRejectCode localRejection = ResourceRejectCode::none;
  const bool succeeded =
      writeRegularExclusivelyAt(*root_, parentDescriptor_, temporaryName_, contents, &localRejection);
  if (rejection != nullptr)
    *rejection = localRejection;
  return succeeded;
}

bool ResourceReplacementHandle::readTemporaryAll(QByteArray *contents, ResourceRejectCode *rejection) const {
  ResourceRejectCode localRejection = ResourceRejectCode::none;
  const bool succeeded = readRegularAt(*root_, parentDescriptor_, temporaryName_, contents, &localRejection);
  if (rejection != nullptr)
    *rejection = localRejection;
  return succeeded;
}

bool ResourceReplacementHandle::replaceTemporaryAndSync(ResourceRejectCode *rejection) const {
  ResourceRejectCode localRejection = ResourceRejectCode::none;
  if (!verifyUsableRoot(*root_, &localRejection)) {
    if (rejection != nullptr)
      *rejection = localRejection;
    return false;
  }
  const QByteArray temporary = QFile::encodeName(temporaryName_);
  const QByteArray target = QFile::encodeName(targetName_);
  if (renameat(parentDescriptor_, temporary.constData(), parentDescriptor_, target.constData()) != 0 ||
      fsync(parentDescriptor_) != 0 || !verifyUsableRoot(*root_, &localRejection)) {
    if (localRejection == ResourceRejectCode::none)
      localRejection = ResourceRejectCode::resource_open_failed;
    if (rejection != nullptr)
      *rejection = localRejection;
    return false;
  }
  if (rejection != nullptr)
    *rejection = ResourceRejectCode::none;
  return true;
}

bool ResourceRootResult::isAccepted() const { return root.has_value() && rejection == ResourceRejectCode::none; }

bool ResourceOpenResult::isAccepted() const { return handle != nullptr && rejection == ResourceRejectCode::none; }

bool ResourceReplacementResult::isAccepted() const {
  return handle != nullptr && rejection == ResourceRejectCode::none;
}

bool ResourceListResult::isAccepted() const { return rejection == ResourceRejectCode::none; }

ResourceResolver::ResourceResolver() : impl_(std::make_unique<Impl>()) {}

ResourceResolver::~ResourceResolver() = default;

ResourceRootResult ResourceResolver::registerRoot(const QString &path, ResourceAccess access) {
  if (hasForbiddenPathSyntax(path))
    return rejectedRoot(ResourceRejectCode::invalid_root_path);
  const QFileInfo information(path);
  if (!information.exists() || !information.isDir() || information.canonicalFilePath().isEmpty())
    return rejectedRoot(ResourceRejectCode::invalid_root_path);
  const QString canonicalPath = information.canonicalFilePath();
  if (hasSymlinkComponent(canonicalPath))
    return rejectedRoot(ResourceRejectCode::invalid_root_path);
  const QByteArray encoded = QFile::encodeName(canonicalPath);
  struct stat status{};
  if (lstat(encoded.constData(), &status) != 0 || !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode))
    return rejectedRoot(ResourceRejectCode::invalid_root_path);

  std::lock_guard lock(impl_->mutex);
  for (const auto &[unusedId, existing] : impl_->roots) {
    static_cast<void>(unusedId);
    const bool samePhysicalDirectory = existing->device == status.st_dev && existing->inode == status.st_ino;
    if (!existing->revoked.load() && (samePhysicalDirectory || isInsideRoot(existing->canonicalPath, canonicalPath) ||
                                      isInsideRoot(canonicalPath, existing->canonicalPath))) {
      return rejectedRoot(ResourceRejectCode::root_overlap);
    }
  }
  const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto root = std::make_shared<ResourceRootState>();
  root->id = id;
  root->canonicalPath = canonicalPath;
  root->device = status.st_dev;
  root->inode = status.st_ino;
  root->access = access;
  impl_->roots.emplace(id, root);
  return {.rejection = ResourceRejectCode::none,
          .root =
              ResourceRoot{.id = id, .authorizationRevision = root->authorizationRevision.load(), .access = access}};
}

ResourceRootResult ResourceResolver::revokeRoot(const QString &rootId) {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->roots.find(rootId);
  if (found == impl_->roots.end())
    return rejectedRoot(ResourceRejectCode::root_not_found);
  const std::shared_ptr<ResourceRootState> &root = found->second;
  const bool wasRevoked = root->revoked.exchange(true);
  const quint64 revision =
      wasRevoked ? root->authorizationRevision.load() : root->authorizationRevision.fetch_add(1) + 1;
  return {.rejection = ResourceRejectCode::none,
          .root = ResourceRoot{.id = root->id, .authorizationRevision = revision, .access = root->access}};
}

ResourceOpenResult ResourceResolver::resolveAndOpen(const QString &rootId, const QString &relativePath,
                                                    ResourceOpenMode mode) const {
  std::shared_ptr<ResourceRootState> root;
  {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->roots.find(rootId);
    if (found == impl_->roots.end())
      return rejectedOpen(ResourceRejectCode::root_not_found);
    root = found->second;
  }
  if (root->revoked.load())
    return rejectedOpen(ResourceRejectCode::root_revoked);
  if (mode == ResourceOpenMode::read_write && root->access != ResourceAccess::read_write)
    return rejectedOpen(ResourceRejectCode::access_denied);
  if (const auto rootRejection = verifyRootIdentity(*root); rootRejection.has_value())
    return rejectedOpen(*rootRejection);

  ResourceRejectCode rejection = ResourceRejectCode::none;
  const auto requestedComponents = relativeComponents(relativePath, &rejection);
  if (!requestedComponents)
    return rejectedOpen(rejection);
  if (mode == ResourceOpenMode::read_write &&
      hasSymlinkComponent(QDir(root->canonicalPath).filePath(requestedComponents->join('/')))) {
    return rejectedOpen(ResourceRejectCode::symlink_forbidden);
  }
  const auto target = canonicalTarget(*root, *requestedComponents, &rejection);
  if (!target)
    return rejectedOpen(rejection);
  const auto targetComponents = componentsBelowRoot(*root, *target);
  if (!targetComponents)
    return rejectedOpen(ResourceRejectCode::path_escape);
  FileDescriptor directory = openDirectory(*root, &rejection);
  if (!directory.isValid())
    return rejectedOpen(rejection);
  FileDescriptor resource = openRelative(std::move(directory), *targetComponents, mode, &rejection);
  if (!resource.isValid())
    return rejectedOpen(rejection);
  struct stat status{};
  if (fstat(resource.value, &status) != 0)
    return rejectedOpen(ResourceRejectCode::resource_open_failed);
  if (!S_ISREG(status.st_mode))
    return rejectedOpen(ResourceRejectCode::resource_not_regular_file);
  if (root->revoked.load())
    return rejectedOpen(ResourceRejectCode::root_revoked);
  if (const auto rootRejection = verifyRootIdentity(*root); rootRejection.has_value())
    return rejectedOpen(*rootRejection);
  return {.rejection = ResourceRejectCode::none,
          .handle = std::unique_ptr<ResourceHandle>(
              new ResourceHandle(root, resource.release(),
                                 ResourceIdentity{.device = static_cast<quint64>(status.st_dev),
                                                  .inode = static_cast<quint64>(status.st_ino)}))};
}

ResourceReplacementResult ResourceResolver::openForAtomicReplacement(const QString &rootId, const QString &relativePath,
                                                                     const QString &operationId) const {
  std::shared_ptr<ResourceRootState> root;
  {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->roots.find(rootId);
    if (found == impl_->roots.end())
      return rejectedReplacement(ResourceRejectCode::root_not_found);
    root = found->second;
  }
  if (root->access != ResourceAccess::read_write)
    return rejectedReplacement(ResourceRejectCode::access_denied);
  ResourceRejectCode rejection = ResourceRejectCode::none;
  if (!verifyUsableRoot(*root, &rejection))
    return rejectedReplacement(rejection);
  const auto components = relativeComponents(relativePath, &rejection);
  if (!components || operationId.isEmpty() || operationId.contains(QChar::Null))
    return rejectedReplacement(components ? ResourceRejectCode::invalid_relative_path : rejection);
  FileDescriptor rootDirectory = openDirectory(*root, &rejection);
  if (!rootDirectory.isValid())
    return rejectedReplacement(rejection);
  FileDescriptor parent = openParentDirectory(std::move(rootDirectory), *components, &rejection);
  if (!parent.isValid())
    return rejectedReplacement(rejection);
  QByteArray targetContents;
  if (!readRegularAt(*root, parent.value, components->back(), &targetContents, &rejection))
    return rejectedReplacement(rejection);
  const QString lockName = "." + components->back() + ".pros.lock";
  const QByteArray encodedLockName = QFile::encodeName(lockName);
  FileDescriptor lockDescriptor(
      openat(parent.value, encodedLockName.constData(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (!lockDescriptor.isValid() || flock(lockDescriptor.value, LOCK_EX) != 0)
    return rejectedReplacement(ResourceRejectCode::resource_open_failed);
  if (!verifyUsableRoot(*root, &rejection))
    return rejectedReplacement(rejection);
  return {.rejection = ResourceRejectCode::none,
          .handle = std::unique_ptr<ResourceReplacementHandle>(
              new ResourceReplacementHandle(root, parent.release(), lockDescriptor.release(), components->back(),
                                            temporaryName(components->back(), operationId)))};
}

ResourceListResult ResourceResolver::listRegularFiles(const QString &rootId) const {
  std::shared_ptr<ResourceRootState> root;
  {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->roots.find(rootId);
    if (found == impl_->roots.end())
      return rejectedList(ResourceRejectCode::root_not_found);
    root = found->second;
  }
  if (root->revoked.load())
    return rejectedList(ResourceRejectCode::root_revoked);
  if (const auto rootRejection = verifyRootIdentity(*root); rootRejection.has_value())
    return rejectedList(*rootRejection);

  std::vector<QString> paths;
  QDirIterator iterator(root->canonicalPath, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    const QString absolutePath = iterator.next();
    const QFileInfo information(absolutePath);
    if (!information.isFile() || information.isSymLink())
      continue;
    const QString relativePath = QDir(root->canonicalPath).relativeFilePath(absolutePath);
    ResourceRejectCode rejection = ResourceRejectCode::none;
    if (relativeComponents(relativePath, &rejection).has_value())
      paths.push_back(relativePath);
  }
  if (root->revoked.load())
    return rejectedList(ResourceRejectCode::root_revoked);
  if (const auto rootRejection = verifyRootIdentity(*root); rootRejection.has_value())
    return rejectedList(*rootRejection);
  std::ranges::sort(paths, [](const QString &left, const QString &right) { return left.toUtf8() < right.toUtf8(); });
  return {.rejection = ResourceRejectCode::none, .relativePaths = std::move(paths)};
}

} // namespace pros::infrastructure
