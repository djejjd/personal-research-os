#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <compare>

namespace pros::infrastructure {

/** Markdown 结构化投影的可用性；枚举值是可持久化的稳定协议语义。 */
enum class MarkdownParseStatus {
  parsed,
  degraded_invalid_utf8,
};

/** 将解析状态编码为稳定英文协议值；调用方不得依赖展示文案处理降级。 */
[[nodiscard]] const char *markdownParseStatusName(MarkdownParseStatus status);

/** V0.1 支持的两种链接档案类型。 */
enum class MarkdownLinkKind {
  relative_markdown,
  wiki,
};

/** 单个可重建 Markdown 链接投影；target 不包含显示文本或格式化信息。 */
struct MarkdownLink final {
  QString target;
  MarkdownLinkKind kind = MarkdownLinkKind::relative_markdown;

  auto operator<=>(const MarkdownLink &) const = default;
};

/**
 * 保真 Markdown 原文及其有限结构化投影。
 *
 * @note `source` 是唯一可序列化正文；`tags` 与 `links` 仅供可重建视图使用，绝不作为写回依据。
 */
struct MarkdownDocument final {
  QByteArray source;
  QStringList tags;
  QVector<MarkdownLink> links;
  MarkdownParseStatus status = MarkdownParseStatus::parsed;

  [[nodiscard]] bool hasStructuredView() const;
};

/**
 * V0.1 受限 Markdown 档案解析器。
 *
 * 仅识别行首 `#tag`、标准相对 Markdown 链接与 `[[relative path or title]]`。YAML frontmatter、未知语法和
 * 不能安全解码的内容均保留在 `source`；无效 UTF-8 只降低结构化视图，不阻止原文被打开或原样序列化。
 */
class MarkdownParser final {
public:
  /**
   * 从不可信原始字节构建有限投影。
   *
   * @return 始终保留输入字节。有效 UTF-8 仅产生 V0.1 承诺的投影；无效 UTF-8 返回
   * `degraded_invalid_utf8` 且不猜测任何标签或链接。
   * @note 该函数不访问文件、数据库或网络，重复调用无副作用且结果确定。
   */
  [[nodiscard]] static MarkdownDocument parse(const QByteArray &source);
};

/** V0.1 保真 Markdown 序列化器。 */
class MarkdownSerializer final {
public:
  /**
   * 返回加载时保存的原始字节。
   *
   * @return 不重排、不格式化、不删除 frontmatter 或未知语法的原文。重复调用幂等，且不执行文件写入。
   */
  [[nodiscard]] static QByteArray serialize(const MarkdownDocument &document);
};

} // namespace pros::infrastructure
