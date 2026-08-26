#pragma once

#include <QString>
#include <QStringList>

namespace pros::application {

class AppConfig final {
 public:
  static AppConfig fromArguments(const QStringList& arguments);

  [[nodiscard]] const QString& dataDirectory() const;

 private:
  explicit AppConfig(QString dataDirectory);

  QString dataDirectory_;
};

}  // namespace pros::application
