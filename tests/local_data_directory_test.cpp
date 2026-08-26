#include "pros/infrastructure/local_data_directory.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class LocalDataDirectoryTest final : public QObject {
  Q_OBJECT

private slots:
  void createsMissingDirectory();
  void rejectsFileAsDirectory();
};

void LocalDataDirectoryTest::createsMissingDirectory() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());

  pros::infrastructure::LocalDataDirectory dataDirectory;
  QString errorMessage;
  const QString nestedPath = temporaryDirectory.path() + "/nested/data";

  QVERIFY2(dataDirectory.ensureExists(nestedPath, &errorMessage), qPrintable(errorMessage));
  QVERIFY(QDir(nestedPath).exists());
}

void LocalDataDirectoryTest::rejectsFileAsDirectory() {
  QTemporaryDir temporaryDirectory;
  QVERIFY(temporaryDirectory.isValid());
  const QString filePath = temporaryDirectory.path() + "/token_sk_example_sensitive_note_body";
  QFile file(filePath);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.close();

  pros::infrastructure::LocalDataDirectory dataDirectory;
  QString errorMessage;

  QVERIFY(!dataDirectory.ensureExists(filePath, &errorMessage));
  QVERIFY(!errorMessage.isEmpty());
  QVERIFY(!errorMessage.contains(filePath));
  QVERIFY(!errorMessage.contains("sk_example_sensitive_note_body"));
}

QTEST_APPLESS_MAIN(LocalDataDirectoryTest)

#include "local_data_directory_test.moc"
