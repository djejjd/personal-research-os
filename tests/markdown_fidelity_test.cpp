#include "pros/infrastructure/markdown_fidelity.h"

#include <QCryptographicHash>
#include <QFile>
#include <QtTest>

namespace {

using pros::infrastructure::MarkdownLink;
using pros::infrastructure::MarkdownLinkKind;
using pros::infrastructure::MarkdownParser;
using pros::infrastructure::MarkdownParseStatus;
using pros::infrastructure::MarkdownSerializer;

QByteArray fixtureContents() {
  QFile fixture(QStringLiteral(PROS_V01_KNOW_05_FIXTURE));
  if (!fixture.open(QIODevice::ReadOnly))
    return {};
  return fixture.readAll();
}

} // namespace

class MarkdownFidelityTest final : public QObject {
  Q_OBJECT

private slots:
  void v01Know05FixtureProducesOnlySupportedProjection();
  void serializerReturnsInputBytesAfterUnrelatedExternalEdit();
  void invalidUtf8DegradesOnlyStructuredView();
  void rejectsAbsoluteAndSchemeMarkdownTargets();
  void keepsMixedFenceDelimitersOpaque();
};

void MarkdownFidelityTest::v01Know05FixtureProducesOnlySupportedProjection() {
  const QByteArray source = fixtureContents();
  QVERIFY(!source.isEmpty());

  const auto document = MarkdownParser::parse(source);
  QVERIFY(document.hasStructuredView());
  QCOMPARE(document.status, MarkdownParseStatus::parsed);
  QCOMPARE(document.tags, QStringList({QStringLiteral("research/topic")}));
  QCOMPARE(document.links,
           QVector<MarkdownLink>({MarkdownLink{QStringLiteral("notes/related.md"), MarkdownLinkKind::relative_markdown},
                                  MarkdownLink{QStringLiteral("guides/start.md"), MarkdownLinkKind::relative_markdown},
                                  MarkdownLink{QStringLiteral("Projects/Current Plan"), MarkdownLinkKind::wiki}}));
  QCOMPARE(MarkdownSerializer::serialize(document), source);
  QCOMPARE(QCryptographicHash::hash(MarkdownSerializer::serialize(document), QCryptographicHash::Sha256),
           QCryptographicHash::hash(source, QCryptographicHash::Sha256));
}

void MarkdownFidelityTest::serializerReturnsInputBytesAfterUnrelatedExternalEdit() {
  QByteArray externallyEdited = fixtureContents();
  QVERIFY(!externallyEdited.isEmpty());
  QVERIFY(externallyEdited.contains("current project context"));
  externallyEdited.replace("current project context", "updated project context");

  const auto document = MarkdownParser::parse(externallyEdited);
  QCOMPARE(document.status, MarkdownParseStatus::parsed);
  QCOMPARE(MarkdownSerializer::serialize(document), externallyEdited);
  QVERIFY(MarkdownSerializer::serialize(document).contains("unknown_plugin:"));
  QVERIFY(MarkdownSerializer::serialize(document).contains("<custom syntax="));
}

void MarkdownFidelityTest::invalidUtf8DegradesOnlyStructuredView() {
  QByteArray source = "#tag\n[link](notes/related.md)\n";
  source.append(char(0xFF));

  const auto document = MarkdownParser::parse(source);
  QCOMPARE(document.status, MarkdownParseStatus::degraded_invalid_utf8);
  QVERIFY(!document.hasStructuredView());
  QVERIFY(document.tags.isEmpty());
  QVERIFY(document.links.isEmpty());
  QCOMPARE(MarkdownSerializer::serialize(document), source);
  QCOMPARE(QString::fromLatin1(pros::infrastructure::markdownParseStatusName(document.status)),
           QStringLiteral("degraded_invalid_utf8"));
}

void MarkdownFidelityTest::rejectsAbsoluteAndSchemeMarkdownTargets() {
  const QByteArray source = "[absolute](/notes/entry.md) [web](https://example.test) [relative](notes/entry.md)";

  const auto document = MarkdownParser::parse(source);
  QCOMPARE(document.links, QVector<MarkdownLink>(
                               {MarkdownLink{QStringLiteral("notes/entry.md"), MarkdownLinkKind::relative_markdown}}));
}

void MarkdownFidelityTest::keepsMixedFenceDelimitersOpaque() {
  const QByteArray source = "```markdown\n~~~\n#not-a-tag\n[not-a-link](ignored.md)\n```\n#visible-tag\n";

  const auto document = MarkdownParser::parse(source);
  QCOMPARE(document.tags, QStringList({QStringLiteral("visible-tag")}));
  QVERIFY(document.links.isEmpty());
}

QTEST_APPLESS_MAIN(MarkdownFidelityTest)

#include "markdown_fidelity_test.moc"
