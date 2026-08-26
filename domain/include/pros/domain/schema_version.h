#pragma once

namespace pros::domain {

/** 当前应用唯一支持的 SQLite schema 版本；迁移器拒绝未知版本。 */
inline constexpr int kCurrentSchemaVersion = 2;

} // namespace pros::domain
