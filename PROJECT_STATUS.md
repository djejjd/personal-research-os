# Project Status

状态：暂时封存

封存时间：2026-08-29

## 结论

当前主线保留了 V0.1 的本地优先工程基础，但尚未进入面向用户的 Qt 工作区开发。项目可以作为 Qt/C++、CMake、SQLite 和分层架构的学习基线；恢复开发时应从 S4 Qt 人工闭环重新排期。

## 已合并进展

| 阶段 | 已完成内容 | 交付证据 |
| --- | --- | --- |
| S0 | Qt 6 Widgets、CMake/Ninja、C++20 分层工程；显式数据目录；SQLite schema metadata 迁移；结构化启动日志；统一检查入口 | PR #3、PR #4 |
| S1 | 领域命令与聚合、SQLite 原子事务、operation 幂等、domain event/outbox/activity、审批与验收治理链 | PR #5 |
| S2 | 授权资源根、Markdown 保真、文件 CAS/oplog、项目 provisioning saga、watcher/reconcile 和恢复边界 | PR #6 |
| S3 | 可重建知识投影、精确词/目录/标签/链接查询、watermark 和索引故障降级信封 | PR #8 |
| 治理 | 公开协作规范、独立审查要求、CI 推送/手动触发兜底 | PR #2、PR #7、PR #9 |

## 当前可验证范围

- macOS arm64 本地 Qt 工程，CMake 最低 Qt 6.8、SQLite 3.35。
- 应用启动、数据目录创建、SQLite 迁移和 schema 版本读取。
- S1-S3 的领域事务、文件协调/恢复和知识查询测试。
- `QT_QPA_PLATFORM=offscreen` 下的应用冒烟和 Qt Test。
- `clang-format`、`clang-tidy`、秘密扫描、差异空白检查和文档检查。

## 尚未实现

- S4 Qt 业务工作区、项目文档树、搜索界面、活动和恢复交互。
- 键盘可达性、焦点断言、窗口布局和 UI 截图验收。
- S5 备份恢复收口、发布打包、签名和精确工具链锁定。
- 网络、AI、Agent、自动化、同步和移动端界面。

## 封存规则

- 不在封存分支继续添加产品功能。
- 未提交的实验性修改不属于本封存快照；恢复时需重新基于最新 `main` 审查。
- `docs/` 目录为本地私有设计资料，未纳入本公开状态快照。

