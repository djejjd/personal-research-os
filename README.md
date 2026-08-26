# Personal Research OS

Personal Research OS is a local-first desktop workspace for personal knowledge, research, and project management.

The repository currently contains the V0.1 local Qt engineering baseline. Product features have not started yet.

## Development Status

The first planned engineering milestone establishes the Qt and CMake baseline. Product design records are maintained locally and are intentionally excluded from this public repository until they have been prepared for publication.

## Development Dependencies

On macOS, install the development dependencies with `brew bundle install --file Brewfile`. The project enforces the minimum Qt 6.8 and SQLite 3.35 compatibility baseline through CMake. Exact dependency artifacts are reserved in `toolchain.lock.sh` and checked by `scripts/release/check-toolchain-lock.sh` during S5 release preparation, not during feature development.

## License

This project is licensed under the [MIT License](LICENSE).
