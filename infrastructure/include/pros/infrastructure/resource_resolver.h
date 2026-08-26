#pragma once

#include <QByteArray>
#include <QString>

#include <compare>
#include <memory>
#include <optional>
#include <vector>

namespace pros::infrastructure {

/** ResourceResolver 对外公开且可持久化记录的拒绝码。 */
enum class ResourceRejectCode {
  none,
  invalid_root_path,
  root_overlap,
  root_not_found,
  root_revoked,
  root_identity_changed,
  invalid_relative_path,
  path_escape,
  access_denied,
  symlink_forbidden,
  resource_not_found,
  resource_open_failed,
  resource_not_regular_file,
};

/** 将拒绝码编码为稳定英文协议值；不得以展示文案判断失败原因。 */
[[nodiscard]] const char *resourceRejectCodeName(ResourceRejectCode code);

/** 已注册根的读写授权范围。 */
enum class ResourceAccess { read_only, read_write };

/** 打开资源时请求的最小访问能力。 */
enum class ResourceOpenMode { read_only, read_write };

struct ResourceRootState;

/** 授权根内普通文件的稳定物理身份；不暴露其物理路径。 */
struct ResourceIdentity final {
  quint64 device = 0;
  quint64 inode = 0;

  auto operator<=>(const ResourceIdentity &) const = default;
};

/**
 * 已打开资源的受限句柄。
 *
 * @note 句柄不公开物理路径或文件描述符。每次读取都会复验根的撤权状态；根撤销后旧句柄立即失败。
 */
class ResourceHandle final {
public:
  ResourceHandle(const ResourceHandle &) = delete;
  ResourceHandle &operator=(const ResourceHandle &) = delete;
  ResourceHandle(ResourceHandle &&) = delete;
  ResourceHandle &operator=(ResourceHandle &&) = delete;
  ~ResourceHandle();

  /**
   * 从已验证的普通文件读取全部内容。
   *
   * @param contents 成功时接收字节内容；失败时保持未指定，不能为空指针。
   * @param rejection 失败时接收稳定拒绝码，可为 nullptr。
   * @return 仅当根仍被授权且文件读取完成时返回 true；不执行写入或路径解析副作用。
   */
  [[nodiscard]] bool readAll(QByteArray *contents, ResourceRejectCode *rejection = nullptr) const;

  /** 返回打开时已复验的普通文件身份；不访问文件系统，重复调用结果不变。 */
  [[nodiscard]] ResourceIdentity identity() const;

private:
  friend class ResourceResolver;

  ResourceHandle(std::shared_ptr<ResourceRootState> root, int descriptor, ResourceIdentity identity);

  std::shared_ptr<ResourceRootState> root_;
  int descriptor_;
  ResourceIdentity identity_;
};

/** 根注册成功后返回的稳定标识与授权 revision。 */
struct ResourceRoot {
  QString id;
  quint64 authorizationRevision = 0;
  ResourceAccess access = ResourceAccess::read_only;
};

/** 根注册或撤销的结构化结果。 */
struct ResourceRootResult {
  ResourceRejectCode rejection = ResourceRejectCode::none;
  std::optional<ResourceRoot> root;

  [[nodiscard]] bool isAccepted() const;
};

/** 资源打开的结构化结果；失败时不返回句柄。 */
struct ResourceOpenResult {
  ResourceRejectCode rejection = ResourceRejectCode::none;
  std::unique_ptr<ResourceHandle> handle;

  [[nodiscard]] bool isAccepted() const;
};

/** 授权根内普通文件的受限枚举结果；路径始终相对于已授权根。 */
struct ResourceListResult {
  ResourceRejectCode rejection = ResourceRejectCode::none;
  std::vector<QString> relativePaths;

  [[nodiscard]] bool isAccepted() const;
};

/**
 * 授权资源根的唯一物理路径入口。
 *
 * 注册会保存目录的 canonical identity；解析只接收相对定位，并在每次打开时复验授权与根身份。
 * 根撤销是幂等的，随后打开和既有句柄读取均返回 `root_revoked`，不会访问根外文件。
 */
class ResourceResolver final {
public:
  ResourceResolver();
  ResourceResolver(const ResourceResolver &) = delete;
  ResourceResolver &operator=(const ResourceResolver &) = delete;
  ~ResourceResolver();

  /**
   * 注册一个现有目录为资源根。
   *
   * @param path 必须是没有空段、`.`、`..` 或软链接组件的绝对目录。
   * @param access 根的授权读写范围。
   * @return 成功时生成稳定 root ID；别名、重叠根或无法复验的目录会被拒绝且不改变注册表。
   */
  [[nodiscard]] ResourceRootResult registerRoot(const QString &path, ResourceAccess access);

  /**
   * 撤销根的后续访问授权。
   *
   * @return 已撤销根重复调用仍成功且返回当前 revision；未知根返回 `root_not_found`。
   */
  [[nodiscard]] ResourceRootResult revokeRoot(const QString &rootId);

  /**
   * 解析并打开已授权根内的既有普通文件。
   *
   * 路径首先经语法校验，再复验目录 identity、根授权和访问范围；最后以目录句柄相对打开且不跟随链接，
   * 并对打开文件进行 identity 与范围复验。失败不会退化为字符串拼接或空数据。
   */
  [[nodiscard]] ResourceOpenResult resolveAndOpen(const QString &rootId, const QString &relativePath,
                                                  ResourceOpenMode mode) const;

  /**
   * 枚举授权根内的普通文件相对路径。
   *
   * @return 仅在根授权和目录身份持续有效时返回结果；软链接不会作为结果返回。调用方仍必须通过
   * `resolveAndOpen` 打开每个路径，以处理枚举后的竞态、撤权和文件替换。
   * @note 此操作不读取文件内容，不接受调用方提供的物理路径，结果顺序按 UTF-8 字节序稳定。
   */
  [[nodiscard]] ResourceListResult listRegularFiles(const QString &rootId) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace pros::infrastructure
