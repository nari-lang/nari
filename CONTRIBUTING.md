# Contributing to Nari

Thanks for your interest in Nari. This document covers the practical bits:
how to build, how the codebase is laid out, the coding conventions we follow,
and what to do before opening a pull request.

## Code of Conduct

This project follows the [Contributor Covenant Code of Conduct](CODE_OF_CONDUCT.md).
By participating, you are expected to uphold it.

## Project layout

| Path  | Purpose  |
| ----------------------- | ---------------------------------------------------------------- |
| `src/`  | C++17 interpreter, JIT, bytecode compiler, runtime, builtins  |
| `src/stdlib/stdlib.nari`| Standard library (loaded at interpreter startup)  |
| `src/builtins/`  | Native builtins exposed to Nari code  |
| `npkg/`  | `npkg` package manager (Go)  |
| `tests/expect_pass/`  | Test scripts that must exit 0  |
| `tests/expect_fail/`  | Test scripts whose runtime/parse error is expected  |
| `tests/`  | Shell harnesses (`naric_robustness.sh`, `npkg_*_smoke.sh`)  |

## Building

Requirements: a C++17 compiler (clang recommended), Meson, Ninja, mbedtls,
libarchive, and (for the JIT) AsmJit.

```sh
./build.sh  # debug build  -> build/debug/nari
./build.sh --release  # release + LTO -> build/release/nari
./build.sh --sanitize  # AddressSanitizer + UBSan -> build/sanitize/nari
./build.sh --reconfigure  # force meson to reconfigure
```

Other flags: `--musl` (experimental), `--emscripten` (WebAssembly, no FFI/HTTP/JIT),
`--wipe` (clean build dir first).

## Running the test suite

```sh
./run_tests.sh # default: release
./run_tests.sh --debug
./run_tests.sh --sanitize
./run_tests.sh --tree-walk
```

The harness runs every script in `tests/expect_pass/` (must exit 0), every
script in `tests/expect_fail/` (must fail), then the `*.sh` smoke harnesses.
A passing run prints `Passed tests: N / N`. Smoke tests that require a local
registry (`npkg_search_smoke.sh`, `npkg_owner_smoke.sh`) are skipped if the
registry at `http://localhost:8080` is unreachable.

## Coding conventions

- **C++17** throughout. `cpp_std=c++17` is enforced in `meson.build`. Do not
  reach for C++20-only features.
- **Formatting:** `.clang-format` (LLVM base, 4-space indent, `IndentCaseLabels`,
  `PointerAlignment: Right`, `InsertBraces`, no column limit). Run
  `clang-format -i` before committing.
- **Editor:** see `.editorconfig`.
- **Builtins:** new native functions go through the `BUILTIN_FUNCTIONS` X-macro
  in `src/runtime.h`, with a matching declaration in the `ScriptRuntime` class
  and an implementation in one of `src/builtins/*.cpp`. Stdlib-only wrappers
  belong in `src/stdlib/stdlib.nari` and follow the existing
  `Hash`/`JSON`/`Archive` pattern: a native `__foo_bar` builtin plus a thin
  global wrapper.
- **Errors:** runtime errors raise via `runtime_fatal(...)` with a precise
  message. Prefer throwing over silent fallbacks.
- **No silent correctness bugs.** When in doubt about a primitive's behavior,
  verify by reading the implementation rather than guessing.

## Tests

Every behavior change needs a test.

- `tests/expect_pass/test_<feature>.nari` - exits 0 on success, calls
  `throw "..."` on failure. Use `system.print` for human-readable output.
- `tests/expect_fail/test_<feature>.nari` - must produce a parse or runtime
  error that the harness expects.

Run the full suite (`./run_tests.sh`) before opening a PR.

## Pull request checklist

1. The full test suite passes locally (`./run_tests.sh`).
2. New native builtins have stdlib wrappers, doc comments in `stdlib.nari`,
  and at least one `expect_pass` test.
3. `clang-format` has been run on touched C++ files.
4. The PR description explains the motivation and any user-visible changes.
5. If the change is user-visible, add an entry to `CHANGELOG.md` under
  `[Unreleased]`.

## Reporting bugs and requesting features

Use the GitHub issue templates under
[Issues](https://github.com/wearrrrr/Nari/issues/new/choose).

## License

By contributing, you agree that your contributions will be licensed under the
GNU General Public License v3.0 (see [LICENSE](LICENSE)).
