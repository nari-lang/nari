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
| `src/stdlib/std/` | Implicit prelude and importable standard library modules |
| `src/builtins/`  | Native builtins exposed to Nari code  |
| `tests/expect_pass/`  | Test scripts that must exit 0  |
| `tests/expect_fail/`  | Test scripts whose runtime/parse error is expected  |
| `tests/`  | Test helpers and shell harnesses such as `naric_robustness.sh` |

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
```

The harness runs scripts in `tests/expect_pass/` (must exit 0), scripts in
`tests/expect_fail/` (must fail), and shell harnesses.
A passing language-test run prints `Passed tests: N / N`. Tests whose names
contain `npkg` are excluded because they depend on an uncommitted workspace.
Set `SKIP_NETWORK_TESTS=1` to skip network tests, or `SKIP_BUILD=1` to test an
existing build.

## Coding conventions

- **C++17** throughout. `cpp_std=c++17` is the default in `meson.build`. Do not
  reach for C++20-only features.
- **Formatting:** `.clang-format` (LLVM base, 4-space indent, `IndentCaseLabels`,
  `PointerAlignment: Right`, `InsertBraces`, 140-column limit). Run
  `clang-format -i` before committing.
- **Editor:** see `.editorconfig`.
- **Builtins:** new native functions go through the `BUILTIN_FUNCTIONS` X-macro
  in `src/builtins/builtin_lists.h`, with a matching declaration in `ScriptRuntime`
  and an implementation in one of `src/builtins/*.cpp`. Stdlib-only wrappers
  belong in the corresponding `src/stdlib/std/<module>.nari` module. Use
  `src/stdlib/std/prelude.nari` for definitions available without an import.
- **Errors:** C++ exceptions are disabled. `runtime_fatal(...)` reports a fatal
  runtime error and latches the runtime error flag; callers must propagate the
  failure immediately. Expected, recoverable failures should use `Result` or
  `Option`. Script tests use `panic(...)` for failed assertions.
- **No silent correctness bugs.** When in doubt about a primitive's behavior,
  verify by reading the implementation rather than guessing.

## Tests

Every behavior change needs a test.

- `tests/expect_pass/test_<feature>.nari` - exits 0 on success, calls
  `panic("...")` on failure. Use `print(...)` for human-readable output.
- `tests/expect_fail/test_<feature>.nari` - must produce a parse or runtime
  error that the harness expects.

Run the full suite (`./run_tests.sh`) before opening a PR.

## Pull request checklist

1. The full test suite passes locally (`./run_tests.sh`).
2. New native builtins have stdlib wrappers, documentation in the relevant module,
  and at least one `expect_pass` test.
3. `clang-format` has been run on touched C++ files.
4. The PR description explains the motivation and any user-visible changes.
5. User-visible changes are reflected in the relevant language or standard library docs.

## Reporting bugs and requesting features

Use the GitHub issue templates under
[Issues](https://github.com/nari-lang/nari/issues/new/choose).

## License

By contributing, you agree that your contributions will be licensed under the
GNU General Public License v3.0 (see [LICENSE](LICENSE)).
