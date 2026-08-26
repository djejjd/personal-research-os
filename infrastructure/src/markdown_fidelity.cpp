#include "pros/infrastructure/markdown_fidelity.h"

#include <QStringDecoder>

#include <utility>

namespace pros::infrastructure {
namespace {

bool isHorizontalSpace(const QChar character) { return character == u' ' || character == u'\t'; }

bool isTagCharacter(const QChar character) {
  return character.isLetterOrNumber() || character == u'_' || character == u'-' || character == u'/';
}

bool isLineMarker(const QString &line, const QString &marker) { return line.trimmed() == marker; }

QChar fenceDelimiter(const QString &line) {
  qsizetype index = 0;
  while (index < line.size() && index < 3 && line.at(index) == u' ')
    ++index;
  if (index + 2 >= line.size() || (line.at(index) != u'`' && line.at(index) != u'~'))
    return {};
  const QChar delimiter = line.at(index);
  if (line.at(index + 1) != delimiter || line.at(index + 2) != delimiter)
    return {};
  return delimiter;
}

bool isRelativeMarkdownTarget(const QString &target) {
  if (target.isEmpty() || target.startsWith(u'/') || target.startsWith(u'\\'))
    return false;
  const QChar first = target.at(0);
  if (!first.isLetter())
    return true;
  for (qsizetype index = 1; index < target.size(); ++index) {
    const QChar character = target.at(index);
    if (character == u':')
      return false;
    if (!character.isLetterOrNumber() && character != u'+' && character != u'-' && character != u'.')
      return true;
  }
  return true;
}

bool isWikiTarget(const QString &target) {
  return !target.isEmpty() && !target.contains(u'\n') && !target.contains(u'\r') && !target.contains(u'|');
}

bool skipMarkdownTitle(const QString &line, qsizetype *cursor) {
  if (*cursor >= line.size())
    return false;
  const QChar opening = line.at(*cursor);
  const QChar closing = opening == u'(' ? u')' : opening;
  if (opening != u'\'' && opening != u'"' && opening != u'(')
    return false;
  ++*cursor;
  int nestedParentheses = 0;
  while (*cursor < line.size()) {
    const QChar character = line.at(*cursor);
    if (character == u'\\' && *cursor + 1 < line.size()) {
      *cursor += 2;
      continue;
    }
    if (opening == u'(' && character == u'(') {
      ++nestedParentheses;
    } else if (character == closing) {
      if (nestedParentheses == 0) {
        ++*cursor;
        return true;
      }
      --nestedParentheses;
    }
    ++*cursor;
  }
  return false;
}

qsizetype closingBracket(const QString &line, qsizetype start) {
  for (qsizetype index = start; index < line.size(); ++index) {
    if (line.at(index) == u'\\') {
      ++index;
      continue;
    }
    if (line.at(index) == u']')
      return index;
  }
  return -1;
}

bool skipInlineCode(const QString &line, qsizetype *index) {
  if (line.at(*index) != u'`')
    return false;
  const qsizetype opening = *index;
  while (*index < line.size() && line.at(*index) == u'`')
    ++*index;
  const qsizetype length = *index - opening;
  for (; *index + length <= line.size(); ++*index) {
    if (line.mid(*index, length) == line.mid(opening, length)) {
      *index += length;
      return true;
    }
  }
  return true;
}

bool parseMarkdownLink(const QString &line, qsizetype *index, MarkdownLink *link) {
  const qsizetype opening = *index;
  if (line.at(opening) != u'[' || (opening > 0 && line.at(opening - 1) == u'!'))
    return false;
  const qsizetype labelEnd = closingBracket(line, opening + 1);
  if (labelEnd < 0 || labelEnd + 1 >= line.size() || line.at(labelEnd + 1) != u'(')
    return false;

  qsizetype cursor = labelEnd + 2;
  while (cursor < line.size() && isHorizontalSpace(line.at(cursor)))
    ++cursor;
  const qsizetype targetStart = cursor;
  QString target;
  if (cursor < line.size() && line.at(cursor) == u'<') {
    ++cursor;
    const qsizetype angleTargetStart = cursor;
    while (cursor < line.size() && line.at(cursor) != u'>')
      ++cursor;
    if (cursor >= line.size())
      return false;
    target = line.mid(angleTargetStart, cursor - angleTargetStart);
    ++cursor;
  } else {
    int nestedParentheses = 0;
    while (cursor < line.size()) {
      const QChar character = line.at(cursor);
      if (character == u'\\' && cursor + 1 < line.size()) {
        cursor += 2;
        continue;
      }
      if (isHorizontalSpace(character) || (character == u')' && nestedParentheses == 0))
        break;
      if (character == u'(')
        ++nestedParentheses;
      else if (character == u')')
        --nestedParentheses;
      ++cursor;
    }
    target = line.mid(targetStart, cursor - targetStart);
  }
  if (target.isEmpty())
    return false;

  while (cursor < line.size() && isHorizontalSpace(line.at(cursor)))
    ++cursor;
  if (cursor < line.size() && line.at(cursor) != u')') {
    if (!skipMarkdownTitle(line, &cursor))
      return false;
    while (cursor < line.size() && isHorizontalSpace(line.at(cursor)))
      ++cursor;
  }
  if (cursor >= line.size() || line.at(cursor) != u')')
    return false;

  *index = cursor + 1;
  if (!isRelativeMarkdownTarget(target))
    return true;
  *link = MarkdownLink{.target = target, .kind = MarkdownLinkKind::relative_markdown};
  return true;
}

void parseLine(const QString &line, MarkdownDocument *document) {
  if (line.trimmed().startsWith(u'<'))
    return;
  if (!line.isEmpty() && line.at(0) == u'#') {
    qsizetype end = 1;
    while (end < line.size() && isTagCharacter(line.at(end)))
      ++end;
    if (end > 1)
      document->tags.append(line.mid(1, end - 1));
  }

  for (qsizetype index = 0; index < line.size();) {
    if (line.at(index) == u'\\') {
      index += 2;
      continue;
    }
    if (line.at(index) == u'`') {
      skipInlineCode(line, &index);
      continue;
    }
    if (line.at(index) == u'[' && index + 1 < line.size() && line.at(index + 1) == u'[') {
      const qsizetype end = line.indexOf(QStringLiteral("]]"), index + 2);
      if (end >= 0) {
        const QString target = line.mid(index + 2, end - index - 2).trimmed();
        if (isWikiTarget(target))
          document->links.append(MarkdownLink{.target = target, .kind = MarkdownLinkKind::wiki});
        index = end + 2;
        continue;
      }
    }
    MarkdownLink link;
    if (line.at(index) == u'[' && parseMarkdownLink(line, &index, &link)) {
      if (!link.target.isEmpty())
        document->links.append(std::move(link));
      continue;
    }
    ++index;
  }
}

} // namespace

const char *markdownParseStatusName(MarkdownParseStatus status) {
  switch (status) {
  case MarkdownParseStatus::parsed:
    return "parsed";
  case MarkdownParseStatus::degraded_invalid_utf8:
    return "degraded_invalid_utf8";
  }
  return "degraded_invalid_utf8";
}

bool MarkdownDocument::hasStructuredView() const { return status == MarkdownParseStatus::parsed; }

MarkdownDocument MarkdownParser::parse(const QByteArray &source) {
  MarkdownDocument document;
  document.source = source;
  QStringDecoder decoder(QStringDecoder::Utf8);
  const QString decoded = decoder.decode(source);
  if (decoder.hasError()) {
    document.status = MarkdownParseStatus::degraded_invalid_utf8;
    return document;
  }

  bool inFrontmatter = false;
  QChar activeFenceDelimiter;
  bool firstLine = true;
  const QStringList lines = decoded.split(u'\n');
  for (const QString &line : lines) {
    if (firstLine && isLineMarker(line, QStringLiteral("---"))) {
      inFrontmatter = true;
      firstLine = false;
      continue;
    }
    firstLine = false;
    if (inFrontmatter) {
      if (isLineMarker(line, QStringLiteral("---")) || isLineMarker(line, QStringLiteral("...")))
        inFrontmatter = false;
      continue;
    }
    const QChar delimiter = fenceDelimiter(line);
    if (!delimiter.isNull()) {
      if (activeFenceDelimiter.isNull())
        activeFenceDelimiter = delimiter;
      else if (activeFenceDelimiter == delimiter)
        activeFenceDelimiter = {};
      continue;
    }
    if (activeFenceDelimiter.isNull())
      parseLine(line, &document);
  }
  return document;
}

QByteArray MarkdownSerializer::serialize(const MarkdownDocument &document) { return document.source; }

} // namespace pros::infrastructure
