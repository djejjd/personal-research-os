#!/bin/sh
set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$PROJECT_ROOT/build/release-toolchain-check"}
QT_ROOT=${QT_ROOT:-/opt/homebrew/opt/qt}
LLVM_BIN=${LLVM_BIN:-"$(brew --prefix llvm)/bin"}
SQLITE_ROOT=${SQLITE_ROOT:-"$(brew --prefix sqlite)"}
SDK_ROOT=${SDK_ROOT:-"$(xcrun --sdk macosx --show-sdk-path)"}

. "$PROJECT_ROOT/toolchain.lock.sh"

require_version() {
  tool_name=$1
  expected_version=$2
  actual_version=$3
  if [ "$actual_version" != "$expected_version" ]; then
    echo "$tool_name 版本不符合发布工具链锁：期望 $expected_version，实际 $actual_version。" >&2
    exit 1
  fi
}

require_version cmake "$PROS_CMAKE_VERSION" "$(cmake --version | sed -n '1s/.* //p')"
require_version ninja "$PROS_NINJA_VERSION" "$(ninja --version)"
require_version Qt "$PROS_QT_VERSION" "$("$QT_ROOT/bin/qmake" -query QT_VERSION)"
require_version LLVM "$PROS_LLVM_VERSION" "$("$LLVM_BIN/clang-tidy" --version | sed -n '1s/.*version \([0-9.]*\).*/\1/p')"
require_version SQLite "$PROS_SQLITE_VERSION" "$("$SQLITE_ROOT/bin/sqlite3" --version | awk '{print $1}')"

brew_repository=$(brew --repository)
test "$(git -C "$brew_repository" rev-parse HEAD)" = "$PROS_HOMEBREW_REVISION"
core_repository=$(brew --repo homebrew/core)
test "$(git -C "$core_repository" rev-parse HEAD)" = "$PROS_HOMEBREW_CORE_REVISION"

PKG_CONFIG_PATH="$SQLITE_ROOT/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
  cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_ROOT" \
    -DCMAKE_CXX_COMPILER="$LLVM_BIN/clang++" -DCMAKE_OSX_SYSROOT="$SDK_ROOT" -U 'SQLite3_*' -U 'PC_SQLite3_*'
SQLITE_INCLUDE=$(sed -n 's/^SQLite3_INCLUDE_DIR:PATH=//p' "$BUILD_DIR/CMakeCache.txt")
SQLITE_LIBRARY=$(sed -n 's/^SQLite3_LIBRARY:FILEPATH=//p' "$BUILD_DIR/CMakeCache.txt")
case "$SQLITE_INCLUDE" in "$SQLITE_ROOT"/*) ;; *) echo "发布构建未使用锁定 SQLite 头文件。" >&2; exit 1 ;; esac
case "$SQLITE_LIBRARY" in "$SQLITE_ROOT"/*) ;; *) echo "发布构建未使用锁定 SQLite 库。" >&2; exit 1 ;; esac

echo "发布工具链锁定验证通过；S5 仍需归档 Homebrew bottle 元数据与发布产物哈希。"
