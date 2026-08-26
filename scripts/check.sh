#!/bin/sh
set -eu

PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="$PROJECT_ROOT/build/check"
QT_ROOT=${QT_ROOT:-/opt/homebrew/opt/qt}
BASE_REF=${BASE_REF:-origin/main}
LLVM_BIN=${LLVM_BIN:-"$(brew --prefix llvm)/bin"}
SDK_ROOT=${SDK_ROOT:-"$(xcrun --sdk macosx --show-sdk-path)"}
SQLITE_ROOT=${SQLITE_ROOT:-"$(brew --prefix sqlite)"}
SOURCE_FILE_LIST=$(mktemp "${TMPDIR:-/tmp}/personal-research-os-source-files.XXXXXX")
trap 'rm -f "$SOURCE_FILE_LIST"' EXIT HUP INT TERM

if [ ! -x "$LLVM_BIN/clang-format" ] || [ ! -x "$LLVM_BIN/clang-tidy" ]; then
  echo "clang-format 和 clang-tidy 是必需的 S0 门禁。" >&2
  exit 1
fi

PKG_CONFIG_PATH="$SQLITE_ROOT/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
  cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$QT_ROOT" \
    -DCMAKE_CXX_COMPILER="$LLVM_BIN/clang++" -DCMAKE_OSX_SYSROOT="$SDK_ROOT" -U 'SQLite3_*' -U 'PC_SQLite3_*'
cmake --build "$BUILD_DIR"
ctest --test-dir "$BUILD_DIR" --output-on-failure

if git -C "$PROJECT_ROOT" rev-parse --verify --quiet "$BASE_REF" >/dev/null; then
  git -C "$PROJECT_ROOT" diff --name-only --diff-filter=ACMR "$BASE_REF"...HEAD
else
  find "$PROJECT_ROOT/app" "$PROJECT_ROOT/application" "$PROJECT_ROOT/domain" "$PROJECT_ROOT/infrastructure" "$PROJECT_ROOT/tests" \
    -type f \( -name '*.cpp' -o -name '*.h' \) -print | sed "s#^$PROJECT_ROOT/##"
fi >"$SOURCE_FILE_LIST"
git -C "$PROJECT_ROOT" diff --name-only --diff-filter=ACMR >>"$SOURCE_FILE_LIST"
git -C "$PROJECT_ROOT" diff --cached --name-only --diff-filter=ACMR >>"$SOURCE_FILE_LIST"
git -C "$PROJECT_ROOT" ls-files --others --exclude-standard >>"$SOURCE_FILE_LIST"

awk '/^(app|application|domain|infrastructure|tests)\/.*\.(cpp|h)$/' "$SOURCE_FILE_LIST" | sort -u | while IFS= read -r source_file; do
  [ -f "$PROJECT_ROOT/$source_file" ] || continue
  "$LLVM_BIN/clang-format" --dry-run --Werror "$PROJECT_ROOT/$source_file"
  case "$source_file" in
    *.cpp) "$LLVM_BIN/clang-tidy" --quiet -p "$BUILD_DIR" "$PROJECT_ROOT/$source_file" ;;
  esac
done

git -C "$PROJECT_ROOT" diff --check
if git -C "$PROJECT_ROOT" rev-parse --verify --quiet "$BASE_REF" >/dev/null; then
  git -C "$PROJECT_ROOT" diff --check "$BASE_REF"...HEAD
fi
SECRET_PATTERN='BEGIN [A-Z ]*PRIVATE KEY|ghp_[A-Za-z0-9]+|github_pat_[A-Za-z0-9_]+|sk-[A-Za-z0-9]{16,}|AKIA[0-9A-Z]{16}'
if git -C "$PROJECT_ROOT" diff "$BASE_REF"...HEAD | grep -n -E -i "$SECRET_PATTERN"; then
  exit 1
fi
if git -C "$PROJECT_ROOT" diff | grep -n -E -i "$SECRET_PATTERN"; then
  exit 1
fi
if git -C "$PROJECT_ROOT" diff --cached | grep -n -E -i "$SECRET_PATTERN"; then
  exit 1
fi
if [ -s "$SOURCE_FILE_LIST" ]; then
  while IFS= read -r candidate_file; do
    [ -f "$PROJECT_ROOT/$candidate_file" ] || continue
    if grep -n -E -i "$SECRET_PATTERN" -- "$PROJECT_ROOT/$candidate_file"; then
      exit 1
    fi
  done <"$SOURCE_FILE_LIST"
fi

if [ -f "$PROJECT_ROOT/docs/README.md" ] && [ -f "$PROJECT_ROOT/scripts/docs/check-docs.mjs" ]; then
  node "$PROJECT_ROOT/scripts/docs/check-docs.mjs"
fi
