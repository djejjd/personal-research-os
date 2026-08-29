# Personal Research OS

Personal Research OS is a local-first desktop workspace for personal knowledge, research, and project management.

The repository is temporarily archived at the end of the V0.1 local Qt engineering baseline. Product-facing Qt workspace features have not started yet.

## Development Status

The first engineering milestones establish the Qt/CMake baseline, local domain transactions, file coordination and recovery, and rebuildable knowledge indexing. Product design records are maintained locally and are intentionally excluded from this public repository until they have been prepared for publication.

See [PROJECT_STATUS.md](PROJECT_STATUS.md) for the archived progress snapshot and [QT_LEARNING_PLAN.md](QT_LEARNING_PLAN.md) for a code-grounded Qt study path.

## Development Dependencies

On macOS, install the development dependencies with `brew bundle install --file Brewfile`. The project enforces the minimum Qt 6.8 and SQLite 3.35 compatibility baseline through CMake; `scripts/check.sh` selects the installed Homebrew SQLite headers and library when available so the macOS SDK version does not cap development. Exact dependency artifacts are reserved in `toolchain.lock.sh` and checked by `scripts/release/check-toolchain-lock.sh` during S5 release preparation, not during feature development.

## License

This project is licensed under the [MIT License](LICENSE).
