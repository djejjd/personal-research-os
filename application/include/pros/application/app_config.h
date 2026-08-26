#pragma once

#include <QString>
#include <QStringList>

namespace pros::application {

class AppConfig final {
public:
  /**
   * 从进程参数创建本地应用配置。
   *
   * @throws std::invalid_argument 参数缺失、重复或 `--data-dir` 值为空时抛出；不会创建目录或访问磁盘。
   */
  static AppConfig fromArguments(const QStringList &arguments);

  /** @return 已验证但尚未创建的数据目录路径。 */
  [[nodiscard]] const QString &dataDirectory() const;

private:
  explicit AppConfig(QString dataDirectory);

  QString dataDirectory_;
};

} // namespace pros::application
